/*
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
 * Copyright (c) 2020 Inria
 * Copyright (c) 2016 Georgia Institute of Technology
 * Copyright (c) 2008 Princeton University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "NetworkLink.hh"

#include "gem5_compat/base/trace.hh"
#include "gem5_compat/debug/RubyNetwork.hh"
#include "CreditLink.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

NetworkLink::NetworkLink(const Params &p)
    : ClockedObject(p), Consumer(this), m_id(p.link_id),
      m_type(NUM_LINK_TYPES_),
      m_latency(p.link_latency), m_link_utilized(0),
      m_virt_nets(p.virt_nets), linkBuffer(),
      link_consumer(nullptr), link_srcQueue(nullptr)
{
    int num_vnets = (p.supported_vnets).size();
    mVnets.resize(num_vnets);
    bitWidth = p.width;
    m_vc_depth = p.vc_depth;   // (1.12.233)
    m_vc_depth_exact = p.vc_depth_exact;   // (1.12.233)
    for (int i = 0; i < num_vnets; i++) {
        mVnets[i] = p.supported_vnets[i];
    }
}

void
NetworkLink::setLinkConsumer(Consumer *consumer)
{
    link_consumer = consumer;
}

void
NetworkLink::setVcsPerVnet(uint32_t consumerVcs)
{
    m_vc_load.resize(m_virt_nets * consumerVcs);
}

void
NetworkLink::setSourceQueue(flitBuffer *src_queue, ClockedObject *srcClockObj)
{
    link_srcQueue = src_queue;
    src_object = srcClockObj;
}

void
NetworkLink::wakeup()
{
    DPRINTF(RubyNetwork, "Woke up to transfer flits from %s\n",
        src_object->name());
    assert(link_srcQueue != nullptr);
    assert(curTick() == clockEdge());
    // Per-link bandwidth: number of network cycles a flit OCCUPIES this link
    // before the next flit can be pulled. bitWidth carries the per-layer link
    // bandwidth in bits/network-cycle; a flit of m_width bits occupies the
    // link for ceil(m_width/bitWidth) cycles, so a narrower (lower-bandwidth)
    // link delivers fewer flits/cycle -> real throughput contention + credit
    // backpressure. Applies to DATA (network) links only; credit links
    // (m_type == NUM_LINK_TYPES_) stay at 1 cycle.
    if (link_srcQueue->isReady(curTick())) {
        flit *t_flit = link_srcQueue->getTopFlit();
        DPRINTF(RubyNetwork, "Transmission will finish at %ld :%s\n",
                clockEdge(m_latency), *t_flit);
        if (m_type != NUM_LINK_TYPES_) {
            // Per-link bandwidth is NOT modeled by link occupancy any more: the
            // old approach held the upstream VC for K cycles and deadlocked
            // converging hubs (single-channel DRAM: 16 banks -> one channel-DQ).
            // Bandwidth is now modeled by NI-side FLIT COUNT (a lower-bandwidth
            // tech splits each access into more flits); links pass one flit per
            // cycle and never pin a VC. Only vnet membership is asserted.
            assert((std::find(mVnets.begin(), mVnets.end(),
                t_flit->get_vnet()) != mVnets.end()) ||
                (mVnets.size() == 0));
        }
        t_flit->set_time(clockEdge(m_latency));
        linkBuffer.insert(t_flit);
        link_consumer->scheduleEventAbsolute(clockEdge(m_latency));
        m_link_utilized++;
        m_vc_load[t_flit->get_vc()]++;
    }

    if (!link_srcQueue->isEmpty()) {
        scheduleEvent(Cycles(1));
    }
}

void
NetworkLink::resetStats()
{
    for (int i = 0; i < m_vc_load.size(); i++) {
        m_vc_load[i] = 0;
    }

    m_link_utilized = 0;
}

bool
NetworkLink::functionalRead(Packet *pkt, WriteMask &mask)
{
    return linkBuffer.functionalRead(pkt, mask);
}

uint32_t
NetworkLink::functionalWrite(Packet *pkt)
{
    return linkBuffer.functionalWrite(pkt);
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
