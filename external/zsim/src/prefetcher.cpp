/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "prefetcher.h"
#include "bithacks.h"
#include "dram_epoch_replay.h"
#include "event_recorder.h"
#include "timing_event.h"
#include "zsim.h"

//#define DBG(args...) info(args)
#define DBG(args...)

void StreamPrefetcher::setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network) {
    childId = _childId;
    if (parents.size() != 1) panic("Must have one parent");
    if (network) panic("Network not handled");
    parent = parents[0];
}

void StreamPrefetcher::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    if (children.size() != 1) panic("Must have one children");
    if (network) panic("Network not handled");
    child = children[0];
}

void StreamPrefetcher::initStats(AggregateStat* parentStat) {
    AggregateStat* s = new AggregateStat();
    s->init(name.c_str(), "Prefetcher stats");
    profAccesses.init("acc", "Accesses"); s->append(&profAccesses);
    profPrefetches.init("pf", "Issued prefetches"); s->append(&profPrefetches);
    profDoublePrefetches.init("dpf", "Issued double prefetches"); s->append(&profDoublePrefetches);
    profPageHits.init("pghit", "Page/entry hit"); s->append(&profPageHits);
    profHits.init("hit", "Prefetch buffer hits, short and full"); s->append(&profHits);
    profShortHits.init("shortHit", "Prefetch buffer short hits"); s->append(&profShortHits);
    profStrideSwitches.init("strideSwitches", "Predicted stride switches"); s->append(&profStrideSwitches);
    profLowConfAccs.init("lcAccs", "Low-confidence accesses with no prefetches"); s->append(&profLowConfAccs);
    parentStat->append(s);
}

