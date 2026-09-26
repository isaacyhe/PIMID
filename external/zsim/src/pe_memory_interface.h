/**
 * pe_memory_interface.h — PE Memory Interface
 *
 * Each PE-MI is bound to a group of PEs and covers a defined set of memory
 * units (banks, subarrays, etc.).  Accesses within coverage are served locally;
 * accesses outside trigger hierarchy traversal to the authoritative PE-MI.
 *
 * M/D/1 queuing is always active for bandwidth contention modeling.
 * (The old SimplePEMemoryController and MD1PEMemoryController are merged.)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PE_MEMORY_INTERFACE_H_
#define PE_MEMORY_INTERFACE_H_

#include <atomic>
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include "g_std/g_string.h"
#include "garnet_network.h"
#include "memory_hierarchy.h"
#include "hierarchy_util.h"
#include "sparse_htree.h"
#include "pimid_noc_shm.h"
#include <cstdlib>
#include "pad.h"
#include "stats.h"
#include "zsim.h"

/**
 * PE Memory Interface: coverage routing + hierarchy traversal + M/D/1 queuing.
 * Supports both contiguous [start, end) and non-contiguous coverage sets.
 */
class PEMemoryInterface : public MemObject {
protected:
    g_string name_;
    uint32_t mcId_;
    uint32_t coverageStart_;  // [start, end) unit range (contiguous fast path)
    uint32_t coverageEnd_;
    uint32_t totalUnits_;     // total units at placement level

    // Non-contiguous coverage support
    bool nonContiguous_ = false;
    std::vector<uint32_t> coverageSet_;  // sorted for binary search

    // Connection mode link latency
    uint32_t localLinkLat_ = 0;  // additional PE↔mem hop for SEPARATE_ENDPOINTS

    // Phase tracking for Garnet batch processing
    uint64_t batchLastPhase_ = 0;

    // M/D/1 queuing (always active)
    double maxRequestsPerCycle_;
    // TEMP (pre-arXiv): parallel server count for M/D/c queuing — the number of
    // DRAM channels serving requests concurrently (HBM3=16, HBM2=8, others=1).
    // Effective queuing load is divided by this so high-channel technologies stay
    // unsaturated under contention. A proper N-parallel-network model is future
    // work; this is a stop-gap. 1 = ordinary M/D/1.
    uint32_t numChannels_ = 1;
    uint32_t localLatency_;
    uint32_t curLatency_;
    uint64_t lastPhase_;
    double smoothedPhaseAccesses_;
    uint32_t curPhaseAccesses_;
    lock_t updateLock_;

    // -- 1.9.0 deterministic epoch-frozen scalar (E2 fix) --------------------
    // Accumulate an integer access COUNT per epoch (on the per-core roiRel axis);
    // an epoch is FINAL when the in-process consistent cut has cleared its end
    // (all ACTIVE cores passed it), at which point a frozen value is computed from
    // its total count. An access reads the PREVIOUS epoch's frozen value. All state
    // under one lock, so the value is a pure function of the (order-free integer)
    // per-epoch count -- independent of intra-epoch cross-thread arrival order.
    // Removes the read-instant nondeterminism (E2) for the per-channel BW wait and
    // the per-MI M/D/1 device time, on the SAME cut the NoC epoch pricing uses.
    struct EpochFrozenScalar {
        lock_t lk = {0};
        std::map<uint64_t, uint64_t> acc;     // epoch -> accumulating count
        std::map<uint64_t, uint32_t> frozen;  // epoch -> frozen value
    };
    EpochFrozenScalar md1Epoch_;   // per-MI M/D/1 epoch-frozen device time

    PAD();

    // Stats
    Counter profLocalAccesses_;
    Counter profRemoteAccesses_;
    Counter profLocalLatency_;
    Counter profRemoteLatency_;
    Counter profReads_;
    Counter profWrites_;
    /* 1.11.52 (audit D003): MEASURED row-buffer behaviour. The array energy
     * model weighted its activate/precharge term by a hardcoded
     * ROW_MISS_FRAC = 0.5 -- the dominant term in DRAM access energy set by
     * a coin flip. The interface already sees every access address and the
     * unit it belongs to, so the open-row state is observable here: one
     * last-row register per unit, exactly what a closed-page-vs-open-page
     * controller would track. Hits/misses are exported and the energy model
     * uses the measured fraction (and refuses to invent one when this was
     * never armed, i.e. dramRowBytes == 0). */
    Counter profRowHits_;
    Counter profRowMisses_;
    /* 1.11.57 (audit D006, D007, D008): one open-row register per BANK, sized
     * ONCE at construction, indexed by a row number computed on the RANK's
     * address stripe rather than on one device's page. See noteRowAccess()
     * for what each of those three corrections was fixing. ~0ull = no open
     * row yet. */
    std::vector<uint64_t> lastRow_;
    uint64_t rowStrideBytes_ = 0;     // system bytes one ACT makes resident
    uint32_t rowSlotsPerUnit_ = 1;    // independent open rows inside one unit

public:
    // Contiguous coverage
    PEMemoryInterface(uint32_t id, uint32_t start, uint32_t end,
                      uint32_t totalUnits, uint32_t localLatency,
                      uint32_t linkLat, uint64_t bandwidthMBs,
                      uint32_t lineSize, uint32_t freqMHz, g_string& name,
                      uint32_t numChannels = 1)
        : name_(name), mcId_(id),
          coverageStart_(start), coverageEnd_(end),
          totalUnits_(totalUnits),
          localLinkLat_(linkLat),
          numChannels_(numChannels >= 1 ? numChannels : 1),
          localLatency_(localLatency),
          curLatency_(localLatency),
          lastPhase_(0), smoothedPhaseAccesses_(0.0), curPhaseAccesses_(0)
    {
        futex_init(&updateLock_);
        double bytesPerCycle = (bandwidthMBs > 0) ? (bandwidthMBs * 1e6) / (freqMHz * 1e6) : 0.0;
        maxRequestsPerCycle_ = (bytesPerCycle > 0.0) ? bytesPerCycle / lineSize : 0.0;
        initRowModel_();   // 1.11.57 (D007): size the open-row table off the access path
    }

    // Non-contiguous coverage
    PEMemoryInterface(uint32_t id, const std::vector<uint32_t>& coverage,
                      uint32_t totalUnits, uint32_t localLatency,
                      uint32_t linkLat, uint64_t bandwidthMBs,
                      uint32_t lineSize, uint32_t freqMHz, g_string& name,
                      uint32_t numChannels = 1)
        : name_(name), mcId_(id),
          coverageStart_(0), coverageEnd_(0),
          totalUnits_(totalUnits),
          nonContiguous_(true), coverageSet_(coverage),
          localLinkLat_(linkLat),
          numChannels_(numChannels >= 1 ? numChannels : 1),
          localLatency_(localLatency),
          curLatency_(localLatency),
          lastPhase_(0), smoothedPhaseAccesses_(0.0), curPhaseAccesses_(0)
    {
        std::sort(coverageSet_.begin(), coverageSet_.end());
        if (!coverageSet_.empty()) {
            coverageStart_ = coverageSet_.front();
            coverageEnd_ = coverageSet_.back() + 1;
        }
        futex_init(&updateLock_);
        double bytesPerCycle = (bandwidthMBs > 0) ? (bandwidthMBs * 1e6) / (freqMHz * 1e6) : 0.0;
        maxRequestsPerCycle_ = (bytesPerCycle > 0.0) ? bytesPerCycle / lineSize : 0.0;
        initRowModel_();   // 1.11.57 (D007): size the open-row table off the access path
    }

    bool isLocalAddress(Address lineAddr) const {
        // Device-only near-data (option 1): route ALL of a PE's accesses through
        // the PE-MI (the placement-aware local path), not the ALU core's flat
        // direct path -- so every access shares one comparable cost model and the
        // in-coverage / out-of-coverage split does not confound placement levels.
        if (zinfo->hierarchy.assumeLocal) return false;
        uint32_t unit = addrToUnit(lineAddr);
        return isLocal(unit);
    }

    // The SHARED sparse placement-driven H-tree -- the SINGLE SOURCE OF TRUTH it
    // co-owns with the topology emitter in src/main.cpp (invariant #2). Built once
    // from the SAME inputs the emitter used (PE home units, placement level, dims,
    // channel count), via the SAME buildSparseHTree(), so the endpoint ids used to
    // route at runtime can never drift from the emitted Garnet topology. The link
    // layer widths/latencies are irrelevant here (routing needs only the STRUCTURE:
    // routerOf / abstractOf / peOfLeaf), so dummy layer arrays are passed.
    static const pimid_htree::SparseHTree& tree() {
        static pimid_htree::SparseHTree t;
        static std::once_flag onceFlag;
        std::call_once(onceFlag, []() {
            std::vector<uint64_t> peHomes;
            uint32_t numPEs = zinfo->hierarchy.peMemMapSize;
            for (uint32_t p = 0; p < numPEs; p++) {
                uint32_t off = zinfo->hierarchy.peMemMapOffsets[p];
                peHomes.push_back(off < 4096
                    ? (uint64_t)zinfo->hierarchy.peMemMapData[off] : (uint64_t)p);
            }
            const int W[4] = {1, 1, 1, 1}, Lt[4] = {1, 1, 1, 1};  // structure only
            t = pimid_htree::buildSparseHTree(
                peHomes, (int)zinfo->hierarchy.placementLevel,
                (int)zinfo->hierarchy.dramChannels,
                (int)zinfo->hierarchy.subarraysPerBank, (int)zinfo->hierarchy.banksPerBG,
                (int)zinfo->hierarchy.bgPerChip, (int)zinfo->hierarchy.chipsPerRank,
                (int)zinfo->hierarchy.ranksPerChannel, W, Lt);
        });
        return t;
    }

    // Garnet endpoint a target unit routes to: a PE endpoint if the unit hosts a
    // PE, else the abstract endpoint of its deepest LIVE region (invariant #4). -1
    // only if the tree has no PEs at all. Delegates to the shared tree so both
    // sides agree.
    static int unitToEndpoint(uint32_t unit) { return tree().endpointForUnit(unit); }

    // Replay timestamp for the SINGLE-PROCESS (OMP/device) batch: per-core
    // curCycle values are PRIVATE work clocks with no cross-thread sync (an
    // init-heavy thread sits hundreds of millions of cycles ahead of a fresh
    // worker), so they are NOT one timeline. Stamping records with them makes
    // a batch span the whole skew window: the replay ticks through billions of
    // empty cycles (4h wall for one cell) AND same-phase packets almost never
    // coexist -- OMP contention systematically understated. The honest shared
    // axis inside a process is the GLOBAL phase clock; the core's intra-phase
    // offset spreads injections deterministically within the phase.
    // 1.6 thread-MPI deterministic pricing: the batch-smoothed EWMA read is
    // wall-order-dependent (which drains completed before this access is a
    // host race), and making it causal across parked ranks is a conservative-
    // synchronization (PDES) problem. In thread mode we therefore price from
    // the STATIC analytic NoC latency -- topology-pure, load-independent,
    // bit-deterministic. The Garnet batch replay still runs and reports
    // measured contention in stats; it just does not feed back into per-access
    // latency. Documented model boundary (HANDOFF_16_THREAD_MPI.md).
    static bool mpiThreadDetPricing() {
        // Thread-based rank emulation is the only exec-method MPI model
        // (1.8.3): active whenever the launcher requested >1 rank and this
        // is not a per-rank trace-gen process. MUST match the plugin's
        // g_mpi_thread_mode resolution -- pricing and scheduling semantics
        // have to flip together.
        static int v = -1;
        if (v < 0) {
            const char* r = getenv("PIMID_MPI_RANKS");
            v = (r && atoi(r) > 1 && !getenv("PIMID_MPI_RANK")) ? 1 : 0;
        }
        return v == 1;
    }

