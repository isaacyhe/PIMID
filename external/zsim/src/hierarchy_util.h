/**
 * hierarchy_util.h — Shared hierarchy position mapping and LCA traversal
 *
 * Used by:
 *   - PEMemoryInterface (pe_memory_interface.h) for local/remote routing
 *   - zsim_trace_driver.cpp for hierarchy latency injection
 *   - MPI timing handler in qemu_zsim_plugin.cpp for PE-to-PE latency
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HIERARCHY_UTIL_H_
#define HIERARCHY_UTIL_H_

#include <stdint.h>

struct HierPos {
    int subarray;    // L0
    int bank;        // L1
    int bank_group;  // L2
    int chip;        // L3
    int rank;        // L4
    int channel;     // L5 (device_id in multi-device)
    int system;      // L6 (system node in multi-host/device)
};

/**
 * Map a flat unit ID (bank, subarray, etc.) to a hierarchical position.
 *
 * @param unit_id          Flat unit index (depends on placement level)
 * @param placement        Placement level: 0=SUBARRAY, 1=BANK, ..., 4=RANK, 5=CHANNEL, 6=SYSTEM
 * @param sa_per_bank      Subarrays per bank
 * @param banks_per_bg     Banks per bank group
 * @param bg_per_chip      Bank groups per chip
 * @param chips_per_rank   Chips per rank (default 1 for backward compat)
 * @param ranks_per_channel Ranks per channel (default 1 for backward compat)
 */
static inline HierPos unitToHierPos(uint32_t unit_id, uint32_t placement,
                                     uint32_t sa_per_bank, uint32_t banks_per_bg,
                                     uint32_t bg_per_chip,
                                     uint32_t chips_per_rank = 1,
                                     uint32_t ranks_per_channel = 1) {
    HierPos pos = {0, 0, 0, 0, 0, 0, 0};
    int banks_per_chip = (int)(banks_per_bg * bg_per_chip);
    int banks_per_rank = banks_per_chip * (int)chips_per_rank;
    int banks_per_channel = banks_per_rank * (int)ranks_per_channel;

    switch (placement) {
        case 0: { // SUBARRAY
            pos.subarray = unit_id % sa_per_bank;
            int bank_id = unit_id / sa_per_bank;
            pos.bank = bank_id % (int)banks_per_bg;
            pos.bank_group = (bank_id / (int)banks_per_bg) % (int)bg_per_chip;
            int chip_id = bank_id / banks_per_chip;
            pos.chip = chip_id % (int)chips_per_rank;
            int rank_id = chip_id / (int)chips_per_rank;
            pos.rank = rank_id % (int)ranks_per_channel;
            pos.channel = rank_id / (int)ranks_per_channel;
            break;
        }
        case 1: { // BANK (default)
            pos.bank = unit_id % (int)banks_per_bg;
            pos.bank_group = (unit_id / (int)banks_per_bg) % (int)bg_per_chip;
            int chip_id = unit_id / banks_per_chip;
            pos.chip = chip_id % (int)chips_per_rank;
            int rank_id = chip_id / (int)chips_per_rank;
            pos.rank = rank_id % (int)ranks_per_channel;
            pos.channel = rank_id / (int)ranks_per_channel;
            break;
        }
        case 2: { // BANK_GROUP
            pos.bank_group = unit_id % (int)bg_per_chip;
            int chip_id = unit_id / (int)bg_per_chip;
            pos.chip = chip_id % (int)chips_per_rank;
            int rank_id = chip_id / (int)chips_per_rank;
            pos.rank = rank_id % (int)ranks_per_channel;
            pos.channel = rank_id / (int)ranks_per_channel;
            break;
        }
        case 3: { // CHIP
            pos.chip = unit_id % (int)chips_per_rank;
            int rank_id = unit_id / (int)chips_per_rank;
            pos.rank = rank_id % (int)ranks_per_channel;
            pos.channel = rank_id / (int)ranks_per_channel;
            break;
        }
        case 4: { // RANK
            pos.rank = unit_id % (int)ranks_per_channel;
            pos.channel = unit_id / (int)ranks_per_channel;
            break;
        }
        case 5: pos.channel = unit_id; break;  // CHANNEL
        /* 1.11.60 (audit round 4, D005): level 6 is the emitter's LOGIC_DIE
         * ("SYSTEM" here is this file's older name for the same tier, the top
         * of the in-stack tree). Recorded because the naming difference read
         * as a level this file knew about and pe_memory_interface.h did not,
         * which is exactly how LOGIC_DIE ended up with no case in the row
         * model's switch. Decoding is unchanged. */
        case 6: pos.system = unit_id; break;    // SYSTEM == LOGIC_DIE
        default: pos.bank = unit_id; break;
    }
    return pos;
}

