/** $glic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 * Copyright (C) 2011 Google Inc.
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

#include "init.h"
#include <algorithm>
#include <list>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <sys/time.h>
#include <vector>

// Use std namespace (needed with PIN_CRT to make std types visible)
using namespace std;
#include "cache.h"
#include "cache_arrays.h"
#include "config.h"
#include "constants.h"
#include "contention_sim.h"
#include "core.h"
#include "debug_zsim.h"
#include "pe_memory_interface.h"
#include "ramulator_mem_ctrl.h"
#include "event_queue.h"
#include "filter_cache.h"
#include "galloc.h"
#include "hash.h"
#include "ideal_arrays.h"
#include "locks.h"
#include "log.h"
#include "mem_ctrls.h"
#include "network.h"
#include "garnet_network.h"
#include "null_core.h"
#include "alu_core.h"
#include "ooo_core.h"
#include "part_repl_policies.h"
#include "prefetcher.h"
#include "proc_stats.h"
#include "process_stats.h"
#include "process_tree.h"
#include "profile_stats.h"
#include "repl_policies.h"
#include "scheduler.h"
#include "simple_core.h"
#include "stats.h"
#include "stats_filter.h"
#include "str.h"
#include "timing_cache.h"
#include "in_order_core.h"
#include "timing_event.h"
#include "trace_driver.h"
#include "tracing_cache.h"
#include "port_virtualizer.h"
#include "weave_md1_mem.h" //validation, could be taken out...
#include "zsim.h"

extern void EndOfPhaseActions(); //in qemu_zsim_plugin.cpp

/* zsim should be initialized in a deterministic and logical order, to avoid re-reading config vars
 * all over the place and give a predictable global state to constructors. Ideally, this should just
 * follow the layout of zinfo, top-down.
 */

/* 1.11.59 (audit F018): who owns a cache group, so the flush registry can be
 * role-aware instead of asserting in a comment that it does not need to be.
 *
 * The system-scope emitter names every cache group "<node.name>_l1d" /
 * "_l1i" / "_l2" / "_l3" and gates emission on CORE TYPE (alu_core and
 * null_core get none), never on ROLE -- so a node declared `role: DEVICE`
 * with an in_order/simple/ooo core is given a full L1I/L1D/L2 hierarchy.
 * Those caches landed in the same global registry as the host's, and the
 * host's coherence flush then charged their dirty lines to a HOST core in
 * cycles and to HOST DRAM in writeback bytes. Nothing warned: the registry
 * was a flat vector and the only stated guard was the sentence below.
 *
 * The node map is parsed earlier in InitSystem than the cache groups are
 * built, so the role is available here. Longest-name-first, because one node
 * name may be a prefix of another ("dev" and "dev0"). Returns -1 when there
 * is no node map at all (device-scope runs, where sys.coherence is never
 * emitted and the registry is never walked) or when no node claims the
 * group, and the caller then keeps the old register-everything behaviour. */
static int cacheGroupOwnerNode(const string& groupName) {
    if (!zinfo || zinfo->nodeMap.numNodes == 0) return -1;
    int best = -1;
    size_t bestLen = 0;
    for (uint32_t n = 0; n < zinfo->nodeMap.numNodes && n < 16; n++) {
        const string nodeName(zinfo->nodeMap.nodes[n].name);
        if (nodeName.empty()) continue;
        const string pfx = nodeName + "_";
        if (groupName.compare(0, pfx.size(), pfx) == 0 && pfx.size() > bestLen) {
            best = (int)n;
            bestLen = pfx.size();
        }
    }
    return best;
}

/* 1.11.59 (audit F018): banks excluded from the flush registry because their
 * owning node is a DEVICE, reported once the groups are built so the decision
 * is visible in the log rather than inferred from a count that silently got
 * smaller. */
static uint32_t g_flushRegistryDeviceBanks = 0;

BaseCache* BuildCacheBank(Config& config, const string& prefix, g_string& name, uint32_t bankSize, bool isTerminal, uint32_t domain) {
    string type = config.get<const char*>(prefix + "type", "Simple");
    // Shortcut for TraceDriven type
    if (type == "TraceDriven") {
        assert(zinfo->traceDriven);
        assert(isTerminal);
        return new TraceDriverProxyCache(name);
    }

    uint32_t lineSize = zinfo->lineSize;
    assert(lineSize > 0); //avoid config deps
    if (bankSize % lineSize != 0) panic("%s: Bank size must be a multiple of line size", name.c_str());

    uint32_t numLines = bankSize/lineSize;

    //Array
    uint32_t numHashes = 1;
    uint32_t ways = config.get<uint32_t>(prefix + "array.ways", 4);
    string arrayType = config.get<const char*>(prefix + "array.type", "SetAssoc");
    uint32_t candidates = (arrayType == "Z")? config.get<uint32_t>(prefix + "array.candidates", 16) : ways;

    //Need to know number of hash functions before instantiating array
    if (arrayType == "SetAssoc") {
        numHashes = 1;
    } else if (arrayType == "Z") {
        numHashes = ways;
        assert(ways > 1);
    } else if (arrayType == "IdealLRU" || arrayType == "IdealLRUPart") {
        ways = numLines;
        numHashes = 0;
    } else {
        panic("%s: Invalid array type %s", name.c_str(), arrayType.c_str());
    }

    // Power of two sets check; also compute setBits, will be useful later
    uint32_t numSets = numLines/ways;
    uint32_t setBits = 31 - __builtin_clz(numSets);
    if ((1u << setBits) != numSets) panic("%s: Number of sets must be a power of two (you specified %d sets)", name.c_str(), numSets);

    //Hash function
    HashFamily* hf = nullptr;
    string hashType = config.get<const char*>(prefix + "array.hash", (arrayType == "Z")? "H3" : "None"); //zcaches must be hashed by default
    if (numHashes) {
        if (hashType == "None") {
            if (arrayType == "Z") panic("ZCaches must be hashed!"); //double check for stupid user
            assert(numHashes == 1);
            hf = new IdHashFamily;
        } else if (hashType == "H3") {
            // Use portable FNV hash (zsim_fnv_hash replaces glibc's _Fnv_hash_bytes)
            size_t seed = zsim_fnv_hash(prefix.c_str(), prefix.size()+1, 0xB4AC5B);
            //info("%s -> %lx", prefix.c_str(), seed);
            hf = new H3HashFamily(numHashes, setBits, 0xCAC7EAFFA1 + seed /*make randSeed depend on prefix*/);
        } else if (hashType == "SHA1") {
            hf = new SHA1HashFamily(numHashes);
        } else {
            panic("%s: Invalid value %s on array.hash", name.c_str(), hashType.c_str());
        }
    }

    //Replacement policy
    string replType = config.get<const char*>(prefix + "repl.type", (arrayType == "IdealLRUPart")? "IdealLRUPart" : "LRU");
    ReplPolicy* rp = nullptr;

    if (replType == "LRU" || replType == "LRUNoSh") {
        bool sharersAware = (replType == "LRU") && !isTerminal;
        if (sharersAware) {
            rp = new LRUReplPolicy<true>(numLines);
        } else {
            rp = new LRUReplPolicy<false>(numLines);
        }
    } else if (replType == "LFU") {
        rp = new LFUReplPolicy(numLines);
    } else if (replType == "LRUProfViol") {
        ProfViolReplPolicy< LRUReplPolicy<true> >* pvrp = new ProfViolReplPolicy< LRUReplPolicy<true> >(numLines);
        pvrp->init(numLines);
        rp = pvrp;
    } else if (replType == "TreeLRU") {
        rp = new TreeLRUReplPolicy(numLines, candidates);
    } else if (replType == "NRU") {
        rp = new NRUReplPolicy(numLines, candidates);
    } else if (replType == "Rand") {
        rp = new RandReplPolicy(candidates);
    } else if (replType == "WayPart" || replType == "Vantage" || replType == "IdealLRUPart") {
        if (replType == "WayPart" && arrayType != "SetAssoc") panic("WayPart replacement requires SetAssoc array");

        //Partition mapper
        // TODO: One partition mapper per cache (not bank).
        string partMapper = config.get<const char*>(prefix + "repl.partMapper", "Core");
        PartMapper* pm = nullptr;
        if (partMapper == "Core") {
            pm = new CorePartMapper(zinfo->numCores); //NOTE: If the cache is not fully shared, trhis will be inefficient...
        } else if (partMapper == "InstrData") {
            pm = new InstrDataPartMapper();
        } else if (partMapper == "InstrDataCore") {
            pm = new InstrDataCorePartMapper(zinfo->numCores);
        } else if (partMapper == "Process") {
            pm = new ProcessPartMapper(zinfo->numProcs);
        } else if (partMapper == "InstrDataProcess") {
            pm = new InstrDataProcessPartMapper(zinfo->numProcs);
        } else if (partMapper == "ProcessGroup") {
            pm = new ProcessGroupPartMapper();
        } else {
            panic("Invalid repl.partMapper %s on %s", partMapper.c_str(), name.c_str());
        }

        // Partition monitor
        uint32_t umonLines = config.get<uint32_t>(prefix + "repl.umonLines", 256);
        uint32_t umonWays = config.get<uint32_t>(prefix + "repl.umonWays", ways);
        uint32_t buckets;
        if (replType == "WayPart") {
            buckets = ways; //not an option with WayPart
        } else { //Vantage or Ideal
            buckets = config.get<uint32_t>(prefix + "repl.buckets", 256);
        }

        PartitionMonitor* mon = new UMonMonitor(numLines, umonLines, umonWays, pm->getNumPartitions(), buckets);

        //Finally, instantiate the repl policy
        PartReplPolicy* prp;
        double allocPortion = 1.0;
        if (replType == "WayPart") {
            //if set, drives partitioner but doesn't actually do partitioning
            bool testMode = config.get<bool>(prefix + "repl.testMode", false);
            prp = new WayPartReplPolicy(mon, pm, numLines, ways, testMode);
        } else if (replType == "IdealLRUPart") {
            prp = new IdealLRUPartReplPolicy(mon, pm, numLines, buckets);
        } else { //Vantage
            uint32_t assoc = (arrayType == "Z")? candidates : ways;
            allocPortion = .85;
            bool smoothTransients = config.get<bool>(prefix + "repl.smoothTransients", false);
            prp = new VantageReplPolicy(mon, pm, numLines, assoc, (uint32_t)(allocPortion * 100), 10, 50, buckets, smoothTransients);
        }
        rp = prp;

        // Partitioner
        // TODO: Depending on partitioner type, we want one per bank or one per cache.
        Partitioner* p = new LookaheadPartitioner(prp, pm->getNumPartitions(), buckets, 1, allocPortion);

        //Schedule its tick
        uint32_t interval = config.get<uint32_t>(prefix + "repl.interval", 5000); //phases
        zinfo->eventQueue->insert(new Partitioner::PartitionEvent(p, interval));
    } else {
        panic("%s: Invalid replacement type %s", name.c_str(), replType.c_str());
    }
    assert(rp);


    //Alright, build the array
    CacheArray* array = nullptr;
    if (arrayType == "SetAssoc") {
        array = new SetAssocArray(numLines, ways, rp, hf);
    } else if (arrayType == "Z") {
        array = new ZArray(numLines, ways, candidates, rp, hf);
    } else if (arrayType == "IdealLRU") {
        assert(replType == "LRU");
        assert(!hf);
        IdealLRUArray* ila = new IdealLRUArray(numLines);
        rp = ila->getRP();
        array = ila;
    } else if (arrayType == "IdealLRUPart") {
        assert(!hf);
        // Use virtual method instead of dynamic_cast for -fno-rtti compatibility
        IdealLRUPartReplPolicy* irp = rp->asIdealLRUPartReplPolicy();
        if (!irp) panic("IdealLRUPart array needs IdealLRUPart repl policy!");
        array = new IdealLRUPartArray(numLines, irp);
    } else {
        panic("This should not happen, we already checked for it!"); //unless someone changed arrayStr...
    }

    //Latency
    uint32_t latency = config.get<uint32_t>(prefix + "latency", 10);
    uint32_t accLat = (isTerminal)? 0 : latency; //terminal caches has no access latency b/c it is assumed accLat is hidden by the pipeline
    uint32_t invLat = latency;

    // Inclusion?
    bool nonInclusiveHack = config.get<bool>(prefix + "nonInclusiveHack", false);
    if (nonInclusiveHack) assert(type == "Simple" && !isTerminal);

    // Finally, build the cache
    Cache* cache;
    CC* cc;
    if (isTerminal) {
        cc = new MESITerminalCC(numLines, name);
    } else {
        cc = new MESICC(numLines, nonInclusiveHack, name);
    }
    rp->setCC(cc);
    if (!isTerminal) {
        if (type == "Simple") {
            cache = new Cache(numLines, cc, array, rp, accLat, invLat, name);
        } else if (type == "Timing") {
            uint32_t mshrs = config.get<uint32_t>(prefix + "mshrs", 16);
            uint32_t tagLat = config.get<uint32_t>(prefix + "tagLat", 5);
            uint32_t timingCandidates = config.get<uint32_t>(prefix + "timingCandidates", candidates);
            cache = new TimingCache(numLines, cc, array, rp, accLat, invLat, mshrs, tagLat, ways, timingCandidates, domain, name);
        } else if (type == "Tracing") {
            g_string traceFile = config.get<const char*>(prefix + "traceFile","");
            if (traceFile.empty()) traceFile = g_string(zinfo->outputDir) + "/" + name + ".trace";
            cache = new TracingCache(numLines, cc, array, rp, accLat, invLat, traceFile, name);
        } else {
            panic("Invalid cache type %s", type.c_str());
        }
    } else {
        //Filter cache optimization
        if (type != "Simple") panic("Terminal cache %s can only have type == Simple", name.c_str());
        if (arrayType != "SetAssoc" || hashType != "None" || replType != "LRU") panic("Invalid FilterCache config %s", name.c_str());
        cache = new FilterCache(numSets, numLines, cc, array, rp, accLat, invLat, name);
    }

    /* 1.11.40 (audit N7): register every cache so the coherence flush can MEASURE
     * its footprint instead of using a constant. There was no global registry;
     * caches were only reachable through the local cMap during InitSystem.
     * Device PEs of type alu_core have no cache at all (see pe_memory_interface.h,
     * "The ALU has no cache"), so in a co-sim run every entry here is a host
     * cache -- which is what makes summing the whole registry the right answer
     * for a HOST flush.
     *
     * 1.11.59 (audit F018): that held only because the shipped co-sim gives
     * its device node an alu_core. The condition was never CHECKED -- the
     * push_back was unconditional and the sentence that followed it said a
     * cache-bearing PE type "must" make this role-aware, which is a comment,
     * not a guard. A device node with core_type in_order/simple/ooo gets a
     * full L1I/L1D/L2 from the same emitter (it gates on core type, not on
     * role), and every dirty line in it was then written back on a HOST core
     * and billed to HOST DRAM by the roi_begin flush -- silently, in exactly
     * the placement sweeps the flush charge exists to keep honest. The
     * registry is the HOST flush domain now, and a device-owned bank is
     * excluded and counted rather than summed. */
    {
        string groupName(prefix);
        const string kPfx = "sys.caches.";
        if (groupName.compare(0, kPfx.size(), kPfx) == 0) groupName.erase(0, kPfx.size());
        if (!groupName.empty() && groupName.back() == '.') groupName.pop_back();
        const int ownerNode = cacheGroupOwnerNode(groupName);
        const bool deviceOwned = (ownerNode >= 0) &&
                                 (zinfo->nodeMap.nodes[ownerNode].role != 0);
        if (deviceOwned) {
            g_flushRegistryDeviceBanks++;
        } else {
            zsimAllCaches().push_back(cache);
        }
    }

#if 0
    info("Built L%d bank, %d bytes, %d lines, %d ways (%d candidates if array is Z), %s array, %s hash, %s replacement, accLat %d, invLat %d name %s",
            level, bankSize, numLines, ways, candidates, arrayType.c_str(), hashType.c_str(), replType.c_str(), accLat, invLat, name.c_str());
#endif

    return cache;
}