    // 1.9.0 escape hatch (A/B only). Default thread-MPI pricing is now the
    // deterministic epoch-frozen MEASURED Garnet feedback (latencyForEpoch);
    // PIMID_MPI_ANALYTICAL_PRICING=1 restores the pre-1.9.0 analytical override
    // (static topology-pure one-way latency, load-independent) so the two models
    // can be compared on one binary. NOT for production sweeps.
    static bool forceAnalyticalPricing() {
        static int v = -1;
        if (v < 0) v = getenv("PIMID_MPI_ANALYTICAL_PRICING") ? 1 : 0;
        return v == 1;
    }

    static uint64_t phaseStamp(uint64_t coreCycle) {
        uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
        // Stamp on numPhases -- THE SAME counter that gates batch claims -- not
        // on globPhaseCycles. glob freezes whenever the scheduler stops ticking
        // (thread leave/park at OMP region boundaries, termination tail) while
        // numPhases keeps advancing; stamping on the frozen clock while claiming
        // on the live one mixes eras inside a batch and the replay crawls
        // through the divergence window. Tying stamp and claim to ONE counter
        // makes a batch's span <= ~1 phase BY CONSTRUCTION.
        return (uint64_t)zinfo->numPhases * pl + (coreCycle % pl);
    }

    // ── Shared detailed-MPI NoC: ONE logical Garnet driven by ALL ranks ──────
    // When the launcher exports PIMID_NOC_SHM (MPI + detailed), every rank
    // publishes its network-traversing accesses {src,dst,cycle} to its ring in
    // the shared log and, at its own phase drain, replays the IDENTICAL merged
    // multi-rank stream (up to the min-watermark consistent cut) through its
    // local Garnet replica. processBatch resets Garnet per drain, so a drain is
    // a pure function of the record window: N deterministic replicas of one
    // logical network, all seeing all ranks' packets. No barriers, no async
    // coordinator; quiescent (guest blocked in MPI) and exited ranks are exempt
    // from the cut, so no rank ever waits on another.
    struct SharedNoc {
        PimidNocHdr* h = nullptr;
        uint32_t rank = 0;
        uint32_t nranks = 1;
        std::vector<uint64_t> cursor;   // per-ring read position (this process)
        std::vector<uint64_t> lost;     // per-ring overwritten-before-read count
        uint64_t pressureDrains = 0;    // past-the-cut consumptions (valve hits)
        // Records consumed from the rings but not yet replayed. Replay happens
        // strictly in per-phase buckets up to the consistent cut; stamps beyond
        // the cut wait here. This makes the replayed windows a pure function of
        // SIMULATED time: host-scheduling skew between rank processes changes
        // only when buckets get replayed, never their contents. (Before this,
        // a drain replayed whatever had accumulated -- on a loaded host that
        // was thousands of phases lumped into one window, and simulated cycle
        // counts varied up to 9x with the execution venue.)
        std::vector<GarnetNetwork::BatchAccess> pending;
        lock_t lock;
    };
    static SharedNoc* sharedNoc() {
        static SharedNoc ctx;
        static std::once_flag onceFlag;
        std::call_once(onceFlag, []() {
            const char* nm = getenv("PIMID_NOC_SHM");
            const char* rk = getenv("PIMID_MPI_RANK");
            if (!rk) return;               // not an MPI rank: single-process Garnet
            if (!nm) {
                // MPI rank in detailed mode without the shared log = someone
                // bypassed the launcher. There is NO isolated multi-Garnet
                // mode to fall back to -- refuse rather than silently run N
                // blind networks.
                panic("[SharedNoC] detailed-MPI rank without PIMID_NOC_SHM: "
                      "the shared Garnet stream is the only MPI NoC model; "
                      "launch through pimid --mpi-ranks");
            }
            PimidNocHdr* h = pimid_noc_shm_attach(nm);
            if (!h) {
                panic("[SharedNoC] PIMID_NOC_SHM=%s attach failed: cannot run "
                      "detailed-MPI without the shared Garnet stream (no "
                      "isolated-Garnet fallback exists)", nm);
            }
            ctx.h = h;
            ctx.rank = (uint32_t)atoi(rk);
            ctx.nranks = h->nranks;
            ctx.cursor.assign(h->nranks, 0);
            ctx.lost.assign(h->nranks, 0);
            futex_init(&ctx.lock);
            pimid_noc_set_state(h, ctx.rank, PIMID_NOC_ACTIVE);
            info("[SharedNoC] rank %u/%u attached %s (ringSlots=%u nodes=%u "
                 "nodesPerRank=%u): ONE logical Garnet across all ranks",
                 ctx.rank, ctx.nranks, nm, h->ringSlots, h->numNodes,
                 h->nodesPerRank);
        });
        return ctx.h ? &ctx : nullptr;
    }

    // Collect the merged multi-rank stream and replay it through the local
    // Garnet replica in PER-PHASE BUCKETS up to the consistent cut. Never
    // blocks on peers: the cut is min-watermark over ACTIVE ranks;
    // quiescent/exited/not-yet-publishing ranks are exempt.
    //
    // VENUE INDEPENDENCE (defect #9): rings are always consumed fully into a
    // persistent pending buffer, but replay happens ONLY in complete
    // phase-sized buckets whose stamps are <= the cut. A drain that finds ten
    // thousand accumulated phases (host-loaded venue: rank processes skewed by
    // the OS scheduler) replays them as ten thousand single-phase windows --
    // byte-for-byte the same sequence a fast host produces by draining every
    // phase. Replayed contention is a pure function of the simulated-time
    // record stream; the host changes only when the arithmetic runs.
    // The old design replayed each drain's accumulation as ONE window, so
    // same-phase packets from skewed ranks landed in different windows
    // (contention undercounted up to 9x) and multi-phase lumps hit the replay
    // as synthetic bursts. The pressure valve no longer forces early replay
    // either -- backlogged records just wait in pending (memory, not skew).
    static void sharedNocDrain(GarnetNetwork* gn, SharedNoc* sn, uint64_t phaseNum) {
        futex_lock(&sn->lock);
        uint64_t cut = pimid_noc_cut(sn->h);
        // 1) Consume the rings fully into pending (never leave records exposed
        //    to overrun; the ring is the only lossy structure here).
        for (uint32_t r = 0; r < sn->nranks; r++) {
            PimidNocRank* rk = pimid_noc_rank(sn->h, r);
            PimidNocRec* ring = pimid_noc_ring(sn->h, r);
            uint64_t seq = __atomic_load_n(&rk->seq, __ATOMIC_ACQUIRE);
            uint64_t& cur = sn->cursor[r];
            if (seq > (uint64_t)sn->h->ringSlots &&
                cur < seq - sn->h->ringSlots) {
                // Producer lapped us: entries [cur, seq-ringSlots) are gone.
                uint64_t dropped = (seq - sn->h->ringSlots) - cur;
                sn->lost[r] += dropped;
                cur = seq - sn->h->ringSlots;
                warn("[SharedNoC] rank %u ring overran reader by %lu recs "
                     "(total lost from rank %u: %lu)",
                     r, dropped, r, sn->lost[r]);
            }
            while (cur < seq) {
                PimidNocRec rec = ring[cur & (sn->h->ringSlots - 1)];
                // Tear check: if the producer lapped us mid-copy, discard+resync.
                uint64_t seq2 = __atomic_load_n(&rk->seq, __ATOMIC_ACQUIRE);
                if (seq2 > (uint64_t)sn->h->ringSlots &&
                    cur < seq2 - sn->h->ringSlots) {
                    break;  // next drain's overrun branch resyncs + counts
                }
                sn->pending.push_back({rec.src, rec.dst, rec.cycle});
                cur++;
            }
        }
        // 2) Partition pending: stamps <= cut are FINAL (no rank can publish
        //    below its watermark) and get bucketed by phase; the rest wait.
        uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
        std::map<uint64_t, std::vector<GarnetNetwork::BatchAccess>> buckets;
        std::vector<GarnetNetwork::BatchAccess> keep;
        keep.reserve(sn->pending.size());
        for (auto& a : sn->pending) {
            if (a.cycle <= cut) buckets[a.cycle / pl].push_back(a);
            else                keep.push_back(a);
        }
        sn->pending.swap(keep);
        if (sn->pending.size() > (size_t)sn->h->ringSlots * 4) {
            sn->pressureDrains++;
            if ((sn->pressureDrains & (sn->pressureDrains - 1)) == 0)  // 1,2,4,8...
                warn("[SharedNoC] pending backlog %zu recs beyond the cut "
                     "(a rank is far behind in host time; replay waits -- "
                     "correctness unaffected, memory grows; warnings: %lu)",
                     sn->pending.size(), sn->pressureDrains);
        }
        futex_unlock(&sn->lock);
        // 3) Replay each complete phase bucket in order, one reset per bucket:
        //    identical windows on every host, loaded or idle. (phaseNum tag is
        //    display-only in the replay; use each bucket's own phase.)
        for (auto& kv : buckets)
            gn->processBatchRecords(std::move(kv.second), kv.first);
        (void)phaseNum;
    }

    // -- 1.9.0 in-process thread-MPI NoC cut (deterministic epoch membership) ----
    // Per-core roiRel = curCycle - THIS core's own roiBaseCycle: a DETERMINISTIC,
    // cross-core-comparable stamp (unlike the wall-decoupled global numPhases).
    // Epoch index for an access on the epoch axis. Thread-MPI / OMP (sn == null)
    // key on the GLOBAL phase clock numPhases: it is advanced ONLY at the phase
    // barrier (all cores synced), so an access in numPhases-epoch k structurally
    // GATES on epoch k-1 being complete -- the barrier fold has already replayed it
    // (no blocking read needed). Shared/process MPI (sn != null) keys on roiRel.
    // Per-core deterministic roiRel: curCycle - THIS core's own roiBaseCycle. This
    // removes the cross-core startup SKEW (rank 0 did init, others spawned fresh),
    // so contemporaneous ROI accesses from different cores align on ONE axis.
    static uint64_t perCoreRoiRel(uint32_t srcId, uint64_t cycle) {
        uint64_t base = 0;
        if (srcId < zinfo->numCores && zinfo->cores[srcId])
            base = zinfo->cores[srcId]->getRoiBaseCycle();
        return (cycle > base) ? cycle - base : 0;
    }

    // NoC read-lag in EPOCHS. The barrier cut folds roiRel up to min-active-roiRel;
    // with the cores kept within ~1 phase by the weave, a 2-epoch lag guarantees the
    // read epoch is already folded (the cut has cleared it) => no read/fold race.
    static const uint64_t kNocReadLagEpochs = 2;

