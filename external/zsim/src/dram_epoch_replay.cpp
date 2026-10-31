/* dram_epoch_replay.cpp -- 1.11.98 (ruling 4 (c)): see the header. */
#ifdef _WITH_RAMULATOR_
#include "base/base.h"
#include "base/request.h"
#include "base/factory.h"
#include "frontend/frontend.h"
#include "memory_system/memory_system.h"
#include "dram/dram.h"
#include <yaml-cpp/yaml.h>
#endif
#include "dram_epoch_replay.h"
#include "hierarchy_util.h"
#include "core.h"
#include "pe_memory_interface.h"   // 1.12.18 (R0605): the interfaces' stamp, epoch and cut helpers, reused for the host
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "log.h"
#include "zsim.h"
#include "prefetcher.h"
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>

/* (1.12.153): the host core of the host access being recorded, and the charge of the device access being recorded */
static thread_local uint32_t t_recSrc = ~0u;
static thread_local uint32_t t_recChg = 0;
/* (1.12.189): the core of the access being recorded and whether it waits for it; the record this thread made last (its
 * epoch, index, core and the refund its charge took), for chargeLast */
static thread_local uint32_t t_recCore = ~0u;
static thread_local uint8_t t_recHold = 0;
static thread_local uint32_t t_recDma = 0;   // (1.12.189): the DMA transfer being recorded on this thread (0: none)
static thread_local bool t_lastOk = false;
static thread_local uint64_t t_lastEp = 0, t_lastStamp = 0;
static thread_local size_t t_lastIdx = 0;
static thread_local uint32_t t_lastCore = ~0u, t_lastRef = 0;

static thread_local bool t_recPf = false;     // the host access being recorded was issued by an L2 prefetcher
static thread_local uint32_t t_pfTag = 0;     // its reconciliation tag (0: none)
/* a recorded prefetch, until its replayed completion and its consumer (or that it has none) are both known */
struct PfEnt { bool done = false, cons = false, dropped = false; int64_t delta = 0; uint64_t waitStart = 0, estArr = 0, iss = 0; };
/* per core (its L2 prefetcher's requests are only its own): its recorded prefetches by tag; its consumed ones in the
 * order of their wait start, settled from the front once their arrival is known; the state of the settlement */
struct PfShard {
    std::mutex m;
    DramEpochReplay* inst = nullptr;
    std::unordered_map<uint32_t, PfEnt> ent;
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> ord; uint64_t ordSeq = 0;
    int64_t estEnd = 0, trueEnd = 0;   // the latest end of the waits it was charged; of the settled waits (+ C)
    int64_t C = 0, C0 = 0;             // the corrections settled so far; before the oldest kept in hist
    std::deque<std::pair<uint64_t, int64_t>> hist;   // (wait start, C after it) of the latest settled waits
    uint64_t n = 0, late = 0; int64_t corr = 0;      // for the report
};
static const uint32_t kPfCores = 4096;
static PfShard g_pfSh[kPfCores];
/* settle core k's consumed prefetches from the front while their arrival is known (under k.m). The core's stall on
 * prefetches is the union of its waits; each settles the growth of the union of the waits the measured arrivals give
 * less the growth of the union of the waits it was charged. A measured arrival is placed on the consumer's
 * timeline: the prefetch was issued at its own time, so the corrections settled for the core's waits that started
 * between its issue and this wait (the core moved by them, the prefetch did not) come off it; a delay ahead of the
 * issue moved both. Returns the sum, for the core's balance. */
static int64_t pfDrain(PfShard& k) {
    int64_t corr = 0;
    while (!k.ord.empty()) {
        auto f = k.ord.begin(); auto pe = k.ent.find(f->second);
        if (pe != k.ent.end() && !pe->second.done) break;
        if (pe != k.ent.end()) {
            const PfEnt& e = pe->second; const int64_t w0 = (int64_t)e.waitStart;
            int64_t Ci = k.C0;   // the corrections settled up to the prefetch's issue
            if (!k.hist.empty()) {
                auto it = std::upper_bound(k.hist.begin(), k.hist.end(), std::make_pair(e.iss, INT64_MAX));
                if (it != k.hist.begin()) Ci = std::prev(it)->second;
            }
            const int64_t eEnd = std::max<int64_t>(w0, (int64_t)e.estArr);
            const int64_t tEnd = std::max<int64_t>(w0, (int64_t)e.estArr + e.delta - (k.C - Ci));
            const int64_t de = std::max<int64_t>(0, eEnd - std::max(w0, k.estEnd));
            const int64_t dt = std::max<int64_t>(0, tEnd - std::max(w0, k.trueEnd - k.C));
            k.estEnd = std::max(k.estEnd, eEnd); k.trueEnd = std::max(k.trueEnd, tEnd + k.C);
            const int64_t c = dt - de;
            k.C += c; corr += c; k.n++; if (c > 0) k.late++;
            k.hist.emplace_back(e.waitStart, k.C);
            while (k.hist.size() > 4096) { k.C0 = k.hist.front().second; k.hist.pop_front(); }
            k.ent.erase(pe);
        }
        k.ord.erase(f);
    }
    k.corr += corr;
    return corr;
}
static thread_local bool t_recTail = false;   // (1.12.181): the device access being recorded is in a rank's post-ROI tail

#ifdef _WITH_RAMULATOR_

DramEpochReplay::DramEpochReplay(const std::string& configFile, uint64_t coreFreqHz,
                                 uint32_t numInterfaces, uint32_t burstsPerLine, uint32_t lineBits,
                                 const char* name, bool hostOnly, uint32_t hostChannels)
    : name_(name), numMIs_(numInterfaces), burstsPerLine_(burstsPerLine < 1 ? 1 : burstsPerLine),
      lineBits_(lineBits) {
    futex_init(&lock_);
    pool_.reset(new std::atomic<int64_t>[kRecCores]()); used_.reset(new std::atomic<int64_t>[kRecCores]());   // (1.12.189)
    ovA_.reset(new std::atomic<double>[kRecCores]); for (uint32_t k = 0; k < kRecCores; k++) ovA_[k].store(1.0);
    syncP_.reset(new std::atomic<uint8_t>[kRecCores]); for (uint32_t k = 0; k < kRecCores; k++) syncP_[k].store(0);   // (1.12.189)
    hostOnly_ = hostOnly;   // 1.12.18 (R0605)
    {   /* 1.12.17: message passing (thread-MPI) replays in cut mode */
        const char* r = getenv("PIMID_MPI_RANKS");
        cutMode_ = (r && atoi(r) > 1 && !getenv("PIMID_MPI_RANK"));
        if (PEMemoryInterface::singleProcCut()) cutMode_ = true;   // 1.12.18 (ruling 2026-10-24): single-process runs too
        mpiHost_ = cutMode_ && PEMemoryInterface::mpiThreadDetPricing();   // (1.12.153): see mpiHost_
    }
    YAML::Node config = YAML::LoadFile(configFile);
    /* 1.11.98 (gate 1208A): ONE instance for the whole DEVICE. The device's
     * Ramulator YAML (written for one controller per channel) declares one
     * channel; the replay sets the device's channel count and the hierarchy's
     * ranks per channel, and a linear mapper it can invert exactly
     * (ChRaBaRoCo: the address is a mixed-radix number in level order), so
     * every request lands on its target bank (composeAddr). Replaying the raw
     * line address against the one-channel organisation put the whole
     * device's traffic on channel 0, on banks chosen by address bits. */
    const std::string dimpl = config["MemorySystem"]["DRAM"]["impl"].as<std::string>("");
    /* 1.12.18 (ruling 17 / R0605): a hostOnly instance is the decoupled host
     * memory: its own channel count, one rank (the population the host array's
     * energy is priced at, decoupledHostChannels / 1 rank), and the YAML's own
     * address mapper; no device hierarchy, no PE placement. */
    const uint32_t nch = hostOnly_ ? std::max(1u, hostChannels)
                                   : ((zinfo->hierarchy.dramChannels > 0) ? zinfo->hierarchy.dramChannels : 1);
    const bool hasRankLevel = (dimpl == "DDR3" || dimpl == "DDR4" || dimpl == "DDR5" || dimpl == "LPDDR5");
    {   /* 1.12.18 (user ruling 2026-10-24): per-chip bank state for chip-level PE placement */
        const uint32_t cpr0 = zinfo->hierarchy.chipsPerRank, plv0 = zinfo->hierarchy.placementLevel;
        chipSplit_ = !hostOnly_ && hasRankLevel && cpr0 > 1 && plv0 <= 3 && !(nch > 1 && cpr0 == nch);
        chips_ = chipSplit_ ? cpr0 : 1;
        const uint32_t bpg0 = std::max(1u, zinfo->hierarchy.banksPerBG), bgc0 = std::max(1u, zinfo->hierarchy.bgPerChip);
        hostRowSlots_ = (plv0 == 2) ? bpg0 : (plv0 == 3) ? bpg0 * bgc0 : 1u;   // the PE row model's slots per unit
        if (chipSplit_) config["MemorySystem"]["Controller"]["pimid_host_ca_bus"] = true;
        /* (1.12.151): a host request is sent once and driven on the rank's chips in lock step by the controller */
        if (chipSplit_) config["MemorySystem"]["Controller"]["pimid_lockstep_chips"] = (int)chips_;
    }
    config["MemorySystem"]["DRAM"]["org"]["channel"] = (int)nch;
    if (hasRankLevel)
        config["MemorySystem"]["DRAM"]["org"]["rank"] = hostOnly_ ? 1 : (int)(((zinfo->hierarchy.ranksPerChannel > 0) ? zinfo->hierarchy.ranksPerChannel : 1) * chips_);   // 1.12.18: x chips when split
    if (hostOnly_) hostRoBaRaCoCh_ = (config["MemorySystem"]["AddrMapper"]["impl"].as<std::string>("") == "RoBaRaCoCh");
    else config["MemorySystem"]["AddrMapper"]["impl"] = "ChRaBaRoCo";
    /* 1.12.17: a PE placed below the channel tier (0 SUBARRAY, 1 BANK,
     * 2 BANK_GROUP) drives its own bank: the controller serves it bank-parallel
     * (no shared command or data bus); see the constraint filter below. */
    const uint32_t plv = zinfo->hierarchy.placementLevel;
    const bool bankPar = !hostOnly_ && (plv <= 2);   // 1.12.18: never for the host memory
    if (zinfo->hierarchy.dramPowerDown) {   /* 1.12.20: precharge power-down in the controller (every instance) */
        config["MemorySystem"]["Controller"]["pimid_power_down"] = true;
        config["MemorySystem"]["Controller"]["pimid_pd_idle_clocks"] = (int)zinfo->hierarchy.dramPdIdleClocks;
        config["MemorySystem"]["Controller"]["pimid_pd_txp_ns"] = zinfo->hierarchy.dramPdTxpNs;
        config["MemorySystem"]["Controller"]["pimid_pd_txp_nck"] = (int)zinfo->hierarchy.dramPdTxpNck;
        pdOn_ = true;
    }
    if (bankPar) config["MemorySystem"]["Controller"]["pimid_bank_parallel"] = true;
    /* (1.12.169): a host column command crosses the DIMM data bus, which the bank-parallel controller does not model for the PEs
     * (their data stays in the chip). DDR5 only: the standard whose DIMM sub-channel bus the host baselines and co-sim hosts share. */
    hostDataBus_ = bankPar && dimpl == "DDR5";
    if (hostDataBus_) config["MemorySystem"]["Controller"]["pimid_host_data_bus"] = true;
    fe_  = Ramulator::Factory::create_frontend(config);
    sys_ = Ramulator::Factory::create_memory_system(config);
    fe_->connect_memory_system(sys_);
    sys_->connect_frontend(fe_);
    {
        Ramulator::IDRAM* d = sys_->get_ifce<Ramulator::IDRAM>();
        counts_.assign(d->m_organization.count.begin(), d->m_organization.count.end());
        rowIdx_ = d->m_levels("row");
        colIdx_ = (int)counts_.size() - 1;
        const int prefetch = (d->m_internal_prefetch_size > 0) ? d->m_internal_prefetch_size : 1;
        colCount_ = (uint64_t)std::max(1, counts_[colIdx_] / prefetch);
        const uint32_t txBytes = (uint32_t)std::max(1, prefetch * d->m_channel_width / 8);
        while ((1u << txBits_) < txBytes) txBits_++;
        banksPerChannel_ = 1;
        for (int i = 1; i < rowIdx_; i++) banksPerChannel_ *= (uint64_t)std::max(1, counts_[i]);
        if (bankPar) {
            /* 1.12.17: remove the bus constraints a PE below the channel tier does
             * not cross. Above the PE's tier (the bank for SUBARRAY/BANK, the bank
             * group for BANK_GROUP): every column-to-column constraint (data bus
             * nBL, nCCDS/nCCDL, read/write turnaround, rank switching); at the
             * channel level also the command-bus occupancy after ACT and column
             * commands. Kept: ACT-to-ACT (tRRD, tFAW), bank timing, precharge-all
             * and refresh. */
            const int bankLv = d->m_levels("bank");
            int keepLv = bankLv;
            if (plv == 2) {
                int bg = -1;
                for (int l = 0; l < bankLv; l++) if (std::string(d->m_levels(l)) == "bankgroup") bg = l;
                if (bg >= 0) keepLv = bg;
            }
            auto cmdId = [d](const char* n) -> int {
                for (int c = 0; c < (int)d->m_commands.size(); c++) if (std::string(d->m_commands(c)) == n) return c;
                return -1;
            };
            std::vector<int> col;
            for (const char* n : {"RD", "WR", "RDA", "WRA"}) { const int c = cmdId(n); if (c >= 0) col.push_back(c); }
            const int act = cmdId("ACT");
            auto isCol = [&col](int c) { return std::find(col.begin(), col.end(), c) != col.end(); };
            uint64_t removed = 0, kept = 0;
            stripCons_ = [keepLv, act, col](Ramulator::IDRAM* dd) {   // (1.12.144): the same filter for the channel instances
                auto isC = [&col](int c) { return std::find(col.begin(), col.end(), c) != col.end(); };
                for (int l = 0; l < keepLv; l++)
                    for (int pc = 0; pc < (int)dd->m_timing_cons[l].size(); pc++) {
                        auto& v = dd->m_timing_cons[l][pc];
                        v.erase(std::remove_if(v.begin(), v.end(), [&](const Ramulator::TimingConsEntry& t) {
                            if (isC(pc) && isC(t.cmd)) return true;
                            if (l == 0 && (pc == act || isC(pc))) return true;
                            return false;
                        }), v.end());
                    }
            };
            for (int l = 0; l < keepLv; l++) {
                for (int pc = 0; pc < (int)d->m_timing_cons[l].size(); pc++) {
                    auto& v = d->m_timing_cons[l][pc];
                    const size_t before = v.size();
                    v.erase(std::remove_if(v.begin(), v.end(), [&](const Ramulator::TimingConsEntry& t) {
                        if (isCol(pc) && isCol(t.cmd)) return true;                 // data bus / CCD / turnaround
                        if (l == 0 && (pc == act || isCol(pc))) return true;       // channel command bus
                        return false;
                    }), v.end());
                    removed += before - v.size();
                    kept += v.size();
                }
            }
            info("[mem] %s: PE placement tier %u is below the channel: bank-parallel controller, %lu bus constraint(s) above the %s tier removed, %lu kept (activation limits, refresh, precharge-all) [1.12.17]",
                 name, plv, (unsigned long)removed, plv == 2 ? "bank-group" : "bank", (unsigned long)kept);
        }
        const uint32_t cpr = zinfo->hierarchy.chipsPerRank;
        foldedChannels_ = !hostOnly_ && (nch > 1 && cpr == nch);
        perChan_.assign((size_t)std::max(1, counts_[0]), 0);
        if (hostOnly_) {   // 1.12.18 (R0605)
            info("[mem] %s: host memory replay organisation %s, %d channel(s), %lu banks per channel, address mapper %s (the YAML's own), %u B per column access [1.12.18 ruling 17]",
                 name, dimpl.c_str(), counts_[0], (unsigned long)banksPerChannel_,
                 config["MemorySystem"]["AddrMapper"]["impl"].as<std::string>("").c_str(), 1u << txBits_);
        } else {
        /* 1.12.18: the chips count too -- at a chip-level placement every chip's banks are their own state
         * (with the old lock-step keying this check would have printed 32 vs 128 for DDR5 x8) */
        const uint64_t pimidBanks = (uint64_t)std::max(1u, zinfo->hierarchy.bgPerChip) * (uint64_t)std::max(1u, zinfo->hierarchy.banksPerBG)
                                  * (foldedChannels_ ? 1ull : (uint64_t)std::max(1u, zinfo->hierarchy.ranksPerChannel))
                                  * ((!foldedChannels_ && zinfo->hierarchy.placementLevel <= 3) ? (uint64_t)std::max(1u, zinfo->hierarchy.chipsPerRank) : 1ull);
        info("[mem] %s: replay organisation %s, %d channel(s), %lu banks per channel (PIMID hierarchy: %lu), ChRaBaRoCo, %u B per column access%s",
             name, dimpl.c_str(), counts_[0], (unsigned long)banksPerChannel_, (unsigned long)pimidBanks, 1u << txBits_,
             (pimidBanks != banksPerChannel_) ? " -- MISMATCH: bank indices wrap modulo the Ramulator count" : "");
        if (chipSplit_)
            info("[mem] %s: 1.12.18 (user ruling 2026-10-24): PE placement at the chip tier or below -- each of the %u chips of a rank is its own Ramulator rank "
                 "(own bank state, rows, tRRD/tFAW; PE commands from the in-chip controller, no DIMM CA bus); a host request is replayed on all %u chips in "
                 "lock step (copy 0 counted, the others timed and state-changing), completes with its slowest copy, and obeys the channel's CA bus",
                 name, chips_, chips_);
        }
    }
    const double tck = sys_->get_tCK();   // ns
    const double corePeriodNs = (coreFreqHz > 0) ? 1e9 / (double)coreFreqHz : 0.0;
    if (!(tck > 0.0) || !(corePeriodNs > 0.0))
        panic("DramEpochReplay %s: no DRAM clock (tCK %.3f ns, core %.3f ns)", name, tck, corePeriodNs);
    ticksPerCycle_ = corePeriodNs / tck;
    coreFreqHz_ = coreFreqHz; tckNs_ = tck;   // 1.12.18 (R2): the run time's clock may be another node's
    miRd_.assign(numMIs_, nullptr); miWr_.assign(numMIs_, nullptr);   // 1.12.18 (R1)
    miPriced_.assign(numMIs_, nullptr); miChgLat_.assign(numMIs_, nullptr);   // (1.12.153)
    outPriced_.assign(numMIs_, 0); outChgLat_.assign(numMIs_, 0); outPricedApplied_.assign(numMIs_, 0); outChgLatApplied_.assign(numMIs_, 0);
    outRd_.assign(numMIs_, 0); outWr_.assign(numMIs_, 0); outRdApplied_.assign(numMIs_, 0); outWrApplied_.assign(numMIs_, 0);
    sumLat_.assign(numMIs_, 0); cntLat_.assign(numMIs_, 0);
    lastValue_.assign(kPriceKinds * (size_t)numMIs_, 0);   // (1.12.189): a price slot per interface and kind
    miTicksPerCycle_.assign(numMIs_, ticksPerCycle_);   // 1.12.18 (R0605): host interfaces carry their own clock
    /* (1.12.144): the per-channel replay (see Chan in the header). Only for DDR5, the organisation it was validated on: a
     * DDR5 channel is a separate DIMM sub-channel with its own chips, so a one-channel instance keeps every chip's
     * density and organisation. A GDDR6 device holds both channels on one die (its density counts them), and other
     * standards are not validated: they keep the single instance. */
    /* (1.12.189): the closed-loop replay holds a core's next access until an earlier one completes, which can be on the
     * other channel: it runs the single instance */
    if (cutMode_ && !hostOnly_ && dimpl == "DDR5" && counts_.size() > 1 && counts_[0] == 2 && !(getenv("PIMID_REPLAY_SERIAL") && atoi(getenv("PIMID_REPLAY_SERIAL"))) && !clOn()) {
        YAML::Node one = YAML::Clone(config);
        one["MemorySystem"]["DRAM"]["org"]["channel"] = 1;
        uint64_t stride = 1;
        for (int i = 1; i <= rowIdx_; i++) stride *= (uint64_t)std::max(1, counts_[i]);
        stride = (stride * colCount_) << txBits_;
        chans_.resize(2);
        for (int c = 0; c < 2; c++) {
            Chan& K = chans_[c];
            K.fe = Ramulator::Factory::create_frontend(one);
            K.sys = Ramulator::Factory::create_memory_system(one);
            K.fe->connect_memory_system(K.sys);
            K.sys->connect_frontend(K.fe);
            if (stripCons_) stripCons_(K.sys->get_ifce<Ramulator::IDRAM>());
            K.addrOffset = (uint64_t)c * stride;
            if (K.sys->get_tCK() != tck) panic("DramEpochReplay %s: channel instance tCK %.4f != %.4f", name, K.sys->get_tCK(), tck);
        }
        info("[mem] %s: the two channels are replayed on two host threads, one Ramulator2 instance each [(1.12.144): host speed]", name);
    }
    info("[mem] %s: DRAM epoch replay armed: Ramulator2 from %s, tCK %.4f ns = %.4f DRAM cycles per core cycle, %u interfaces, %u column commands per line [1.11.98 ruling 4 (c)]",
         name, configFile.c_str(), tck, ticksPerCycle_, numMIs_, burstsPerLine_);
}