uint64_t StreamPrefetcher::access(MemReq& req) {
    uint32_t origChildId = req.childId;
    req.childId = childId;

    if (req.type != GETS) return parent->access(req); //other reqs ignored, including stores

    profAccesses.inc();

    uint64_t reqCycle = req.cycle;
    uint64_t respCycle = parent->access(req);

    Address pageAddr = req.lineAddr >> 6;
    uint32_t pos = req.lineAddr & (64-1);
    uint32_t idx = 16;
    // This loop gets unrolled and there are no control dependences. Way faster than a break (but should watch for the avoidable loop-carried dep)
    for (uint32_t i = 0; i < 16; i++) {
        bool match = (pageAddr == tag[i]);
        idx = match?  i : idx;  // ccmov, no branch
    }

    DBG("%s: 0x%lx page %lx pos %d", name.c_str(), req.lineAddr, pageAddr, pos);

    if (idx == 16) {  // entry miss
        uint32_t cand = 16;
        uint64_t candScore = -1;
        //uint64_t candScore = 0;
        for (uint32_t i = 0; i < 16; i++) {
            if (array[i].lastCycle > reqCycle + 500) continue;  // warm prefetches, not even a candidate
            /*uint64_t score = (reqCycle - array[i].lastCycle)*(3 - array[i].conf.counter());
            if (score > candScore) {
                cand = i;
                candScore = score;
            }*/
            if (array[i].ts < candScore) {  // just LRU
                cand = i;
                candScore = array[i].ts;
            }
        }

        if (cand < 16) {
            idx = cand;
            array[idx].alloc(reqCycle);
            array[idx].lastPos = pos;
            array[idx].ts = timestamp++;
            tag[idx] = pageAddr;
        }
        DBG("%s: MISS alloc idx %d", name.c_str(), idx);
    } else {  // entry hit
        profPageHits.inc();
        Entry& e = array[idx];
        array[idx].ts = timestamp++;
        DBG("%s: PAGE HIT idx %d", name.c_str(), idx);

        // 1. Did we prefetch-hit?
        bool shortPrefetch = false;
        if (e.valid[pos]) {
            uint64_t pfRespCycle = e.times[pos].respCycle;
            shortPrefetch = pfRespCycle > respCycle;
            e.valid[pos] = false;  // close, will help with long-lived transactions
            respCycle = MAX(pfRespCycle, respCycle);
            e.lastCycle = MAX(respCycle, e.lastCycle);
            profHits.inc();
            if (shortPrefetch) profShortHits.inc();
            DBG("%s: pos %d prefetched on %ld, pf resp %ld, demand resp %ld, short %d", name.c_str(), pos, e.times[pos].startCycle, pfRespCycle, respCycle, shortPrefetch);
        }

        // 2. Update predictors, issue prefetches
        int32_t stride = pos - e.lastPos;
        DBG("%s: pos %d lastPos %d lastLastPost %d e.stride %d", name.c_str(), pos, e.lastPos, e.lastLastPos, e.stride);
        if (e.stride == stride) {
            e.conf.inc();
            if (e.conf.pred()) {  // do prefetches
                int32_t fetchDepth = (e.lastPrefetchPos - e.lastPos)/stride;
                uint32_t prefetchPos = e.lastPrefetchPos + stride;
                if (fetchDepth < 1) {
                    prefetchPos = pos + stride;
                    fetchDepth = 1;
                }
                DBG("%s: pos %d stride %d conf %d lastPrefetchPos %d prefetchPos %d fetchDepth %d", name.c_str(), pos, stride, e.conf.counter(), e.lastPrefetchPos, prefetchPos, fetchDepth);

                if (prefetchPos < 64 && !e.valid[prefetchPos]) {
                    MESIState state = I;
                    MemReq pfReq = {req.lineAddr + prefetchPos - pos, GETS, req.childId, &state, reqCycle, req.childLock, state, req.srcId, MemReq::PREFETCH};
                    uint64_t pfRespCycle = parent->access(pfReq);  // FIXME, might segfault
                    e.valid[prefetchPos] = true;
                    e.times[prefetchPos].fill(reqCycle, pfRespCycle);
                    profPrefetches.inc();

                    if (shortPrefetch && fetchDepth < 8 && prefetchPos + stride < 64 && !e.valid[prefetchPos + stride]) {
                        prefetchPos += stride;
                        pfReq.lineAddr += stride;
                        pfRespCycle = parent->access(pfReq);
                        e.valid[prefetchPos] = true;
                        e.times[prefetchPos].fill(reqCycle, pfRespCycle);
                        profPrefetches.inc();
                        profDoublePrefetches.inc();
                    }
                    e.lastPrefetchPos = prefetchPos;
                    assert(state == I);  // prefetch access should not give us any permissions
                }
            } else {
                profLowConfAccs.inc();
            }
        } else {
            e.conf.dec();
            // See if we need to switch strides
            if (!e.conf.pred()) {
                int32_t lastStride = e.lastPos - e.lastLastPos;

                if (stride && stride != e.stride && stride == lastStride) {
                    e.conf.reset();
                    e.stride = stride;
                    profStrideSwitches.inc();
                }
            }
            e.lastPrefetchPos = pos;
        }

        e.lastLastPos = e.lastPos;
        e.lastPos = pos;
    }

    req.childId = origChildId;
    return respCycle;
}

// nop for now; do we need to invalidate our own state?
uint64_t StreamPrefetcher::invalidate(const InvReq& req) {
    return child->invalidate(req);
}



/* ---------------------------------------------------------------------------------------------------- */
/* L2StreamPrefetcher (see prefetcher.h)                                                                 */

static const uint32_t PF_MAX_CORES = 4096;
static uint16_t g_pfSuperqueue[PF_MAX_CORES];   // per core: its L2 prefetcher's superqueue entries (0: none)
uint32_t L2StreamPrefetcher::superqueueOf(uint32_t core) { return core < PF_MAX_CORES ? g_pfSuperqueue[core] : 0; }

L2StreamPrefetcher::L2StreamPrefetcher(const g_string& _name, const Params& _p) : p(_p), name(_name) {
    if (p.streams < 1 || p.streams > MAX_STREAMS) panic("%s: streams must be 1..%u (got %u)", name.c_str(), MAX_STREAMS, p.streams);
    if (p.queue < 1 || p.queue > MAX_QUEUE) panic("%s: queue must be 1..%u (got %u)", name.c_str(), MAX_QUEUE, p.queue);
    if (p.distance < 1 || p.distance > 63) panic("%s: distance must be 1..63 lines (got %u)", name.c_str(), p.distance);
    if (p.perLookup < 1 || p.perLookup > 8) panic("%s: perLookup must be 1..8 (got %u)", name.c_str(), p.perLookup);
}

