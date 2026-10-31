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
 * the replay is on.
 *
 * 1.12.18 (ruling 17 / R0605, user: "make the power model complete"): the HOST
 * memory is replayed the same way. The host memory controller (SimpleMemory /
 * WeaveSimpleMemory) records every host access through hostAccess() and
 * prices it at the replay's measured service latency of its own interface
 * (plus the calibrated host-path adder, which holds no DRAM timing); the
 * M/D/1 queue and the tRCD+tCAS zero-load latency serve only the bootstrap.
 * Coupled systems (the device IS the host's memory): the host interfaces join
 * the DEVICE instance, numbered after the device interfaces, so one controller
 * sees all traffic on the shared array; their requests carry Ramulator
 * source_id 1, and the controller's row hits / misses / conflicts and
 * activates are split by origin (pimid_src_totals). A host access lands on the
 * bank the device's own placement gives that line (setHostTarget). Decoupled
 * systems: a host-only instance (hostOnly) of the host memory's technology,
 * with that memory's channel count, one rank, and Ramulator's own address
 * mapper from the YAML. */
#ifndef DRAM_EPOCH_REPLAY_H_
#define DRAM_EPOCH_REPLAY_H_

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <atomic>
#include <deque>
#include <unordered_map>
#include <memory>
#include "locks.h"
#include "stats.h"

namespace Ramulator { class IFrontEnd; class IMemorySystem; class IDRAM; }

class DramEpochReplay {
  public:
    DramEpochReplay(const std::string& configFile, uint64_t coreFreqHz,
                    uint32_t numInterfaces, uint32_t burstsPerLine, uint32_t lineBits,
                    const char* name, bool hostOnly = false, uint32_t hostChannels = 0);   // 1.12.18 (R0605): hostOnly = the decoupled host memory

    /* 1.12.18 (ruling 17 / R0605): add one HOST memory interface (a host
     * memory controller) clocked at hostFreqHz; returns its interface number,
     * after every device interface. Called at construction time, before any
     * access. Its service latency is returned in HOST cycles. */
    uint32_t addHostInterface(uint64_t hostFreqHz);
    /* 1.12.18: the bank a host access lands on in a coupled system -- the
     * device's own placement of that line (unit, bank slot, row), so the host
     * and the device see one array. Not used by a hostOnly instance. */
    void setHostTarget(std::function<void(uint64_t lineAddr, uint32_t& unit, uint32_t& slot, uint64_t& row)> f) { hostTarget_ = f; }
    /* 1.12.18: record one host access (host-clock cycle `cycle`, core srcId)
     * on interface `mi` and return its service latency in host cycles: the
     * previous epoch's measured mean of that interface; 0 = not yet measured
     * (the caller keeps its analytical bootstrap). Same epoch, stamp and cut
     * discipline as the device interfaces (PEMemoryInterface). */
    /* 1.12.18: adjustedClock = `cycle` is the core's wall-free clock
     * (Core::getCycles(), as the coherence flush passes it), not the raw
     * curCycle a memory request carries. */
    uint32_t hostAccess(uint32_t mi, uint64_t lineAddr, uint64_t cycle, uint32_t srcId, bool isWrite,
                        bool adjustedClock = false);
    /* A host core's L2 prefetcher. setHostPrefetch: the host access recorded next (this thread) is a prefetch -- no
     * core waits for it (not a hold, never reconciled to a core); in the closed loop it holds one of its core's L2
     * superqueue entries (L2StreamPrefetcher::superqueueOf) until it completes. setPrefetchTag: its reconciliation tag
     * (unique per core; 0 clears). prefetchConsumed: a request of `core` found the prefetched line (issued at
     * `issueCycle`) and waited from `waitStart` until its estimated arrival `estArrival` (host cycles; no wait when
     * it had arrived); once the prefetch's replayed completion is known, the core's balance takes the change of its
     * stall on prefetches (see pfDrain). prefetchDropped: the line left the prefetcher unconsumed. */
    static void setHostPrefetch(bool on);
    static void setPrefetchTag(uint32_t tag);
    static void prefetchConsumed(uint32_t tag, uint32_t core, uint64_t waitStart, uint64_t estArrival, uint64_t issueCycle);
    static void prefetchDropped(uint32_t tag, uint32_t core);
    /* 1.12.18 (user ruling R1 2026-10-24): the request counters of interface
     * `mi` (a PE memory interface's rd/wr, a host memory controller's rd/wr).
     * A request the replay finds stamped after the ROI end is still replayed
     * (the controller state stays correct) but is taken out of these
     * counters, as out of every replayed ROI total, and reported apart. */
    /* (1.12.153, user ruling 2026-10-28: the message-passing and device latency gap): a device interface also binds
     * its charged-latency counters (replayPriced / replayLat); an access priced at the replay and found outside the
     * ROI leaves them as it leaves rd/wr, so the charged view and the replay's measured view (latencySum /
     * ctrlRequests, ROI only) count the same requests. */
    void bindCounters(uint32_t mi, Counter* rd, Counter* wr, Counter* priced = nullptr, Counter* chargedLat = nullptr);
    /* (1.12.147, user ruling 2026-10-28): the requesters of this instance's host
     * interfaces are the DEVICE's elements (a memory of type Ramulator in
     * device scope: HOST_MC placement, the elements share the host memory
     * controller). Under the single-process cut their records are stamped on
     * the axis the cut folds on and the device interfaces stamp on
     * (PEMemoryInterface::cutRoiRel), and an in-order / OOO element's latest
     * stamp joins its ROI end (clkNoteRec), as at a device interface. Off
     * (every host memory before the ruling): unchanged. */
    void setDeviceRequesters(bool on) { devRequesters_ = on; }
    bool hostOnly() const { return hostOnly_; }
    bool hasHost() const { return hostMis_ > 0; }
    ~DramEpochReplay();

    /* Record one device DRAM access of epoch `epoch` (stamp on the phase axis)
     * at its TARGET bank: `unit` is the PIMID memory organisation the address
     * belongs to, `slot` the bank slot inside that unit and `row` the row, both
     * from the interface's own row model (PEMemoryInterface::rowOf_). */
    /* (1.12.189): a PE's scratchpad DMA transfer starts (true) / ends (false) on the calling thread: the lines it records
     * in between are one transfer's */
    static void dmaBurst(bool on);
    void noteSync(uint32_t core);   // (1.12.189): a message-passing wait just placed the core's clock
    void record(uint64_t epoch, uint64_t stamp, uint32_t unit, uint32_t slot, uint64_t row,
                uint64_t lineAddr, bool isWrite, uint32_t mi, uint32_t charged = 0, bool tail = false, uint32_t core = ~0u, bool holds = true);   // (1.12.153): charged = the latency the access was priced at (0: bootstrap); (1.12.181): tail = issued in a co-sim MPI rank's post-ROI tail; (1.12.189): core = the core that issued it, holds = it waits for the access (not a writeback)

    /* The service latency (core cycles, device time including queueing) that
     * interface `mi` charges in epoch `curEpoch`: the mean measured by
     * replaying epoch curEpoch-1 (replayed on demand, once). 0 = not yet
     * measurable (first epoch, or the interface had no access in the previous
     * epoch): the caller keeps its analytical service. */
    uint32_t serviceLatency(uint32_t mi, uint64_t curEpoch, bool write = false, bool pf = false);   // (1.12.189): write = a store's price (its interface's writes)

    /* 1.12.17 (user 2026-10-22: "we need message passing side measured as
     * well"): the exec-method message-passing mode (thread-MPI: every rank a
     * thread of one process). The global phase clock keeps advancing while
     * ranks sit parked in their MPI waits, so stamping on it replayed mostly
     * dead time (gate 1208A: 317 M DRAM cycles inside a ~6 M ROI). In this
     * mode a record is stamped on the recording core's ROI-relative cycle --
     * the axis the thread-MPI NoC pricing uses -- and keyed by that stamp's
     * epoch; an epoch is replayed only by foldByCut, at the phase barrier, once
     * the consistent cut (the minimum ROI-relative cycle over the active cores)
     * has passed its end; never on an access path. Set at construction from
     * the same environment test as PEMemoryInterface::mpiThreadDetPricing(). */
    bool cutMode() const { return cutMode_; }
    void foldByCut(uint64_t cut, uint64_t epochLen);
    /* The service latency interface `mi` charges for an access in epoch
     * `epoch` (cut mode): the value folded for epoch - lag, carried over the
     * epochs in which `mi` had no access; 0 = nothing folded yet (the caller
     * keeps its analytical service). */
    uint32_t serviceLatencyCut(uint32_t mi, uint64_t epoch, uint64_t lag, bool write = false, bool pf = false);   // (1.12.189): write = a store's price

    /* (1.12.145, user ruling 2026-10-28: the multi-core latency gap) the replay's own epoch length in phases,
     * simulation.dram_epoch_phases (handed over as PIMID_DRAM_EPOCH_PHASES; default kDramEpochPhases),
     * separate from the NoC's det_epoch_phases. It was det_epoch_phases (4 phases = 40 000 cycles): a
     * price two such epochs old let the cores of a loaded controller issue in bursts the controller took
     * several epochs to serve. The read lag stays two epochs (kDramReadLagEpochs). */
    static constexpr uint32_t kDramEpochPhases = 1;
    static constexpr uint64_t kDramReadLagEpochs = 2;
    /* (1.12.153, user ruling 2026-10-28: the message-passing and device latency gap) the price is the request-weighted
     * mean latency over the epoch read (final, or in flight with its unfinished requests at their age) and the
     * kDramPriceWindow - 1 finalized epochs before it, simulation.dram_price_window (handed over as
     * PIMID_DRAM_PRICE_WINDOW; 1 = the epoch read alone, as before). A price read two epochs back from a single
     * epoch let a bursty load lock into a limit cycle of period two lags: a burst epoch priced at the quiet one
     * before it, the quiet epoch after it at the burst (device spmv_csr OpenMP: charged / measured 0.32). */
    static constexpr uint32_t kDramPriceWindow = 8;
    static uint32_t priceWindow() {
        static const uint32_t w = [] {
            const char* v = getenv("PIMID_DRAM_PRICE_WINDOW");
            const int n = v ? atoi(v) : 0;
            return n > 0 ? (uint32_t)n : kDramPriceWindow;
        }();
        return w;
    }
    /* (next, ruling 2026-10-30 (4)): each epoch of the price window weighs its mean latency by its request count, and
     * a read and a write are priced apart, each from its interface's requests of its own kind (a slot per interface
     * and kind, priceSlot). The reconciliation corrects what the price misses; of the windows measured against what
     * the closed-loop replay then measured (count^2 over every request, count over every request, count over the
     * reads), the reads alone by count erred least on reads (host 269 / 314 / 324 cycles mean |measured - charged|,
     * device 11.4 / 12.6 / 12.4), and a store priced at the read window was over-charged 6x (29.5 vs 5.1 cycles). */
    static constexpr uint32_t kPriceKinds = 3;   // (1.12.189): a read a core waits for, a write, a prefetch's read
    static uint32_t priceSlot(uint32_t mi, bool write, bool pf = false) { return kPriceKinds * mi + (pf ? 2 : (write ? 1 : 0)); }
    static uint32_t epochPhases() {
        static const uint32_t e = [] {
            const char* v = getenv("PIMID_DRAM_EPOCH_PHASES");
            const int n = v ? atoi(v) : 0;
            return n > 0 ? (uint32_t)n : kDramEpochPhases;
        }();
        return e;
    }
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
    /* (1.12.189) the closed-loop replay and the reconciliation. The bound phase charges each access an estimate (the price
     * window below; before the first replayed epoch the analytical service). The replay then times the access as the
     * core issues it: a core's accesses reach the controller in the core's order, at the core's stamp plus its
     * unapplied shift (the sum of measured - charged of its completed accesses, less what was applied to its clock),
     * and no sooner than the core can issue them: a core that waits for each access (ALU, in-order) has one in
     * flight, an out-of-order core as many as its record's miss-handling capacity (Core::memMissCapacity). Each
     * completed access the core waited for (a read, a store that holds the core; not a writeback) adds measured -
     * charged to the core's balance, divided by the core's overlap in that epoch for an out-of-order core (the sum of
     * the charges over the length of the union of the charged intervals). At the phase barrier, after the folds,
     * reconcileAtBarrier applies the whole cycles of a positive balance to every core canTake(core) accepts
     * (Core::addDelay); a negative balance (charged more than measured) is refunded: it lowers the core's next
     * charges (chargeLast), never below zero. */
    void reconcileAtBarrier(const std::function<bool(uint32_t)>& canTake);
    /* (next, ruling 2026-10-30 (4)) the core enters a message-passing operation: its balance is applied now, from its
     * own thread, so the clock its partners see carries it -- a positive balance as a delay, an over-charge (a
     * negative balance) as a rewind of at most maxRewind cycles (the caller bounds it by the core's last
     * synchronisation and the replay's cut). Returns the cycles applied (negative: rewound). */
    int64_t reconcileCore(uint32_t core, uint64_t maxRewind);
    /* (next, ruling 2026-10-30 (3)) the run's end: markRunEnd when the run time is taken (what was applied to each
     * core so far is in it); settleFinal replays what is pending and drains the controller (finalize's first steps);
     * owedEnd(core) is then the core's balance the run time did not see, which extends its end. finalize re-arms the
     * replay's close when the run time was raised (the drain keeps its periodic snapshots for that). */
    void markRunEnd();
    void settleFinal();
    double owedEnd(uint32_t core);
    bool heldEnd(uint32_t core, uint64_t end, double& out);   // (1.12.189): a core's end by its last ROI access it waited for
    /* (1.12.189) the charge of the access this thread recorded last: `lat` (the replay's price, or with boot the
     * analytical service's, which the record then carries), less the refund the core has available; unchanged when
     * the access was not recorded; a refund leaves at least `floor`. lastRecorded: the access was recorded. */
    uint32_t chargeLast(uint32_t lat, bool boot, uint32_t floor = 0);
    bool lastRecorded() const;

    bool available() const { return sys_ != nullptr; }
    double dramTicksPerCoreCycle() const { return ticksPerCycle_; }

  private:
    struct Rec { uint64_t stamp; uint64_t addr; uint32_t mi; uint8_t wr; uint32_t ch; uint8_t shadow = 0; uint32_t grp = 0;
                 uint32_t src = ~0u, chg = 0; uint8_t tail = 0; uint32_t core = ~0u, ref = 0; int32_t dly = 0; uint8_t hold = 0, boot = 0; float refT = 0.0f; uint32_t dmab = 0; uint8_t pf = 0; uint32_t pfTag = 0;   /* pf: issued by a host core's L2 prefetcher (no core waits for it); pfTag: its reconciliation tag (0: none) */ uint8_t sync = 0; };   // (1.12.189): sync = the core's first record after a synchronization   // (1.12.189): dmab = the DMA transfer the line belongs to (0: none)   // (1.12.189): core = the core that issued it, ref = the refund its charge took (refT: in the core's time, ref / its overlap), dly = the delay applied to the core before it, hold = the core waits for it, boot = charged by the analytical service; (1.12.181): tail = issued in a co-sim MPI rank's post-ROI tail (outside)   // (1.12.153): src = the host core of a thread-MPI host record (~0u otherwise); chg = the latency charged   // 1.12.18 (drain the lag): ch = the controller channel; shadow = a host lock-step copy past chip 0, grp = its host request
    struct Inflight { uint32_t remaining; uint64_t issueTick; uint32_t mi; bool wr; bool out; uint64_t epoch; uint32_t grp = 0; uint32_t chg = 0; uint32_t core = ~0u; bool hold = false, boot = false, gated = false; uint32_t ref = 0; bool posted = false; uint32_t dmab = 0; bool pfg = false; uint32_t pfTag = 0;   /* pfg: a prefetch counted in its core's superqueue */ bool accp = false; bool pfr = false; };   // (1.12.189): pfr = a prefetch's request; accp = priced at its acceptance   // (1.12.189): core, hold, boot, ref, posted (its latency noted at acceptance); gated = counted in its core's in-flight accesses; (1.12.153): chg   // (1.12.145): issueTick = the tick the request reached the controller's door   // 1.12.18: out = stamped after the ROI end (R1); grp
    /* 1.12.18 (user ruling 2026-10-24, "drain the lag"): a record that finds
     * its controller queue full waits at that queue's door (FIFO per channel
     * and direction) and enters the instant the controller can take it; an
     * epoch's requests stay in flight across the epoch boundary, and its
     * measured service latency is final when its last request completes. */
    struct Waiting { Rec r; uint64_t epoch; uint64_t target; bool out; uint32_t b; uint64_t id; uint64_t feed = 0; bool gated = false; bool pfg = false; };   // (1.12.145): feed = the tick the record reached the door
    /* (1.12.189): per core, an epoch's accesses it waits for: the sum of their charges and the length of the union of their
     * charged intervals [stamp, stamp + charge] on the core's clock (their overlap) */
    struct RecAcc { double chg = 0.0, uni = 0.0; bool host = false; };
    struct EpochAcc { std::vector<uint64_t> sum, cnt; uint64_t pend = 0; bool fed = false; std::map<uint32_t, RecAcc> rec; };
    std::map<uint32_t, std::deque<Waiting>> doors_;
    uint64_t waitingN_ = 0, nextId_ = 0;
    std::map<uint64_t, EpochAcc> acc_;
    /* (1.12.189): the closed-loop replay and the reconciliation, see reconcileAtBarrier */
    struct CoreCL {
        bool on = false, ooo = false, host = false;
        uint32_t cap = 1, out = 0;            // miss-handling capacity; its accesses in flight (holding)
        uint32_t id = ~0u;                    // the core
        uint32_t sqCap = 0, pfOut = 0;        // its L2 superqueue (0: no L2 prefetcher); its prefetches in flight
        std::deque<Waiting> fifo;             // its records not released yet, in its order
        double diff = 0.0;                    // sum of (measured - charged) / overlap of its completed accesses
        double refSeen = 0.0, dlySeen = 0.0;  // refunds / delays carried by its records released so far
        int64_t dlyTot = 0, pendDly = 0;      // delays applied (rewinds negative); applied and not carried by a record yet
        int64_t dlyAtEnd = 0, usedAtEnd = 0;  // (1.12.189): delays and refunds (1/1024 cycles) applied when the run time was taken
        double ovC = 1.0;                     // its latest overlap (an out-of-order core; 1 otherwise): a refund's charge cycles per cycle of its time
        uint32_t dmaOpen = 0;                 // (1.12.189): the DMA transfer whose lines are in flight (0: none)
        uint64_t dmaComp = 0;                 //   its latest line completion (DRAM tick)
        double dmaChgEnd = 0.0, dmaTpc = 1.0; //   the latest charged end of its lines (tick), ticks per cycle of its interface
        uint64_t lastDoneT = 0;               // (1.12.189) a blocking core: the tick its last access (or DMA transfer) completed,
        double lastChgEndS = 0.0, pendChgEndS = 0.0;   // that access's charged end on its own clock, and the one in flight's
        double dmaT0 = 0.0; uint64_t dmaS0 = 0;        // (1.12.189) the DMA transfer's first line: its release tick, its stamp
        uint64_t roiDoneT = 0; double roiChgEndS = 0.0; int64_t pendAtEnd = 0;   // (1.12.189) the same for its last ROI access; delays not carried at the run end
    };
    std::map<uint32_t, CoreCL> cl_;
    std::multimap<std::pair<uint64_t, uint64_t>, Waiting> rel_;   // (tick, order) -> a released record, fed at that tick
    uint64_t relSeq_ = 0, clHeld_ = 0;
    static constexpr uint32_t kRecCores = 4096;
    std::unique_ptr<std::atomic<int64_t>[]> pool_, used_;   // per core: refund available (charge cycles); refunds taken (its time, 1/1024 cycles)
    std::unique_ptr<std::atomic<double>[]> ovA_;              // per core: ovC, for the refund at the access
    std::unique_ptr<std::atomic<uint8_t>[]> syncP_;           // (1.12.189) per core: synchronized since its last record
    bool clOn() const;
    CoreCL* clCore(uint32_t core);
    void clRelease(CoreCL& k);
    void clQueue(const Waiting& w, uint64_t tick);
    void serveRel();
    void recFeed(EpochAcc& acc, const std::vector<Rec>& recs);
    void recDone(std::map<uint64_t, EpochAcc>::iterator a, uint32_t core, uint32_t mi, bool wr, bool hold, bool out, bool boot,
                 uint64_t meas, uint32_t chg, uint64_t doneTick = 0, uint32_t dmab = 0);
    void recDmaClose(CoreCL& k);   // (1.12.189)
    void pfApply(uint32_t core, int64_t corr);   // consumed prefetches' settled waits into the consumer's balance (under lock_)
    double recDma_ = 0.0; uint64_t recDmaN_ = 0;
    void recSettle(EpochAcc& acc);
    void recReport();
    static constexpr uint64_t kRecHist = 65536;
    struct RecStat { uint64_t n = 0, absSum = 0, chg = 0, meas = 0; int64_t sum = 0; std::vector<uint64_t> hist; };
    RecStat recSt_[3][2];   // [device read, host read, device store that holds its PE][replay-priced, bootstrap] (since roi_begin)
    double recCredit_[3] = {0.0, 0.0, 0.0}, recOvChg_[2] = {0.0, 0.0}, recOvUni_[2] = {0.0, 0.0};
    uint64_t recApplied_ = 0, recDelays_ = 0, recBarriers_ = 0, recRefund_ = 0, recAppliedCls_[2] = {0, 0};
    uint64_t recCommDelay_ = 0, recCommRewind_ = 0, recComms_ = 0;   // (1.12.189): applied at message-passing entry
    bool runEndMarked_ = false, keepHist_ = false, settled_ = false; uint64_t settleRun_ = 0;
    double clShiftSum_ = 0.0; uint64_t clShiftN_ = 0;
    /* the closed loop holds a core's records until it can issue them, so an epoch is final only when its last held
     * record completes: per interface, the mean of the requests that completed in the latest whole epoch of the
     * replay's own clock, the price when the epochs the window reads have no measured request of it yet */
    struct CompBin { uint64_t bin = ~0ull, sum = 0, cnt = 0; uint32_t last = 0; };
    std::vector<CompBin> compBin_;
    void compNote(uint32_t slot, uint64_t latCyc);
    Counter profRecReads_, profRecCycles_, profRecRefund_, profRecPending_;
    Counter profRecCharged_, profRecPendingMax_, profRoiTail_, profRoiDrain_;   // (1.12.189): for the checker
    uint64_t recChgSum_ = 0, lastRoiDone_ = 0, lastHeldDone_ = 0;   // (1.12.189): the ROI's last completion; of an access a core waited for
    bool acceptOne(Waiting& w);
    void serveDoors();
    void finalizeEpoch(uint64_t epoch);
    uint32_t partialMean(uint64_t e, uint32_t slot);   // 1.12.18; (1.12.189): a price slot (priceSlot)
    /* (1.12.145, user ruling 2026-10-28): the partial mean of an epoch still in flight counts its unfinished
     * requests (in the controller or waiting at its door) at their age so far; cached per controller tick. */
    struct PartialCache { uint64_t tick = ~0ull; std::vector<uint64_t> sum, cnt; };
    std::map<uint64_t, PartialCache> pcache_;
    uint64_t partialAged_ = 0;                       // reads whose partial mean included unfinished requests

    uint64_t partial_ = 0;                           // 1.12.18: reads served a partial mean
    void drainAll();
    uint64_t doorRecs_ = 0, doorTicks_ = 0, doorMax_ = 0; bool doorReported_ = false;
    /* 1.12.18 (R0605): one record at a composed Ramulator address */
    void recordAddr(uint64_t epoch, uint64_t stamp, uint64_t addr, uint32_t ch, bool isWrite, uint32_t mi, uint8_t shadow = 0, uint32_t grp = 0);
    /* mpiHost_: the replay of a thread-MPI run (cut mode). Its host records are stamped on the ROI-relative phase
     * grid (hostAccess), so they never fall behind the replay clock, and a host read takes epoch k - lag like every
     * cut-mode read. */
    bool mpiHost_ = false;
    std::map<uint64_t, std::vector<uint64_t>> winSum_, winCnt_;   // (1.12.153): finalized epochs' sums and counts (cut mode) for the price window
    std::vector<Counter*> miPriced_, miChgLat_;                     // (1.12.153): bound charged-latency counters
    std::vector<uint64_t> outPriced_, outChgLat_, outPricedApplied_, outChgLatApplied_;
    bool isHostMi(uint32_t mi) const { return mi >= hostMi0_ && mi < hostMi0_ + hostMis_; }
    bool hostOnly_ = false;
    bool devRequesters_ = false;   // (1.12.147, user ruling 2026-10-28): see setDeviceRequesters
    uint32_t hostMi0_ = ~0u, hostMis_ = 0;     // host interfaces [hostMi0_, hostMi0_ + hostMis_)
    std::vector<double> miTicksPerCycle_;     // DRAM ticks per cycle of each interface's own clock
    std::function<void(uint64_t, uint32_t&, uint32_t&, uint64_t&)> hostTarget_;
    bool hostRoBaRaCoCh_ = false;             // hostOnly: the YAML maps RoBaRaCoCh (channel = low bits above the column access)

    void replayEpoch(uint64_t epoch);
    /* 1.11.98 (ruling 4 (c): "queueing at the target bank"): the Ramulator2
     * address whose ChRaBaRoCo decode is exactly (channel, the levels between
     * channel and row, row, column) of the request's target bank. */
    uint64_t composeAddr(uint32_t unit, uint32_t slot, uint64_t row, uint64_t lineAddr, uint32_t* chOut, int chipSel = -1);
    /* 1.12.18 (user ruling 2026-10-24): PE placement at or below the CHIP tier on a rank-level DRAM whose
     * chips are not folded into channels: every chip is its own Ramulator rank (own banks, rows, tRRD/tFAW);
     * a host request is replayed once per chip (lock step). */
    bool chipSplit_ = false;
    uint32_t chips_ = 1, hostRowSlots_ = 1, nextGrp_ = 0;
    bool chipReported_ = false;
    bool hostDataBus_ = false, hdbReported_ = false;   // (1.12.169): the host data bus (bank-parallel DDR5) and its report line
    std::map<uint32_t, std::pair<uint32_t, uint64_t>> grpLat_;   // host request -> (copies outstanding, slowest latency)
    std::vector<int> counts_;     // Ramulator level counts (channel first, column last)
    int rowIdx_ = -1, colIdx_ = -1;
    uint64_t colCount_ = 1;       // column addresses per row at the transfer granularity
    uint32_t txBits_ = 0;         // log2(bytes per column access)
    uint64_t banksPerChannel_ = 1;
    bool foldedChannels_ = false; // HBM: channels folded into chips_per_rank
    std::vector<uint64_t> perChan_;
    void tickTo(uint64_t tick);
    void tickOnce();

    /* (1.12.144): host speed -- the per-channel replay. A device whose replay organisation has exactly two channels
     * (ChRaBaRoCo: the channel is the top address field) is replayed in cut mode as two one-channel Ramulator2
     * instances, one per channel, ticked on two host threads (the calling thread and one helper) through every
     * replay call; the two-channel instance above keeps the organisation (composeAddr, tCK, counts) and is never
     * ticked. The channels of one Ramulator2 memory system share nothing that times a command (each controller
     * drives its own channel's nodes; the DRAM's future actions and bank-open sums are per channel), so each
     * one-channel instance goes through the states its channel went through, cycle for cycle. What the replay
     * itself shares between the channels is merged after each call in the order the single instance produced it:
     *  - request completions (epoch accounting, epoch finalisation, latency sums), by (DRAM tick, channel, order
     *    inside the channel): the single instance ticked channel 0's controller before channel 1's;
     *  - the idle-tick count (no request in flight on ANY channel), from each channel's idle state per tick;
     *  - the periodic and the ROI-end snapshots (the two channels' sums at the same tick, the merged idle count);
     *  - the request and door counters (sums; the door wait maximum).
     * Request ids are given in record order before the records are split. PIMID_REPLAY_SERIAL=1 keeps the single
     * instance. */
    struct ChanEv { uint64_t tick; uint32_t seq; uint64_t epoch; uint32_t mi; uint64_t latCyc; bool account, out, host; uint32_t chg; bool wr, hold, boot; uint32_t core, ref; };   // (1.12.153): chg; (1.12.189): wr, hold, boot, core, ref
    struct ChanSnap { uint64_t tick = 0, refreshes = 0, bankW = 0, bankO = 0, pdC = 0, pdX = 0; };
    struct ChanWork { uint64_t target; Waiting w; };
    struct Chan {
        Ramulator::IFrontEnd* fe = nullptr;
        Ramulator::IMemorySystem* sys = nullptr;
        uint64_t addrOffset = 0;      // the channel's base in the two-channel address
        std::map<uint32_t, std::deque<Waiting>> doors;
        uint64_t waitingN = 0, outstanding = 0, curTick = 0;
        std::unordered_map<uint64_t, Inflight> inflight;
        std::map<uint32_t, std::pair<uint32_t, uint64_t>> grpLat;
        // per call
        std::vector<ChanEv> evs; uint32_t seq = 0; uint64_t evTick = ~0ull;
        std::vector<uint64_t> idleFlips;   // ticks at which this channel's idle state changes
        bool idleAtStart = true, idleNow = true;
        std::vector<ChanSnap> snaps; bool closeTaken = false; ChanSnap closeSnap;
        bool closeSnappedIn = false;
        struct Cnt { uint64_t req = 0, rd = 0, wr = 0, hreq = 0, hrd = 0, hwr = 0, oreq = 0, ord = 0, owr = 0, ohreq = 0, ohrd = 0, ohwr = 0; };
        Cnt d;                        // request counters of this call
        uint64_t doorRecs = 0, doorTicks = 0, doorMax = 0;
        uint64_t drained = 0;         // drainAll: ticks this channel needed
        std::vector<uint64_t> outRd, outWr;   // outside requests per interface, this call
    };
    std::vector<Chan> chans_;         // empty = the single-instance path
    std::function<void(Ramulator::IDRAM*)> stripCons_;   // the bank-parallel constraint filter, for the channel instances
    bool par() const { return chans_.size() == 2; }
    void chanTotals(uint64_t& h, uint64_t& m, uint64_t& c, uint64_t& r) const;
    bool chanAccept(Chan& C, Waiting& w);
    void chanServeDoors(Chan& C);
    void chanTickOnce(Chan& C);
    void chanTickTo(Chan& C, uint64_t tick);
    void chanTakeSnap(Chan& C, ChanSnap& s) const;
    void chanBegin();
    void chanMerge(uint64_t startTick, uint64_t endTick);
    void parRun(const std::function<void(Chan&)>& f);
    void replayEpochPar(uint64_t epoch);
    void tickToPar(uint64_t tick);
    void drainAllPar();
    struct ParHelper;
    ParHelper* helper_ = nullptr;

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
    // 1.12.17 cut mode (message passing): see cutMode()
    bool cutMode_ = false;
    uint64_t roiTick0_ = 0;                                  // DRAM tick of the ROI-relative origin (set at markRoi)
    uint64_t lastFolded_ = 0; bool anyFolded_ = false;
    std::map<uint64_t, std::vector<uint32_t>> carried_;     // epoch -> per-MI latency, idle interfaces carried
    uint64_t late_ = 0, stale_ = 0;
                          // records re-keyed past the cut; reads ahead of the cut

    // replay scratch
    std::unordered_map<uint64_t, Inflight> inflight_;   // 1.12.18 (drain the lag): by request id, across epochs
    std::vector<uint64_t> sumLat_, cntLat_;                  // per-MI accumulators of one epoch
    std::vector<uint32_t> lastValue_;                        // per-MI last replayed service latency (carried across idle epochs)
    uint64_t outstanding_ = 0;

    // raw cumulative accumulators (under lock_; the priced/bootstrap pair is
    // atomic: notePriced is on every access) and their roi_begin snapshot
    struct Raw { uint64_t req = 0, rd = 0, wr = 0, lat = 0, epochs = 0, unserved = 0, ticks = 0, idle = 0,
                 hits = 0, misses = 0, conflicts = 0, refreshes = 0,
                 bankW = 0, bankO = 0, acts = 0,     // 1.12.18: bank-open unit cycles (window, open)
                 pdC = 0, pdU = 0, pdX = 0,          // 1.12.20: power-down unit-cycles, units, exits
                 hreq = 0, hrd = 0, hwr = 0, hlat = 0, hrlat = 0,            // 1.12.18 (R0605): the host interfaces' share; (1.12.189): hrlat = the reads'
                 hhits = 0, hmisses = 0, hconflicts = 0, hacts = 0,         //   (source_id 1 in the controller)
                 /* 1.12.18 (user ruling R1 2026-10-24): requests stamped after
                  * the ROI end -- replayed, never priced (source_id 2 device,
                  * 3 host in the controller) */
                 oreq = 0, ord = 0, owr = 0, olat = 0, ohreq = 0, ohrd = 0, ohwr = 0,
                 ohits = 0, omisses = 0, oconflicts = 0, oacts = 0, ohacts = 0; };
    Raw raw_, base_;
    std::atomic<uint64_t> rawPriced_{0}, rawBoot_{0};
    uint64_t basePriced_ = 0, baseBoot_ = 0;
    std::atomic<uint64_t> rawHPriced_{0}, rawHBoot_{0};   // 1.12.18 (R0605): host accesses priced by the replay / by the bootstrap
    uint64_t baseHPriced_ = 0, baseHBoot_ = 0;
    uint64_t hLatMin_ = ~0ull, hLatMax_ = 0;   // 1.12.18: the window's shortest / longest host request (host cycles), PIMID_DEBUG_REPLAY only
    bool roiMarked_ = false;
    bool closeReported_ = false;   // 1.12.18: the ROI-end close is reported once
    /* 1.12.18 (shift by lag): the window's time-based counters every
     * kSnapEvery DRAM cycles until the close is armed, so a close the clock
     * has already passed takes its counters at the close, not at "now". */
    struct Snap { uint64_t tick = 0, ticks = 0, idle = 0, refreshes = 0, bankW = 0, bankO = 0, pdC = 0, pdX = 0; };   // pd*: 1.12.19
    static constexpr uint64_t kSnapEvery = 1024;
    std::vector<Snap> hist_;
    void takeSnap(Snap& s);
    void applySnap(const Snap& s);
    /* 1.12.18: cut mode (message passing) -- the window's time-based counters
     * (DRAM ticks, idle ticks, refreshes, bank-open time) are frozen when the
     * replay clock reaches the ROI end (roiTick0_ + the run time's mpiEnd).
     * Records stamped past it (a co-sim rank's post-kernel tail on its device
     * PE) are still replayed and counted as requests, reads, writes and row
     * events, but the DRAM time they extend the replay by is outside the
     * priced window, as it is outside the run time. */
    /* 1.12.18 (R1, B4): every mode. Cut mode closes at roiTick0_ + mpiEnd
     * (the run time's clock); phase mode at the pricing window's end -- the
     * roi_begin stamp plus the run time raised to the busy max as the power
     * model raises it (phaseWindowCycles) -- armed in finalize, when the busy
     * counts are final. Requests stamped past the close are "outside". */
    void armClose(bool fromFinalize);
    void snapClose();
    uint64_t phaseWindowCycles(std::string* why, bool withRunTime = true);   // withRunTime false = the busy max alone
    double runTicksPerCycle() const;   // DRAM ticks per cycle of the clock roiWall.mpiEnd is counted in
    uint64_t coreFreqHz_ = 0;
    double tckNs_ = 0.0;
    std::vector<Counter*> miRd_, miWr_;                       // bound request counters, per interface
    std::vector<uint64_t> outRd_, outWr_, outRdApplied_, outWrApplied_;   // outside requests per interface; already taken out
    void applyOutside();
    bool closeArmed_ = false, closeSnapped_ = false;
    uint64_t closeTick_ = 0;        // DRAM tick of the ROI end (cut mode)
    uint64_t closeLate_ = 0;        // ticks the clock already stood past the end when the end became known
    uint64_t pastEndRecs_ = 0;      // records issued at a stamp past the ROI end (counted as requests)
    uint64_t snapTicks_ = 0, snapIdle_ = 0, snapRefreshes_ = 0, snapBankW_ = 0, snapBankO_ = 0;
    uint64_t snapPdC_ = 0, snapPdX_ = 0;   // 1.12.19
    bool pdOn_ = false;
    Counter profPdCycles_, profPdUnits_, profPdExits_;
    std::atomic<uint64_t> hostCoreStamps_{0};    // 1.12.18 (R2): host records stamped on the host core's own ROI-relative clock
    std::atomic<uint64_t> hostPhaseStamps_{0};   // 1.12.18: host records stamped on the ROI-relative phase grid (no core ROI baseline)
    void ctrlTotals(uint64_t& h, uint64_t& m, uint64_t& c, uint64_t& r);
    /* 1.12.18 (ruling R8-7 b / 4 c): the replayed device's bank-open time,
     * kept by Ramulator2 (dram/pimid_bank_open.h); exported only when the
     * device tracks it, so an untracked device never reads as measured 0. */
    bool bankTracked_ = false;
    void bankTotals();
    Counter profBankOpenCycles_, profBankOpenWindow_;
    Counter profActivates_;   // 1.12.18: ACT commands the replayed controller issued (ROI)
    // exported (ROI window)
    Counter profRequests_, profEpochs_, profReads_, profWrites_;
    Counter profLatSum_;                 // sum of measured latencies (core cycles)
    Counter profUnserved_;               // requests still in flight at the drain bound
    Counter profRowHits_, profRowMisses_, profRowConflicts_, profRefreshes_;
    Counter profTicks_, profIdleTicks_;   // DRAM cycles replayed, and those with no request in flight (row 28: idle cycles)
    Counter profChannelsUsed_, profMaxChanPermille_;   // channels that received requests; the busiest channel's share (per mille)
    Counter profPriced_, profBootstrap_;  // accesses priced by a replay value / by the analytical fallback
    /* 1.12.18 (ruling 17 / R0605): the host interfaces' share, exported when
     * the instance serves a host memory controller */
    Counter profHRequests_, profHReads_, profHWrites_, profHLatSum_, profHRLatSum_, profHRowHits_, profHRowMisses_,
            profHRowConflicts_, profHActivates_, profHPriced_, profHBootstrap_;
    /* 1.12.18 (user ruling R1 2026-10-24): outside the ROI, not priced */
    Counter profORequests_, profOReads_, profOWrites_, profORowHits_, profORowMisses_, profORowConflicts_,
            profOActivates_, profOHostReads_, profOHostWrites_, profOHostActivates_;
  public:
    void notePriced(bool byReplay) { if (byReplay) rawPriced_.fetch_add(1, std::memory_order_relaxed); else rawBoot_.fetch_add(1, std::memory_order_relaxed); }
};

#endif  // DRAM_EPOCH_REPLAY_H_