DramEpochReplay::~DramEpochReplay() {}

uint32_t DramEpochReplay::addHostInterface(uint64_t hostFreqHz) {
    /* 1.12.18 (ruling 17 / R0605): see the header. The interface's latency is
     * measured in DRAM ticks and returned in its own (host) cycles. */
    const double tck = sys_->get_tCK();
    const double tpc = (hostFreqHz > 0 && tck > 0.0) ? (1e9 / (double)hostFreqHz) / tck : 0.0;
    if (!(tpc > 0.0)) panic("DramEpochReplay %s: no host clock for the host interface (%lu Hz)", name_.c_str(), (unsigned long)hostFreqHz);
    futex_lock(&lock_);
    if (hostMis_ == 0) hostMi0_ = numMIs_;
    else if (hostMi0_ + hostMis_ != numMIs_) panic("DramEpochReplay %s: host interfaces must be numbered contiguously", name_.c_str());
    const uint32_t mi = numMIs_++;
    hostMis_++;
    sumLat_.push_back(0); cntLat_.push_back(0); for (uint32_t k = 0; k < kPriceKinds; k++) lastValue_.push_back(0);   // (1.12.189): its price slots
    miTicksPerCycle_.push_back(tpc);
    miRd_.push_back(nullptr); miWr_.push_back(nullptr);   // 1.12.18 (R1)
    miPriced_.push_back(nullptr); miChgLat_.push_back(nullptr);   // (1.12.153)
    outPriced_.push_back(0); outChgLat_.push_back(0); outPricedApplied_.push_back(0); outChgLatApplied_.push_back(0);
    outRd_.push_back(0); outWr_.push_back(0); outRdApplied_.push_back(0); outWrApplied_.push_back(0);
    futex_unlock(&lock_);
    info("[mem] %s: host memory interface %u joined the replay (%s): host clock %.1f MHz = %.4f DRAM cycles per host cycle; its requests carry source_id 1 and the controller splits their row hits/misses/conflicts and activates [1.12.18 ruling 17]",
         name_.c_str(), mi, hostOnly_ ? "host-only instance" : "the device's instance: the shared array, interfaces after the device's",
         (double)hostFreqHz / 1e6, tpc);
    return mi;
}

void DramEpochReplay::bindCounters(uint32_t mi, Counter* rd, Counter* wr, Counter* priced, Counter* chargedLat) {   // 1.12.18 (R1)
    futex_lock(&lock_);
    if (mi < numMIs_) { miRd_[mi] = rd; miWr_[mi] = wr; miPriced_[mi] = priced; miChgLat_[mi] = chargedLat; }
    futex_unlock(&lock_);
}

/* 1.12.18 (user ruling R1 2026-10-24): take the requests found outside the
 * ROI out of the bound interface counters (the rd/wr the power model and
 * McPAT read), once each; finalize runs at every statistics dump. */
void DramEpochReplay::applyOutside() {
    for (uint32_t m = 0; m < numMIs_; m++) {
        const uint64_t dr = outRd_[m] - outRdApplied_[m], dw = outWr_[m] - outWrApplied_[m];
        if (dr && miRd_[m]) { miRd_[m]->atomicInc((uint64_t)0 - dr); outRdApplied_[m] += dr; }
        if (dw && miWr_[m]) { miWr_[m]->atomicInc((uint64_t)0 - dw); outWrApplied_[m] += dw; }
        /* (1.12.153): the charged view of the same requests (a device interface's replayPriced / replayLat) */
        const uint64_t dp = outPriced_[m] - outPricedApplied_[m], dl = outChgLat_[m] - outChgLatApplied_[m];
        if (dp && miPriced_[m]) { miPriced_[m]->atomicInc((uint64_t)0 - dp); outPricedApplied_[m] += dp; }
        if (dl && miChgLat_[m]) { miChgLat_[m]->atomicInc((uint64_t)0 - dl); outChgLatApplied_[m] += dl; }
    }
}

void DramEpochReplay::initStats(AggregateStat* parentStat) {
    AggregateStat* s = new AggregateStat();
    s->init(name_.c_str(), hostOnly_ ? "DRAM epoch replay (1.12.18 ruling 17): the decoupled host memory's controller replayed per epoch"
                                     : "DRAM epoch replay (ruling 4 (c)): the device controller replayed per epoch");
    profRequests_.init("ctrlRequests", "Device DRAM requests replayed through the controller"); s->append(&profRequests_);
    profEpochs_.init("epochs", "Epochs replayed"); s->append(&profEpochs_);
    profReads_.init("reads", "Replayed reads"); s->append(&profReads_);
    profWrites_.init("writes", "Replayed writes"); s->append(&profWrites_);
    profLatSum_.init("latencySum", "Sum of measured request latencies (core cycles; mean = latencySum / requests)"); s->append(&profLatSum_);
    profUnserved_.init("unserved", "Requests still in flight at the drain bound (should be 0)"); s->append(&profUnserved_);
    profRowHits_.init("ctrlRowHits", "Controller row hits (Ramulator2 totals over the replayed run; 1.12.18: the device interfaces only when host interfaces joined, see hostRow*)"); s->append(&profRowHits_);
    profRowMisses_.init("ctrlRowMisses", "Controller row misses (a closed bank)"); s->append(&profRowMisses_);
    profRowConflicts_.init("ctrlRowConflicts", "Controller row conflicts (another row open: precharge + activate)"); s->append(&profRowConflicts_);
    profRefreshes_.init("ctrlRefreshes", "Refresh commands the controller issued over the replayed run (row 28: measured background)"); s->append(&profRefreshes_);
    if (pdOn_) {   // 1.12.19
        profPdCycles_.init("ctrlPdUnitCycles", "Replayed DRAM cycles x power-down units spent in precharge power-down (ROI window)"); s->append(&profPdCycles_);
        profPdUnits_.init("ctrlPdUnits", "Power-down units (ranks, or channels without a rank level) of the replayed controller"); s->append(&profPdUnits_);
        profPdExits_.init("ctrlPdExits", "Power-down exits in the ROI window (each delays the next command by tXP)"); s->append(&profPdExits_);
    }
    profTicks_.init("ctrlTicks", "DRAM clock cycles the controller was replayed for"); s->append(&profTicks_);
    profIdleTicks_.init("ctrlIdleTicks", "Replayed DRAM cycles with no request in flight (row 28: measured idle)"); s->append(&profIdleTicks_);
    profChannelsUsed_.init("ctrlChannelsUsed", "Channels of the device that received replayed requests"); s->append(&profChannelsUsed_);
    profMaxChanPermille_.init("ctrlMaxChannelPermille", "The busiest channel's share of the replayed requests (per mille)"); s->append(&profMaxChanPermille_);
    profPriced_.init("priced", "Interface accesses priced at the previous epoch's replayed service latency"); s->append(&profPriced_);
    profBootstrap_.init("bootstrap", "Interface accesses priced by the analytical service (no replayed epoch yet)"); s->append(&profBootstrap_);
    profRecReads_.init("reconciledAccesses", "Accesses a core waited for (reads, blocking stores; replay- or bootstrap-priced) whose measured - charged latency went to the core's balance (ROI)"); s->append(&profRecReads_);   // (1.12.189)
    profRecCycles_.init("reconciledCycles", "Cycles the reconciliation added to the cores as delays at the phase barrier (ROI)"); s->append(&profRecCycles_);
    profRecRefund_.init("reconcileRefunds", "Cycles of over-charge refunded: taken off the cores' later charges (ROI)"); s->append(&profRecRefund_);
    profRecPending_.init("reconcilePending", "Positive balance not applied at the dump (cycles)"); s->append(&profRecPending_);
    profRecPendingMax_.init("reconcilePendingMax", "The largest one core's positive balance not applied at the dump (cycles; the run time includes it)"); s->append(&profRecPendingMax_);
    profRecCharged_.init("reconciledCharged", "Sum of the charges of the reconciled accesses before any refund (cycles; reconcileRefunds cannot exceed it)"); s->append(&profRecCharged_);
    profRoiTail_.init("ctrlRoiTailTicks", "DRAM cycles past the window's close at which the ROI's last access a core waited for completed (0: within the window)"); s->append(&profRoiTail_);
    profRoiDrain_.init("ctrlRoiDrainTicks", "DRAM cycles past the window's close at which the ROI's last request completed (writebacks and posted stores drain after the cores; 0: within the window)"); s->append(&profRoiDrain_);
    profActivates_.init("ctrlActivates", "ACT commands the replayed controller issued (ROI; refresh-forced re-opens included; 1.12.18: the device interfaces only, see hostActivates)"); s->append(&profActivates_);   // 1.12.18
    if (sys_) { uint64_t w = 0, o = 0; bankTracked_ = sys_->pimid_bank_open_totals(w, o); }   // 1.12.18
    if (bankTracked_) {
        profBankOpenCycles_.init("bankOpenCycles", "Replayed DRAM cycles x units with a bank open (ROI; ruling R8-7 b / 4 c)"); s->append(&profBankOpenCycles_);
        profBankOpenWindow_.init("bankOpenWindow", "Replayed DRAM cycles x units sampled (the window bankOpenCycles is a fraction of)"); s->append(&profBankOpenWindow_);
    }
    /* 1.12.18 (user ruling R1 2026-10-24) */
    profORequests_.init("outsideRequests", "Device requests stamped after the ROI end: replayed (controller state), NOT priced, not in any count above"); s->append(&profORequests_);
    profOReads_.init("outsideReads", "Device reads stamped after the ROI end (not priced)"); s->append(&profOReads_);
    profOWrites_.init("outsideWrites", "Device writes stamped after the ROI end (not priced)"); s->append(&profOWrites_);
    profORowHits_.init("outsideRowHits", "Row hits of the requests outside the ROI (device and host)"); s->append(&profORowHits_);
    profORowMisses_.init("outsideRowMisses", "Row misses of the requests outside the ROI"); s->append(&profORowMisses_);
    profORowConflicts_.init("outsideRowConflicts", "Row conflicts of the requests outside the ROI"); s->append(&profORowConflicts_);
    profOActivates_.init("outsideActivates", "ACT commands issued for device requests outside the ROI (not priced)"); s->append(&profOActivates_);
    profOHostReads_.init("outsideHostReads", "Host reads stamped after the ROI end (not priced)"); s->append(&profOHostReads_);
    profOHostWrites_.init("outsideHostWrites", "Host writes stamped after the ROI end (not priced)"); s->append(&profOHostWrites_);
    profOHostActivates_.init("outsideHostActivates", "ACT commands issued for host requests outside the ROI (not priced)"); s->append(&profOHostActivates_);
    if (hostMis_ > 0) {   // 1.12.18 (ruling 17 / R0605)
        profHRequests_.init("hostRequests", "Host DRAM requests replayed through the controller (the ctrl* request/row/activate counters above exclude them)"); s->append(&profHRequests_);
        profHReads_.init("hostReads", "Replayed host reads"); s->append(&profHReads_);
        profHWrites_.init("hostWrites", "Replayed host writes (dirty writebacks)"); s->append(&profHWrites_);
        profHLatSum_.init("hostLatencySum", "Sum of measured host request latencies (HOST cycles; mean = hostLatencySum / hostRequests)"); s->append(&profHLatSum_);
        profHRLatSum_.init("hostReadLatencySum", "Sum of measured host READ latencies (HOST cycles; mean = hostReadLatencySum / hostReads; a writeback holds no core)"); s->append(&profHRLatSum_);   // (1.12.189)
        profHRowHits_.init("hostRowHits", "Controller row hits of the host requests"); s->append(&profHRowHits_);
        profHRowMisses_.init("hostRowMisses", "Controller row misses of the host requests (a closed bank)"); s->append(&profHRowMisses_);
        profHRowConflicts_.init("hostRowConflicts", "Controller row conflicts of the host requests (another row open)"); s->append(&profHRowConflicts_);
        profHActivates_.init("hostActivates", "ACT commands issued for host requests (ROI; refresh-forced re-opens included)"); s->append(&profHActivates_);
        profHPriced_.init("hostPriced", "Host accesses priced at the previous epoch's replayed service latency"); s->append(&profHPriced_);
        profHBootstrap_.init("hostBootstrap", "Host accesses priced by the analytical bootstrap (no replayed epoch yet)"); s->append(&profHBootstrap_);
    }
    parentStat->append(s);
}

uint64_t DramEpochReplay::composeAddr(uint32_t unit, uint32_t slot, uint64_t row, uint64_t lineAddr, uint32_t* chOut, int chipSel) {
    const auto& h = zinfo->hierarchy;
    const HierPos pos = unitToHierPos(unit, h.placementLevel, h.subarraysPerBank, h.banksPerBG,
                                      h.bgPerChip, h.chipsPerRank, h.ranksPerChannel);
    const uint64_t nch = (uint64_t)std::max(1, counts_[0]);
    /* Channel: HBM folds its channels into chips_per_rank (the chip IS the
     * channel); the DDR/LPDDR/GDDR classes carry the channel above the rank. */
    const uint64_t ch = (uint64_t)(foldedChannels_ ? pos.chip : pos.channel) % nch;
    /* Bank index inside the channel: (rank, bank group, bank) of the unit plus
     * the interface's slot inside it (a unit coarser than a bank spans
     * rowSlotsPerUnit banks). Chips of a DDR rank work in lock step, so the
     * chip index names no separate Ramulator bank. */
    const uint64_t bpg = std::max(1u, h.banksPerBG), bgc = std::max(1u, h.bgPerChip);
    /* 1.12.18: at a chip split the chip names its own Ramulator rank (chips_ = 1 otherwise: unchanged) */
    const uint64_t chip = chipSplit_ ? (uint64_t)((chipSel >= 0) ? (uint32_t)chipSel : pos.chip) % chips_ : 0;
    uint64_t flat = (((uint64_t)(foldedChannels_ ? 0 : pos.rank) * chips_ + chip) * bgc + (uint64_t)pos.bank_group) * bpg + (uint64_t)pos.bank + slot;
    flat %= banksPerChannel_;
    /* Mixed radix in ChRaBaRoCo order: channel, levels 1..row-1, row, column. */
    uint64_t addr = ch;
    std::vector<uint64_t> lv((size_t)rowIdx_, 0);
    for (int i = rowIdx_ - 1; i >= 1; i--) { const uint64_t c = (uint64_t)std::max(1, counts_[i]); lv[i] = flat % c; flat /= c; }
    for (int i = 1; i < rowIdx_; i++) addr = addr * (uint64_t)std::max(1, counts_[i]) + lv[i];
    addr = addr * (uint64_t)std::max(1, counts_[rowIdx_]) + (row % (uint64_t)std::max(1, counts_[rowIdx_]));
    const uint64_t col = ((lineAddr << lineBits_) >> txBits_) % colCount_;
    addr = addr * colCount_ + col;
    if (chOut) *chOut = (uint32_t)ch;
    return addr << txBits_;
}

void DramEpochReplay::record(uint64_t epoch, uint64_t stamp, uint32_t unit, uint32_t slot, uint64_t row,
                             uint64_t lineAddr, bool isWrite, uint32_t mi, uint32_t charged, bool tail, uint32_t core, bool holds) {
    /* 1.11.98: the window closes with the ROI, as every other counter does
     * (post-termination accesses are not counted anywhere). */
    t_lastOk = false;   // (1.12.189)
    if (zinfo->terminationConditionMet) return;
    uint32_t ch = 0;
    const uint64_t addr = composeAddr(unit, slot, row, lineAddr, &ch);
    t_recChg = charged;   // (1.12.153)
    t_recTail = tail;     // (1.12.181)
    t_recCore = core; t_recHold = holds ? 1 : 0;   // (1.12.189): the core, whether it waits for the access
    recordAddr(epoch, stamp, addr, ch, isWrite, mi);
    t_recChg = 0;
    t_recTail = false;
    t_recCore = ~0u; t_recHold = 0;
}

void DramEpochReplay::recordAddr(uint64_t epoch, uint64_t stamp, uint64_t addr, uint32_t ch, bool isWrite, uint32_t mi, uint8_t shadow, uint32_t grp) {
    futex_lock(&lock_);
    /* 1.12.17 cut mode: a record for an epoch the cut has already folded (a
     * core that was exempt from the cut while parked in an MPI wait) joins
     * the next unfolded epoch -- its stamp is behind the replay clock, so it
     * issues at the clock, as the shared-memory replay does with a late
     * stamp; a folded epoch is never replayed twice. */
    if (cutMode_ && anyFolded_ && epoch <= lastFolded_) { epoch = lastFolded_ + 1; late_++; }
    Rec rc{stamp, addr, mi, (uint8_t)(isWrite ? 1 : 0), ch, shadow, grp,
           (mpiHost_ && isHostMi(mi)) ? t_recSrc : ~0u, t_recChg, (uint8_t)(t_recTail ? 1 : 0)};   // (1.12.153): source, charge; (1.12.181): tail
    rc.core = t_recCore; rc.hold = t_recHold; rc.dmab = t_recDma;   // (1.12.189)
    if (t_recPf && isHostMi(mi)) {
        rc.pf = 1;
        if (t_pfTag && clOn() && roiMarked_ && rc.core < kPfCores) {
            rc.pfTag = t_pfTag;
            PfShard& k = g_pfSh[rc.core];
            std::lock_guard<std::mutex> g(k.m);
            k.inst = this; k.ent[t_pfTag] = PfEnt();
        }
    }
    if (rc.core < kRecCores && syncP_ && syncP_[rc.core].exchange(0, std::memory_order_relaxed)) rc.sync = 1;   // (1.12.189)
    if (clOn() && roiMarked_ && rc.core != ~0u && rc.core < kRecCores) {
        /* (1.12.189): the delay applied to the core since its last record (its records from here on carry it), and a
         * refund of the core's over-charges, which lowers this access's charge */
        auto k = cl_.find(rc.core);
        if (k != cl_.end() && k->second.pendDly) { rc.dly = (int32_t)std::max<int64_t>(INT32_MIN, std::min<int64_t>(k->second.pendDly, INT32_MAX)); k->second.pendDly -= rc.dly; }
        if (rc.hold && rc.chg) {
            const int64_t av = pool_[rc.core].load(std::memory_order_relaxed);
            const uint32_t take = (uint32_t)std::min<int64_t>(av, (int64_t)rc.chg);
            if (take) {
                const double tu = (double)take / ovA_[rc.core].load(std::memory_order_relaxed);
                pool_[rc.core].fetch_sub(take, std::memory_order_relaxed); used_[rc.core].fetch_add((int64_t)std::llround(tu * 1024.0), std::memory_order_relaxed);
                rc.ref = take; rc.refT = (float)tu; recRefund_ += take;
            }
        }
    }
    std::vector<Rec>& pv = pending_[epoch];
    pv.push_back(rc);
    t_lastOk = true; t_lastEp = epoch; t_lastIdx = pv.size() - 1; t_lastCore = rc.core; t_lastRef = rc.ref; t_lastStamp = stamp;   // (1.12.189)
    if (ch < perChan_.size()) perChan_[ch]++;
    futex_unlock(&lock_);
}

