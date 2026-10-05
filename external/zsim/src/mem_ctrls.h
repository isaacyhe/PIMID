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

#ifndef MEM_CTRLS_H_
#define MEM_CTRLS_H_

#include <cmath>
#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "locks.h"
#include "memory_hierarchy.h"
#include "pad.h"
#include "stats.h"

// System-level Garnet network for detailed model
#include "garnet_network.h"

/* Simple memory controller with M/D/1 queuing for bandwidth contention.
 * This is the standard simple memory model -- M/D/1 is always active.
 * (The old fixed-latency SimpleMemory has been removed.)
 */
class SimpleMemory : public MemObject {
    private:
        uint64_t lastPhase;
        double maxRequestsPerCycle;
        double smoothedPhaseAccesses;
        uint32_t zeroLoadLatency;
        uint32_t curLatency;

        PAD();

        Counter profReads;
        Counter profWrites;
        Counter profTotalRdLat;
        Counter profTotalWrLat;
        Counter profLoad;
        Counter profUpdates;
        Counter profClampedLoads;
        uint32_t curPhaseAccesses;

        g_string name; //barely used
        lock_t updateLock;
        PAD();

    public:
        SimpleMemory(uint32_t lineSize, uint32_t megacyclesPerSecond, uint32_t megabytesPerSecond, uint32_t _zeroLoadLatency, g_string& _name);

        void initStats(AggregateStat* parentStat) {
            AggregateStat* memStats = new AggregateStat();
            memStats->init(name.c_str(), "Memory controller stats");
            profReads.init("rd", "Read requests"); memStats->append(&profReads);
            profWrites.init("wr", "Write requests"); memStats->append(&profWrites);
            profTotalRdLat.init("rdlat", "Total latency experienced by read requests"); memStats->append(&profTotalRdLat);
            profTotalWrLat.init("wrlat", "Total latency experienced by write requests"); memStats->append(&profTotalWrLat);
            profLoad.init("load", "Sum of load factors (0-100) per update"); memStats->append(&profLoad);
            profUpdates.init("ups", "Number of latency updates"); memStats->append(&profUpdates);
            profClampedLoads.init("clampedLoads", "Number of updates where the load was clamped to 95%"); memStats->append(&profClampedLoads);
            parentStat->append(memStats);
        }

        uint64_t access(MemReq& req);

        const char* getName() {return name.c_str();}

    private:
        void updateLatency();
};

// Fan out addresses interleaved across multiple memory controllers
class SplitAddrMemory : public MemObject {
    private:
        const g_vector<MemObject*> mems;
        const g_string name;
    public:
        SplitAddrMemory(const g_vector<MemObject*>& _mems, const char* _name) : mems(_mems), name(_name) {}

        uint64_t access(MemReq& req) {
            Address addr = req.lineAddr;
            uint32_t mem = addr % mems.size();
            Address ctrlAddr = addr/mems.size();
            req.lineAddr = ctrlAddr;
            uint64_t respCycle = mems[mem]->access(req);
            req.lineAddr = addr;
            return respCycle;
        }

        const char* getName() {
            return name.c_str();
        }

        void initStats(AggregateStat* parentStat) {
            for (auto mem : mems) mem->initStats(parentStat);
        }
};

/* Fixed one-hop crossbar latency for a MULTI-CORE host memory path.
 *
 * Wraps the host memory controller (or the SplitAddrMemory fan-out over the
 * per-tech channels) and adds a constant hop latency AFTER the MC access -- so
 * the hop is NOT inflated by the MC's M/D/1 port queueing (port contention is
 * the MC's job; this term is only the uncontended core->LLC/MC crossbar
 * traversal). Analytic host-side fabric term (ISSUE-5 crossbar tier): a 1-core
 * host emits no fabric, so this wrapper is only installed for num_cores>1.
 * Deliberately NOT a Garnet instance -- the detailed host tier is a later
 * increment; and the host caches are deliberately NOT pointed at the device
 * system network's getRTT (that link is host<->device, not host-internal).
 */
class CrossbarHopMemory : public MemObject {
    private:
        MemObject* mem;
        uint32_t hopCycles;
        g_string name;
    public:
        CrossbarHopMemory(MemObject* _mem, uint32_t _hopCycles, const char* _name)
            : mem(_mem), hopCycles(_hopCycles), name(_name) {}

        uint64_t access(MemReq& req) {
            uint64_t respCycle = mem->access(req);
            // PUTS (clean writeback) is not a real access -- charge no hop.
            if (req.type != PUTS) respCycle += hopCycles;
            return respCycle;
        }

        const char* getName() { return name.c_str(); }

        void initStats(AggregateStat* parentStat) { mem->initStats(parentStat); }
};

/**
 * System-level address router for multi-device configurations.
 * Routes memory requests to per-device MCs based on address range.
 * Adds system network latency based on topology (no local bypass).
 */
class SystemAddressRouter : public MemObject {
    public:
        struct DeviceEntry {
            uint64_t addrStart;       // line address range start (inclusive)
            uint64_t addrEnd;         // line address range end (exclusive)
            MemObject* mc;            // MC for this device
            uint32_t networkNodeId;   // node position in system network topology
        };

