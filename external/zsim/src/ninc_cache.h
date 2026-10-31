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

#ifndef NINC_CACHE_H_
#define NINC_CACHE_H_

#include <unordered_set>
#include <vector>

#include "cache.h"
#include "locks.h"
#include "pad.h"
#include "stats.h"

/* A shared last-level cache that is NOT inclusive of the private caches below
 * it, the organisation of the Intel server last-level cache since Skylake-SP
 * and of Sapphire Rapids (Intel, "The New Intel Xeon Scalable Processor
 * (formerly Skylake-SP)", Hot Chips 29, 2017: "private L2 becomes primary
 * cache with shared L3 used as overflow cache"; Chips and Cheese, "Sapphire
 * Rapids: Golden Cove Hits Servers", 2023: "SPR's L3 is clearly
 * non-inclusive").
 *
 * Two structures, as in the hardware:
 *  - the data array: a set-associative cache of the configured size and ways
 *    (LRU), which holds the lines the children have given up and the lines
 *    shared across them;
 *  - the snoop filter: a directory of every line a child holds (sharers,
 *    whether one child holds it exclusively, and the line's state with respect
 *    to memory). It holds no data. It is sized by what the children hold, so it
 *    never has to invalidate a child to make room, and coherence is exact.
 *
 * Policy (the three points of the Hot Chips slide "Inclusive vs Non-Inclusive
 * L3"):
 *  1. "Memory reads fill directly to the L2, no longer to both the L2 and L3":
 *     a line read from memory fills the requesting child only.
 *  2. "When a L2 line needs to be evicted, both modified and unmodified lines
 *     are written back": a child's eviction, clean or dirty, is written into
 *     the data array. A dirty data-array victim no child holds is written back
 *     to memory; a clean one is dropped.
 *  3. "Data shared across cores are copied into the L3 for servicing future L2
 *     misses": a line read while another child holds it is kept here as well.
 *     A line found here that no other child holds moves to the requester (the
 *     L2 is the primary cache), and a line requested for writing leaves this
 *     cache (the writer's L2 holds the only copy).
 *
 * One access makes at most one memory request (a read, or the write-back of the
 * data-array victim of a fill), so one access never leaves two timing records. */
class NonInclusiveCache : public Cache {
    private:
        struct SFEntry {
            Address tag;
            uint32_t numSharers;   // 0: a free slot
            uint8_t exclusive;
            uint8_t mem;           // the line's state with respect to memory (S, E or M)
            bool isExclusive() const { return numSharers == 1 && exclusive; }
        };

        // data array
        uint32_t dWays, dSets;
        Address* dTag;
        uint8_t* dState;    // I: invalid; S or E: clean; M: dirty with respect to memory
        uint64_t* dStamp;   // LRU
        uint64_t dClock;

        // snoop filter: an open-addressing hash table (linear probing, backward-shift deletion), sharers as bit words
        SFEntry* sfTab;
        uint64_t* sfBits;
        uint32_t sfWords;
        uint32_t sfShift;
        uint64_t sfCap, sfUsed;

        g_vector<MemObject*> parents;
        g_vector<uint32_t> parentRTTs;
        uint32_t selfId;
        g_vector<BaseCache*> children;
        g_vector<uint32_t> childrenRTTs;

        PAD();
        lock_t ccLock;
        PAD();

        Counter profGETSHit, profGETXHit, profGETSMiss, profGETXMissIM, profGETXMissSM;
        Counter profPUTS, profPUTX;
        Counter profDataHit, profPeerHit, profFill, profFillDirty, profSharedCopy;
        Counter profDataEvict, profDataWB, profRace;
        Counter profGETNextLevelLat, profGETNetLat;

        uint32_t getParentId(Address lineAddr) const;

        int32_t dLookup(Address lineAddr) const;
        void dTouch(uint32_t id) { dStamp[id] = ++dClock; }
        void dDrop(uint32_t id) { dTag[id] = 0; dState[id] = I; dStamp[id] = 0; }
        void dFill(Address lineAddr, MESIState st, uint64_t cycle, uint32_t srcId);   // write a line into the data array

        uint64_t sfHome(Address a) const { return (a * 0x9E3779B97F4A7C15ull) >> sfShift; }
        int64_t sfFind(Address a) const;
        uint64_t sfAdd(Address a, MESIState mem);   // a line no child holds yet; may grow the table (slot numbers change)
        void sfErase(uint64_t slot);
        void sfAlloc(uint64_t cap);
        bool sfHas(uint64_t slot, uint32_t c) const { return (sfBits[slot * sfWords + c / 64] >> (c % 64)) & 1; }
        void sfSet(uint64_t slot, uint32_t c) { sfBits[slot * sfWords + c / 64] |= (1ull << (c % 64)); }
        void sfClr(uint64_t slot, uint32_t c) { sfBits[slot * sfWords + c / 64] &= ~(1ull << (c % 64)); }

        uint64_t sendInvalidates(Address lineAddr, uint64_t slot, InvType type, bool* reqWriteback, uint64_t cycle, uint32_t srcId);

    public:
        NonInclusiveCache(uint32_t _numLines, uint32_t _ways, uint32_t _accLat, uint32_t _invLat, const g_string& _name);

        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector<BaseCache*>& children, Network* network);
        void initStats(AggregateStat* parentStat);

        uint64_t access(MemReq& req);
        uint64_t invalidate(const InvReq& req);
        uint64_t flushDirtyLines(std::unordered_set<Address>& distinctDirty, std::vector<Address>* order = nullptr);
};

#endif  // NINC_CACHE_H_