void DramEpochReplay::setHostPrefetch(bool on) { t_recPf = on; }
void DramEpochReplay::setPrefetchTag(uint32_t tag) { t_pfTag = tag; }

void DramEpochReplay::prefetchConsumed(uint32_t tag, uint32_t core, uint64_t waitStart, uint64_t estArrival, uint64_t issueCycle) {
    if (!tag || core >= kPfCores) return;
    PfShard& k = g_pfSh[core];
    DramEpochReplay* inst = nullptr; int64_t corr = 0;
    {
        std::lock_guard<std::mutex> g(k.m);
        auto pe = k.ent.find(tag);
        if (pe == k.ent.end() || pe->second.cons) return;   // the prefetch made no replayed request (an LLC hit), or it was outside the ROI
        pe->second.cons = true; pe->second.waitStart = waitStart; pe->second.estArr = estArrival; pe->second.iss = issueCycle;
        k.ord.emplace(std::make_pair(waitStart, k.ordSeq++), tag);
        inst = k.inst;
        corr = pfDrain(k);
    }
    if (corr && inst) { futex_lock(&inst->lock_); inst->pfApply(core, corr); futex_unlock(&inst->lock_); }
}

void DramEpochReplay::prefetchDropped(uint32_t tag, uint32_t core) {
    if (!tag || core >= kPfCores) return;
    PfShard& k = g_pfSh[core];
    std::lock_guard<std::mutex> g(k.m);
    auto pe = k.ent.find(tag);
    if (pe == k.ent.end() || pe->second.cons) return;
    if (pe->second.done) k.ent.erase(pe); else pe->second.dropped = true;
}

/* the settled change of a core's stall on prefetches into its balance (the union already counts overlapping waits
 * once). Under lock_. */
void DramEpochReplay::pfApply(uint32_t core, int64_t corr) {
    if (!clOn() || !roiMarked_) return;
    auto k = cl_.find(core);
    if (k == cl_.end() || !k->second.on) return;
    k->second.diff += (double)corr;
}

uint32_t DramEpochReplay::hostAccess(uint32_t mi, uint64_t lineAddr, uint64_t cycle, uint32_t srcId, bool isWrite, bool adjustedClock) {
    /* 1.12.18 (ruling 17 / R0605): the host twin of the interface hook in
     * PEMemoryInterface::localAccessLatency -- the same epoch key, stamp axis,
     * cut discipline and read lag, so a host record and a device record of
     * one phase sit on one axis. The stamp is the zsim phase grid, the one
     * axis every core shares (each core advances phaseLength of its own
     * cycles per phase); the DRAM clock reads it at the instance's clock, and
     * the latency is converted to host cycles per interface. */
    if (!sys_ || !isHostMi(mi)) return 0;
    t_recSrc = srcId;   // (1.12.153): the source of the records below
    t_lastOk = false;   // (1.12.189)
    const bool detMpi = PEMemoryInterface::cutPricing();   // 1.12.18: thread-MPI or single-process cut
    if (cutMode_ != detMpi)
        panic("DramEpochReplay %s: cut mode %d disagrees with the pricing mode %d (host interface)",
              name_.c_str(), (int)cutMode_, (int)detMpi);
    if (cutMode_ && !zinfo->hierarchy.mpiNocBaselined) { rawHBoot_.fetch_add(1, std::memory_order_relaxed); return 0; }
    const uint32_t E = epochPhases();   // (1.12.145, ruling 2026-10-28): the replay's own epoch
    uint64_t epoch, stamp;
    if (cutMode_) {
        const uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
        /* 1.12.18: the ROI-relative stamp needs the source core's ROI
         * baseline. A co-sim host core that never took one (thread-MPI
         * re-baselines only rank 0's own core, by then its device PE; the
         * host core keeps roiBaseCycle 0) gave its ABSOLUTE cycle here: the
         * roi_begin flush writebacks of cosim_HBM3_mpi_gemv were stamped at
         * 15.3 M cycles, the replay clock ran to 49.3 M DRAM cycles for a
         * 0.29 M-cycle ROI, and refresh and the bank-open fraction were
         * measured over that span. Such a core is stamped on the ROI-relative
         * phase grid instead (phases since the ROI baseline phase, plus the
         * core's intra-phase offset): the axis the shared-memory replay puts
         * host records on, here shifted to the ROI origin. */
        const bool based = srcId < zinfo->numCores && zinfo->cores[srcId] && zinfo->cores[srcId]->getRoiBaseCycle() > 0;
        /* 1.12.18 (R2, B3): a host core keeps no roiBaseCycle of its own
         * (OOOCore/InOrderCore do not export it), but the ROI baseline
         * snapshot (the first ROI barrier) recorded its wall-free clock
         * (Core::getCycles()); the stamp is that clock relative to it (a
         * single-process cut; a thread-MPI host core is stamped on the phase
         * grid below). A memory request carries the RAW clock (curCycle, halted and
         * phantom-wait cycles included): it is moved to the wall-free clock
         * by the core's present offset between the two. */
        /* 1.12.18 (W9): with the ROI baseline snapshot taken, every core
         * has an origin on its wall-free clock (PEMemoryInterface::
         * roiOriginOf; 0 for a core that had not run yet -- it used to fall to
         * the phase grid here while the run time skipped it); the request's
         * raw cycle is moved to that clock (wallFreeCycle; the flush passes
         * the wall-free clock already). */
        const uint64_t* hb = zinfo->hierarchy.mpiRoiBaseCyc;
        const bool hostBased = !based && hb && srcId < zinfo->numCores && srcId < zinfo->hierarchy.mpiRoiBaseCycN && zinfo->cores[srcId];
        if (devRequesters_) {
            /* (1.12.147, user ruling 2026-10-28): a device element at the host
             * memory controller (HOST_MC): the device interfaces' stamp and
             * ROI-end note (PEMemoryInterface::localAccessLatency) */
            stamp = PEMemoryInterface::cutRoiRel(srcId, cycle);
            if (PEMemoryInterface::singleProcCut() && srcId < zinfo->numCores && zinfo->cores[srcId] && !zinfo->cores[srcId]->wakeStampable())
                PEMemoryInterface::clkNoteRec(srcId, stamp);
        } else if (based) {
            stamp = PEMemoryInterface::perCoreRoiRel(srcId, cycle);
        } else if (hostBased && !mpiHost_) {
            const uint64_t c = adjustedClock ? cycle : PEMemoryInterface::wallFreeCycle(srcId, cycle);
            stamp = PEMemoryInterface::roiRelOf(srcId, c);   // (1.12.189): a signed origin
            hostCoreStamps_.fetch_add(1, std::memory_order_relaxed);
        } else {
            /* The ROI-relative phase grid: phases since the ROI baseline, plus the core's offset in its phase -- the
             * axis the shared-memory replay stamps host records on. A thread-MPI host core is stamped here: its own
             * clock stops while its rank waits, so on that clock a rank that had waited trailed the others and its
             * records fell behind the replay clock; on the grid every host record lands in the epoch of the phase it
             * was issued in, and the run time of a host-rank run is counted on the same grid. A coherence-flush
             * writeback of such a core passes the flushing core's clock plus its offset into the flush
             * (adjustedClock): it keeps that pacing from the flush's place on the grid. */
            const uint64_t bp = zinfo->hierarchy.mpiNocRoiBasePhase;
            const uint64_t rel = (zinfo->numPhases >= bp) ? zinfo->numPhases - bp : 0;
            uint64_t off = cycle % pl;
            if (adjustedClock && mpiHost_ && srcId < zinfo->numCores && zinfo->cores[srcId]) {
                const uint64_t now = zinfo->cores[srcId]->getCycles();
                off = (now % pl) + (cycle > now ? cycle - now : 0);
            }
            stamp = rel * pl + off;
            hostPhaseStamps_.fetch_add(1, std::memory_order_relaxed);
        }
        epoch = stamp / ((uint64_t)pl * E);
    } else {
        const uint64_t bp = zinfo->hierarchy.mpiNocRoiBasePhase;
        const uint64_t rel = (zinfo->numPhases >= bp) ? zinfo->numPhases - bp : 0;
        epoch = rel / E;
        stamp = PEMemoryInterface::phaseStamp(cycle);
    }
    /* (1.12.189): priced first, so the record carries its charge, its core and whether the core waits for it, as at a
     * device interface; the price reads epochs before `epoch`, which this record does not join */
    const uint32_t v = cutMode_ ? serviceLatencyCut(mi, epoch, kDramReadLagEpochs, isWrite, t_recPf && !isWrite)
                                : serviceLatency(mi, epoch, isWrite, t_recPf && !isWrite);   // (1.12.189): a prefetch's read at the prefetches' price
    t_recChg = v; t_recCore = srcId; t_recHold = (isWrite || t_recPf) ? 0 : 1;   // a prefetch: no core waits for it
    if (!zinfo->terminationConditionMet) {
        uint64_t addr = 0; uint32_t ch = 0;
        if (hostOnly_) {
            /* the host memory's own controller: the line's physical address,
             * decoded by Ramulator's address mapper from the YAML */
            addr = lineAddr << lineBits_;
            if (hostRoBaRaCoCh_ && !counts_.empty()) ch = (uint32_t)((addr >> txBits_) % (uint64_t)std::max(1, counts_[0]));
        } else {
            if (!hostTarget_) panic("DramEpochReplay %s: a host interface on the shared array needs the device placement (setHostTarget)", name_.c_str());
            uint32_t unit = 0, slot = 0; uint64_t row = 0;
            hostTarget_(lineAddr, unit, slot, row);
            if (chipSplit_) {
                /* 1.12.18 (user ruling 2026-10-24): a host access drives every chip of the rank in lock step:
                 * its row is chips_ x the per-chip row (the PE row model's stripe is one chip's page), the same
                 * bank and row in every chip. (1.12.151): sent ONCE, at chip 0's address; the controller
                 * (pimid_lockstep_chips) drives the rank's chips with it -- one queue entry, one rank-wide command
                 * per step on the CA bus, one burst. Before, each chip was a request of its own (copy 0 counted,
                 * the others source_id 4/6 shadows), so the queue held 4 entries per host request. */
                const uint64_t s = hostRowSlots_ ? hostRowSlots_ : 1;
                const uint64_t stripe = (row * s + slot) / chips_;
                slot = (uint32_t)(stripe % s); row = stripe / s;
                addr = composeAddr(unit, slot, row, lineAddr, &ch, 0);
                recordAddr(epoch, stamp, addr, ch, isWrite, mi);
            } else {
                addr = composeAddr(unit, slot, row, lineAddr, &ch);
                recordAddr(epoch, stamp, addr, ch, isWrite, mi);
            }
        }
        if (hostOnly_) recordAddr(epoch, stamp, addr, ch, isWrite, mi);
    }
    t_recChg = 0; t_recCore = ~0u; t_recHold = 0;
    if (v > 0) rawHPriced_.fetch_add(1, std::memory_order_relaxed); else rawHBoot_.fetch_add(1, std::memory_order_relaxed);
    return v;
}

uint32_t DramEpochReplay::serviceLatency(uint32_t mi, uint64_t curEpoch, bool write, bool pf) {
    if (curEpoch == 0 || mi >= numMIs_) return 0;
    const uint32_t sl = priceSlot(mi, write, pf);   // (1.12.189)
    futex_lock(&lock_);
    /* every epoch below curEpoch is complete (the phase barrier that ended it
     * has passed for every core): replay the ones not replayed yet, in order. */
    while (!pending_.empty() && pending_.begin()->first < curEpoch) {
        uint64_t ep = pending_.begin()->first;
        replayEpoch(ep);
        pending_.erase(ep);
    }
    uint32_t v = 0;
    auto it = frozen_.find(curEpoch - 1);
    if (it != frozen_.end() && sl < it->second.size()) v = it->second[sl];
    else if (it == frozen_.end()) v = partialMean(curEpoch - 1, sl);   // 1.12.18: still in flight
    /* 1.11.98 (gate 1208B): an interface with no access in the previous epoch
     * keeps the last service latency the replay measured for it (bursty
     * interfaces otherwise fell back to the analytical service every other
     * epoch); the analytical bootstrap only before its first replayed value. */
    if (v == 0 && sl < lastValue_.size()) v = lastValue_[sl];
    /* keep the frozen epochs near the current one. Pruning the SMALLEST keys
     * (1208B) evicted every new ROI epoch: the ROI snapshot restarts epoch
     * numbering at 0 while stale pre-ROI keys were larger. */
    while (!frozen_.empty() && frozen_.begin()->first + 8 < curEpoch) frozen_.erase(frozen_.begin());
    futex_unlock(&lock_);
    return v;
}

void DramEpochReplay::foldByCut(uint64_t cut, uint64_t epochLen) {
    if (!cutMode_ || epochLen == 0) return;
    futex_lock(&lock_);
    /* every epoch whose whole span is at or below the cut is final: no active
     * core can record into it any more. Replay them in order. */
    while (!pending_.empty()) {
        const uint64_t ep = pending_.begin()->first;
        if (cut != ~0ull && (ep + 1) * epochLen > cut) break;
        replayEpoch(ep);
        pending_.erase(ep);
    }
    futex_unlock(&lock_);
}

uint32_t DramEpochReplay::serviceLatencyCut(uint32_t mi, uint64_t epoch, uint64_t lag, bool write, bool pf) {
    if (!cutMode_ || mi >= numMIs_) return 0;
    const uint32_t sl = priceSlot(mi, write, pf);   // (1.12.189): the access's own kind
    /* an access of epoch 1 (before the first epoch the lag can name exists) reads epoch 0 once it has been fed, as
     * an access of epoch k reads k-2; an access of epoch 0 keeps the bootstrap */
    if (epoch == 0) return 0;
    const uint64_t r = epoch < lag ? epoch - 1 : epoch - lag;
    futex_lock(&lock_);
    uint32_t v = 0;
    /* the latest folded epoch at or before r: r itself once the cut has
     * passed it (the lag is chosen so it has, as for the NoC); an earlier one
     * when the cut trails further (counted: stale_) */
    auto it = carried_.upper_bound(r);
    /* 1.12.18: epoch r fed but still in flight (drain the lag: its last
     * requests have not completed): the mean of its requests completed so
     * far, not an older final epoch's value */
    /* (1.12.145, user ruling 2026-10-28): the newest epoch at or before r the controller has been fed, final or
     * not. When the cut trails (r not fed yet) and the newest fed epoch is still in flight, its aged partial
     * mean is newer than any final value; such a read used to fall back to an older final epoch or, when
     * none had finished, to the analytical bootstrap (16-core DDR5 pointer chase: 84 022 of 87 893 priced
     * reads), so the cores issued far faster than the controller served. */
    uint64_t fedE = ~0ull;
    for (auto a = acc_.begin(); a != acc_.end() && a->first <= r; ++a) if (a->second.fed) fedE = a->first;
    const bool newerFed = fedE != ~0ull && (it == carried_.begin() || std::prev(it)->first < fedE);
    uint64_t e0 = ~0ull, n0 = 0;   // (1.12.153): the epoch read and its request count, for the price window
    if (newerFed && (v = partialMean(fedE, sl)) > 0) {
        partial_++;
        if (fedE != r) stale_++;
        e0 = fedE;
        auto a = acc_.find(fedE); auto pc = pcache_.find(fedE);
        n0 = ((a != acc_.end() && sl < a->second.cnt.size()) ? a->second.cnt[sl] : 0)
           + ((pc != pcache_.end() && sl < pc->second.cnt.size()) ? pc->second.cnt[sl] : 0);
    } else if (it != carried_.begin()) {
        --it;
        if (it->first != r) stale_++;
        if (sl < it->second.size()) v = it->second[sl];
        e0 = it->first;
        auto w = winCnt_.find(e0);
        n0 = (w != winCnt_.end() && sl < w->second.size()) ? w->second[sl] : 0;
    }
    /* (1.12.153, user ruling 2026-10-28): the price window -- the request-weighted mean over the epoch read and the
     * priceWindow() - 1 finalized epochs before it (an interface idle in the epoch read weighs its carried value
     * once) */
    const uint32_t W = priceWindow();
    if (W > 1 && v > 0 && e0 != ~0ull) {
        /* (1.12.189): each epoch weighs its mean by its request count of this kind (see priceSlot) */
        double s = (double)v * (double)(n0 ? n0 : 1), c = (double)(n0 ? n0 : 1);
        uint32_t k = 1;
        for (auto w = winSum_.lower_bound(e0); w != winSum_.begin() && k < W; k++) {
            --w;
            auto wc = winCnt_.find(w->first);
            if (sl < w->second.size() && wc != winCnt_.end()) {
                s += (double)w->second[sl]; c += (double)wc->second[sl];
            }
        }
        v = std::max<uint32_t>(1, (uint32_t)(s / c + 0.5));   // (1.12.189): see partialMean
    }
    /* (1.12.189): the closed loop -- see compBin_ */
    if (v == 0 && clOn() && roiMarked_ && sl < compBin_.size()) v = compBin_[sl].last;
    while (carried_.size() > 1 && carried_.begin()->first + 16 < r && std::next(carried_.begin())->first <= r)
            carried_.erase(carried_.begin());
    futex_unlock(&lock_);
    return v;
}