/**
 * Compute the Lowest Common Ancestor level between two positions.
 * Returns the hierarchy level (0=subarray, 1=bank, ..., 4=rank, 5=channel, 6=system)
 * at which the paths from src and dst first merge.
 */
static inline int computeLCA(const HierPos& a, const HierPos& b) {
    if (a.system != b.system) return 6;
    if (a.channel != b.channel) return 5;
    if (a.rank != b.rank) return 4;
    if (a.chip != b.chip) return 3;
    if (a.bank_group != b.bank_group) return 2;
    if (a.bank != b.bank) return 1;
    if (a.subarray != b.subarray) return 0;
    return 0;  // same position
}

/**
 * Compute hierarchy traversal latency between two units.
 * Routes UP from source level to LCA, across LCA, then DOWN to destination level.
 *
 * @param src_unit          Source unit ID
 * @param dst_unit          Destination unit ID
 * @param levelLatency      Per-level transfer latency (7 entries)
 * @param bridgeLatency     Per-gateway crossing latency (6 entries)
 * @param placement         Placement level (0-6)
 * @param sa_per_bank       Subarrays per bank
 * @param banks_per_bg      Banks per bank group
 * @param bg_per_chip       Bank groups per chip
 * @param chips_per_rank    Chips per rank (default 1)
 * @param ranks_per_channel Ranks per channel (default 1)
 */
static inline uint64_t computeHierTraversal(
        uint32_t src_unit, uint32_t dst_unit,
        const uint32_t levelLatency[7], const uint32_t bridgeLatency[6],
        uint32_t placement, uint32_t sa_per_bank,
        uint32_t banks_per_bg, uint32_t bg_per_chip,
        uint32_t chips_per_rank = 1, uint32_t ranks_per_channel = 1) {
    if (src_unit == dst_unit) return 0;

    HierPos src = unitToHierPos(src_unit, placement, sa_per_bank, banks_per_bg,
                                 bg_per_chip, chips_per_rank, ranks_per_channel);
    HierPos dst = unitToHierPos(dst_unit, placement, sa_per_bank, banks_per_bg,
                                 bg_per_chip, chips_per_rank, ranks_per_channel);
    int lca = computeLCA(src, dst);
    int start = (int)placement;

    uint64_t total = 0;
    // UP: source level -> LCA
    for (int l = start; l < lca; l++) {
        total += levelLatency[l];
        if (l < 6) total += bridgeLatency[l];
    }
    // AT LCA
    total += levelLatency[lca];
    // DOWN: LCA -> destination level
    for (int l = lca - 1; l >= start; l--) {
        if (l < 6) total += bridgeLatency[l];
        total += levelLatency[l];
    }
    return total;
}

/* 1.11.92 (F1): the SAME tier walk as computeHierTraversal, counting instead
 * of charging. perLevel[l] += the number of times the walk visits tier l's
 * node: twice for every tier below the lowest common ancestor (up and back
 * down), once at it -- links + 1 router visits for a walk of `links` links,
 * the same traversal semantics as the detailed path. Returns the link count
 * (2 x (lca - placement)); 0 and no visits for src == dst, which does not
 * traverse, exactly as computeHierTraversal charges it nothing. */
