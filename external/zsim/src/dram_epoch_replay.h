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
    /* 1.12.18 (user ruling R1 2026-10-24): the request counters of interface
     * `mi` (a PE memory interface's rd/wr, a host memory controller's rd/wr).
     * A request the replay finds stamped after the ROI end is still replayed
     * (the controller state stays correct) but is taken out of these
     * counters, as out of every replayed ROI total, and reported apart. */
    /* (next, user ruling 2026-10-28: the message-passing and device latency gap): a device interface also binds
     * its charged-latency counters (replayPriced / replayLat); an access priced at the replay and found outside the
     * ROI leaves them as it leaves rd/wr, so the charged view and the replay's measured view (latencySum /
     * ctrlRequests, ROI only) count the same requests. */
    void bindCounters(uint32_t mi, Counter* rd, Counter* wr, Counter* priced = nullptr, Counter* chargedLat = nullptr);
    /* (next, user ruling 2026-10-28): the requesters of this instance's host
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
    void record(uint64_t epoch, uint64_t stamp, uint32_t unit, uint32_t slot, uint64_t row,
                uint64_t lineAddr, bool isWrite, uint32_t mi, uint32_t charged = 0);   // (1.12.153): charged = the latency the access was priced at (0: bootstrap)

    /* The service latency (core cycles, device time including queueing) that
     * interface `mi` charges in epoch `curEpoch`: the mean measured by
     * replaying epoch curEpoch-1 (replayed on demand, once). 0 = not yet
     * measurable (first epoch, or the interface had no access in the previous
     * epoch): the caller keeps its analytical service. */
    uint32_t serviceLatency(uint32_t mi, uint64_t curEpoch);

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
    uint32_t serviceLatencyCut(uint32_t mi, uint64_t epoch, uint64_t lag);

    /* (next, user ruling 2026-10-28: the multi-core latency gap) the replay's own epoch length in phases,
     * simulation.dram_epoch_phases (handed over as PIMID_DRAM_EPOCH_PHASES; default kDramEpochPhases),
     * separate from the NoC's det_epoch_phases. It was det_epoch_phases (4 phases = 40 000 cycles): a
     * price two such epochs old let the cores of a loaded controller issue in bursts the controller took
     * several epochs to serve. The read lag stays two epochs (kDramReadLagEpochs). */
    static constexpr uint32_t kDramEpochPhases = 1;
    static constexpr uint64_t kDramReadLagEpochs = 2;
    /* (next, user ruling 2026-10-28: the message-passing and device latency gap) the price is the request-weighted
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

    bool available() const { return sys_ != nullptr; }
    double dramTicksPerCoreCycle() const { return ticksPerCycle_; }

  private:
    struct Rec { uint64_t stamp; uint64_t addr; uint32_t mi; uint8_t wr; uint32_t ch; uint8_t shadow = 0; uint32_t grp = 0;
                 uint32_t src = ~0u, chg = 0; };   // (1.12.153): src = the host core of a thread-MPI host record (~0u otherwise); chg = the latency charged   // 1.12.18 (drain the lag): ch = the controller channel; shadow = a host lock-step copy past chip 0, grp = its host request
    struct Inflight { uint32_t remaining; uint64_t issueTick; uint32_t mi; bool wr; bool out; uint64_t epoch; uint32_t grp = 0; uint32_t chg = 0; };   // (1.12.153): chg   // (1.12.145): issueTick = the tick the request reached the controller's door   // 1.12.18: out = stamped after the ROI end (R1); grp
    /* 1.12.18 (user ruling 2026-10-24, "drain the lag"): a record that finds
     * its controller queue full waits at that queue's door (FIFO per channel
     * and direction) and enters the instant the controller can take it; an
     * epoch's requests stay in flight across the epoch boundary, and its
     * measured service latency is final when its last request completes. */
    struct Waiting { Rec r; uint64_t epoch; uint64_t target; bool out; uint32_t b; uint64_t id; uint64_t feed = 0; };   // (1.12.145): feed = the tick the record reached the door
    struct EpochAcc { std::vector<uint64_t> sum, cnt; uint64_t pend = 0; bool fed = false; };
    std::map<uint32_t, std::deque<Waiting>> doors_;
    uint64_t waitingN_ = 0, nextId_ = 0;
    std::map<uint64_t, EpochAcc> acc_;
    bool acceptOne(Waiting& w);
    void serveDoors();
    void finalizeEpoch(uint64_t epoch);
    uint32_t partialMean(uint64_t e, uint32_t mi);   // 1.12.18
    /* (next, user ruling 2026-10-28): the partial mean of an epoch still in flight counts its unfinished
     * requests (in the controller or waiting at its door) at their age so far; cached per controller tick. */
    struct PartialCache { uint64_t tick = ~0ull; std::vector<uint64_t> sum, cnt; };
    std::map<uint64_t, PartialCache> pcache_;
    uint64_t partialAged_ = 0;                       // reads whose partial mean included unfinished requests

    uint64_t partial_ = 0;                           // 1.12.18: reads served a partial mean
    void drainAll();
    uint64_t doorRecs_ = 0, doorTicks_ = 0, doorMax_ = 0; bool doorReported_ = false;
    /* 1.12.18 (R0605): one record at a composed Ramulator address */
    void recordAddr(uint64_t epoch, uint64_t stamp, uint64_t addr, uint32_t ch, bool isWrite, uint32_t mi, uint8_t shadow = 0, uint32_t grp = 0);
    /* (next, user ruling 2026-10-28: the message-passing latency gap) thread-MPI host interfaces. The host cores of a
     * thread-MPI run never hold the consistent cut (their ROI baseline is no core roiBaseCycle: the cut reads their
     * absolute clock), so every host record is folded at the next phase barrier; a rank whose ROI-relative clock
     * trails the others (it waited, or was descheduled: a 16-rank DDR5 stream_triad rank 363 epochs behind) records
     * behind the replay clock. Each such record used to issue at the clock, a phase of that rank's traffic at one
     * tick: door waits of 0.48 M DRAM cycles and a measured 6820 cycles per request on 16-rank DDR5 bfs.
     *  - mpiHost_: the host interfaces of a thread-MPI run;
     *  - shiftLate: a source whose record is behind the clock is shifted, as a whole, to the clock (a per-source offset
     *    that only grows), so its requests keep their spacing;
     *  - an access reads the newest epoch the controller has been fed, not k-2 of its own clock: the epochs are the
     *    phase barriers' folds, and k-2 of a trailing rank's clock was many folds old. */
    bool mpiHost_ = false;
    std::unordered_map<uint32_t, uint64_t> srcShift_;   // source core -> cycles its records are shifted by
    uint64_t shiftedRecs_ = 0, shiftMax_ = 0;
    void shiftLate(std::vector<Rec>& recs);
    std::map<uint64_t, std::vector<uint64_t>> winSum_, winCnt_;   // (1.12.153): finalized epochs' sums and counts (cut mode) for the price window
    std::vector<Counter*> miPriced_, miChgLat_;                     // (1.12.153): bound charged-latency counters
    std::vector<uint64_t> outPriced_, outChgLat_, outPricedApplied_, outChgLatApplied_;
    bool isHostMi(uint32_t mi) const { return mi >= hostMi0_ && mi < hostMi0_ + hostMis_; }
    bool hostOnly_ = false;
    bool devRequesters_ = false;   // (next, user ruling 2026-10-28): see setDeviceRequesters
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
    struct ChanEv { uint64_t tick; uint32_t seq; uint64_t epoch; uint32_t mi; uint64_t latCyc; bool account, out, host; uint32_t chg; };   // (1.12.153): chg
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
                 hreq = 0, hrd = 0, hwr = 0, hlat = 0,                       // 1.12.18 (R0605): the host interfaces' share
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
    uint64_t phaseWindowCycles(std::string* why);
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
    Counter profHRequests_, profHReads_, profHWrites_, profHLatSum_, profHRowHits_, profHRowMisses_,
            profHRowConflicts_, profHActivates_, profHPriced_, profHBootstrap_;
    /* 1.12.18 (user ruling R1 2026-10-24): outside the ROI, not priced */
    Counter profORequests_, profOReads_, profOWrites_, profORowHits_, profORowMisses_, profORowConflicts_,
            profOActivates_, profOHostReads_, profOHostWrites_, profOHostActivates_;
  public:
    void notePriced(bool byReplay) { if (byReplay) rawPriced_.fetch_add(1, std::memory_order_relaxed); else rawBoot_.fetch_add(1, std::memory_order_relaxed); }
};

#endif  // DRAM_EPOCH_REPLAY_H_