MemObject* BuildMemoryController(Config& config, uint32_t lineSize, uint32_t frequency, uint32_t domain, g_string& name) {
    string type = config.get<const char*>("sys.mem.type", "Simple");
    uint32_t latency = config.get<uint32_t>("sys.mem.latency", 100);

    MemObject* mem = nullptr;
    if (type == "Simple") {
        uint32_t bandwidth = config.get<uint32_t>("sys.mem.bandwidth", 6400);
        mem = new SimpleMemory(lineSize, frequency, bandwidth, latency, name);
    } else if (type == "WeaveSimple") {
        uint32_t bandwidth = config.get<uint32_t>("sys.mem.bandwidth", 6400);
        uint32_t boundLatency = config.get<uint32_t>("sys.mem.boundLatency", latency);
        mem = new WeaveSimpleMemory(lineSize, frequency, bandwidth, latency, boundLatency, domain, name);
    } else if (type == "MD1" || type == "WeaveMD1") {
        panic("Memory controller type '%s' has been removed. Use 'Simple' (M/D/1 is always active) or 'WeaveSimple'.", type.c_str());
    } else if (type == "Ramulator") {
        string ramCfg = config.get<const char*>("sys.mem.configFile");
        uint64_t cpuFreqHz = 1000000ULL * frequency;
        mem = new RamulatorMemory(ramCfg, cpuFreqHz, latency, domain, name);
    } else {
        panic("Invalid memory controller type %s", type.c_str());
    }
    return mem;
}

typedef vector<vector<BaseCache*>> CacheGroup;

CacheGroup* BuildCacheGroup(Config& config, const string& name, bool isTerminal) {
    CacheGroup* cgp = new CacheGroup;
    CacheGroup& cg = *cgp;

    string prefix = "sys.caches." + name + ".";

    bool isPrefetcher = config.get<bool>(prefix + "isPrefetcher", false);
    if (isPrefetcher) { //build a prefetcher group
        uint32_t prefetchers = config.get<uint32_t>(prefix + "prefetchers", 1);
        cg.resize(prefetchers);
        for (vector<BaseCache*>& bg : cg) bg.resize(1);
        for (uint32_t i = 0; i < prefetchers; i++) {
            stringstream ss;
            ss << name << "-" << i;
            g_string pfName(ss.str().c_str());
            cg[i][0] = new StreamPrefetcher(pfName);
        }
        return cgp;
    }

    uint32_t size = config.get<uint32_t>(prefix + "size", 64*1024);
    uint32_t banks = config.get<uint32_t>(prefix + "banks", 1);
    uint32_t caches = config.get<uint32_t>(prefix + "caches", 1);

    uint32_t bankSize = size/banks;
    if (size % banks != 0) {
        panic("%s: banks (%d) does not divide the size (%d bytes)", name.c_str(), banks, size);
    }

    cg.resize(caches);
    for (vector<BaseCache*>& bg : cg) bg.resize(banks);

    for (uint32_t i = 0; i < caches; i++) {
        for (uint32_t j = 0; j < banks; j++) {
            stringstream ss;
            ss << name << "-" << i;
            if (banks > 1) {
                ss << "b" << j;
            }
            g_string bankName(ss.str().c_str());
            uint32_t domain = (i*banks + j)*zinfo->numDomains/(caches*banks); //(banks > 1)? nextDomain() : (i*banks + j)*zinfo->numDomains/(caches*banks);
            cg[i][j] = BuildCacheBank(config, prefix, bankName, bankSize, isTerminal, domain);
        }
    }

    return cgp;
}