void L2StreamPrefetcher::setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network) {
    childId = _childId;
    if (parents.size() != 1) panic("%s: must have one parent", name.c_str());
    if (network) panic("%s: network not handled", name.c_str());
    parent = parents[0];
    parentHitLat = parent->getAccessLatency();
}

void L2StreamPrefetcher::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    if (children.size() != 1) panic("%s: must have one child", name.c_str());
    if (network) panic("%s: network not handled", name.c_str());
    child = children[0];
}

void L2StreamPrefetcher::initStats(AggregateStat* parentStat) {
    AggregateStat* s = new AggregateStat();
    s->init(name.c_str(), "L2 streamer prefetcher stats");
    profAccesses.init("acc", "L1D requests seen (loads and stores)"); s->append(&profAccesses);
    profDemandL2Miss.init("demL2Miss", "Requests that missed the L2"); s->append(&profDemandL2Miss);
    profIssued.init("pf", "Prefetch requests issued (L2 lookups)"); s->append(&profIssued);
    profIssuedFetched.init("pfFetch", "Prefetch requests that fetched their line into the L2"); s->append(&profIssuedFetched);
    profUseful.init("useful", "Requests that found a prefetched line"); s->append(&profUseful);
    profLate.init("late", "Useful prefetches still in flight when the request came (it waited for the line)"); s->append(&profLate);
    profDroppedQueue.init("dropQ", "Prefetches not issued: every L2 outstanding-request entry held"); s->append(&profDroppedQueue);
    profStreamsDetected.init("streams", "Streams detected"); s->append(&profStreamsDetected);
    profSpatial.init("spatial", "Pair-line (spatial) prefetches issued"); s->append(&profSpatial);
    parentStat->append(s);
}

uint32_t L2StreamPrefetcher::inFlight(uint64_t t) const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < p.queue; i++) n += (slots[i].start <= t && t < slots[i].end) ? 1 : 0;
    return n;
}

void L2StreamPrefetcher::takeSlot(uint64_t start, uint64_t end) {
    // a free entry at `start`, else the one that frees first (a demand is never refused: the L1D's fill
    // buffers bound the demand misses; only prefetches are dropped when the entries are held)
    uint32_t best = 0; uint64_t bestEnd = ~0ull;
    for (uint32_t i = 0; i < p.queue; i++) {
        if (!(slots[i].start <= start && start < slots[i].end)) { best = i; break; }
        if (slots[i].end < bestEnd) { bestEnd = slots[i].end; best = i; }
    }
    slots[best].start = start; slots[best].end = end;
}

bool L2StreamPrefetcher::isPending(uint64_t page, uint32_t pos) const {
    for (uint32_t i = 0; i < p.streams; i++) if (streamTab[i].page == page && streamTab[i].pending[pos]) return true;
    return false;
}

L2StreamPrefetcher::Stream* L2StreamPrefetcher::allocate(uint64_t page, int32_t pos) {
    uint32_t v = 0;
    for (uint32_t i = 1; i < p.streams; i++) if (streamTab[i].ts < streamTab[v].ts) v = i;   // LRU (never-used entries have ts 0)
    Stream& e = streamTab[v];
    for (uint32_t i = 0; i < 64; i++) if (e.pending[i]) forget(e, i);   // its lines still prefetched and not consumed
    e.page = page; e.dir = 0; e.lastPos = pos; e.pfPos = -1; e.pending.reset(); e.ts = ++timestamp;
    return &e;
}

/* a prefetched line leaves the table unconsumed (its entry replaced, or the line evicted before use): the replay
 * no longer keeps it for a consumer */
void L2StreamPrefetcher::forget(Stream& e, uint32_t pos) {
    if (e.tag[pos]) DramEpochReplay::prefetchDropped(e.tag[pos], core);
    e.tag[pos] = 0;
}

