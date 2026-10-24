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
#include <cstdlib>
#include <iterator>
#include "log.h"
#include "zsim.h"

#ifdef _WITH_RAMULATOR_

DramEpochReplay::DramEpochReplay(const std::string& configFile, uint64_t coreFreqHz,
                                 uint32_t numInterfaces, uint32_t burstsPerLine, uint32_t lineBits,
                                 const char* name, bool hostOnly, uint32_t hostChannels)
    : name_(name), numMIs_(numInterfaces), burstsPerLine_(burstsPerLine < 1 ? 1 : burstsPerLine),
      lineBits_(lineBits) {
    futex_init(&lock_);
    hostOnly_ = hostOnly;   // 1.12.18 (R0605)
    {   /* 1.12.17: message passing (thread-MPI) replays in cut mode */
        const char* r = getenv("PIMID_MPI_RANKS");
        cutMode_ = (r && atoi(r) > 1 && !getenv("PIMID_MPI_RANK"));
        if (PEMemoryInterface::singleProcCut()) cutMode_ = true;   // 1.12.18 (ruling 2026-10-24): single-process runs too
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
    outRd_.assign(numMIs_, 0); outWr_.assign(numMIs_, 0); outRdApplied_.assign(numMIs_, 0); outWrApplied_.assign(numMIs_, 0);
    sumLat_.assign(numMIs_, 0); cntLat_.assign(numMIs_, 0);
    lastValue_.assign(numMIs_, 0);
    miTicksPerCycle_.assign(numMIs_, ticksPerCycle_);   // 1.12.18 (R0605): host interfaces carry their own clock
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
    sumLat_.push_back(0); cntLat_.push_back(0); lastValue_.push_back(0);
    miTicksPerCycle_.push_back(tpc);
    miRd_.push_back(nullptr); miWr_.push_back(nullptr);   // 1.12.18 (R1)
    outRd_.push_back(0); outWr_.push_back(0); outRdApplied_.push_back(0); outWrApplied_.push_back(0);
    futex_unlock(&lock_);
    info("[mem] %s: host memory interface %u joined the replay (%s): host clock %.1f MHz = %.4f DRAM cycles per host cycle; its requests carry source_id 1 and the controller splits their row hits/misses/conflicts and activates [1.12.18 ruling 17]",
         name_.c_str(), mi, hostOnly_ ? "host-only instance" : "the device's instance: the shared array, interfaces after the device's",
         (double)hostFreqHz / 1e6, tpc);
    return mi;
}

void DramEpochReplay::bindCounters(uint32_t mi, Counter* rd, Counter* wr) {   // 1.12.18 (R1)
    futex_lock(&lock_);
    if (mi < numMIs_) { miRd_[mi] = rd; miWr_[mi] = wr; }
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
                             uint64_t lineAddr, bool isWrite, uint32_t mi) {
    /* 1.11.98: the window closes with the ROI, as every other counter does
     * (post-termination accesses are not counted anywhere). */
    if (zinfo->terminationConditionMet) return;
    uint32_t ch = 0;
    const uint64_t addr = composeAddr(unit, slot, row, lineAddr, &ch);
    recordAddr(epoch, stamp, addr, ch, isWrite, mi);
}

void DramEpochReplay::recordAddr(uint64_t epoch, uint64_t stamp, uint64_t addr, uint32_t ch, bool isWrite, uint32_t mi, uint8_t shadow, uint32_t grp) {
    futex_lock(&lock_);
    /* 1.12.17 cut mode: a record for an epoch the cut has already folded (a
     * core that was exempt from the cut while parked in an MPI wait) joins
     * the next unfolded epoch -- its stamp is behind the replay clock, so it
     * issues at the clock, as the shared-memory replay does with a late
     * stamp; a folded epoch is never replayed twice. */
    if (cutMode_ && anyFolded_ && epoch <= lastFolded_) { epoch = lastFolded_ + 1; late_++; }
    pending_[epoch].push_back(Rec{stamp, addr, mi, (uint8_t)(isWrite ? 1 : 0), ch, shadow, grp});
    if (ch < perChan_.size()) perChan_[ch]++;
    futex_unlock(&lock_);
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
    const bool detMpi = PEMemoryInterface::cutPricing();   // 1.12.18: thread-MPI or single-process cut
    if (cutMode_ != detMpi)
        panic("DramEpochReplay %s: cut mode %d disagrees with the pricing mode %d (host interface)",
              name_.c_str(), (int)cutMode_, (int)detMpi);
    if (cutMode_ && !zinfo->hierarchy.mpiNocBaselined) { rawHBoot_.fetch_add(1, std::memory_order_relaxed); return 0; }
    uint32_t E = zinfo->garnetNetwork ? zinfo->garnetNetwork->detEpochPhases() : 4;
    if (E < 1) E = 1;
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
         * (Core::getCycles()); the stamp is that clock relative to it -- the
         * axis the run time of a host-rank (no-offload) baseline is counted
         * on. A memory request carries the RAW clock (curCycle, halted and
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
        if (based) {
            stamp = PEMemoryInterface::perCoreRoiRel(srcId, cycle);
        } else if (hostBased) {
            const uint64_t c = adjustedClock ? cycle : PEMemoryInterface::wallFreeCycle(srcId, cycle);
            const uint64_t o = PEMemoryInterface::roiOriginOf(srcId);
            stamp = (c > o) ? c - o : 0;
            hostCoreStamps_.fetch_add(1, std::memory_order_relaxed);
        } else {
            const uint64_t bp = zinfo->hierarchy.mpiNocRoiBasePhase;
            const uint64_t rel = (zinfo->numPhases >= bp) ? zinfo->numPhases - bp : 0;
            stamp = rel * pl + (cycle % pl);
            hostPhaseStamps_.fetch_add(1, std::memory_order_relaxed);
        }
        epoch = stamp / ((uint64_t)pl * E);
    } else {
        const uint64_t bp = zinfo->hierarchy.mpiNocRoiBasePhase;
        const uint64_t rel = (zinfo->numPhases >= bp) ? zinfo->numPhases - bp : 0;
        epoch = rel / E;
        stamp = PEMemoryInterface::phaseStamp(cycle);
    }
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
                 * bank and row in every chip; copy 0 is the counted request, the others its shadows. */
                const uint64_t s = hostRowSlots_ ? hostRowSlots_ : 1;
                const uint64_t stripe = (row * s + slot) / chips_;
                slot = (uint32_t)(stripe % s); row = stripe / s;
                futex_lock(&lock_);
                uint32_t grp = ++nextGrp_; if (grp == 0) grp = ++nextGrp_;
                futex_unlock(&lock_);
                for (uint32_t c = 0; c < chips_; c++) {
                    const uint64_t a = composeAddr(unit, slot, row, lineAddr, &ch, (int)c);
                    recordAddr(epoch, stamp, a, ch, isWrite, mi, (uint8_t)(c ? 1 : 0), grp);
                }
            } else {
                addr = composeAddr(unit, slot, row, lineAddr, &ch);
                recordAddr(epoch, stamp, addr, ch, isWrite, mi);
            }
        }
        if (hostOnly_) recordAddr(epoch, stamp, addr, ch, isWrite, mi);
    }
    const uint32_t v = cutMode_ ? serviceLatencyCut(mi, epoch, PEMemoryInterface::kNocReadLagEpochs)
                                : serviceLatency(mi, epoch);
    if (v > 0) rawHPriced_.fetch_add(1, std::memory_order_relaxed); else rawHBoot_.fetch_add(1, std::memory_order_relaxed);
    return v;
}