static void InitSystem(Config& config) {
    unordered_map<string, string> parentMap; //child -> parent
    unordered_map<string, vector<vector<string>>> childMap; //parent -> children (a parent may have multiple children)

    auto parseChildren = [](string children) {
        // 1st dim: concatenated caches; 2nd dim: interleaved caches
        // Example: "l2-beefy l1i-wimpy|l1d-wimpy" produces [["l2-beefy"], ["l1i-wimpy", "l1d-wimpy"]]
        // If there are 2 of each cache, the final vector will be l2-beefy-0 l2-beefy-1 l1i-wimpy-0 l1d-wimpy-0 l1i-wimpy-1 l1d-wimpy-1
        vector<string> concatGroups = ParseList<string>(children);
        vector<vector<string>> cVec;
        for (string cg : concatGroups) cVec.push_back(ParseList<string>(cg, "|"));
        return cVec;
    };

    // Build network model: either Garnet mesh or simple file-based delays
    // GarnetNetwork inherits from Network, so we use a single Network* pointer
    Network* network = nullptr;
    zinfo->garnetNetwork = nullptr;  // Initialize to null

    string networkType = config.get<const char*>("sys.networkType", "");
    if (networkType == "garnet" || networkType == "mesh") {
        // Use Garnet-based network (multi-topology, simple or detailed)
        uint32_t meshRows = config.get<uint32_t>("sys.network.rows", 4);
        uint32_t meshCols = config.get<uint32_t>("sys.network.cols", 4);
        uint32_t routerLat = config.get<uint32_t>("sys.network.routerLatency", 1);
        uint32_t linkLat = config.get<uint32_t>("sys.network.linkLatency", 1);
        bool cycleAccurate = config.get<bool>("sys.network.cycleAccurate", false);
        double clockMhz = static_cast<double>(zinfo->freqMHz);

        // New multi-topology parameters
        string topoStr = config.get<const char*>("sys.network.topology", "MESH_2D");
        string routingStr = config.get<const char*>("sys.network.routing", "");
        uint32_t vcsPerVnet = config.get<uint32_t>("sys.network.vcsPerVnet", 4);
        uint32_t buffersPerVc = config.get<uint32_t>("sys.network.buffersPerVc", 4);
        string topoFile = config.get<const char*>("sys.network.topologyFile", "");
        string routingTableFile = config.get<const char*>("sys.network.routingTableFile", "");

        NoCTopology topo = parseNoCTopology(topoStr);
        NoCRouting routing = routingStr.empty() ? getDefaultRouting(topo) : parseNoCRouting(routingStr);

        uint32_t controlMsgBits = config.get<uint32_t>("sys.network.controlMsgBits", 0);
        uint32_t dataMsgBits = config.get<uint32_t>("sys.network.dataMsgBits", 0);
        bool ringUnidirectional = config.get<bool>("sys.network.ringUnidirectional", false);

        GarnetNetwork* garnet = new GarnetNetwork(
            topo, meshRows, meshCols, routerLat, linkLat,
            cycleAccurate, routing, vcsPerVnet, buffersPerVc,
            clockMhz, 128, topoFile, routingTableFile,
            controlMsgBits, dataMsgBits, ringUnidirectional);
        network = garnet;
        // Raise the gem5 deadlock-detector threshold. Under heavy halo-exchange
        // congestion (e.g. stencil MPI on high-latency DRAM like DDR5/HBM2), a VC
        // can stay busy past the default 500k-cycle threshold and trip a FALSE
        // "Possible network deadlock" panic -- packets are still being delivered,
        // the network is just saturated. processBatch is already bounded by its
        // own maxTick drain window, so this only suppresses the false positive.
        garnet->setDeadlockThreshold(100000000u);
        zinfo->garnetNetwork = garnet;  // Store for stats output
        info("[ZSim] Using Garnet %s network, routing=%s",
             topoStr.c_str(), nocRoutingStr(routing).c_str());

        // ── DIAGNOSTIC: synthetic injection-rate sweep on THIS (per-tech CUSTOM)
        // topology. Drives the network at controlled offered loads to expose the
        // latency-vs-load curve and the saturation knee (= the channel bandwidth)
        // -- which the workload batch-replay cannot reach (it self-regulates to
        // the unloaded rate). Env-gated; runs the sweep and exits (measurement).
        if (getenv("PIMID_SYNTH_SWEEP")) {
            int pat = getenv("PIMID_SYNTH_PATTERN") ?
                      atoi(getenv("PIMID_SYNTH_PATTERN")) : 0;   // 0=uniform random
            uint64_t npkts = getenv("PIMID_SYNTH_NPKTS") ?
                      (uint64_t)atoll(getenv("PIMID_SYNTH_NPKTS")) : 512;
            info("[SynthSweep] topology=%s pattern=%d npkts=%lu (rate avgLat throughput delivered)",
                 topoFile.empty() ? "(builtin)" : topoFile.c_str(), pat, (unsigned long)npkts);
            const double rates[] = {0.002,0.005,0.01,0.02,0.04,0.08,0.15,0.3,0.5};
            for (double r : rates) {
                auto res = garnet->runSyntheticTraffic(pat, r, npkts, 32);
                info("[SynthSweep] rate=%.4f avgLat=%.1f throughput=%.5f delivered=%lu",
                     r, res.avgLatency, res.throughput, (unsigned long)res.totalPackets);
            }
            info("[SynthSweep] done; exiting");
            exit(0);
        }
    } else {
        // Fall back to simple file-based network
        string networkFile = config.get<const char*>("sys.networkFile", "");
        network = (networkFile != "")? new Network(networkFile.c_str()) : nullptr;
    }

    // Read PIMID hierarchy config
    zinfo->hierarchy.enabled = config.exists("sys.hierarchy");
    if (zinfo->hierarchy.enabled) {
        zinfo->hierarchy.placementLevel = config.get<uint32_t>("sys.hierarchy.placementLevel", 1);
        zinfo->hierarchy.subarraysPerBank = config.get<uint32_t>("sys.hierarchy.subarraysPerBank", 4);
        zinfo->hierarchy.banksPerBG = config.get<uint32_t>("sys.hierarchy.banksPerBG", 4);
        zinfo->hierarchy.bgPerChip = config.get<uint32_t>("sys.hierarchy.bgPerChip", 4);
        zinfo->hierarchy.chipsPerRank = config.get<uint32_t>("sys.hierarchy.chipsPerRank", 8);
        zinfo->hierarchy.ranksPerChannel = config.get<uint32_t>("sys.hierarchy.ranksPerChannel", 1);
        zinfo->hierarchy.channelsPerSystem = config.get<uint32_t>("sys.hierarchy.channelsPerSystem", 1);
        zinfo->hierarchy.dramChannels = config.get<uint32_t>("sys.hierarchy.dramChannels", 1);
        zinfo->hierarchy.nocAggBandwidthMBs = config.get<uint64_t>("sys.hierarchy.nocAggBandwidthMBs", 0);
        zinfo->hierarchy.dqTurnNsX100 = config.get<uint32_t>("sys.hierarchy.dqTurnNsX100", 0);
        zinfo->hierarchy.fpEmulCycles = config.get<uint32_t>("sys.hierarchy.fpEmulCycles", 0);  // 1.11.11
        zinfo->hierarchy.peHasFpu = config.get<bool>("sys.hierarchy.peHasFpu", true);           // 1.11.11
        // Device clock used for bandwidth->cycle conversion (see zsim.h). 0 =>
        // use the global sys.frequency. In system-scope co-sim PIMID emits the
        // DEVICE node frequency here so device contention is not host-clocked.
        zinfo->hierarchy.nocBandwidthFreqMHz = config.get<uint32_t>("sys.hierarchy.nocBandwidthFreqMHz", 0);
        for (int i = 0; i < 7; i++) {
            char key[64]; snprintf(key, sizeof(key), "sys.hierarchy.levelLatency%d", i);
            zinfo->hierarchy.levelLatency[i] = config.get<uint32_t>(key, 0);
        }
        for (int i = 0; i < 6; i++) {
            char key[64]; snprintf(key, sizeof(key), "sys.hierarchy.bridgeLatency%d", i);
            zinfo->hierarchy.bridgeLatency[i] = config.get<uint32_t>(key, 0);
        }
        for (int i = 0; i < 6; i++) {
            char key[64]; snprintf(key, sizeof(key), "sys.hierarchy.bridgeModel%d", i);
            const char* bm = config.get<const char*>(key, "auto");
            // Map string to enum: 0=auto, 1=simple, 2=md1, 3=detailed
            if (strcmp(bm, "simple") == 0)        zinfo->hierarchy.bridgeModel[i] = 1;
            else if (strcmp(bm, "md1") == 0)      zinfo->hierarchy.bridgeModel[i] = 2;
            else if (strcmp(bm, "detailed") == 0) zinfo->hierarchy.bridgeModel[i] = 3;
            else                                  zinfo->hierarchy.bridgeModel[i] = 0;  // auto
        }
        // PE-MC distributed memory controller fields
        zinfo->hierarchy.totalUnits = config.get<uint32_t>("sys.hierarchy.totalUnits", 128);
        zinfo->hierarchy.pagesPerUnit = config.get<uint32_t>("sys.hierarchy.pagesPerUnit", 32);
        zinfo->hierarchy.dramRowBytes = config.get<uint32_t>("sys.hierarchy.dramRowBytes", 0);  // 1.11.52 (D003)
        zinfo->hierarchy.assumeLocal = config.get<uint32_t>("sys.hierarchy.assumeLocal", 0) != 0;
        zinfo->hierarchy.chargePrep = config.get<uint32_t>("sys.hierarchy.chargePrep", 0) != 0;
        zinfo->hierarchy.hostLinkXferCycles = config.get<uint32_t>("sys.hierarchy.hostLinkXferCycles", 0);
        zinfo->hierarchy.totalMCs = config.get<uint32_t>("sys.hierarchy.totalMCs", 0);
        zinfo->hierarchy.pesPerMC = config.get<uint32_t>("sys.hierarchy.pesPerMC", 1);
        zinfo->hierarchy.localLatency = config.get<uint32_t>("sys.hierarchy.localLatency", 10);
        zinfo->hierarchy.defaultBandwidthMBs = config.get<uint64_t>("sys.hierarchy.defaultBandwidthMBs", 0);

        // M:N PE-to-memory-org mapping
        zinfo->hierarchy.connectionMode = config.get<uint32_t>("sys.hierarchy.connectionMode", 0);
        zinfo->hierarchy.localLinkLatency = config.get<uint32_t>("sys.hierarchy.localLinkLatency", 0);
        zinfo->hierarchy.mcStandalone = config.get<uint32_t>("sys.hierarchy.mcStandalone", 0) != 0;
        zinfo->hierarchy.totalMemOrgs = config.get<uint32_t>("sys.hierarchy.totalMemOrgs", 0);
        zinfo->hierarchy.totalNetworkEndpoints = config.get<uint32_t>("sys.hierarchy.totalNetworkEndpoints", 0);
        zinfo->hierarchy.nocAvgOneWayLatency = config.get<uint32_t>("sys.hierarchy.nocAvgOneWayLatency", 0);
        zinfo->hierarchy.nocBisectionLinks = config.get<uint32_t>("sys.hierarchy.nocBisectionLinks", 1);
        zinfo->hierarchy.nocFlitsPerPacket = config.get<uint32_t>("sys.hierarchy.nocFlitsPerPacket", 5);
        zinfo->hierarchy.nocPerHopCycles = config.get<uint32_t>("sys.hierarchy.nocPerHopCycles", 2);
        zinfo->hierarchy.nocLinkWidthBits = config.get<uint32_t>("sys.hierarchy.nocLinkWidthBits", 128);
        zinfo->hierarchy.nocVcsPerVnet = config.get<uint32_t>("sys.hierarchy.nocVcsPerVnet", 4);
        zinfo->hierarchy.nocAvgHopsTimes100 = config.get<uint32_t>("sys.hierarchy.nocAvgHopsTimes100", 100);
        zinfo->hierarchy.nocNumNodes = config.get<uint32_t>("sys.hierarchy.nocNumNodes", 16);
        zinfo->hierarchy.nocTopologyClass = config.get<uint32_t>("sys.hierarchy.nocTopologyClass", 2);
        zinfo->hierarchy.nocTotalChannels = config.get<uint32_t>("sys.hierarchy.nocTotalChannels", 32);
        zinfo->hierarchy.nocHotspotFactor100 = config.get<uint32_t>("sys.hierarchy.nocHotspotFactor100", 100);

        // -- "calibrated" NoC model: zero-load injector probe ------------------
        // When noc.model=calibrated, measure the network's TRUE per-access
        // traversal latency L0 by injecting a trickle of synthetic traffic
        // (rate->0, fully drains) through a cycle-accurate copy of the runtime
        // topology. The simple remote path then uses L0 instead of the analytical
        // hop estimate. Gated on nocInjectorCalib, so simple/detailed are
        // untouched (nocCurveBaseLat stays 0).
        zinfo->hierarchy.nocInjectorCalib = config.get<uint32_t>("sys.hierarchy.nocInjectorCalib", 0);
        zinfo->hierarchy.nocCurveModel = config.get<uint32_t>("sys.hierarchy.nocCurveModel", 0);
        zinfo->hierarchy.nocCalQueue = config.get<uint32_t>("sys.hierarchy.nocCalQueue", 0);
        zinfo->hierarchy.nocMlpModel = config.get<uint32_t>("sys.hierarchy.nocMlpModel", 0);
        zinfo->hierarchy.nocMlpDegree = config.get<uint32_t>("sys.hierarchy.nocMlpDegree", 1);
#ifdef HAVE_GARNET
        if (zinfo->hierarchy.nocInjectorCalib &&
            !config.get<bool>("sys.network.cycleAccurate", false)) {
            string ptopo  = config.get<const char*>("sys.network.topology", "MESH_2D");
            string proute = config.get<const char*>("sys.network.routing", "");
            NoCTopology pt = parseNoCTopology(ptopo);
            NoCRouting  pr = proute.empty() ? getDefaultRouting(pt) : parseNoCRouting(proute);
            uint32_t prows = config.get<uint32_t>("sys.network.rows", 4);
            uint32_t pcols = config.get<uint32_t>("sys.network.cols", 4);
            uint32_t prl   = config.get<uint32_t>("sys.network.routerLatency", 1);
            uint32_t pll   = config.get<uint32_t>("sys.network.linkLatency", 1);
            uint32_t pvc   = config.get<uint32_t>("sys.network.vcsPerVnet", 4);
            uint32_t pbuf  = config.get<uint32_t>("sys.network.buffersPerVc", 4);
            // Topology-aware calibrated (Fix B): publish whether the runtime
            // topology is a grid (MESH/TORUS) so the calibrated runtime branch
            // can ADD the M/D/1 contention + memory terms for grids (where
            // detailed is HIGHER due to real contention) while leaving non-grid
            // (H-tree) untouched (stripped base, which already fits detailed-low).
            bool calIsGrid = (pt == NoCTopology::MESH_2D || pt == NoCTopology::TORUS_2D);
            zinfo->hierarchy.nocCurveIsGrid = calIsGrid ? 1u : 0u;
            try {
                GarnetNetwork probe(pt, prows, pcols, prl, pll, true, pr, pvc, pbuf,
                                    (double)zinfo->freqMHz, 128, "", "", 0, 0, false);
                probe.setDeadlockThreshold(100000000u);  // low rate can't deadlock; guard anyway
                auto res = probe.runSyntheticTraffic(0 /*uniform*/, 0.003, 64, 8);
                if (res.totalPackets > 0)
                    zinfo->hierarchy.nocCurveBaseLat = (uint32_t)(res.avgLatency + 0.5);
                info("[NoC calibrated] injector probe: L0=%u cyc "
                     "(%u/%u packets, %ux%u %s)",
                     zinfo->hierarchy.nocCurveBaseLat,
                     (uint32_t)res.totalPackets, 64u, prows, pcols, ptopo.c_str());
            } catch (const std::exception& e) {
                warn("[NoC calibrated] injector probe failed (%s); using analytical",
                     e.what());
                zinfo->hierarchy.nocCurveBaseLat = 0;
            }
        }

        // -- "curve" NoC model: latency-vs-load probe -------------------------
        // Mirror the calibrated probe but sweep SEVERAL increasing injection
        // rates to build a load->latency curve. At runtime we estimate the
        // offered per-node load and interpolate this curve, so contention is
        // captured analytically (no Garnet at runtime) yet load-dependent.
        if (zinfo->hierarchy.nocCurveModel &&
            !config.get<bool>("sys.network.cycleAccurate", false)) {
            string ptopo  = config.get<const char*>("sys.network.topology", "MESH_2D");
            string proute = config.get<const char*>("sys.network.routing", "");
            NoCTopology pt = parseNoCTopology(ptopo);
            NoCRouting  pr = proute.empty() ? getDefaultRouting(pt) : parseNoCRouting(proute);
            uint32_t prows = config.get<uint32_t>("sys.network.rows", 4);
            uint32_t pcols = config.get<uint32_t>("sys.network.cols", 4);
            uint32_t prl   = config.get<uint32_t>("sys.network.routerLatency", 1);
            uint32_t pll   = config.get<uint32_t>("sys.network.linkLatency", 1);
            uint32_t pvc   = config.get<uint32_t>("sys.network.vcsPerVnet", 4);
            uint32_t pbuf  = config.get<uint32_t>("sys.network.buffersPerVc", 4);

            // Non-grid topologies (H-tree, ring, bus, fat-tree, crossbar) have a
            // narrow bisection and saturate at far lower injection rates than a
            // mesh/torus; pushing them hard yields garbage/huge latency or can
            // hang the drain. Cap the max probed rate for those.
            bool isGrid = (pt == NoCTopology::MESH_2D || pt == NoCTopology::TORUS_2D);
            // Fix-1 support: publish whether this topology is a grid so the
            // runtime curve branch can decide whether to add the memory
            // remoteLat (grid: yes; non-grid: no -> avoid double-counting).
            zinfo->hierarchy.nocCurveIsGrid = isGrid ? 1u : 0u;
            // Fix-2: probe with a MEMORY-DIRECTED (hotspot) pattern instead of
            // uniform, so the curve reflects the workload's many-PE -> few-MC
            // contention. A hotspot saturates at FAR lower per-node injection
            // than uniform (all traffic funnels onto 1-2 destination nodes), so
            // use lower max rates; the bounding/clamping below still guards
            // against any exploded points.
            static const double kRatesGrid[6]    = {0.002, 0.01, 0.02, 0.04, 0.07, 0.12};
            static const double kRatesNonGrid[6] = {0.002, 0.006, 0.012, 0.02, 0.03, 0.05};
            const double* rates = isGrid ? kRatesGrid : kRatesNonGrid;
            const uint32_t nRates = 6;

            double L0 = 0.0;     // zero-load reference (first point) for sanity bounds
            uint32_t kept = 0;
            double prevLat = 0.0;
            try {
                for (uint32_t i = 0; i < nRates && kept < zinfo->hierarchy.NOC_CURVE_MAX; ++i) {
                    GarnetNetwork probe(pt, prows, pcols, prl, pll, true, pr, pvc, pbuf,
                                        (double)zinfo->freqMHz, 128, "", "", 0, 0, false);
                    probe.setDeadlockThreshold(100000000u);
                    // fewer packets at high rate to keep the drain bounded
                    uint64_t npkts = (i == 0) ? 64 : 256;
                    auto res = probe.runSyntheticTraffic(8 /*memory-directed*/, rates[i], npkts, 8);
                    if (res.totalPackets == 0) continue;
                    double lat = res.avgLatency;
                    if (i == 0) L0 = lat;
                    // Sanity: drop absurd / exploded points. A saturated H-tree
                    // can report >>10x L0; cap the curve at a sane multiple so the
                    // interpolation never returns garbage. Stop sweeping once the
                    // curve explodes (the remaining points would all be garbage).
                    double cap = (L0 > 0.0 ? 8.0 * L0 : 1e9);
                    if (lat > cap || lat <= 0.0 || lat > 1e6) {
                        info("[NoC curve] drop point rate=%.3f lat=%.1f (>cap %.1f); "
                             "stopping sweep (saturated)", rates[i], lat, cap);
                        break;
                    }
                    // Enforce non-decreasing latency (monotone-ish); a small dip
                    // is noise, clamp it up to the previous point.
                    if (kept > 0 && lat < prevLat) lat = prevLat;
                    zinfo->hierarchy.nocCurveRates[kept] = rates[i];
                    zinfo->hierarchy.nocCurveLat[kept]   = lat;
                    prevLat = lat;
                    kept++;
                }
                zinfo->hierarchy.nocCurveN = kept;
                // Also publish the zero-load point as nocCurveBaseLat for any
                // bootstrap before the curve is consulted at runtime.
                if (kept > 0)
                    zinfo->hierarchy.nocCurveBaseLat =
                        (uint32_t)(zinfo->hierarchy.nocCurveLat[0] + 0.5);
                info("[NoC curve] %u points, %ux%u %s (%s, memory-directed probe, "
                     "remoteLat %s at runtime):",
                     kept, prows, pcols, ptopo.c_str(), isGrid ? "grid" : "non-grid",
                     isGrid ? "KEPT" : "DROPPED");
                for (uint32_t i = 0; i < kept; ++i)
                    info("[NoC curve]   rate=%.3f -> lat=%.1f cyc",
                         zinfo->hierarchy.nocCurveRates[i],
                         zinfo->hierarchy.nocCurveLat[i]);
            } catch (const std::exception& e) {
                warn("[NoC curve] probe failed (%s); curve disabled, using analytical",
                     e.what());
                zinfo->hierarchy.nocCurveN = 0;
            }
        }
#endif

        // (NoC<->endpoint wiring check moved below, after the PE-mem map is
        // parsed, so it can test ACTIVE-PE collision rather than total endpoints.)

        // Parse flattened PE-mem mapping
        zinfo->hierarchy.peMemMapSize = config.get<uint32_t>("sys.hierarchy.peMemMapSize", 0);
        if (zinfo->hierarchy.peMemMapSize > MAX_THREADS) {
            warn("peMemMapSize %u exceeds MAX_THREADS (%d), clamping",
                 zinfo->hierarchy.peMemMapSize, MAX_THREADS);
            zinfo->hierarchy.peMemMapSize = MAX_THREADS;
        }
        if (zinfo->hierarchy.peMemMapSize > 0) {
            // Parse space-separated offset and data strings
            string offStr = config.get<const char*>("sys.hierarchy.peMemMapOffsets", "");
            string datStr = config.get<const char*>("sys.hierarchy.peMemMapData", "");

            // Parse offsets (need peMemMapSize + 1 entries for the sentinel)
            if (!offStr.empty()) {
                std::istringstream iss(offStr);
                uint32_t val;
                uint32_t idx = 0;
                uint32_t maxIdx = zinfo->hierarchy.peMemMapSize; // sentinel at [peMemMapSize]
                while (iss >> val && idx <= maxIdx) {
                    zinfo->hierarchy.peMemMapOffsets[idx++] = val;
                }
            }
            // Parse data
            if (!datStr.empty()) {
                std::istringstream iss(datStr);
                uint32_t val;
                uint32_t idx = 0;
                while (iss >> val && idx < 4096) {
                    zinfo->hierarchy.peMemMapData[idx++] = val;
                }
            }
        }

        // PCIe/CXL/NVLink timing
        zinfo->pcie.enabled = config.get<uint32_t>("sys.hierarchy.pcieEnabled", 0) != 0;
        zinfo->pcie.baseLatencyCycles = config.get<uint32_t>("sys.hierarchy.pcieBaseLatencyCycles", 0);
        zinfo->pcie.model = config.get<uint32_t>("sys.hierarchy.pcieModel", 0);
        zinfo->pcie.headerBytes = config.get<uint32_t>("sys.hierarchy.pcieHeaderBytes", 20);
        zinfo->pcie.coherenceExtraCycles = config.get<uint32_t>("sys.hierarchy.pcieCoherenceExtraCycles", 0);
        futex_init(&zinfo->pcie.updateLock);
        // pcieBytesPerCycle is a double but ZSim config only supports string → parse
        {
            const char* bpcStr = config.get<const char*>("sys.hierarchy.pcieBytesPerCycle", "0.0");
            zinfo->pcie.bytesPerCycle = atof(bpcStr);
        }

        info("[ZSim] DRAM hierarchy enabled (placement=L%d, 7 levels, PEMCs=%u, mapping=%u PEs, conn=%s, endpoints=%u)",
             zinfo->hierarchy.placementLevel, zinfo->hierarchy.totalMCs,
             zinfo->hierarchy.peMemMapSize,
             (zinfo->hierarchy.connectionMode == 0) ? "shared_io" : "separate_endpoints",
             zinfo->hierarchy.totalNetworkEndpoints);

        // -- NoC <-> endpoint wiring check ------------------------------------
        // Endpoints (PEs + EVERY mem org + caches) map onto Garnet nodes by
        // (unit % numNodes). For DRAM the node count is the fixed datapath
        // abstraction (the channel-DQ chokepoint), so at fine placement the many
        // PASSIVE mem-org endpoints (e.g. all subarrays) ALIAS onto it BY DESIGN
        // -- that is the intended model, not a mis-wire. A real collision only
        // occurs when the ACTIVE injectors (the PEs that actually drive traffic)
        // outnumber the nodes; warn only then. (Was: warned on total endpoints,
        // which falsely flagged every fine-placement run.)
        if (zinfo->garnetNetwork) {
            uint32_t nNodes  = zinfo->garnetNetwork->getNumNodes();
            uint32_t nEnd    = zinfo->hierarchy.totalNetworkEndpoints;
            uint32_t nActive = zinfo->hierarchy.peMemMapSize;   // PEs = traffic injectors
            if (nNodes > 0 && nActive > nNodes) {
                warn("[NoC wiring] %u ACTIVE PEs exceed %u network nodes -- active "
                     "endpoints will alias; raise the NoC node budget", nActive, nNodes);
            } else if (nNodes > 0 && nEnd > nNodes) {
                info("[NoC wiring] %u endpoints over %u nodes (OK: %u active PEs; "
                     "surplus mem-org endpoints alias onto the datapath nodes by "
                     "design)", nEnd, nNodes, nActive);
            } else if (nEnd > 0) {
                info("[NoC wiring] %u endpoints over %u network nodes (OK)",
                     nEnd, nNodes);
            }
        }
    }

    // Host<->device TWO-LAYER BRIDGE (1.7.1). Standalone top-level block so it
    // is read whether or not a DRAM hierarchy block is present. When enabled it
    // supersedes the flat pcie charge at WORK_BEGIN/END (see qemu_zsim_plugin
    // boundaryTransferCycles). Cycle-converted at the host/reference clock.
    if (config.exists("sys.bridge")) {
        zinfo->bridge.enabled = config.get<uint32_t>("sys.bridge.enabled", 0) != 0;
        zinfo->bridge.phyLatencyCycles = config.get<uint32_t>("sys.bridge.phyLatencyCycles", 0);
        zinfo->bridge.protocolOverheadCycles = config.get<uint32_t>("sys.bridge.protocolOverheadCycles", 0);
        zinfo->bridge.channels = config.get<uint32_t>("sys.bridge.channels", 1);
        zinfo->bridge.uncachedCycles = config.get<uint32_t>("sys.bridge.uncachedCycles", 0);
        const char* bpc = config.get<const char*>("sys.bridge.bytesPerCycle", "0.0");
        zinfo->bridge.bytesPerCycle = atof(bpc);
        info("[ZSim] Host<->device bridge %s (phy=%u cyc + proto=%u cyc, %.4f B/cyc, c=%u, uncached=%u cyc)",
             zinfo->bridge.enabled ? "enabled" : "present-disabled",
             zinfo->bridge.phyLatencyCycles, zinfo->bridge.protocolOverheadCycles,
             zinfo->bridge.bytesPerCycle, zinfo->bridge.channels, zinfo->bridge.uncachedCycles);
    }

    // Case-1 COHERENCE flush accounting (1.7.2). Standalone top-level block,
    // read whether or not a DRAM hierarchy is present. Charged on the host core
    // at roi_begin in system-scope co-sim (see qemu_zsim_plugin coherenceFlushCycles).
    if (config.exists("sys.coherence")) {
        zinfo->coherence.enabled = config.get<uint32_t>("sys.coherence.enabled", 0) != 0;
        zinfo->coherence.mode = config.get<uint32_t>("sys.coherence.mode", 0);
        // footprintBytes is 64-bit; ZSim config get<> lacks uint64_t, so parse
        // the string form the emitter writes.
        {
            const char* fb = config.get<const char*>("sys.coherence.footprintBytes", "0");
            const uint64_t configuredFootprint = strtoull(fb, nullptr, 10);
            /* 1.11.40 (N7): footprintBytes is read only so an explicitly
             * configured value can be REFUSED rather than silently ignored --
             * the flush size is measured from the caches' dirty lines at each
             * flush and is a different number at each one.
             *
             * 1.11.57 (latent F021): the refusal is an ABORT, and the emitter
             * still advertises the key as a working override. src/main.cpp
             * parses sys.coherence.footprint_bytes, writes footprintBytes
             * into the config, and prints an "(OVERRIDE)" branch describing
             * the value as honoured -- a branch this panic guarantees no run
             * can ever reach.
             *
             * 1.11.59 (audit F021, fork half): the value is now a LOCAL. It
             * used to be stored in zinfo->coherence.footprintBytes, where it
             * was read by nothing except this panic and the startup line
             * below, which printed it as "footprint=%llu B" -- always 0, and
             * indistinguishable from a genuine report of the quantity the
             * flush actually uses. Keeping a config field alive next to the
             * measurement it was replaced by is how a reader (or a later
             * edit) comes to believe the flush still has a configured size.
             * The refusal is unchanged and deliberately loud. The other half
             * is NOT fixable here: src/main.cpp still emits the key and still
             * prints an "(OVERRIDE)" branch for a value that only ever aborts
             * the run it claims to configure. That belongs to whoever owns
             * src/, and is reported rather than reached across the boundary. */
            if (configuredFootprint > 0) {
                panic("sys.coherence.footprintBytes=%llu is set, but the flush "
                      "footprint is MEASURED from host dirty lines at each "
                      "flush -- it is not one number. Remove the key.",
                      (unsigned long long)configuredFootprint);
            }
        }
        zinfo->coherence.flushFixedCycles = config.get<uint32_t>("sys.coherence.flushFixedCycles", 0);
        const char* wbpc = config.get<const char*>("sys.coherence.writebackBytesPerCycle", "0.0");
        zinfo->coherence.writebackBytesPerCycle = atof(wbpc);
        /* 1.11.59 (audit F021): this line used to print "footprint=%llu B"
         * from the refused config field, which could only ever be 0 and read
         * as a report of the flush size. The flush size is measured per
         * flush and printed there; what belongs here is that fact. */
        info("[ZSim] Case-1 coherence flush %s (mode=%s, footprint=measured per "
             "flush from host dirty lines, %.4f B/cyc writeback, fixed=%u cyc)",
             zinfo->coherence.enabled ? "enabled" : "present-disabled",
             zinfo->coherence.mode == 0 ? "unified" : "separate",
             zinfo->coherence.writebackBytesPerCycle, zinfo->coherence.flushFixedCycles);
    }

    // Kernel LAUNCH cost tree (1.7.3, HANDOFF ISSUE 2). Standalone top-level
    // block, read whether or not a DRAM hierarchy is present. Charged on the host
    // core at the offload doorbell (co-sim roi_begin / WORK_BEGIN) in system-scope
    // co-sim (see qemu_zsim_plugin launchCostCycles). Cycle-converted at emission
    // to the host/reference clock; cmd/ack bytes cross the two-layer bridge.
    if (config.exists("sys.launch")) {
        zinfo->launch.enabled = config.get<uint32_t>("sys.launch.enabled", 0) != 0;
        zinfo->launch.doorbellCycles = config.get<uint32_t>("sys.launch.doorbellCycles", 0);
        zinfo->launch.dispatchCycles = config.get<uint32_t>("sys.launch.dispatchCycles", 0);
        zinfo->launch.cmdBytes = config.get<uint32_t>("sys.launch.cmdBytes", 64);
        zinfo->launch.ackBytes = config.get<uint32_t>("sys.launch.ackBytes", 64);
        info("[ZSim] Kernel launch cost %s (doorbell=%u cyc + dispatch=%u cyc + bridge(cmd=%u B, ack=%u B))",
             zinfo->launch.enabled ? "enabled" : "present-disabled",
             zinfo->launch.doorbellCycles, zinfo->launch.dispatchCycles,
             zinfo->launch.cmdBytes, zinfo->launch.ackBytes);
    }

    // Read node map (multi-host/multi-device system mode)
    if (config.exists("sys.nodeMap")) {
        zinfo->nodeMap.numNodes = config.get<uint32_t>("sys.nodeMap.numNodes", 0);
        for (uint32_t n = 0; n < zinfo->nodeMap.numNodes && n < 16; n++) {
            char base[64];
            snprintf(base, sizeof(base), "sys.nodeMap.node%u", n);
            string nodePrefix(base);

            const char* name = config.get<const char*>((nodePrefix + ".name").c_str(), "");
            strncpy(zinfo->nodeMap.nodes[n].name, name, 63);
            zinfo->nodeMap.nodes[n].name[63] = '\0';
            zinfo->nodeMap.nodes[n].role = config.get<uint32_t>((nodePrefix + ".role").c_str(), 0);
            zinfo->nodeMap.nodes[n].coreStart = config.get<uint32_t>((nodePrefix + ".coreStart").c_str(), 0);
            zinfo->nodeMap.nodes[n].coreEnd = config.get<uint32_t>((nodePrefix + ".coreEnd").c_str(), 0);
            zinfo->nodeMap.nodes[n].freqMHz = config.get<uint32_t>((nodePrefix + ".freqMHz").c_str(), 0);
            zinfo->nodeMap.nodes[n].networkNodeId = config.get<uint32_t>((nodePrefix + ".networkNodeId").c_str(), n);
            zinfo->nodeMap.nodes[n].addrStart = config.get<uint64_t>((nodePrefix + ".addrStart").c_str(), 0);
            zinfo->nodeMap.nodes[n].addrEnd = config.get<uint64_t>((nodePrefix + ".addrEnd").c_str(), 0);
        }

        // Build core-to-node mapping
        for (uint32_t n = 0; n < zinfo->nodeMap.numNodes && n < 16; n++) {
            for (uint32_t c = zinfo->nodeMap.nodes[n].coreStart;
                 c < zinfo->nodeMap.nodes[n].coreEnd && c < 2048; c++) {
                zinfo->systemNetwork.coreToNode[c] = zinfo->nodeMap.nodes[n].networkNodeId;
            }
        }

        info("[ZSim] Node map: %u nodes", zinfo->nodeMap.numNodes);
    }

    // Read system network config
    if (config.exists("sys.systemNetwork")) {
        zinfo->systemNetwork.enabled = config.get<uint32_t>("sys.systemNetwork.enabled", 0) != 0;
        zinfo->systemNetwork.numNodes = config.get<uint32_t>("sys.systemNetwork.numNodes", 0);
        zinfo->systemNetwork.model = config.get<uint32_t>("sys.systemNetwork.model", 0);
        if (zinfo->systemNetwork.model == 1) zinfo->systemNetwork.model = 0;  // backward compat: MD1->SIMPLE
        zinfo->systemNetwork.linkWidthBits = config.get<uint32_t>("sys.systemNetwork.linkWidthBits", 512);

        // Read pre-computed inter-node latency matrix
        uint32_t nn = zinfo->systemNetwork.numNodes;
        for (uint32_t i = 0; i < nn && i < 16; i++) {
            for (uint32_t j = 0; j < nn && j < 16; j++) {
                if (i == j) continue;
                char key[96];
                snprintf(key, sizeof(key), "sys.systemNetwork.linkLatency_%u_%u", i, j);
                zinfo->systemNetwork.linkLatency[i * nn + j] =
                    config.get<uint32_t>(key, 0);
            }
        }

        info("[ZSim] System network: %u nodes, model=%u, enabled=%d",
             zinfo->systemNetwork.numNodes, zinfo->systemNetwork.model,
             zinfo->systemNetwork.enabled);
    }

    // Build the caches
    vector<const char*> cacheGroupNames;
    config.subgroups("sys.caches", cacheGroupNames);
    for (size_t i = 0; i < cacheGroupNames.size(); i++) {
    }
    string prefix = "sys.caches.";

    for (const char* grp : cacheGroupNames) {
        string group(grp);
        if (group == "mem") panic("'mem' is an invalid cache group name");
        if (childMap.count(group)) panic("Duplicate cache group %s", (prefix + group).c_str());

        string children = config.get<const char*>(prefix + group + ".children", "");
        childMap[group] = parseChildren(children);
        for (auto v : childMap[group]) for (auto child : v) {
            if (parentMap.count(child)) {
                panic("Cache group %s can have only one parent (%s and %s found)", child.c_str(), parentMap[child].c_str(), grp);
            }
            parentMap[child] = group;
        }
    }

    // Check that children are valid (another cache)
    for (auto& it : parentMap) {
        bool found = false;
        for (auto& grp : cacheGroupNames) found |= it.first == grp;
        if (!found) panic("%s has invalid child %s", it.second.c_str(), it.first.c_str());
    }

    // Get the LLC(s) — skip when no caches (e.g. ALU-only config)
    // Multiple parentless cache groups are allowed (LLC=0 / no-unified-LLC mode):
    // each top-level group connects independently to memory with no global coherence.
    bool hasCaches = !cacheGroupNames.empty();
    vector<string> llcGroups;  // one or more top-level cache groups
    string llc;  // kept for single-LLC backward compat
    if (hasCaches) {
        for (auto& it : childMap) if (!parentMap.count(it.first)) llcGroups.push_back(it.first);
        if (llcGroups.empty()) panic("Cache groups defined but none are parentless (cycle?)");
        llc = llcGroups[0];  // primary LLC (used by single-LLC code paths)
        if (llcGroups.size() > 1) {
            info("Multi-LLC mode: %ld independent top-level cache groups (no global coherence)", llcGroups.size());
        }
    }

    auto isTerminal = [&](string group) -> bool {
        return childMap[group].size() == 0;
    };

    // Build each of the groups, starting with the LLC(s) (skip for cacheless configs)
    unordered_map<string, CacheGroup*> cMap;
    if (hasCaches) {
    list<string> fringe;  // FIFO — seed with all top-level groups
    for (const string& grp : llcGroups) fringe.push_back(grp);
    while (!fringe.empty()) {
        string group = fringe.front();
        fringe.pop_front();
        if (cMap.count(group)) panic("The cache 'tree' has a loop at %s", group.c_str());
        cMap[group] = BuildCacheGroup(config, group, isTerminal(group));
        for (auto& childVec : childMap[group]) fringe.insert(fringe.end(), childVec.begin(), childVec.end());
    }

    // LLC constraints relaxed: top-level cache groups may have caches > 1
    // (no-unified-LLC / clustered-LLC mode). Each instance connects to memory
    // independently. No global coherence across instances.
    for (const string& grp : llcGroups) {
        if (cMap[grp]->size() > 1) {
            info("Top-level cache group %s has %ld instances (no-unified-LLC mode)", grp.c_str(), cMap[grp]->size());
        }
    }

    /* Since we have checked for no loops, parent is mandatory, and all parents are checked valid,
     * it follows that we have a fully connected tree finishing at the LLC.
     */
    /* 1.11.59 (audit F018): say what the flush domain ended up containing.
     * The flush log already prints the registry size; without this line a
     * reader could not tell a smaller registry from a config that simply has
     * fewer caches, which is how the un-checked assumption stayed invisible
     * in the first place. */
    info("[ZSim] Coherence-flush cache registry: %zu host-owned bank(s), "
         "%u device-owned bank(s) excluded (a HOST flush writes back HOST "
         "dirty lines; a device cache is not in the host's flush domain)",
         zsimAllCaches().size(), g_flushRegistryDeviceBanks);
    } // hasCaches

    //Build the memory controllers
    //If PE-MCs are configured (totalMCs > 0), create distributed PE-MCs.
    //Standalone device: they ARE the memory system (mems). Co-sim (a host is
    //present, marked by sys.nodeMap): the SAME PE-MIs serve the device PEs
    //via their mi_ pointers, and the host keeps its own memory controllers --
    //the device is the device, the host is the host.
    g_vector<MemObject*> mems;
    g_vector<PEMemoryInterface*> rawMIs;  // saved before SplitAddrMemory wrapping, for ALU core wiring
    bool cosimMode = config.exists("sys.nodeMap");

    if (zinfo->hierarchy.enabled && zinfo->hierarchy.totalMCs > 0) {
        uint32_t mcCount = zinfo->hierarchy.totalMCs;
        uint32_t totalUnits = zinfo->hierarchy.totalUnits;
        uint32_t linkLat = zinfo->hierarchy.localLinkLatency;
        // Clock used to convert each PE-MI's bandwidth (bytes/s) into bytes/cycle
        // for its M/D/1 service rate. Must be the DEVICE clock, not the global
        // sys.frequency (= host in system-scope co-sim), or the device's memory
        // contention scales with the host clock. 0 => freqMHz already is the
        // device clock (standalone device scope).
        uint32_t peMiFreqMHz = (zinfo->hierarchy.nocBandwidthFreqMHz > 0)
                               ? zinfo->hierarchy.nocBandwidthFreqMHz
                               : zinfo->freqMHz;

        // Build per-MI coverage sets from the mapping table
        // Each MI covers a group of PEs; the coverage = union of all mapped mem orgs
        bool hasMapping = (zinfo->hierarchy.peMemMapSize > 0);
        uint32_t pesPerMC = zinfo->hierarchy.pesPerMC;

        mems.resize(mcCount);
        for (uint32_t i = 0; i < mcCount; i++) {
            stringstream ss;
            ss << "pe-mi-" << i;
            g_string mcName(ss.str().c_str());

            // Read per-group bandwidth and latency overrides
            char bwKey[96], latKey[96];
            snprintf(bwKey, sizeof(bwKey), "sys.hierarchy.mcGroup%u.bandwidthMBs", i);
            snprintf(latKey, sizeof(latKey), "sys.hierarchy.mcGroup%u.localLatency", i);
            uint64_t bw = config.get<uint64_t>(bwKey, zinfo->hierarchy.defaultBandwidthMBs);
            uint32_t lat = config.get<uint32_t>(latKey, zinfo->hierarchy.localLatency);
            // Per-level near-data bandwidth gradient (ISPASS placement): a finer
            // placement sits on a wider internal datapath (subarray row buffer >>
            // bank >> channel DQ). Scale the local MI bandwidth by placement level
            // so a subarray-local access is the widest/fastest path, overcoming
            // its extra hops. subarray=4x, bank/bank-group=2x, coarser=1x.
            {
                uint32_t lvl = zinfo->hierarchy.placementLevel;
                uint32_t bwmul = (lvl == 0) ? 4u : ((lvl == 1 || lvl == 2) ? 2u : 1u);
                bw *= bwmul;
            }

            if (hasMapping) {
                // MI-i owns the CONTIGUOUS org slice of the PEs assigned to it.
                // PEs group into MIs by INDEX (MI-i serves PEs [i*pesPerMc,
                // (i+1)*pesPerMc)); the map stores only each PE's distant HOME org
                // (slice start), which we expand to the full [home, home+orgsPerPe)
                // slice -- storing every org would overflow peMemMapData[4096] at
                // fine placement, and this range form avoids it. (The old code
                // compared a PE's home-org VALUE to the MI index i, so with distant
                // placement every MI but #0 got EMPTY coverage -> all-remote; and it
                // only added the single home org, so coverage was 1 unit -> any
                // working set > one unit spilled remote.)
                uint32_t numPEs = zinfo->hierarchy.peMemMapSize;
                uint32_t pesPerMc = (mcCount > 0) ? (numPEs / mcCount) : 1;
                if (pesPerMc < 1) pesPerMc = 1;
                uint32_t orgsPerPe = (numPEs > 0) ? (totalUnits / numPEs) : totalUnits;
                if (orgsPerPe < 1) orgsPerPe = 1;
                uint32_t firstPe = i * pesPerMc;
                uint32_t covStart = 0, covEnd = totalUnits;
                if (firstPe < numPEs) {
                    uint32_t off0 = zinfo->hierarchy.peMemMapOffsets[firstPe];
                    if (off0 > 4095) off0 = 4095;
                    covStart = zinfo->hierarchy.peMemMapData[off0];   // firstPe's home org
                    covEnd = covStart + pesPerMc * orgsPerPe;
                    if (covEnd > totalUnits) covEnd = totalUnits;
                }
                mems[i] = new PEMemoryInterface(
                    i, covStart, covEnd, totalUnits, lat,
                    linkLat, bw, zinfo->lineSize, peMiFreqMHz, mcName,
                    zinfo->hierarchy.dramChannels);
            } else {
                // Legacy contiguous coverage
                uint32_t coveragePerMC = totalUnits / mcCount;
                if (coveragePerMC == 0) coveragePerMC = 1;
                uint32_t start = i * coveragePerMC;
                uint32_t end = (i == mcCount - 1) ? totalUnits : start + coveragePerMC;

                mems[i] = new PEMemoryInterface(
                    i, start, end, totalUnits, lat,
                    linkLat, bw, zinfo->lineSize, peMiFreqMHz, mcName,
                    zinfo->hierarchy.dramChannels);
            }
        }
        info("[ZSim] Created %u PE-MIs (%u PEs/MI, conn=%s)",
             mcCount, pesPerMC,
             (zinfo->hierarchy.connectionMode == 0) ? "shared_io" : "separate_endpoints");

        // Save raw PEMemoryInterface pointers before SplitAddrMemory wrapping
        rawMIs.resize(mcCount);
        for (uint32_t i = 0; i < mcCount; i++)
            rawMIs[i] = static_cast<PEMemoryInterface*>(mems[i]);

        if (cosimMode) {
            // Co-sim: the PE-MIs serve the device PEs directly (mi_ wiring
            // below); the cache hierarchy's memory side belongs to the HOST,
            // built in the host branch that follows.
            mems.clear();
            info("[ZSim] Co-sim: %u PE-MIs serve the device; host keeps its own MCs", mcCount);
        } else if (mcCount > 1) {
            // Use SplitAddrMemory if multiple PE-MIs to fan out addresses
            MemObject* splitter = new SplitAddrMemory(mems, "pe-mi-splitter");
            mems.resize(1);
            mems[0] = splitter;
        }
    }

    if (mems.empty()) {
        // Host memory controllers (standalone host-MC mode, or the host side
        // of a co-sim). Skip if SystemRouter — that path builds per-device
        // MCs below.
        string memType = config.get<const char*>("sys.mem.type", "");
        if (memType != "SystemRouter") {
            uint32_t memControllers = config.get<uint32_t>("sys.mem.controllers", 1);
            assert(memControllers > 0);

            mems.resize(memControllers);
            for (uint32_t i = 0; i < memControllers; i++) {
                stringstream ss;
                ss << "mem-" << i;
                g_string name(ss.str().c_str());
                uint32_t domain = i*zinfo->numDomains/memControllers;
                mems[i] = BuildMemoryController(config, zinfo->lineSize, zinfo->freqMHz, domain, name);
            }

            if (memControllers > 1) {
                bool splitAddrs = config.get<bool>("sys.mem.splitAddrs", true);
                if (splitAddrs) {
                    MemObject* splitter = new SplitAddrMemory(mems, "mem-splitter");
                    mems.resize(1);
                    mems[0] = splitter;
                }
            }

            // Host crossbar fabric hop (analytic, multi-core host). A 1-core host
            // emits enabled=0 (no fabric). When enabled, add a fixed one-hop
            // latency on the host memory path; port contention is already priced
            // by the host MC M/D/1 above. Wraps the (single) host MC / splitter.
            if (config.get<uint32_t>("sys.hostNetwork.enabled", 0) != 0 &&
                mems.size() == 1) {
                uint32_t hop = config.get<uint32_t>("sys.hostNetwork.hopCycles", 0);
                if (hop > 0) {
                    g_string hopName("host-xbar");
                    mems[0] = new CrossbarHopMemory(mems[0], hop, hopName.c_str());
                    info("[ZSim] Host crossbar fabric: +%u-cycle one-hop on host memory path", hop);
                }
            }
        }
    }

    // Build SystemAddressRouter for multi-device system mode
    string memType2 = config.get<const char*>("sys.mem.type", "");
    if (memType2 == "SystemRouter" && config.exists("sys.nodeMap")) {
        uint32_t numDevices = config.get<uint32_t>("sys.mem.numDevices", 0);
        if (numDevices > 0) {
            g_vector<SystemAddressRouter::DeviceEntry> devices;
            devices.resize(numDevices);

            // Build per-device MCs
            for (uint32_t d = 0; d < numDevices; d++) {
                char devKey[96];
                snprintf(devKey, sizeof(devKey), "sys.mem.device%u", d);
                string devPrefix(devKey);

                uint64_t addrStart = config.get<uint64_t>((devPrefix + ".addrStart").c_str(), 0);
                uint64_t addrEnd = config.get<uint64_t>((devPrefix + ".addrEnd").c_str(), 0);
                string devType = config.get<const char*>((devPrefix + ".type").c_str(), "Simple");
                uint32_t devLat = config.get<uint32_t>((devPrefix + ".latency").c_str(), 100);
                uint32_t netNodeId = config.get<uint32_t>((devPrefix + ".networkNodeId").c_str(), d);

                stringstream ss;
                ss << "sys-mc-" << d;
                g_string mcName(ss.str().c_str());

                MemObject* mc;
                if (devType == "Ramulator") {
                    string ramCfg = config.get<const char*>((devPrefix + ".configFile").c_str(), "");
                    if (ramCfg.empty()) {
                        panic("SystemRouter device%u: Ramulator type requires configFile", d);
                    }
                    uint64_t cpuFreqHz = 1000000ULL * zinfo->freqMHz;
                    uint32_t domain = d * zinfo->numDomains / numDevices;
                    RamulatorMemory* rmc = new RamulatorMemory(ramCfg, cpuFreqHz, devLat, domain, mcName);
                    rmc->setPGDevice(true);   // 1.11.18: device-side PG tracker
                    mc = rmc;
                } else if (devType == "WeaveSimple") {
                    uint32_t bw = config.get<uint32_t>((devPrefix + ".bandwidth").c_str(), 12800);
                    uint32_t boundLat = config.get<uint32_t>((devPrefix + ".boundLatency").c_str(), devLat);
                    uint32_t domain = d * zinfo->numDomains / numDevices;
                    mc = new WeaveSimpleMemory(zinfo->lineSize, zinfo->freqMHz, bw, devLat, boundLat, domain, mcName);
                } else if (devType == "MD1" || devType == "WeaveMD1") {
                    panic("SystemRouter device%u: type '%s' removed, use 'Simple' or 'WeaveSimple'", d, devType.c_str());
                } else {
                    // Default: Simple (M/D/1 always active)
                    uint32_t bw = config.get<uint32_t>((devPrefix + ".bandwidth").c_str(), 12800);
                    mc = new SimpleMemory(zinfo->lineSize, zinfo->freqMHz, bw, devLat, mcName);
                }

                // Convert byte addresses to line addresses
                devices[d].addrStart = addrStart >> lineBits;
                devices[d].addrEnd = addrEnd >> lineBits;
                devices[d].mc = mc;
                devices[d].networkNodeId = netNodeId;
            }

            // System network params
            uint32_t routerLat = config.get<uint32_t>("sys.systemNetwork.routerLatency", 1);
            uint32_t linkLat = config.get<uint32_t>("sys.systemNetwork.latencyCycles", 5);
            uint32_t linkWidth = config.get<uint32_t>("sys.systemNetwork.linkWidthBits", 512);
            uint32_t netModel = zinfo->systemNetwork.model;
            uint32_t numNodes = zinfo->systemNetwork.numNodes;

            uint32_t perHopLat = routerLat + linkLat;
            uint32_t serialization = (uint32_t)((64 * 8 + linkWidth - 1) / linkWidth);
            int totalVCs = config.get<uint32_t>("sys.systemNetwork.virtualChannelsPerVn", 1) * 2;

            // Build hop matrix from pre-computed latencies
            double hopMatrix[256] = {};
            for (uint32_t i = 0; i < numNodes && i < 16; i++) {
                for (uint32_t j = 0; j < numNodes && j < 16; j++) {
                    if (i != j) {
                        uint32_t lat = zinfo->systemNetwork.linkLatency[i * numNodes + j];
                        // Convert back to hops: (lat - serialization) / perHopLat
                        if (perHopLat > 0 && lat > serialization) {
                            hopMatrix[i * numNodes + j] = (double)(lat - serialization) / perHopLat;
                        } else {
                            hopMatrix[i * numNodes + j] = 1.0;  // minimum 1 hop
                        }
                    }
                }
            }

            MemObject* router = new SystemAddressRouter(
                devices, zinfo->systemNetwork.coreToNode, zinfo->numCores,
                perHopLat, serialization, netModel, hopMatrix, numNodes,
                totalVCs, zinfo->systemNetwork.linkWidthBits, "sys-router");

            mems.resize(1);
            mems[0] = router;

            info("[ZSim] SystemAddressRouter: %u devices, %u nodes, model=%u",
                 numDevices, numNodes, netModel);

            // Create system-level Garnet network for detailed model (model==2)
            if (netModel == 2) {
                string sysTopoStr = config.get<const char*>("sys.systemNetwork.topology", "CROSSBAR");
                NoCTopology sysTopo = parseNoCTopology(sysTopoStr);
                NoCRouting sysRouting = getDefaultRouting(sysTopo);
                uint32_t sysVCs = config.get<uint32_t>("sys.systemNetwork.virtualChannelsPerVn", 1);
                uint32_t sysBuffers = config.get<uint32_t>("sys.systemNetwork.inputBufferDepth", 4);
                uint32_t sysFlitBits = linkWidth;  // reuse link width as flit size

                // For non-grid topologies, use numNodes x 1
                uint32_t sysRows = numNodes, sysCols = 1;
                if (sysTopo == NoCTopology::MESH_2D || sysTopo == NoCTopology::TORUS_2D) {
                    sysCols = (uint32_t)std::ceil(std::sqrt((double)numNodes));
                    sysRows = (numNodes + sysCols - 1) / sysCols;
                }

                zinfo->systemGarnetNetwork = new GarnetNetwork(
                    sysTopo, sysRows, sysCols,
                    routerLat, linkLat, true /* cycleAccurate */,
                    sysRouting, sysVCs, sysBuffers,
                    1000.0 /* clockMhz — will use reference freq */, sysFlitBits);

                // Register system network nodes
                for (uint32_t n = 0; n < numNodes; n++) {
                    char nodeName[32];
                    snprintf(nodeName, sizeof(nodeName), "sys-node-%u", n);
                    zinfo->systemGarnetNetwork->registerNode(nodeName, n);
                }

                // Wire Garnet to the SystemAddressRouter
                static_cast<SystemAddressRouter*>(router)->setSystemGarnet(
                    zinfo->systemGarnetNetwork);

                info("[ZSim] System Garnet network created: %u nodes, topology=%s",
                     numNodes, sysTopoStr.c_str());
            }
        }
    }

    //Connect everything
    bool printHierarchy = config.get<bool>("sim.printHierarchy", false);

    if (hasCaches) {
    // Connect top-level cache group(s) to memory.
    // Single-LLC: one group with one instance, all banks share memory.
    // Multi-LLC: each top-level group instance connects independently to memory.
    for (const string& llcGrp : llcGroups) {
        CacheGroup& llcCaches = *cMap[llcGrp];
        for (uint32_t inst = 0; inst < llcCaches.size(); inst++) {
            uint32_t childId = 0;
            for (BaseCache* llcBank : llcCaches[inst]) {
                llcBank->setParents(childId++, mems, network);
            }
        }
    }

    // Rest of caches
    for (const char* grp : cacheGroupNames) {
        if (isTerminal(grp)) continue; //skip terminal caches

        CacheGroup& parentCaches = *cMap[grp];
        uint32_t parents = parentCaches.size();
        assert(parents);

        // Linearize concatenated / interleaved caches from childMap cacheGroups
        CacheGroup childCaches;

        for (auto childVec : childMap[grp]) {
            if (!childVec.size()) continue;
            size_t vecSize = cMap[childVec[0]]->size();
            for (string child : childVec) {
                if (cMap[child]->size() != vecSize) {
                    panic("In interleaved group %s, %s has a different number of caches", Str(childVec).c_str(), child.c_str());
                }
            }

            CacheGroup interleavedGroup;
            for (uint32_t i = 0; i < vecSize; i++) {
                for (uint32_t j = 0; j < childVec.size(); j++) {
                    interleavedGroup.push_back(cMap[childVec[j]]->at(i));
                }
            }

            childCaches.insert(childCaches.end(), interleavedGroup.begin(), interleavedGroup.end());
        }

        uint32_t children = childCaches.size();
        assert(children);

        uint32_t childrenPerParent = children/parents;
        if (children % parents != 0) {
            panic("%s has %d caches and %d children, they are non-divisible. "
                  "Use multiple groups for non-homogeneous children per parent!", grp, parents, children);
        }

        for (uint32_t p = 0; p < parents; p++) {
            g_vector<MemObject*> parentsVec;
            parentsVec.insert(parentsVec.end(), parentCaches[p].begin(), parentCaches[p].end()); //BaseCache* to MemObject* is a safe cast

            uint32_t childId = 0;
            g_vector<BaseCache*> childrenVec;
            for (uint32_t c = p*childrenPerParent; c < (p+1)*childrenPerParent; c++) {
                for (BaseCache* bank : childCaches[c]) {
                    bank->setParents(childId++, parentsVec, network);
                    childrenVec.push_back(bank);
                }
            }

            if (printHierarchy) {
                vector<string> cacheNames;
                std::transform(childrenVec.begin(), childrenVec.end(), std::back_inserter(cacheNames),
                        [](BaseCache* c) -> string { string s = c->getName(); return s; });

                string parentName = parentCaches[p][0]->getName();
                if (parentCaches[p].size() > 1) {
                    parentName += "..";
                    parentName += parentCaches[p][parentCaches[p].size()-1]->getName();
                }
                info("Hierarchy: %s -> %s", Str(cacheNames).c_str(), parentName.c_str());
            }

            for (BaseCache* bank : parentCaches[p]) {
                bank->setChildren(childrenVec, network);
            }
        }
    }

    //Check that all the terminal caches have a single bank
    for (const char* grp : cacheGroupNames) {
        if (isTerminal(grp)) {
            uint32_t banks = (*cMap[grp])[0].size();
            if (banks != 1) panic("Terminal cache group %s needs to have a single bank, has %d", grp, banks);
        }
    }
    } // hasCaches — end of cache connection

    //Tracks how many terminal caches have been allocated to cores
    unordered_map<string, uint32_t> assignedCaches;
    for (const char* grp : cacheGroupNames) if (isTerminal(grp)) assignedCaches[grp] = 0;

    if (!zinfo->traceDriven) {
        //Instantiate the cores
        vector<const char*> coreGroupNames;
        unordered_map <string, vector<Core*>> coreMap;
        config.subgroups("sys.cores", coreGroupNames);

        uint32_t coreIdx = 0;
        for (const char* group : coreGroupNames) {
            if (parentMap.count(group)) panic("Core group name %s is invalid, a cache group already has that name", group);

            coreMap[group] = vector<Core*>();

            string prefix = string("sys.cores.") + group + ".";
            uint32_t cores = config.get<uint32_t>(prefix + "cores", 1);
            string type = config.get<const char*>(prefix + "type", "Simple");
            /* 1.11.43 (audit E23): the FPU flag is PER GROUP, not global. The
             * old sys.hierarchy.peHasFpu applied the device PE's soft-float
             * penalty to every core in the simulation, host included. The
             * global keys survive as the default so old emitted configs keep
             * their meaning; a group key overrides for that group only. */
            bool grpHasFpu = config.get<bool>(prefix + "hasFpu",
                                              zinfo->hierarchy.peHasFpu);
            uint32_t grpFpEmul = config.get<uint32_t>(prefix + "fpEmulCycles",
                                              zinfo->hierarchy.fpEmulCycles);

            //Build the core group
            union {
                SimpleCore* simpleCores;
                InOrderCore* inOrderCores;
                OOOCore* oooCores;
                NullCore* nullCores;
                ALUCore* aluCores;
            };
            if (type == "Simple") {
                simpleCores = gm_memalign<SimpleCore>(CACHE_LINE_BYTES, cores);
            } else if (type == "InOrder") {
                inOrderCores = gm_memalign<InOrderCore>(CACHE_LINE_BYTES, cores);
            } else if (type == "OOO" || type == "OoO") {
                oooCores = gm_memalign<OOOCore>(CACHE_LINE_BYTES, cores);
                zinfo->oooDecode = true; //enable uop decoding, this is false by default, must be true if even one OOO cpu is in the system
            } else if (type == "Null") {
                nullCores = gm_memalign<NullCore>(CACHE_LINE_BYTES, cores);
            } else if (type == "ALU") {
                aluCores = gm_memalign<ALUCore>(CACHE_LINE_BYTES, cores);
            } else {
                panic("%s: Invalid core type %s", group, type.c_str());
            }

            if (type != "Null" && type != "ALU") {
                string icache = config.get<const char*>(prefix + "icache");
                string dcache = config.get<const char*>(prefix + "dcache");

                if (!assignedCaches.count(icache)) panic("%s: Invalid icache parameter %s", group, icache.c_str());
                if (!assignedCaches.count(dcache)) panic("%s: Invalid dcache parameter %s", group, dcache.c_str());

                for (uint32_t j = 0; j < cores; j++) {
                    stringstream ss;
                    ss << group << "-" << j;
                    g_string name(ss.str().c_str());
                    Core* core;

                    //Get the caches
                    CacheGroup& igroup = *cMap[icache];
                    CacheGroup& dgroup = *cMap[dcache];

                    if (assignedCaches[icache] >= igroup.size()) {
                        panic("%s: icache group %s (%ld caches) is fully used, can't connect more cores to it", name.c_str(), icache.c_str(), igroup.size());
                    }
                    // Use virtual method instead of dynamic_cast for -fno-rtti compatibility
                    FilterCache* ic = igroup[assignedCaches[icache]][0]->asFilterCache();
                    assert(ic);
                    ic->setSourceId(coreIdx);
                    ic->setFlags(MemReq::IFETCH | MemReq::NOEXCL);
                    assignedCaches[icache]++;

                    if (assignedCaches[dcache] >= dgroup.size()) {
                        panic("%s: dcache group %s (%ld caches) is fully used, can't connect more cores to it", name.c_str(), dcache.c_str(), dgroup.size());
                    }
                    // Use virtual method instead of dynamic_cast for -fno-rtti compatibility
                    FilterCache* dc = dgroup[assignedCaches[dcache]][0]->asFilterCache();
                    assert(dc);
                    dc->setSourceId(coreIdx);
                    assignedCaches[dcache]++;

                    //Build the core
                    if (type == "Simple") {
                        SimpleCore* score = new (&simpleCores[j]) SimpleCore(ic, dc, name);
                        score->setFpuCapability(grpHasFpu, grpFpEmul);   // 1.11.43 (E23)
                        core = score;
                    } else if (type == "InOrder") {
                        uint32_t domain = j*zinfo->numDomains/cores;
                        // In-order superscalar issue width (YAML pim.pe.issue_width;
                        // PIMID_INORDER_WIDTH env overrides inside the ctor). Default 2.
                        uint32_t issueWidth = config.get<uint32_t>(prefix + "issueWidth", 2);
                        InOrderCore* tcore = new (&inOrderCores[j]) InOrderCore(ic, dc, domain, name, issueWidth);
                        tcore->setFpuCapability(grpHasFpu, grpFpEmul);   // 1.11.43 (E23)
                        zinfo->eventRecorders[coreIdx] = tcore->getEventRecorder();
                        zinfo->eventRecorders[coreIdx]->setSourceId(coreIdx);
                        core = tcore;
                    } else {
                        assert(type == "OOO" || type == "OoO");
                        OOOCore* ocore = new (&oooCores[j]) OOOCore(ic, dc, name);
                        ocore->setFpuCapability(grpHasFpu, grpFpEmul);   // 1.11.47 (L203)
                        zinfo->eventRecorders[coreIdx] = ocore->getEventRecorder();
                        zinfo->eventRecorders[coreIdx]->setSourceId(coreIdx);
                        core = ocore;
                    }
                    coreMap[group].push_back(core);
                    coreIdx++;
                }
            } else if (type == "Null") {
                for (uint32_t j = 0; j < cores; j++) {
                    stringstream ss;
                    ss << group << "-" << j;
                    g_string name(ss.str().c_str());
                    Core* core = new (&nullCores[j]) NullCore(name);
                    coreMap[group].push_back(core);
                    coreIdx++;
                }
            } else {
                assert(type == "ALU");
                static uint32_t aluCoreIdx = 0;  // PE ordinal across ALU groups (PE-mem map index)
                double computeFactor    = config.get<double>(prefix + "computeFactor", 1.0);
                double accessFactor     = config.get<double>(prefix + "accessFactor", 1.0);
                double throughputFactor  = config.get<double>(prefix + "throughputFactor", 1.0);
                uint32_t operandWidth   = config.get<uint32_t>(prefix + "operandWidth", 32);
                double energyFactor     = config.get<double>(prefix + "energyFactor", 1.0);
                bool bitSerial          = config.get<bool>(prefix + "bitSerial", false);
                for (uint32_t j = 0; j < cores; j++) {
                    stringstream ss;
                    ss << group << "-" << j;
                    g_string name(ss.str().c_str());
                    // Wire PE memory interface if hierarchy PE-MIs exist.
                    // Assign core to MI by home bank (not sequential grouping).
                    // The PE-mem map is indexed by PE ORDINAL (aluCoreIdx),
                    // not global core index: in co-sim, host cores precede
                    // the device PEs in the global core array, but the device
                    // model's map covers only its own PEs.
                    PEMemoryInterface* mi = nullptr;
                    if (!rawMIs.empty()) {
                        // MI index == PE ORDINAL group, by construction of the
                        // coverage build above (MI-i covers PE-i-group's slice).
                        // The old home-ORG-id lookup was a stale pre-slice
                        // convention: at SUBARRAY placement home orgs (0, 2048,
                        // 4096, ...) overflow the MI array, silently leaving
                        // every PE but PE 0 with mi_ == nullptr (flat path, no
                        // PE-MI, no NoC).
                        uint32_t miIdx = aluCoreIdx / std::max(zinfo->hierarchy.pesPerMC, 1u);
                        // MPI: every rank builds the same P cores but runs its
                        // single guest thread on core 0. Rank r IS the r-th PE
                        // group of the shared device: offset so rank r's core 0
                        // drives MI r*(P/N) -- its own slice, its own Garnet
                        // source node (matches the addrToUnit rank offset and
                        // the shared NoC log's nodesPerRank mapping).
                        const char* mpiRk = getenv("PIMID_MPI_RANK");
                        const char* mpiRn = getenv("PIMID_MPI_RANKS");
                        if (mpiRk && mpiRn && !rawMIs.empty()) {
                            uint32_t r = (uint32_t)atoi(mpiRk);
                            uint32_t n = (uint32_t)atoi(mpiRn);
                            uint32_t misPerRank = (n > 0 && rawMIs.size() >= n)
                                                  ? rawMIs.size() / n : 1;
                            miIdx = (miIdx + r * misPerRank) % rawMIs.size();
                        }
                        if (miIdx < rawMIs.size()) mi = rawMIs[miIdx];
                    }
                    ALUCore* acore = new (&aluCores[j]) ALUCore(name, computeFactor, accessFactor,
                                                             throughputFactor, operandWidth, energyFactor,
                                                             mi, coreIdx, bitSerial);
                    acore->setFpuCapability(grpHasFpu, grpFpEmul);   // 1.11.43 (E23)
                    Core* core = acore;
                    coreMap[group].push_back(core);
                    coreIdx++;
                    aluCoreIdx++;
                }
            }
        }

        //Check that all the terminal caches are fully connected
        for (const char* grp : cacheGroupNames) {
            if (isTerminal(grp) && assignedCaches[grp] != cMap[grp]->size()) {
                panic("%s: Terminal cache group not fully connected, %ld caches, %d assigned", grp, cMap[grp]->size(), assignedCaches[grp]);
            }
        }

        //Populate global core info
        assert(zinfo->numCores == coreIdx);
        zinfo->cores = gm_memalign<Core*>(CACHE_LINE_BYTES, zinfo->numCores);
        coreIdx = 0;
        for (const char* group : coreGroupNames) for (Core* core : coreMap[group]) zinfo->cores[coreIdx++] = core;

        //Init stats: cores
        for (const char* group : coreGroupNames) {
            AggregateStat* groupStat = new AggregateStat(true);
            groupStat->init(gm_strdup(group), "Core stats");
            for (Core* core : coreMap[group]) core->initStats(groupStat);
            zinfo->rootStat->append(groupStat);
        }
    } else {  // trace-driven: create trace driver and proxy caches
        vector<TraceDriverProxyCache*> proxies;
        for (const char* grp : cacheGroupNames) {
            if (isTerminal(grp)) {
                for (vector<BaseCache*> cv : *cMap[grp]) {
                    assert(cv.size() == 1);
                    // Use virtual method instead of dynamic_cast for -fno-rtti compatibility
                    TraceDriverProxyCache* proxy = cv[0]->asTraceDriverProxyCache();
                    assert(proxy);
                    proxies.push_back(proxy);
                }
            }
        }

        //FIXME: For now, we assume we are driving a single-bank LLC
        string traceFile = config.get<const char*>("sim.traceFile");
        string retraceFile = config.get<const char*>("sim.retraceFile", ""); //leave empty to not retrace
        zinfo->traceDriver = new TraceDriver(traceFile, retraceFile, proxies,
                config.get<bool>("sim.useSkews", true), // incorporate skews in to playback and simulator results, not only the output trace
                config.get<bool>("sim.playPuts", true),
                config.get<bool>("sim.playAllGets", true));
        zinfo->traceDriver->initStats(zinfo->rootStat);
    }

    //Init stats: caches, mem
    if (!zinfo->roiRebaseStats) zinfo->roiRebaseStats = new g_vector<AggregateStat*>();   // 1.11.90
    for (const char* group : cacheGroupNames) {
        AggregateStat* groupStat = new AggregateStat(true);
        groupStat->init(gm_strdup(group), "Cache stats");
        for (vector<BaseCache*>& banks : *cMap[group]) for (BaseCache* bank : banks) bank->initStats(groupStat);
        zinfo->rootStat->append(groupStat);
        zinfo->roiRebaseStats->push_back(groupStat);   // 1.11.90
    }

    //Initialize event recorders
    //for (uint32_t i = 0; i < zinfo->numCores; i++) eventRecorders[i] = new EventRecorder();

    AggregateStat* memStat = new AggregateStat(true);
    memStat->init("mem", "Memory controller stats");
    for (auto mem : mems) mem->initStats(memStat);
    zinfo->roiRebaseStats->push_back(memStat);   // 1.11.90
    /* 1.9.35: in co-simulation the PE memory interfaces were CONSTRUCTED and
     * then dropped from `mems` (see the cosimMode branch above, which clears the
     * vector so the host's own controllers can occupy it). They survived only in
     * rawMIs, for wiring. This registration loop therefore never saw them, so a
     * co-simulation stats dump carried NO device-side memory group at all: the
     * counters existed, were incremented on the live path, and were never
     * emitted. The power model consequently priced the device at zero
     * memory-controller activity while charging the host's controller for
     * traffic that was partly the device's.
     *
     * GUARDED ON cosimMode. rawMIs is populated unconditionally above, and
     * OUTSIDE co-simulation the very same objects are still in `mems` -- only
     * the co-sim branch clears that vector. Registering both lists there calls
     * initStats() twice on one object, which trips the "name already set"
     * assertion in stats.h and takes the run down with it. Only co-simulation
     * has PE-MIs that `mems` no longer holds. */
    zinfo->rootStat->append(memStat);
    /* The PE-MIs need their OWN aggregate, not `mem`. The HDF5 backend requires
     * a "regular" aggregate -- every child the same type -- and `mem` already
     * holds the host's MemoryController stats, whose shape differs from a
     * PEMemoryInterface's. Appending both panics at hdf5_stats.cpp:139. A
     * separate group also reads better: host controllers under `mem`, device
     * interfaces under `pe_mem`. The text reader matches on the child names
     * ("pe-mi-<N>"), so it is unaffected by the parent group's name. */
    if (cosimMode && !rawMIs.empty()) {
        AggregateStat* peMemStat = new AggregateStat(true);
        peMemStat->init("pe_mem", "Device PE memory interface stats");
        for (auto mi : rawMIs) {
            if (mi) mi->initStats(peMemStat);
        }
        zinfo->rootStat->append(peMemStat);
        zinfo->roiRebaseStats->push_back(peMemStat);   // 1.11.90
    }

    //Odds and ends: BuildCacheGroup new'd the cache groups, we need to delete them
    for (pair<string, CacheGroup*> kv : cMap) delete kv.second;
    cMap.clear();

    info("Initialized system");
}

