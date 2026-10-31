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

#ifndef CACHE_H_
#define CACHE_H_

#include <unordered_set>
#include <vector>

#include "cache_arrays.h"
#include "coherence_ctrls.h"
#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "memory_hierarchy.h"
#include "repl_policies.h"
#include "stats.h"

class Network;

/* General coherent modular cache. The replacement policy and cache array are
 * pretty much mix and match. The coherence controller interfaces are general
 * too, but to avoid virtual function call overheads we work with MESI
 * controllers, since for now we only have MESI controllers
 */
/* 1.11.40 (audit N7): global cache registry. Function-local static so it is
 * safe before any other static initialiser runs; populated once per cache in
 * InitSystem. Read ONLY at roi_begin, to measure the coherence-flush footprint
 * that used to be a 16 MiB constant. */
class Cache;
g_vector<Cache*>& zsimAllCaches();

class Cache : public BaseCache {
    protected:
        CC* cc;
        CacheArray* array;
        ReplPolicy* rp;

        uint32_t numLines;

        //Latencies
        uint32_t accLat; //latency of a normal access (could split in get/put, probably not needed)
        uint32_t invLat; //latency of an invalidation

        g_string name;

    public:
        Cache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const g_string& _name);
        uint32_t getAccessLatency() const { return accLat; }

        const char* getName();
        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector<BaseCache*>& children, Network* network);
        /* the host L3 holds the mean fabric round trip in its measured load-to-use: the part beyond the port's minimum comes out of its latency */
        void reduceAccLat(uint32_t d) { accLat = (accLat > d + 1) ? accLat - d : 1; }
        void initStats(AggregateStat* parentStat);

        virtual uint64_t access(MemReq& req);

        /* 1.11.57 (audit D001/D002/D003): the coherence flush's one entry
         * point into a cache, replacing dirtyBytes() (1.11.40 N7) and
         * cleanDirtyBytes() (1.11.55 F017).
         *
         * Those two were separate UNLOCKED O(numLines) walks: one counted the
         * lines in M, the other -- issued later, from a different place --
         * wrote them back to E. Splitting measurement from cleaning is what
         * let the first rank of a multi-rank co-sim charge for a dirty set
         * and then wipe it before the other ranks measured, and it is what
         * let an array[i] = E land on a line another host thread was driving
         * to M at that instant.
         *
         * Here the two are one locked pass, and it reports ADDRESSES rather
         * than a count: a dirty line is resident at every level of an
         * inclusive hierarchy, so the caller unions the addresses across all
         * caches and gets the number of DISTINCT lines a writeback flush must
         * move. That is the quantity, and it does not depend on any inclusion
         * or silent-upgrade argument holding.
         *
         * Returns the number of lines THIS cache transitioned M -> E, which
         * is the per-level sum and is legitimately larger than the distinct
         * count -- the flush log prints both, labelled.
         *
         * 1.11.62 (ruling 5b): this is now called once per ARRIVAL at the
         * flush point, not once per epoch. Because the pass CLEANS, each
         * caller's walk returns only what has been dirtied since the previous
         * walk, and that delta property is what makes the per-rank charges sum
         * to the working set exactly once. Anything that made this measure
         * without cleaning would break that invariant.
         *
         * 1.11.60 (audit round 4, D004): VIRTUAL, because cleaning the tag
         * array is not the whole job on a cache that also owns an L0 filter.
         * FilterCache::store() can return a hit without ever calling
         * Cache::access, so an M -> E clean that touches only the array
         * leaves the filter advertising write permission the line no longer
         * has, and the next store never re-establishes M. The registry the
         * flush walks holds Cache*, so the override only runs if the call
         * dispatches -- it did not, and nothing said so. See
         * FilterCache::flushDirtyLines for the mechanism and the bound. */
        /* 1.12.18: `order`, when given, receives each newly found dirty
         * address in the order the walk meets it (this cache's line ids:
         * set-major for a set-associative array) -- the order a hardware
         * flush writes the lines back. */
        virtual uint64_t flushDirtyLines(std::unordered_set<Address>& distinctDirty, std::vector<Address>* order = nullptr);

        //NOTE: reqWriteback is pulled up to true, but not pulled down to false.
        virtual uint64_t invalidate(const InvReq& req) {
            startInvalidate();
            return finishInvalidate(req);
        }

    protected:
        void initCacheStats(AggregateStat* cacheStat);

        void startInvalidate(); // grabs cc's downLock
        uint64_t finishInvalidate(const InvReq& req); // performs inv and releases downLock
};

#endif  // CACHE_H_