void DramEpochReplay::bankTotals() {
    if (par()) {   // (1.12.144): the sums over the channel instances
        Raw t;
        for (Chan& K : chans_) {
            uint64_t w = 0, o = 0, a = 0;
            if (bankTracked_ && K.sys->pimid_bank_open_totals(w, o)) { t.bankW += w; t.bankO += o; }
            if (K.sys->pimid_cmd_totals(a)) t.acts += a;
            if (pdOn_) { uint64_t pc = 0, pu = 0, px = 0; K.sys->pimid_pd_totals(pc, pu, px); t.pdC += pc; t.pdU += pu; t.pdX += px; }
            uint64_t sh = 0, sm = 0, sc = 0, sa = 0;
            if (hostMis_ > 0 && K.sys->pimid_src_totals(sh, sm, sc, sa)) { t.hhits += sh; t.hmisses += sm; t.hconflicts += sc; t.hacts += sa; }
            uint64_t oh = 0, om = 0, oc = 0, oa = 0, oha = 0;
            if (K.sys->pimid_out_totals(oh, om, oc, oa, oha)) { t.ohits += oh; t.omisses += om; t.oconflicts += oc; t.oacts += oa; t.ohacts += oha; }
        }
        if (bankTracked_) { raw_.bankW = t.bankW; raw_.bankO = t.bankO; }
        raw_.acts = t.acts;
        if (pdOn_) { raw_.pdC = t.pdC; raw_.pdU = t.pdU; raw_.pdX = t.pdX; }
        if (hostMis_ > 0) { raw_.hhits = t.hhits; raw_.hmisses = t.hmisses; raw_.hconflicts = t.hconflicts; raw_.hacts = t.hacts; }
        raw_.ohits = t.ohits; raw_.omisses = t.omisses; raw_.oconflicts = t.oconflicts; raw_.oacts = t.oacts; raw_.ohacts = t.ohacts;
        return;
    }
    uint64_t w = 0, o = 0, a = 0;
    if (sys_ && bankTracked_ && sys_->pimid_bank_open_totals(w, o)) { raw_.bankW = w; raw_.bankO = o; }
    if (sys_ && sys_->pimid_cmd_totals(a)) raw_.acts = a;   // 1.12.18
    if (pdOn_ && sys_) sys_->pimid_pd_totals(raw_.pdC, raw_.pdU, raw_.pdX);   // 1.12.19
    uint64_t sh = 0, sm = 0, sc = 0, sa = 0;   // 1.12.18 (R0605): the host requests' share (source_id 1)
    if (sys_ && hostMis_ > 0 && sys_->pimid_src_totals(sh, sm, sc, sa)) {
        raw_.hhits = sh; raw_.hmisses = sm; raw_.hconflicts = sc; raw_.hacts = sa;
    }
    uint64_t oh = 0, om = 0, oc = 0, oa = 0, oha = 0;   // 1.12.18 (R1): source_id 2/3, outside the ROI
    if (sys_ && sys_->pimid_out_totals(oh, om, oc, oa, oha)) {
        raw_.ohits = oh; raw_.omisses = om; raw_.oconflicts = oc; raw_.oacts = oa; raw_.ohacts = oha;
    }
}

void DramEpochReplay::tickOnce() {
    sys_->tick();
    curTick_++;
    raw_.ticks++;
    if (outstanding_ == 0 && waitingN_ == 0) raw_.idle++;   // row 28: a controller cycle with nothing in flight
    if (waitingN_ > 0) serveDoors();   // 1.12.18 (drain the lag)
    if (!rel_.empty()) serveRel();     // (1.12.189): the closed-loop replay's released records
    if (closeArmed_ && !closeSnapped_ && curTick_ >= closeTick_) snapClose();   // 1.12.18: the ROI end
    else if (roiMarked_ && (!closeArmed_ || keepHist_ || clOn()) && (curTick_ % kSnapEvery) == 0) { Snap s; takeSnap(s); hist_.push_back(s); }   // 1.12.18 (shift by lag); (1.12.189): and past the close under the closed loop (the run time can be raised after the final drain)
}

/* 1.12.18 (R2): DRAM ticks per cycle of the clock roiWall.mpiEnd is counted
 * in -- the rank cores' (the device PEs', or the host cores' of a no-offload
 * baseline). */
double DramEpochReplay::runTicksPerCycle() const {
    const uint32_t f = zinfo->roiWall.mpiEndFreqMHz;
    if (f == 0 || (uint64_t)f * 1000000ull == coreFreqHz_ || !(tckNs_ > 0.0)) return ticksPerCycle_;
    return (1e3 / (double)f) / tckNs_;
}

/* 1.12.18 (B4): the phase-mode pricing window, in this instance's core
 * cycles: the power model prices a stamped ROI over max(run time, busy max)
 * per node at that node's clock (runWindowCycles / the system-scope run-time
 * window), so the replay closes there too. Read from the same per-core stats
 * the dump writes (the "cycles" lines), evaluated now -- finalize runs right
 * before every dump. A host core's cCycles is not added: the parser stopped
 * adding it in 1.12.75 (the unhalted cycles already hold the contention
 * stalls), and adding it here closed the replay past the priced window. */
uint64_t DramEpochReplay::phaseWindowCycles(std::string* why, bool withRunTime) {
    const uint64_t roi = withRunTime ? zinfo->roiWall.cycles() : 0;
    const uint32_t nn = std::min<uint32_t>(zinfo->nodeMap.numNodes, 16);
    std::vector<uint64_t> wall(nn ? nn : 1, 0);
    uint32_t k = 0;
    AggregateStat* root = zinfo->rootStat;
    for (uint32_t g = 0; root && g < root->size(); g++) {
        AggregateStat* grp = root->get(g)->asAggregate();
        if (!grp || std::string(grp->desc()) != "Core stats") continue;
        for (uint32_t c = 0; c < grp->size(); c++, k++) {
            AggregateStat* cs = grp->get(c)->asAggregate();
            if (!cs) continue;
            uint64_t cyc = 0, cc = 0;
            for (uint32_t i = 0; i < cs->size(); i++) {
                Stat* st = cs->get(i);
                ScalarStat* sc = st->asScalar();
                if (!sc) continue;
                if (std::string(st->name()) == "cycles" && std::string(st->desc()).rfind("Simulated", 0) == 0) cyc = sc->get();
                else if (std::string(st->name()) == "cCycles") cc = sc->get();
            }
            uint32_t node = 0;
            bool host = false;
            for (uint32_t n = 0; n < nn; n++)
                if (k >= zinfo->nodeMap.nodes[n].coreStart && k < zinfo->nodeMap.nodes[n].coreEnd) { node = n; host = zinfo->nodeMap.nodes[n].role == 0; break; }
            (void)cc; (void)host;
            const uint64_t w = cyc;
            if (w > wall[node]) wall[node] = w;
        }
    }
    double best = (double)roi;
    std::string src = "run time";
    for (uint32_t n = 0; n < wall.size(); n++) {
        if (wall[n] == 0) continue;
        const double f = nn ? (double)zinfo->nodeMap.nodes[n].freqMHz * 1e6 : (double)coreFreqHz_;
        if (!(f > 0.0)) continue;
        const double v = (double)std::max(wall[n], roi) * (double)coreFreqHz_ / f;
        if (v > best + 0.5) { best = v; src = "busy max"; }
    }
    if (why) *why = src;
    return (uint64_t)(best + 0.5);
}

/* 1.12.18: see closeArmed_ in the header. Called under lock_. */
void DramEpochReplay::armClose(bool fromFinalize) {
    if (!roiMarked_ || closeArmed_ || !zinfo->roiWall.ended) return;
    if (cutMode_ && zinfo->roiWall.source == 3) {
        /* A single-process run in cut mode closes at its run time, raised to the busy max as the power model
         * prices it (as source 1 below). The latest device PE ROI end is read on the PE clock, which for an
         * out-of-order or in-order core excludes wall waits and can lie below its busy cycles; the run time printed
         * and priced is then the busy max, and a replay closed at the lower ROI end would cover only part of the
         * priced window (refresh and background energy short by the same fraction). Armed at the dump, where the
         * busy counts are final. */
        if (!fromFinalize) return;
        const uint64_t run = (uint64_t)((double)zinfo->roiWall.mpiEnd * runTicksPerCycle());
        std::string why;
        const uint64_t busyCyc = phaseWindowCycles(&why, false);
        const uint64_t busy = (uint64_t)((double)busyCyc * ticksPerCycle_);
        closeTick_ = roiTick0_ + std::max(run, busy);
        if (busy > run)
            info("[mem] %s: replay window = the pricing window: run time %lu cycles raised to the busy max %lu (core clock of this replay)",
                 name_.c_str(), (unsigned long)zinfo->roiWall.mpiEnd, (unsigned long)busyCyc);
    } else if (cutMode_ && zinfo->roiWall.source == 1) {
        /* 1.12.18 (ruling 2026-10-24): a single-process run in cut mode closes
         * at the pricing window (run time raised to the busy max, B4), counted
         * from the ROI's DRAM tick on the ROI-relative axis; armed at the dump
         * like phase mode (the busy counts are final there). */
        if (!fromFinalize) return;
        std::string why;
        const uint64_t w = phaseWindowCycles(&why);
        closeTick_ = roiTick0_ + (uint64_t)((double)w * ticksPerCycle_);
        if (w != zinfo->roiWall.cycles())
            info("[mem] %s: 1.12.18: replay window = the pricing window: run time %lu cycles raised to the busy max %lu (core clock of this replay) [B4]",
                 name_.c_str(), (unsigned long)zinfo->roiWall.cycles(), (unsigned long)w);
    } else if (cutMode_) {
        if (zinfo->roiWall.source != 2) return;
        closeTick_ = roiTick0_ + (uint64_t)((double)zinfo->roiWall.mpiEnd * runTicksPerCycle());
    } else {
        if (!fromFinalize) return;   // phase mode: the busy counts are final at the dump
        if (zinfo->roiWall.source == 1) {
            std::string why;
            const uint64_t w = phaseWindowCycles(&why);
            closeTick_ = (uint64_t)((double)(zinfo->roiWall.beginStamp + w) * ticksPerCycle_);
            if (w != zinfo->roiWall.cycles())
                info("[mem] %s: 1.12.18: replay window = the pricing window: run time %lu cycles raised to the busy max %lu (core clock of this replay) [B4]",
                     name_.c_str(), (unsigned long)zinfo->roiWall.cycles(), (unsigned long)w);
        } else if (zinfo->pgres.kernelClosed) {
            const uint64_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
            closeTick_ = (uint64_t)((double)(zinfo->pgres.closePhase * pl) * ticksPerCycle_);
        } else {
            return;
        }
    }
    closeArmed_ = true;
    if (curTick_ >= closeTick_) {
        /* 1.12.18 (shift by lag): the clock can stand well past the close when
         * it becomes known (a shifted epoch runs late). The window counters
         * come from the last periodic snapshot at or before the close (every
         * kSnapEvery DRAM cycles), not from now. */
        closeLate_ = curTick_ - closeTick_;
        const Snap* best = nullptr;
        for (const Snap& s : hist_) { if (s.tick <= closeTick_) best = &s; else break; }
        if (best) applySnap(*best); else snapClose();
    }
    if (!keepHist_ && !clOn()) { hist_.clear(); hist_.shrink_to_fit(); }   // (1.12.189): kept under the closed loop
}

/* 1.12.18: cut mode -- see closeArmed_ in the header. Called under lock_. */

void DramEpochReplay::takeSnap(Snap& s) {
    if (par()) {   // (1.12.144): between calls: the channel sums
        s.tick = curTick_; s.ticks = raw_.ticks; s.idle = raw_.idle; s.refreshes = 0;
        s.bankW = raw_.bankW; s.bankO = raw_.bankO; s.pdC = 0; s.pdX = 0;
        uint64_t bw = 0, bo = 0;
        for (Chan& K : chans_) { ChanSnap p; chanTakeSnap(K, p); s.refreshes += p.refreshes; bw += p.bankW; bo += p.bankO; s.pdC += p.pdC; s.pdX += p.pdX; }
        if (bankTracked_) { s.bankW = bw; s.bankO = bo; }
        return;
    }
    uint64_t h = 0, m = 0, c = 0, r = 0;
    ctrlTotals(h, m, c, r);
    s.tick = curTick_; s.ticks = raw_.ticks; s.idle = raw_.idle; s.refreshes = r;
    s.bankW = raw_.bankW; s.bankO = raw_.bankO;
    uint64_t w = 0, o = 0;
    if (sys_ && bankTracked_ && sys_->pimid_bank_open_totals(w, o)) { s.bankW = w; s.bankO = o; }
    if (pdOn_ && sys_) { uint64_t u = 0; sys_->pimid_pd_totals(s.pdC, u, s.pdX); }   // 1.12.19
}

void DramEpochReplay::applySnap(const Snap& s) {
    snapTicks_ = s.ticks; snapIdle_ = s.idle; snapRefreshes_ = s.refreshes;
    snapBankW_ = s.bankW; snapBankO_ = s.bankO;
    snapPdC_ = s.pdC; snapPdX_ = s.pdX;   // 1.12.19
    closeSnapped_ = true;
}

void DramEpochReplay::snapClose() {
    Snap s; takeSnap(s); applySnap(s);
}

void DramEpochReplay::tickTo(uint64_t tick) {
    if (par()) { tickToPar(tick); return; }   // (1.12.144)
    while (curTick_ < tick) tickOnce();
}

/* 1.12.18 (user ruling 2026-10-24, "drain the lag"): each record issues at
 * max(its own stamp, the instant its controller queue can take it). A full
 * queue holds the record at that queue's door -- FIFO per channel and
 * direction, so a read never waits behind a full write queue -- and the
 * backlog drains as the controller frees entries; the lag goes back to zero
 * whenever the controller has idle time. Epochs are no longer drained to
 * completion before the next epoch issues: that forced drain was what pushed
 * a late epoch's records onto one instant (the 1.12.17 collapse) or, under
 * the earlier shift-by-lag rule, kept a stale lag for the rest of the run (cosim_DDR5_mpi_gemv:
 * 3k-23k ROI requests shifted past the close). An epoch's measured service
 * latency is final when its last request completes (finalizeEpoch); until
 * then a reader gets the latest final value, as for an idle interface. */
bool DramEpochReplay::acceptOne(Waiting& w) {
    const Rec& r = w.r;
    const uint64_t lineBytes = 1ull << lineBits_;
    const uint64_t step = lineBytes / burstsPerLine_;
    const int typeId = r.wr ? Ramulator::Request::Type::Write : Ramulator::Request::Type::Read;
    for (; w.b < burstsPerLine_; ++w.b) {
        const uint64_t id = w.id;
        auto cb = [this, id](Ramulator::Request&) {
            auto it = inflight_.find(id);
            if (it == inflight_.end()) return;
            Inflight& f = it->second;
            if (f.remaining > 1) { f.remaining--; return; }
            const uint64_t lat = (curTick_ + 1 > f.issueTick) ? (curTick_ + 1 - f.issueTick) : 1;
            /* 1.12.18 (R0605): in the interface's own cycles (a host interface: host cycles) */
            const double tpc = (f.mi < miTicksPerCycle_.size()) ? miTicksPerCycle_[f.mi] : ticksPerCycle_;
            uint64_t latCyc = (uint64_t)((double)lat / tpc + 0.5);
            const uint64_t ep = f.epoch;
            auto a = acc_.find(ep);
            bool account = true;
            if (f.grp) {   /* 1.12.18: a lock-step host request completes with its slowest chip copy */
                auto& g = grpLat_[f.grp];
                if (g.first == 0) g.first = chips_;
                if (latCyc > g.second) g.second = latCyc;
                if (--g.first > 0) account = false; else { latCyc = g.second; grpLat_.erase(f.grp); }
            }
            if (account) {
            if (!f.posted && !f.accp) { const uint32_t sl = priceSlot(f.mi, f.wr, f.pfr);   // (1.12.189): the request's kind (a write with posted stores: noted at acceptance)
              if (a != acc_.end() && sl < a->second.sum.size()) { a->second.sum[sl] += latCyc; a->second.cnt[sl] += 1; } }
            recDone(a, f.core, f.mi, f.wr, f.hold, f.out, f.boot, latCyc, f.chg, curTick_ + 1, f.gated ? f.dmab : 0);   // (1.12.189)
            if (!f.out && !f.posted && !f.accp) compNote(priceSlot(f.mi, f.wr, f.pfr), latCyc);   // (1.12.189)
            if (f.out && f.chg && !f.boot && f.mi < outPriced_.size()) { outPriced_[f.mi]++; outChgLat_[f.mi] += f.chg - std::min(f.chg, f.ref); }   // (1.12.153); (1.12.189): not a bootstrap charge; the charge after its refund
            if (f.out) raw_.olat += latCyc;   // 1.12.18 (R1)
            else if (isHostMi(f.mi)) {
                raw_.hlat += latCyc;
                if (!f.wr) raw_.hrlat += latCyc;   // (1.12.189)
                if (latCyc < hLatMin_) hLatMin_ = latCyc;
                if (latCyc > hLatMax_) hLatMax_ = latCyc;
            } else raw_.lat += latCyc;
            }
            outstanding_--;
            if (!f.out && roiMarked_) lastRoiDone_ = curTick_;   // (1.12.189)
            if (!f.out && f.hold && roiMarked_ && curTick_ + 1 > lastHeldDone_) { lastHeldDone_ = curTick_ + 1; }   // (1.12.189): a core waited for it
            const bool gated = f.gated; const uint32_t fcore = f.core; const bool fdma = f.dmab != 0, fhold = f.hold, fout = f.out;   // (1.12.189)
            const bool fpfg = f.pfg; const uint32_t fpfTag = f.pfTag;
            if (fpfTag && fcore < kPfCores) {   // a prefetch's replayed completion: its consumer's wait settles once the earlier ones have
                const int64_t delta = (int64_t)latCyc - (int64_t)f.chg;
                int64_t corr = 0;
                {
                    PfShard& k = g_pfSh[fcore];
                    std::lock_guard<std::mutex> g(k.m);
                    auto pe = k.ent.find(fpfTag);
                    if (pe != k.ent.end()) {
                        if (pe->second.dropped) k.ent.erase(pe);   // no request will wait for it
                        else {
                            pe->second.delta = f.out ? 0 : delta; pe->second.done = true;
                            if (pe->second.cons) corr = pfDrain(k);
                        }
                    }
                }
                if (corr) pfApply(fcore, corr);
            }
            inflight_.erase(it);
            if (fpfg) {   // the prefetch frees its core's superqueue entry
                auto k = cl_.find(fcore);
                if (k != cl_.end()) { if (k->second.pfOut) k->second.pfOut--; if (!gated) clRelease(k->second); }
            }
            if (gated) {   // (1.12.189): the core can issue its next access
                auto k = cl_.find(fcore);
                if (k != cl_.end()) {
                    if (k->second.out) k->second.out--;
                    if (fhold && !fdma) { k->second.lastDoneT = curTick_ + 1; k->second.lastChgEndS = k->second.pendChgEndS;
                                          if (!fout && roiMarked_) { k->second.roiDoneT = curTick_ + 1; k->second.roiChgEndS = k->second.pendChgEndS; } }
                    clRelease(k->second);
                }
            }
            if (a != acc_.end() && a->second.pend > 0 && --a->second.pend == 0 && a->second.fed) finalizeEpoch(ep);
        };
        if (!fe_->receive_external_requests(typeId, (Ramulator::Addr_t)(r.addr + w.b * step),
                                            (isHostMi(r.mi) ? 1 : 0) + (w.out ? 2 : 0) + (r.shadow ? 3 : 0), cb))   /* 1.12.18: +3 = a lock-step shadow copy */   // 1.12.18 (R1): +2 = outside the ROI; (R0605): source_id 1 = host
            return false;   // the queue is full: the record waits at the door
        if (w.b == 0) {   // the request has entered the controller: in flight from now
            /* (1.12.145, user ruling 2026-10-28): the latency is counted from the tick the record reached the
             * door (its stamp, or later when the replay clock already stood past it). A request that waited
             * at a full queue's door was waiting for this controller; its latency used to start only when it
             * entered the queue, so the wait was charged to no core. */
            inflight_[w.id] = Inflight{burstsPerLine_, std::min<uint64_t>(w.feed, curTick_), r.mi, r.wr != 0, w.out, w.epoch, r.grp, r.chg, r.core, r.hold != 0, r.boot != 0, w.gated, r.ref};
            inflight_[w.id].dmab = r.dmab;   // (1.12.189)
            inflight_[w.id].pfg = w.pfg; inflight_[w.id].pfTag = r.pfTag; inflight_[w.id].pfr = r.pf != 0;
            if (w.gated && r.wr) {   // (1.12.189): a store holds its core until the controller takes it into its write queue (posted)
                Inflight& f = inflight_[w.id];
                const double tpc = (r.mi < miTicksPerCycle_.size()) ? miTicksPerCycle_[r.mi] : ticksPerCycle_;
                const uint64_t lat = (curTick_ + 1 > f.issueTick) ? (curTick_ + 1 - f.issueTick) : 1;
                const uint64_t latCyc = (uint64_t)((double)lat / tpc + 0.5);
                auto a = acc_.find(w.epoch);
                recDone(a, r.core, r.mi, true, true, w.out, r.boot != 0, latCyc, r.chg, curTick_ + 1, r.dmab);
                if (!w.out && roiMarked_ && curTick_ + 1 > lastHeldDone_) { lastHeldDone_ = curTick_ + 1; }   // (1.12.189): its core goes on now
                /* the price of the interface's writes is what a store holds its core for, as for the reconciliation */
                if (!w.out) {
                    const uint32_t sl = priceSlot(r.mi, true);
                    if (a != acc_.end() && sl < a->second.sum.size()) { a->second.sum[sl] += latCyc; a->second.cnt[sl] += 1; }
                    compNote(sl, latCyc);
                }
                f.gated = false; f.hold = false; f.posted = true;   // its completion is neither noted nor releases the core again
                auto k = cl_.find(r.core);
                if (k != cl_.end()) {
                    if (k->second.out) k->second.out--;
                    if (!r.dmab) { k->second.lastDoneT = curTick_ + 1; k->second.lastChgEndS = k->second.pendChgEndS;   // (1.12.189)
                                   if (!w.out && roiMarked_) { k->second.roiDoneT = curTick_ + 1; k->second.roiChgEndS = k->second.pendChgEndS; } }
                    clRelease(k->second);
                }
            }
            if (r.wr && !w.out && !inflight_[w.id].posted) {   // (1.12.189): a write no core waits for, priced as a store is
                Inflight& f = inflight_[w.id];
                const double tpc = (r.mi < miTicksPerCycle_.size()) ? miTicksPerCycle_[r.mi] : ticksPerCycle_;
                const uint64_t lat = (curTick_ + 1 > f.issueTick) ? (curTick_ + 1 - f.issueTick) : 1;
                const uint64_t latCyc = (uint64_t)((double)lat / tpc + 0.5);
                auto a = acc_.find(w.epoch);
                const uint32_t sl = priceSlot(r.mi, true);
                if (a != acc_.end() && sl < a->second.sum.size()) { a->second.sum[sl] += latCyc; a->second.cnt[sl] += 1; }
                compNote(sl, latCyc);
                f.accp = true;
            }
            outstanding_++;
            if (curTick_ > w.target) {
                const uint64_t d = curTick_ - w.target;
                doorRecs_++; doorTicks_ += d; if (d > doorMax_) doorMax_ = d;
            }
            if (r.shadow) { /* 1.12.18: a shadow copy is not a request of its own */ }
            else if (w.out) {   // 1.12.18 (R1)
                if (isHostMi(r.mi)) { raw_.ohreq++; if (r.wr) raw_.ohwr++; else raw_.ohrd++; }
                else { raw_.oreq++; if (r.wr) raw_.owr++; else raw_.ord++; }
                if (r.mi < outRd_.size()) { if (r.wr) outWr_[r.mi]++; else outRd_[r.mi]++; }
            }
            else if (isHostMi(r.mi)) { raw_.hreq++; if (r.wr) raw_.hwr++; else raw_.hrd++; }   // 1.12.18 (R0605)
            else { raw_.req++; if (r.wr) raw_.wr++; else raw_.rd++; }
        }
    }
    return true;
}