static void PreInitStats() {
    zinfo->rootStat = new AggregateStat();
    zinfo->rootStat->init("root", "Stats");
}

static void PostInitStats(bool perProcessDir, Config& config) {
    zinfo->rootStat->makeImmutable();
    zinfo->trigger = 15000;

    string pathStr = zinfo->outputDir;
    pathStr += "/";

    // Absolute paths for stats files. Note these must be in the global heap.
    const char* pStatsFile = gm_strdup((pathStr + "zsim.h5").c_str());
    const char* evStatsFile = gm_strdup((pathStr + "zsim-ev.h5").c_str());
    const char* cmpStatsFile = gm_strdup((pathStr + "zsim-cmp.h5").c_str());
    const char* statsFile = gm_strdup((pathStr + "zsim.out").c_str());

    if (zinfo->statsPhaseInterval) {
        const char* periodicStatsFilter = config.get<const char*>("sim.periodicStatsFilter", "");
        AggregateStat* prStat = (!strlen(periodicStatsFilter))? zinfo->rootStat : FilterStats(zinfo->rootStat, periodicStatsFilter);
        if (!prStat) panic("No stats match sim.periodicStatsFilter regex (%s)! Set interval to 0 to avoid periodic stats", periodicStatsFilter);
#ifndef ZSIM_NO_HDF5
        zinfo->periodicStatsBackend = new HDF5Backend(pStatsFile, prStat, (1 << 20) /* 1MB chunks */, zinfo->skipStatsVectors, zinfo->compactPeriodicStats);
#else
        // Use text backend as fallback when HDF5 is disabled
        zinfo->periodicStatsBackend = new TextBackend(pStatsFile, prStat);
#endif
        zinfo->periodicStatsBackend->dump(true); //must have a first sample

        class PeriodicStatsDumpEvent : public Event {
            public:
                explicit PeriodicStatsDumpEvent(uint32_t period) : Event(period) {}
                void callback() {
                    zinfo->trigger = 10000;
                    zinfo->periodicStatsBackend->dump(true /*buffered*/);
                }
        };

        zinfo->eventQueue->insert(new PeriodicStatsDumpEvent(zinfo->statsPhaseInterval));
        zinfo->statsBackends->push_back(zinfo->periodicStatsBackend);
    } else {
        zinfo->periodicStatsBackend = nullptr;
    }

#ifndef ZSIM_NO_HDF5
    zinfo->eventualStatsBackend = new HDF5Backend(evStatsFile, zinfo->rootStat, (1 << 17) /* 128KB chunks */, zinfo->skipStatsVectors, false /* don't sum regular aggregates*/);
#else
    zinfo->eventualStatsBackend = new TextBackend(evStatsFile, zinfo->rootStat);
#endif
    zinfo->eventualStatsBackend->dump(true); //must have a first sample
    zinfo->statsBackends->push_back(zinfo->eventualStatsBackend);

    if (zinfo->maxMinInstrs) {
        warn("maxMinInstrs IS DEPRECATED");
        for (uint32_t i = 0; i < zinfo->numCores; i++) {
            auto getInstrs = [i]() { return zinfo->cores[i]->getInstrs(); };
            auto dumpStats = [i]() {
                info("Dumping eventual stats for core %d", i);
                zinfo->trigger = i;
                zinfo->eventualStatsBackend->dump(true /*buffered*/);
            };
            zinfo->eventQueue->insert(makeAdaptiveEvent(getInstrs, dumpStats, 0, zinfo->maxMinInstrs, MAX_IPC*zinfo->phaseLength));
        }
    }

    // Convenience stats
#ifndef ZSIM_NO_HDF5
    StatsBackend* compactStats = new HDF5Backend(cmpStatsFile, zinfo->rootStat, 0 /* no aggregation, this is just 1 record */, zinfo->skipStatsVectors, true); //don't dump a first sample.
#else
    StatsBackend* compactStats = new TextBackend(cmpStatsFile, zinfo->rootStat);
#endif
    StatsBackend* textStats = new TextBackend(statsFile, zinfo->rootStat);
    zinfo->statsBackends->push_back(compactStats);
    zinfo->statsBackends->push_back(textStats);
}

