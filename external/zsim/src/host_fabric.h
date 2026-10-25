/* 1.12.23: the host CPU's on-chip fabric, timing side.
 *
 * Three topologies, chosen by sys.hostNetwork.topology (PIMID: the host's
 * noc.topology):
 *
 *   ring (default)  The published Nehalem-EX design (Kottapalli and Baxter,
 *                   "Nehalem-EX CPU Architecture", Hot Chips 21, 2009, slide
 *                   "On-die network: Ring Interconnect"): two counter-rotating
 *                   rings, ring stops shared by a core and its cache slice,
 *                   the ring advances one stop per clock, 32-byte data ring
 *                   per direction. Here: one stop per core (its L2 and the L3
 *                   slices it holds), plus one stop per memory controller,
 *                   spaced evenly around the ring. A message takes the
 *                   shorter direction; latency = its hops x 1 cycle per stop
 *                   (zero-load: per-segment contention is NOT modelled; the
 *                   host memory controller keeps its own queue). A GET pays
 *                   the request and the response (2 x hops), a dirty
 *                   writeback the data trip (hops).
 *   crossbar        The pre-1.12.23 analytic crossbar: one uniform hop of
 *                   hopCycles on every fabric access (core <-> L3 and L3 <->
 *                   memory controller), no contention.
 *   bus             One shared bus: every access holds it for 1 cycle
 *                   (address/command) plus the data beats of a line
 *                   (line / 256 b); an access waits until the bus is free,
 *                   at most one transaction of every other core (round-robin
 *                   arbitration).
 *                   The free cycle is shared by all cores (an approximation
 *                   under zsim's bound-weave phases: cores' clocks inside a
 *                   phase are not globally ordered).
 *
 * The L3 is one zsim cache; the slice a line lives in is a hash of its line
 * address (Nehalem-EX hashes lines across slices; the hash function is not
 * published, a multiplicative hash is used). The memory controller of a line
 * is the splitter's (line % controllers).
 *
 * The counters (own top-level group, ROI-rebased) feed the power model:
 * data messages (each moves one cache line), their hops, and all accesses. */

#ifndef HOST_FABRIC_H_
#define HOST_FABRIC_H_

#include <algorithm>
#include <cstdint>
#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "galloc.h"
#include "memory_hierarchy.h"
#include "stats.h"
#include "noc_wire_data.h"
extern uint32_t lineBits;   // (user ruling 2026-10-25: measured wire activity)

class HostFabric : public GlobAlloc {
    public:
        enum Mode { RING = 0, CROSSBAR = 1, BUS = 2 };
    private:
        Mode mode;
        uint32_t cores, mcs, slices, hopCycles, beats;
        bool hasL3;
        uint32_t numStops;
        g_vector<uint32_t> coreStop, mcStop;
        volatile uint64_t busFree;
        Counter profDataMsgs, profDataHops, profReqMsgs, profBusWait;
        bool statsDone;
        NocWireData wire;                  // measured data-path activity (user ruling 2026-10-25: measured wire activity)
        NocWireData::Stream* wireStreams;  // one per stop: the sender's last flit

        uint32_t dist(uint32_t a, uint32_t b) const {
            const uint32_t d = (a > b) ? (a - b) : (b - a);
            return std::min(d, numStops - d);
        }
    public:
        HostFabric(Mode _mode, uint32_t _cores, uint32_t _mcs, uint32_t _slices, bool _hasL3,
                   uint32_t _hopCycles, uint32_t _beats)
            : mode(_mode), cores(std::max(1u, _cores)), mcs(std::max(1u, _mcs)),
              slices(std::max(1u, _slices)), hopCycles(_hopCycles), beats(std::max(1u, _beats)),
              hasL3(_hasL3), busFree(0), statsDone(false) {
            /* ring order: the core stops, with the controllers' stops spread
             * evenly between them (after every cores/mcs core stops). */
            numStops = cores + mcs;
            coreStop.resize(cores);
            mcStop.resize(mcs);
            uint32_t pos = 0, m = 0;
            for (uint32_t c = 0; c < cores; c++) {
                coreStop[c] = pos++;
                while (m < mcs && (uint64_t)(c + 1) * mcs >= (uint64_t)(m + 1) * cores) mcStop[m++] = pos++;
            }
            while (m < mcs) mcStop[m++] = pos++;
            wireStreams = new NocWireData::Stream[numStops];
        }

        Mode getMode() const { return mode; }
        uint32_t stops() const { return numStops; }

