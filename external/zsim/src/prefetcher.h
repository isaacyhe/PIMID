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

#ifndef PREFETCHER_H_
#define PREFETCHER_H_

#include <bitset>
#include "bithacks.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "stats.h"
#include "event_recorder.h"

/* Prefetcher models: Basic operation is to interpose between cache levels, issue additional accesses,
 * and keep a small table with delays; when the demand access comes, we do it and account for the
 * latency as when it was first fetched (to avoid hit latencies on partial latency overlaps).
 */

template <int32_t M, int32_t T, int32_t I>  // max value, threshold, initial
class SatCounter {
    private:
        int32_t count;
    public:
        SatCounter() : count(I) {}
        void reset() { count = I; }
        void dec() { count = MAX(count - 1, 0); }
        void inc() { count = MIN(count + 1, M); }
        bool pred() const { return count >= T; }
        uint32_t counter() const { return count; }
};

/* This is basically a souped-up version of the DLP L2 prefetcher in Nehalem: 16 stream buffers,
 * but (a) no up/down distinction, and (b) strided operation based on dominant stride detection
 * to try to subsume as much of the L1 IP/strided prefetcher as possible.
 *
 * FIXME: For now, mostly hardcoded; 64-line entries (4KB w/64-byte lines), fixed granularities, etc.
 * TODO: Adapt to use weave models
 */
class StreamPrefetcher : public BaseCache {
    private:
        struct Entry {
            // Two competing strides; at most one active
            int32_t stride;
            SatCounter<3, 2, 1> conf;

            struct AccessTimes {
                uint64_t startCycle;  // FIXME: Dead for now, we should use it for profiling
                uint64_t respCycle;

                void fill(uint32_t s, uint64_t r) { startCycle = s; respCycle = r; }
            };

            AccessTimes times[64];
            std::bitset<64> valid;

            uint32_t lastPos;
            uint32_t lastLastPos;
            uint32_t lastPrefetchPos;
            uint64_t lastCycle;  // updated on alloc and hit
            uint64_t ts;

            void alloc(uint64_t curCycle) {
                stride = 1;
                lastPos = 0;
                lastLastPos = 0;
                lastPrefetchPos = 0;
                conf.reset();
                valid.reset();
                lastCycle = curCycle;
            }
        };

        uint64_t timestamp;  // for LRU
        Address tag[16];
        Entry array[16];

        Counter profAccesses, profPrefetches, profDoublePrefetches, profPageHits, profHits, profShortHits, profStrideSwitches, profLowConfAccs;

        MemObject* parent;
        BaseCache* child;
        uint32_t childId;
        g_string name;

    public:
        explicit StreamPrefetcher(const g_string& _name) : timestamp(0), name(_name) {}
        void initStats(AggregateStat* parentStat);
        const char* getName() { return name.c_str();}
        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector<BaseCache*>& children, Network* network);

        uint64_t access(MemReq& req);
        uint64_t invalidate(const InvReq& req);
};


/* The L2 streamer of a host core (Intel 64 and IA-32 Architectures Optimization Reference Manual,
 * 248966-048, "Data Prefetch to the L2 and Last Level Cache", p. 2-17): it watches the L1D's requests
 * to the L2 (loads and stores) for ascending and descending line sequences and prefetches the lines
 * ahead of them into the L2 (and, through the L2's own miss, the LLC):
 *   - up to `streams` streams (32); one forward and one backward stream per 4 KB page;
 *   - prefetched lines stay in the 4 KB page of the request that triggered them;
 *   - at most `perLookup` prefetch requests (2) per L2 lookup, at most `distance` lines (20) ahead of
 *     the request;
 *   - the spatial prefetcher (`spatial`) completes every line the L2 fetches with its pair line in the
 *     128-byte aligned block.
 * A stream is detected by two requests to one page whose line offsets move in one direction (the
 * fewest that define a direction). An L2 miss (demand or prefetch) holds one of the L2's `queue`
 * superqueue entries until its line arrives (Golden Cove: 48, "only applies to the first miss for a
 * cache line"; fill buffer and superqueue entries can also be occupied by prefetch requests -- Chips and
 * Cheese, "Popping the Hood on Golden Cove"); a prefetch is not issued when none is free (the manual:
 * with many outstanding requests the streamer prefetches less far ahead).
 * Prefetches are side requests: the line is brought into the L2 (MESI: a PREFETCH GETS touches the
 * bottom controller only), the demand that later finds it waits for its arrival when the prefetch is
 * still in flight, and the prefetch's own timing record runs beside the demand's (writeback-style:
 * started with it, never waited for), so a core never waits for a prefetch it did not consume. */
class L2StreamPrefetcher : public BaseCache {
    public:
        struct Params { uint32_t streams = 32, distance = 20, perLookup = 2, queue = 48; bool spatial = true; };
    private:
        static const uint32_t MAX_STREAMS = 64, MAX_QUEUE = 128;
        struct Stream {
            uint64_t page = ~0ull;
            int32_t dir = 0;         // +1 forward, -1 backward, 0 training (one request seen)
            int32_t lastPos = 0;     // line offset in the page of the latest request
            int32_t pfPos = -1;      // furthest line offset prefetched ahead (-1: none)
            uint64_t ts = 0;         // LRU
            std::bitset<64> pending; // lines of the page prefetched by this entry and not yet consumed
            uint64_t resp[64];       // their arrival cycles
            uint64_t iss[64];        // their issue cycles
            uint32_t tag[64];        // their reconciliation tags (the replay settles a request's wait against the arrival it measured)
        };
        struct Slot { uint64_t start = 0, end = 0; };   // an outstanding-request entry of the L2

        Params p;
        Stream streamTab[MAX_STREAMS];
        Slot slots[MAX_QUEUE];
        uint64_t timestamp = 0;
        uint32_t core = ~0u;     // the core whose L1D this prefetcher serves (its requests' source)
        uint32_t tagSeq = 0;     // the latest reconciliation tag (per core)

        Counter profAccesses, profDemandL2Miss, profIssued, profIssuedFetched, profUseful, profLate, profDroppedQueue,
                profStreamsDetected, profSpatial;

        MemObject* parent = nullptr;
        BaseCache* child = nullptr;
        uint32_t childId = 0;
        uint32_t parentHitLat = 0;
        g_string name;

        uint32_t inFlight(uint64_t t) const;
        void takeSlot(uint64_t start, uint64_t end);
        bool isPending(uint64_t page, uint32_t pos) const;
        Stream* allocate(uint64_t page, int32_t pos);
        void forget(Stream& e, uint32_t pos);
        bool issue(MemReq& req, uint64_t reqCycle, uint64_t lineAddr, Stream& owner, TimingRecord* recs, uint32_t& nrec, bool spatial);

    public:
        L2StreamPrefetcher(const g_string& _name, const Params& _p);
        void initStats(AggregateStat* parentStat);
        const char* getName() { return name.c_str(); }
        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector<BaseCache*>& children, Network* network);
        uint64_t access(MemReq& req);
        uint64_t invalidate(const InvReq& req) { return child->invalidate(req); }
        /* The superqueue entries of core `core`'s L2 (0: no L2 prefetcher), known once the core's first L1D miss
         * has passed: a replay that releases a core's prefetch records in a closed loop gates them by it. */
        static uint32_t superqueueOf(uint32_t core);
};

#endif  // PREFETCHER_H_