void DramEpochReplay::serveDoors() {
    for (auto& kv : doors_) {
        std::deque<Waiting>& q = kv.second;
        while (!q.empty() && q.front().target <= curTick_ && acceptOne(q.front())) { q.pop_front(); waitingN_--; }
    }
}

/* 1.12.18: the mean service latency of interface mi over epoch e's
 * requests completed so far; 0 = e is not in flight or mi has none yet.
 * Called under lock_. */
uint32_t DramEpochReplay::partialMean(uint64_t e, uint32_t mi) {   // (1.12.189): mi = a price slot (priceSlot)
    auto a = acc_.find(e);
    if (a == acc_.end() || !a->second.fed || mi >= a->second.cnt.size()) return 0;
    /* (1.12.145, user ruling 2026-10-28): the finished requests of an epoch in flight are its fast ones; the
     * slow ones are still queued. Each unfinished request now counts at its age so far (a lower bound on
     * its latency): in the controller since it reached the door, or still waiting at the door. */
    PartialCache& pc = pcache_[e];
    if (pc.tick != curTick_) {
        pc.tick = curTick_;
        pc.sum.assign(kPriceKinds * (size_t)numMIs_, 0); pc.cnt.assign(kPriceKinds * (size_t)numMIs_, 0);   // (1.12.189): price slots
        std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> grpAge;   // a lock-step host request counts once, as its oldest copy
        auto add = [&](uint32_t m, uint64_t from, uint32_t grp) {
            if (m >= kPriceKinds * numMIs_) return;   // (1.12.189): m = a price slot
            const uint64_t t = (curTick_ + 1 > from) ? (curTick_ + 1 - from) : 1;
            const double tpc = (m / kPriceKinds < miTicksPerCycle_.size()) ? miTicksPerCycle_[m / kPriceKinds] : ticksPerCycle_;
            const uint64_t c = (uint64_t)((double)t / tpc + 0.5);
            if (grp) { auto& g = grpAge[grp]; if (c > g.first) g.first = c; g.second = m; return; }
            pc.sum[m] += c; pc.cnt[m] += 1;
        };
        for (const auto& kv : inflight_)
            if (kv.second.epoch == e && !kv.second.posted && !kv.second.accp) add(priceSlot(kv.second.mi, kv.second.wr, kv.second.pfr), kv.second.issueTick, kv.second.grp);
        for (const auto& q : doors_) for (const Waiting& w : q.second)
            if (w.epoch == e && !w.r.shadow) add(priceSlot(w.r.mi, w.r.wr != 0, w.r.pf != 0), w.feed, 0);
        for (const Chan& K : chans_) {   // (1.12.144) the per-channel replay keeps its own in-flight requests and doors
            for (const auto& kv : K.inflight) if (kv.second.epoch == e) add(priceSlot(kv.second.mi, kv.second.wr), kv.second.issueTick, kv.second.grp);
            for (const auto& q : K.doors) for (const Waiting& w : q.second)
                if (w.epoch == e && !w.r.shadow) add(priceSlot(w.r.mi, w.r.wr != 0), w.feed, 0);
        }
        for (const auto& kv : grpAge) { pc.sum[kv.second.second] += kv.second.first; pc.cnt[kv.second.second] += 1; }
        while (pcache_.size() > 4) pcache_.erase(pcache_.begin());
    }
    const uint64_t n = a->second.cnt[mi] + (mi < pc.cnt.size() ? pc.cnt[mi] : 0);
    if (n == 0) return 0;
    const uint64_t s = a->second.sum[mi] + (mi < pc.sum.size() ? pc.sum[mi] : 0);
    if (mi < pc.cnt.size() && pc.cnt[mi] > 0) partialAged_++;
    if (!cutMode_) partial_++;
    return std::max<uint32_t>(1, (uint32_t)((s + n / 2) / n));   // (1.12.189): measured, if below a cycle (0 = none measured)
}

/* ---- (1.12.189): the closed-loop replay and the reconciliation (see reconcileAtBarrier in the header) ---- */

bool DramEpochReplay::clOn() const { return cutMode_; }

void DramEpochReplay::noteSync(uint32_t core) {
    if (core < kRecCores && syncP_) syncP_[core].store(1, std::memory_order_relaxed);
}

void DramEpochReplay::dmaBurst(bool on) {
    static std::atomic<uint32_t> seq{0};
    t_recDma = on ? seq.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
}

/* (1.12.189): a DMA transfer whose lines have all completed: the core waited for its last line; the balance takes that
 * line's completion - the latest charged end of the transfer's lines, once. Under lock_. */
void DramEpochReplay::recDmaClose(CoreCL& k) {
    if (k.dmaOpen && k.dmaComp && roiMarked_) {
        const double d = ((double)k.dmaComp - k.dmaChgEnd) / k.dmaTpc;
        k.diff += d; recDma_ += d; recDmaN_++;
    }
    if (k.dmaComp) { k.lastDoneT = k.dmaComp; k.lastChgEndS = k.pendChgEndS; k.roiDoneT = k.dmaComp; k.roiChgEndS = k.pendChgEndS; }   // (1.12.189): the core goes on after its last line (dmaComp: ROI lines only)
    k.dmaOpen = 0; k.dmaComp = 0; k.dmaChgEnd = 0.0;
}

bool DramEpochReplay::lastRecorded() const { return t_lastOk; }

/* the core's queue in the closed-loop replay; nullptr: the record is replayed at its stamp (no core). Called under
 * lock_. */
DramEpochReplay::CoreCL* DramEpochReplay::clCore(uint32_t core) {
    if (core == ~0u || core >= kRecCores || core >= zinfo->numCores || !zinfo->cores[core]) return nullptr;
    auto it = cl_.find(core);
    if (it == cl_.end()) {
        CoreCL k;
        Core* co = zinfo->cores[core];
        k.ooo = co->asOOOCore() != nullptr;
        k.on = true;
        k.cap = std::max<uint32_t>(1, co->memMissCapacity());
        k.id = core;
        it = cl_.emplace(core, std::move(k)).first;
    }
    return it->second.on ? &it->second : nullptr;
}

void DramEpochReplay::clQueue(const Waiting& w, uint64_t tick) {
    rel_.emplace(std::make_pair(tick, relSeq_++), w);
}

/* the core issues what it can: its next records in its order, an access it waits for only while it has fewer than
 * its capacity in flight; each at its stamp plus the core's unapplied shift (its completed accesses' measured -
 * charged, plus the refunds and less the delays its earlier records carry; not below 0), never before now. Under
 * lock_. */
void DramEpochReplay::clRelease(CoreCL& k) {
    while (!k.fifo.empty()) {
        Waiting& h = k.fifo.front();
        /* (1.12.189): a line of the DMA transfer the core has started goes at its own stamp (the engine's pace); anything
         * else the core waits for goes only when every access it has in flight, a whole transfer included, is done */
        const bool inDma = h.r.dmab && h.r.dmab == k.dmaOpen;
        /* a host core with an L2 prefetcher: its L2 misses in flight (the accesses it waits for and its prefetches)
         * hold its superqueue entries; a prefetch, and an access it waits for, goes only while one is free */
        if (!k.sqCap && k.id != ~0u) k.sqCap = L2StreamPrefetcher::superqueueOf(k.id);
        const bool sqFull = k.sqCap && k.out + k.pfOut >= k.sqCap;
        if (h.r.pf && sqFull) break;
        if (h.r.hold && !inDma && !(h.r.sync && !k.ooo) && (k.out >= k.cap || sqFull || (k.dmaOpen && k.out > 0))) break;
        if (h.r.hold && !inDma && k.dmaOpen) recDmaClose(k);
        k.dlySeen += (double)h.r.dly;
        /* the core's timeline: its stamp + its balance not applied yet. An out-of-order core charged more than measured
         * (a negative balance) is timed at its stamps: released behind the clock, its records would bunch there up to
         * its capacity; its refunds bring its own clock back. (1.12.189) A blocking core is timed at its balance either
         * way, and no earlier than its previous access's completion plus its own time between the two: at its stamps,
         * its replayed accesses trailed its timeline by the over-charge and each access it was under-charged for held
         * the next one at the gate. */
        const double bal = k.diff + k.refSeen - k.dlySeen;
        const double off = k.ooo ? std::max(0.0, bal) : bal;
        double t = ((double)h.r.stamp + off) * ticksPerCycle_;
        if (cutMode_) t += (double)roiTick0_;
        if (!k.ooo && h.r.hold && !inDma && !h.r.sync && k.lastDoneT) {
            const double gap = std::max(0.0, (double)h.r.stamp - k.lastChgEndS - (double)h.r.dly);
            t = std::max(t, (double)k.lastDoneT + gap * ticksPerCycle_);
        }
        if (inDma && h.r.hold)   // a later line of the transfer: the engine's pace from the transfer's start
            t = k.dmaT0 + (double)(h.r.stamp > k.dmaS0 ? h.r.stamp - k.dmaS0 : 0) * ticksPerCycle_;
        uint64_t tgt = (t > 0.0) ? (uint64_t)t : 0;
        if (tgt < curTick_) tgt = curTick_;
        clShiftSum_ += (double)tgt - (double)h.target; clShiftN_++;
        h.target = tgt;
        k.refSeen += (double)h.r.refT;
        if (h.r.hold) { k.out++; h.gated = true; }
        if (h.r.pf) { k.pfOut++; h.pfg = true; }
        if (h.r.hold && !k.ooo) {   // (1.12.189): its charged end on its own clock (a DMA transfer: its lines' latest)
            const double ce = (double)h.r.stamp + (double)(h.r.chg - std::min(h.r.chg, h.r.ref));
            k.pendChgEndS = (h.r.dmab && inDma) ? std::max(k.pendChgEndS, ce) : ce;
        }
        if (h.r.hold && h.r.dmab) {   // (1.12.189): a DMA transfer's line; in the ROI, the transfer's charged end on the shifted stamps
            const double tpc = (h.r.mi < miTicksPerCycle_.size()) ? miTicksPerCycle_[h.r.mi] : ticksPerCycle_;
            if (!inDma) { k.dmaOpen = h.r.dmab; k.dmaComp = 0; k.dmaChgEnd = 0.0; k.dmaT0 = (double)tgt; k.dmaS0 = h.r.stamp; }
            k.dmaTpc = tpc;
            if (!h.out && roiMarked_) k.dmaChgEnd = std::max(k.dmaChgEnd, t + (double)(h.r.chg - std::min(h.r.chg, h.r.ref)) * tpc);
        }
        clQueue(h, tgt);
        k.fifo.pop_front(); clHeld_--;
    }
}

/* the released records whose tick has come reach their controller queue's door (as a stamped record does in the
 * open-loop replay). Under lock_. */
void DramEpochReplay::serveRel() {
    while (!rel_.empty() && rel_.begin()->first.first <= curTick_) {
        Waiting w = rel_.begin()->second;
        rel_.erase(rel_.begin());
        w.id = nextId_++; w.feed = curTick_;
        const uint32_t key = w.r.ch * 2u + (w.r.wr ? 1u : 0u);
        std::deque<Waiting>& q = doors_[key];
        if (!q.empty() || !acceptOne(w)) { q.push_back(w); waitingN_++; }
    }
}

/* at the feed, each core's accesses it waits for in the epoch (in the ROI; stamps in order): the sum of their charges
 * and the length of the union of [stamp, stamp + charge] on the core's clock. Under lock_. */
void DramEpochReplay::recFeed(EpochAcc& acc, const std::vector<Rec>& recs) {
    struct Run { uint64_t s = 0, e = 0; bool open = false; };
    std::map<uint32_t, Run> run;
    for (const Rec& r : recs) {
        if (!r.hold || !r.chg || r.core == ~0u || r.shadow) continue;
        uint64_t target = (uint64_t)((double)r.stamp * ticksPerCycle_);
        if (cutMode_) target += roiTick0_;
        if ((closeArmed_ && target > closeTick_) || r.tail) continue;   // outside the ROI
        RecAcc& a = acc.rec[r.core];
        a.chg += (double)r.chg; a.host = isHostMi(r.mi);
        Run& u = run[r.core];
        const uint64_t s = r.stamp, e = r.stamp + r.chg;
        if (!u.open || s > u.e) { if (u.open) a.uni += (double)(u.e - u.s); u.s = s; u.e = e; u.open = true; }
        else if (e > u.e) u.e = e;
    }
    for (const auto& kv : run) if (kv.second.open) acc.rec[kv.first].uni += (double)(kv.second.e - kv.second.s);
}

/* an access completed (measured `meas`, charged `chg`, in its interface's cycles): the error statistics, and for an
 * access a closed-loop core waited for, measured - charged (divided by the core's overlap in the epoch, an
 * out-of-order core) into the core's balance. Under lock_. */
void DramEpochReplay::recDone(std::map<uint64_t, EpochAcc>::iterator a, uint32_t core, uint32_t mi, bool wr, bool hold, bool out,
                              bool boot, uint64_t meas, uint32_t chg, uint64_t doneTick, uint32_t dmab) {
    if (dmab && hold && !out && core != ~0u && clOn() && roiMarked_) {   // (1.12.189): a DMA line: its transfer reconciles once, at its last line (recDmaClose)
        auto kd = cl_.find(core);
        if (kd != cl_.end() && dmab == kd->second.dmaOpen && doneTick > kd->second.dmaComp) kd->second.dmaComp = doneTick;
    }
    if (!hold || out || !chg || core == ~0u) return;
    const bool host = isHostMi(mi);
    const int cls = host ? 1 : (wr ? 2 : 0);
    const int64_t d = (int64_t)meas - (int64_t)chg;
    RecStat& s = recSt_[cls][boot ? 1 : 0];
    const uint64_t ad = (d < 0) ? (uint64_t)(-d) : (uint64_t)d;
    s.n++; s.absSum += ad; s.sum += d; s.chg += chg; s.meas += meas;
    recChgSum_ += chg;   // (1.12.189)
    if (s.hist.empty()) s.hist.assign(kRecHist + 1, 0);
    s.hist[std::min<uint64_t>(ad, kRecHist)]++;
    if (!clOn() || !roiMarked_) return;
    auto k = cl_.find(core);
    if (k == cl_.end() || !k->second.on) return;
    if (dmab) return;   // (1.12.189): a DMA line (above)
    double ov = 1.0;
    if (k->second.ooo && a != acc_.end()) {
        auto ra = a->second.rec.find(core);
        if (ra != a->second.rec.end() && ra->second.uni > 0.0) ov = std::max(1.0, ra->second.chg / ra->second.uni);
    }
    k->second.diff += (double)d / ov;
    k->second.host = host;
    recCredit_[cls] += (double)d / ov;
}

/* a request of interface mi completed now (closed loop): its completion-time bin of the replay's clock. Under lock_. */
void DramEpochReplay::compNote(uint32_t mi, uint64_t latCyc) {   // mi = a price slot
    if (!clOn() || !roiMarked_) return;
    if (compBin_.size() < kPriceKinds * (size_t)numMIs_) compBin_.resize(kPriceKinds * (size_t)numMIs_);
    if (mi >= compBin_.size()) return;
    const double eTicks = (double)epochPhases() * (double)(zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000) * ticksPerCycle_;
    const uint64_t bin = (curTick_ > roiTick0_ && eTicks > 0.0) ? (uint64_t)((double)(curTick_ - roiTick0_) / eTicks) : 0;
    CompBin& b = compBin_[mi];
    if (bin != b.bin) {
        if (b.cnt) b.last = std::max<uint32_t>(1, (uint32_t)((b.sum + b.cnt / 2) / b.cnt));   // (1.12.189): see partialMean
        b.bin = bin; b.sum = 0; b.cnt = 0;
    }
    b.sum += latCyc; b.cnt++;
}

/* the epoch is final: its cores' overlap, for the report. Under lock_. */
void DramEpochReplay::recSettle(EpochAcc& acc) {
    for (const auto& kv : acc.rec) {
        const int h = kv.second.host ? 1 : 0;
        recOvChg_[h] += kv.second.chg; recOvUni_[h] += kv.second.uni;
        auto k = cl_.find(kv.first);
        if (k != cl_.end() && k->second.ooo && kv.second.uni > 0.0) k->second.ovC = std::max(1.0, kv.second.chg / kv.second.uni);
    }
}

