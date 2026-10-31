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
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ninc_cache.h"
#include <algorithm>
#include "galloc.h"
#include "network.h"
#include "zsim.h"

NonInclusiveCache::NonInclusiveCache(uint32_t _numLines, uint32_t _ways, uint32_t _accLat, uint32_t _invLat, const g_string& _name)
    : Cache(_numLines, nullptr, nullptr, nullptr, _accLat, _invLat, _name), dWays(_ways), dClock(0),
      sfTab(nullptr), sfBits(nullptr), sfWords(1), sfShift(64), sfCap(0), sfUsed(0), selfId(0) {
    if (dWays == 0 || numLines % dWays != 0) panic("[%s] %u lines are not a whole number of %u-way sets", name.c_str(), numLines, dWays);
    dSets = numLines / dWays;
    if (dSets & (dSets - 1)) panic("[%s] the number of sets (%u) must be a power of two", name.c_str(), dSets);
    dTag = gm_calloc<Address>(numLines);
    dState = gm_calloc<uint8_t>(numLines);
    dStamp = gm_calloc<uint64_t>(numLines);
    for (uint32_t i = 0; i < numLines; i++) dDrop(i);
    futex_init(&ccLock);
}

/* The address hash a MESI bottom controller uses to choose among several parents. */
uint32_t NonInclusiveCache::getParentId(Address lineAddr) const {
    uint32_t res = 0;
    uint64_t tmp = lineAddr;
    for (uint32_t i = 0; i < 4; i++) {
        res ^= (uint32_t) (((uint64_t)0xffff) & tmp);
        tmp = tmp >> 16;
    }
    return (res % parents.size());
}

void NonInclusiveCache::setParents(uint32_t _childId, const g_vector<MemObject*>& _parents, Network* network) {
    selfId = _childId;
    parents.resize(_parents.size());
    parentRTTs.resize(_parents.size());
    for (uint32_t p = 0; p < parents.size(); p++) {
        parents[p] = _parents[p];
        parentRTTs[p] = (network) ? network->getRTT(name.c_str(), parents[p]->getName()) : 0;
    }
}

void NonInclusiveCache::setChildren(const g_vector<BaseCache*>& _children, Network* network) {
    if (_children.size() > MAX_CACHE_CHILDREN) {
        panic("[%s] Children size (%d) > MAX_CACHE_CHILDREN (%d)", name.c_str(), (uint32_t)_children.size(), MAX_CACHE_CHILDREN);
    }
    children.resize(_children.size());
    childrenRTTs.resize(_children.size());
    for (uint32_t c = 0; c < children.size(); c++) {
        children[c] = _children[c];
        childrenRTTs[c] = (network) ? network->getRTT(name.c_str(), children[c]->getName()) : 0;
    }
    sfWords = std::max<uint32_t>(1, (children.size() + 63) / 64);
    // the children hold about as many lines as this cache: start at twice its lines (the table grows if they hold more)
    uint64_t cap = 1024;
    while (cap < 2ull * numLines) cap *= 2;
    sfAlloc(cap);
}

void NonInclusiveCache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "Cache stats");
    profGETSHit.init("hGETS", "GETS hits (served on chip: by this cache or by another child)");
    profGETXHit.init("hGETX", "GETX hits (served on chip)");
    profGETSMiss.init("mGETS", "GETS misses");
    profGETXMissIM.init("mGETXIM", "GETX I->M misses");
    profGETXMissSM.init("mGETXSM", "GETX S->M misses (upgrade misses)");
    profPUTS.init("PUTS", "Clean evictions (from lower level)");
    profPUTX.init("PUTX", "Dirty evictions (from lower level)");
    profDataHit.init("dHit", "GET hits on this cache's data array");
    profPeerHit.init("cHit", "GET hits on a line only another child holds");
    profFill.init("fill", "Lines written into the data array (children's evictions and copies of shared lines)");
    profFillDirty.init("fillM", "Of those, dirty lines");
    profSharedCopy.init("shCopy", "Copies of lines shared across children");
    profDataEvict.init("dEvict", "Data-array evictions");
    profDataWB.init("dWB", "Data-array evictions written back to memory");
    profRace.init("race", "Child requests that raced with an invalidation");
    profGETNextLevelLat.init("latGETnl", "GET request latency on next level");
    profGETNetLat.init("latGETnet", "GET request latency on network to next level");
    Counter* all[] = {&profGETSHit, &profGETXHit, &profGETSMiss, &profGETXMissIM, &profGETXMissSM, &profPUTS, &profPUTX,
                      &profDataHit, &profPeerHit, &profFill, &profFillDirty, &profSharedCopy, &profDataEvict, &profDataWB,
                      &profRace, &profGETNextLevelLat, &profGETNetLat};
    for (Counter* c : all) cacheStat->append(c);
    parentStat->append(cacheStat);
}

