/*****************************************************************************
 *                                McPAT
 *                      SOFTWARE LICENSE AGREEMENT
 *            Copyright 2012 Hewlett-Packard Development Company, L.P.
 *                          All Rights Reserved
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
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.”
 *
 ***************************************************************************/


#include "interconnect.h"
#include "wire.h"
#include <assert.h>
#include <iostream>
#include <sstream>
#include <string>
#include "globalvar.h"

interconnect::interconnect(
    string name_,
    enum Device_ty device_ty_,
	double base_w, double base_h,
    int data_w, double len,const InputParameter *configure_interface,
    int start_wiring_level_,
    bool pipelinable_ ,
    double route_over_perc_ ,
    bool opt_local_,
    enum Core_type core_ty_,
    enum Wire_type wire_model,
    double width_s, double space_s,
    DeviceType *dt
)
 :name(name_),
  device_ty(device_ty_),
  in_rise_time(0),
  out_rise_time(0),
  base_width(base_w),
  base_height(base_h),
  data_width(data_w),
  wt(wire_model),
  width_scaling(width_s),
  space_scaling(space_s),
  start_wiring_level(start_wiring_level_),
  length(len),
  //interconnect_latency(1e-12),
  //interconnect_throughput(1e-12),
  opt_local(opt_local_),
  core_ty(core_ty_),
  pipelinable(pipelinable_),
  route_over_perc(route_over_perc_),
  deviceType(dt)
{

  wt = Global;
  l_ip=*configure_interface;

  try {
    local_result = init_interface(&l_ip);
  } catch (...) {
    /* Wire/CACTI init failed for this technology -- power stays at zero.
     * PIMID 1.11.90: no longer silent. Printed, counted in the substitution
     * ledger (basic_components.h), refused by PIMID unless
     * PIMID_ALLOW_ARRAY_CLAMP=1. */
    pimid_note_substitution(PIMID_SUBST_LINK, name,
        "[mcpat] WARNING: link " + name + " (" + std::to_string(data_width)
        + " b, " + std::to_string(length) + " um): CACTI interface init threw,"
        " left at 0 power and 0 area");
    return;
  }

  max_unpipelined_link_delay = 0; //TODO
  min_w_nmos = g_tp.min_w_nmos_;
  min_w_pmos = deviceType->n_to_p_eff_curr_drv_ratio * min_w_nmos;



  latency               = l_ip.latency;
  throughput            = l_ip.throughput;
  latency_overflow=false;
  throughput_overflow=false;

  /*
   * TODO: Add wiring option from semi-global to global automatically
   * And directly jump to global if semi-global cannot satisfy timing
   * Fat wires only available for global wires, thus
   * if signal wiring layer starts from semi-global,
   * the next layer up will be global, i.e., semi-global does
   * not have fat wires.
   */
  try {
  if (pipelinable == false)
  //Non-pipelinable wires, such as bypass logic, care latency
  {
	  compute();
	  if (opt_for_clk && opt_local)
	  {
		  while (delay > latency && width_scaling<3.0)
		  {
			  width_scaling *= 2;
			  space_scaling *= 2;
			  Wire winit(width_scaling, space_scaling);
			  compute();
		  }
		  if (delay > latency)
		  {
			  latency_overflow=true;
		  }
	  }
  }
  else //Pipelinable wires, such as bus, does not care latency but throughput
  {
	  /*
	   * TODO: Add pipe regs power, area, and timing;
	   * Pipelinable wires optimize latency first.
	   */
	  compute();
	  if (opt_for_clk && opt_local)
	  {
		  while (delay > throughput && width_scaling<3.0)
		  {
			  width_scaling *= 2;
			  space_scaling *= 2;
			  Wire winit(width_scaling, space_scaling);
			  compute();
		  }
		  if (delay > throughput)
			  // insert pipeline stages
		  {
			  num_pipe_stages = (int)ceil(delay/throughput);
			  assert(num_pipe_stages>0);
			  delay = delay/num_pipe_stages + num_pipe_stages*0.05*delay;
		  }
	  }
  }
  } catch (...) {
    /* Wire construction failed -- power stays at zero. PIMID 1.11.90:
     * printed and counted, as above. */
    pimid_note_substitution(PIMID_SUBST_LINK, name,
        "[mcpat] WARNING: link " + name + " (" + std::to_string(data_width)
        + " b, " + std::to_string(length) + " um): wire construction threw,"
        " left at 0 power");
    return;
  }

  power_bit = power;
  power.readOp.dynamic *= data_width;
  power.readOp.leakage *= data_width;
  power.readOp.gate_leakage *= data_width;
  area.set_area(area.get_area()*data_width);
  no_device_under_wire_area.h *= data_width;

  if (latency_overflow==true)
  		cout<< "Warning: "<< name <<" wire structure cannot satisfy latency constraint." << endl;

  /* Upstream asserted all three > 0 here; this fork returned early instead,
   * which also skips the sckRation scaling, the long-channel leakage and the
   * power-gating endpoints below -- a link at zero (or partial) power with
   * no message. PIMID 1.11.90: `!(x > 0)` so a NaN fails the test too, and
   * the early return is printed and counted in the substitution ledger
   * (basic_components.h); PIMID refuses the run unless
   * PIMID_ALLOW_ARRAY_CLAMP=1. A link with no bits or no length is exempt
   * only when all three are exactly zero: zero is then the computed answer
   * for a wire that does not exist, not a substitution. */
  if (pimid_fault("link")) power.readOp.dynamic = NAN;
  if (!(power.readOp.dynamic > 0) || !(power.readOp.leakage > 0) || !(power.readOp.gate_leakage > 0))
  {
      const bool absent = (data_width <= 0 || length <= 0) &&
                          power.readOp.dynamic == 0 &&
                          power.readOp.leakage == 0 &&
                          power.readOp.gate_leakage == 0;
      if (!absent) {
          std::ostringstream os;
          os << "[mcpat] WARNING: link " << name << " (" << data_width
             << " b, " << length << " um) priced at dynamic "
             << power.readOp.dynamic << ", leakage " << power.readOp.leakage
             << ", gate leakage " << power.readOp.gate_leakage
             << " -- not all positive; the circuit-switching, long-channel"
                " and power-gating scaling were skipped";
          pimid_note_substitution(PIMID_SUBST_LINK, name, os.str());
      }
      return;
  }

  double long_channel_device_reduction = longer_channel_device_reduction(device_ty,core_ty);
  double pg_reduction = power_gating_leakage_reduction(false);//

  double sckRation = g_tp.sckt_co_eff;
  power.readOp.dynamic *= sckRation;
  power.writeOp.dynamic *= sckRation;
  power.searchOp.dynamic *= sckRation;

  power.readOp.longer_channel_leakage =
	  power.readOp.leakage*long_channel_device_reduction;

  power.readOp.power_gated_leakage =
	  power.readOp.leakage*pg_reduction;

  power.readOp.power_gated_with_long_channel_leakage =
	  power.readOp.power_gated_leakage*long_channel_device_reduction;

  if (pipelinable)//Only global wires has the option to choose whether routing over or not
	  area.set_area(area.get_area()*route_over_perc + no_device_under_wire_area.get_area()*(1-route_over_perc));

  Wire wreset();
}