static void InitGlobalStats() {
    zinfo->profSimTime = new TimeBreakdownStat();
    const char* stateNames[] = {"init", "bound", "weave", "ff"};
    zinfo->profSimTime->init("time", "Simulator time breakdown", 4, stateNames);
    zinfo->rootStat->append(zinfo->profSimTime);

    ProxyStat* triggerStat = new ProxyStat();
    triggerStat->init("trigger", "Reason for this stats dump", &zinfo->trigger);
    zinfo->rootStat->append(triggerStat);

    ProxyStat* phaseStat = new ProxyStat();
    phaseStat->init("phase", "Simulated phases", &zinfo->numPhases);
    zinfo->rootStat->append(phaseStat);

    /* 1.11.7 (#85): crossing counters -> zsim.out root scalars.
     *
     * 1.11.54 (audit F009): these are plain ProxyStats over the raw
     * accumulators -- NO ROI base, unlike the pg counters below -- and they
     * ARE priced (link framing + byte energy, and the flush writebacks).
     * That is correct today only because of an INVARIANT: every crossing
     * site in the plugin sits at or inside the offload region (the flush is
     * charged at roi_begin; launch runs inside cosimRoiBeginOffload, which
     * the handler calls AFTER markRoi; WORK_BEGIN/WORK_END are by
     * definition inside). A crossing recorded BEFORE zsim_roi_begin would be
     * priced against an ROI-windowed wall clock and silently inflate link
     * energy. If you add a crossing site, either keep it inside the ROI or
     * give these counters an ROI base like addPgStat does. */
    ProxyStat* xH2D = new ProxyStat();
    xH2D->init("xingH2DBytes", "Host->device crossing payload bytes",
               (uint64_t*)&zinfo->xing.h2dBytes);
    zinfo->rootStat->append(xH2D);
    ProxyStat* xD2H = new ProxyStat();
    xD2H->init("xingD2HBytes", "Device->host crossing payload bytes",
               (uint64_t*)&zinfo->xing.d2hBytes);
    zinfo->rootStat->append(xD2H);
    ProxyStat* xCnt = new ProxyStat();
    xCnt->init("xingCount", "Discrete host<->device crossings",
               (uint64_t*)&zinfo->xing.count);
    zinfo->rootStat->append(xCnt);
    ProxyStat* xFlush = new ProxyStat();
    xFlush->init("xingFlushBytes", "Coherence-flush footprint bytes crossed",
                 (uint64_t*)&zinfo->xing.flushBytes);
    zinfo->rootStat->append(xFlush);

    // 1.11.8 (#84): PG residency trackers -> zsim.out root scalars.
    // Idle residency derives at consumption: r = 1 - activePhases/phase.
    /* 1.11.18 (audit go-through): ROI-RELATIVE. These were whole-run while
     * every activity counter the residency is weighed against is windowed
     * at roi_begin; pgPhaseWindow is the matching denominator (numPhases
     * minus the ROI phase), so consumers divide two numbers from the same
     * window. Without an ROI every base is 0 and these are the old values. */
    auto addPgStat = [](const char* n, const char* d,
                        volatile uint64_t* cur, uint64_t* base) {
        auto fn = [cur, base]() -> uint64_t {
            uint64_t c = *cur, b = *base;
            return (c > b) ? (c - b) : 0;
        };
        auto* st = new LambdaStat<decltype(fn)>(fn);
        st->init(n, d);
        zinfo->rootStat->append(st);
    };
    addPgStat("pgAnyCoreActivePhases", "Phases with >=1 core retiring (ROI)",
              &zinfo->pgres.anyCore.activePhases, &zinfo->pgres.roiAnyCore);
    addPgStat("pgSharedCacheActivePhases", "Phases with >=1 shared-cache access (ROI)",
              &zinfo->pgres.sharedCache.activePhases, &zinfo->pgres.roiSharedCache);
    addPgStat("pgNocActivePhases", "Phases with >=1 fabric injection (ROI)",
              &zinfo->pgres.noc.activePhases, &zinfo->pgres.roiNoc);
    addPgStat("pgHostMCActivePhases", "Phases with >=1 host-MC access (ROI)",
              &zinfo->pgres.hostMC.activePhases, &zinfo->pgres.roiHostMC);
    addPgStat("pgDevMCActivePhases", "Phases with >=1 device-MC access (ROI)",
              &zinfo->pgres.devMC[0].activePhases, &zinfo->pgres.roiDevMC0);
    {   // the denominator for all of the above
        auto fnw = []() -> uint64_t {
            uint64_t p = zinfo->numPhases, b = zinfo->pgres.roiPhase;
            return (p > b) ? (p - b) : 0;
        };
        auto* stw = new LambdaStat<decltype(fnw)>(fnw);
        stw->init("pgPhaseWindow", "Phases in the priced (ROI) window");
        zinfo->rootStat->append(stw);
    }
    /* 1.11.51 (E17, closing the measure-first ruling): the devMC gap
     * histogram leaves the log and enters the stats stream, so the power
     * model can read it back. The 10,000-cycle phase residency above is
     * three orders of magnitude coarser than tXP-scale power-down windows
     * -- it measured r_idle = 0 on cells whose own histogram shows ~90% of
     * ROI cycles sitting in gaps a JEDEC power-down could use. The
     * THRESHOLD is not chosen here: the plugin exports the raw log2
     * buckets; the orchestrator applies the per-generation sourced tXP (and
     * the user's override), which is where that knowledge lives. NoC gaps
     * stay measurement-only: a logic clock-gating wake penalty has no
     * source, and one is not invented (the E17 ruling). */
    {
        static char nmC[GapHist::kBuckets][32], nmY[GapHist::kBuckets][32];
        for (int b = 0; b < GapHist::kBuckets; b++) {
            snprintf(nmC[b], sizeof(nmC[b]), "pgDevMCGapCnt_%d", b);
            snprintf(nmY[b], sizeof(nmY[b]), "pgDevMCGapCyc_%d", b);
            auto fc = [b]() -> uint64_t { return zinfo->pgres.devMCGaps.count[b]; };
            auto* sc = new LambdaStat<decltype(fc)>(fc);
            sc->init(nmC[b], "devMC gap-histogram count, log2 bucket");
            zinfo->rootStat->append(sc);
            auto fy = [b]() -> uint64_t { return zinfo->pgres.devMCGaps.cycles[b]; };
            auto* sy = new LambdaStat<decltype(fy)>(fy);
            sy->init(nmY[b], "devMC gap-histogram cycles, log2 bucket");
            zinfo->rootStat->append(sy);
        }
        auto fs = []() -> uint64_t { return zinfo->pgres.devMCGaps.spanCycles(); };
        auto* ss = new LambdaStat<decltype(fs)>(fs);
        ss->init("pgDevMCGapSpan", "devMC gap-histogram ROI span, cycles");
        zinfo->rootStat->append(ss);
    }
}