void DramEpochReplay::reconcileAtBarrier(const std::function<bool(uint32_t)>& canTake) {
    if (!sys_ || !clOn()) return;
    futex_lock(&lock_);
    if (roiMarked_) {
        recBarriers_++;
        for (auto& kv : cl_) {
            CoreCL& k = kv.second;
            const uint32_t c = kv.first;
            if (!k.on || c >= kRecCores) continue;
            if (k.dmaOpen && k.out == 0) recDmaClose(k);   // (1.12.189): a transfer done before the barrier
            double owed = k.diff + (double)used_[c].load(std::memory_order_relaxed) / 1024.0 - (double)k.dlyTot;
            if (owed >= 1.0 && c < zinfo->numCores && zinfo->cores[c] && canTake(c)) {
                const uint32_t d = (uint32_t)std::min(owed, 4294967295.0);   // whole cycles; the fraction stays
                zinfo->cores[c]->addDelay(d);
                k.dlyTot += d; k.pendDly += d;
                recApplied_ += d; recDelays_++; recAppliedCls_[k.host ? 1 : 0] += d;
                owed -= (double)d;
            }
            /* an over-charge (a negative balance) is refunded from the core's next charges */
            ovA_[c].store(k.ovC, std::memory_order_relaxed);
            pool_[c].store(owed < 0.0 ? (int64_t)(-owed * k.ovC) : 0, std::memory_order_relaxed);
        }
    }
    futex_unlock(&lock_);
}

int64_t DramEpochReplay::reconcileCore(uint32_t core, uint64_t maxRewind) {
    if (!sys_ || !clOn() || core >= kRecCores || core >= zinfo->numCores || !zinfo->cores[core]) return 0;
    int64_t applied = 0;
    futex_lock(&lock_);
    auto it = cl_.find(core);
    if (roiMarked_ && !runEndMarked_ && it != cl_.end() && it->second.on) {
        CoreCL& k = it->second;
        Core* co = zinfo->cores[core];
        if (k.dmaOpen && k.out == 0) recDmaClose(k);   // (1.12.189)
        double owed = k.diff + (double)used_[core].load(std::memory_order_relaxed) / 1024.0 - (double)k.dlyTot;
        recComms_++;
        if (owed >= 1.0) {
            const uint32_t d = (uint32_t)std::min(owed, 4294967295.0);
            co->addDelay(d);
            k.dlyTot += d; k.pendDly += d; recApplied_ += d; recDelays_++; recAppliedCls_[k.host ? 1 : 0] += d; recCommDelay_ += d;
            applied = d; owed -= (double)d;
        } else if (owed <= -1.0 && maxRewind > 0) {
            /* the core's clock moves back by the over-charge (Core::pimidRewindCycles: the ALU element's clock, an
             * in-order / out-of-order core's wall-free clock), as far as it went */
            const uint64_t want = (uint64_t)std::min<double>(-owed, (double)maxRewind);
            const uint64_t c0 = co->getCycles();
            co->pimidRewindCycles(want);
            const uint64_t c1 = co->getCycles();
            const int64_t r = (c0 > c1) ? (int64_t)(c0 - c1) : 0;
            k.dlyTot -= r; k.pendDly -= r; recCommRewind_ += (uint64_t)r;
            applied = -r; owed += (double)r;
        }
        pool_[core].store(owed < 0.0 ? (int64_t)(-owed * k.ovC) : 0, std::memory_order_relaxed);
    }
    futex_unlock(&lock_);
    return applied;
}

void DramEpochReplay::markRunEnd() {
    if (!sys_ || !clOn()) return;
    futex_lock(&lock_);
    if (!runEndMarked_) {
        runEndMarked_ = true;
        for (auto& kv : cl_) {
            kv.second.dlyAtEnd = kv.second.dlyTot;
            kv.second.usedAtEnd = (kv.first < kRecCores) ? used_[kv.first].load(std::memory_order_relaxed) : 0;
            kv.second.pendAtEnd = kv.second.pendDly;   // (1.12.189)
        }
    }
    futex_unlock(&lock_);
}

void DramEpochReplay::settleFinal() {
    if (!sys_ || !clOn() || !roiMarked_ || settled_) return;
    futex_lock(&lock_);
    keepHist_ = true;
    armClose(true);
    while (!pending_.empty()) { uint64_t ep = pending_.begin()->first; replayEpoch(ep); pending_.erase(ep); }
    drainAll();
    for (auto& kv : cl_) if (kv.second.dmaOpen && kv.second.out == 0) recDmaClose(kv.second);   // (1.12.189)
    keepHist_ = false;
    settled_ = true;
    settleRun_ = zinfo->roiWall.cycles();
    futex_unlock(&lock_);
}

bool DramEpochReplay::heldEnd(uint32_t core, uint64_t end, double& out) {
    if (!sys_ || !clOn()) return false;
    futex_lock(&lock_);
    bool ok = false;
    auto it = cl_.find(core);
    if (it != cl_.end() && it->second.on && it->second.roiDoneT && roiMarked_) {
        const CoreCL& k = it->second;
        const double tpr = runTicksPerCycle(), t0 = cutMode_ ? (double)roiTick0_ : 0.0;
        const bool waited = core < kRecCores && syncP_ && syncP_[core].load(std::memory_order_relaxed);
        const double pend = runEndMarked_ ? (double)k.pendAtEnd : (double)k.pendDly;
        out = ((double)k.roiDoneT - t0) / tpr + ((waited || k.ooo) ? 0.0 : std::max(0.0, (double)end - k.roiChgEndS - pend));
        ok = true;
    }
    futex_unlock(&lock_);
    return ok;
}

double DramEpochReplay::owedEnd(uint32_t core) {
    if (!sys_ || !clOn()) return 0.0;
    futex_lock(&lock_);
    double o = 0.0;
    auto it = cl_.find(core);
    if (it != cl_.end() && it->second.on && core < kRecCores) {
        const CoreCL& k = it->second;
        const int64_t used = runEndMarked_ ? k.usedAtEnd : used_[core].load(std::memory_order_relaxed);
        const int64_t dly = runEndMarked_ ? k.dlyAtEnd : k.dlyTot;
        o = k.diff + (double)used / 1024.0 - (double)dly;
    }
    futex_unlock(&lock_);
    return o > 0.0 ? o : 0.0;
}

uint32_t DramEpochReplay::chargeLast(uint32_t lat, bool boot, uint32_t floor) {
    if (!t_lastOk) return lat;
    t_lastOk = false;
    if (!boot) return (t_lastRef < lat) ? lat - t_lastRef : 0;   // the refund was taken at the record
    futex_lock(&lock_);
    auto it = pending_.find(t_lastEp);
    if (it != pending_.end() && t_lastIdx < it->second.size() && it->second[t_lastIdx].core == t_lastCore &&
        it->second[t_lastIdx].stamp == t_lastStamp) {
        Rec& r = it->second[t_lastIdx];
        r.chg = lat; r.boot = 1;
        if (clOn() && roiMarked_ && r.hold && r.core < kRecCores) {
            const int64_t av = pool_[r.core].load(std::memory_order_relaxed);
            const uint32_t take = (uint32_t)std::min<int64_t>(av, (lat > floor) ? (int64_t)(lat - floor) : 0);
            if (take) {
                const double tu = (double)take / ovA_[r.core].load(std::memory_order_relaxed);
                pool_[r.core].fetch_sub(take, std::memory_order_relaxed); used_[r.core].fetch_add((int64_t)std::llround(tu * 1024.0), std::memory_order_relaxed);
                r.ref = take; r.refT = (float)tu; recRefund_ += take; lat -= take;
            }
        }
    }
    futex_unlock(&lock_);
    return lat;
}

/* the dump's reconciliation counters and report. Under lock_. */
void DramEpochReplay::recReport() {
    double pend = 0.0, neg = 0.0, pmax = 0.0; uint32_t pn = 0, nc = 0; int pcore = -1;
    for (const auto& kv : cl_) {
        if (!kv.second.on || kv.first >= kRecCores) continue;
        nc++;
        const double owed = kv.second.diff + (double)used_[kv.first].load(std::memory_order_relaxed) / 1024.0 - (double)kv.second.dlyTot;
        if (owed >= 1.0) { pend += owed; pn++; if (owed > pmax) { pmax = owed; pcore = (int)kv.first; } }
        else if (owed < 0.0) neg += owed;
    }
    uint64_t n = 0;
    for (const auto& ar : recSt_) for (const RecStat& s : ar) n += s.n;
    profRecReads_.set(n); profRecCycles_.set(recApplied_); profRecRefund_.set(recRefund_); profRecPending_.set((uint64_t)pend);
    profRecPendingMax_.set((uint64_t)pmax); profRecCharged_.set(recChgSum_);
    profRoiTail_.set((closeArmed_ && lastHeldDone_ > closeTick_) ? lastHeldDone_ - closeTick_ : 0);
    profRoiDrain_.set((closeArmed_ && lastRoiDone_ > closeTick_) ? lastRoiDone_ - closeTick_ : 0);
    static const char* cn[3] = {"device", "host", "devstore"};
    for (int k = 0; k < 3; k++) for (int b = 0; b < 2; b++) {
        const RecStat& s = recSt_[k][b];
        if (s.n == 0) continue;
        uint64_t p95 = 0, cum = 0;
        const uint64_t lim = (s.n * 95 + 99) / 100;
        for (uint64_t i = 0; i < s.hist.size(); i++) { cum += s.hist[i]; if (cum >= lim) { p95 = i; break; } }
        info("[rec] %s %s%s: n=%lu charged=%.2f measured=%.2f absErr=%.2f p95=%lu%s bias=%+.2f",
             name_.c_str(), cn[k], b ? "/boot" : "", (unsigned long)s.n,
             (double)s.chg / (double)s.n, (double)s.meas / (double)s.n, (double)s.absSum / (double)s.n,
             (unsigned long)p95, p95 >= kRecHist ? "+" : "", (double)s.sum / (double)s.n);
    }
    if (n == 0) return;
    info("[rec] %s: closed loop %s, %u core(s); release shift mean %.1f cycles over %lu records; credit device %.0f host %.0f devstore %.0f; overlap device %.3f host %.3f",
         name_.c_str(), clOn() ? "on" : "off", nc, clShiftN_ ? clShiftSum_ / (double)clShiftN_ / ticksPerCycle_ : 0.0, (unsigned long)clShiftN_,
         recCredit_[0], recCredit_[1], recCredit_[2], recOvUni_[0] > 0.0 ? recOvChg_[0] / recOvUni_[0] : 1.0, recOvUni_[1] > 0.0 ? recOvChg_[1] / recOvUni_[1] : 1.0);
    if (recDmaN_) info("[rec] %s: DMA transfers reconciled %lu, their balance %.0f cycles", name_.c_str(), (unsigned long)recDmaN_, recDma_);
    info("[rec] %s: applied=%lu cycles in %lu delays over %lu barriers; refunded=%lu; pending=%.0f on %u core(s) (max %.0f, core %d); negative=%.0f; applied device %lu host %lu",
         name_.c_str(), (unsigned long)recApplied_, (unsigned long)recDelays_, (unsigned long)recBarriers_, (unsigned long)recRefund_, pend, pn, pmax, pcore, neg,
         (unsigned long)recAppliedCls_[0], (unsigned long)recAppliedCls_[1]);
    {   // the host cores' prefetched lines their requests found (their shards hold only this replay's records)
        uint64_t pn = 0, pl = 0; int64_t pc = 0;
        for (uint32_t c = 0; c < kPfCores; c++) {
            PfShard& k = g_pfSh[c];
            std::lock_guard<std::mutex> g(k.m);
            if (k.inst != this) continue;
            pn += k.n; pl += k.late; pc += k.corr;
        }
        if (pn)
            info("[rec] %s: requests that found a line an L2 prefetcher fetched: %lu settled against its replayed arrival (%lu waited "
                 "longer than charged), correction %+ld cycles", name_.c_str(), (unsigned long)pn, (unsigned long)pl, (long)pc);
    }
    if (recComms_)
        info("[rec] %s: at message-passing entry (%lu): delayed %lu cycles, rewound %lu cycles", name_.c_str(), (unsigned long)recComms_,
             (unsigned long)recCommDelay_, (unsigned long)recCommRewind_);
}

void DramEpochReplay::finalizeEpoch(uint64_t epoch) {
    auto a = acc_.find(epoch);
    if (a == acc_.end()) return;
    recSettle(a->second);   // (1.12.189)
    std::vector<uint32_t> perMi(kPriceKinds * (size_t)numMIs_, 0);   // (1.12.189): per price slot
    for (uint32_t m = 0; m < kPriceKinds * numMIs_ && m < a->second.cnt.size(); ++m)
        if (a->second.cnt[m] > 0) perMi[m] = std::max<uint32_t>(1, (uint32_t)((a->second.sum[m] + a->second.cnt[m] / 2) / a->second.cnt[m]));   // (1.12.189): see partialMean
    if (cutMode_ && priceWindow() > 1) {   // (1.12.153): the price window's finalized epochs
        winSum_[epoch] = a->second.sum; winCnt_[epoch] = a->second.cnt;
        while (winSum_.size() > 4 * (size_t)priceWindow() + 8) { winSum_.erase(winSum_.begin()); winCnt_.erase(winCnt_.begin()); }
    }
    acc_.erase(a);
    frozen_[epoch] = perMi;
    if (cutMode_) {   // 1.12.17: an interface idle in this epoch keeps its last measured value
        std::vector<uint32_t> c = perMi;
        for (uint32_t m = 0; m < kPriceKinds * numMIs_ && m < lastValue_.size(); ++m) if (c[m] == 0) c[m] = lastValue_[m];
        carried_[epoch] = c;
    }
    for (uint32_t m = 0; m < kPriceKinds * numMIs_ && m < lastValue_.size(); ++m) if (perMi[m] > 0) lastValue_[m] = perMi[m];
}

/* everything fed is served (pre-ROI before the window opens; the tail before
 * the counters are read). Bounded so a lost request cannot hang the run. */
void DramEpochReplay::drainAll() {
    if (par()) { drainAllPar(); return; }   // (1.12.144)
    uint64_t guard = 0;
    while ((outstanding_ > 0 || waitingN_ > 0 || !rel_.empty() || clHeld_ > 0) && guard < 50000000ull) { tickOnce(); guard++; }   // (1.12.189): + released / held
    if (outstanding_ > 0 || waitingN_ > 0 || !rel_.empty() || clHeld_ > 0) {
        raw_.unserved += outstanding_ + waitingN_ + rel_.size() + clHeld_;
        outstanding_ = 0; waitingN_ = 0; doors_.clear(); inflight_.clear();
        rel_.clear(); clHeld_ = 0; for (auto& kv : cl_) { kv.second.fifo.clear(); kv.second.out = 0; }
    }
    while (!acc_.empty()) finalizeEpoch(acc_.begin()->first);
}

void DramEpochReplay::replayEpoch(uint64_t epoch) {
    if (par()) { replayEpochPar(epoch); return; }   // (1.12.144)
    armClose(false);   // 1.12.18: cut mode, once the run time's ROI end is known
    std::vector<Rec>& recs = pending_[epoch];
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        if (a.stamp != b.stamp) return a.stamp < b.stamp;
        if (a.mi != b.mi) return a.mi < b.mi;
        if (a.addr != b.addr) return a.addr < b.addr;
        return a.wr < b.wr;
    });
    EpochAcc& acc = acc_[epoch];
    if (acc.sum.empty()) { acc.sum.assign(kPriceKinds * (size_t)numMIs_, 0); acc.cnt.assign(kPriceKinds * (size_t)numMIs_, 0); }   // (1.12.189): price slots
    acc.pend += recs.size(); acc.fed = false;
    recFeed(acc, recs);   // (1.12.189)
    if (clOn() && roiMarked_) {
        /* (1.12.189): the closed-loop replay (see reconcileAtBarrier). Each record goes to its core's queue and reaches
         * the controller when the core can issue it (clRelease), or, without a core, at its stamp; the clock runs
         * to the epoch's last stamp, as below. */
        uint64_t endT = curTick_;
        for (size_t i = 0; i < recs.size(); ++i) {
            const Rec& r = recs[i];
            uint64_t target = (uint64_t)((double)r.stamp * ticksPerCycle_);
            if (cutMode_) target += roiTick0_;
            const bool out = (closeArmed_ && target > closeTick_) || r.tail;   // as below
            if (out) pastEndRecs_++;
            if (target > endT) endT = target;
            Waiting w{r, epoch, target, out, 0, 0, 0};
            CoreCL* k = clCore(r.core);
            if (k) { k->fifo.push_back(w); clHeld_++; clRelease(*k); }
            else clQueue(w, target);
        }
        acc.fed = true;
        serveRel();
        tickTo(endT);
        auto ai = acc_.find(epoch);
        if (ai != acc_.end() && ai->second.fed && ai->second.pend == 0) finalizeEpoch(epoch);
        if (cutMode_) { lastFolded_ = epoch; anyFolded_ = true; }
        lastReplayed_ = epoch; anyReplayed_ = true;
        raw_.epochs++;
        return;
    }
    for (size_t i = 0; i < recs.size(); ++i) {
        const Rec& r = recs[i];
        uint64_t target = (uint64_t)((double)r.stamp * ticksPerCycle_);
        if (cutMode_) target += roiTick0_;   // 1.12.17: ROI-relative stamps start at the ROI's DRAM tick
        /* 1.12.18 (user ruling R1 2026-10-24): a request stamped after the
         * ROI end is replayed (the controller's state stays correct) but is
         * outside: source_id 2 (device) / 3 (host) in the controller, and in
         * none of the ROI counts, latencies or row/ACT totals. Judged on its
         * own stamp: a wait at the door never pushes it outside. (1.12.181): a
         * request a co-sim MPI rank issued after its window-close marker is its
         * post-ROI tail and outside whatever its stamp (a rank whose ROI origin
         * is later than another's stamps the start of its tail before the end). */
        const bool out = (closeArmed_ && target > closeTick_) || r.tail;
        if (out) pastEndRecs_++;
        tickTo(target);   // monotone: a stamp behind the clock arrives now
        Waiting w{r, epoch, target, out, 0, nextId_++, curTick_};   // (1.12.145): feed = now
        const uint32_t key = r.ch * 2u + (r.wr ? 1u : 0u);
        std::deque<Waiting>& q = doors_[key];
        if (!q.empty() || !acceptOne(w)) { q.push_back(w); waitingN_++; }   // FIFO behind its queue's door
    }
    acc.fed = true;
    if (acc.pend == 0) finalizeEpoch(epoch);
    if (cutMode_) { lastFolded_ = epoch; anyFolded_ = true; }
    lastReplayed_ = epoch; anyReplayed_ = true;
    raw_.epochs++;
}


/* ---- (1.12.144): the per-channel replay (see Chan in the header) ---- */

struct DramEpochReplay::ParHelper {
    std::mutex mu;
    std::condition_variable cv;
    const std::function<void(Chan&)>* job = nullptr;
    Chan* chan = nullptr;
    uint64_t gen = 0, done = 0;
};