    static uint64_t epochOfAccess(GarnetNetwork* gn, SharedNoc* sn,
                                  uint64_t reqCycle, uint32_t srcId) {
        uint32_t E = gn->detEpochPhases();
        uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
        uint64_t epochLen = (uint64_t)pl * E;
        if (sn) {
            uint64_t base = zinfo->hierarchy.mpiNocRoiBase;
            uint64_t rel = (reqCycle > base) ? reqCycle - base : 0;
            return epochLen ? rel / epochLen : 0;
        }
        if (!zinfo->hierarchy.mpiNocBaselined) return 0;   // pre-ROI: bootstrap
        return epochLen ? perCoreRoiRel(srcId, reqCycle) / epochLen : 0;
    }

    // Epoch-frozen one-way NoC latency: read epoch (k - lag)'s frozen sample. The
    // barrier cut guarantees it is folded (complete) by construction; the lag
    // absorbs the small cross-core roiRel spread. 0 (bootstrap epochs) -> analytical.
    static uint32_t epochFrozenNocLat(GarnetNetwork* gn, SharedNoc* sn,
                                      uint64_t reqCycle, uint32_t srcId) {
        uint64_t k = epochOfAccess(gn, sn, reqCycle, srcId);
        uint64_t lag = sn ? 1 : kNocReadLagEpochs;
        return (k >= lag) ? gn->latencyForEpoch(k - lag) : 0;
    }

    /* 1.10.6: direction mix of the traffic offered to the channel DQ bus.
     * Aggregate, not per-channel: elements round-robin across channels, so
     * every channel sees statistically the same mix, and one pair of counters
     * keeps the deterministic (epoch-frozen) MPI path free of per-channel
     * ordering state. The RATIO is quantized to 1/64 before use, so the
     * rounding of the final wait is insensitive to the last few accesses. */
    struct DqMix {
        static inline std::atomic<uint64_t> reads{0};
        static inline std::atomic<uint64_t> writes{0};
    };

