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

#ifndef FILTER_CACHE_H_
#define FILTER_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "zsim.h"

/* Extends Cache with an L0 direct-mapped cache, optimized to hell for hits
 *
 * L1 lookups are dominated by several kinds of overhead (grab the cache locks,
 * several virtual functions for the replacement policy, etc.). This
 * specialization of Cache solves these issues by having a filter array that
 * holds the most recently used line in each set. Accesses check the filter array,
 * and then go through the normal access path. Because there is one line per set,
 * it is fine to do this without grabbing a lock.
 */

class FilterCache : public Cache {
    private:
        struct FilterEntry {
            volatile Address rdAddr;
            volatile Address wrAddr;
            volatile uint64_t availCycle;

            void clear() {wrAddr = 0; rdAddr = 0; availCycle = 0;}
        };

        //Replicates the most accessed line of each set in the cache
        FilterEntry* filterArray;
        Address setMask;
        uint32_t numSets;
        uint32_t srcId; //should match the core
        uint32_t reqFlags;

        lock_t filterLock;
        uint64_t fGETSHit, fGETXHit;

    public:
        FilterCache(uint32_t _numSets, uint32_t _numLines, CC* _cc, CacheArray* _array,
                ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, g_string& _name)
            : Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, _name)
        {
            numSets = _numSets;
            setMask = numSets - 1;
            filterArray = gm_memalign<FilterEntry>(CACHE_LINE_BYTES, numSets);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_init(&filterLock);
            fGETSHit = fGETXHit = 0;
            srcId = -1;
            reqFlags = 0;
        }

        // Override type check for use without RTTI (Pin 4.x requires -fno-rtti)
        FilterCache* asFilterCache() override { return this; }

        void setSourceId(uint32_t id) {
            srcId = id;
        }

        void setFlags(uint32_t flags) {
            reqFlags = flags;
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* cacheStat = new AggregateStat();
            cacheStat->init(name.c_str(), "Filter cache stats");

            ProxyStat* fgetsStat = new ProxyStat();
            fgetsStat->init("fhGETS", "Filtered GETS hits", &fGETSHit);
            ProxyStat* fgetxStat = new ProxyStat();
            fgetxStat->init("fhGETX", "Filtered GETX hits", &fGETXHit);
            cacheStat->append(fgetsStat);
            cacheStat->append(fgetxStat);

            initCacheStats(cacheStat);
            parentStat->append(cacheStat);
        }

        /* 1.11.97 (review x05-zsim-cores-5, gate 1207D): the L1 HIT latency,
         * for the in-order core's load-use charge. zsim builds every terminal
         * cache with accLat = 0 (init.cpp: "terminal caches has no access
         * latency b/c it is assumed accLat is hidden by the pipeline"), so a
         * filter hit returns the line's availability -- zero latency -- and
         * the configured latency survived only as the invalidation latency.
         * The configured latency (CACTI-derived, or the cache.<level>
         * override) is kept here for the core to charge. 1207D caught the
         * first version reading accLat, which is 0: the charge was inert. */
        uint32_t hitLat = 0;
        void setHitLatency(uint32_t l) { hitLat = l; }
        uint32_t getHitLatency() const { return hitLat; }
        inline uint64_t load(Address vAddr, uint64_t curCycle) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if (vLineAddr == filterArray[idx].rdAddr) {
                fGETSHit++;
                return MAX(curCycle, availCycle);
            } else {
                return replace(vLineAddr, idx, true, curCycle);
            }
        }

        inline uint64_t store(Address vAddr, uint64_t curCycle) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if (vLineAddr == filterArray[idx].wrAddr) {
                fGETXHit++;
                //NOTE: Stores don't modify availCycle; we'll catch matches in the core
                //filterArray[idx].availCycle = curCycle; //do optimistic store-load forwarding
                return MAX(curCycle, availCycle);
            } else {
                return replace(vLineAddr, idx, false, curCycle);
            }
        }

        uint64_t replace(Address vLineAddr, uint32_t idx, bool isLoad, uint64_t curCycle) {
            Address pLineAddr = procMask | vLineAddr;
            MESIState dummyState = MESIState::I;
            futex_lock(&filterLock);
            MemReq req = {pLineAddr, isLoad? GETS : GETX, 0, &dummyState, curCycle, &filterLock, dummyState, srcId, reqFlags};
            uint64_t respCycle  = access(req);

            //Due to the way we do the locking, at this point the old address might be invalidated, but we have the new address guaranteed until we release the lock

            //Careful with this order
            Address oldAddr = filterArray[idx].rdAddr;
            filterArray[idx].wrAddr = isLoad? -1L : vLineAddr;
            filterArray[idx].rdAddr = vLineAddr;

            //For LSU simulation purposes, loads bypass stores even to the same line if there is no conflict,
            //(e.g., st to x, ld from x+8) and we implement store-load forwarding at the core.
            //So if this is a load, it always sets availCycle; if it is a store hit, it doesn't
            if (oldAddr != vLineAddr) filterArray[idx].availCycle = respCycle;

            futex_unlock(&filterLock);
            return respCycle;
        }

        uint64_t invalidate(const InvReq& req) {
            Cache::startInvalidate();  // grabs cache's downLock
            futex_lock(&filterLock);
            uint32_t idx = req.lineAddr & setMask; //works because of how virtual<->physical is done...
            if ((filterArray[idx].rdAddr | procMask) == req.lineAddr) { //FIXME: If another process calls invalidate(), procMask will not match even though we may be doing a capacity-induced invalidation!
                filterArray[idx].wrAddr = -1L;
                filterArray[idx].rdAddr = -1L;
            }
            uint64_t respCycle = Cache::finishInvalidate(req); // releases cache's downLock
            futex_unlock(&filterLock);
            return respCycle;
        }

        void contextSwitch() {
            futex_lock(&filterLock);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_unlock(&filterLock);
        }

        /* 1.11.60 (audit round 4, D004): THE M -> E CLEAN HAS TO DROP THE L0
         * WRITE FILTER, or a store after the flush never re-dirties the line.
         *
         * store() above returns at the fGETXHit line WITHOUT calling
         * Cache::access whenever the line is the filter's current wrAddr, so
         * it never reaches MESIBottomCC::processAccess and never puts the line
         * back in M. Cache::flushDirtyLines drives the line M -> E in the tag
         * array and, until now, left wrAddr pointing straight at it: from that
         * instant the line was dirty in the guest and clean in the model. The
         * next flush did not count it, and when it was finally evicted
         * processEviction sent PUTS instead of PUTX, so its bytes were neither
         * written back nor charged. Only invalidate() and contextSwitch()
         * cleared the filter, and the flush called neither.
         *
         * Why it was invisible: the loss is bounded by the filter, numSets
         * lines per host L1D per flush -- 64 lines, 4 KB, at the shipped
         * 32 KB / 8-way / 64 B host L1D -- and no counter contradicts it. The
         * flush footprint is the only place it surfaces, and it surfaces as a
         * slightly SMALLER number, which is the direction a reader already
         * expects a repeated flush to trend. It grows with numSets and with
         * the number of flushes, and the 1.11.57 redesign reasoned explicitly
         * about zsim's silent E -> M upgrade inside the cache while never
         * mentioning the level above it.
         *
         * Only wrAddr is dropped, with invalidate()'s own -1L idiom. The line
         * is still VALID (E) after the clean, so read hits stay correct; a
         * store now misses the filter, goes through replace() -> GETX, and
         * that is the path that re-establishes M. Cost is at most one extra
         * GETX per set per flush, and replace() leaves availCycle alone when
         * the address is unchanged (oldAddr == vLineAddr), so the re-dirtying
         * store keeps exactly the timing a filtered store hit had.
         *
         * Lock discipline: nothing is added to load() or store(); the fast
         * path is untouched. Cache::flushDirtyLines takes and RELEASES the cc
         * lock before we reach filterLock, so we never hold both and cannot
         * invert replace()'s filterLock -> cc-lock order. Holding filterLock
         * across the clear is what closes the remaining race -- a replace()
         * that has already upgraded the line to M but has not yet published
         * wrAddr would otherwise re-open the identical hole behind us. */
        uint64_t flushDirtyLines(std::unordered_set<Address>& distinctDirty) override {
            uint64_t cleaned = Cache::flushDirtyLines(distinctDirty);
            futex_lock(&filterLock);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].wrAddr = -1L;
            futex_unlock(&filterLock);
            return cleaned;
        }
};

#endif  // FILTER_CACHE_H_
