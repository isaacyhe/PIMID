// Include Ramulator/spdlog headers FIRST, before ZSim headers.
// ZSim's log.h defines macros (info, warn, panic) that conflict with
// spdlog function names used by Ramulator2.
#ifdef _WITH_RAMULATOR_
#include "base/base.h"
#include "base/request.h"
#include "base/factory.h"
#include "frontend/frontend.h"
#include "memory_system/memory_system.h"
#include <yaml-cpp/yaml.h>
#endif

// Now include ZSim headers (log.h macros are safe after spdlog is parsed)
#include "ramulator_mem_ctrl.h"
#include <map>
#include <string>
#include "event_recorder.h"
#include "tick_event.h"
#include "timing_event.h"
#include "zsim.h"

#ifdef _WITH_RAMULATOR_

class RamulatorAccEvent : public TimingEvent {
    private:
        RamulatorMemory* mem;
        bool write;
        Address addr;

    public:
        uint64_t sCycle;

        RamulatorAccEvent(RamulatorMemory* _mem, bool _write, Address _addr, int32_t domain)
            : TimingEvent(0, 0, domain), mem(_mem), write(_write), addr(_addr) {}

        bool isWrite() const { return write; }
        Address getAddr() const { return addr; }

        void simulate(uint64_t startCycle) {
            sCycle = startCycle;
            mem->enqueue(this, startCycle);
        }
};

RamulatorMemory::RamulatorMemory(const std::string& configFile,
                                 uint64_t cpuFreqHz,
                                 uint32_t _minLatency,
                                 uint32_t _domain,
                                 const g_string& _name)
{
    curCycle = 0;
    minLatency = _minLatency;
    domain = _domain;
    name = _name;

    YAML::Node config = YAML::LoadFile(configFile);
    ramulatorFE = Ramulator::Factory::create_frontend(config);
    ramulatorSys = Ramulator::Factory::create_memory_system(config);

    ramulatorFE->connect_memory_system(ramulatorSys);
    ramulatorSys->connect_frontend(ramulatorFE);

    /* 1.11.91 (audit R8-7): does the device keep the bank-open sums? */
    {
        uint64_t w = 0, o = 0;
        bankOpenTracked = ramulatorSys->pimid_bank_open_totals(w, o);
        if (getenv("PIMID_DEBUG_BANKOPEN"))
            fprintf(stderr, "[bankopen] %s: tracked=%d at construction (w=%lu o=%lu)\n",
                    name.c_str(), bankOpenTracked ? 1 : 0, (unsigned long)w, (unsigned long)o);
    }

    TickEvent<RamulatorMemory>* tickEv = new TickEvent<RamulatorMemory>(this, domain);
    tickEv->queue(0);
}

RamulatorMemory::~RamulatorMemory() {
    if (ramulatorSys) ramulatorSys->finalize();
    if (ramulatorFE) ramulatorFE->finalize();
    delete ramulatorSys;
    delete ramulatorFE;
}

void RamulatorMemory::initStats(AggregateStat* parentStat) {
    AggregateStat* memStats = new AggregateStat();
    memStats->init(name.c_str(), "Memory controller stats");
    profReads.init("rd", "Read requests"); memStats->append(&profReads);
    profWrites.init("wr", "Write requests"); memStats->append(&profWrites);
    profTotalRdLat.init("rdlat", "Total latency experienced by read requests"); memStats->append(&profTotalRdLat);
    profTotalWrLat.init("wrlat", "Total latency experienced by write requests"); memStats->append(&profTotalWrLat);
    /* 1.11.91 (audit R8-7): ALWAYS registered. The first cut registered them
     * only when the device reported itself tracked at construction, and a
     * device whose init() had not yet run read as untracked, so no counter
     * ever reached the dump (gate 1200A, D8/S8). A window of 0 in the dump
     * now means "counter present, no bank state was ever sampled", and the
     * reader says so; a missing counter means an older build. */
    profBankOpenCycles.init("bankOpenCycles",
        "Memory cycles x units (rank, or channel without ranks) with >= 1 bank open (Ramulator2 bank state)");
    memStats->append(&profBankOpenCycles);
    profBankOpenWindow.init("bankOpenWindow",
        "Memory cycles x units sampled (the window bankOpenCycles is a fraction of)");
    memStats->append(&profBankOpenWindow);
    parentStat->append(memStats);
}

