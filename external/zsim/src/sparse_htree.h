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

struct SparseHTree;

/* 1.11.97 (ruling 7 (c); gate 1206B arm P2): the per-access tier walk of the
 * ANALYTICAL path, taken over the BUILT tree -- the same tree the detailed
 * path routes on and the power model is priced from.
 *
 * Until now the analytical path walked an arithmetic hierarchy
 * (hierarchy_util.h: unit id -> subarray/bank/bank-group/chip/rank/channel
 * digits, lowest common ancestor by digit), which places PE i at unit i. The
 * tree builder places PEs by the placement map (one per channel on an HBM
 * stack, round-robin over the leaves), hangs an aggregated endpoint on the
 * deepest live router of every empty region, and puts its one branch router
 * at the rank tier. The two disagreed on where the traffic goes: on HBM3,
 * 16 elements at BANK, the arithmetic walk put every remote access through
 * a chip-level LCA and nothing through the rank router, while Garnet
 * measured one rank-router crossing per packet (gate 1206B: analytical NoC
 * dynamic 8x detailed; 3.3x at equal work). This walker follows the tree:
 * endpoint -> its router -> parent pointers up to the lowest common ancestor
 * -> down to the destination endpoint's router. It counts the routers it
 * visits per tier (the quantity Garnet's crossbars count) and prices the
 * path as censusSparseTree does for the hotspot and mean-cost walk: every
 * router's tier latency plus the bridge at every tier boundary. */
/* 1.11.103 (sweep-94 ruling 9, user "a" 2026-10-05): the NON-TREE IN-DIE FABRIC.
 * A DRAM device whose configuration asks for a mesh, ring or crossbar is
 * modelled with that fabric INSIDE EACH DIE, over the die's organisations at
 * the placement tier (the grid: the bank groups x banks [x subarrays] of one
 * chip, gridW x gridH); the chip, rank and channel tiers above the die keep
 * the tree. Every grid node is a router at the placement level with its own
 * endpoint (the PE placed there, else an aggregated endpoint fronting that one
 * organisation); the grid's exit to the chip router is node (0,0) (MESH,
 * RING) or the hub (CROSSBAR). Hop counts come from the grid positions. */
struct InDieFabric {
    int kind = 0;        // 0 none (the tree), 1 MESH_2D, 2 RING, 3 CROSSBAR
    int gridW = 0, gridH = 0;
};
struct TreeWalker {
    std::vector<int> parentOf;    // router -> parent router (-1 at ROOT)
    std::vector<int> routerOfEp;  // endpoint -> the router it hangs on (-1 = none)
    bool built = false;

    void build(const SparseHTree& t);
    /* 1.11.103: the router sequence of the src -> dst path (the in-die grid walk,
     * the exit, the tree above, the far die's walk), the same sequence walk()
     * prices; the census takes its link loads and hop counts from it. */
    bool path(const SparseHTree& t, int srcEp, int dstEp, std::vector<int>& routers) const;

