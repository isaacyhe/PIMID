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
#include <algorithm>
#include "log.h"
#include "zsim.h"

#ifdef _WITH_RAMULATOR_

DramEpochReplay::DramEpochReplay(const std::string& configFile, uint64_t coreFreqHz,
                                 uint32_t numInterfaces, uint32_t burstsPerLine, uint32_t lineBits,
                                 const char* name)
    : name_(name), numMIs_(numInterfaces), burstsPerLine_(burstsPerLine < 1 ? 1 : burstsPerLine),
      lineBits_(lineBits) {
    futex_init(&lock_);
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
    const uint32_t nch = (zinfo->hierarchy.dramChannels > 0) ? zinfo->hierarchy.dramChannels : 1;
    const bool hasRankLevel = (dimpl == "DDR3" || dimpl == "DDR4" || dimpl == "DDR5" || dimpl == "LPDDR5");
    config["MemorySystem"]["DRAM"]["org"]["channel"] = (int)nch;
    if (hasRankLevel)
        config["MemorySystem"]["DRAM"]["org"]["rank"] = (int)((zinfo->hierarchy.ranksPerChannel > 0) ? zinfo->hierarchy.ranksPerChannel : 1);
    config["MemorySystem"]["AddrMapper"]["impl"] = "ChRaBaRoCo";
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
        const uint32_t cpr = zinfo->hierarchy.chipsPerRank;
        foldedChannels_ = (nch > 1 && cpr == nch);
        perChan_.assign((size_t)std::max(1, counts_[0]), 0);
        const uint64_t pimidBanks = (uint64_t)std::max(1u, zinfo->hierarchy.bgPerChip) * (uint64_t)std::max(1u, zinfo->hierarchy.banksPerBG)
                                  * (foldedChannels_ ? 1ull : (uint64_t)std::max(1u, zinfo->hierarchy.ranksPerChannel));
        info("[mem] %s: replay organisation %s, %d channel(s), %lu banks per channel (PIMID hierarchy: %lu), ChRaBaRoCo, %u B per column access%s",
             name, dimpl.c_str(), counts_[0], (unsigned long)banksPerChannel_, (unsigned long)pimidBanks, 1u << txBits_,
             (pimidBanks != banksPerChannel_) ? " -- MISMATCH: bank indices wrap modulo the Ramulator count" : "");
    }
    const double tck = sys_->get_tCK();   // ns
    const double corePeriodNs = (coreFreqHz > 0) ? 1e9 / (double)coreFreqHz : 0.0;
    if (!(tck > 0.0) || !(corePeriodNs > 0.0))
        panic("DramEpochReplay %s: no DRAM clock (tCK %.3f ns, core %.3f ns)", name, tck, corePeriodNs);
    ticksPerCycle_ = corePeriodNs / tck;
    sumLat_.assign(numMIs_, 0); cntLat_.assign(numMIs_, 0);
    lastValue_.assign(numMIs_, 0);
    info("[mem] %s: DRAM epoch replay armed: Ramulator2 from %s, tCK %.4f ns = %.4f DRAM cycles per core cycle, %u interfaces, %u column commands per line [1.11.98 ruling 4 (c)]",
         name, configFile.c_str(), tck, ticksPerCycle_, numMIs_, burstsPerLine_);
}

DramEpochReplay::~DramEpochReplay() {}

void DramEpochReplay::initStats(AggregateStat* parentStat) {
    AggregateStat* s = new AggregateStat();
    s->init(name_.c_str(), "DRAM epoch replay (ruling 4 (c)): the device controller replayed per epoch");
    profRequests_.init("ctrlRequests", "Device DRAM requests replayed through the controller"); s->append(&profRequests_);
    profEpochs_.init("epochs", "Epochs replayed"); s->append(&profEpochs_);
    profReads_.init("reads", "Replayed reads"); s->append(&profReads_);
    profWrites_.init("writes", "Replayed writes"); s->append(&profWrites_);
    profLatSum_.init("latencySum", "Sum of measured request latencies (core cycles; mean = latencySum / requests)"); s->append(&profLatSum_);
    profUnserved_.init("unserved", "Requests still in flight at the drain bound (should be 0)"); s->append(&profUnserved_);
    profRowHits_.init("ctrlRowHits", "Controller row hits (Ramulator2 totals over the replayed run)"); s->append(&profRowHits_);
    profRowMisses_.init("ctrlRowMisses", "Controller row misses (a closed bank)"); s->append(&profRowMisses_);
    profRowConflicts_.init("ctrlRowConflicts", "Controller row conflicts (another row open: precharge + activate)"); s->append(&profRowConflicts_);
    profRefreshes_.init("ctrlRefreshes", "Refresh commands the controller issued over the replayed run (row 28: measured background)"); s->append(&profRefreshes_);
    profTicks_.init("ctrlTicks", "DRAM clock cycles the controller was replayed for"); s->append(&profTicks_);
    profIdleTicks_.init("ctrlIdleTicks", "Replayed DRAM cycles with no request in flight (row 28: measured idle)"); s->append(&profIdleTicks_);
    profChannelsUsed_.init("ctrlChannelsUsed", "Channels of the device that received replayed requests"); s->append(&profChannelsUsed_);
    profMaxChanPermille_.init("ctrlMaxChannelPermille", "The busiest channel's share of the replayed requests (per mille)"); s->append(&profMaxChanPermille_);
    profPriced_.init("priced", "Interface accesses priced at the previous epoch's replayed service latency"); s->append(&profPriced_);
    profBootstrap_.init("bootstrap", "Interface accesses priced by the analytical service (no replayed epoch yet)"); s->append(&profBootstrap_);
    parentStat->append(s);
}

