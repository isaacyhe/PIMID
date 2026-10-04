/* dram_epoch_replay.h -- 1.11.98 (sweep-94 ruling 4 (c), user "go" 2026-10-03):
 * the live DRAM controller serves EVERY element type by EPOCH REPLAY.
 *
 * The weave cores (in-order, out-of-order) reach a RamulatorMemory through
 * their event recorders; the bound-only ALU and simple elements have no weave
 * phase, so a weave controller can never time their accesses, and their
 * memory side was served by the PE memory interface's analytical service
 * (zero-load latency + M/D/1 + a private open-row register per unit). Under
 * ruling 4 (c) every device DRAM request of an epoch -- (stamp, address,
 * read/write, interface) -- is replayed through ONE Ramulator2 instance per
 * device at the epoch boundary, in stamp order. The per-interface mean service
 * latency Ramulator measures (row hit / miss / conflict, queueing at the
 * target bank, refresh, write-to-read turnaround: the controller owns the
 * banks) prices the NEXT epoch's accesses of that interface, exactly as the
 * deterministic-epoch M/D/1 did (one-epoch lag, epochFrozenScalar discipline).
 *
 * Deterministic: the replay input is sorted by (stamp, interface, address,
 * type) so the thread arrival order inside an epoch cannot change it; the
 * Ramulator instance persists across epochs, so bank state and the refresh
 * schedule carry over. The DRAM clock advances monotonically on the stamp
 * axis (phaseStamp: the global phase clock plus the core's intra-phase
 * offset, the same axis the NoC batch replay uses).
 *
 * Measured, exported: requests replayed, epochs, mean latency, and the
 * controller's own row hit / miss / conflict totals (parsed from Ramulator's
 * statistics at the end of the run). The private open-row register of the
 * interface stays for its own statistic; it no longer prices anything when
 * the replay is on. */
#ifndef DRAM_EPOCH_REPLAY_H_
#define DRAM_EPOCH_REPLAY_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <atomic>
#include "locks.h"
#include "stats.h"

namespace Ramulator { class IFrontEnd; class IMemorySystem; }

class DramEpochReplay {
  public:
    DramEpochReplay(const std::string& configFile, uint64_t coreFreqHz,
                    uint32_t numInterfaces, uint32_t burstsPerLine, uint32_t lineBits,
                    const char* name);
    ~DramEpochReplay();

    /* Record one device DRAM access of epoch `epoch` (stamp on the phase axis)
     * at its TARGET bank: `unit` is the PIMID memory organisation the address
     * belongs to, `slot` the bank slot inside that unit and `row` the row, both
     * from the interface's own row model (PEMemoryInterface::rowOf_). */
    void record(uint64_t epoch, uint64_t stamp, uint32_t unit, uint32_t slot, uint64_t row,
                uint64_t lineAddr, bool isWrite, uint32_t mi);

    /* The service latency (core cycles, device time including queueing) that
     * interface `mi` charges in epoch `curEpoch`: the mean measured by
     * replaying epoch curEpoch-1 (replayed on demand, once). 0 = not yet
     * measurable (first epoch, or the interface had no access in the previous
     * epoch): the caller keeps its analytical service. */
    uint32_t serviceLatency(uint32_t mi, uint64_t curEpoch);

    void initStats(AggregateStat* parentStat);
    /* 1.11.98 (gate 1208A): open the statistics window at roi_begin, where the
     * traffic counters are rebased (1.11.90). Everything recorded so far is
     * pre-ROI and is replayed now (the bank state carries on); every exported
     * counter then reports the ROI only, like every other counter in the dump.
     * Called at each rebase; the last one opens the window. */
    void markRoi();
    /* Write the ROI-window counters (current totals minus the roi_begin
     * snapshot); called before every statistics dump. */
    void finalize();

    bool available() const { return sys_ != nullptr; }
    double dramTicksPerCoreCycle() const { return ticksPerCycle_; }

