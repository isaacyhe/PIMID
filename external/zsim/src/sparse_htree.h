/* sparse_htree.h -- SHARED, deterministic sparse placement-driven DRAM H-tree.
 *
 * SINGLE SOURCE OF TRUTH (invariant #2): the topology emitter (src/main.cpp) and
 * the PE-MI routing (pe_memory_interface.h) both call buildSparseHTree() and
 * SparseHTree::endpointForUnit(), so endpoint ids can never drift between the
 * emitted Garnet topology and the runtime routing.
 *
 * MODEL: only PE-hosting branches are materialized, down to the placement level.
 *  - Each PE home unit -> a leaf ENDPOINT; endpoint e == PE e.
 *  - For every router that has EMPTY (no-PE) children, ONE abstract ENDPOINT is
 *    attached at that router -- the maximal-empty-subtree root, on the genuinely
 *    shared parent link (invariant #4: abstract = terminus, no deeper). A non-PE
 *    unit routes to the abstract endpoint of its DEEPEST LIVE ancestor, so near
 *    (same bank as a PE) vs far (other channel) is tiered by real tree distance.
 *  - Below the tree = the aggregate DRAM model (tech base latency + M/D/1 +
 *    channel-BW), charged by the caller. No per-unit nodes, no compute.
 *
 * Node count ~ PEs x tree-depth, not unit count -> sparse sweeps stay tiny.
 */
#ifndef PIMID_SPARSE_HTREE_H_
#define PIMID_SPARSE_HTREE_H_

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <set>

namespace pimid_htree {

// Decompose a flat unit id (at placement level `peLevel`) into hierarchy coords,
// TOP->LEAF (path[0]=channel ... path.back()=the peLevel unit). Matches
// hierarchy_util.h::unitToHierPos. fan[k] = children per parent for k->k+1:
//   0->1 subarrays/bank, 1->2 banks/bg, 2->3 bg/chip, 3->4 chips/rank,
//   4->5 ranks/channel; above channel = ROOT.
inline std::vector<long> decompose(uint64_t unit, int peLevel,
                                   int SA, int BpBG, int BGpC, int CpR, int RpCh) {
    long fan[5] = { SA > 0 ? SA : 1, BpBG > 0 ? BpBG : 1, BGpC > 0 ? BGpC : 1,
                    CpR > 0 ? CpR : 1, RpCh > 0 ? RpCh : 1 };
    std::vector<long> fineToCoarse;
    long id = (long)unit;
    int lvl0 = peLevel < 0 ? 0 : peLevel;
    for (int lvl = lvl0; lvl < 5; ++lvl) {
        long fn = fan[lvl] > 0 ? fan[lvl] : 1;
        fineToCoarse.push_back(id % fn);
        id /= fn;
    }
    fineToCoarse.push_back(id);                 // channel (top)
    return std::vector<long>(fineToCoarse.rbegin(), fineToCoarse.rend());  // top->leaf
}

struct Link { int a, b, w, lat; };

struct SparseHTree {
    int numRouters = 1;           // ROOT=0 + internal routers
    int numPEs = 0;               // PE endpoints [0, numPEs)
    int numAbstract = 0;          // abstract endpoints [numPEs, numPEs+numAbstract)
    std::vector<Link> intLinks;   // router<->router (both directions emitted by caller)
    std::vector<Link> extLinks;   // endpoint->router (a=endpoint, b=router, w, lat)

    // dims for endpointForUnit
    int peLevel = 1, SA = 1, BpBG = 1, BGpC = 1, CpR = 1, RpCh = 1;
    std::map<std::string,int> routerOf;   // coord-prefix key -> router id
    std::map<int,int> abstractOf;         // router id -> abstract endpoint id (if it has empties)
    std::map<int,int> peOfLeaf;           // leaf router id -> PE index