uint32_t DramEpochReplay::serviceLatency(uint32_t mi, uint64_t curEpoch) {
    if (curEpoch == 0 || mi >= numMIs_) return 0;
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
    if (it != frozen_.end() && mi < it->second.size()) v = it->second[mi];
    else if (it == frozen_.end()) v = partialMean(curEpoch - 1, mi);   // 1.12.18: still in flight
    /* 1.11.98 (gate 1208B): an interface with no access in the previous epoch
     * keeps the last service latency the replay measured for it (bursty
     * interfaces otherwise fell back to the analytical service every other
     * epoch); the analytical bootstrap only before its first replayed value. */
    if (v == 0 && mi < lastValue_.size()) v = lastValue_[mi];
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

uint32_t DramEpochReplay::serviceLatencyCut(uint32_t mi, uint64_t epoch, uint64_t lag) {
    if (!cutMode_ || mi >= numMIs_ || epoch < lag) return 0;
    const uint64_t r = epoch - lag;
    futex_lock(&lock_);
    uint32_t v = 0;
    /* the latest folded epoch at or before r: r itself once the cut has
     * passed it (the lag is chosen so it has, as for the NoC); an earlier one
     * when the cut trails further (counted: stale_) */
    auto it = carried_.upper_bound(r);
    /* 1.12.18: epoch r fed but still in flight (drain the lag: its last
     * requests have not completed): the mean of its requests completed so
     * far, not an older final epoch's value */
    if ((it == carried_.begin() || std::prev(it)->first != r) && (v = partialMean(r, mi)) > 0) {
        partial_++;
    } else if (it != carried_.begin()) {
        --it;
        if (it->first != r) stale_++;
        if (mi < it->second.size()) v = it->second[mi];
    }
    while (carried_.size() > 1 && carried_.begin()->first + 16 < r && std::next(carried_.begin())->first <= r)
        carried_.erase(carried_.begin());
    futex_unlock(&lock_);
    return v;
}

void DramEpochReplay::bankTotals() {
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
    if (closeArmed_ && !closeSnapped_ && curTick_ >= closeTick_) snapClose();   // 1.12.18: the ROI end
    else if (roiMarked_ && !closeArmed_ && (curTick_ % kSnapEvery) == 0) { Snap s; takeSnap(s); hist_.push_back(s); }   // 1.12.18 (shift by lag)
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
 * the dump writes (the "cycles" lines, host cCycles added as the parser
 * does), evaluated now -- finalize runs right before every dump. */
uint64_t DramEpochReplay::phaseWindowCycles(std::string* why) {
    const uint64_t roi = zinfo->roiWall.cycles();
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
            const uint64_t w = cyc + (host ? cc : 0);
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
        /* 1.12.18 (user ruling 2026-10-24 (a)): a single-process run in cut
         * mode closes at its run time, the latest device PE's ROI end on the
         * records' own axis (as thread-MPI's source 2): window == run time. */
        closeTick_ = roiTick0_ + (uint64_t)((double)zinfo->roiWall.mpiEnd * runTicksPerCycle());
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
    hist_.clear(); hist_.shrink_to_fit();
}

/* 1.12.18: cut mode -- see closeArmed_ in the header. Called under lock_. */

void DramEpochReplay::takeSnap(Snap& s) {
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
            if (a != acc_.end() && f.mi < a->second.sum.size()) { a->second.sum[f.mi] += latCyc; a->second.cnt[f.mi] += 1; }
            if (f.out) raw_.olat += latCyc;   // 1.12.18 (R1)
            else if (isHostMi(f.mi)) {
                raw_.hlat += latCyc;
                if (latCyc < hLatMin_) hLatMin_ = latCyc;
                if (latCyc > hLatMax_) hLatMax_ = latCyc;
            } else raw_.lat += latCyc;
            }
            outstanding_--;
            inflight_.erase(it);
            if (a != acc_.end() && a->second.pend > 0 && --a->second.pend == 0 && a->second.fed) finalizeEpoch(ep);
        };
        if (!fe_->receive_external_requests(typeId, (Ramulator::Addr_t)(r.addr + w.b * step),
                                            (isHostMi(r.mi) ? 1 : 0) + (w.out ? 2 : 0) + (r.shadow ? 3 : 0), cb))   /* 1.12.18: +3 = a lock-step shadow copy */   // 1.12.18 (R1): +2 = outside the ROI; (R0605): source_id 1 = host
            return false;   // the queue is full: the record waits at the door
        if (w.b == 0) {   // the request has entered the controller: in flight from now
            inflight_[w.id] = Inflight{burstsPerLine_, curTick_, r.mi, r.wr != 0, w.out, w.epoch, r.grp};
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
uint32_t DramEpochReplay::partialMean(uint64_t e, uint32_t mi) {
    auto a = acc_.find(e);
    if (a == acc_.end() || !a->second.fed || mi >= a->second.cnt.size() || a->second.cnt[mi] == 0) return 0;
    if (!cutMode_) partial_++;
    return (uint32_t)((a->second.sum[mi] + a->second.cnt[mi] / 2) / a->second.cnt[mi]);
}

void DramEpochReplay::finalizeEpoch(uint64_t epoch) {
    auto a = acc_.find(epoch);
    if (a == acc_.end()) return;
    std::vector<uint32_t> perMi(numMIs_, 0);
    for (uint32_t m = 0; m < numMIs_ && m < a->second.cnt.size(); ++m)
        if (a->second.cnt[m] > 0) perMi[m] = (uint32_t)((a->second.sum[m] + a->second.cnt[m] / 2) / a->second.cnt[m]);
    acc_.erase(a);
    frozen_[epoch] = perMi;
    if (cutMode_) {   // 1.12.17: an interface idle in this epoch keeps its last measured value
        std::vector<uint32_t> c = perMi;
        for (uint32_t m = 0; m < numMIs_ && m < lastValue_.size(); ++m) if (c[m] == 0) c[m] = lastValue_[m];
        carried_[epoch] = c;
    }
    for (uint32_t m = 0; m < numMIs_ && m < lastValue_.size(); ++m) if (perMi[m] > 0) lastValue_[m] = perMi[m];
}

/* everything fed is served (pre-ROI before the window opens; the tail before
 * the counters are read). Bounded so a lost request cannot hang the run. */
void DramEpochReplay::drainAll() {
    uint64_t guard = 0;
    while ((outstanding_ > 0 || waitingN_ > 0) && guard < 50000000ull) { tickOnce(); guard++; }
    if (outstanding_ > 0 || waitingN_ > 0) {
        raw_.unserved += outstanding_ + waitingN_;
        outstanding_ = 0; waitingN_ = 0; doors_.clear(); inflight_.clear();
    }
    while (!acc_.empty()) finalizeEpoch(acc_.begin()->first);
}

void DramEpochReplay::replayEpoch(uint64_t epoch) {
    armClose(false);   // 1.12.18: cut mode, once the run time's ROI end is known
    std::vector<Rec>& recs = pending_[epoch];
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        if (a.stamp != b.stamp) return a.stamp < b.stamp;
        if (a.mi != b.mi) return a.mi < b.mi;
        if (a.addr != b.addr) return a.addr < b.addr;
        return a.wr < b.wr;
    });
    EpochAcc& acc = acc_[epoch];
    if (acc.sum.empty()) { acc.sum.assign(numMIs_, 0); acc.cnt.assign(numMIs_, 0); }
    acc.pend += recs.size(); acc.fed = false;
    for (size_t i = 0; i < recs.size(); ++i) {
        const Rec& r = recs[i];
        uint64_t target = (uint64_t)((double)r.stamp * ticksPerCycle_);
        if (cutMode_) target += roiTick0_;   // 1.12.17: ROI-relative stamps start at the ROI's DRAM tick
        /* 1.12.18 (user ruling R1 2026-10-24): a request stamped after the
         * ROI end is replayed (the controller's state stays correct) but is
         * outside: source_id 2 (device) / 3 (host) in the controller, and in
         * none of the ROI counts, latencies or row/ACT totals. Judged on its
         * own stamp: a wait at the door never pushes it outside. */
        const bool out = closeArmed_ && target > closeTick_;
        if (out) pastEndRecs_++;
        tickTo(target);   // monotone: a stamp behind the clock arrives now
        Waiting w{r, epoch, target, out, 0, nextId_++};
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

void DramEpochReplay::ctrlTotals(uint64_t& h, uint64_t& m, uint64_t& c, uint64_t& r) {
    h = m = c = r = 0;
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
                     "%lu record(s) stamped past it replayed OUTSIDE the ROI (not priced, see outside*), %lu DRAM cycles replayed past it; "
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
    if (chipSplit_ && !chipReported_) {   /* 1.12.18 (user ruling 2026-10-24) */
        uint64_t sa = 0, cw = 0;
        if (sys_ && sys_->pimid_chip_totals(sa, cw)) {
            chipReported_ = true;
            info("[mem] %s: 1.12.18: chip split x%u -- host lock-step shadow-copy ACTs %lu (host chip ACTs beyond chip 0, counted in the host ACTs for energy, never in the row counts); "
                 "host candidate-cycles held by the DIMM CA bus %lu (cumulative, every window)", name_.c_str(), chips_, (unsigned long)sa, (unsigned long)cw);
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
    const uint64_t late = late_, stale = stale_;
    futex_unlock(&lock_);
    if (doorRecs_ > 0 && !doorReported_) {   // 1.12.18 (drain the lag)
        doorReported_ = true;
        info("[mem] %s: 1.12.18 drain the lag: %lu request(s) found their controller queue full and entered when it could take them, mean wait %.0f, max %lu DRAM cycles; %lu read(s) of an epoch still in flight took its partial mean",
             name_.c_str(), (unsigned long)doorRecs_, (double)doorTicks_ / (double)doorRecs_, (unsigned long)doorMax_, (unsigned long)partial_);
    }
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
void DramEpochReplay::bindCounters(uint32_t, Counter*, Counter*) {}
void DramEpochReplay::recordAddr(uint64_t, uint64_t, uint64_t, uint32_t, bool, uint32_t, uint8_t, uint32_t) {}
DramEpochReplay::~DramEpochReplay() {}
void DramEpochReplay::record(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint64_t, bool, uint32_t) {}
void DramEpochReplay::markRoi() {}
void DramEpochReplay::ctrlTotals(uint64_t&, uint64_t&, uint64_t&, uint64_t&) {}
uint64_t DramEpochReplay::composeAddr(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t*, int) { return 0; }
uint32_t DramEpochReplay::serviceLatency(uint32_t, uint64_t) { return 0; }
void DramEpochReplay::initStats(AggregateStat*) {}
void DramEpochReplay::finalize() {}
void DramEpochReplay::replayEpoch(uint64_t) {}
void DramEpochReplay::tickTo(uint64_t) {}
void DramEpochReplay::tickOnce() {}
void DramEpochReplay::foldByCut(uint64_t, uint64_t) {}
uint32_t DramEpochReplay::serviceLatencyCut(uint32_t, uint64_t, uint64_t) { return 0; }
void DramEpochReplay::bankTotals() {}
#endif