void
interconnect::compute()
{

  Wire *wtemp1 = 0;
  wtemp1 = new Wire(wt, length, 1, width_scaling, space_scaling);
  delay = wtemp1->delay;
  power.readOp.dynamic = wtemp1->power.readOp.dynamic;
  power.readOp.leakage = wtemp1->power.readOp.leakage;
  power.readOp.gate_leakage = wtemp1->power.readOp.gate_leakage;

  area.set_area(wtemp1->area.get_area());
  no_device_under_wire_area.h =  (wtemp1->wire_width + wtemp1->wire_spacing);
  no_device_under_wire_area.w = length;

  if (wtemp1)
   delete wtemp1;

}

void interconnect::leakage_feedback(double temperature)//TODO: add code for processing power gating
{
  l_ip.temp = (unsigned int)round(temperature/10.0)*10;
  uca_org_t init_result = init_interface(&l_ip); // init_result is dummy

  compute();

  power_bit = power;
  power.readOp.dynamic *= data_width;
  power.readOp.leakage *= data_width;
  power.readOp.gate_leakage *= data_width;

  assert(power.readOp.dynamic > 0);
  assert(power.readOp.leakage > 0);
  assert(power.readOp.gate_leakage > 0);

  double long_channel_device_reduction = longer_channel_device_reduction(device_ty,core_ty);

  double sckRation = g_tp.sckt_co_eff;
  power.readOp.dynamic *= sckRation;
  power.writeOp.dynamic *= sckRation;
  power.searchOp.dynamic *= sckRation;

  power.readOp.longer_channel_leakage = power.readOp.leakage*long_channel_device_reduction;
}