/* ---- data array ---- */

int32_t NonInclusiveCache::dLookup(Address lineAddr) const {
    const uint32_t first = (uint32_t)(lineAddr & (dSets - 1)) * dWays;
    for (uint32_t id = first; id < first + dWays; id++) {
        if (dState[id] != I && dTag[id] == lineAddr) return (int32_t)id;
    }
    return -1;
}

void NonInclusiveCache::dFill(Address lineAddr, MESIState st, uint64_t cycle, uint32_t srcId) {
    assert(st != I);
    profFill.inc();
    if (st == M) profFillDirty.inc();
    const int32_t hit = dLookup(lineAddr);
    if (hit >= 0) {
        dState[hit] = st;
        dTouch(hit);
        return;
    }
    const uint32_t first = (uint32_t)(lineAddr & (dSets - 1)) * dWays;
    uint32_t victim = first;
    for (uint32_t id = first; id < first + dWays; id++) {   // a free way, else the least recently used
        if (dState[id] == I) { victim = id; break; }
        if (dStamp[id] < dStamp[victim]) victim = id;
    }
    if (dState[victim] != I) {
        profDataEvict.inc();
        const Address vAddr = dTag[victim];
        /* A victim a child still holds keeps its state in the snoop filter, so dropping the copy loses nothing. A dirty
         * victim no child holds is written back (not on the critical path, as every eviction); a clean one is dropped. */
        if (dState[victim] == M && sfFind(vAddr) < 0) {
            profDataWB.inc();
            MESIState wbState = M;
            MemReq wb = {vAddr, PUTX, selfId, &wbState, cycle, &ccLock, wbState, srcId, 0 /*no flags*/};
            parents[getParentId(vAddr)]->access(wb);
        }
        dDrop(victim);
    }
    dTag[victim] = lineAddr;
    dState[victim] = st;
    dTouch(victim);
}

/* ---- snoop filter ---- */

void NonInclusiveCache::sfAlloc(uint64_t cap) {
    SFEntry* oldTab = sfTab;
    uint64_t* oldBits = sfBits;
    const uint64_t oldCap = sfCap;
    sfCap = cap;
    sfShift = 64 - __builtin_ctzll(cap);
    sfTab = gm_calloc<SFEntry>(sfCap);
    sfBits = gm_calloc<uint64_t>(sfCap * sfWords);
    sfUsed = 0;
    for (uint64_t s = 0; s < oldCap; s++) {
        if (!oldTab[s].numSharers) continue;
        uint64_t t = sfHome(oldTab[s].tag);
        while (sfTab[t].numSharers) t = (t + 1) & (sfCap - 1);
        sfTab[t] = oldTab[s];
        std::copy(&oldBits[s * sfWords], &oldBits[(s + 1) * sfWords], &sfBits[t * sfWords]);
        sfUsed++;
    }
    if (oldTab) { gm_free(oldTab); gm_free(oldBits); }
}

int64_t NonInclusiveCache::sfFind(Address a) const {
    for (uint64_t s = sfHome(a); sfTab[s].numSharers; s = (s + 1) & (sfCap - 1)) {
        if (sfTab[s].tag == a) return (int64_t)s;
    }
    return -1;
}

uint64_t NonInclusiveCache::sfAdd(Address a, MESIState mem) {
    if (4 * (sfUsed + 1) > 3 * sfCap) sfAlloc(2 * sfCap);   // load factor at most 3/4
    uint64_t s = sfHome(a);
    while (sfTab[s].numSharers) s = (s + 1) & (sfCap - 1);
    sfTab[s].tag = a;
    sfTab[s].numSharers = 0;   // the caller adds the first sharer
    sfTab[s].exclusive = 0;
    sfTab[s].mem = mem;
    std::fill(&sfBits[s * sfWords], &sfBits[(s + 1) * sfWords], 0);
    sfUsed++;
    return s;
}

void NonInclusiveCache::sfErase(uint64_t i) {
    const uint64_t mask = sfCap - 1;
    uint64_t j = i;
    while (true) {
        j = (j + 1) & mask;
        if (!sfTab[j].numSharers) break;
        const uint64_t k = sfHome(sfTab[j].tag);
        // the entry at j may move back to i unless its home lies cyclically in (i, j]
        const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
        if (stays) continue;
        sfTab[i] = sfTab[j];
        std::copy(&sfBits[j * sfWords], &sfBits[(j + 1) * sfWords], &sfBits[i * sfWords]);
        i = j;
    }
    sfTab[i].numSharers = 0;
    sfTab[i].tag = 0;
    std::fill(&sfBits[i * sfWords], &sfBits[(i + 1) * sfWords], 0);
    sfUsed--;
}