    uint64_t access(MemReq& req) override {
        if (req.type == GETS) DqMix::reads.fetch_add(1, std::memory_order_relaxed);
        else                  DqMix::writes.fetch_add(1, std::memory_order_relaxed);
        /* 1.11.8 PG residency: every device memory access rides the device MC
         * path, so devMC[0] is marked here, unconditionally, and that is
         * correct -- there is no path out of access() that skips the memory
         * controller. devMC[0] carries the aggregate device-MC activity
         * (per-channel split is a refinement once channel ids are surfaced
         * here -- stated, not hidden).
         *
         * 1.11.38 (audit E16): the NoC marker used to sit HERE too, on the
         * claim that every access "injects into the tree fabric". That is
         * true under detailed Garnet, which forces wantLocal false below and
         * routes everything -- but NOT under the analytical NoC, where a
         * local-region access takes the fast path and returns without a
         * packet, and not on the degenerate no-PE path, which says in its own
         * comment that it performs no traversal. Marking the fabric busy for
         * those accesses drove its idle residency to zero and cost it gating
         * credit it had earned. The NoC is now marked at the three sites that
         * actually generate fabric traffic; search pgres.noc.touch. */
        { uint64_t _ph = zinfo->numPhases;
          zinfo->pgres.devMC[0].touch(_ph);
          zinfo->pgres.devMCGaps.event(req.cycle); }   // E17 measurement
        // Update coherence state
        switch (req.type) {
            case PUTS: case PUTX: *req.state = I; break;
            case GETS: *req.state = req.is(MemReq::NOEXCL) ? S : E; break;
            case GETX: *req.state = M; break;
        }

        uint32_t targetUnit = addrToUnit(req.lineAddr);

        // Memory behind a network. The simulator prices an access ONLY by WHERE
        // the data is: the PE's own placement region (isLocal, via the device-local
        // contiguous addrToUnit map) is CLOSE -> local fast path below; anywhere
        // else is FAR -> crosses the network (remote path below), real hop cost.
        // Nothing is moved or cached automatically -- keeping data close, reuse,
        // and any host/device staging are the USER's job (their code, or a
        // cache-based PE which ZSim already models). The ALU has no cache, so it
        // simply pays the distance.
        // DETAILED Garnet: there is NO computed / uniform-local shortcut. EVERY
        // access is a packet that travels the H-tree from the PE's home node to the
        // target node; Garnet returns the latency it actually took. Own subarray =
        // same node = 0 network hops; a nearer unit = fewer hops; a farther one =
        // more -- the tiering is whatever the real tree yields, at the placement
        // level's resolution. So do not special-case "local"; route all traffic
        // through the network path below. (isLocal is kept only for the analytical
        // NoC, which has no packets and must fall back to a distance model.)
        bool wantLocal = zinfo->garnetNetwork && zinfo->garnetNetwork->isCycleAccurate()
                         ? false : isLocal(targetUnit);
        if (wantLocal) {
            uint32_t lat = localAccessLatency(req) + localLinkLat_;

            // DRAM channel bandwidth bottleneck (accuracy fix): even a "local"
            // bank access consumes the shared DRAM channel's DQ bandwidth. In
            // detailed (cycle-accurate) mode, charge the shared M/D/c channel-BW
            // queueing wait so the aggregate effective DRAM BW is capped at the
            // datasheet value across ALL bank-MIs.
            {
                GarnetNetwork* gnLocal = zinfo->garnetNetwork;
                if (gnLocal && gnLocal->isCycleAccurate() &&
                    zinfo->hierarchy.nocAggBandwidthMBs > 0) {
                    lat += channelBandwidthWait(req.srcId, req.cycle);
                }
            }

            // Standalone MC: the MC fronting this memory is a SEPARATE NoC
            // endpoint (one per memory org), so even a local PE access must
            // cross the NoC to reach the MC (core → MC node → memory). Charge an
            // extra one-RTT NoC hop to the MC node on top of the local access.
            // Standalone MC: each memory org fronts a separate NoC endpoint, so a
            // PE access crosses the NoC to its MC node. This hop is the near-data
            // LATENCY differentiator: finer placement co-locates the PE with its
            // own MC (fewest hops -> fastest), coarser placement reaches a distant
            // shared MC (more hops). Keep it for device-only AND co-sim.
            if (zinfo->hierarchy.mcStandalone) {
                lat += mcHopLatency(targetUnit, req.cycle, req.srcId);
                // E16 site 1: the ONLY fabric traffic a local access makes.
                zinfo->pgres.noc.touch(zinfo->numPhases);
                zinfo->pgres.nocGaps.event(req.cycle);   // E17 measurement
            }

            profLocalAccesses_.inc();
            profLocalLatency_.inc(lat);
            noteRowAccess(targetUnit, req.lineAddr);   // 1.11.52 (D003)
            return req.cycle + lat;
        } else {
            // Remote: route through network to destination MI
            uint32_t myUnit = representativeUnit();
            uint32_t remoteLat = localAccessLatency(req);

            GarnetNetwork* gn = zinfo->garnetNetwork;
            if (gn && gn->isCycleAccurate()) {
                // EVERY access TRAVELS the sparse H-tree. endpointForUnit maps the
                // target unit to its Garnet endpoint: the PE index if the unit hosts
                // a PE, else the abstract endpoint of its DEEPEST LIVE region -- a
                // non-PE unit routes to that region's terminus (invariant #4), so its
                // network distance is the real tier (same bank as a PE < other
                // bank-group < other channel) and the DRAM device time is still
                // charged via remoteLat below. src = THIS PE (endpoint == mcId_);
                // dst==src (own unit) = 0 network hops = near-data; nearer = fewer
                // hops; farther = more.
                int dstEp = unitToEndpoint(targetUnit);
                if (dstEp < 0) {
                    // Degenerate: the tree has no PEs (no placement) -> nothing to
                    // route to. Price as plain Ramulator DRAM (no Garnet traversal).
                    uint32_t lat = remoteLat + 2 * localLinkLat_;
                    if (zinfo->hierarchy.nocAggBandwidthMBs > 0) lat += channelBandwidthWait(req.srcId, req.cycle);
                    // E16: no traversal happens here (see the comment above), so
                    // the fabric is marked only for the MC hop, as in the local
                    // path -- not by falling through to the routed-path marker.
                    if (zinfo->hierarchy.mcStandalone) {
                        lat += mcHopLatency(targetUnit, req.cycle, req.srcId);
                        zinfo->pgres.noc.touch(zinfo->numPhases);
                zinfo->pgres.nocGaps.event(req.cycle);   // E17 measurement
                    }
                    noteRowAccess(targetUnit, req.lineAddr);   // 1.11.52 (D003)
            profRemoteAccesses_.inc();
                    profRemoteLatency_.inc(lat);
                    return req.cycle + lat;
                }
                // E16 site 2: past the degenerate check, every access here is a
                // real packet on the H-tree. dst==src is 0 HOPS but still an
                // injection at the node, so it counts as fabric activity.
                zinfo->pgres.noc.touch(zinfo->numPhases);
                zinfo->pgres.nocGaps.event(req.cycle);   // E17 measurement
                uint32_t srcNode = (uint32_t)mcId_ % gn->getNumNodes();
                uint32_t dstNode = (uint32_t)dstEp % gn->getNumNodes();
                uint32_t networkLat;
                // Synchronous shared-Garnet accounting (the ONLY detailed NoC):
                // record this access and drain the batch on the critical path at
                // each phase boundary. The drain flag keeps the watchdog from
                // misreading the frozen phase clock as a fake-leave stall.
                //   Single process (OMP/device): record on the in-process Garnet.
                //   MPI shared mode (PIMID_NOC_SHM): publish to the cross-rank
                //   shared log instead, and drain the MERGED multi-rank stream --
                //   one logical Garnet driven by all ranks' traffic.
                SharedNoc* sn = sharedNoc();
                // Thread-MPI (one process, N core-threads, one Garnet): NOT the shm
                // path (sn==null). Uses the in-process per-core roiRel cut.
                bool threadDet = mpiThreadDetPricing() && !sn;
                // POST-ROI GUARD: at ROI end the plugin requests termination and
                // the scheduler -- the only writer of globPhaseCycles -- stops
                // advancing it, while instrumentation keeps running until the
                // watchdog ends the sim. Records stamped in that regime carry a
                // FROZEN phase clock and mix eras inside one batch: the replay
                // then ticks through a fake multi-billion-cycle span (hours of
                // wall for a post-ROI checksum loop nobody measures). Nothing
                // reported lives outside the ROI, so the post-ROI tail keeps
                // paying the last measured latencies but feeds the replay
                // NOTHING.
                bool postRoi = zinfo->terminationConditionMet;
                if (!postRoi) {
                if (sn) {
                    // ROI-RELATIVE clock: per-rank absolute cycles carry huge
                    // startup skew (QEMU boot staggering) and are NOT comparable
                    // across ranks -- publishing on them pins the merged-replay
                    // cut at the slowest-starting rank and overruns the rings.
                    // roiRel (cycle - this rank's ROI baseline) is the
                    // established floor-free cross-rank axis (1.3.x rendezvous).
                    // Pre-baseline (startup/warmup) traffic is not published.
                    if (zinfo->hierarchy.mpiNocBaselined) {
                        uint64_t base = zinfo->hierarchy.mpiNocRoiBase;
                        uint64_t rel = (req.cycle > base) ? req.cycle - base : 0;
                        if (srcNode != dstNode)   // src==dst never traverses
                            pimid_noc_publish(sn->h, sn->rank, srcNode, dstNode, rel);
                        else
                            pimid_noc_touch(sn->h, sn->rank, rel);
                    }
                } else if (threadDet) {
                    // Thread-MPI: record ONLY post-ROI-baseline. The pre-ROI init /
                    // data-prep runs on rank 0 SOLO for thousands of phases while the
                    // other ranks spawn (wall-dependent), and its traffic would
                    // otherwise pollute the epoch table and make the ROI pricing
                    // wall-dependent (the FIRST-DIVERGENCE was exactly a pre-ROI
                    // rank-0 access). The record is stamped on the per-core roiRel
                    // axis (skew-free); the replay/fold is NOT done here on the access
                    // path -- it is done at the phase barrier by the single-threaded
                    // barrier cut (foldByCut from EndOfPhaseActions).
                    if (zinfo->hierarchy.mpiNocBaselined)
                        gn->recordBatchAccess(srcNode, dstNode,
                                              perCoreRoiRel(req.srcId, req.cycle));
                } else {
                    // OMP / non-MPI: record on the global phase clock + inline
                    // per-phase processBatch (single process, live EWMA).
                    gn->recordBatchAccess(srcNode, dstNode, phaseStamp(req.cycle));
                    if (zinfo->numPhases > batchLastPhase_) {
                        __sync_fetch_and_add(&zinfo->hierarchy.nocInlineDrain, 1);
                        gn->processBatch(zinfo->numPhases, zinfo->phaseLength);
                        __sync_fetch_and_sub(&zinfo->hierarchy.nocInlineDrain, 1);
                        batchLastPhase_ = zinfo->numPhases;
                    }
                }
                if (sn && zinfo->numPhases > batchLastPhase_) {
                    __sync_fetch_and_add(&zinfo->hierarchy.nocInlineDrain, 1);
                    sharedNocDrain(gn, sn, zinfo->numPhases);
                    __sync_fetch_and_sub(&zinfo->hierarchy.nocInlineDrain, 1);
                    batchLastPhase_ = zinfo->numPhases;
                }
                }  // !postRoi

                // Own unit (src == dst): the packet does not traverse the tree
                // -- 0 network hops, the near-data case the placement model
                // exists to express. The replay itself already treats src==dst
                // as non-traversing (it never enters Garnet); the CHARGE must
                // match, or near-data pays the batch average of everyone
                // else's deep-tree traffic and the placement tiers collapse.
                // Cross-tree accesses use the Garnet-measured RTT (bootstrap
                // with analytical until the first drain publishes).
                if (srcNode == dstNode) {
                    networkLat = 0;
                } else {
                    // 1.9.0 pricing:
                    //  - thread-MPI DEFAULT: epoch-frozen MEASURED Garnet feedback
                    //    (deterministic keyed lookup of the prior epoch's sample);
                    //  - PIMID_MPI_ANALYTICAL_PRICING=1: static analytical override;
                    //  - OMP/non-MPI: live rolling EWMA (unchanged).
                    uint32_t garnetLat;
                    if (mpiThreadDetPricing()) {
                        garnetLat = forceAnalyticalPricing()
                            ? 0
                            : epochFrozenNocLat(gn, sn, req.cycle, req.srcId);
                    } else {
                        garnetLat = gn->getBatchAvgLatency();
                    }
                    networkLat = (garnetLat > 0)
                        ? 2 * garnetLat
                        : 2 * zinfo->hierarchy.nocAvgOneWayLatency;
                }

                uint32_t totalLat = networkLat + remoteLat + 2 * localLinkLat_;
                // DRAM channel bandwidth bottleneck (accuracy fix): the H-tree
                // Garnet replays accesses at their original (load-spread) cycles
                // and resets per drain, so it never models the SHARED DRAM
                // channel saturating — detailed otherwise permits ~num_banks ×
                // per-MI BW. Add a shared M/D/c channel-BW queueing wait so
                // effective aggregate DRAM BW is capped at the datasheet value.
                if (zinfo->hierarchy.nocAggBandwidthMBs > 0) {
                    totalLat += channelBandwidthWait(req.srcId, req.cycle);
                }
                // Standalone MC: extra core → MC node hop on top of routing.
                if (zinfo->hierarchy.mcStandalone) {
                    totalLat += mcHopLatency(targetUnit, req.cycle, req.srcId);
                }
                noteRowAccess(targetUnit, req.lineAddr);   // 1.11.52 (D003)
            profRemoteAccesses_.inc();
                profRemoteLatency_.inc(totalLat);
                return req.cycle + totalLat;
            }

            // ── Simple model: hierarchy traversal + serialization + contention ──
            // E16 site 3: the analytical NoC's remote path. This is the branch
            // the finding was about -- it traverses (computeHierTraversal walks
            // the LCA path and charges hops), so it marks; its LOCAL sibling
            // above does not, and used to.
            zinfo->pgres.noc.touch(zinfo->numPhases);
                zinfo->pgres.nocGaps.event(req.cycle);   // E17 measurement
            // hierLat = per-tier hop-based latency through LCA path (no double-counting)
            uint32_t hierLat = (uint32_t)computeHierTraversal(
                myUnit, targetUnit,
                zinfo->hierarchy.levelLatency, zinfo->hierarchy.bridgeLatency,
                zinfo->hierarchy.placementLevel, zinfo->hierarchy.subarraysPerBank,
                zinfo->hierarchy.banksPerBG, zinfo->hierarchy.bgPerChip,
                zinfo->hierarchy.chipsPerRank, zinfo->hierarchy.ranksPerChannel);

            /* 1.11.57 (audit D013): THE 64 B LINE IS SERIALISED ONCE, BY THE
             * LADDER. This site used to add a wormhole term on top --
             * (nocFlitsPerPacket - 1) cycles each way, with flits_per_packet =
             * ceil(576/128) = 5 from a hardcoded 128-bit link width in
             * src/main.cpp -- so every remote access carried 2 * 4 = 8 PE
             * cycles of serialisation beyond whatever the tiers charged.
             *
             * That was not a double charge until 1.11.56. Before it, B041
             * flattened the whole seven-tier ladder to one scalar on any
             * analytical DRAM run, so this WAS the only serialisation term.
             * B041 stopped flattening (the flattening made an analytical run
             * compare a different hierarchy model against detailed, not a
             * different network model), and since then computeHierTraversal
             * sums per-tier latencies that each already contain
             * ceil(64*8 / tier_width) at the tier's SOURCED width --
             * InternalDRAMNetwork::getTransferLatencyNs over the D064 ladder,
             * converted once at the PE clock. The tiers know the DDR4 chip-DQ
             * tier is 8 bits wide and the rank tier 64; the literal 128 here
             * contradicted both, and it was invisible because each half looks
             * correct in its own file and the release that made them overlap
             * changed neither.
             *
             * Removing THIS one rather than the ladder's: the ladder's width
             * came from the DRAM oracle per tier, the 128 is a Garnet flit
             * size that the fork otherwise never reads (nocLinkWidthBits is
             * parsed into zinfo->hierarchy at init.cpp and consumed nowhere),
             * and it survives only through flits_per_packet.
             * nocFlitsPerPacket still sets the M/M/1 service time below, which
             * is a different quantity (packets per service, not latency), and
             * is left alone. */
            // One-way = hierarchy traversal (serialisation included per tier); RTT = 2x
            uint32_t networkLat = 2 * hierLat;
            uint32_t nocContentionLat = networkContentionLatency();

            uint32_t totalLat;
            if (zinfo->hierarchy.nocMlpModel) {
                // ── Unified analytical model: hop-count + M/D/1 + MLP ──────────
                //   t_eff = max( (L + W_q) / M ,  P*D/c )
                //   L   = unloaded per-access latency (network RTT + DRAM + links)
                //         = networkLat + remoteLat + 2*localLinkLat_   [hop-count]
                //   W_q = M/D/1 contention wait (= nocContentionLat)   [M/D/1]
                //   M   = PE outstanding-access window (MLP); M=1 == 'simple'.
                //   P*D/c = aggregate-bandwidth floor: P PEs share c channels of
                //         deterministic service time D = line/perChanBW; per-PE
                //         floor = P / (aggregate lines-per-cycle). Prevents the
                //         /M overlap from exceeding the physical channel rate.
                // M=1, low load -> L+W_q (today's simple). M large -> bandwidth.
                uint32_t L = networkLat + remoteLat + 2 * localLinkLat_;
                uint32_t M = zinfo->hierarchy.nocMlpDegree;
                if (M < 1) M = 1;
                double overlapped = (double)(L + nocContentionLat) / (double)M;
                double bwFloor = 0.0;
                if (zinfo->hierarchy.nocAggBandwidthMBs > 0) {
                    uint32_t lvl_ = zinfo->hierarchy.placementLevel;
                    uint32_t bwmul_ = (lvl_ == 0) ? 4u : ((lvl_ == 1 || lvl_ == 2) ? 2u : 1u);
                    double aggBps = (double)zinfo->hierarchy.nocAggBandwidthMBs * 1e6 * bwmul_;
                    // Convert at the DEVICE clock, not the global (host) clock:
                    // in system-scope co-sim sys.frequency = max = host, so using
                    // zinfo->freqMHz here would scale the device's bandwidth floor
                    // with the host clock (the host-clock leak). 0 => not system
                    // scope -> freqMHz already IS the device clock.
                    double bwFreqMHz = (zinfo->hierarchy.nocBandwidthFreqMHz > 0)
                        ? (double)zinfo->hierarchy.nocBandwidthFreqMHz
                        : (double)zinfo->freqMHz;
                    double freqHz = bwFreqMHz * 1e6;
                    double lsz    = (zinfo->lineSize > 0) ? (double)zinfo->lineSize : 64.0;
                    double aggLinesPerCyc =
                        (freqHz > 0.0) ? (aggBps / freqHz) / lsz : 0.0;
                    double P = (double)((zinfo->numCores > 0) ? zinfo->numCores : 1);
                    if (aggLinesPerCyc > 1e-12) bwFloor = P / aggLinesPerCyc;
                }
                double te = (overlapped > bwFloor) ? overlapped : bwFloor;
                totalLat = (te >= 1.0) ? (uint32_t)(te + 0.5) : 1;
                // Env-gated term dump (PIMID_MLP_DIAG=1): aggregated means so
                // per-tech cross-checks against detailed are one grep away.
                if (mlpDiagEnabled()) {
                    static std::atomic<uint64_t> dN(0), dL(0), dNet(0), dRem(0),
                                                 dWq(0), dHier(0);
                    dN.fetch_add(1, std::memory_order_relaxed);
                    dL.fetch_add(L, std::memory_order_relaxed);
                    dNet.fetch_add(networkLat, std::memory_order_relaxed);
                    dRem.fetch_add(remoteLat, std::memory_order_relaxed);
                    dWq.fetch_add(nocContentionLat, std::memory_order_relaxed);
                    dHier.fetch_add(hierLat, std::memory_order_relaxed);
                    uint64_t n = dN.load(std::memory_order_relaxed);
                    if ((n & 0xFFFF) == 0) {
                        fprintf(stderr, "[MLPDIAG] n=%lu avgL=%.1f avgNet=%.1f "
                                "avgHier=%.1f avgRem=%.1f avgWq=%.1f M=%u "
                                "bwFloor=%.2f te=%.1f\n",
                                (unsigned long)n, (double)dL/n, (double)dNet/n,
                                (double)dHier/n, (double)dRem/n, (double)dWq/n,
                                M, bwFloor, te);
                    }
                }
            } else if (zinfo->hierarchy.nocCurveModel && zinfo->hierarchy.nocCurveN > 0) {
                // Curve model (2b): estimate the offered per-node load and
                // interpolate the latency-vs-load curve probed at init. Unlike
                // calibrated (zero-load only), this captures CONTENTION from the
                // measured curve. O(1) lookup.
                uint32_t curveLat = curveNetworkLatency();   // one-way network lat
                // Fix 1: stop double-counting the memory path on non-grid.
                // On non-grid topologies (H-tree/DRAM) the synthetic probe
                // traffic already traverses the network to the memory-org node,
                // so the probed curveLat already embeds that path; adding the
                // destination memory remoteLat on top inflates DRAM (calibrated
                // wins there precisely because it drops remoteLat). Keep
                // remoteLat only for grid (MESH/TORUS), where detailed is HIGHER
                // than analytical and the memory term helps close the gap.
                if (zinfo->hierarchy.nocCurveIsGrid) {
                    totalLat = curveLat + remoteLat + 2 * localLinkLat_;
                } else {
                    totalLat = curveLat + 2 * localLinkLat_;
                }
            } else if (zinfo->hierarchy.nocCalQueue && zinfo->hierarchy.nocCurveBaseLat > 0) {
                // Fix 1 (calqueue): use the probe-measured zero-load base L0
                // (accurate base, wins H-tree) but ADD BACK simple's M/D/1
                // contention + memory terms (wins MESH). i.e. simple's full
                // formula with L0 replacing the hop-count networkLat.
                totalLat = zinfo->hierarchy.nocCurveBaseLat + nocContentionLat
                         + remoteLat + 2 * localLinkLat_;
            } else if (zinfo->hierarchy.nocCurveBaseLat > 0) {
                // Calibrated model: the injector-measured per-access traversal
                // latency L0 (from a cycle-accurate probe of this topology)
                // replaces the analytical network estimate. Profiling shows L0 ~=
                // the detailed per-access remote latency, so this tracks detailed
                // far better than the analytical hop count, at simple-mode speed.
                //
                // Fix B (topology-aware): on GRID topologies (MESH/TORUS) the
                // detailed ground truth is HIGHER than this stripped base because
                // of real network contention; the stripped form under-shoots
                // MESH by ~50%. So for grids ADD BACK the M/D/1 contention and
                // memory terms (i.e. simple's full formula, with L0 as the base
                // when it is a usable positive value, else the analytical hop
                // networkLat). Non-grid (H-tree) is LEFT UNTOUCHED: detailed is
                // low there and the stripped base already fits (~25%). Guarded by
                // PIMID_CAL_GRIDQ (default ON) for in-binary A/B validation.
                if (calGridQueueEnabled() && zinfo->hierarchy.nocCurveIsGrid) {
                    // Use the probed L0 as the base if it is a real positive
                    // value; otherwise fall back to the analytical hop networkLat
                    // (so grid degenerates to simple's formula, the ~22% floor).
                    uint32_t gridBase = (zinfo->hierarchy.nocCurveBaseLat > 0)
                        ? zinfo->hierarchy.nocCurveBaseLat : networkLat;
                    totalLat = gridBase + nocContentionLat + remoteLat
                             + 2 * localLinkLat_;
                } else {
                    totalLat = zinfo->hierarchy.nocCurveBaseLat + 2 * localLinkLat_;
                }
            } else {
                totalLat = networkLat + nocContentionLat + remoteLat + 2 * localLinkLat_;
            }
            // Standalone MC: extra core → MC node hop on top of routing.
            if (zinfo->hierarchy.mcStandalone) {
                totalLat += mcHopLatency(targetUnit, req.cycle, req.srcId);
            }
            noteRowAccess(targetUnit, req.lineAddr);   // 1.11.52 (D003)
            profRemoteAccesses_.inc();
            profRemoteLatency_.inc(totalLat);
            return req.cycle + totalLat;
        }
    }

