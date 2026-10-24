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

#include "mem_ctrls.h"
#include "dram_epoch_replay.h"   // 1.12.18 (ruling 17 / R0605)
#include "zsim.h"

SimpleMemory::SimpleMemory(uint32_t requestSize, uint32_t megacyclesPerSecond, uint32_t megabytesPerSecond, uint32_t _zeroLoadLatency, g_string& _name)
    : zeroLoadLatency(_zeroLoadLatency), name(_name)
{
    lastPhase = 0;

    double bytesPerCycle = ((double)megabytesPerSecond)/((double)megacyclesPerSecond);
    maxRequestsPerCycle = bytesPerCycle/requestSize;
    assert(maxRequestsPerCycle > 0.0);

    zeroLoadLatency = _zeroLoadLatency;

    smoothedPhaseAccesses = 0.0;
    curPhaseAccesses = 0;
    curLatency = zeroLoadLatency;

    futex_init(&updateLock);
}

void SimpleMemory::updateLatency() {
    uint32_t phaseCycles = (zinfo->numPhases - lastPhase)*(zinfo->phaseLength);
    if (phaseCycles < 10000) return; //Skip with short phases

    smoothedPhaseAccesses =  (curPhaseAccesses*0.5) + (smoothedPhaseAccesses*0.5);
    double requestsPerCycle = smoothedPhaseAccesses/((double)phaseCycles);
    double load = requestsPerCycle/maxRequestsPerCycle;

    //Clamp load
    if (load > 0.95) {
        load = 0.95;
        profClampedLoads.inc();
    }

    double latMultiplier = 1.0 + 0.5*load/(1.0 - load); //See Pollancek-Khinchine formula
    curLatency = (uint32_t)(latMultiplier*zeroLoadLatency);

    uint32_t intLoad = (uint32_t)(load*100.0);
    profLoad.inc(intLoad);
    profUpdates.inc();

    curPhaseAccesses = 0;
    __sync_synchronize();
    lastPhase = zinfo->numPhases;
}

void SimpleMemory::flushWriteback(Address lineAddr, uint64_t cycle, uint32_t srcId) {   // 1.12.18 (bug 7)
    if (hostReplay) (void)hostReplay->hostAccess(hostReplayMi, lineAddr, cycle, srcId, true, true);   // 1.12.18 (R2): the flush passes the wall-free clock
    profWrites.atomicInc();
    zinfo->pgres.hostMC.touch(zinfo->numPhases); zinfo->pgres.hostUnion.touch(zinfo->numPhases);
}

uint64_t SimpleMemory::access(MemReq& req) {
    return accessPriced(req, nullptr);
}

uint64_t SimpleMemory::accessPriced(MemReq& req, bool* byReplay) {
    if (byReplay) *byReplay = false;
    if (zinfo->numPhases > lastPhase) {
        futex_lock(&updateLock);
        //Recheck, someone may have updated already
        if (zinfo->numPhases > lastPhase) {
            updateLatency();
        }
        futex_unlock(&updateLock);
    }

    /* 1.12.18 (ruling 17 / R0605): the replayed controller's measured service
     * latency of this interface (host cycles: bank, row hit / miss / conflict,
     * queueing at the bank, refresh, turnaround, the burst) plus the host-path
     * adder replaces tRCD + tCAS + adder + the M/D/1 wait; the zero-load DRAM
     * time is not added again. A clean writeback (PUTS) is not an access. */
    uint32_t lat = curLatency;
    if (hostReplay && req.type != PUTS) {
        const uint32_t v = hostReplay->hostAccess(hostReplayMi, req.lineAddr, req.cycle, req.srcId, req.type == PUTX);
        if (v > 0) {
            lat = hostPathCycles + v;
            if (byReplay) *byReplay = true;
        }
    }

    switch (req.type) {
        case PUTX:
            //Dirty wback
            profWrites.atomicInc();
            profTotalWrLat.atomicInc(lat);   // 1.12.18: the latency charged
            __sync_fetch_and_add(&curPhaseAccesses, 1);
            /* 1.11.18 (audit go-through): a dirty writeback IS memory traffic
             * -- the controller is busy. 1.11.8 marked only GETS/GETX, so a
             * write-heavy phase could be credited as idle and power-gated.
             * (PUTS falls through below without marking: a clean writeback is
             * explicitly not a real access here.) */
            zinfo->pgres.hostMC.touch(zinfo->numPhases); zinfo->pgres.hostUnion.touch(zinfo->numPhases);
            //Note no break
        case PUTS:
            //Not a real access -- memory must treat clean wbacks as if they never happened.
            *req.state = I;
            break;
        case GETS:
            profReads.atomicInc();
            profTotalRdLat.atomicInc(lat);   // 1.12.18: the latency charged
            __sync_fetch_and_add(&curPhaseAccesses, 1);
            zinfo->pgres.hostMC.touch(zinfo->numPhases); zinfo->pgres.hostUnion.touch(zinfo->numPhases);  // 1.11.8 PG residency
            *req.state = req.is(MemReq::NOEXCL)? S : E;
            break;
        case GETX:
            profReads.atomicInc();
            profTotalRdLat.atomicInc(lat);   // 1.12.18: the latency charged
            __sync_fetch_and_add(&curPhaseAccesses, 1);
            zinfo->pgres.hostMC.touch(zinfo->numPhases); zinfo->pgres.hostUnion.touch(zinfo->numPhases);  // 1.11.8 PG residency
            *req.state = M;
            break;

        default: panic("!?");
    }
    return req.cycle + ((req.type == PUTS)? 0 /*PUTS is not a real access*/ : lat);
}
