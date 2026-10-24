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

#ifndef WEAVE_MD1_MEM_H_
#define WEAVE_MD1_MEM_H_

#include "mem_ctrls.h"
#include "timing_event.h"
#include "zsim.h"

/* Weave-phase memory controller based on the SimpleMemory (M/D/1) controller.
 * Returns bounded latency immediately in the bound phase, pushes real M/D/1
 * latency to the event recorder for weave-phase replay.
 */

//Weave-phase event
class WeaveMemAccEvent : public TimingEvent {
    private:
        uint32_t lat;

    public:
        WeaveMemAccEvent(uint32_t _lat, int32_t domain, uint32_t preDelay, uint32_t postDelay) :  TimingEvent(preDelay, postDelay, domain), lat(_lat) {}

        void simulate(uint64_t startCycle) {
            done(startCycle + lat);
        }
};

// WeaveSimpleMemory: phase-aware wrapper around SimpleMemory (M/D/1 always active)
class WeaveSimpleMemory : public SimpleMemory {
    private:
        const uint32_t zeroLoadLatency;
        const uint32_t boundLatency;
        const uint32_t domain;
        uint32_t preDelay, postDelay;

    public:
        WeaveSimpleMemory(uint32_t lineSize, uint32_t megacyclesPerSecond, uint32_t megabytesPerSecond, uint32_t _zeroLoadLatency, uint32_t _boundLatency, uint32_t _domain, g_string& _name) :
            SimpleMemory(lineSize, megacyclesPerSecond, megabytesPerSecond, _zeroLoadLatency, _name), zeroLoadLatency(_zeroLoadLatency), boundLatency(_boundLatency), domain(_domain)
        {
            preDelay = zeroLoadLatency/2;
            postDelay = zeroLoadLatency - preDelay;
        }

        uint64_t access(MemReq& req) {
            bool byReplay = false;
            uint64_t realRespCycle = SimpleMemory::accessPriced(req, &byReplay);
            uint32_t realLatency = realRespCycle - req.cycle;

            /* 1.12.18 (ruling 17 / R0605): a replay-priced access carries its
             * whole measured latency (queueing at the bank included) in the
             * bound phase; a row hit can be shorter than the tRCD + tCAS
             * zero-load latency, so it is not a lower bound to add an M/D/1
             * excess to. The weave event spans the same latency and adds
             * nothing (the contention is the replay's, one epoch back). */
            if (byReplay) {
                if (zinfo->eventRecorders[req.srcId]) {
                    const uint32_t pre = realLatency / 2;
                    WeaveMemAccEvent* memEv = new (zinfo->eventRecorders[req.srcId]) WeaveMemAccEvent(0, domain, pre, realLatency - pre);
                    memEv->setMinStartCycle(req.cycle);
                    TimingRecord tr = {req.lineAddr, req.cycle, realRespCycle, req.type, memEv, memEv};
                    zinfo->eventRecorders[req.srcId]->pushRecord(tr);
                }
                return realRespCycle;
            }

            uint64_t respCycle = req.cycle + ((req.type == PUTS)? 0 : boundLatency);
            assert(realRespCycle >= respCycle);
            assert(req.type == PUTS || realLatency >= zeroLoadLatency);

            if ((req.type != PUTS) && zinfo->eventRecorders[req.srcId]) {
                WeaveMemAccEvent* memEv = new (zinfo->eventRecorders[req.srcId]) WeaveMemAccEvent(realLatency-zeroLoadLatency, domain, preDelay, postDelay);
                memEv->setMinStartCycle(req.cycle);
                TimingRecord tr = {req.lineAddr, req.cycle, respCycle, req.type, memEv, memEv};
                zinfo->eventRecorders[req.srcId]->pushRecord(tr);
            }

            return respCycle;
        }
};

#endif  // WEAVE_MD1_MEM_H_