  private:
    struct Rec { uint64_t stamp; uint64_t addr; uint32_t mi; uint8_t wr; };
    struct Inflight { uint32_t remaining; uint64_t issueTick; uint32_t mi; bool wr; };

    void replayEpoch(uint64_t epoch);
    /* 1.11.98 (ruling 4 (c): "queueing at the target bank"): the Ramulator2
     * address whose ChRaBaRoCo decode is exactly (channel, the levels between
     * channel and row, row, column) of the request's target bank. */
    uint64_t composeAddr(uint32_t unit, uint32_t slot, uint64_t row, uint64_t lineAddr, uint32_t* chOut);
    std::vector<int> counts_;     // Ramulator level counts (channel first, column last)
    int rowIdx_ = -1, colIdx_ = -1;
    uint64_t colCount_ = 1;       // column addresses per row at the transfer granularity
    uint32_t txBits_ = 0;         // log2(bytes per column access)
    uint64_t banksPerChannel_ = 1;
    bool foldedChannels_ = false; // HBM: channels folded into chips_per_rank
    std::vector<uint64_t> perChan_;
    void tickTo(uint64_t tick);
    void tickOnce();

    std::string name_;
    Ramulator::IFrontEnd*     fe_  = nullptr;
    Ramulator::IMemorySystem* sys_ = nullptr;
    double   ticksPerCycle_ = 0.0;
    double   tickAcc_ = 0.0;
    uint64_t curTick_ = 0;
    uint32_t numMIs_ = 0;
    uint32_t burstsPerLine_ = 1;
    uint32_t lineBits_ = 6;

    lock_t lock_;
    std::map<uint64_t, std::vector<Rec>> pending_;           // epoch -> records
    std::map<uint64_t, std::vector<uint32_t>> frozen_;      // epoch -> per-MI mean latency (core cycles)
    uint64_t lastReplayed_ = 0; bool anyReplayed_ = false;

    // replay scratch
    std::vector<Inflight> inflight_;
    std::vector<uint64_t> sumLat_, cntLat_;                  // per-MI accumulators of one epoch
    std::vector<uint32_t> lastValue_;                        // per-MI last replayed service latency (carried across idle epochs)
    uint64_t outstanding_ = 0;

    // raw cumulative accumulators (under lock_; the priced/bootstrap pair is
    // atomic: notePriced is on every access) and their roi_begin snapshot
    struct Raw { uint64_t req = 0, rd = 0, wr = 0, lat = 0, epochs = 0, unserved = 0, ticks = 0, idle = 0,
                 hits = 0, misses = 0, conflicts = 0, refreshes = 0; };
    Raw raw_, base_;
    std::atomic<uint64_t> rawPriced_{0}, rawBoot_{0};
    uint64_t basePriced_ = 0, baseBoot_ = 0;
    bool roiMarked_ = false;
    void ctrlTotals(uint64_t& h, uint64_t& m, uint64_t& c, uint64_t& r);
    // exported (ROI window)
    Counter profRequests_, profEpochs_, profReads_, profWrites_;
    Counter profLatSum_;                 // sum of measured latencies (core cycles)
    Counter profUnserved_;               // requests still in flight at the drain bound
    Counter profRowHits_, profRowMisses_, profRowConflicts_, profRefreshes_;
    Counter profTicks_, profIdleTicks_;   // DRAM cycles replayed, and those with no request in flight (row 28: idle cycles)
    Counter profChannelsUsed_, profMaxChanPermille_;   // channels that received requests; the busiest channel's share (per mille)
    Counter profPriced_, profBootstrap_;  // accesses priced by a replay value / by the analytical fallback
  public:
    void notePriced(bool byReplay) { if (byReplay) rawPriced_.fetch_add(1, std::memory_order_relaxed); else rawBoot_.fetch_add(1, std::memory_order_relaxed); }
};

#endif  // DRAM_EPOCH_REPLAY_H_