uint64_t RamulatorMemory::access(MemReq& req) {
    switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL) ? S : E;
            break;
        case GETX:
            *req.state = M;
            break;
        default: panic("!?");
    }

    /* 1.11.18 (audit go-through): PG residency. Everything except a clean
     * writeback (PUTS, explicitly not a real access) keeps this controller
     * busy. Without this the tracker never advanced on a Ramulator-backed
     * machine, so r_idle came out 1.0 and power gating was credited with
     * the whole leakage, silently. */
    if (req.type != PUTS) {
        uint64_t ph = zinfo->numPhases;
        if (pgIsDevice) zinfo->pgres.devMC[0].touch(ph);
        else            zinfo->pgres.hostMC.touch(ph);
    }

    uint64_t respCycle = req.cycle + minLatency;
    assert(respCycle > req.cycle);

    if ((req.type != PUTS) && zinfo->eventRecorders[req.srcId]) {
        Address addr = req.lineAddr << lineBits;
        bool isWrite = (req.type == PUTX);
        RamulatorAccEvent* memEv = new (zinfo->eventRecorders[req.srcId]) RamulatorAccEvent(this, isWrite, addr, domain);
        memEv->setMinStartCycle(req.cycle);
        TimingRecord tr = {addr, req.cycle, respCycle, req.type, memEv, memEv};
        zinfo->eventRecorders[req.srcId]->pushRecord(tr);
    }

    return respCycle;
}

uint32_t RamulatorMemory::tick(uint64_t cycle) {
    ramulatorSys->tick();
    curCycle++;
    /* 1.11.91 (audit R8-7): mirror the device's cumulative sums into the two
     * Counters. set() keeps the raw count; the 1.11.90 roi_begin rebase
     * subtracts the value at roi_begin, so get() is the ROI window. */
    {
        uint64_t w = 0, o = 0;
        if (ramulatorSys->pimid_bank_open_totals(w, o)) {   // tracked or not, decided per tick
            profBankOpenWindow.set(w);
            profBankOpenCycles.set(o);
        }
    }
    return 1;
}

void RamulatorMemory::enqueue(RamulatorAccEvent* ev, uint64_t cycle) {
    int typeId = ev->isWrite() ? Ramulator::Request::Type::Write : Ramulator::Request::Type::Read;
    Ramulator::Addr_t addr = static_cast<Ramulator::Addr_t>(ev->getAddr());

    auto callback = [this, ev](Ramulator::Request& req) {
        this->completionCallback(ev);
    };

    bool accepted = ramulatorFE->receive_external_requests(typeId, addr, 0, callback);
    if (!accepted) {
        // Ramulator queue full — retry next cycle via re-enqueue
        // For simplicity, spin-retry: hold the event and try again on next tick
        // This matches how gem5 retries when the port is busy
    }

    inflightRequests.insert(std::pair<uint64_t, RamulatorAccEvent*>(ev->getAddr(), ev));
    ev->hold();
}

void RamulatorMemory::completionCallback(RamulatorAccEvent* ev) {
    auto it = inflightRequests.find(ev->getAddr());
    if (it == inflightRequests.end()) panic("Ramulator completion callback: request not found in inflight map");

    uint32_t lat = curCycle + 1 - ev->sCycle;
    if (ev->isWrite()) {
        profWrites.inc();
        profTotalWrLat.inc(lat);
    } else {
        profReads.inc();
        profTotalRdLat.inc(lat);
    }

    ev->release();
    ev->done(curCycle + 1);
    inflightRequests.erase(it);
}

#else  // no ramulator, have the class fail when constructed

RamulatorMemory::RamulatorMemory(const std::string& configFile,
                                 uint64_t cpuFreqHz,
                                 uint32_t _minLatency,
                                 uint32_t _domain,
                                 const g_string& _name)
{
    panic("Cannot use RamulatorMemory, zsim was not compiled with Ramulator2");
}

RamulatorMemory::~RamulatorMemory() {}
void RamulatorMemory::initStats(AggregateStat* parentStat) { panic("???"); }
uint64_t RamulatorMemory::access(MemReq& req) { panic("???"); return 0; }
uint32_t RamulatorMemory::tick(uint64_t cycle) { panic("???"); return 0; }
void RamulatorMemory::enqueue(RamulatorAccEvent* ev, uint64_t cycle) { panic("???"); }
void RamulatorMemory::completionCallback(RamulatorAccEvent* ev) { panic("???"); }

#endif