    /* 1.10: an abstract endpoint's COVERAGE -- how many PE-level memory
     * organisations sit behind it. The build used to test `live < fanout` and
     * then discard `fanout - live`, so an endpoint knew it stood for something
     * but not how much. That missing number is why the power model could not be
     * driven off this tree and fell back to organisation fan-out instead.
     *
     * Coverage does NOT change what the interface costs. An interface is priced
     * by the LINK it terminates -- buffer depth is that link's bandwidth-delay
     * product, port width is that link's width -- so one interface fronting many
     * organisations costs the same as one fronting a single organisation. That
     * is exactly why memory without a processing element can be aggregated and a
     * processing element cannot: a PE injects continuously and needs its own,
     * while memory only responds, at a rate the link above it already caps.
     *
     * What coverage IS for: charging the ARRAY behind the interface, sizing the
     * link against the aggregate it carries, and reporting an endpoint as the
     * concrete thing it represents rather than an unnamed placeholder. */
    std::map<int,long> coverageOf;        // abstract endpoint id -> PE-level orgs behind it
    std::map<int,int>  frontsLevel;       // abstract endpoint id -> level those orgs hang below

    /* 1.11: per-LEVEL census of what was actually built, for the power model's
     * hierarchical (levels) machinery. branchAtLevel counts routers with >= 2
     * children -- arbitration that exists. endpointsAtLevel counts network
     * endpoints attached at that level: PE leaves at the placement level, and
     * each aggregated region at the level it fronts. A level with neither is
     * pure pass-through wire and costs the power model nothing. */
    int branchAtLevel[7] = {0,0,0,0,0,0,0};
    int endpointsAtLevel[7] = {0,0,0,0,0,0,0};

    /* 1.11.92 (F4): the tree LEVEL of every router, indexed by router id
     * (ROOT = 6 ... the placement level). The census above counts routers
     * per level but threw away which router sat where, so nothing downstream
     * could attribute a measured per-router traversal count to a level --
     * and the power model split one hop total across levels by a preset.
     * Filled from the same info map the build maintains, so it cannot
     * disagree with the tree it describes. */
    std::vector<int> levelOfRouter;

    int totalEndpoints() const { return numPEs + numAbstract; }

    /* 1.10: total PE-level organisations this tree accounts for -- one per PE
     * (a PE sits AT its organisation) plus every organisation behind an abstract
     * endpoint. Must equal the memory's total organisation count. */
    long coveredOrgs() const {
        long n = numPEs;
        for (const auto& kv : coverageOf) n += kv.second;
        return n;
    }