uint64_t NonInclusiveCache::sendInvalidates(Address lineAddr, uint64_t slot, InvType type, bool* reqWriteback, uint64_t cycle, uint32_t srcId) {
    SFEntry* e = &sfTab[slot];
    if (type == INVX && !e->isExclusive()) return cycle;   // only an exclusive holder is downgraded
    uint64_t maxCycle = cycle;   // sent in parallel
    for (uint32_t c = 0; c < children.size(); c++) {
        if (!sfHas(slot, c)) continue;
        InvReq ir = {lineAddr, type, reqWriteback, cycle, srcId};
        const uint64_t respCycle = children[c]->invalidate(ir) + childrenRTTs[c];
        maxCycle = MAX(respCycle, maxCycle);
        if (type == INV) sfClr(slot, c);
    }
    if (type == INV) e->numSharers = 0;
    e->exclusive = 0;
    return maxCycle;
}

/* ---- accesses ---- */

uint64_t NonInclusiveCache::access(MemReq& req) {
    assert((req.type == GETS) || (req.type == GETX) || (req.type == PUTS) || (req.type == PUTX));
    {   // the shared-cache residency marker, as Cache::access
        size_t p = name.rfind('_');
        const char* base = (p != g_string::npos && p + 2 < name.size()) ? name.c_str() + p + 1 : name.c_str();
        const bool shared = (base[0] == 'l') && (base[1] == '2' || base[1] == '3' || (base[1] == 'l' && base[2] == 'c'));
        if (shared) zinfo->pgres.sharedCache.touch(zinfo->numPhases);
    }
    // hand-over-hand locking, as the MESI controllers: release the child, then take this cache
    if (req.childLock) futex_unlock(req.childLock);
    futex_lock(&ccLock);

    uint64_t respCycle = req.cycle;
    const bool skipAccess = CheckForMESIRace(req.type /*may change*/, req.state, req.initialState);
    const Address addr = req.lineAddr;
    const uint32_t c = req.childId;
    assert(c < children.size());
    if (unlikely(skipAccess)) {
        profRace.inc();
    } else if (req.type == PUTS || req.type == PUTX) {
        respCycle += accLat;
        const int64_t s = sfFind(addr);
        if (s < 0 || !sfHas(s, c)) {
            // the child's copy was invalidated while its eviction was on the way: nothing to keep
            profRace.inc();
            *req.state = I;
        } else {
            SFEntry* e = &sfTab[s];
            if (req.type == PUTX) { profPUTX.inc(); e->mem = M; } else { profPUTS.inc(); }
            if (req.type == PUTX && (req.flags & MemReq::PUTX_KEEPEXCL)) {
                assert(*req.state == M);
                *req.state = E;   // a pure write-back: the child keeps the line, clean
            } else {
                sfClr(s, c);
                e->numSharers--;
                if (!e->numSharers) e->exclusive = 0;
                *req.state = I;
            }
            // 2. the child's line, modified or unmodified, is written into this cache
            const MESIState st = (MESIState)e->mem;
            const bool last = (e->numSharers == 0);
            if (last) sfErase(s);   // the line now lives here alone, with its state
            dFill(addr, st, respCycle, req.srcId);
        }
    } else {   // GETS or GETX
        /* A prefetch into this cache alone (no requester taking the line) is not modelled: the children here are MESI
         * caches, which take the prefetch flag off before a request leaves them. */
        if (req.flags & MemReq::PREFETCH) panic("[%s] prefetch request 0x%lx: a prefetch into a non-inclusive last-level cache is not modelled", name.c_str(), addr);
        respCycle += accLat;
        int64_t s = sfFind(addr);
        int32_t did = dLookup(addr);
        const bool mine = (s >= 0) && sfHas(s, c);
        const uint32_t others = (s >= 0) ? sfTab[s].numSharers - (mine ? 1 : 0) : 0;
        MESIState mem = (s >= 0) ? (MESIState)sfTab[s].mem : ((did >= 0) ? (MESIState)dState[did] : I);
        const bool fromMem = (req.type == GETS) ? (mem == I) : (mem == I || mem == S);
        if (fromMem) {
            // 1. not on chip, or held shared for a write: read it from memory; it fills the requester only
            const MESIState before = mem;
            MESIState ns = mem;
            const uint32_t p = getParentId(addr);
            MemReq mreq = {addr, req.type, selfId, &ns, respCycle, &ccLock, ns, req.srcId, req.flags};
            const uint64_t nextLevelResp = parents[p]->access(mreq);
            const uint32_t nextLevelLat = nextLevelResp - respCycle;
            const uint32_t netLat = parentRTTs[p];
            profGETNextLevelLat.inc(nextLevelLat);
            profGETNetLat.inc(netLat);
            respCycle = nextLevelResp + netLat;
            mem = ns;
            if (req.type == GETS) profGETSMiss.inc();
            else if (before == I) profGETXMissIM.inc();
            else profGETXMissSM.inc();
        } else {
            if (req.type == GETS) profGETSHit.inc(); else profGETXHit.inc();
            if (did >= 0) profDataHit.inc(); else profPeerHit.inc();
        }
        if (s < 0) s = (int64_t)sfAdd(addr, mem);   // no other slot number is held across this call
        SFEntry* e = &sfTab[s];
        e->mem = mem;

        if (req.type == GETS) {
            if (!e->numSharers && (mem == E || mem == M) && !(req.flags & MemReq::NOEXCL)) {
                // the requester holds it alone: exclusive, and a copy found here moves to it
                e->exclusive = 1;
                sfSet(s, c);
                e->numSharers = 1;
                *req.state = E;
                if (did >= 0) dDrop(did);
            } else {
                if (e->isExclusive() && !mine) {
                    // another child holds it exclusively: downgrade it (its dirty data, if any, comes back with the line)
                    bool wb = false;
                    respCycle = sendInvalidates(addr, s, INVX, &wb, respCycle, req.srcId);
                    if (wb) { mem = M; e->mem = M; }
                } else if (others && did < 0) {
                    // other children hold it shared and this cache has no copy: one of them sends the line
                    for (uint32_t o = 0; o < children.size(); o++) {
                        if (o == c || !sfHas(s, o)) continue;
                        bool fwb = false;
                        InvReq fr = {addr, FWD, &fwb, respCycle, req.srcId};
                        respCycle = children[o]->invalidate(fr) + childrenRTTs[o];
                        break;
                    }
                }
                if (!mine) { sfSet(s, c); e->numSharers++; }
                e->exclusive = 0;
                *req.state = S;
                if (others) {
                    // 3. a line shared across cores is copied into this cache
                    if (did < 0) profSharedCopy.inc();
                    dFill(addr, mem, respCycle, req.srcId);
                } else if (did >= 0) {
                    dState[did] = mem;   // a line held shared by its only reader (no exclusivity asked) keeps its copy
                    dTouch(did);
                }
            }
        } else {   // GETX
            if (mine) {
                assert_msg(!e->isExclusive(), "[%s] spurious GETX, childId %u numSharers %u", name.c_str(), c, e->numSharers);
                sfClr(s, c);
                e->numSharers--;
            }
            bool wb = false;
            respCycle = sendInvalidates(addr, s, INV, &wb, respCycle, req.srcId);   // every other copy
            sfSet(s, c);
            e->numSharers = 1;
            e->exclusive = 1;
            e->mem = M;
            *req.state = M;
            if (did >= 0) dDrop(did);   // a line to be written leaves this cache
        }
    }

    // relock the child before releasing this cache (hand-over-hand)
    if (req.childLock) futex_lock(req.childLock);
    futex_unlock(&ccLock);
    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s, respCycle %ld reqCycle %ld",
               name.c_str(), req.lineAddr, AccessTypeName(req.type), respCycle, req.cycle);
    return respCycle;
}