void SimInit(const char* configFile, const char* outputDir, uint32_t shmid) {
    zinfo = gm_calloc<GlobSimInfo>();
    zinfo->outputDir = gm_strdup(outputDir);
    zinfo->statsBackends = new g_vector<StatsBackend*>();

    Config config(configFile);

    //Debugging
    //NOTE: This should be as early as possible, so that we can attach to the debugger before initialization.
    zinfo->attachDebugger = config.get<bool>("sim.attachDebugger", false);
    zinfo->harnessPid = getppid();
    getLibzsimAddrs(&zinfo->libzsimAddrs);

    if (zinfo->attachDebugger) {
        gm_set_secondary_ptr(&zinfo->libzsimAddrs);
        notifyHarnessForDebugger(zinfo->harnessPid);
    }

    PreInitStats();

    zinfo->traceDriven = config.get<bool>("sim.traceDriven", false);

    if (zinfo->traceDriven) {
        zinfo->numCores = 0;
    } else {
        // Get the number of cores
        // TODO: There is some duplication with the core creation code. This should be fixed eventually.
        uint32_t numCores = 0;
        vector<const char*> groups;
        config.subgroups("sys.cores", groups);
        for (const char* group : groups) {
            uint32_t cores = config.get<uint32_t>(string("sys.cores.") + group + ".cores", 1);
            numCores += cores;
        }

        if (numCores == 0) panic("Config must define some core classes in sys.cores; sys.numCores is deprecated");
        zinfo->numCores = numCores;
        assert(numCores <= MAX_THREADS); //TODO: Is there any reason for this limit?
    }

    zinfo->numDomains = config.get<uint32_t>("sim.domains", 1);
    uint32_t numSimThreads = config.get<uint32_t>("sim.contentionThreads", MAX((uint32_t)1, zinfo->numDomains/2)); //gives a bit of parallelism, TODO tune
    zinfo->contentionSim = new ContentionSim(zinfo->numDomains, numSimThreads);
    zinfo->contentionSim->initStats(zinfo->rootStat);
    zinfo->eventRecorders = gm_calloc<EventRecorder*>(zinfo->numCores);

    zinfo->traceWriters = new g_vector<AccessTraceWriter*>();

    // Global simulation values
    zinfo->numPhases = 0;

    zinfo->phaseLength = config.get<uint32_t>("sim.phaseLength", 10000);
    zinfo->statsPhaseInterval = config.get<uint32_t>("sim.statsPhaseInterval", 100);
    zinfo->freqMHz = config.get<uint32_t>("sys.frequency", 2000);

    //Maxima/termination conditions
    zinfo->maxPhases = config.get<uint64_t>("sim.maxPhases", 0);
    zinfo->maxMinInstrs = config.get<uint64_t>("sim.maxMinInstrs", 0);
    zinfo->maxTotalInstrs = config.get<uint64_t>("sim.maxTotalInstrs", 0);

    uint64_t maxSimTime = config.get<uint32_t>("sim.maxSimTime", 0);
    zinfo->maxSimTimeNs = maxSimTime*1000L*1000L*1000L;

    zinfo->maxProcEventualDumps = config.get<uint32_t>("sim.maxProcEventualDumps", 0);
    zinfo->procEventualDumps = 0;

    zinfo->skipStatsVectors = config.get<bool>("sim.skipStatsVectors", false);
    zinfo->compactPeriodicStats = config.get<bool>("sim.compactPeriodicStats", false);

    //Fast-forwarding and magic ops
    zinfo->ignoreHooks = config.get<bool>("sim.ignoreHooks", false);
    zinfo->ffReinstrument = config.get<bool>("sim.ffReinstrument", false);
    if (zinfo->ffReinstrument) warn("sim.ffReinstrument = true, switching fast-forwarding on a multi-threaded process may be unstable");

    zinfo->registerThreads = config.get<bool>("sim.registerThreads", false);
    zinfo->globalPauseFlag = config.get<bool>("sim.startInGlobalPause", false);

    zinfo->eventQueue = new EventQueue(); //must be instantiated before the memory hierarchy

    if (!zinfo->traceDriven) {
        //Build the scheduler
        uint32_t parallelism = config.get<uint32_t>("sim.parallelism", 2*sysconf(_SC_NPROCESSORS_ONLN));
        if (parallelism < zinfo->numCores) info("Limiting concurrent threads to %d", parallelism);
        assert(parallelism > 0); //jeez...

        uint32_t schedQuantum = config.get<uint32_t>("sim.schedQuantum", 10000); //phases
        zinfo->sched = new Scheduler(EndOfPhaseActions, parallelism, zinfo->numCores, schedQuantum);
    } else {
        zinfo->sched = nullptr;
    }

    zinfo->blockingSyscalls = config.get<bool>("sim.blockingSyscalls", false);

    if (zinfo->blockingSyscalls) {
        warn("sim.blockingSyscalls = True, will likely deadlock with multi-threaded apps!");
    }

    InitGlobalStats();

    //Core stats (initialized here for cosmetic reasons, to be above cache stats)
    AggregateStat* allCoreStats = new AggregateStat(false);
    allCoreStats->init("core", "Core stats");
    zinfo->rootStat->append(allCoreStats);

    //Process tree needs this initialized, even though it is part of the memory hierarchy
    zinfo->lineSize = config.get<uint32_t>("sys.lineSize", 64);
    assert(zinfo->lineSize > 0);

    // SIMD width configuration (128, 256, 512 bits)
    zinfo->simdWidth = config.get<uint32_t>("sys.simdWidth", 128);
    if (zinfo->simdWidth != 128 && zinfo->simdWidth != 256 && zinfo->simdWidth != 512) {
        warn("Invalid SIMD width %d, using default 128 bits (SSE)", zinfo->simdWidth);
        zinfo->simdWidth = 128;
    }
    info("SIMD width: %d bits", zinfo->simdWidth);

    //Port virtualization
    for (uint32_t i = 0; i < MAX_PORT_DOMAINS; i++) zinfo->portVirt[i] = new PortVirtualizer();

    //Process hierarchy
    //NOTE: Due to partitioning, must be done before initializing memory hierarchy
    CreateProcessTree(config);
    zinfo->procArray[0]->notifyStart(); //called here so that we can detect end-before-start races

    //Caches, cores, memory controllers
    InitSystem(config);

    //Sched stats (deferred because of circular deps)
    if (zinfo->sched) zinfo->sched->initStats(zinfo->rootStat);

    zinfo->processStats = new ProcessStats(zinfo->rootStat);

    const char* procStatsFilter = config.get<const char*>("sim.procStatsFilter", "");
    if (strlen(procStatsFilter)) {
        zinfo->procStats = new ProcStats(zinfo->rootStat, FilterStats(zinfo->rootStat, procStatsFilter));
    } else {
        zinfo->procStats = nullptr;
    }

    //It's a global stat, but I want it to be last...
    // PIMID: heartbeats are unused under the QEMU+ZSim flow — the HEARTBEAT
    // magic op (1028) is never dispatched by our plugin, so this counter would
    // always report all-zeros and only clutters publication output. The stat
    // emission is disabled; profHeartbeats stays nullptr (zinfo is gm_calloc'd)
    // and the increment site in process_tree.cpp is null-guarded. To restore
    // per-process heartbeat reporting, re-enable these three lines.
    // zinfo->profHeartbeats = new VectorCounter();
    // zinfo->profHeartbeats->init("heartbeats", "Per-process heartbeats", zinfo->lineSize);
    // zinfo->rootStat->append(zinfo->profHeartbeats);

    bool perProcessDir = config.get<bool>("sim.perProcessDir", false);
    PostInitStats(perProcessDir, config);

    zinfo->perProcessCpuEnum = config.get<bool>("sim.perProcessCpuEnum", false);

    //Odds and ends
    bool printMemoryStats = config.get<bool>("sim.printMemoryStats", false);
    if (printMemoryStats) {
        gm_stats();
    }

    // Consume config keys emitted by PIMID but not read elsewhere in init.
    // Without this, strict config checking would panic on unused settings.
    config.get<bool>("sim.aslr", false);

    //Write config out
    bool strictConfig = config.get<bool>("sim.strictConfig", true); //if true, panic on unused variables
    config.writeAndClose((string(zinfo->outputDir) + "/out.cfg").c_str(), strictConfig);

    zinfo->contentionSim->postInit();

    info("Initialization complete");

    //Causes every other process to wake up
    gm_set_glob_ptr(zinfo);
}

