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

#include "cache.h"
#include "hash.h"

#include "event_recorder.h"
#include "timing_event.h"
#include "zsim.h"

/* 1.11.40 (audit N7): the registry itself. Function-local static avoids any
 * static-init-order dependency; InitSystem is the only writer and it runs
 * single-threaded, so no lock is needed here. */
g_vector<Cache*>& zsimAllCaches() {
    static g_vector<Cache*> caches;
    return caches;
}

Cache::Cache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const g_string& _name)
    : cc(_cc), array(_array), rp(_rp), numLines(_numLines), accLat(_accLat), invLat(_invLat), name(_name) {}

const char* Cache::getName() {
    return name.c_str();
}

/* 1.11.57 (audit D001/D002/D003): measure and clean in ONE pass, under the
 * cache's own coherence lock, reporting addresses so the caller can
 * de-duplicate across levels. See the declaration in cache.h for why each of
 * those three properties is required. The tag read has to happen INSIDE the
 * lock: once it is dropped, a concurrent eviction may re-tag the same line id
 * and the address we recorded would belong to a different line. */
uint64_t Cache::flushDirtyLines(std::unordered_set<Address>& distinctDirty, std::vector<Address>* order) {
    std::vector<uint32_t> ids;
    cc->flushLock();
    cc->takeDirtyLineIds(ids);
    for (uint32_t id : ids) {
        const Address a = array->getLineAddr(id);
        if (distinctDirty.insert(a).second && order) order->push_back(a);   // 1.12.18: walk order
    }
    cc->flushUnlock();
    return (uint64_t)ids.size();
}

void Cache::setParents(uint32_t childId, const g_vector<MemObject*>& parents, Network* network) {
    cc->setParents(childId, parents, network);
}

void Cache::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    cc->setChildren(children, network);
}

void Cache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "Cache stats");
    initCacheStats(cacheStat);
    parentStat->append(cacheStat);
}

void Cache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    array->initStats(cacheStat);
    rp->initStats(cacheStat);
}

uint64_t Cache::access(MemReq& req) {
    /* 1.11.8 PG residency: mark the shared-cache tracker for L2+/LLC
     * instances only (private L1s ride their core's tracker). Name-based
     * level test matches this repo's cache naming (l2-*, l3-*, llc*).
     * 1.11.17 (audit go-through): system-scope caches are named
     * "<node>_l2-0" etc., which the bare prefix test could never match --
     * the 1.9.29 node-prefix parser bug, re-made here. Match the level
     * token anywhere after an optional node prefix.
     *
     * 1.11.57 (latent F025): TWO DEFECTS IN THAT TEST, both of the same kind
     * -- the code matched something narrower than the comment claimed.
     * (a) find('_') takes the FIRST underscore, but node names may contain
     *     one: the documented "hbm_pim" node yields base = "pim_l2-0", which
     *     starts with 'p' and can never match, so that node's LLC would never
     *     mark the tracker. rfind takes the LAST underscore, which is the one
     *     that separates the node prefix from the level token in every name
     *     the emitters write ("<node>_l2-0").
     * (b) the comment advertised an "llc*" pattern the test could not
     *     express: for a name beginning "llc", base[1] is 'l', which fails
     *     both digit branches. Either the pattern or the claim had to go;
     *     the pattern is cheap, so it is implemented.
     * Nothing reachable moves today: the only consumer of
     * pgSharedCacheActivePhases is the DEVICE-scope branch of
     * runPowerAnalysis, and device-scope caches are emitted with no node
     * prefix at all ("l2", "l3"), where both spellings already matched. The
     * system path never reads the counter. */
    {
        size_t p = name.rfind('_');
        const char* base = (p != g_string::npos && p + 2 < name.size())
                               ? name.c_str() + p + 1 : name.c_str();
        const bool shared = (base[0] == 'l') &&
                            (base[1] == '2' || base[1] == '3' ||
                             (base[1] == 'l' && base[2] == 'c'));
        if (shared) zinfo->pgres.sharedCache.touch(zinfo->numPhases);
    }
    uint64_t respCycle = req.cycle;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t lineId = array->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;

        if (lineId == -1 && cc->shouldAllocate(req)) {
            //Make space for new line
            Address wbLineAddr;
            lineId = array->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

            //Evictions are not in the critical path in any sane implementation -- we do not include their delays
            //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
            cc->processEviction(req, wbLineAddr, lineId, respCycle); //1. if needed, send invalidates/downgrades to lower level

            array->postinsert(req.lineAddr, &req, lineId); //do the actual insertion. NOTE: Now we must split insert into a 2-phase thing because cc unlocks us.
        }
        // Enforce single-record invariant: Writeback access may have a timing
        // record. If so, read it.
        EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
        TimingRecord wbAcc;
        wbAcc.clear();
        if (unlikely(evRec && evRec->hasRecord())) {
            wbAcc = evRec->popRecord();
        }

        respCycle = cc->processAccess(req, lineId, respCycle);

        // Access may have generated another timing record. If *both* access
        // and wb have records, stitch them together
        if (unlikely(wbAcc.isValid())) {
            if (!evRec->hasRecord()) {
                // Downstream should not care about endEvent for PUTs
                wbAcc.endEvent = nullptr;
                evRec->pushRecord(wbAcc);
            } else {
                // Connect both events
                TimingRecord acc = evRec->popRecord();
                assert(wbAcc.reqCycle >= req.cycle);
                assert(acc.reqCycle >= req.cycle);
                DelayEvent* startEv = new (evRec) DelayEvent(0);
                DelayEvent* dWbEv = new (evRec) DelayEvent(wbAcc.reqCycle - req.cycle);
                DelayEvent* dAccEv = new (evRec) DelayEvent(acc.reqCycle - req.cycle);
                startEv->setMinStartCycle(req.cycle);
                dWbEv->setMinStartCycle(req.cycle);
                dAccEv->setMinStartCycle(req.cycle);
                startEv->addChild(dWbEv, evRec)->addChild(wbAcc.startEvent, evRec);
                startEv->addChild(dAccEv, evRec)->addChild(acc.startEvent, evRec);

                acc.reqCycle = req.cycle;
                acc.startEvent = startEv;
                // endEvent / endCycle stay the same; wbAcc's endEvent not connected
                evRec->pushRecord(acc);
            }
        }
    }

    cc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void Cache::startInvalidate() {
    cc->startInv(); //note we don't grab tcc; tcc serializes multiple up accesses, down accesses don't see it
}

uint64_t Cache::finishInvalidate(const InvReq& req) {
    int32_t lineId = array->lookup(req.lineAddr, nullptr, false);
    assert_msg(lineId != -1, "[%s] Invalidate on non-existing address 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    uint64_t respCycle = req.cycle + invLat;
    trace(Cache, "[%s] Invalidate start 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    respCycle = cc->processInv(req, lineId, respCycle); //send invalidates or downgrades to children, and adjust our own state
    trace(Cache, "[%s] Invalidate end 0x%lx type %s lineId %d, reqWriteback %d, latency %ld", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback, respCycle - req.cycle);

    return respCycle;
}