uint64_t NonInclusiveCache::invalidate(const InvReq& req) {
    panic("[%s] invalidation 0x%lx %s from a parent: a last-level cache has none", name.c_str(), req.lineAddr, InvTypeName(req.type));
    return req.cycle;
}

/* The flush walk: the data array in line-id (set-major) order, then the lines only children hold that this level knows dirty,
 * in the same set-major order (set index, then address), so the walk does not depend on the snoop filter's layout. */
uint64_t NonInclusiveCache::flushDirtyLines(std::unordered_set<Address>& distinctDirty, std::vector<Address>* order) {
    futex_lock(&ccLock);
    uint64_t n = 0;
    for (uint32_t id = 0; id < numLines; id++) {
        if (dState[id] != M) continue;
        dState[id] = E;
        const int64_t s = sfFind(dTag[id]);
        if (s >= 0) sfTab[s].mem = E;   // a line here and in a child has one state
        n++;
        if (distinctDirty.insert(dTag[id]).second && order) order->push_back(dTag[id]);
    }
    std::vector<uint64_t> held;
    for (uint64_t s = 0; s < sfCap; s++) {
        if (sfTab[s].numSharers && sfTab[s].mem == M) held.push_back(s);
    }
    const uint64_t setMask = dSets - 1;
    std::sort(held.begin(), held.end(), [&](uint64_t x, uint64_t y) {
        const Address ax = sfTab[x].tag, ay = sfTab[y].tag;
        return ((ax & setMask) != (ay & setMask)) ? ((ax & setMask) < (ay & setMask)) : (ax < ay);
    });
    for (uint64_t s : held) {
        sfTab[s].mem = E;
        n++;
        if (distinctDirty.insert(sfTab[s].tag).second && order) order->push_back(sfTab[s].tag);
    }
    futex_unlock(&ccLock);
    return n;
}