void DramEpochReplay::parRun(const std::function<void(Chan&)>& f) {
    if (!helper_) {
        helper_ = new ParHelper();
        ParHelper* h = helper_;
        std::thread th([h]() {
            uint64_t seen = 0;
            while (true) {
                const std::function<void(Chan&)>* j; Chan* c;
                {
                    std::unique_lock<std::mutex> lk(h->mu);
                    h->cv.wait(lk, [&]() { return h->gen != seen; });
                    seen = h->gen; j = h->job; c = h->chan;
                }
                (*j)(*c);
                { std::lock_guard<std::mutex> lk(h->mu); h->done = seen; }
                h->cv.notify_all();
            }
        });
        th.detach();
    }
    ParHelper* h = helper_;
    uint64_t g;
    { std::lock_guard<std::mutex> lk(h->mu); h->job = &f; h->chan = &chans_[1]; g = ++h->gen; }
    h->cv.notify_all();
    f(chans_[0]);
    std::unique_lock<std::mutex> lk(h->mu);
    h->cv.wait(lk, [&]() { return h->done == g; });
}

void DramEpochReplay::chanTotals(uint64_t& h, uint64_t& m, uint64_t& c, uint64_t& r) const {
    h = m = c = r = 0;
    for (const Chan& K : chans_) { uint64_t a = 0, b = 0, x = 0, y = 0; K.sys->pimid_ctrl_totals(a, b, x, y); h += a; m += b; c += x; r += y; }
}

void DramEpochReplay::chanTakeSnap(Chan& K, ChanSnap& s) const {
    s.tick = K.curTick;
    uint64_t a = 0, b = 0, x = 0, y = 0;
    K.sys->pimid_ctrl_totals(a, b, x, y); s.refreshes = y;
    uint64_t w = 0, o = 0;
    if (bankTracked_ && K.sys->pimid_bank_open_totals(w, o)) { s.bankW = w; s.bankO = o; }
    if (pdOn_) { uint64_t u = 0; K.sys->pimid_pd_totals(s.pdC, u, s.pdX); }
}

/* acceptOne on one channel: its own door, in-flight table and counters; the completion is logged, not applied */
bool DramEpochReplay::chanAccept(Chan& K, Waiting& w) {
    const Rec& r = w.r;
    const uint64_t lineBytes = 1ull << lineBits_;
    const uint64_t step = lineBytes / burstsPerLine_;
    const int typeId = r.wr ? Ramulator::Request::Type::Write : Ramulator::Request::Type::Read;
    for (; w.b < burstsPerLine_; ++w.b) {
        const uint64_t id = w.id;
        Chan* Kp = &K;
        auto cb = [this, Kp, id](Ramulator::Request&) {
            Chan& Q = *Kp;
            auto it = Q.inflight.find(id);
            if (it == Q.inflight.end()) return;
            Inflight& f = it->second;
            if (f.remaining > 1) { f.remaining--; return; }
            const uint64_t lat = (Q.curTick + 1 > f.issueTick) ? (Q.curTick + 1 - f.issueTick) : 1;
            const double tpc = (f.mi < miTicksPerCycle_.size()) ? miTicksPerCycle_[f.mi] : ticksPerCycle_;
            uint64_t latCyc = (uint64_t)((double)lat / tpc + 0.5);
            bool account = true;
            if (f.grp) {
                auto& g = Q.grpLat[f.grp];
                if (g.first == 0) g.first = chips_;
                if (latCyc > g.second) g.second = latCyc;
                if (--g.first > 0) account = false; else { latCyc = g.second; Q.grpLat.erase(f.grp); }
            }
            if (Q.evTick != Q.curTick) { Q.evTick = Q.curTick; Q.seq = 0; }
            Q.evs.push_back(ChanEv{Q.curTick, Q.seq++, f.epoch, f.mi, latCyc, account, f.out, isHostMi(f.mi), f.chg, f.wr, f.hold, f.boot, f.core, f.ref});
            Q.outstanding--;
            Q.inflight.erase(it);
        };
        if (!K.fe->receive_external_requests(typeId, (Ramulator::Addr_t)(r.addr - K.addrOffset + w.b * step),
                                             (isHostMi(r.mi) ? 1 : 0) + (w.out ? 2 : 0) + (r.shadow ? 3 : 0), cb))
            return false;
        if (w.b == 0) {
            K.inflight[w.id] = Inflight{burstsPerLine_, std::min<uint64_t>(w.feed, K.curTick), r.mi, r.wr != 0, w.out, w.epoch, r.grp, r.chg, r.core, r.hold != 0, r.boot != 0, false, r.ref};   // (1.12.145): from the door
            K.outstanding++;
            if (K.curTick > w.target) {
                const uint64_t dd = K.curTick - w.target;
                K.doorRecs++; K.doorTicks += dd; if (dd > K.doorMax) K.doorMax = dd;
            }
            if (r.shadow) { }
            else if (w.out) {
                if (isHostMi(r.mi)) { K.d.ohreq++; if (r.wr) K.d.ohwr++; else K.d.ohrd++; }
                else { K.d.oreq++; if (r.wr) K.d.owr++; else K.d.ord++; }
                if (r.mi < outRd_.size()) { if (r.wr) K.outWr[r.mi]++; else K.outRd[r.mi]++; }
            }
            else if (isHostMi(r.mi)) { K.d.hreq++; if (r.wr) K.d.hwr++; else K.d.hrd++; }
            else { K.d.req++; if (r.wr) K.d.wr++; else K.d.rd++; }
        }
    }
    return true;
}

void DramEpochReplay::chanServeDoors(Chan& K) {
    for (auto& kv : K.doors) {
        std::deque<Waiting>& q = kv.second;
        while (!q.empty() && q.front().target <= K.curTick && chanAccept(K, q.front())) { q.pop_front(); K.waitingN--; }
    }
}

/* tickOnce on one channel: the controller tick, its idle state, its door, the snapshots */
void DramEpochReplay::chanTickOnce(Chan& K) {
    K.sys->tick();
    K.curTick++;
    const bool idle = (K.outstanding == 0 && K.waitingN == 0);
    if (idle != K.idleNow) { K.idleFlips.push_back(K.curTick); K.idleNow = idle; }
    if (K.waitingN > 0) chanServeDoors(K);
    if (closeArmed_ && !K.closeSnappedIn && K.curTick >= closeTick_) { chanTakeSnap(K, K.closeSnap); K.closeTaken = true; K.closeSnappedIn = true; }
    else if (roiMarked_ && !closeArmed_ && (K.curTick % kSnapEvery) == 0) { ChanSnap s; chanTakeSnap(K, s); K.snaps.push_back(s); }
}

void DramEpochReplay::chanTickTo(Chan& K, uint64_t tick) {
    while (K.curTick < tick) chanTickOnce(K);
}

void DramEpochReplay::chanBegin() {
    for (Chan& K : chans_) {
        K.evs.clear(); K.seq = 0; K.evTick = ~0ull;
        K.idleFlips.clear(); K.idleAtStart = K.idleNow = (K.outstanding == 0 && K.waitingN == 0);
        K.snaps.clear(); K.closeTaken = false; K.closeSnappedIn = closeSnapped_;
        K.d = Chan::Cnt(); K.doorRecs = K.doorTicks = K.doorMax = 0; K.drained = 0;
        K.outRd.assign(outRd_.size(), 0); K.outWr.assign(outWr_.size(), 0);
        if (K.curTick != curTick_) panic("DramEpochReplay %s: channel clock %lu != %lu", name_.c_str(), (unsigned long)K.curTick, (unsigned long)curTick_);
    }
}

/* the shared state after a parallel call over (startTick, endTick], in the single instance's order */
void DramEpochReplay::chanMerge(uint64_t startTick, uint64_t endTick) {
    /* idle ticks in (startTick, upTo]: both channels idle after the tick (a flip at t holds from t on) */
    auto idleCount = [&](uint64_t upTo) -> uint64_t {
        uint64_t n = 0;
        size_t i0 = 0, i1 = 0;
        bool s0 = chans_[0].idleAtStart, s1 = chans_[1].idleAtStart;
        const auto& F0 = chans_[0].idleFlips; const auto& F1 = chans_[1].idleFlips;
        uint64_t t = startTick;   // ticks <= t are counted
        while (t < upTo) {
            uint64_t nx = upTo + 1;
            if (i0 < F0.size() && F0[i0] < nx) nx = F0[i0];
            if (i1 < F1.size() && F1[i1] < nx) nx = F1[i1];
            const uint64_t last = std::min(nx - 1, upTo);   // ticks t+1 .. last keep the states
            if (last > t && s0 && s1) n += last - t;
            if (nx > upTo) break;
            while (i0 < F0.size() && F0[i0] == nx) { s0 = !s0; i0++; }
            while (i1 < F1.size() && F1[i1] == nx) { s1 = !s1; i1++; }
            if (s0 && s1) n++;   // tick nx
            t = nx;
        }
        return n;
    };
    const uint64_t idle0 = raw_.idle, ticks0 = raw_.ticks;
    const auto& A = chans_[0].snaps; const auto& B = chans_[1].snaps;
    if (A.size() != B.size()) panic("DramEpochReplay %s: channel snapshots differ (%lu vs %lu)", name_.c_str(), (unsigned long)A.size(), (unsigned long)B.size());
    for (size_t i = 0; i < A.size(); i++) {
        Snap s; s.tick = A[i].tick; s.ticks = ticks0 + (A[i].tick - startTick); s.idle = idle0 + idleCount(A[i].tick);
        s.refreshes = A[i].refreshes + B[i].refreshes;
        s.bankW = raw_.bankW; s.bankO = raw_.bankO;
        if (bankTracked_) { s.bankW = A[i].bankW + B[i].bankW; s.bankO = A[i].bankO + B[i].bankO; }
        s.pdC = A[i].pdC + B[i].pdC; s.pdX = A[i].pdX + B[i].pdX;
        hist_.push_back(s);
    }
    if (chans_[0].closeTaken != chans_[1].closeTaken) panic("DramEpochReplay %s: channel close snapshots differ", name_.c_str());
    if (chans_[0].closeTaken) {
        const ChanSnap& a = chans_[0].closeSnap; const ChanSnap& b = chans_[1].closeSnap;
        Snap s; s.tick = a.tick; s.ticks = ticks0 + (a.tick - startTick); s.idle = idle0 + idleCount(a.tick);
        s.refreshes = a.refreshes + b.refreshes;
        s.bankW = raw_.bankW; s.bankO = raw_.bankO;
        if (bankTracked_) { s.bankW = a.bankW + b.bankW; s.bankO = a.bankO + b.bankO; }
        s.pdC = a.pdC + b.pdC; s.pdX = a.pdX + b.pdX;
        applySnap(s);
    }
    raw_.ticks += endTick - startTick;
    raw_.idle += idleCount(endTick);
    curTick_ = endTick;
    for (Chan& K : chans_) {
        raw_.req += K.d.req; raw_.rd += K.d.rd; raw_.wr += K.d.wr;
        raw_.hreq += K.d.hreq; raw_.hrd += K.d.hrd; raw_.hwr += K.d.hwr;
        raw_.oreq += K.d.oreq; raw_.ord += K.d.ord; raw_.owr += K.d.owr;
        raw_.ohreq += K.d.ohreq; raw_.ohrd += K.d.ohrd; raw_.ohwr += K.d.ohwr;
        for (size_t m = 0; m < outRd_.size() && m < K.outRd.size(); m++) { outRd_[m] += K.outRd[m]; outWr_[m] += K.outWr[m]; }
        doorRecs_ += K.doorRecs; doorTicks_ += K.doorTicks; if (K.doorMax > doorMax_) doorMax_ = K.doorMax;
    }
    outstanding_ = chans_[0].outstanding + chans_[1].outstanding;
    waitingN_ = chans_[0].waitingN + chans_[1].waitingN;
    // completions: tick, then channel 0 before channel 1, then the channel's own order
    const auto& E0 = chans_[0].evs; const auto& E1 = chans_[1].evs;
    size_t i = 0, j = 0;
    while (i < E0.size() || j < E1.size()) {
        const bool take0 = (j >= E1.size()) || (i < E0.size() && E0[i].tick <= E1[j].tick);
        const ChanEv& e = take0 ? E0[i++] : E1[j++];
        auto a = acc_.find(e.epoch);
        if (!e.out && roiMarked_) lastRoiDone_ = e.tick;   // (1.12.189)
        if (!e.out && e.hold && roiMarked_ && e.tick > lastHeldDone_) { lastHeldDone_ = e.tick; }   // (1.12.189)
        if (e.account) {
            { const uint32_t sl = priceSlot(e.mi, e.wr);   // (1.12.189): the request's kind
              if (a != acc_.end() && sl < a->second.sum.size()) { a->second.sum[sl] += e.latCyc; a->second.cnt[sl] += 1; } }
            recDone(a, e.core, e.mi, e.wr, e.hold, e.out, e.boot, e.latCyc, e.chg);   // (1.12.189): statistics (open loop)
            if (e.out && e.chg && !e.boot && e.mi < outPriced_.size()) { outPriced_[e.mi]++; outChgLat_[e.mi] += e.chg - std::min(e.chg, e.ref); }   // (1.12.153); (1.12.189): not a bootstrap charge; the charge after its refund
            if (e.out) raw_.olat += e.latCyc;
            else if (e.host) {
                raw_.hlat += e.latCyc;
                if (!e.wr) raw_.hrlat += e.latCyc;   // (1.12.189)
                if (e.latCyc < hLatMin_) hLatMin_ = e.latCyc;
                if (e.latCyc > hLatMax_) hLatMax_ = e.latCyc;
            } else raw_.lat += e.latCyc;
        }
        if (a != acc_.end() && a->second.pend > 0 && --a->second.pend == 0 && a->second.fed) finalizeEpoch(e.epoch);
    }
}

void DramEpochReplay::tickToPar(uint64_t tick) {
    if (curTick_ >= tick) return;
    const uint64_t t0 = curTick_;
    chanBegin();
    std::function<void(Chan&)> f = [this, tick](Chan& K) { chanTickTo(K, tick); };
    parRun(f);
    chanMerge(t0, tick);
}

void DramEpochReplay::drainAllPar() {
    const uint64_t t0 = curTick_;
    chanBegin();
    std::function<void(Chan&)> f1 = [this, t0](Chan& K) {
        uint64_t guard = 0;
        while ((K.outstanding > 0 || K.waitingN > 0) && guard < 50000000ull) { chanTickOnce(K); guard++; }
        K.drained = K.curTick - t0;
    };
    parRun(f1);
    const uint64_t end = t0 + std::max(chans_[0].drained, chans_[1].drained);
    std::function<void(Chan&)> f2 = [this, end](Chan& K) { chanTickTo(K, end); };
    parRun(f2);
    chanMerge(t0, end);
    if (outstanding_ > 0 || waitingN_ > 0) {
        raw_.unserved += outstanding_ + waitingN_;
        outstanding_ = 0; waitingN_ = 0;
        for (Chan& K : chans_) { K.outstanding = 0; K.waitingN = 0; K.doors.clear(); K.inflight.clear(); }
    }
    while (!acc_.empty()) finalizeEpoch(acc_.begin()->first);
}

void DramEpochReplay::replayEpochPar(uint64_t epoch) {
    armClose(false);
    std::vector<Rec>& recs = pending_[epoch];
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        if (a.stamp != b.stamp) return a.stamp < b.stamp;
        if (a.mi != b.mi) return a.mi < b.mi;
        if (a.addr != b.addr) return a.addr < b.addr;
        return a.wr < b.wr;
    });
    EpochAcc& acc = acc_[epoch];
    if (acc.sum.empty()) { acc.sum.assign(kPriceKinds * (size_t)numMIs_, 0); acc.cnt.assign(kPriceKinds * (size_t)numMIs_, 0); }   // (1.12.189): price slots
    acc.pend += recs.size(); acc.fed = false;
    recFeed(acc, recs);   // (1.12.189)
    std::vector<ChanWork> work[2];
    uint64_t end = curTick_;
    for (size_t i = 0; i < recs.size(); ++i) {
        const Rec& r = recs[i];
        uint64_t target = (uint64_t)((double)r.stamp * ticksPerCycle_);
        if (cutMode_) target += roiTick0_;
        const bool out = (closeArmed_ && target > closeTick_) || r.tail;   // (1.12.181): or the tail (see replayEpoch)
        if (out) pastEndRecs_++;
        if (target > end) end = target;
        if (r.ch > 1) panic("DramEpochReplay %s: record on channel %u of a two-channel replay", name_.c_str(), r.ch);
        work[r.ch].push_back(ChanWork{target, Waiting{r, epoch, target, out, 0, nextId_++}});
    }
    const uint64_t t0 = curTick_;
    chanBegin();
    std::function<void(Chan&)> f = [this, &work, end](Chan& K) {
        const int c = (&K == &chans_[0]) ? 0 : 1;
        for (ChanWork& cw : work[c]) {
            chanTickTo(K, cw.target);
            cw.w.feed = K.curTick;   // (1.12.145, ruling 2026-10-28): the tick the record reached the door
            const uint32_t key = cw.w.r.ch * 2u + (cw.w.r.wr ? 1u : 0u);
            std::deque<Waiting>& q = K.doors[key];
            if (!q.empty() || !chanAccept(K, cw.w)) { q.push_back(cw.w); K.waitingN++; }
        }
        chanTickTo(K, end);
    };
    parRun(f);
    chanMerge(t0, end);
    acc.fed = true;
    if (acc.pend == 0) finalizeEpoch(epoch);
    if (cutMode_) { lastFolded_ = epoch; anyFolded_ = true; }
    lastReplayed_ = epoch; anyReplayed_ = true;
    raw_.epochs++;
}

void DramEpochReplay::ctrlTotals(uint64_t& h, uint64_t& m, uint64_t& c, uint64_t& r) {
    h = m = c = r = 0;
    if (par()) { chanTotals(h, m, c, r); return; }   // (1.12.144)
    if (!sys_->pimid_ctrl_totals(h, m, c, r))
        warn("[mem] %s: the memory system keeps no controller totals; row and refresh counters stay 0", name_.c_str());
}