    // Analytical-model term dump toggle (PIMID_MLP_DIAG=1). Diagnostic only.
    static bool mlpDiagEnabled() {
        static const bool en = []() {
            const char* e = getenv("PIMID_MLP_DIAG");
            return (e != nullptr) && (e[0] == '1');
        }();
        return en;
    }

    // detailed mode always drains the shared Garnet synchronously per phase
    // (see processBatch/runBatchDrain_); use noc.model=analytical when speed
    // matters more than cycle accounting.

    // Topology-aware calibrated toggle (Fix B). Default ON: on GRID topologies
    // the calibrated model ADDS the M/D/1 contention + memory terms back onto
    // the probed base L0 so it tracks MESH contention (old stripped form
    // under-shoots MESH ~50%). Set PIMID_CAL_GRIDQ=0 to restore the old
    // stripped calibrated behavior (for in-binary A/B validation).
    static bool calGridQueueEnabled() {
        static const bool en = []() {
            const char* e = getenv("PIMID_CAL_GRIDQ");
            return (e == nullptr) || (e[0] != '0');
        }();
        return en;
    }

    const char* getName() override { return name_.c_str(); }

    void initStats(AggregateStat* parentStat) override {
        AggregateStat* s = new AggregateStat();
        s->init(name_.c_str(), "PE memory interface stats");
        /* 1.11.57 (audit D008): "the unit's open row" was the claim a single
         * register per placement unit could support; the model now holds one
         * per BANK inside the unit, which is what the part does. */
        profRowHits_.init("rowHits", "Accesses hitting the bank's open row (measured)");
        s->append(&profRowHits_);
        profRowMisses_.init("rowMisses", "Accesses opening a different row (measured)");
        s->append(&profRowMisses_);
        profLocalAccesses_.init("localAcc", "Local accesses (within coverage)");
        s->append(&profLocalAccesses_);
        profRemoteAccesses_.init("remoteAcc", "Remote accesses (hierarchy traversal)");
        s->append(&profRemoteAccesses_);
        profLocalLatency_.init("localLat", "Total local access latency cycles");
        s->append(&profLocalLatency_);
        profRemoteLatency_.init("remoteLat", "Total remote access latency cycles");
        s->append(&profRemoteLatency_);
        profReads_.init("rd", "Read requests");
        s->append(&profReads_);
        profWrites_.init("wr", "Write requests");
        s->append(&profWrites_);
        parentStat->append(s);
    }

protected:
    // Count this access into curEpoch (= numPhases/E), FREEZE every epoch strictly
    // below curEpoch via Compute(count), and return the PREVIOUS epoch's frozen
    // value. curEpoch keys on the GLOBAL phase clock, advanced ONLY at the phase
    // barrier, so an epoch below curEpoch is COMPLETE (every core finished all its
    // phases) => its integer count is order-free and its frozen value is
    // deterministic. Frozen epochs are erased from acc (they take no more accesses).
    template <typename Compute>
    static uint32_t epochFrozenScalar(EpochFrozenScalar& s, uint64_t curEpoch,
                                      Compute compute) {
        futex_lock(&s.lk);
        s.acc[curEpoch] += 1;
        while (!s.acc.empty() && s.acc.begin()->first < curEpoch) {
            auto it = s.acc.begin();
            if (!s.frozen.count(it->first))
                s.frozen[it->first] = compute(it->second);
            s.acc.erase(it);
        }
        uint32_t v = 0;
        if (curEpoch > 0) {
            auto it = s.frozen.find(curEpoch - 1);
            if (it != s.frozen.end()) v = it->second;
        }
        futex_unlock(&s.lk);
        return v;
    }

    // Shared M/D/c channel-BW queueing wait (cycles) for an aggregate offered rate
    // (lines/cycle across the whole device). Extracted verbatim from the original
    // channelBandwidthWait phase-update so the rolling-EWMA (OMP) path and the
    // epoch-frozen (thread-MPI) path use one identical formula.
    static uint32_t channelWaitFromRate(double aggArrivalRate) {
        uint64_t aggMBs = zinfo->hierarchy.nocAggBandwidthMBs;
        {
            uint32_t lvl = zinfo->hierarchy.placementLevel;
            aggMBs *= (lvl == 0) ? 4u : ((lvl == 1 || lvl == 2) ? 2u : 1u);
        }
        {
            const char* e = getenv("PIMID_NOC_AGGBW_MBS");
            if (e && e[0]) { long long v = atoll(e); if (v > 0) aggMBs = (uint64_t)v; }
        }
        double aggBytesPerSec = (double)aggMBs * 1e6;
        double bwFreqMHz = (zinfo->hierarchy.nocBandwidthFreqMHz > 0)
            ? (double)zinfo->hierarchy.nocBandwidthFreqMHz
            : (double)zinfo->freqMHz;
        double freqHz = bwFreqMHz * 1e6;
        double lineSize = (double)zinfo->lineSize;
        if (lineSize < 1.0) lineSize = 64.0;
        double aggLinesPerCycle = (freqHz > 0.0)
            ? (aggBytesPerSec / freqHz) / lineSize : 0.0;
        uint32_t c = zinfo->hierarchy.dramChannels;
        if (c < 1) c = 1;
        double perChanSvcRate = aggLinesPerCycle / (double)c;
        double wait = 0.0;
        if (perChanSvcRate > 1e-9) {
            double svcTime = 1.0 / perChanSvcRate;
            /* 1.10.6: the DQ bus pays to reverse direction, and a pure
             * bandwidth limit does not know that. Expected turnaround per
             * access for a mixed stream = 2 * P(read) * P(write) * t_turn
             * (a direction change happens when consecutive accesses differ).
             * All-read or all-write streams pay nothing, exactly as the bus
             * would. t_turn is the technology's own tWTR from its Ramulator2
             * preset, converted here at the SAME clock as svcTime. */
            /* Turnaround is SERVICE, not queueing: the access itself holds
             * the bus for the reversal whether or not anyone is waiting
             * behind it. The first version of this charge lived only inside
             * the queueing term, where rho ~ 0.02 on every measured
             * configuration multiplied it to zero -- the gate caught the
             * penalty never engaging. So the expected turnaround is charged
             * per access directly, AND lengthens the service time the queue
             * sees, so waiting traffic queues behind the reversal too. */
            double turnExp = 0.0;
            if (zinfo->hierarchy.dqTurnNsX100 > 0) {
                uint64_t rd = DqMix::reads.load(std::memory_order_relaxed);
                uint64_t wr = DqMix::writes.load(std::memory_order_relaxed);
                uint64_t tot = rd + wr;
                if (tot > 0) {
                    uint32_t prQ = (uint32_t)((rd * 64) / tot);   // P(read) in 64ths
                    double pr = (double)prQ / 64.0;
                    double turnCyc = ((double)zinfo->hierarchy.dqTurnNsX100 / 100.0)
                                     * bwFreqMHz / 1000.0;
                    turnExp = 2.0 * pr * (1.0 - pr) * turnCyc;
                    svcTime += turnExp;
                }
            }
            double rho = (aggArrivalRate / (double)c) * svcTime;
            if (rho > 0.98) rho = 0.98;
            if (rho > 0.01)
                wait = (rho * svcTime) / (2.0 * (1.0 - rho));
            wait += turnExp;   // the access's own reversal, paid regardless of load
        }
        return (uint32_t)(wait + 0.5);
    }

