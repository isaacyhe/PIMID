/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
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


#ifndef __MEM_RUBY_NETWORK_GARNET_0_OUTVCSTATE_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_OUTVCSTATE_HH__

#include "CommonTypes.hh"
#include "GarnetNetwork.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

class OutVcState
{
  public:
    OutVcState(int id, GarnetNetwork *network_ptr, uint32_t consumerVcs);

    int get_credit_count()          { return m_credit_count; }
    VC_state_type getState() const  { return m_vc_state; }
    Tick getStateTime() const       { return m_time; }
    inline bool has_credit()       { return (m_credit_count > 0); }
    // (1.12.233): the receiving buffer is d deep (never below the network's floor)
    void setDepth(int d) { if (d > m_max_credit_count) { m_max_credit_count = d; m_credit_count = d; } }
    // (1.12.233): an explicit depth setting, as given (set before any traffic)
    void setDepthExact(int d) { if (d > 0) { m_max_credit_count = d; m_credit_count = d; } }
    void increment_credit();
    void decrement_credit();

    inline bool
    isInState(VC_state_type state, Tick request_time)
    {
        return ((m_vc_state == state) && (request_time >= m_time) );
    }
    inline void
    setState(VC_state_type state, Tick time)
    {
        m_vc_state = state;
        m_time = time;
    }

    // Reset to initial state (IDLE, full credits).
    // Used during network state reset between phases.
    void resetState()
    {
        m_vc_state = IDLE_;
        m_time = 0;
        m_credit_count = m_max_credit_count;
    }

  private:
    int m_id ;
    Tick m_time;
    VC_state_type m_vc_state;
    int m_credit_count;
    int m_max_credit_count;
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif //__MEM_RUBY_NETWORK_GARNET_0_OUTVCSTATE_HH__