void DramEpochReplay::markRoi() {
    if (!sys_) return;
    futex_lock(&lock_);
    /* everything recorded so far is pre-ROI: replay it now so the bank state
     * carries into the ROI, then snapshot */
    while (!pending_.empty()) { uint64_t ep = pending_.begin()->first; replayEpoch(ep); pending_.erase(ep); }
    drainAll();   // 1.12.18 (drain the lag): the pre-ROI requests are served before the window opens
    /* the ROI snapshot restarts epoch numbering (mpiNocRoiBasePhase): the
     * frozen pre-ROI epochs are on the old axis and must not be found, or
     * pruned instead of the new ones (gate 1208B) */
    frozen_.clear();
    pcache_.clear();   // (1.12.145, ruling 2026-10-28)
    winSum_.clear(); winCnt_.clear();   // (1.12.153)
    if (cutMode_) {   // 1.12.17: the ROI-relative axis starts here
        roiTick0_ = curTick_;
        carried_.clear();
        anyFolded_ = false; lastFolded_ = 0;
    } else {
        /* 1.12.17: open the window AT the ROI start. Records are stamped on the
         * phase clock (numPhases x phase length + the core's offset), so the
         * first ROI record is at or after numPhases x phase length now. The
         * DRAM clock sat at the last pre-ROI request (0 when the device had no
         * pre-ROI traffic, as in co-simulation), and the first ROI replay then
         * ticked through the whole pre-ROI span INSIDE the window: co-sim
         * cosim_HBM3_omp_gemv counted 9.49e9 DRAM ticks (2.96e9 core cycles)
         * against a 3.53e8-cycle ROI, with its idle ticks and refreshes. The
         * same ticks are run either way (bank and refresh state are unchanged);
         * only the window boundary moves. */
        const uint64_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
        /* 1.12.18 (ruling 2026-10-23): open at the run time's roi_begin
         * stamp, the phase clock plus the opening core's offset. */
        const uint64_t open = (zinfo->roiWall.source == 1 && !zinfo->roiWall.ended && zinfo->roiWall.beginStamp > 0)
                            ? zinfo->roiWall.beginStamp : (uint64_t)zinfo->numPhases * pl;
        tickTo((uint64_t)((double)open * ticksPerCycle_));
    }
    closeArmed_ = closeSnapped_ = false; closeTick_ = closeLate_ = pastEndRecs_ = 0;   // 1.12.18 (R1): every mode
    hist_.clear();   // 1.12.18 (shift by lag)
    ctrlTotals(raw_.hits, raw_.misses, raw_.conflicts, raw_.refreshes);
    bankTotals();   // 1.12.18
    base_ = raw_;
    basePriced_ = rawPriced_.load(); baseBoot_ = rawBoot_.load();
    baseHPriced_ = rawHPriced_.load(); baseHBoot_ = rawHBoot_.load();   // 1.12.18 (R0605)
    hLatMin_ = ~0ull; hLatMax_ = 0;
    /* (1.12.189): the reconciliation counts the ROI's accesses only; the replay is drained, no record is held */
    for (auto& ar : recSt_) for (RecStat& s : ar) s = RecStat();
    for (double& x : recCredit_) x = 0.0;
    recOvChg_[0] = recOvChg_[1] = recOvUni_[0] = recOvUni_[1] = 0.0;
    recApplied_ = recDelays_ = recBarriers_ = recRefund_ = 0; recAppliedCls_[0] = recAppliedCls_[1] = 0; recChgSum_ = 0; lastRoiDone_ = lastHeldDone_ = 0; recDma_ = 0.0; recDmaN_ = 0;
    recCommDelay_ = recCommRewind_ = recComms_ = 0; runEndMarked_ = settled_ = false;
    clShiftSum_ = 0.0; clShiftN_ = 0; cl_.clear(); compBin_.clear();
    for (uint32_t k = 0; k < kRecCores; k++) { pool_[k].store(0, std::memory_order_relaxed); used_[k].store(0, std::memory_order_relaxed); ovA_[k].store(1.0, std::memory_order_relaxed); }
    std::fill(perChan_.begin(), perChan_.end(), 0);
    const bool again = roiMarked_;
    roiMarked_ = true;
    futex_unlock(&lock_);
    info("[mem] %s: statistics window %s at roi_begin (%lu pre-ROI requests replayed; counters report the ROI only) [1.11.98]",
         name_.c_str(), again ? "re-opened" : "opened", (unsigned long)base_.req);
}

void DramEpochReplay::finalize() {
    if (!sys_) return;
    /* 1.11.98 (gate 1208A): never block here. finalize() runs inside the
     * statistics dump, which zsim also runs from its exit handler; if the
     * model exits in the middle of a replay (Ramulator calls std::exit on an
     * invalid state) the exiting thread already holds the lock and a plain
     * lock deadlocks the process instead of letting it exit. */
    if (!futex_trylock_nospin_timeout(&lock_, 2000000000ull)) {
        warn("[mem] %s: the replay lock is held (exit during a replay?); the counters keep their last window", name_.c_str());
        return;
    }
    armClose(true);   // 1.12.18: before the pending epochs cross the ROI end (phase mode: armed here)
    if (settled_ && closeArmed_ && zinfo->roiWall.cycles() != settleRun_) {   // (1.12.189): the run time was raised (settleFinal)
        closeArmed_ = closeSnapped_ = false; closeLate_ = 0; settleRun_ = zinfo->roiWall.cycles();
        armClose(true);
    }
    /* replay whatever is still pending so the controller totals cover the window */
    while (!pending_.empty()) { uint64_t ep = pending_.begin()->first; replayEpoch(ep); pending_.erase(ep); }
    drainAll();   // 1.12.18 (drain the lag): every fed request served before the counters are read
    /* 1.12.18 (ruling 17 / R0605): a host-only instance closes its window at
     * the current phase clock, as markRoi opens it there: a decoupled host
     * memory sits idle through a co-simulated kernel, and its replayed DRAM
     * time (and the refresh commands counted in it) would otherwise end at
     * the host's last request. Monotone; bank and refresh state carry on. */
    /* 1.12.18 (user ruling 2026-10-23, "run time = elapsed wall clock"):
     * every instance closes its window at the ROI END, the run time's one
     * owner (zinfo->roiWall), not at its last request's drain: the DRAM
     * refreshes and idles through the rest of the ROI, and its background
     * and refresh are priced over the same window as everything else.
     * Monotone (a drain past the end stays); bank and refresh state carry on.
     * Phase-clock stamps in phase mode; the ROI-relative end in cut mode. */
    bool closedAtEnd = false;
    if (roiMarked_ && zinfo->roiWall.ended && closeArmed_) {
        /* 1.12.18 (R1, R2, B4): the close armed above -- cut mode at the
         * ROI-relative end on the rank cores' clock, phase mode at the
         * pricing window's end (the run time raised to the busy max). */
        const uint64_t t0 = curTick_;
        tickTo(closeTick_);
        closedAtEnd = true;
        if (!closeReported_) {
            closeReported_ = true;
            info("[mem] %s: replay window closed at the ROI end (run time %lu cycles, source %u): %lu DRAM cycles ticked past the last request's drain; window %lu DRAM cycles [1.12.18]",
                 name_.c_str(), (unsigned long)zinfo->roiWall.cycles(), (unsigned)zinfo->roiWall.source,
                 (unsigned long)(curTick_ - t0), (unsigned long)((closeSnapped_ ? snapTicks_ : raw_.ticks) - base_.ticks));
            if (closeSnapped_ && (cutMode_ || pastEndRecs_ > 0 || closeLate_ > 0))
                info("[mem] %s: 1.12.18: window frozen at the ROI end (DRAM tick %lu%s): ticks, idle ticks, refreshes and bank-open time are counted to it; "
                     "%lu record(s) stamped past it or issued in a rank's post-ROI tail replayed OUTSIDE the ROI (not priced, see outside*), %lu DRAM cycles replayed past it; "
                     "host records stamped on the host core's own clock %lu, on the ROI-relative phase grid %lu",
                     name_.c_str(), (unsigned long)closeTick_, closeLate_ ? " -- reached late, the clock stood past it" : "",
                     (unsigned long)pastEndRecs_, (unsigned long)(curTick_ > closeTick_ ? curTick_ - closeTick_ : 0),
                     (unsigned long)hostCoreStamps_.load(), (unsigned long)hostPhaseStamps_.load());
        }
    }
    if (!closedAtEnd && hostOnly_ && !cutMode_ && roiMarked_) {
        const uint64_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
        tickTo((uint64_t)((double)((uint64_t)zinfo->numPhases * pl) * ticksPerCycle_));
    }
    if (hostDataBus_ && !hdbReported_) {   /* (1.12.169): the host data bus at bank-parallel placements */
        uint64_t hc = 0, hb = 0;
        bool got = false;
        if (par()) {
            for (Chan& K : chans_) { uint64_t a = 0, b = 0; if (K.sys->pimid_hdb_totals(a, b)) { hc += a; hb += b; got = true; } }
        } else got = sys_ && sys_->pimid_hdb_totals(hc, hb);
        if (got) {
            hdbReported_ = true;
            info("[mem] %s: host data bus (bank-parallel placement; JEDEC JESD79-5 burst length BL/2 clocks per column access of a channel, "
                 "either direction; PE data stays in the chip and does not use it): host column commands %lu, burst clocks %lu (cumulative, every window)",
                 name_.c_str(), (unsigned long)hc, (unsigned long)hb);
        }
    }
    if (chipSplit_ && !chipReported_) {   /* 1.12.18 (user ruling 2026-10-24) */
        uint64_t sa = 0, cw = 0, cc = 0, ca = 0;
        bool got = false;
        if (par()) {   // (1.12.144)
            for (Chan& K : chans_) { uint64_t a = 0, b = 0, x = 0, y = 0; if (K.sys->pimid_chip_totals(a, b, x, y)) { sa += a; cw += b; cc += x; ca += y; got = true; } }
        } else got = sys_ && sys_->pimid_chip_totals(sa, cw, cc, ca);
        if (got) {
            chipReported_ = true;
            info("[mem] %s: chip split x%u, host requests in lock step: host commands on the DIMM CA bus %lu, of them ACTs %lu (TIMING: one rank-wide ACT each); "
                 "host chip ACTs beyond chip 0 %lu (ENERGY: every chip's ACT is in hostActivates); host candidate-cycles held by the DIMM CA bus %lu (cumulative, every window)",
                 name_.c_str(), chips_, (unsigned long)cc, (unsigned long)ca, (unsigned long)sa, (unsigned long)cw);
        }
    }
    ctrlTotals(raw_.hits, raw_.misses, raw_.conflicts, raw_.refreshes);
    bankTotals();   // 1.12.18
    /* 1.12.18 (R0605): the ctrl* row and activate counters are the DEVICE
     * interfaces' share (the controller's totals minus the host requests'),
     * the host* counters the host's; refreshes, ticks, idle ticks and the
     * bank-open time are the array's, whoever caused them. */
    /* 1.12.18 (R1): and minus the requests outside the ROI (source_id 2/3) */
    profActivates_.set((raw_.acts - raw_.hacts - raw_.oacts) - (base_.acts - base_.hacts - base_.oacts));   // 1.12.18
    if (hostMis_ > 0) {
        profHRequests_.set(raw_.hreq - base_.hreq);
        profHReads_.set(raw_.hrd - base_.hrd);
        profHWrites_.set(raw_.hwr - base_.hwr);
        profHLatSum_.set(raw_.hlat - base_.hlat);
        profHRLatSum_.set(raw_.hrlat - base_.hrlat);   // (1.12.189)
        profHRowHits_.set(raw_.hhits - base_.hhits);
        profHRowMisses_.set(raw_.hmisses - base_.hmisses);
        profHRowConflicts_.set(raw_.hconflicts - base_.hconflicts);
        profHActivates_.set(raw_.hacts - base_.hacts);
        profHPriced_.set(rawHPriced_.load() - baseHPriced_);
        profHBootstrap_.set(rawHBoot_.load() - baseHBoot_);
    }
    /* 1.12.18: cut mode -- the time-based counters end at the ROI end */
    const bool snap = closeSnapped_;
    if (bankTracked_) {
        profBankOpenCycles_.set((snap ? snapBankO_ : raw_.bankO) - base_.bankO);
        profBankOpenWindow_.set((snap ? snapBankW_ : raw_.bankW) - base_.bankW);
    }
    /* SET, not add: finalize() runs at every statistics dump and the totals
     * are cumulative (adding them counted every request ~4x, gate 1208A). */
    profRequests_.set(raw_.req - base_.req);
    profReads_.set(raw_.rd - base_.rd);
    profWrites_.set(raw_.wr - base_.wr);
    profLatSum_.set(raw_.lat - base_.lat);
    profEpochs_.set(raw_.epochs - base_.epochs);
    profUnserved_.set(raw_.unserved - base_.unserved);
    profTicks_.set((snap ? snapTicks_ : raw_.ticks) - base_.ticks);
    profIdleTicks_.set((snap ? snapIdle_ : raw_.idle) - base_.idle);
    profRowHits_.set((raw_.hits - raw_.hhits - raw_.ohits) - (base_.hits - base_.hhits - base_.ohits));   // 1.12.18 (R0605): the device's share; (R1) ROI only
    profRowMisses_.set((raw_.misses - raw_.hmisses - raw_.omisses) - (base_.misses - base_.hmisses - base_.omisses));
    profRowConflicts_.set((raw_.conflicts - raw_.hconflicts - raw_.oconflicts) - (base_.conflicts - base_.hconflicts - base_.oconflicts));
    /* 1.12.18 (user ruling R1 2026-10-24): outside the ROI, not priced */
    profORequests_.set(raw_.oreq - base_.oreq);
    profOReads_.set(raw_.ord - base_.ord);
    profOWrites_.set(raw_.owr - base_.owr);
    profORowHits_.set(raw_.ohits - base_.ohits);
    profORowMisses_.set(raw_.omisses - base_.omisses);
    profORowConflicts_.set(raw_.oconflicts - base_.oconflicts);
    profOActivates_.set((raw_.oacts - raw_.ohacts) - (base_.oacts - base_.ohacts));
    profOHostReads_.set(raw_.ohrd - base_.ohrd);
    profOHostWrites_.set(raw_.ohwr - base_.ohwr);
    profOHostActivates_.set(raw_.ohacts - base_.ohacts);
    applyOutside();
    profRefreshes_.set((snap ? snapRefreshes_ : raw_.refreshes) - base_.refreshes);
    if (pdOn_) {   // 1.12.20: the power-down unit-cycles over the same window as ctrlTicks
        profPdCycles_.set((snap ? snapPdC_ : raw_.pdC) - base_.pdC);
        profPdUnits_.set(raw_.pdU);
        profPdExits_.set((snap ? snapPdX_ : raw_.pdX) - base_.pdX);
    }
    profPriced_.set(rawPriced_.load() - basePriced_);
    profBootstrap_.set(rawBoot_.load() - baseBoot_);
    uint64_t used = 0, mx = 0, tot = 0;
    for (uint64_t v : perChan_) { if (v) used++; if (v > mx) mx = v; tot += v; }
    profChannelsUsed_.set(used);
    profMaxChanPermille_.set(tot ? (mx * 1000 + tot / 2) / tot : 0);
    recReport();   // (1.12.189)
    const uint64_t late = late_, stale = stale_;
    futex_unlock(&lock_);
    if (doorRecs_ > 0 && !doorReported_) {   // 1.12.18 (drain the lag)
        doorReported_ = true;
        info("[mem] %s: 1.12.18 drain the lag: %lu request(s) found their controller queue full and entered when it could take them, mean wait %.0f, max %lu DRAM cycles; %lu read(s) of an epoch still in flight took its partial mean (%lu with unfinished requests at their age)",
             name_.c_str(), (unsigned long)doorRecs_, (double)doorTicks_ / (double)doorRecs_, (unsigned long)doorMax_, (unsigned long)partial_, (unsigned long)partialAged_);
    }
    {   // (1.12.145, ruling 2026-10-28): once per dump
        info("[mem] %s: (1.12.145) epoch %u phase(s), read %lu epochs back; %lu partial mean(s) counted unfinished requests at their age",
             name_.c_str(), epochPhases(), (unsigned long)kDramReadLagEpochs, (unsigned long)partialAged_);
    }
    info("[mem] %s: (1.12.153) price window %u epoch(s), each epoch weighted by its request count, a read and a write priced apart (1.12.189)", name_.c_str(), priceWindow());
    if (cutMode_)
        info("[mem] %s: message-passing epoch replay (1.12.17): ROI-relative stamps, epochs folded at the consistent cut; "
             "%lu record(s) joined a later epoch, %lu read(s) took an epoch before the lag", name_.c_str(),
             (unsigned long)late, (unsigned long)stale);
    if (getenv("PIMID_DEBUG_REPLAY"))
        info("[replaydbg] window%s: requests=%lu hits=%lu misses=%lu conflicts=%lu refreshes=%lu ticks=%lu idle=%lu channels=%lu max=%lu permille",
             roiMarked_ ? " (ROI)" : " (whole run: no roi_begin seen)",
             (unsigned long)(raw_.req - base_.req), (unsigned long)(raw_.hits - base_.hits), (unsigned long)(raw_.misses - base_.misses),
             (unsigned long)(raw_.conflicts - base_.conflicts), (unsigned long)(raw_.refreshes - base_.refreshes),
             (unsigned long)(raw_.ticks - base_.ticks), (unsigned long)(raw_.idle - base_.idle), (unsigned long)used,
             (unsigned long)(tot ? (mx * 1000 + tot / 2) / tot : 0));
    if (getenv("PIMID_DEBUG_REPLAY") && hostMis_ > 0)   // 1.12.18 (R0605)
        info("[replaydbg] %s host share%s: requests=%lu hits=%lu misses=%lu conflicts=%lu activates=%lu meanLatency=%.1f host cycles (min %lu max %lu) priced=%lu bootstrap=%lu",
             name_.c_str(), roiMarked_ ? " (ROI)" : " (whole run)",
             (unsigned long)(raw_.hreq - base_.hreq), (unsigned long)(raw_.hhits - base_.hhits),
             (unsigned long)(raw_.hmisses - base_.hmisses), (unsigned long)(raw_.hconflicts - base_.hconflicts),
             (unsigned long)(raw_.hacts - base_.hacts),
             (raw_.hreq > base_.hreq) ? (double)(raw_.hlat - base_.hlat) / (double)(raw_.hreq - base_.hreq) : 0.0,
             (unsigned long)(hLatMin_ == ~0ull ? 0 : hLatMin_), (unsigned long)hLatMax_,
             (unsigned long)(rawHPriced_.load() - baseHPriced_), (unsigned long)(rawHBoot_.load() - baseHBoot_));
}

#else
DramEpochReplay::DramEpochReplay(const std::string&, uint64_t, uint32_t, uint32_t, uint32_t, const char* name, bool, uint32_t) { panic("DramEpochReplay %s: built without Ramulator", name); }
uint32_t DramEpochReplay::addHostInterface(uint64_t) { return 0; }
uint32_t DramEpochReplay::hostAccess(uint32_t, uint64_t, uint64_t, uint32_t, bool, bool) { return 0; }
void DramEpochReplay::setHostPrefetch(bool) {}
void DramEpochReplay::setPrefetchTag(uint32_t) {}
void DramEpochReplay::prefetchConsumed(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t) {}
void DramEpochReplay::prefetchDropped(uint32_t, uint32_t) {}
void DramEpochReplay::bindCounters(uint32_t, Counter*, Counter*, Counter*, Counter*) {}
void DramEpochReplay::recordAddr(uint64_t, uint64_t, uint64_t, uint32_t, bool, uint32_t, uint8_t, uint32_t) {}
DramEpochReplay::~DramEpochReplay() {}
void DramEpochReplay::record(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint64_t, bool, uint32_t, uint32_t, bool, uint32_t, bool) {}
void DramEpochReplay::reconcileAtBarrier(const std::function<bool(uint32_t)>&) {}
void DramEpochReplay::dmaBurst(bool) {}
void DramEpochReplay::noteSync(uint32_t) {}
int64_t DramEpochReplay::reconcileCore(uint32_t, uint64_t) { return 0; }
void DramEpochReplay::markRunEnd() {}
void DramEpochReplay::settleFinal() {}
double DramEpochReplay::owedEnd(uint32_t) { return 0.0; }
bool DramEpochReplay::heldEnd(uint32_t, uint64_t, double&) { return false; }
uint32_t DramEpochReplay::chargeLast(uint32_t lat, bool, uint32_t) { return lat; }
bool DramEpochReplay::lastRecorded() const { return false; }
void DramEpochReplay::markRoi() {}
void DramEpochReplay::ctrlTotals(uint64_t&, uint64_t&, uint64_t&, uint64_t&) {}
uint64_t DramEpochReplay::composeAddr(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t*, int) { return 0; }
uint32_t DramEpochReplay::serviceLatency(uint32_t, uint64_t, bool, bool) { return 0; }
void DramEpochReplay::initStats(AggregateStat*) {}
void DramEpochReplay::finalize() {}
void DramEpochReplay::replayEpoch(uint64_t) {}
void DramEpochReplay::tickTo(uint64_t) {}
void DramEpochReplay::tickOnce() {}
void DramEpochReplay::foldByCut(uint64_t, uint64_t) {}
uint32_t DramEpochReplay::serviceLatencyCut(uint32_t, uint64_t, uint64_t, bool, bool) { return 0; }
void DramEpochReplay::bankTotals() {}
#endif