    uint32_t localAccessLatency(MemReq& req) {
        uint32_t result;
        if (mpiThreadDetPricing() && !sharedNoc() && maxRequestsPerCycle_ > 0.0 &&
            zinfo->hierarchy.mpiNocBaselined) {
            // 1.9.0 thread-MPI: DETERMINISTIC epoch-frozen M/D/1 device time (E2).
            // Count this MI's accesses per ROI-relative epoch; price from the
            // PREVIOUS (barrier-final) epoch's frozen wait -- independent of
            // intra-epoch cross-thread arrival order. The epoch key advances ONLY at
            // the phase barrier, so epoch k-1's count is complete before any epoch-k
            // access reads it. Pre-ROI (not baselined) falls to the base path below.
            uint32_t E = zinfo->garnetNetwork
                ? zinfo->garnetNetwork->detEpochPhases() : 4;
            uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
            uint64_t epochLen = (uint64_t)pl * (E > 0 ? E : 1);
            uint64_t bp = zinfo->hierarchy.mpiNocRoiBasePhase;
            uint64_t rel = (zinfo->numPhases >= bp) ? zinfo->numPhases - bp : 0;
            uint64_t curEpoch = rel / (E > 0 ? E : 1);
            double mrpc = maxRequestsPerCycle_;
            uint32_t nch = numChannels_;
            uint32_t wait = epochFrozenScalar(md1Epoch_, curEpoch,
                [epochLen, mrpc, nch](uint64_t cnt) -> uint32_t {
                    double load = ((double)cnt / (double)epochLen) / mrpc;
                    if (nch > 1) load /= (double)nch;
                    if (load > 0.95) load = 0.95;
                    double svcTime = 1.0 / mrpc;
                    double waitTime = (load > 0.01)
                        ? (load * svcTime) / (2.0 * (1.0 - load))
                        : 0.0;
                    return (uint32_t)(waitTime + 0.5);
                });
            result = localLatency_ + wait;
        } else {
            // OMP / non-MPI (or no BW model): original rolling-EWMA path, kept
            // byte-identical so the live-feedback single-process behavior is
            // unchanged.
            // M/D/1 update on phase boundary (when bandwidth is configured)
            if (maxRequestsPerCycle_ > 0.0 && zinfo->numPhases > lastPhase_) {
                futex_lock(&updateLock_);
                if (zinfo->numPhases > lastPhase_) {
                    double alpha = 0.5;
                    smoothedPhaseAccesses_ = (1.0 - alpha) * smoothedPhaseAccesses_
                                             + alpha * (double)curPhaseAccesses_;
                    curPhaseAccesses_ = 0;

                    double load = smoothedPhaseAccesses_ /
                                  (zinfo->phaseLength * maxRequestsPerCycle_);

                    // TEMP (pre-arXiv) M/D/c stop-gap: numChannels_ independent DRAM
                    // channels serve requests in parallel, so the offered load is
                    // spread across c servers — effective per-server utilization is
                    // load / c. High-channel technologies (HBM) therefore stay
                    // unsaturated where a single-channel part (DDR) would queue.
                    // Approximated by reducing the utilization that drives the
                    // standard M/D/1 wait formula (a true M/D/c is future work).
                    if (numChannels_ > 1) load /= (double)numChannels_;
                    if (load > 0.95) load = 0.95;

                    // M/D/1: E[T] = S + (rho * S) / (2 * (1 - rho))
                    double svcTime = 1.0 / maxRequestsPerCycle_;
                    double waitTime = (load > 0.01)
                        ? (load * svcTime) / (2.0 * (1.0 - load))
                        : 0.0;
                    curLatency_ = localLatency_ + (uint32_t)(waitTime + 0.5);
                    lastPhase_ = zinfo->numPhases;
                }
                futex_unlock(&updateLock_);
            }

            curPhaseAccesses_++;
            result = curLatency_;
        }

        // Cacheless PEs issue stores as GETX and never emit a PUTX writeback,
        // so at this interface GETX IS the memory write. PUTS (clean wback)
        // is not a real access and counts as neither.
        if (req.type == GETS) {
            profReads_.inc();
        } else if (req.type != PUTS) {
            profWrites_.inc();
        }

        return result;
    }

    // Standalone-MC extra hop: the MC fronting memory org `targetUnit` is a
    // distinct NoC endpoint (one per memory org), so reaching it costs one NoC
    // RTT (core → MC node → and back) charged on top of the access. In
    // cycle-accurate mode this routes real packets through Garnet so the MC
    // traffic shows up in network contention; otherwise it is analytical.
    uint32_t mcHopLatency(uint32_t targetUnit, uint64_t cycle, uint32_t srcId = 0) {
        (void)srcId;
        GarnetNetwork* gn = zinfo->garnetNetwork;
        if (gn && gn->isCycleAccurate()) {
            uint32_t srcNode = representativeUnit() % gn->getNumNodes();
            // MC node for memory org `targetUnit` is modeled as node `targetUnit`.
            uint32_t mcNode = targetUnit % gn->getNumNodes();
            // Post-ROI: charge, but never record (POST-ROI GUARD in access()).
            if (!zinfo->terminationConditionMet) {
                // Global-phase-clock stamp; thread-MPI folds at the barrier, OMP
                // drains inline (single process).
                gn->recordBatchAccess(srcNode, mcNode, phaseStamp(cycle));
                if (!mpiThreadDetPricing() && zinfo->numPhases > batchLastPhase_) {
                    __sync_fetch_and_add(&zinfo->hierarchy.nocInlineDrain, 1);
                    gn->processBatch(zinfo->numPhases, zinfo->phaseLength);
                    __sync_fetch_and_sub(&zinfo->hierarchy.nocInlineDrain, 1);
                    batchLastPhase_ = zinfo->numPhases;
                }
            }
            uint32_t garnetLat;
            if (mpiThreadDetPricing()) {
                garnetLat = forceAnalyticalPricing()
                    ? 0
                    : epochFrozenNocLat(gn, nullptr, cycle, srcId);
            } else {
                garnetLat = gn->getBatchAvgLatency();
            }
            return (garnetLat > 0)
                ? 2 * garnetLat
                : 2 * zinfo->hierarchy.nocAvgOneWayLatency;
        }
        // Simple model: one analytical NoC RTT to the MC node.
        return 2 * zinfo->hierarchy.nocAvgOneWayLatency;
    }

    /* 1.11.52 (D003): a last-open-row register. A row hit is an access to a row
     * already open in that unit; anything else is a miss that pays
     * activate+precharge. Armed only when the row size is known
     * (zinfo->hierarchy.dramRowBytes > 0), so nothing is claimed for a
     * configuration where the row geometry was never established.
     *
     * 1.11.57 (audit D006): THE ROW NUMBER IS COMPUTED ON THE RANK'S ADDRESS
     * SPACE, NOT ON ONE DEVICE'S PAGE. `lineAddr << 6` is a SYSTEM byte
     * address, but dramRowBytes is the PER-DEVICE page (1 KB for DDR3/4/5),
     * and on a DDR-class 64-bit rank a single 64 B access engages every chip
     * in the rank at once -- which the energy layer this measurement feeds
     * already knows and prices (pimid_energy.h devicesPerAccess(): "a 64 B
     * access on a DDR-class 64-bit rank engages EVERY chip in the rank
     * simultaneously"). One ACT therefore opens chipsPerRank chip-rows that
     * together span chipsPerRank KB of system address space, while the old
     * `addr / 1024` declared a NEW row every 1 KB. The two PIMID additions
     * contradicted each other about the same access, and the disagreement was
     * invisible because each looks self-consistent in its own file: the miss
     * fraction is printed as MEASURED, so nobody re-derived it. For a pure
     * 64 B sequential stream at x8 it reported 0.0625 where the part delivers
     * 0.0078 -- 8x -- and on DDR4 (e_actpre ~1.42 nJ against e_rd ~0.39 nJ)
     * that is roughly +19% on per-access read energy.
     *
     * 1.11.57 (audit D008): ONE OPEN ROW PER BANK, NOT PER PLACEMENT UNIT.
     * `unit` is addrToUnit()'s index at the CONFIGURED PLACEMENT LEVEL. At
     * BANK placement a unit is a bank and a single register is right; at CHIP,
     * RANK or CHANNEL placement a unit contains 4-16 banks, each holding its
     * own open row in the part being priced, and one register reported a miss
     * every time the stream alternated banks -- biasing the fraction toward
     * 1.0 at exactly the coarse placements the corpus uses. The bank index is
     * taken from the standard JEDEC field order (column low, bank/bank-group
     * next, row high), i.e. bank = stripe_index % banks_in_unit and
     * row = stripe_index / banks_in_unit.
     *
     * 1.11.57 (audit D007): NO RESIZE ON THE ACCESS PATH. lastRow_ was grown
     * with resize() from access(), which several PE threads enter concurrently
     * whenever pesPerMC > 1 (init.cpp maps miIdx = aluCoreIdx / pesPerMC), and
     * `unit` ranges over ALL units because remote call sites pass their target
     * -- so the vector reallocated while other threads dereferenced
     * lastRow_[unit]: a read of freed storage, not merely a lost count. The
     * table is now sized once, at construction, from the unit count the
     * interface is built with, and the per-slot update uses relaxed atomics.
     * A concurrent update can still cost one classification (two PEs racing on
     * the same bank slot), which is a counter question, not a memory-safety
     * one, and the two Counters on this path are already incremented without a
     * lock. */
    inline void noteRowAccess(uint32_t unit, Address lineAddr) {
        if (rowStrideBytes_ == 0 || lastRow_.empty()) return;
        const uint64_t stripe = (uint64_t)(lineAddr << 6) / rowStrideBytes_;
        const uint32_t bank = (uint32_t)(stripe % rowSlotsPerUnit_);
        const uint64_t row = stripe / rowSlotsPerUnit_;
        const size_t slot = (size_t)unit * rowSlotsPerUnit_ + bank;
        if (slot >= lastRow_.size()) return;   // out-of-range unit: claim nothing
        if (__atomic_load_n(&lastRow_[slot], __ATOMIC_RELAXED) == row) {
            profRowHits_.inc();
        } else {
            profRowMisses_.inc();
            __atomic_store_n(&lastRow_[slot], row, __ATOMIC_RELAXED);
        }
    }