        uint32_t sliceStop(Address lineAddr) const {
            const uint64_t h = (uint64_t)lineAddr * 0x9E3779B97F4A7C15ull;
            const uint32_t s = (uint32_t)((h >> 32) % slices);
            return coreStop[(uint64_t)s * cores / slices];
        }
        uint32_t coreStopOf(uint32_t core) const { return coreStop[core % cores]; }
        uint32_t mcStopOf(Address lineAddr) const { return mcStop[lineAddr % mcs]; }
        bool l3Present() const { return hasL3; }

        /* Extra latency of one fabric access between two stops, and its
         * counts. PUTS (clean writeback) is not a real access. */
        uint64_t charge(uint32_t a, uint32_t b, const MemReq& req) {
            if (req.type == PUTS) return 0;
            const bool get = (req.type == GETS || req.type == GETX);
            profReqMsgs.atomicInc();
            profDataMsgs.atomicInc();   // a GET is answered with a line; a PUTX carries one
            /* the line on the data path, on the SENDER's stream (a GET's line comes
             * back from b, a PUTX's goes from a). */
            wire.countLine(wireStreams[(get ? b : a) % numStops], req.lineAddr << lineBits, 1u << lineBits);   // its own event: a fabric hop is not a DRAM access
            if (mode == RING) {
                const uint32_t h = dist(a, b);
                profDataHops.atomicInc(h);
                return get ? 2ull * h : (uint64_t)h;
            }
            if (mode == CROSSBAR) {
                profDataHops.atomicInc(1);
                return hopCycles;
            }
            /* BUS: hold the bus for the command beat plus the data beats. The
             * arbitration is round-robin, so a request waits at most one
             * transaction of every other core: the wait is capped there. The
             * cap also keeps cores whose clocks are not aligned (thread-MPI
             * ranks run one after another) from queueing behind each other's
             * future. */
            const uint64_t occ = 1 + beats;
            const uint64_t cap = (uint64_t)(cores - 1) * occ;
            uint64_t cur = busFree, start, end;
            do {
                cur = busFree;
                start = std::min<uint64_t>(std::max<uint64_t>(cur, req.cycle), req.cycle + cap);
                end = std::max<uint64_t>(cur, start + occ);
            } while (!__sync_bool_compare_and_swap(&busFree, cur, end));
            const uint64_t wait = start - req.cycle;
            profBusWait.atomicInc(wait);
            return wait + occ;
        }

        /* Its own top-level group (the "mem" aggregate must stay regular). */
        AggregateStat* initStats(AggregateStat* parentStat) {
            if (statsDone) return nullptr;
            statsDone = true;
            AggregateStat* s = new AggregateStat();
            s->init("hostFabric", "Host on-chip fabric (1.12.23): ring | crossbar | bus");
            profDataMsgs.init("hostFabDataMsgs", "Fabric messages carrying one cache line (GET responses, dirty writebacks)"); s->append(&profDataMsgs);
            profDataHops.init("hostFabDataHops", "Sum of the data messages' hops (ring: stops travelled; crossbar: 1 each; bus: 0)"); s->append(&profDataHops);
            profReqMsgs.init("hostFabReqMsgs", "Fabric accesses (one command each)"); s->append(&profReqMsgs);
            profBusWait.init("hostFabBusWait", "Bus: cycles accesses waited for the bus"); s->append(&profBusWait);
            wire.initStats(s, "hostFabWire", "Measured host data-path wire activity (256-bit flits, per sending stop) (user ruling 2026-10-25: measured wire activity)");
            parentStat->append(s);
            return s;
        }
};

/* One port of the fabric: wraps the object a cache talks to. side CORE: an
 * L2's port to the L3 (the core's stop -> the line's slice stop); side MEM:
 * the LLC's port to the memory controllers (the slice stop, or the L2's core
 * stop when there is no L3, -> the line's controller stop). */
class HostFabricPort : public MemObject {
    private:
        HostFabric* fab;
        MemObject* next;
        bool memSide;
        uint32_t core;
        g_string name;
    public:
        HostFabricPort(HostFabric* _fab, MemObject* _next, bool _memSide, uint32_t _core, const char* _name)
            : fab(_fab), next(_next), memSide(_memSide), core(_core), name(_name) {}

        uint64_t access(MemReq& req) {
            const Address line = req.lineAddr;
            uint64_t resp = next->access(req);
            uint32_t a, b;
            if (memSide) {
                a = fab->l3Present() ? fab->sliceStop(line) : fab->coreStopOf(req.childId);
                b = fab->mcStopOf(line);
            } else {
                a = fab->coreStopOf(core);
                b = fab->sliceStop(line);
            }
            return resp + fab->charge(a, b, req);
        }

        const char* getName() { return name.c_str(); }

        void initStats(AggregateStat* parentStat) {
            if (memSide) next->initStats(parentStat);   // the fabric's own group is registered by init.cpp
        }
};

#endif  // HOST_FABRIC_H_