static inline uint32_t hierTraversalLevels(
        uint32_t src_unit, uint32_t dst_unit, uint32_t perLevel[7],
        uint32_t placement, uint32_t sa_per_bank,
        uint32_t banks_per_bg, uint32_t bg_per_chip,
        uint32_t chips_per_rank = 1, uint32_t ranks_per_channel = 1) {
    for (int l = 0; l < 7; l++) perLevel[l] = 0;
    if (src_unit == dst_unit) return 0;
    HierPos src = unitToHierPos(src_unit, placement, sa_per_bank, banks_per_bg,
                                 bg_per_chip, chips_per_rank, ranks_per_channel);
    HierPos dst = unitToHierPos(dst_unit, placement, sa_per_bank, banks_per_bg,
                                 bg_per_chip, chips_per_rank, ranks_per_channel);
    int lca = computeLCA(src, dst);
    int start = (int)placement;
    for (int l = start; l < lca; l++)
        if (l >= 0 && l < 7) perLevel[l] += 2;
    if (lca >= 0 && lca < 7) perLevel[lca] += 1;
    return (lca > start) ? (uint32_t)(2 * (lca - start)) : 0u;
}

/**
 * Map a PE to its primary (home) memory organization unit using the
 * flattened mapping table.  Returns pe_id when no mapping is configured
 * (backward-compatible 1:1 identity).
 *
 * @param pe_id    PE index
 * @param offsets  Prefix-sum offsets into data[] (peMemMapSize+1 entries)
 * @param data     Flat mem_org_id array
 * @param mapSize  Number of PEs in the map (0 = legacy 1:1)
 */
static inline uint32_t peToHomeUnit(uint32_t pe_id,
        const uint32_t* offsets, const uint32_t* data, uint32_t mapSize) {
    if (mapSize == 0 || !offsets || !data) return pe_id;  // legacy 1:1
    if (pe_id >= mapSize) return pe_id;
    uint32_t idx = offsets[pe_id];
    if (idx >= 4096) return pe_id;  // safety: out-of-range offset
    return data[idx];  // primary = first mapped unit
}

/**
 * Compute PE-to-PE latency using the hierarchy.
 * PEs are mapped to units via the optional mapping table.
 *
 * @param mapOffsets        Prefix-sum offsets (or nullptr for 1:1)
 * @param mapData           Flat mem_org_id array (or nullptr for 1:1)
 * @param mapSize           Number of PEs in map (0 = legacy 1:1)
 * @param connMode          0=SHARED_IO, 1=SEPARATE_ENDPOINTS
 * @param linkLat           Local link latency (for SEPARATE_ENDPOINTS)
 * @param chips_per_rank    Chips per rank (default 1)
 * @param ranks_per_channel Ranks per channel (default 1)
 */
static inline uint64_t computePEtoPELatency(
        uint32_t src_pe, uint32_t dst_pe,
        const uint32_t levelLatency[7], const uint32_t bridgeLatency[6],
        uint32_t placement, uint32_t sa_per_bank,
        uint32_t banks_per_bg, uint32_t bg_per_chip,
        const uint32_t* mapOffsets = nullptr,
        const uint32_t* mapData = nullptr,
        uint32_t mapSize = 0,
        uint32_t connMode = 0,
        uint32_t linkLat = 0,
        uint32_t chips_per_rank = 1,
        uint32_t ranks_per_channel = 1) {
    if (src_pe == dst_pe) return 0;

    uint32_t src_unit = peToHomeUnit(src_pe, mapOffsets, mapData, mapSize);
    uint32_t dst_unit = peToHomeUnit(dst_pe, mapOffsets, mapData, mapSize);

    uint64_t lat = computeHierTraversal(src_unit, dst_unit, levelLatency, bridgeLatency,
                                         placement, sa_per_bank, banks_per_bg, bg_per_chip,
                                         chips_per_rank, ranks_per_channel);

    // For SEPARATE_ENDPOINTS: add PE→NI and NI→PE hops at both ends
    if (connMode == 1 && linkLat > 0) {
        lat += 2 * linkLat;
    }

    return lat;
}

#endif  // HIERARCHY_UTIL_H_