        SystemAddressRouter(const g_vector<DeviceEntry>& _devices,
                             const uint32_t* _coreNodeMap, uint32_t _numCores,
                             uint32_t _perHopLatency, uint32_t _serializationCycles,
                             uint32_t _model, const double* _hopMatrix, uint32_t _numNodes,
                             int _totalVCs, uint32_t _linkWidthBits, const char* _name,
                             GarnetNetwork* _garnet = nullptr)
            : devices_(_devices), numCores_(_numCores),
              perHopLatency_(_perHopLatency), serializationCycles_(_serializationCycles),
              model_(_model), numNodes_(_numNodes), totalVCs_(_totalVCs),
              linkWidthBits_(_linkWidthBits > 0 ? _linkWidthBits : 512),
              systemGarnet_(_garnet),
              name_(_name), smoothedRate_(0.0), curAccesses_(0), lastPhase_(0)
        {
            for (uint32_t i = 0; i < _numCores && i < 2048; i++)
                coreNodeMap_[i] = _coreNodeMap[i];
            for (uint32_t i = 0; i < _numNodes * _numNodes && i < 256; i++)
                hopMatrix_[i] = _hopMatrix[i];
            futex_init(&updateLock_);
        }

        uint64_t access(MemReq& req) {
            // Find target device by address
            uint32_t targetIdx = 0;
            for (uint32_t i = 0; i < devices_.size(); i++) {
                if (req.lineAddr >= devices_[i].addrStart &&
                    req.lineAddr < devices_[i].addrEnd) {
                    targetIdx = i;
                    break;
                }
            }

            // MC access latency
            uint64_t respCycle = devices_[targetIdx].mc->access(req);

            // System network latency (topology-based, no bypass)
            uint32_t srcNode = (req.srcId < numCores_) ? coreNodeMap_[req.srcId] : 0;
            uint32_t dstNode = devices_[targetIdx].networkNodeId;

            if (srcNode != dstNode && numNodes_ > 1) {
                uint64_t netLat = 0;

                if (model_ == 2 && systemGarnet_) {
                    // Detailed: use Garnet cycle-accurate simulation
                    // Save/restore global tick to avoid interference with device Garnet
                    char srcName[32], dstName[32];
                    snprintf(srcName, sizeof(srcName), "sys-node-%u", srcNode);
                    snprintf(dstName, sizeof(dstName), "sys-node-%u", dstNode);
#ifdef HAVE_GARNET
                    uint64_t savedTick = systemGarnet_->saveAndSetTick();
                    uint32_t rtt = systemGarnet_->getRTT(srcName, dstName);
                    systemGarnet_->restoreTick(savedTick);
                    netLat = rtt;
#else
                    // Fallback to simple model if Garnet not compiled
                    netLat = systemGarnet_->getRTT(srcName, dstName);
#endif
                } else {
                    // SIMPLE: topology-based hop model + M/D/1 queuing
                    double hops = hopMatrix_[srcNode * numNodes_ + dstNode];
                    netLat = static_cast<uint64_t>(
                        std::ceil(hops * perHopLatency_ + serializationCycles_));

                    if (netLat > 0 && totalVCs_ > 0) {
                        // M/D/1 queuing with VCs as parallel servers
                        futex_lock(&updateLock_);
                        curAccesses_++;
                        // Simple EWMA-based utilization tracking
                        double rho = smoothedRate_ / (totalVCs_ * (1.0 / netLat));
                        if (rho > 0.95) rho = 0.95;
                        if (rho > 0.0) {
                            netLat = static_cast<uint64_t>(
                                netLat * (1.0 + 0.5 * rho / (1.0 - rho)));
                        }
                        futex_unlock(&updateLock_);
                    }
                }

                respCycle += netLat;
            }

            profAccesses_.inc();
            profTotalLat_.inc(respCycle - req.cycle);
            return respCycle;
        }

        const char* getName() { return name_.c_str(); }

        void initStats(AggregateStat* parentStat) {
            // Use a non-regular aggregate to hold heterogeneous sub-device stats
            AggregateStat* routerStats = new AggregateStat();
            routerStats->init(name_.c_str(), "System address router stats");
            profAccesses_.init("acc", "Total accesses"); routerStats->append(&profAccesses_);
            profTotalLat_.init("lat", "Total latency"); routerStats->append(&profTotalLat_);
            // Nest per-device MC stats under the router aggregate
            for (auto& dev : devices_) {
                dev.mc->initStats(routerStats);
            }
            parentStat->append(routerStats);
        }

        // Set system Garnet network (called after construction when model==2)
        void setSystemGarnet(GarnetNetwork* garnet) { systemGarnet_ = garnet; }

        // Phase update for M/D/1 smoothing (always active in SIMPLE model)
        void phaseUpdate(uint64_t phase) {
            futex_lock(&updateLock_);
            if (phase > lastPhase_) {
                double rate = (double)curAccesses_ / (phase - lastPhase_);
                smoothedRate_ = 0.5 * smoothedRate_ + 0.5 * rate;
                curAccesses_ = 0;
                lastPhase_ = phase;
            }
            futex_unlock(&updateLock_);
        }

    private:
        g_vector<DeviceEntry> devices_;
        uint32_t coreNodeMap_[2048] = {};
        uint32_t numCores_;
        uint32_t perHopLatency_;
        uint32_t serializationCycles_;
        uint32_t model_;          // 0=SIMPLE (includes M/D/1), 2=DETAILED
        double hopMatrix_[256] = {};  // up to 16x16 nodes
        uint32_t numNodes_;
        int totalVCs_;
        uint32_t linkWidthBits_;
        GarnetNetwork* systemGarnet_;  // system Garnet (model==2 only, nullptr otherwise)
        g_string name_;

        // M/D/1 state
        double smoothedRate_;
        uint32_t curAccesses_;
        uint64_t lastPhase_;
        lock_t updateLock_;

        Counter profAccesses_;
        Counter profTotalLat_;
};

#endif  // MEM_CTRLS_H_