    /* Walk srcEp -> dstEp. perLevel[l] (l in 0..6) = routers of tier l the
     * walk visits (the LCA once, every other router on the up and down
     * paths once). Returns the number of router-to-router links. cost (if
     * non-null) = sum of levelLat[tier] over the routers visited + sum of
     * bridgeLat[min(tier_a, tier_b)] over the links crossed, in the caller's
     * cycle unit. Returns 0 links and no visits when either endpoint is
     * unknown (caller falls back to the arithmetic walk). srcEp == dstEp or
     * both on one router: that router once, no links. */
    uint32_t walk(const SparseHTree& t, int srcEp, int dstEp, uint32_t perLevel[8],
                  const uint32_t* levelLat, const uint32_t* bridgeLat, uint64_t* cost) const;
};

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
    /* 1.11.103: the in-die fabric. gridLinks are the node<->node links of the
     * grids (kept apart from intLinks so the tree's parent map stays a tree);
     * gridX/gridY/gridDie index every router (-1 = not a grid node; a hub has
     * gridX -1 and a die); dieExitOf maps the chip router of a gridded die to
     * its exit node (the hub for a CROSSBAR). */
    int fabricKind = 0, gridW = 0, gridH = 0;
    std::vector<Link> gridLinks;
    std::vector<int> gridX, gridY, gridDie;
    std::map<int,int> dieExitOf;
    int gridRouters = 0;

    int totalEndpoints() const { return numPEs + numAbstract; }

    /* 1.10: total PE-level organisations this tree accounts for -- one per PE
     * (a PE sits AT its organisation) plus every organisation behind an abstract
     * endpoint. Must equal the memory's total organisation count. */
    long coveredOrgs() const {
        /* 1.11.94 (H14 m02-main-1576-3440-1): one organisation per DISTINCT
         * PE home, not per PE. PEs that share a home organisation land on the
         * same leaf router (peOfLeaf has one entry per leaf), and counting
         * numPEs counted that organisation once per PE, so 8 PEs on 4
         * organisations "covered" 8 and the coverage gate in main.cpp exited.
         * When every PE has its own home (every corpus cell) the two counts
         * are equal. */
        long n = (long)peOfLeaf.size();
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
        if (p != peOfLeaf.end()) return p->second;
        /* 1.11.94 (H13 x02-zsim-garnet-htree-2): a full path can also end on
         * a materialised channel router that hosts no PE (a placement at or
         * above the channel tier); its own aggregated endpoint serves it. */
        auto a = abstractOf.find(parent);
        return (a != abstractOf.end()) ? a->second : -1;
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
    /* 1.11.94 (H13 x02-zsim-garnet-htree-2 / H15): ...but only while the
     * placement sits BELOW the folded channel tier. At CHANNEL (5) or
     * LOGIC_DIE (6) decompose() yields the unit id itself as the root's child
     * coordinate, and that id spans the channels (slots = channel count), so
     * the root really has `declared` children. Returning 1 there made the
     * root's live-vs-fanout test blind: HBM3 with 4 PEs at CHANNEL or
     * LOGIC_DIE built a tree over 4 of 16 channels and the coverage gate
     * exited 2. Unfolded parts (declared == 1) are unchanged. */
    if (declared > 1 && (long)CpR == declared && peLevel < 5) return 1;
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
                                    const int layerW[4], const int layerLat[4],
                                    const InDieFabric* fab = nullptr) {
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
    /* 1.11.103: with an in-die fabric the tree stops at the chip (level 3);
     * the tiers below it are the die's grid, built after the PE paths. */
    const bool gridded = (fab && fab->kind > 0 && t.peLevel <= 2);
    if (gridded) { t.fabricKind = fab->kind; t.gridW = std::max(1, fab->gridW); t.gridH = std::max(1, fab->gridH); }
    std::map<int, std::vector<std::pair<size_t, std::vector<long>>>> peOfDie;   // chip router -> (pe, full path)

    for (size_t p = 0; p < peHomes.size(); ++p) {
        std::vector<long> path = decompose(peHomes[p], t.peLevel, SA, BpBG, BGpC, CpR, RpCh);
        int Lp = (int)path.size();
        std::string key;
        int parent = 0;
        for (int i = 0; i < Lp; ++i) {
            if (gridded && info[parent].first <= 3) { peOfDie[parent].push_back({ p, path }); break; }   // the die holds the rest
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
        if (gridded && info[parent].first <= 3) continue;                 // placed on the die's grid below
        t.peOfLeaf[parent] = (int)p;
        t.extLinks.push_back({ (int)p, parent, layerW[0], layerLat[0] });  // PE at its unit -> L0
    }
    t.numPEs = (int)peHomes.size();
    if (gridded) {
        /* the die's grid: every organisation of the chip at the placement tier
         * is a router at that level; node index = the path components below
         * the chip (bank group, bank[, subarray]) in row-major order; the PE
         * placed there is its endpoint, every other node gets an aggregated
         * endpoint fronting that one organisation (the loop below); the chip
         * router links to the exit node (0,0), or to the hub for a CROSSBAR */
        const int li0 = layerForLevel(t.peLevel, chanLevel);
        for (auto& kv : peOfDie) {
            const int chip = kv.first;
            const int chipLevel = info[chip].first;
            const int nodes = t.gridW * t.gridH;
            std::vector<int> nodeRouter((size_t)nodes, -1);
            std::string chipKey;
            for (auto& rk : t.routerOf) if (rk.second == chip) { chipKey = rk.first; break; }
            int hub = -1;
            if (t.fabricKind == 3) {
                hub = nextRouter++;
                info[hub] = { t.peLevel, {} };
                t.intLinks.push_back({ chip, hub, layerW[li0], layerLat[li0] });   // the exit: chip <-> hub
                t.dieExitOf[chip] = hub;
            }
            const long fan[3] = { (long)std::max(1, SA), (long)std::max(1, BpBG), (long)std::max(1, BGpC) };
            for (int n = 0; n < nodes; ++n) {
                std::string key = chipKey;                                // the chip's key + the node's components (top -> leaf)
                std::vector<long> ftc; long rem = n;
                for (int lvl = t.peLevel; lvl <= 2; ++lvl) { ftc.push_back(rem % fan[lvl]); rem /= fan[lvl]; }
                for (auto it = ftc.rbegin(); it != ftc.rend(); ++it) key += "/" + std::to_string(*it);
                const int rid = nextRouter++;
                nodeRouter[(size_t)n] = rid;
                t.routerOf[key] = rid;
                info[rid] = { t.peLevel, {} };
                if (t.fabricKind == 3) t.gridLinks.push_back({ hub, rid, layerW[li0], layerLat[li0] });
            }
            for (long c = 0; c < childFanout(chipLevel, N, SA, BpBG, BGpC, CpR, RpCh); ++c) info[chip].second.insert(c);   // fully live: the grid covers the die
            if (t.fabricKind != 3) { t.intLinks.push_back({ chip, nodeRouter[0], layerW[li0], layerLat[li0] }); t.dieExitOf[chip] = nodeRouter[0]; }
            if (t.fabricKind == 1) {                                    // MESH: 4-neighbour links
                for (int y = 0; y < t.gridH; ++y) for (int x = 0; x < t.gridW; ++x) {
                    const int a = nodeRouter[(size_t)(y * t.gridW + x)];
                    if (x + 1 < t.gridW) t.gridLinks.push_back({ a, nodeRouter[(size_t)(y * t.gridW + x + 1)], layerW[li0], layerLat[li0] });
                    if (y + 1 < t.gridH) t.gridLinks.push_back({ a, nodeRouter[(size_t)((y + 1) * t.gridW + x)], layerW[li0], layerLat[li0] });
                }
            } else if (t.fabricKind == 2 && nodes > 1) {                // RING: i <-> i+1 mod N
                for (int n = 0; n < nodes; ++n) { if (nodes == 2 && n == 1) break; t.gridLinks.push_back({ nodeRouter[(size_t)n], nodeRouter[(size_t)((n + 1) % nodes)], layerW[li0], layerLat[li0] }); }
            }
            if ((int)t.gridX.size() < nextRouter) { t.gridX.resize((size_t)nextRouter, -1); t.gridY.resize((size_t)nextRouter, -1); t.gridDie.resize((size_t)nextRouter, -1); }
            for (int n = 0; n < nodes; ++n) { const int rid = nodeRouter[(size_t)n]; t.gridX[(size_t)rid] = n % t.gridW; t.gridY[(size_t)rid] = n / t.gridW; t.gridDie[(size_t)rid] = chip; }
            if (hub >= 0) { t.gridX[(size_t)hub] = -1; t.gridY[(size_t)hub] = -1; t.gridDie[(size_t)hub] = chip; }
            t.gridRouters += nodes + (hub >= 0 ? 1 : 0);
            for (auto& pp : kv.second) {                                // the die's PEs at their nodes
                const std::vector<long>& path = pp.second;
                const int below = 3 - t.peLevel;                        // components below the chip in the path (top -> leaf)
                long n = 0, mult = 1;
                for (int i = (int)path.size() - 1, lvl = t.peLevel; i >= (int)path.size() - below && lvl <= 2; --i, ++lvl) { n += path[(size_t)i] * mult; mult *= fan[lvl]; }
                if (n < 0 || n >= nodes) n = 0;
                const int rid = nodeRouter[(size_t)n];
                t.peOfLeaf[rid] = (int)pp.first;
                t.extLinks.push_back({ (int)pp.first, rid, layerW[0], layerLat[0] });
            }
        }
    }
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
        /* 1.11.94 (H13 / H15): at CHANNEL or LOGIC_DIE placement the tree's
         * channel routers are the root's children (level 5) whatever the
         * technology folds: decompose() returns the unit id, which IS the
         * channel, as the only coordinate. The folded chip-slot tier (3) does
         * not exist in such a tree, so materialising at chLevel found no
         * parent router and the empty channels were never built. Below the
         * channel tier (peLevel < 5) the level is unchanged. */
        if (t.peLevel >= 5) chLevel = 5;
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
    /* 1.11.94 (H13 x02-zsim-garnet-htree-2 / H15): THE EMPTY-REGION RULE AT
     * THE PE'S OWN LEVEL TOO. The routers of the placement level are the
     * leaves (level min(peLevel,5): decompose() puts a LOGIC_DIE unit on a
     * level-5 router). A leaf router that hosts no PE exists only when the
     * channel pass above materialised it -- an empty channel at a placement
     * at or above the channel tier (HBM2/HBM3/GDDR6 CHIP, any CHANNEL or
     * LOGIC_DIE). It used to be skipped by the `level <= peLevel` test, so
     * those channels had a router and no endpoint: endpointForUnit() returned
     * -1 for them and the coverage gate exited 2 (HBM3 4 PEs at LOGIC_DIE:
     * 4 of 16). Such a router now carries ONE aggregated endpoint covering
     * its own organisation (1), hung on an L0 link exactly as a PE in that
     * slot would be, so an empty channel and a PE channel are priced the same
     * up to the router. Routers BELOW the leaf level (the chip routers a
     * folded RANK placement materialises under its leaf) stay endpoint-free:
     * below the placement level is the memory model's. On every tree that
     * passed the coverage gate before, no such leaf existed, so nothing
     * changes there. */
    const int leafLevel = t.peLevel < 5 ? t.peLevel : 5;
    int nextEndpoint = t.numPEs;
    /* 1.11.103: a CROSSBAR's hub is a switch at the placement level, not an
     * organisation -- it fronts nothing of its own, so it gets no endpoint. */
    auto isHub = [&](int rid) { return rid >= 0 && rid < (int)t.gridDie.size() && t.gridDie[(size_t)rid] >= 0 && t.gridX[(size_t)rid] < 0; };
    for (auto& kv : info) {
        int rid = kv.first;
        int level = kv.second.first;
        int live = (int)kv.second.second.size();
        if (level == leafLevel && rid != 0 && live == 0 && !isHub(rid) &&
                t.peOfLeaf.find(rid) == t.peOfLeaf.end()) {
            int abst = nextEndpoint++;
            t.abstractOf[rid] = abst;
            t.coverageOf[abst] = 1;                   // the router's own organisation
            t.frontsLevel[abst] = level;
            t.extLinks.push_back({ abst, rid, layerW[0], layerLat[0] });
            continue;
        }
        if (level <= leafLevel) continue;             // leaf level: below = Ramulator
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
    if ((int)t.gridX.size() < t.numRouters) { t.gridX.resize((size_t)t.numRouters, -1); t.gridY.resize((size_t)t.numRouters, -1); t.gridDie.resize((size_t)t.numRouters, -1); }   // 1.11.103

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


inline void TreeWalker::build(const SparseHTree& t) {
    parentOf.assign((size_t)std::max(1, t.numRouters), -1);
    for (const auto& l : t.intLinks)
        if (l.b >= 0 && l.b < (int)parentOf.size()) parentOf[(size_t)l.b] = l.a;   // a = parent, b = child
    routerOfEp.assign((size_t)std::max(1, t.totalEndpoints()), -1);
    for (const auto& e : t.extLinks)
        if (e.a >= 0 && e.a < (int)routerOfEp.size()) routerOfEp[(size_t)e.a] = e.b;
    built = true;
}

/* 1.11.103: the router sequence src -> dst. On the tree: up to the LCA and
 * down. On a gridded die: the grid walk (dimension order on a MESH, the
 * shorter arc on a RING, node -> hub -> node on a CROSSBAR) between two nodes
 * of one die; for a cross-die pair the walk to the exit, the chip router, the
 * tree above, the far die's exit and its walk to the destination node. */
inline bool TreeWalker::path(const SparseHTree& t, int srcEp, int dstEp, std::vector<int>& routers) const {
    routers.clear();
    if (!built || srcEp < 0 || dstEp < 0 || srcEp >= (int)routerOfEp.size() || dstEp >= (int)routerOfEp.size()) return false;
    const int rs = routerOfEp[(size_t)srcEp], rd = routerOfEp[(size_t)dstEp];
    if (rs < 0 || rd < 0) return false;
    auto dieOf = [&](int r) -> int { return (r >= 0 && r < (int)t.gridDie.size()) ? t.gridDie[(size_t)r] : -1; };
    auto nodeAt = [&](int die, int x, int y) -> int {
        for (int r = 0; r < (int)t.gridDie.size(); ++r) if (t.gridDie[(size_t)r] == die && t.gridX[(size_t)r] == x && t.gridY[(size_t)r] == y) return r;
        return -1;
    };
    auto gridWalk = [&](int a, int b, std::vector<int>& out) {       // a and b: grid nodes of one die; out gets a..b inclusive
        const int die = t.gridDie[(size_t)a];
        out.push_back(a);
        if (a == b) return;
        if (t.fabricKind == 3) { auto h = t.dieExitOf.find(die); if (h != t.dieExitOf.end()) out.push_back(h->second); out.push_back(b); return; }
        if (t.fabricKind == 2) {
            const int n = t.gridW * t.gridH;
            const int ia = t.gridY[(size_t)a] * t.gridW + t.gridX[(size_t)a], ib = t.gridY[(size_t)b] * t.gridW + t.gridX[(size_t)b];
            const int fwd = (ib - ia + n) % n, bwd = (ia - ib + n) % n; const int step = (fwd <= bwd) ? 1 : -1; const int steps = std::min(fwd, bwd);
            int cur = ia; for (int k = 0; k < steps; ++k) { cur = (cur + step + n) % n; out.push_back(nodeAt(die, cur % t.gridW, cur / t.gridW)); }
            return;
        }
        int x = t.gridX[(size_t)a], y = t.gridY[(size_t)a];
        const int bx = t.gridX[(size_t)b], by = t.gridY[(size_t)b];
        while (x != bx) { x += (bx > x) ? 1 : -1; out.push_back(nodeAt(die, x, y)); }
        while (y != by) { y += (by > y) ? 1 : -1; out.push_back(nodeAt(die, x, y)); }
    };
    auto exitOf = [&](int die) -> int { auto e = t.dieExitOf.find(die); return (e == t.dieExitOf.end()) ? -1 : e->second; };
    if (rs == rd) { routers.push_back(rs); return true; }
    const int ds = dieOf(rs), dd = dieOf(rd);
    if (ds >= 0 && ds == dd) { gridWalk(rs, rd, routers); return true; }
    /* the source side up to its chip router (a die's walk to the exit, then the chip) */
    if (ds >= 0) {
        const int ex = exitOf(ds); if (ex < 0) return false;
        if (t.fabricKind == 3) { routers.push_back(rs); if (rs != ex) routers.push_back(ex); }
        else gridWalk(rs, ex, routers);
        routers.push_back(ds);
    } else routers.push_back(rs);
    const int topS = (ds >= 0) ? ds : rs, topD = (dd >= 0) ? dd : rd;
    std::vector<int> treeUp, treeDn;
    for (int r = topS; r >= 0 && treeUp.size() < 32; r = parentOf[(size_t)r]) treeUp.push_back(r);
    for (int r = topD; r >= 0 && treeDn.size() < 32; r = parentOf[(size_t)r]) treeDn.push_back(r);
    int ui = -1, di = -1;
    for (int i = 0; i < (int)treeUp.size() && ui < 0; ++i) for (int j = 0; j < (int)treeDn.size(); ++j) if (treeUp[(size_t)i] == treeDn[(size_t)j]) { ui = i; di = j; break; }
    if (ui < 0) return false;
    for (int i = 1; i <= ui; ++i) routers.push_back(treeUp[(size_t)i]);                           // climb to the LCA
    for (int i = di - 1; i >= 0; --i) routers.push_back(treeDn[(size_t)i]);                      // down to the far chip (or rd)
    if (dd >= 0) {
        const int ex = exitOf(dd); if (ex < 0) return false;
        if (t.fabricKind == 3) { if (ex != rd) routers.push_back(ex); routers.push_back(rd); }
        else { std::vector<int> w; gridWalk(ex, rd, w); for (auto r : w) routers.push_back(r); }
    }
    return true;
}
inline uint32_t TreeWalker::walk(const SparseHTree& t, int srcEp, int dstEp, uint32_t perLevel[8],
                                 const uint32_t* levelLat, const uint32_t* bridgeLat, uint64_t* cost) const {
    for (int l = 0; l < 8; l++) perLevel[l] = 0;
    if (cost) *cost = 0;
    std::vector<int> rs;
    if (!path(t, srcEp, dstEp, rs) || rs.empty()) return 0;
    auto lvlOf = [&](int r) -> int { return (r >= 0 && r < (int)t.levelOfRouter.size()) ? t.levelOfRouter[(size_t)r] : -1; };
    auto isDieNode = [&](int r) { return r >= 0 && r < (int)t.gridDie.size() && t.gridDie[(size_t)r] >= 0; };
    uint32_t links = 0;
    for (size_t i = 0; i < rs.size(); ++i) {
        const int r = rs[i];
        const int lv = lvlOf(r);
        if (lv >= 0 && lv <= 6) { perLevel[lv]++; if (cost && levelLat) *cost += levelLat[lv]; }
        if (i + 1 < rs.size()) {
            const int rn = rs[i + 1];
            const int la = lvlOf(r), lb = lvlOf(rn);
            /* 1.11.103: a die exit (grid node or hub <-> chip router) crosses the
             * tiers the grid replaced: charge their level latencies and bridges
             * as the tree would, so leaving a gridded die costs what leaving
             * the tree's die costs. */
            const bool exitLink = (isDieNode(r) != isDieNode(rn)) && (std::max(la, lb) >= 3);
            if (exitLink) {
                const int from = std::min(la, lb);
                for (int lv2 = from + 1; lv2 <= 2; ++lv2) { perLevel[lv2]++; if (cost && levelLat) *cost += levelLat[lv2]; if (cost && bridgeLat) *cost += bridgeLat[lv2 - 1]; links++; }
                if (cost && bridgeLat) *cost += bridgeLat[2];
            } else {
                const int lbm = std::min(la, lb);
                if (cost && bridgeLat && lbm >= 0 && lbm <= 5) *cost += bridgeLat[lbm];
            }
            links++;
        }
    }
    return links;
}
} // namespace pimid_htree

#endif // PIMID_SPARSE_HTREE_H_