    /* 1.11.57 (audit D006/D007/D008): the row model's geometry, resolved once
     * at construction so nothing on the access path has to allocate.
     *
     * rowStrideBytes_ -- system bytes made resident by one ACT: the
     * per-device page times the number of devices one access engages.
     * 1.11.91 (audit R8-1): SENT BY THE EMITTER as
     * sys.hierarchy.dramRowStrideBytes (src/main.cpp, from
     * RamulatorWrapper::getDevicesPerAccess() -- the rule the array energy
     * uses), which is the "durable form" the 1.11.57 note here called for.
     * DDR3/DDR4 x8: 8 x page; DDR5 x8 (32-bit sub-channel): 4 x page;
     * LPDDR5/GDDR6: one channel's page; HBM2/HBM3: one pseudo-channel's
     * page. The old reconstruction keyed on "the DDR classes are the ones
     * with a 1 KB page", which stopped being true in 1.11.66.
     *
     * rowSlotsPerUnit_ -- how many independent open rows a placement unit
     * holds, i.e. the banks below the placement level. Level codes follow the
     * emitter: 0 subarray, 1 bank, 2 bank group, 3 chip, 4 rank, 5 channel,
     * 6 logic die.
     * At and below BANK a unit is one row buffer. Above it, the banks
     * multiply; the chip level does NOT multiply on a striped rank, because
     * the chips of a rank operate in lock step and their same-numbered banks
     * are one logical bank as far as the address stream is concerned.
     *
     * 1.11.60 (audit round 4, D005): THE ENUMERATION ABOVE USED TO STOP AT 5,
     * and so did the switch. The emitter has SEVEN levels -- src/main.cpp maps
     * "LOGIC_DIE" to pe_hierarchy_level = 6 -- so LOGIC_DIE fell into the
     * `default` arm and was handed the CHANNEL tier's register count, i.e. the
     * channel dimension was dropped. That is the same defect D008 removed from
     * CHIP/RANK/CHANNEL one release ago, surviving at the one tier D008 did
     * not enumerate. A logic-die unit fronts every channel of the stack, so it
     * is the channel tier times dramChannels; hierarchy.dramChannels was
     * parsed all along (init.cpp) and this function simply never read it.
     * Invisible because `default` is a legal-looking arm: nothing warns, the
     * count it produces is a plausible number, and the only symptom is that
     * ROW_MISS_FRAC -- reported as MEASURED -- is driven toward 1.0 at exactly
     * this placement, which reads as "the logic die has poor locality" rather
     * than as a missing switch case. On HBM3 the table held bpg*bgc*rpc = 32
     * registers for a die with 16 channels x 32 banks = 512, so 16 physically
     * independent banks shared one register and every alternation between them
     * was recorded as a row miss.
     *
     * Note that hierarchy_util.h's decodeUnitPosition() calls level 6 SYSTEM
     * while the emitter calls it LOGIC_DIE; they are the same code and the
     * same tier (the top of the in-stack tree), and only the name differs. */
    void initRowModel_() {
        const uint32_t rowBytes = zinfo ? zinfo->hierarchy.dramRowBytes : 0;
        if (rowBytes == 0) { rowStrideBytes_ = 0; return; }
        /* 1.11.91 (audit R8-1): the stride is the EMITTER'S, explicitly. The
         * `(rowBytes == 1024) ? chipsPerRank : 1` reconstruction that stood
         * here keyed on a page size that stopped identifying the DDR classes
         * when 1.11.66 made the page preset-derived (HBM2/HBM3 = 1 KB per
         * pseudo-channel, DDR3 x8 = 2 KB): it multiplied HBM's page by the
         * channels per stack and left DDR3's at one device. A config that
         * carries a page but no stride (written before 1.11.91) gets NO row
         * measurement rather than a reconstructed one, and says so; the
         * energy consumer then states its own fallback. */
        const uint32_t stride = zinfo->hierarchy.dramRowStrideBytes;
        if (stride == 0) {
            info("[%s] row-buffer model: sys.hierarchy.dramRowBytes = %u but no "
                 "dramRowStrideBytes (config written before 1.11.91); the "
                 "row-miss fraction is NOT measured.",
                 name_.c_str(), rowBytes);
            rowStrideBytes_ = 0;
            return;
        }
        rowStrideBytes_ = (uint64_t)stride;

        const uint32_t bpg = (zinfo->hierarchy.banksPerBG > 0)
                             ? zinfo->hierarchy.banksPerBG : 1u;
        const uint32_t bgc = (zinfo->hierarchy.bgPerChip > 0)
                             ? zinfo->hierarchy.bgPerChip : 1u;
        const uint32_t rpc = (zinfo->hierarchy.ranksPerChannel > 0)
                             ? zinfo->hierarchy.ranksPerChannel : 1u;
        /* 1.11.60 (audit round 4, D005): the channel count, for level 6. */
        const uint32_t chn = (zinfo->hierarchy.dramChannels > 0)
                             ? zinfo->hierarchy.dramChannels : 1u;
        uint32_t slots = 1;
        switch (zinfo->hierarchy.placementLevel) {
            case 0: case 1: slots = 1; break;              // subarray, bank
            case 2: slots = bpg; break;                    // bank group
            case 3: slots = bpg * bgc; break;              // chip
            case 4: slots = bpg * bgc; break;              // rank (chips lock-step)
            case 5: slots = bpg * bgc * rpc; break;        // channel
            case 6: slots = bpg * bgc * rpc * chn; break;  // logic die: all channels (D005)
            default: slots = bpg * bgc * rpc; break;       // unknown: channel tier
        }
        if (slots == 0) slots = 1;
        /* Bound the table: the product is the number of row buffers the whole
         * placement level holds, which is a physical count, but a hand-written
         * config could make it absurd. 1 Mi slots is 8 MB and far past any
         * real part; beyond that, fall back to one register per unit and say
         * so rather than allocating without limit. */
        uint64_t total = (uint64_t)totalUnits_ * slots;
        if (total > (1ull << 20)) {
            info("[%s] row-buffer model: %llu open-row registers exceeds the "
                 "1Mi bound; falling back to one per unit (the measured "
                 "row-miss fraction will be biased high at this placement).",
                 name_.c_str(), (unsigned long long)total);
            slots = 1;
            total = totalUnits_;
        }
        rowSlotsPerUnit_ = slots;
        lastRow_.assign((size_t)total, ~0ull);
    }

    uint32_t addrToUnit(Address lineAddr) const {
        // Device-local CONTIGUOUS block mapping (point-1 fix): each unit owns a
        // contiguous address range (pagesPerUnit 4KB-pages, sized by subarray
        // capacity x subarrays-per-unit), so a PE's contiguous working set lands
        // in its contiguous coverage and is served locally -- instead of the
        // host's page-interleaved layout (page % totalUnits) that scattered every
        // PE's data across all units (the flat-placement bug).
        uint32_t page = (uint32_t)(lineAddr >> 6);
        uint32_t ppu = (zinfo->hierarchy.pagesPerUnit > 0)
                       ? zinfo->hierarchy.pagesPerUnit : 1;
        uint32_t unit = (page / ppu) % totalUnits_;
        // MPI: each rank is a SEPARATE guest process with its own (identical-
        // looking) VA space. Without an offset, every rank's pages would map to
        // the SAME units -- N processes falsely stacked on one region. Offset
        // rank r's mapping into r's own slice (first-touch by the local PE): the
        // honest emulation of per-rank physical placement, and what makes the
        // shared multi-rank Garnet stream meaningful (rank r sources from AND
        // homes in its own region).
        uint32_t rr = mpiRank_(), rn = mpiRanks_();
        if (rn > 1) {
            uint64_t upr = totalUnits_ / rn;      // units per rank (>=0)
            if (upr == 0) upr = 1;
            unit = (uint32_t)((unit + (uint64_t)rr * upr) % totalUnits_);
        }
        return unit;
    }

    static uint32_t mpiRank_() {
        static const uint32_t r = []() {
            const char* e = getenv("PIMID_MPI_RANK");
            return e ? (uint32_t)atoi(e) : 0u;
        }();
        return r;
    }
    static uint32_t mpiRanks_() {
        static const uint32_t n = []() {
            const char* e = getenv("PIMID_MPI_RANKS");
            return e ? (uint32_t)atoi(e) : 1u;
        }();
        return n;
    }

    bool isLocal(uint32_t unit_id) const {
        if (!nonContiguous_) {
            return unit_id >= coverageStart_ && unit_id < coverageEnd_;
        }
        // Binary search for non-contiguous sets
        return std::binary_search(coverageSet_.begin(), coverageSet_.end(), unit_id);
    }

    uint32_t representativeUnit() const {
        return coverageStart_;
    }

    // ── Topology-aware network contention (shared across all PE-MIs) ──
    //
    // Three regimes matching cross-validated Garnet results:
    //  A) BUS: shared medium, M/M/1 with (1-ρ)^1.5 divergence
    //  B) CROSSBAR: per-output M/M/1 with HOL factor 1.58
    //  C) Multi-hop: per-channel M/M/1 × avgHops, hotspot-adjusted
    //
    // M/M/1 chosen over M/D/1 because wormhole blocking and backpressure
    // cascades create higher service time variance than deterministic.
    uint32_t networkContentionLatency() {
        static volatile uint32_t nocRemoteAccesses = 0;
        static volatile double   nocSmoothedRate = 0.0;
        static volatile uint64_t nocLastPhase = 0;
        static volatile uint32_t nocCurContentionLat = 0;
        static lock_t            nocLock = {0};

        __sync_fetch_and_add(&nocRemoteAccesses, 1);

        // Update on phase boundary
        if (zinfo->numPhases > nocLastPhase) {
            futex_lock(&nocLock);
            if (zinfo->numPhases > nocLastPhase) {
                double alpha = 0.5;
                double accesses = (double)nocRemoteAccesses;
                nocSmoothedRate = (1.0 - alpha) * nocSmoothedRate + alpha * accesses;
                nocRemoteAccesses = 0;

                uint32_t flitsPerPkt = zinfo->hierarchy.nocFlitsPerPacket;
                if (flitsPerPkt < 1) flitsPerPkt = 1;
                double svcTime = (double)flitsPerPkt;
                double N = (double)zinfo->hierarchy.nocNumNodes;
                if (N < 2.0) N = 2.0;
                double arrivalRate = nocSmoothedRate / (double)zinfo->phaseLength;

                // VC HOL-blocking reduction: 1/sqrt(VCs) factor (Dally & Towles)
                uint32_t vcs = zinfo->hierarchy.nocVcsPerVnet;
                double vcFactor = (vcs > 1) ? 1.0 / std::sqrt((double)vcs) : 1.0;

                double baseLatency = (double)zinfo->hierarchy.nocAvgOneWayLatency;
                if (baseLatency < 1.0) baseLatency = 1.0;

                uint32_t topoClass = zinfo->hierarchy.nocTopologyClass;
                double waitCycles = 0.0;

                // RATE-SEMANTICS FIX: arrivalRate above is the AGGREGATE
                // injection rate (total remote accesses/cycle across ALL
                // injecting PEs), NOT a per-node rate. The old code multiplied
                // it by N (total network endpoints, mostly passive memory
                // banks) as if it were per-node, inflating offered load by
                // N/P. Harmless when P ~= N (the calibrated mesh cells), but
                // on deep DRAM trees (4 PEs on a 256-endpoint HBM3 tree) it
                // overstated load ~64x and pinned W_q at the saturation clamp,
                // inverting the cross-tech ordering. All three branches now
                // use the aggregate rate directly.
                if (topoClass == 0) {
                    // BUS: every access crosses the single shared medium.
                    // Offered load = aggregate rate x service time.
                    // Steeper divergence: (1-rho)^1.5 for round-robin + HOL.
                    double rho = arrivalRate * svcTime;
                    if (rho >= 0.90) {
                        waitCycles = 10.0 * baseLatency;
                    } else if (rho > 0.01) {
                        double denom = std::pow(1.0 - rho, 1.5);
                        waitCycles = rho * svcTime / denom;
                    }
                } else if (topoClass == 1) {
                    // CROSSBAR: aggregate load spreads uniformly over N output
                    // ports; per-output M/M/1 with HOL factor 1.58.
                    double rhoPerOutput = (arrivalRate / N) * svcTime;
                    double rhoEff = std::min(rhoPerOutput * 1.58, 0.95);
                    if (rhoEff > 0.01) {
                        waitCycles = rhoEff * svcTime / (1.0 - rhoEff);
                    }
                } else {
                    // Multi-hop: per-channel M/M/1 × avgHops
                    double avgHops = (double)zinfo->hierarchy.nocAvgHopsTimes100 / 100.0;
                    if (avgHops < 0.01) avgHops = 0.01;
                    int channels = (int)zinfo->hierarchy.nocTotalChannels;
                    if (channels < 1) channels = 1;
                    double hotspot = (double)zinfo->hierarchy.nocHotspotFactor100 / 100.0;

                    // Aggregate flit-hops per cycle spread over all channels
                    double totalFlitHops = arrivalRate * svcTime * avgHops;
                    double rhoAvg = totalFlitHops / (double)channels;

                    // Bottleneck channel (hotspot-adjusted)
                    double rhoMax = rhoAvg * hotspot;

                    if (rhoMax >= 0.75) {
                        waitCycles = 10.0 * baseLatency;
                    } else if (rhoMax > 0.01) {
                        // M/M/1 per channel: E[W] = ρ×S/(1-ρ), with VC factor
                        double waitPerHop = vcFactor * rhoMax * svcTime / (1.0 - rhoMax);
                        waitCycles = waitPerHop * avgHops;
                    }
                }

                nocCurContentionLat = (uint32_t)(waitCycles + 0.5);
                nocLastPhase = zinfo->numPhases;
            }
            futex_unlock(&nocLock);
        }

        return nocCurContentionLat;
    }

