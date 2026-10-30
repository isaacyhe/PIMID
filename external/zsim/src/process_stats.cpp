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

#include "process_stats.h"
#include "process_tree.h"
#include "scheduler.h"
#include "zsim.h"

ProcessStats::ProcessStats(AggregateStat* parentStat) {
    uint32_t maxProcs = zinfo->lineSize;
    processCycles.resize(maxProcs, 0);
    processInstrs.resize(maxProcs, 0);
    lastCoreCycles.resize(zinfo->numCores, 0);
    lastCoreInstrs.resize(zinfo->numCores, 0);
    lastUpdatePhase = 0;

    auto procCyclesLambda = [this](uint32_t p) { return getProcessCycles(p); };
    auto procCyclesStat = makeLambdaVectorStat(procCyclesLambda, maxProcs);
    procCyclesStat->init("procCycles", "Per-process unhalted core cycles");

    auto procInstrsLambda = [this](uint32_t p) { return getProcessInstrs(p); };
    auto procInstrsStat = makeLambdaVectorStat(procInstrsLambda, maxProcs);
    procInstrsStat->init("procInstrs", "Per-process instructions");

    parentStat->append(procCyclesStat);
    parentStat->append(procInstrsStat);
}

uint64_t ProcessStats::getProcessCycles(uint32_t p) {
    if (unlikely(lastUpdatePhase != zinfo->numPhases)) update();
    assert(p < processCycles.size());
    return processCycles[p];
}

uint64_t ProcessStats::getProcessInstrs(uint32_t p) {
    if (unlikely(lastUpdatePhase != zinfo->numPhases)) update();
    assert(p < processInstrs.size());
    return processInstrs[p];
}

void ProcessStats::notifyDeschedule(uint32_t cid, uint32_t outgoingPid) {
    assert(cid < lastCoreCycles.size());
    assert(outgoingPid < processCycles.size());
    updateCore(cid, outgoingPid);
}

/* Private */

void ProcessStats::updateCore(uint32_t cid, uint32_t p) {
    p = zinfo->procArray[p]->getGroupIdx();

    uint64_t cCycles = zinfo->cores[cid]->getCycles();
    uint64_t cInstrs = zinfo->cores[cid]->getInstrs();

    /* 1.6 thread-MPI: a core's CYCLE counter is NOT monotonic. A rank parking
     * in a transport wait rewinds the core at MPI_COMM_END -- OoO/in-order
     * cores subtract pimidPhantomWait from getCycles(), ALU cores roll curCycle
     * back to the ROI baseline -- so the wall-dependent wait growth is erased
     * and the run stays deterministic (see Core::pimidRewindCycles). This only
     * surfaces in system scope, where a host domain and multi-rank MPI coexist;
     * asserting monotonic cycles here aborted every co-sim MPI run.
     * A device PE's INSTRUCTION counter can go back too: since 1.12.181 a co-sim
     * MPI rank's PE restores its activity counters (instrs among them) to their
     * values at the rank's closing barrier just before the stats are written
     * (Core::restoreActivity, recordProtocolTailStats), so the post-ROI tail is
     * in none of them. A PE sampled here during that tail (its rank descheduled
     * in a tail wait) then reads below its last sample. Both counters mirror a
     * rollback in the process total rather than underflowing the unsigned
     * subtraction. */
    if (likely(cCycles >= lastCoreCycles[cid])) {
        processCycles[p] += cCycles - lastCoreCycles[cid];
    } else {
        uint64_t rewound = lastCoreCycles[cid] - cCycles;
        processCycles[p] = (processCycles[p] > rewound) ? (processCycles[p] - rewound) : 0;
    }
    if (likely(cInstrs >= lastCoreInstrs[cid])) {
        processInstrs[p] += cInstrs - lastCoreInstrs[cid];
    } else {
        uint64_t rewound = lastCoreInstrs[cid] - cInstrs;
        processInstrs[p] = (processInstrs[p] > rewound) ? (processInstrs[p] - rewound) : 0;
    }

    lastCoreCycles[cid] = cCycles;
    lastCoreInstrs[cid] = cInstrs;
}

void ProcessStats::update() {
    assert(lastUpdatePhase < zinfo->numPhases);
    for (uint32_t cid = 0; cid < lastCoreCycles.size(); cid++) {
        uint32_t p = zinfo->sched->getScheduledPid(cid);
        if (p == (uint32_t)-1) continue;
        assert(p < processCycles.size());
        updateCore(cid, p);
    }
    lastUpdatePhase = zinfo->numPhases;
}