bool L2StreamPrefetcher::issue(MemReq& req, uint64_t reqCycle, uint64_t lineAddr, Stream& owner, TimingRecord* recs, uint32_t& nrec, bool spatial) {
    if (inFlight(reqCycle) >= p.queue) { profDroppedQueue.inc(); return false; }
    MESIState state = I;
    MemReq pfReq = {lineAddr, GETS, childId, &state, reqCycle, req.childLock, state, req.srcId,
                    (uint32_t)(MemReq::PREFETCH | MemReq::PF_TRAFFIC)};
    if (++tagSeq == 0) tagSeq = 1;
    const uint32_t tag = tagSeq;   // the replay's record of it, if it reaches memory, carries it (unique per core)
    DramEpochReplay::setPrefetchTag(tag);
    const uint64_t pfResp = parent->access(pfReq);
    DramEpochReplay::setPrefetchTag(0);
    assert(state == I);  // a prefetch gives the requester no permissions
    EventRecorder* evRec = (req.srcId < zinfo->numCores) ? zinfo->eventRecorders[req.srcId] : nullptr;
    if (evRec && evRec->hasRecord()) {
        TimingRecord r = evRec->popRecord();
        if (nrec < 8) recs[nrec++] = r;
        else panic("%s: more than 8 prefetch timing records in one lookup", name.c_str());
    }
    profIssued.inc();
    if (spatial) profSpatial.inc();
    if (pfResp > reqCycle + parentHitLat) {   // the line was fetched (an L2 hit is a lookup only)
        profIssuedFetched.inc();
        takeSlot(reqCycle, pfResp);
        const uint32_t pos = lineAddr & 63;
        owner.pending[pos] = true; owner.resp[pos] = pfResp; owner.iss[pos] = reqCycle; owner.tag[pos] = tag;
    }
    return true;
}