    // ── Detailed-mode DRAM channel bandwidth bottleneck (accuracy fix) ──
    //
    // PROBLEM: in detailed mode each bank-MI runs an INDEPENDENT M/D/1 server at
    // its own per-MI bandwidth (defaultBandwidthMBs). With num_banks MIs (16),
    // the model permits ~num_banks × per-MI-BW aggregate (e.g. DDR4: 16×12.8 =
    // 204.8 GB/s) — ~10× the real DDR4-2400 single-channel 19.2 GB/s. The DRAM
    // CHANNEL (the shared 64-bit DQ bus) is the true bandwidth bottleneck and was
    // never modeled, so detailed under-contends and reports unrealistically high
    // effective DRAM bandwidth ("too optimistic").
    //
    // FIX: model the channel as a SHARED resource. All bank-MIs of a memory
    // technology share `c` channels (DDR4=1, HBM2=8, HBM3=16), each serving at
    // perChannelBW = aggBW / c. We keep a process-wide EWMA of the AGGREGATE
    // remote-access rate across ALL MIs and compute an M/D/c queueing wait that
    // diverges as the aggregate offered load approaches the aggregate channel
    // bandwidth. Added to every DRAM access in detailed mode, this caps the
    // effective aggregate DRAM bandwidth at the datasheet sustainable value.
    //
    // Only active when nocAggBandwidthMBs > 0 (DRAM techs) AND detailed cycle-
    // accurate mode (gated by the caller). Returns the queueing wait in cycles.
    uint32_t channelBandwidthWait(uint32_t srcId = 0, uint64_t reqCycle = 0) {
        static volatile uint64_t cbwAccesses = 0;     // aggregate accesses this phase
        static volatile double   cbwSmoothedRate = 0.0;  // EWMA accesses/phase
        static volatile uint64_t cbwLastPhase = 0;
        static volatile uint32_t cbwWait = 0;         // published wait cycles
        static lock_t            cbwLock = {0};

        // DEBUG: PIMID_NOC_CBW_FORCE=N forces a constant N-cycle wait per access
        // (bypasses the EWMA) to verify the wait actually reaches curCycle.
        {
            static const long forceW = []() {
                const char* e = getenv("PIMID_NOC_CBW_FORCE");
                return e && e[0] ? atol(e) : -1L;
            }();
            if (forceW >= 0) return (uint32_t)forceW;
        }

        // 1.9.0 thread-MPI: DETERMINISTIC epoch-frozen channel-BW wait (E2). The
        // rolling process-wide EWMA below is closed at a wall-timed phase boundary
        // and baked a +-1 phase into COMPUTE-COMPLETE. Instead, count the aggregate
        // accesses per numPhases-epoch and price from the PREVIOUS (barrier-final)
        // epoch's frozen M/D/c wait -- a keyed lookup of an order-free integer count.
        (void)srcId; (void)reqCycle;
        if (mpiThreadDetPricing() && !sharedNoc()) {
            if (!zinfo->hierarchy.mpiNocBaselined) return 0;  // pre-ROI: no wait
            static EpochFrozenScalar cbwEpoch;
            uint32_t E = zinfo->garnetNetwork
                ? zinfo->garnetNetwork->detEpochPhases() : 4;
            uint32_t pl = zinfo->phaseLength > 0 ? zinfo->phaseLength : 10000;
            uint64_t epochLen = (uint64_t)pl * (E > 0 ? E : 1);
            uint64_t bp = zinfo->hierarchy.mpiNocRoiBasePhase;
            uint64_t rel = (zinfo->numPhases >= bp) ? zinfo->numPhases - bp : 0;
            uint64_t curEpoch = rel / (E > 0 ? E : 1);
            return epochFrozenScalar(cbwEpoch, curEpoch,
                [epochLen](uint64_t cnt) -> uint32_t {
                    double rate = (double)cnt / (double)epochLen;
                    return channelWaitFromRate(rate);
                });
        }

        __sync_fetch_and_add(&cbwAccesses, 1);

        if (zinfo->numPhases > cbwLastPhase) {
            futex_lock(&cbwLock);
            if (zinfo->numPhases > cbwLastPhase) {
                double alpha = 0.5;
                double acc = (double)cbwAccesses;
                cbwSmoothedRate = (1.0 - alpha) * cbwSmoothedRate + alpha * acc;
                cbwAccesses = 0;

                // Aggregate offered rate (lines/cycle across the whole device).
                double aggArrivalRate = cbwSmoothedRate / (double)zinfo->phaseLength;

                // Channel service rate (lines/cycle per channel).
                //   aggBW [B/s] -> bytes/cycle = aggBW / freqHz ; lines = /lineSize
                // Datasheet aggregate BW (MB/s); overridable via PIMID_NOC_AGGBW_MBS
                // for validation A/B (force-saturate to prove the cap engages).
                /* 1.10.6: ONE formula. This block was the original the
                 * epoch-frozen MPI path's channelWaitFromRate() was extracted
                 * from -- extracted, but this copy was never rerouted, so the
                 * two paths could drift. They did: the DQ-turnaround charge
                 * landed only in the helper, and every OpenMP run sailed past
                 * it -- the gate showed on == off to the cycle. The duplicate
                 * dies here; both runtimes now price from the same function. */
                cbwWait = channelWaitFromRate(aggArrivalRate);
                cbwLastPhase = zinfo->numPhases;
                if (zinfo->numPhases <= 4 || zinfo->numPhases % 50 == 0)
                    info("[ChanBW] phase=%lu offRate=%.5f wait=%u",
                         zinfo->numPhases, aggArrivalRate, cbwWait);
            }
            futex_unlock(&cbwLock);
        }
        return cbwWait;
    }

    // ── Curve model (2b): load-dependent network latency by interpolation ──
    //
    // The init-time probe injected synthetic UNIFORM traffic at several
    // per-node injection rates and recorded (rate -> avg one-way latency).
    // Here we estimate the current offered per-node load and linearly
    // interpolate that curve.
    //
    // Offered-load estimate: like networkContentionLatency(), we keep a shared
    // EWMA of remote accesses per phase across all PE-MIs. The probe's
    // injectionRate is PER-NODE packets/cycle, so:
    //     perNodeRate = (smoothedRemoteAccesses / phaseLength) / numNodes
    // which is directly comparable to the probed rate axis.
    uint32_t curveNetworkLatency() {
        static volatile uint32_t curveRemoteAccesses = 0;
        static volatile double   curveSmoothedRate = 0.0;
        static volatile uint64_t curveLastPhase = 0;
        static volatile uint32_t curvePublishedLat = 0;
        static lock_t            curveLock = {0};

        __sync_fetch_and_add(&curveRemoteAccesses, 1);

        if (zinfo->numPhases > curveLastPhase) {
            futex_lock(&curveLock);
            if (zinfo->numPhases > curveLastPhase) {
                double alpha = 0.5;
                double accesses = (double)curveRemoteAccesses;
                curveSmoothedRate = (1.0 - alpha) * curveSmoothedRate + alpha * accesses;
                curveRemoteAccesses = 0;

                double N = (double)zinfo->hierarchy.nocNumNodes;
                if (N < 1.0) N = 1.0;
                double aggRate = curveSmoothedRate / (double)zinfo->phaseLength;
                double perNodeRate = aggRate / N;

                curvePublishedLat = (uint32_t)(interpCurve(perNodeRate) + 0.5);
                curveLastPhase = zinfo->numPhases;
            }
            futex_unlock(&curveLock);
        }

        // Bootstrap (phase 0, before any update): use zero-load point.
        if (curvePublishedLat == 0)
            return (uint32_t)(zinfo->hierarchy.nocCurveLat[0] + 0.5);
        return curvePublishedLat;
    }

    // Linear interpolation of the probed latency-vs-load curve. Clamps below the
    // lowest probed rate to L0 and above the highest probed rate to the last
    // (saturated) point — the curve is intentionally bounded at probe time.
    double interpCurve(double rate) const {
        uint32_t n = zinfo->hierarchy.nocCurveN;
        const double* R = zinfo->hierarchy.nocCurveRates;
        const double* L = zinfo->hierarchy.nocCurveLat;
        if (n == 0) return 0.0;
        if (n == 1 || rate <= R[0]) return L[0];
        if (rate >= R[n - 1]) return L[n - 1];
        for (uint32_t i = 1; i < n; ++i) {
            if (rate <= R[i]) {
                double t = (rate - R[i - 1]) / (R[i] - R[i - 1]);
                return L[i - 1] + t * (L[i] - L[i - 1]);
            }
        }
        return L[n - 1];
    }
};

#endif  // PE_MEMORY_INTERFACE_H_