    // Endpoint a target unit routes to: PE index if the unit hosts a PE; else the
    // abstract endpoint of the unit's deepest LIVE ancestor. -1 only if the map is
    // empty (no PEs). Both sides call THIS -> ids agree.
    int endpointForUnit(uint64_t unit) const {
        std::vector<long> path = decompose(unit, peLevel, SA, BpBG, BGpC, CpR, RpCh);
        std::string key;
        int parent = 0;
        for (size_t i = 0; i < path.size(); ++i) {
            key += "/" + std::to_string(path[i]);
            auto it = routerOf.find(key);
            if (it == routerOf.end()) {
                // diverges into an empty region here -> parent's abstract endpoint
                auto a = abstractOf.find(parent);
                return (a != abstractOf.end()) ? a->second : -1;
            }
            parent = it->second;
        }
        auto p = peOfLeaf.find(parent);   // full path live -> a PE leaf
        return (p != peOfLeaf.end()) ? p->second : -1;
    }
};

// children per parent at router level L (6=ROOT/system down to 1=bank; leaf L=peLevel has none)
inline int childFanout(int L, int N, int SA, int BpBG, int BGpC, int CpR, int RpCh) {
    switch (L) {
        case 6: return N > 0 ? N : 1;          // ROOT -> channels
        case 5: return RpCh > 0 ? RpCh : 1;    // channel -> ranks
        case 4: return CpR > 0 ? CpR : 1;      // rank -> chips
        case 3: return BGpC > 0 ? BGpC : 1;    // chip -> bank-groups
        case 2: return BpBG > 0 ? BpBG : 1;    // bank-group -> banks
        case 1: return SA > 0 ? SA : 1;        // bank -> subarrays
        default: return 1;
    }
}

// PE-level organisations beneath ONE node at level L. A node at level L has
// childFanout(L) children at L-1, so the count multiplies down to the PE level.
// L == peLevel is the organisation itself: 1.
inline long orgsBelow(int L, int peLevel, int N,
                      int SA, int BpBG, int BGpC, int CpR, int RpCh) {
    long n = 1;
    for (int l = peLevel + 1; l <= L; ++l) {
        n *= (long)childFanout(l, N, SA, BpBG, BGpC, CpR, RpCh);
    }
    return n;
}

/* The ROOT's real fan-out. childFanout(6) returns the channel count, but some
 * technologies FOLD their channels into a lower level -- HBM puts channels per
 * stack into chips_per_rank (HBM2 8, HBM3 16) -- so for those the channel index
 * that decompose() produces is always zero and the root has exactly one live
 * child. Taking childFanout(6) at face value there invents empty channels that
 * do not exist, and an abstract endpoint covering memory that is already counted
 * below. Same double-book the 1.9.35 HBM rank guard refuses in the other
 * direction.
 *
 * Derive it from the organisation id space instead, which cannot double-count:
 * whatever the unit ids actually span, divided by what one channel holds. */
inline int rootFanout(int peLevel, int N,
                      int SA, int BpBG, int BGpC, int CpR, int RpCh) {
    long perChannel = orgsBelow(5, peLevel, N, SA, BpBG, BGpC, CpR, RpCh);
    long declared   = (long)childFanout(6, N, SA, BpBG, BGpC, CpR, RpCh);
    if (perChannel <= 0) return 1;
    /* If the channel count is already folded below, the id space spans one
     * channel and the root is single-child. Detect that rather than assume it:
     * a folded technology has its channel count equal to a lower fan-out. */
    if (declared > 1 && (long)CpR == declared) return 1;
    return (int)(declared > 0 ? declared : 1);
}

/* 1.10: which ROUTER LEVEL carries the channel dimension.
 *
 * The seven level names are DDR-shaped -- subarray, bank, bankgroup, chip, rank,
 * channel, system -- and HBM does not fit them. An HBM stack has no separate
 * chip dimension, so its channel count is stored in chips_per_rank (the source
 * comment says so: "chips_per_rank = channels per stack (HBM2 8, HBM3 16)").
 * For those parts the CHIP level IS the channel level, and the level named
 * "channel" is empty.
 *
 * This matters because a channel is where the memory model's coverage stops.
 * Aggregating memory ACROSS channels puts several independent data buses behind
 * one endpoint: their parallelism disappears, and the path out to each of them
 * is below the network endpoint and outside the memory model, so nothing prices
 * it. Aggregating WITHIN a channel is fine -- the memory model covers it.
 *
 * Detected, not assumed, by the same test rootFanout uses: a folded technology
 * has its channel count equal to a lower fan-out. */
inline int channelBearingLevel(int N, int CpR) {
    return (N > 1 && CpR == N) ? 3 : 5;   // 3 = chip slot (folded), else 5
}

/* 1.10.3: WHICH LAYER a link belongs to is a property of the TIER it crosses,
 * not of how far it happens to sit from the leaf.
 *
 * The four layers describe a physical ladder: L0 is the innermost link (widest,
 * a subarray or bank datapath), L3 is the channel link (narrowest, the DQ bus
 * out of the channel). Those widths and clocks come from each technology's
 * organisation, so they belong to named tiers.
 *
 * Choosing by distance from the leaf -- min(hops, 3) -- makes a link's width
 * depend on how deep the tree happens to be. The same physical channel link is
 * L3 in a subarray-placed tree, and L1 or L0 in a chip-placed one, because the
 * tree above a coarse placement is shorter. The channel DQ bus, the narrowest
 * link in the device, would then be priced with the width of an inner subarray
 * datapath -- and in the direction that makes the memory look faster than it is.
 *
 * Measure inward from the channel instead, which is where the ladder is anchored
 * and where the memory model's coverage stops. channelBearingLevel already knows
 * that HBM folds its channels into the chip slot, so a folded technology gets
 * its channel link labelled L3 rather than L2. */
inline int layerForLevel(int childLevel, int chanLevel) {
    int d = chanLevel - childLevel;      // tiers inward from the channel
    if (d <= 0) return 3;                // the channel link itself (and above)
    if (d >= 3) return 0;                // innermost: subarray/bank datapath
    return 3 - d;                        // 1 tier in -> L2, 2 tiers in -> L1
}

// Build the sparse tree from the PE home units. layerW/layerLat index the 4 link
// layers (0=leaf/widest ... 3=channel/narrowest), assigned by the tier each link
// crosses (see layerForLevel).
inline SparseHTree buildSparseHTree(const std::vector<uint64_t>& peHomes,
                                    int peLevel, int N,
                                    int SA, int BpBG, int BGpC, int CpR, int RpCh,
                                    const int layerW[4], const int layerLat[4]) {
    SparseHTree t;
    t.peLevel = peLevel < 0 ? 0 : peLevel;
    t.SA = SA; t.BpBG = BpBG; t.BGpC = BGpC; t.CpR = CpR; t.RpCh = RpCh;
    t.routerOf[""] = 0;
    int nextRouter = 1;
    // Which level carries the channel: the anchor the link layers are measured
    // inward from (folded for HBM, where the chip slot IS the channel).
    const int chanLevel = channelBearingLevel(N, CpR);
    // per-router: (level, set of live child coords)
    std::map<int, std::pair<int, std::set<long>>> info;
    info[0] = { 6, {} };                       // ROOT at level 6 (system)

    for (size_t p = 0; p < peHomes.size(); ++p) {
        std::vector<long> path = decompose(peHomes[p], t.peLevel, SA, BpBG, BGpC, CpR, RpCh);
        int Lp = (int)path.size();
        std::string key;
        int parent = 0;
        for (int i = 0; i < Lp; ++i) {
            info[parent].second.insert(path[i]);      // parent now has this live child
            key += "/" + std::to_string(path[i]);
            auto it = t.routerOf.find(key);
            int rid;
            if (it == t.routerOf.end()) {
                rid = nextRouter++;
                t.routerOf[key] = rid;
                int parentLevel = info[parent].first;
                int childLevel = parentLevel - 1;
                info[rid] = { childLevel, {} };
                int li = layerForLevel(childLevel, chanLevel);
                t.intLinks.push_back({ parent, rid, layerW[li], layerLat[li] });
            } else {
                rid = it->second;
            }
            parent = rid;
        }
        t.peOfLeaf[parent] = (int)p;
        t.extLinks.push_back({ (int)p, parent, layerW[0], layerLat[0] });  // PE at its unit -> L0
    }
    t.numPEs = (int)peHomes.size();
    t.numRouters = nextRouter;

    /* 1.10: MATERIALISE EVERY CHANNEL before aggregating.
     *
     * Aggregation collapses a router's empty children into one endpoint. Left
     * unchecked that merges across channels: measured on a single-element HBM3
     * configuration, ONE endpoint fronted 480 organisations -- fifteen entire
     * channels -- because HBM keeps its channels in the chip slot and the level
     * merely looked like a safe sub-channel one.
     *
     * Fifteen channels have fifteen independent data buses. Behind one endpoint
     * their parallelism vanishes, and the path to each is below the network
     * endpoint yet outside the memory model, so nothing prices it.
     *
     * So the channel dimension is always real: every channel gets its own
     * router, whether or not a processing element lives in it. Aggregation then
     * happens strictly WITHIN a channel, where the memory model's coverage
     * holds. Empty channels each get their own endpoint from the pass below --
     * which is what they physically are, not a fifteenth of one thing.
     *
     * Costs a handful of routers on sparse placements and buys a tree whose
     * every endpoint sits inside exactly one channel. */
    {
        int chLevel = channelBearingLevel(N, CpR);
        std::vector<std::pair<int,std::string>> toScan;  // (router id, key prefix)
        for (auto& kv : t.routerOf) toScan.push_back({ kv.second, kv.first });
        for (auto& rk : toScan) {
            auto it = info.find(rk.first);
            if (it == info.end()) continue;
            int level = it->second.first;
            if (level != chLevel + 1) continue;      // parent of the channel tier
            int fanout = (level == 6)
                       ? rootFanout(t.peLevel, N, SA, BpBG, BGpC, CpR, RpCh)
                       : childFanout(level, N, SA, BpBG, BGpC, CpR, RpCh);
            for (int c = 0; c < fanout; ++c) {
                std::string key = rk.second + "/" + std::to_string(c);
                if (t.routerOf.find(key) != t.routerOf.end()) continue;  // already live
                int rid = nextRouter++;
                t.routerOf[key] = rid;
                info[rid] = { chLevel, {} };
                info[rk.first].second.insert(c);      // now a live child of its parent
                int li = layerForLevel(chLevel, chanLevel);
                t.intLinks.push_back({ rk.first, rid, layerW[li], layerLat[li] });
            }
        }
        t.numRouters = nextRouter;
    }

    // Abstract endpoints: a router at level L (> peLevel) whose live-child count is
    // below its fan-out has empty children -> one abstract endpoint, hung on it via
    // the child link layer. Deterministic order = router id.
    int nextEndpoint = t.numPEs;
    for (auto& kv : info) {
        int rid = kv.first;
        int level = kv.second.first;
        int live = (int)kv.second.second.size();
        if (level <= t.peLevel) continue;             // leaf level: below = Ramulator
        int fanout = (level == 6)
                   ? rootFanout(t.peLevel, N, SA, BpBG, BGpC, CpR, RpCh)
                   : childFanout(level, N, SA, BpBG, BGpC, CpR, RpCh);
        if (live < fanout) {                          // has empty children
            int abst = nextEndpoint++;
            t.abstractOf[rid] = abst;
            int childLevel = level - 1;               // the empty children's level
            /* 1.10: what this endpoint concretely stands for -- every PE-level
             * organisation beneath each empty child. Summed over all abstract
             * endpoints and added to the PE count, this must equal the total
             * organisation count exactly; the caller gates on that, because a
             * mismatch means the tree does not cover the memory. */
            t.coverageOf[abst] = (long)(fanout - live)
                               * orgsBelow(childLevel, t.peLevel, N,
                                           SA, BpBG, BGpC, CpR, RpCh);
            t.frontsLevel[abst] = childLevel;
            int li = layerForLevel(childLevel, chanLevel);
            t.extLinks.push_back({ abst, rid, layerW[li], layerLat[li] });
        }
    }
    t.numAbstract = nextEndpoint - t.numPEs;

    /* 1.11: census the built tree by level. Uses the same info map the build
     * maintained, so this cannot disagree with the tree it describes. */
    t.levelOfRouter.assign((size_t)t.numRouters, -1);   // 1.11.92 (F4)
    for (const auto& kv : info) {
        int lvl = kv.second.first;
        if (kv.first >= 0 && kv.first < t.numRouters)
            t.levelOfRouter[(size_t)kv.first] = lvl;
        if (lvl < 0 || lvl > 6) continue;
        if ((int)kv.second.second.size() >= 2) t.branchAtLevel[lvl]++;
    }
    {
        int pl = t.peLevel < 0 ? 0 : t.peLevel;
        t.endpointsAtLevel[pl] += t.numPEs;
        for (const auto& kv : t.frontsLevel) {
            int lvl = kv.second;
            if (lvl >= 0 && lvl <= 6) t.endpointsAtLevel[lvl]++;
        }
    }
    return t;
}

} // namespace pimid_htree

#endif // PIMID_SPARSE_HTREE_H_