uint64_t L2StreamPrefetcher::access(MemReq& req) {
    const uint32_t origChildId = req.childId;
    req.childId = childId;
    if (req.type != GETS && req.type != GETX) {   // writebacks pass through
        const uint64_t r = parent->access(req);
        req.childId = origChildId;
        return r;
    }
    profAccesses.inc();
    core = req.srcId;
    if (req.srcId < PF_MAX_CORES) g_pfSuperqueue[req.srcId] = (uint16_t)p.queue;
    const uint64_t reqCycle = req.cycle;
    const uint64_t lineAddr = req.lineAddr;
    uint64_t respCycle = parent->access(req);
    const bool l2Miss = respCycle > reqCycle + parentHitLat;
    if (l2Miss) profDemandL2Miss.inc();

    // The demand's timing record (a request that went past the L2) is set aside while prefetches run.
    EventRecorder* evRec = (req.srcId < zinfo->numCores) ? zinfo->eventRecorders[req.srcId] : nullptr;
    TimingRecord demandRec; demandRec.clear();
    if (evRec && evRec->hasRecord()) demandRec = evRec->popRecord();

    const uint64_t page = lineAddr >> 6;
    const int32_t pos = (int32_t)(lineAddr & 63);

    // 1. A prefetched line: consumed. Still in flight -> the request waits for it. Evicted before use
    // (the request missed the L2) -> not useful.
    for (uint32_t i = 0; i < p.streams; i++) {
        Stream& e = streamTab[i];
        if (e.page != page || !e.pending[pos]) continue;
        e.pending[pos] = false;
        if (!l2Miss) {
            profUseful.inc();
            const uint64_t hit = respCycle;   // without the wait: an L2 hit
            if (e.resp[pos] > respCycle) { profLate.inc(); respCycle = e.resp[pos]; }
            DramEpochReplay::prefetchConsumed(e.tag[pos], req.srcId, hit, e.resp[pos], e.iss[pos]);   // settled against the replayed arrival
            e.tag[pos] = 0;
        } else {
            forget(e, (uint32_t)pos);
        }
        break;
    }
    if (l2Miss) takeSlot(reqCycle, respCycle);   // a superqueue entry: the first miss for a line, until it arrives

    // 2. Training: continue a stream of the page, detect one from a training entry, or start training.
    Stream* s = nullptr;
    Stream* tr = nullptr;
    Stream* same = nullptr;   // a stream whose latest request was this line (a repeat: nothing to learn)
    bool hasFwd = false, hasBwd = false;
    for (uint32_t i = 0; i < p.streams; i++) {
        Stream& e = streamTab[i];
        if (e.page != page) continue;
        if (e.dir == 1) hasFwd = true;
        if (e.dir == -1) hasBwd = true;
        if (e.dir != 0 && pos != e.lastPos && ((pos > e.lastPos) == (e.dir > 0))) { if (!s) s = &e; }
        else if (e.dir != 0 && pos == e.lastPos) { if (!same) same = &e; }
        else if (e.dir == 0) { if (!tr) tr = &e; }
    }
    if (!s && tr && pos != tr->lastPos) {
        const int32_t d = (pos > tr->lastPos) ? 1 : -1;
        if ((d == 1 && !hasFwd) || (d == -1 && !hasBwd)) {   // one forward and one backward stream per page
            tr->dir = d; tr->pfPos = -1; s = tr; tr = nullptr;
            profStreamsDetected.inc();
        } else {
            tr->lastPos = pos;
        }
    }
    Stream* home = s ? s : (tr ? tr : same);   // the entry that holds this page's pair-line prefetch
    if (!home) home = allocate(page, pos);
    else if (tr) { tr->ts = ++timestamp; if (!s) tr->lastPos = pos; }
    else if (same) same->ts = ++timestamp;

    TimingRecord recs[8];
    uint32_t nrec = 0;
    if (s) {
        s->ts = ++timestamp;
        s->lastPos = pos;
        // 3. Prefetch ahead of the request: at most perLookup lines, at most `distance` lines ahead, in the page.
        int32_t next = (s->pfPos >= 0 && (s->pfPos - pos) * s->dir > 0) ? s->pfPos + s->dir : pos + s->dir;
        uint32_t n = 0;
        while (n < p.perLookup && next >= 0 && next < 64 && (next - pos) * s->dir <= (int32_t)p.distance) {
            if (!isPending(page, (uint32_t)next)) {
                if (!issue(req, reqCycle, (page << 6) | (uint64_t)next, *s, recs, nrec, false)) break;
                n++;
            }
            s->pfPos = next;
            next += s->dir;
        }
    }
    // 4. Spatial: a line the L2 fetched is completed with its pair line (128-byte aligned block).
    if (p.spatial && l2Miss) {
        const uint32_t pair = (uint32_t)pos ^ 1u;
        if (!isPending(page, pair)) (void)issue(req, reqCycle, (page << 6) | (uint64_t)pair, *home, recs, nrec, true);
    }

    // 5. Timing records: the prefetches start with the demand and are never waited for.
    if (nrec > 0 && evRec) {
        DelayEvent* startEv = new (evRec) DelayEvent(0);
        startEv->setMinStartCycle(reqCycle);
        for (uint32_t i = 0; i < nrec; i++) {
            assert(recs[i].reqCycle >= reqCycle);
            DelayEvent* d = new (evRec) DelayEvent(recs[i].reqCycle - reqCycle);
            d->setMinStartCycle(reqCycle);
            startEv->addChild(d, evRec)->addChild(recs[i].startEvent, evRec);
        }
        if (demandRec.isValid()) {
            assert(demandRec.reqCycle >= reqCycle);
            DelayEvent* d = new (evRec) DelayEvent(demandRec.reqCycle - reqCycle);
            d->setMinStartCycle(reqCycle);
            startEv->addChild(d, evRec)->addChild(demandRec.startEvent, evRec);
            demandRec.reqCycle = reqCycle;
            demandRec.startEvent = startEv;   // the demand's end event stays the one the core waits for
            evRec->pushRecord(demandRec);
        } else {
            TimingRecord side;   // side traffic only: a writeback-type record, started, not waited for
            side.addr = lineAddr; side.reqCycle = reqCycle; side.respCycle = reqCycle; side.type = PUTS;
            side.startEvent = startEv; side.endEvent = nullptr; side.side = true;
            evRec->pushRecord(side);
        }
    } else if (demandRec.isValid()) {
        evRec->pushRecord(demandRec);
    }

    req.childId = origChildId;
    return respCycle;
}