uint64_t DramEpochReplay::composeAddr(uint32_t unit, uint32_t slot, uint64_t row, uint64_t lineAddr, uint32_t* chOut) {
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
    uint64_t flat = ((uint64_t)(foldedChannels_ ? 0 : pos.rank) * bgc + (uint64_t)pos.bank_group) * bpg + (uint64_t)pos.bank + slot;
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
    futex_lock(&lock_);
    pending_[epoch].push_back(Rec{stamp, addr, mi, (uint8_t)(isWrite ? 1 : 0)});
    if (ch < perChan_.size()) perChan_[ch]++;
    futex_unlock(&lock_);
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

void DramEpochReplay::tickOnce() {
    sys_->tick();
    curTick_++;
    raw_.ticks++;
    if (outstanding_ == 0) raw_.idle++;   // row 28: a controller cycle with nothing in flight
}

void DramEpochReplay::tickTo(uint64_t tick) {
    while (curTick_ < tick) tickOnce();
}

void DramEpochReplay::replayEpoch(uint64_t epoch) {
    std::vector<Rec>& recs = pending_[epoch];
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        if (a.stamp != b.stamp) return a.stamp < b.stamp;
        if (a.mi != b.mi) return a.mi < b.mi;
        if (a.addr != b.addr) return a.addr < b.addr;
        return a.wr < b.wr;
    });
    std::fill(sumLat_.begin(), sumLat_.end(), 0);
    std::fill(cntLat_.begin(), cntLat_.end(), 0);
    inflight_.clear();
    inflight_.reserve(recs.size());
    outstanding_ = 0;
    const uint64_t lineBytes = 1ull << lineBits_;
    const uint64_t step = lineBytes / burstsPerLine_;
    for (size_t i = 0; i < recs.size(); ++i) {
        const Rec& r = recs[i];
        uint64_t target = (uint64_t)((double)r.stamp * ticksPerCycle_);
        tickTo(target);   // monotone: a stamp behind the clock issues now
        inflight_.push_back(Inflight{burstsPerLine_, curTick_, r.mi, r.wr != 0});
        const size_t idx = inflight_.size() - 1;
        const int typeId = r.wr ? Ramulator::Request::Type::Write : Ramulator::Request::Type::Read;
        for (uint32_t b = 0; b < burstsPerLine_; ++b) {
            auto cb = [this, idx](Ramulator::Request&) {
                Inflight& f = inflight_[idx];
                if (f.remaining > 1) { f.remaining--; return; }
                f.remaining = 0;
                const uint64_t lat = (curTick_ + 1 > f.issueTick) ? (curTick_ + 1 - f.issueTick) : 1;
                const uint64_t latCyc = (uint64_t)((double)lat / ticksPerCycle_ + 0.5);
                if (f.mi < sumLat_.size()) { sumLat_[f.mi] += latCyc; cntLat_[f.mi] += 1; }
                raw_.lat += latCyc;
                outstanding_--;
            };
            /* a full controller queue refuses the request: advance the clock
             * until it is accepted (the request waits at the controller's door,
             * which is the queueing the model is here to measure). */
            uint32_t spin = 0;
            while (!fe_->receive_external_requests(typeId, (Ramulator::Addr_t)(r.addr + b * step), 0, cb)) {
                tickOnce();
                if (++spin > 10000000u) panic("DramEpochReplay %s: the controller never accepted a request (addr 0x%lx)", name_.c_str(), (unsigned long)r.addr);
            }
        }
        outstanding_++;
        raw_.req++;
        if (r.wr) raw_.wr++; else raw_.rd++;
    }
    /* drain: Ramulator2's generic controller completes reads through the
     * callback; writes complete at column issue (1.11.96 H12). Bound the drain
     * so a lost request cannot hang the simulation. */
    uint64_t guard = 0;
    while (outstanding_ > 0 && guard < 50000000ull) { tickOnce(); guard++; }
    if (outstanding_ > 0) { raw_.unserved += outstanding_; outstanding_ = 0; }
    std::vector<uint32_t> perMi(numMIs_, 0);
    for (uint32_t m = 0; m < numMIs_; ++m)
        if (cntLat_[m] > 0) perMi[m] = (uint32_t)((sumLat_[m] + cntLat_[m] / 2) / cntLat_[m]);
    frozen_[epoch] = perMi;
    for (uint32_t m = 0; m < numMIs_ && m < lastValue_.size(); ++m) if (perMi[m] > 0) lastValue_[m] = perMi[m];
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
    /* the ROI snapshot restarts epoch numbering (mpiNocRoiBasePhase): the
     * frozen pre-ROI epochs are on the old axis and must not be found, or
     * pruned instead of the new ones (gate 1208B) */
    frozen_.clear();
    ctrlTotals(raw_.hits, raw_.misses, raw_.conflicts, raw_.refreshes);
    base_ = raw_;
    basePriced_ = rawPriced_.load(); baseBoot_ = rawBoot_.load();
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
    /* replay whatever is still pending so the controller totals cover the window */
    while (!pending_.empty()) { uint64_t ep = pending_.begin()->first; replayEpoch(ep); pending_.erase(ep); }
    ctrlTotals(raw_.hits, raw_.misses, raw_.conflicts, raw_.refreshes);
    /* SET, not add: finalize() runs at every statistics dump and the totals
     * are cumulative (adding them counted every request ~4x, gate 1208A). */
    profRequests_.set(raw_.req - base_.req);
    profReads_.set(raw_.rd - base_.rd);
    profWrites_.set(raw_.wr - base_.wr);
    profLatSum_.set(raw_.lat - base_.lat);
    profEpochs_.set(raw_.epochs - base_.epochs);
    profUnserved_.set(raw_.unserved - base_.unserved);
    profTicks_.set(raw_.ticks - base_.ticks);
    profIdleTicks_.set(raw_.idle - base_.idle);
    profRowHits_.set(raw_.hits - base_.hits);
    profRowMisses_.set(raw_.misses - base_.misses);
    profRowConflicts_.set(raw_.conflicts - base_.conflicts);
    profRefreshes_.set(raw_.refreshes - base_.refreshes);
    profPriced_.set(rawPriced_.load() - basePriced_);
    profBootstrap_.set(rawBoot_.load() - baseBoot_);
    uint64_t used = 0, mx = 0, tot = 0;
    for (uint64_t v : perChan_) { if (v) used++; if (v > mx) mx = v; tot += v; }
    profChannelsUsed_.set(used);
    profMaxChanPermille_.set(tot ? (mx * 1000 + tot / 2) / tot : 0);
    futex_unlock(&lock_);
    if (getenv("PIMID_DEBUG_REPLAY"))
        info("[replaydbg] window%s: requests=%lu hits=%lu misses=%lu conflicts=%lu refreshes=%lu ticks=%lu idle=%lu channels=%lu max=%lu permille",
             roiMarked_ ? " (ROI)" : " (whole run: no roi_begin seen)",
             (unsigned long)(raw_.req - base_.req), (unsigned long)(raw_.hits - base_.hits), (unsigned long)(raw_.misses - base_.misses),
             (unsigned long)(raw_.conflicts - base_.conflicts), (unsigned long)(raw_.refreshes - base_.refreshes),
             (unsigned long)(raw_.ticks - base_.ticks), (unsigned long)(raw_.idle - base_.idle), (unsigned long)used,
             (unsigned long)(tot ? (mx * 1000 + tot / 2) / tot : 0));
}

#else
DramEpochReplay::DramEpochReplay(const std::string&, uint64_t, uint32_t, uint32_t, uint32_t, const char* name) { panic("DramEpochReplay %s: built without Ramulator", name); }
DramEpochReplay::~DramEpochReplay() {}
void DramEpochReplay::record(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint64_t, bool, uint32_t) {}
void DramEpochReplay::markRoi() {}
void DramEpochReplay::ctrlTotals(uint64_t&, uint64_t&, uint64_t&, uint64_t&) {}
uint64_t DramEpochReplay::composeAddr(uint32_t, uint32_t, uint64_t, uint64_t, uint32_t*) { return 0; }
uint32_t DramEpochReplay::serviceLatency(uint32_t, uint64_t) { return 0; }
void DramEpochReplay::initStats(AggregateStat*) {}
void DramEpochReplay::finalize() {}
void DramEpochReplay::replayEpoch(uint64_t) {}
void DramEpochReplay::tickTo(uint64_t) {}
void DramEpochReplay::tickOnce() {}
#endif
