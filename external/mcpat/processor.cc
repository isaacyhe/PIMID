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
#include "common/debug_checks.h"
#include <string.h>
#include <iostream>
#include <stdio.h>
#include <algorithm>
#include <string.h>
#include <cmath>
#include <assert.h>
#include <fstream>
#include <stdexcept>
#include <sstream>   /* PIMID 1.12.18: per-die trunk lines */
#include "parameter.h"
#include "array.h"
#include "const.h"
#include "basic_circuit.h"
#include "XML_Parse.h"
#include "processor.h"
#include "version.h"


Processor::Processor(ParseXML *XML_interface)
:XML(XML_interface),//TODO: using one global copy may have problems.
 mc(0),
 niu(0),
 pcie(0),
 flashcontroller(0)
{
  ClockNetwork::split_enabled = (XML->sys.pimid_clock_split != 0);   /* PIMID 1.12.18 (ruling #4) */
  ClockNetwork::leaf_cluster_sinks = (XML->sys.pimid_clock_leaf_sinks > 0) ? XML->sys.pimid_clock_leaf_sinks : 30;   /* PIMID (1.12.30, F2) */
  /*
   *  placement and routing overhead is 10%, core scales worse than cache 40% is accumulated from 90 to 22nm
   *  There is no point to have heterogeneous memory controller on chip,
   *  thus McPAT only support homogeneous memory controllers.
   */
  int i;
  double pppm_t[4]    = {1,1,1,1};
  set_proc_param();
  if (procdynp.homoCore)
	  numCore = procdynp.numCore==0? 0:1;
  else
	  numCore = procdynp.numCore;

  if (procdynp.homoL2)
	  numL2 = procdynp.numL2==0? 0:1;
  else
	  numL2 = procdynp.numL2;

  if (XML->sys.Private_L2 && numCore != numL2)
  {
	  throw std::runtime_error("McPAT: Number of private L2 does not match number of cores");
  }

  if (procdynp.homoL3)
	  numL3 = procdynp.numL3==0? 0:1;
  else
	  numL3 = procdynp.numL3;

  if (procdynp.homoNOC)
	  numNOC = procdynp.numNOC==0? 0:1;
  else
	  numNOC = procdynp.numNOC;

//  if (!procdynp.homoNOC)
//  {
//	  cout<<"Current McPAT does not support heterogeneous NOC"<<endl;
//      exit(0);
//  }

  if (procdynp.homoL1Dir)
	  numL1Dir = procdynp.numL1Dir==0? 0:1;
  else
	  numL1Dir = procdynp.numL1Dir;

  if (procdynp.homoL2Dir)
	  numL2Dir = procdynp.numL2Dir==0? 0:1;
  else
	  numL2Dir = procdynp.numL2Dir;

  /* PIMID 1.12.18 (user ruling, per-die process): EACH COMPONENT IS PRICED
   * AT THE DIE IT SITS ON. McPAT built one technology per chip
   * (interface_ip / g_tp); every component already copies its own
   * InputParameter and re-initialises CACTI's tables from it
   * (init_interface), and the clock and wire models take the owning
   * component's InputParameter, so the per-die technology is carried by
   * the InputParameter each component is built with:
   *   DRAM die (dram_periph_family 1: cores, caches, controllers, and the
   *     NoC levels marked on_dram_die): interface_ip = core_tech_node /
   *     device_type, then the CACTI comm-dram periphery transform below;
   *   logic die (everything else; every component when the family is 0):
   *     ip_logic_die = interface_ip with pimid_logic_die_node /
   *     pimid_logic_die_device_type when given.
   * Absent both, ip_logic_die IS interface_ip and every number is the
   * one-technology chip's. A NoC instance's own tech_node (its links) still
   * wins inside noc.cc. */
  ip_logic_die = interface_ip;
  logic_die_override = false;
  if (XML->sys.pimid_logic_die_node > 0 && XML->sys.pimid_logic_die_node != (int)interface_ip.F_sz_nm)
  {
	  ip_logic_die.F_sz_nm = XML->sys.pimid_logic_die_node;
	  ip_logic_die.F_sz_um = ip_logic_die.F_sz_nm / 1000;
	  logic_die_override = true;
  }
  if (XML->sys.pimid_logic_die_device_type >= 0 && XML->sys.pimid_logic_die_device_type != XML->sys.device_type)
  {
	  ip_logic_die.data_arr_ram_cell_tech_type    = XML->sys.pimid_logic_die_device_type;
	  ip_logic_die.data_arr_peri_global_tech_type = XML->sys.pimid_logic_die_device_type;
	  ip_logic_die.tag_arr_ram_cell_tech_type     = XML->sys.pimid_logic_die_device_type;
	  ip_logic_die.tag_arr_peri_global_tech_type  = XML->sys.pimid_logic_die_device_type;
	  logic_die_override = true;
  }
  const bool pimid_fam1 = (XML->sys.dram_periph_family == 1);
  /* the InputParameter of a component: on the DRAM die or not */
  auto pimid_ip = [&](bool on_dram) -> InputParameter* {
	  return (logic_die_override && !on_dram) ? &ip_logic_die : &interface_ip;
  };
  auto pimid_noc_ip = [&](int ni) -> InputParameter* {
	  return pimid_ip(pimid_fam1 && XML->sys.NoC[ni].on_dram_die);
  };
  if (logic_die_override)
	  pimid::debugStream(std::cerr) << "[mcpat tech] per-die process: logic die " << ip_logic_die.F_sz_nm << " nm device "
	            << ip_logic_die.data_arr_peri_global_tech_type << "; "
	            << (pimid_fam1 ? "DRAM die (cores, caches, controllers, on-die NoC levels) " : "every component is on the logic die; chip ")
	            << interface_ip.F_sz_nm << " nm device " << interface_ip.data_arr_peri_global_tech_type << std::endl;

  for (i = 0;i < numCore; i++)
  {
		  cores.push_back(new Core(XML,i, pimid_ip(pimid_fam1)));   /* PIMID 1.12.18 (per-die process) */
		  cores[i]->computeEnergy();
		  cores[i]->computeEnergy(false);
		  if (procdynp.homoCore){
			  core.area.set_area(core.area.get_area() + cores[i]->area.get_area()*procdynp.numCore);
			  set_pppm(pppm_t,cores[i]->clockRate*procdynp.numCore, procdynp.numCore,procdynp.numCore,procdynp.numCore);
			  core.power = core.power + cores[i]->power*pppm_t;
			  set_pppm(pppm_t,1/cores[i]->executionTime, procdynp.numCore,procdynp.numCore,procdynp.numCore);
			  core.rt_power = core.rt_power + cores[i]->rt_power*pppm_t;
			  area.set_area(area.get_area() + core.area.get_area());//placement and routing overhead is 10%, core scales worse than cache 40% is accumulated from 90 to 22nm
			  power = power  + core.power;
			  rt_power = rt_power  + core.rt_power;
		  }
		  else{
			  core.area.set_area(core.area.get_area() + cores[i]->area.get_area());
			  area.set_area(area.get_area() + cores[i]->area.get_area());//placement and routing overhead is 10%, core scales worse than cache 40% is accumulated from 90 to 22nm

			  set_pppm(pppm_t,cores[i]->clockRate, 1, 1, 1);
			  core.power = core.power + cores[i]->power*pppm_t;
			  power = power  + cores[i]->power*pppm_t;

			  set_pppm(pppm_t,1/cores[i]->executionTime, 1, 1, 1);
			  core.rt_power = core.rt_power + cores[i]->rt_power*pppm_t;
			  rt_power = rt_power  + cores[i]->rt_power*pppm_t;
		  }
  }

  if (!XML->sys.Private_L2)
  {
  if (numL2 >0)
	  for (i = 0;i < numL2; i++)
	  {
		  l2array.push_back(new SharedCache(XML,i, pimid_ip(pimid_fam1)));
		  l2array[i]->computeEnergy();
		  l2array[i]->computeEnergy(false);
		  if (procdynp.homoL2){
			  l2.area.set_area(l2.area.get_area() + l2array[i]->area.get_area()*procdynp.numL2);
			  set_pppm(pppm_t,l2array[i]->cachep.clockRate*procdynp.numL2, procdynp.numL2,procdynp.numL2,procdynp.numL2);
			  l2.power = l2.power + l2array[i]->power*pppm_t;
			  set_pppm(pppm_t,1/l2array[i]->cachep.executionTime, procdynp.numL2,procdynp.numL2,procdynp.numL2);
			  l2.rt_power = l2.rt_power + l2array[i]->rt_power*pppm_t;
			  area.set_area(area.get_area() + l2.area.get_area());//placement and routing overhead is 10%, l2 scales worse than cache 40% is accumulated from 90 to 22nm
			  power = power  + l2.power;
			  rt_power = rt_power  + l2.rt_power;
		  }
		  else{
			  l2.area.set_area(l2.area.get_area() + l2array[i]->area.get_area());
			  area.set_area(area.get_area() + l2array[i]->area.get_area());//placement and routing overhead is 10%, l2 scales worse than cache 40% is accumulated from 90 to 22nm

			  set_pppm(pppm_t,l2array[i]->cachep.clockRate, 1, 1, 1);
			  l2.power = l2.power + l2array[i]->power*pppm_t;
			  power = power  + l2array[i]->power*pppm_t;;
			  set_pppm(pppm_t,1/l2array[i]->cachep.executionTime, 1, 1, 1);
			  l2.rt_power = l2.rt_power + l2array[i]->rt_power*pppm_t;
			  rt_power = rt_power  + l2array[i]->rt_power*pppm_t;
		  }
	  }
  }

  if (numL3 >0)
	  for (i = 0;i < numL3; i++)
	  {
		  l3array.push_back(new SharedCache(XML,i, pimid_ip(pimid_fam1), L3));
		  l3array[i]->computeEnergy();
		  l3array[i]->computeEnergy(false);
		  if (procdynp.homoL3){
			  l3.area.set_area(l3.area.get_area() + l3array[i]->area.get_area()*procdynp.numL3);
			  set_pppm(pppm_t,l3array[i]->cachep.clockRate*procdynp.numL3, procdynp.numL3,procdynp.numL3,procdynp.numL3);
			  l3.power = l3.power + l3array[i]->power*pppm_t;
			  set_pppm(pppm_t,1/l3array[i]->cachep.executionTime, procdynp.numL3,procdynp.numL3,procdynp.numL3);
              l3.rt_power = l3.rt_power + l3array[i]->rt_power*pppm_t;
			  area.set_area(area.get_area() + l3.area.get_area());//placement and routing overhead is 10%, l3 scales worse than cache 40% is accumulated from 90 to 22nm
			  power = power  + l3.power;
			  rt_power = rt_power  + l3.rt_power;

		  }
		  else{
			  l3.area.set_area(l3.area.get_area() + l3array[i]->area.get_area());
			  area.set_area(area.get_area() + l3array[i]->area.get_area());//placement and routing overhead is 10%, l3 scales worse than cache 40% is accumulated from 90 to 22nm
			  set_pppm(pppm_t,l3array[i]->cachep.clockRate, 1, 1, 1);
			  l3.power = l3.power + l3array[i]->power*pppm_t;
			  power = power  + l3array[i]->power*pppm_t;
			  set_pppm(pppm_t,1/l3array[i]->cachep.executionTime, 1, 1, 1);
              l3.rt_power = l3.rt_power + l3array[i]->rt_power*pppm_t;
              rt_power = rt_power  + l3array[i]->rt_power*pppm_t;

		  }
	  }
  if (numL1Dir >0)
	  for (i = 0;i < numL1Dir; i++)
	  {
		  l1dirarray.push_back(new SharedCache(XML,i, pimid_ip(pimid_fam1), L1Directory));
		  l1dirarray[i]->computeEnergy();
		  l1dirarray[i]->computeEnergy(false);
		  if (procdynp.homoL1Dir){
			  l1dir.area.set_area(l1dir.area.get_area() + l1dirarray[i]->area.get_area()*procdynp.numL1Dir);
			  set_pppm(pppm_t,l1dirarray[i]->cachep.clockRate*procdynp.numL1Dir, procdynp.numL1Dir,procdynp.numL1Dir,procdynp.numL1Dir);
			  l1dir.power = l1dir.power + l1dirarray[i]->power*pppm_t;
			  set_pppm(pppm_t,1/l1dirarray[i]->cachep.executionTime, procdynp.numL1Dir,procdynp.numL1Dir,procdynp.numL1Dir);
              l1dir.rt_power = l1dir.rt_power + l1dirarray[i]->rt_power*pppm_t;
			  area.set_area(area.get_area() + l1dir.area.get_area());//placement and routing overhead is 10%, l1dir scales worse than cache 40% is accumulated from 90 to 22nm
			  power = power  + l1dir.power;
			  rt_power = rt_power  + l1dir.rt_power;

		  }
		  else{
			  l1dir.area.set_area(l1dir.area.get_area() + l1dirarray[i]->area.get_area());
			  area.set_area(area.get_area() + l1dirarray[i]->area.get_area());
			  set_pppm(pppm_t,l1dirarray[i]->cachep.clockRate, 1, 1, 1);
			  l1dir.power = l1dir.power + l1dirarray[i]->power*pppm_t;
			  power = power  + l1dirarray[i]->power;
			  set_pppm(pppm_t,1/l1dirarray[i]->cachep.executionTime, 1, 1, 1);
              l1dir.rt_power = l1dir.rt_power + l1dirarray[i]->rt_power*pppm_t;
			  rt_power = rt_power  + l1dirarray[i]->rt_power;
		  }
	  }

  if (numL2Dir >0)
	  for (i = 0;i < numL2Dir; i++)
	  {
		  l2dirarray.push_back(new SharedCache(XML,i, pimid_ip(pimid_fam1), L2Directory));
		  l2dirarray[i]->computeEnergy();
		  l2dirarray[i]->computeEnergy(false);
		  if (procdynp.homoL2Dir){
			  l2dir.area.set_area(l2dir.area.get_area() + l2dirarray[i]->area.get_area()*procdynp.numL2Dir);
			  set_pppm(pppm_t,l2dirarray[i]->cachep.clockRate*procdynp.numL2Dir, procdynp.numL2Dir,procdynp.numL2Dir,procdynp.numL2Dir);
			  l2dir.power = l2dir.power + l2dirarray[i]->power*pppm_t;
			  set_pppm(pppm_t,1/l2dirarray[i]->cachep.executionTime, procdynp.numL2Dir,procdynp.numL2Dir,procdynp.numL2Dir);
              l2dir.rt_power = l2dir.rt_power + l2dirarray[i]->rt_power*pppm_t;
			  area.set_area(area.get_area() + l2dir.area.get_area());//placement and routing overhead is 10%, l2dir scales worse than cache 40% is accumulated from 90 to 22nm
			  power = power  + l2dir.power;
			  rt_power = rt_power  + l2dir.rt_power;

		  }
		  else{
			  l2dir.area.set_area(l2dir.area.get_area() + l2dirarray[i]->area.get_area());
			  area.set_area(area.get_area() + l2dirarray[i]->area.get_area());
			  set_pppm(pppm_t,l2dirarray[i]->cachep.clockRate, 1, 1, 1);
			  l2dir.power = l2dir.power + l2dirarray[i]->power*pppm_t;
			  power = power  + l2dirarray[i]->power*pppm_t;
			  set_pppm(pppm_t,1/l2dirarray[i]->cachep.executionTime, 1, 1, 1);
              l2dir.rt_power = l2dir.rt_power + l2dirarray[i]->rt_power*pppm_t;
			  rt_power = rt_power  + l2dirarray[i]->rt_power*pppm_t;
		  }
	  }

  if (XML->sys.mc.number_mcs >0 && XML->sys.mc.memory_channels_per_mc>0)
  {
	  mc = new MemoryController(XML, pimid_ip(pimid_fam1), MC);   /* PIMID 1.12.18 (per-die process) */
	  mc->computeEnergy();
	  mc->computeEnergy(false);
	  mcs.area.set_area(mcs.area.get_area()+mc->area.get_area()*XML->sys.mc.number_mcs);
	  area.set_area(area.get_area()+mc->area.get_area()*XML->sys.mc.number_mcs);
	  set_pppm(pppm_t,XML->sys.mc.number_mcs*mc->mcp.clockRate, XML->sys.mc.number_mcs,XML->sys.mc.number_mcs,XML->sys.mc.number_mcs);
	  mcs.power = mc->power*pppm_t;
	  power = power  + mcs.power;
	  set_pppm(pppm_t,1/mc->mcp.executionTime, XML->sys.mc.number_mcs,XML->sys.mc.number_mcs,XML->sys.mc.number_mcs);
	  mcs.rt_power = mc->rt_power*pppm_t;
	  rt_power = rt_power  + mcs.rt_power;

  }

  if (XML->sys.flashc.number_mcs >0 )//flash controller
  {
	  flashcontroller = new FlashController(XML, pimid_ip(false));
	  flashcontroller->computeEnergy();
	  flashcontroller->computeEnergy(false);
	  double number_fcs = flashcontroller->fcp.num_mcs;
	  flashcontrollers.area.set_area(flashcontrollers.area.get_area()+flashcontroller->area.get_area()*number_fcs);
	  area.set_area(area.get_area()+flashcontrollers.area.get_area());
	  set_pppm(pppm_t,number_fcs, number_fcs ,number_fcs, number_fcs );
	  flashcontrollers.power = flashcontroller->power*pppm_t;
	  power = power  + flashcontrollers.power;
	  set_pppm(pppm_t,number_fcs , number_fcs ,number_fcs ,number_fcs );
	  flashcontrollers.rt_power = flashcontroller->rt_power*pppm_t;
	  rt_power = rt_power  + flashcontrollers.rt_power;

  }

  if (XML->sys.niu.number_units >0)
  {
	  niu = new NIUController(XML, pimid_ip(false));
	  niu->computeEnergy();
	  niu->computeEnergy(false);
	  nius.area.set_area(nius.area.get_area()+niu->area.get_area()*XML->sys.niu.number_units);
	  area.set_area(area.get_area()+niu->area.get_area()*XML->sys.niu.number_units);
	  set_pppm(pppm_t,XML->sys.niu.number_units*niu->niup.clockRate, XML->sys.niu.number_units,XML->sys.niu.number_units,XML->sys.niu.number_units);
	  nius.power = niu->power*pppm_t;
	  power = power  + nius.power;
	  set_pppm(pppm_t,XML->sys.niu.number_units*niu->niup.clockRate, XML->sys.niu.number_units,XML->sys.niu.number_units,XML->sys.niu.number_units);
	  nius.rt_power = niu->rt_power*pppm_t;
	  rt_power = rt_power  + nius.rt_power;

  }

  if (XML->sys.pcie.number_units >0 && XML->sys.pcie.num_channels >0)
  {
	  pcie = new PCIeController(XML, pimid_ip(false));   /* PIMID 1.12.18: the link controller sits on the logic die */
	  pcie->computeEnergy();
	  pcie->computeEnergy(false);
	  pcies.area.set_area(pcies.area.get_area()+pcie->area.get_area()*XML->sys.pcie.number_units);
	  area.set_area(area.get_area()+pcie->area.get_area()*XML->sys.pcie.number_units);
	  set_pppm(pppm_t,XML->sys.pcie.number_units*pcie->pciep.clockRate, XML->sys.pcie.number_units,XML->sys.pcie.number_units,XML->sys.pcie.number_units);
	  pcies.power = pcie->power*pppm_t;
	  power = power  + pcies.power;
	  set_pppm(pppm_t,XML->sys.pcie.number_units*pcie->pciep.clockRate, XML->sys.pcie.number_units,XML->sys.pcie.number_units,XML->sys.pcie.number_units);
	  pcies.rt_power = pcie->rt_power*pppm_t;
	  rt_power = rt_power  + pcies.rt_power;

  }

  if (numNOC >0)
  {
	  for (i = 0;i < numNOC; i++)
	  {
		  if (XML->sys.NoC[i].type)
		  {//First add up area of routers if NoC is used
			  nocs.push_back(new NoC(XML,i, pimid_noc_ip(i), 1));   /* PIMID 1.12.18 (per-die process) */
			  if (procdynp.homoNOC)
			  {
				  noc.area.set_area(noc.area.get_area() + nocs[i]->area.get_area()*procdynp.numNOC);
				  area.set_area(area.get_area() + noc.area.get_area());
			  }
			  else
			  {
				  noc.area.set_area(noc.area.get_area() + nocs[i]->area.get_area());
				  area.set_area(area.get_area() + nocs[i]->area.get_area());
			  }
		  }
		  else
		  {//Bus based interconnect
			  /* PIMID 1.11.17 (audit go-through): the bus length spans the
			   * DIE, and under the DRAM-periphery family the die's
			   * transistor-dominated components are reported dram_periph_area
			   * times larger by applyFam AFTER this constructor ran -- so the
			   * wire was sized for a die the run then reports bigger. Length
			   * scales as sqrt(area): apply the same factor here. (The
			   * fabric's own AREA is wire-dominated and stays unscaled by
			   * applyFam -- length and area are different claims.) */
			  /* PIMID 1.11.57 (latent E003): SCOPE IT TO THE DRAM DIE.
			   * The die that grows by dram_periph_area is the memory die.
			   * A level that sits on a buffer or base die instead is
			   * already exempted from the family transform 300 lines below
			   * (the on_dram_die test in the NoC carve-out), and its wire
			   * spans a die that did not grow -- so lengthening it here
			   * would stretch a bus across the wrong silicon. Nothing
			   * reachable differs today: every family-1 run forces H_TREE
			   * or CUSTOM, both of which map to type != 0, so the only
			   * type=0 levels are lvl 0 and lvl 1 and the emitter marks
			   * those on_dram_die. The test is here so a future flat or
			   * BUS level off the memory die cannot inherit the stretch
			   * silently. */
			  double bus_len_scale = (XML->sys.dram_periph_family == 1 &&
			                          XML->sys.NoC[i].on_dram_die)
			      ? sqrt(XML->sys.dram_periph_area) : 1.0;
			  nocs.push_back(new NoC(XML,i, pimid_noc_ip(i), 1, sqrt(area.get_area()*XML->sys.NoC[i].chip_coverage)*bus_len_scale));
			  if (procdynp.homoNOC){
				  noc.area.set_area(noc.area.get_area() + nocs[i]->area.get_area()*procdynp.numNOC);
				  area.set_area(area.get_area() + noc.area.get_area());
			  }
			  else
			  {
				  noc.area.set_area(noc.area.get_area() + nocs[i]->area.get_area());
				  area.set_area(area.get_area() + nocs[i]->area.get_area());
			  }
		  }
	  }

	  /*
	   * Compute global links associated with each NOC, if any. This must be done at the end (even after the NOC router part) since the total chip
	   * area must be obtain to decide the link routing
	   */
	  /* PIMID: the host fabric's networks (data and control) span one die: the area before any link */
	  const double area_before_links_um2 = area.get_area();
	  for (i = 0;i < numNOC; i++)
	  {
		  if (nocs[i]->nocdynp.has_global_link && XML->sys.NoC[i].type)
		  {
			  /* PIMID 1.12.18: LIVE NOW. PIMID emits has_global_link=1 on
			   * the links-only instances it writes for each tree level, with
			   * the structure the level's links span: the die area and the
			   * number of parent regions it is cut into. The length of ONE
			   * link is derived here, from H-tree geometry: the segment from a
			   * node to its parent is half the side of the parent's region,
			   * and the die is shared by the parent regions, so
			   *   len = 0.5 x sqrt(span_area / parent_regions)
			   * (CACTI 6.0, Muralimanohar, Balasubramonian and Jouppi, HP Labs
			   * HPL-2009-85, the H-tree of the bank/mat organisation;
			   * external/cacti/htree2.cc in_htree: each segment is half its
			   * region's side, halved per level). That length is used AS IS:
			   * it is already per link on the die it lies on, so neither the
			   * die-span derivation nor the die-growth stretch below applies.
			   * The upstream derivation (and the stretch) remain for an
			   * instance that supplies no span. */
			  /* PIMID 1.11.57 (latent E011): THE SAME DIE-GROWTH STRETCH AS
			   * THE BUS PATH. A global link is routed across the chip, so
			   * its length scales with sqrt(die area) exactly as the bus
			   * length does above -- but 1.11.17's correction was applied
			   * only to the bus arm, leaving two length computations of the
			   * same quantity disagreeing by sqrt(dram_periph_area) = 1.563
			   * at the corpus factor. Nothing reachable differs today
			   * because PIMID emits has_global_link=0 on every NoC instance
			   * it writes, so this block never executes; the divergence is
			   * closed here rather than left for whoever turns global links
			   * on. Scoped to on_dram_die for the reason given on the bus
			   * arm. */
			  /* PIMID: the host ring and crossbar links span the processor's own area; with a replicated last-level cache the
			   * area so far holds one slice of it, the die holds l3_replicated_slices. */
			  const double link_area_um2 = area_before_links_um2
			      + ((XML->sys.l3_replicated_slices > 1 && !l3array.empty())
			         ? l3array[0]->area.get_area() * (XML->sys.l3_replicated_slices - 1) : 0.0);
			  double link_len_scale = (XML->sys.dram_periph_family == 1 &&
			                           XML->sys.NoC[i].on_dram_die)
			      ? sqrt(XML->sys.dram_periph_area) : 1.0;
			  if (XML->sys.NoC[i].link_span_processor == 1 || XML->sys.NoC[i].link_span_processor == 2)
				  pimid::debugStream(std::cerr) << "  [link] noc" << i << " span: "
				          << link_area_um2 * XML->sys.NoC[i].chip_coverage * 1e-6
				          << " mm^2 (the processor's area before its fabric links)" << std::endl;
			  if (XML->sys.NoC[i].link_span_processor == 2)
				  /* PIMID (user ruling 2026-10-25): a RING's stop-to-stop link. The ring is one loop
				   * folded up and down the die through two columns of stops, as on Nehalem-EX (Rusu,
				   * ISSCC 2009 / JSSC 45(1) 2010: cores and LLC slices in two columns of rows, the two
				   * counter-rotating rings running through the slice columns, one stop per row). The loop
				   * is ~2 die sides long and carries one link per stop:
				   *   len = 2 x sqrt(die area) / stops   (DERIVED; square die).
				   * The per-stop-tile alternative sqrt(area / stops) assumes square stop tiles, which a
				   * two-column floorplan does not have (each stop's core + slice spans half the die
				   * width), so it is not used. McPAT's upstream 2-D mesh derivation (side / ((H+V)/2) / 2)
				   * gave half of this for a 1 x N ring. */
				  nocs[i]->init_link_bus(2.0 * sqrt(link_area_um2 * XML->sys.NoC[i].chip_coverage)
				                         / std::max(1, XML->sys.NoC[i].horizontal_nodes * XML->sys.NoC[i].vertical_nodes));
			  else if (XML->sys.NoC[i].link_span_processor == 3)
			  {
				  /* PIMID (1.12.150): a flat 2-D MESH's neighbour link is ONE TILE PITCH: the span is
				   * divided equally amongst the tiles (DSENT, Sun et al., NOCS 2012, Table III), so
				   *   len = sqrt(span / tiles)   (DERIVED; square tiles)
				   * The span is the memory array the routers sit over (link_span_area_mm2, the area
				   * the memory report prints), the same rule as a tree level's links, which span the
				   * memory die; the processor's own area (x chip_coverage) only when PIMID gives none.
				   * link_parent_regions = the tiles (one router each). McPAT's upstream derivation
				   * (side / ((H+V)/2) / 2) is half a pitch. */
				  const bool has_span = XML->sys.NoC[i].link_span_area_mm2 > 0;
				  const double span = has_span ? XML->sys.NoC[i].link_span_area_mm2 * 1e6
				                               : area.get_area() * XML->sys.NoC[i].chip_coverage;   /* um^2 */
				  const int tiles = std::max(1, XML->sys.NoC[i].link_parent_regions);
				  const double pitch = sqrt(span / tiles);
				  pimid::debugStream(std::cerr) << "  [link] noc" << i << " mesh tile pitch: "
				          << (has_span ? "memory array " : "processor area (no array area given) ")
				          << span * 1e-6 << " mm^2 / " << tiles << " tiles -> sqrt = " << pitch
				          << " um (DSENT tile-pitch convention, DERIVED)" << std::endl;
				  nocs[i]->init_link_bus(pitch);
			  }
			  else if (XML->sys.NoC[i].link_span_processor)   /* 1.12.23: the processor's own area, um^2 */
				  nocs[i]->init_link_bus(0.5 * sqrt(link_area_um2 * XML->sys.NoC[i].chip_coverage
				                                    / std::max(1, XML->sys.NoC[i].link_parent_regions)));
			  else if (XML->sys.NoC[i].link_span_area_mm2 > 0 && XML->sys.NoC[i].link_parent_regions > 0)
				  nocs[i]->init_link_bus(0.5 * sqrt(XML->sys.NoC[i].link_span_area_mm2 * 1e6
				                                    / XML->sys.NoC[i].link_parent_regions));   // 1.12.18, um
			  else
			  nocs[i]->init_link_bus(sqrt(area.get_area()*XML->sys.NoC[i].chip_coverage)*link_len_scale);//compute global links
			  if (procdynp.homoNOC)
			  {
				  noc.area.set_area(noc.area.get_area() + nocs[i]->link_bus_tot_per_Router.area.get_area()
						  * nocs[i]->nocdynp.total_nodes
						  * procdynp.numNOC);
				  area.set_area(area.get_area() + nocs[i]->link_bus_tot_per_Router.area.get_area()
						  * nocs[i]->nocdynp.total_nodes
						  * procdynp.numNOC);
			  }
			  else
			  {
				  noc.area.set_area(noc.area.get_area() + nocs[i]->link_bus_tot_per_Router.area.get_area()
						  * nocs[i]->nocdynp.total_nodes);
				  area.set_area(area.get_area() + nocs[i]->link_bus_tot_per_Router.area.get_area()
						  * nocs[i]->nocdynp.total_nodes);
			  }
		  }
	  }
	  //Compute energy of NoC (w or w/o links) or buses
	  for (i = 0;i < numNOC; i++)
	  {
		  nocs[i]->computeEnergy();
		  nocs[i]->computeEnergy(false);
		  if (procdynp.homoNOC){
			  set_pppm(pppm_t,procdynp.numNOC*nocs[i]->nocdynp.clockRate, procdynp.numNOC,procdynp.numNOC,procdynp.numNOC);
			  noc.power = noc.power + nocs[i]->power*pppm_t;
			  set_pppm(pppm_t,1/nocs[i]->nocdynp.executionTime, procdynp.numNOC,procdynp.numNOC,procdynp.numNOC);
			  noc.rt_power = noc.rt_power + nocs[i]->rt_power*pppm_t;
			  power = power  + noc.power;
			  rt_power = rt_power  + noc.rt_power;
		  }
		  else
		  {
			  set_pppm(pppm_t,nocs[i]->nocdynp.clockRate, 1, 1, 1);
			  noc.power = noc.power + nocs[i]->power*pppm_t;
			  power = power  + nocs[i]->power*pppm_t;
			  set_pppm(pppm_t,1/nocs[i]->nocdynp.executionTime, 1, 1, 1);
			  noc.rt_power = noc.rt_power + nocs[i]->rt_power*pppm_t;
			  rt_power = rt_power  + nocs[i]->rt_power*pppm_t;


		  }
	  }
  }

  /* PIMID 1.12.18: CLOCK-NETWORK SHARE OF EACH AGGREGATE (Wattch clock
   * model, D. Brooks, V. Tiwari, M. Martonosi, ISCA 2000; logic.h
   * ClockNetwork). Each component already added its clock network to its own
   * power and rt_power, so the aggregates above include it; these mirror the
   * aggregation above multiplier for multiplier so the share can be reported.
   * Peak: dynamic x clockRate x instances; runtime: dynamic / executionTime;
   * leakage x instances. */
  {
	  clku_core = clku_l2 = clku_l3 = clku_l1dir = clku_l2dir = clku_mcs = clku_noc = 0;
	  clkg_core = clkg_l2 = clkg_l3 = clkg_l1dir = clkg_l2dir = clkg_mcs = clkg_noc = 0;   /* PIMID 1.12.18 (ruling #4) */
	  auto clkAgg = [](Component &dst, double &ung, double &glob, const Component &src, double src_ung, double src_glob,
	                   double tdp_mul, double n, double rt_mul) {
		  double p[4];
		  set_pppm(p, tdp_mul, n, n, n);
		  dst.power = dst.power + src.power*p;
		  set_pppm(p, rt_mul, n, n, n);
		  dst.rt_power = dst.rt_power + src.rt_power*p;
		  ung += src_ung * rt_mul;
		  glob += src_glob * rt_mul;
	  };
	  for (i = 0; i < numCore; i++)
	  {
		  const double n = procdynp.homoCore ? procdynp.numCore : 1;
		  clkAgg(clk_core, clku_core, clkg_core, cores[i]->clockNetwork, cores[i]->clockNetwork.rt_ungated, cores[i]->clockNetwork.rt_global, cores[i]->clockRate*n, n, 1/cores[i]->executionTime);
		  if (XML->sys.Private_L2 && cores[i]->l2cache)   /* core.cc: peak at the L2/core clock ratio, runtime as is */
			  clkAgg(clk_core, clku_core, clkg_core, cores[i]->l2cache->clockNetwork, cores[i]->l2cache->clockNetwork.rt_ungated, cores[i]->l2cache->clockNetwork.rt_global, cores[i]->l2cache->cachep.clockRate*n, n,
			         1/cores[i]->executionTime);
	  }
	  if (!XML->sys.Private_L2)
		  for (i = 0; i < (int)l2array.size(); i++)
		  {
			  const double n = procdynp.homoL2 ? procdynp.numL2 : 1;
			  clkAgg(clk_l2, clku_l2, clkg_l2, l2array[i]->clockNetwork, l2array[i]->clockNetwork.rt_ungated, l2array[i]->clockNetwork.rt_global, l2array[i]->cachep.clockRate*n, n, 1/l2array[i]->cachep.executionTime);
		  }
	  for (i = 0; i < (int)l3array.size(); i++)
	  {
		  const double n = procdynp.homoL3 ? procdynp.numL3 : 1;
		  clkAgg(clk_l3, clku_l3, clkg_l3, l3array[i]->clockNetwork, l3array[i]->clockNetwork.rt_ungated, l3array[i]->clockNetwork.rt_global, l3array[i]->cachep.clockRate*n, n, 1/l3array[i]->cachep.executionTime);
	  }
	  for (i = 0; i < (int)l1dirarray.size(); i++)
	  {
		  const double n = procdynp.homoL1Dir ? procdynp.numL1Dir : 1;
		  clkAgg(clk_l1dir, clku_l1dir, clkg_l1dir, l1dirarray[i]->clockNetwork, l1dirarray[i]->clockNetwork.rt_ungated, l1dirarray[i]->clockNetwork.rt_global, l1dirarray[i]->cachep.clockRate*n, n, 1/l1dirarray[i]->cachep.executionTime);
	  }
	  for (i = 0; i < (int)l2dirarray.size(); i++)
	  {
		  const double n = procdynp.homoL2Dir ? procdynp.numL2Dir : 1;
		  clkAgg(clk_l2dir, clku_l2dir, clkg_l2dir, l2dirarray[i]->clockNetwork, l2dirarray[i]->clockNetwork.rt_ungated, l2dirarray[i]->clockNetwork.rt_global, l2dirarray[i]->cachep.clockRate*n, n, 1/l2dirarray[i]->cachep.executionTime);
	  }
	  if (mc)
		  clkAgg(clk_mcs, clku_mcs, clkg_mcs, mc->clockNetwork, mc->clockNetwork.rt_ungated, mc->clockNetwork.rt_global, XML->sys.mc.number_mcs*mc->mcp.clockRate, XML->sys.mc.number_mcs,
		         1/mc->mcp.executionTime);
	  for (i = 0; i < (int)nocs.size(); i++)
	  {
		  const double n = procdynp.homoNOC ? procdynp.numNOC : 1;
		  clkAgg(clk_noc, clku_noc, clkg_noc, nocs[i]->clk, nocs[i]->clk_rt_ungated, nocs[i]->clk_rt_global, n*nocs[i]->nocdynp.clockRate, n, 1/nocs[i]->nocdynp.executionTime);
	  }
  }

  /* PIMID 1.11.12: DRAM-PERIPHERY DEVICE FAMILY, applied inside the tool.
   *
   * Components that sit on a DRAM die are built from that die's peripheral
   * transistors -- thick oxide, long channel, high Vth, retention-grade
   * leakage -- not from the logic process McPAT prices everything in. CACTI
   * refuses to price general logic in its comm-dram device (its UCA asserts
   * a positive dynamic term, and that device only functions inside the array
   * machinery with wordline boost), so the family is expressed as a
   * transform of McPAT's own logic result, with factors read from CACTI's hp
   * vs comm-dram device columns at the technology's own generation table.
   *
   * It is applied HERE, over the components McPAT owns, rather than by
   * scaling the outputs afterwards: one owner per model.
   *
   * PIMID 1.11.57 (latent E006): the paragraph that used to sit here
   * described a dram_periph_scope bitmask -- "PE cores (1), their caches (2),
   * the on-die fabric (4), the element controllers (8)" -- as if it decided
   * which components the transform reaches. 1.11.34 (E10) removed that mask
   * because it had exactly one reachable value, and said so 180 lines below;
   * this header kept describing it, so the file contained two contradictory
   * accounts of its own control flow and the false one came first. The mask
   * is gone from XML_Parse too. What actually scopes the transform now is
   * per-component: the NoC keeps its logic-process AREA (wire, not device),
   * and the MC PHY carve-out is gated on NoC[i].on_dram_die.
   *
   * Leakage rebases on PLAIN leakage: the comm-dram ratio already encodes a
   * long-channel device, so stacking McPAT's longer-channel discount on top
   * would double-count it. */
  /* PIMID 1.11.51 (N1): the on-pitch logic share of the core, one
   * computation for both pitch consumers (the family-1 areaMul and the
   * family-0 SUBARRAY-pitch path below). See the L116 boundary statement. */
  auto onPitchLogicShare = [&]() -> double {
      double a_tot = 0.0, a_logic = 0.0;
      for (int ci = 0; ci < numCore; ci++) {
          Core* c = cores[ci];
          double lg = 0.0;
          if (c->exu) {
              lg += c->exu->area.get_area();
              if (c->exu->rfu)   lg -= c->exu->rfu->area.get_area();
              if (c->exu->scheu) lg -= c->exu->scheu->area.get_area();
          }
          /* PIMID 1.11.54 (audit E001): DO NOT ADD THE PIPELINE TWICE.
           * core.cc does not add corepipe to the core area directly (that
           * line is disabled at core.cc:1881); it DISTRIBUTES it, adding
           * pipeline_area_per_unit = corepipe*num_pipelines/4 into each of
           * ifu/lsu/exu/mmu (/5 plus rnu on OOO). So exu->area above already
           * carries its share, and adding corepipe again counted the
           * pipeline as (share + whole) against a denominator that holds
           * exactly corepipe*num_pipelines. At the emitted num_pipelines=1
           * that is +0.25*corepipe of pure over-count; at N>=2 it flips to an
           * under-count. */
          /* PIMID 1.11.57 (audit D010): E001 OVER-CORRECTED. Removing the
           * `+ corepipe` left the numerator carrying ONLY the exu's share of
           * the pipeline, P*N/4, while the denominator t still holds all of
           * it, P*N -- because the other three quarters are sitting inside
           * ifu, lsu and mmu, and this numerator does not include those units.
           * The stated intent, three hundred lines below, is "execution unit
           * MINUS its register-file and scheduler arrays, PLUS pipeline and
           * the undifferentiated core": the whole pipeline, not a quarter of
           * it. E001 correctly deleted a +0.25*P over-count and left a
           * -0.75*P under-count in its place -- the sign flipped and the
           * magnitude tripled. It was invisible because both versions produce
           * a plausible share (0.184867 before, 0.183517 after) and the
           * release measured only that the number moved in the direction it
           * predicted, not by how much; nothing else computes this share to
           * disagree with it.
           *
           * The shares hidden in ifu/lsu/mmu (and rnu on OOO) are added back
           * here: take the whole distributed pipeline P*N and subtract the one
           * share exu already contributed above. Where exu does not exist,
           * core.cc added no share to it, so nothing is subtracted. */
          if (c->corepipe) {
              const double pn = c->corepipe->area.get_area() *
                                (double)c->coredynp.num_pipelines;
              const double per_unit =
                  pn / ((c->coredynp.core_ty == OOO) ? 5.0 : 4.0);
              const double already_in_exu =
                  (c->exu && c->exu->exist) ? per_unit : 0.0;
              lg += pn - already_in_exu;
          }
          if (c->undiffCore) lg += c->undiffCore->area.get_area();
          double t = c->area.get_area();
          if (lg < 0.0) lg = 0.0;
          if (lg > t)   lg = t;
          a_tot += t; a_logic += lg;
      }
      return (a_tot > 0.0) ? a_logic / a_tot : 1.0;
  };

  if (XML->sys.dram_periph_family == 1) {
      const double fa = XML->sys.dram_periph_area;
      /* PIMID 1.11.34 (E11): the PITCH penalty applies to the CORE ONLY. It
       * used to be folded into fa before emission, so it also scaled the caches
       * and the memory controller -- off-pitch circuits that do not abut the
       * array and are limited by wiring rather than by bitline geometry. */
      const double fpitch = (XML->sys.dram_periph_pitch > 0.0)
                          ? XML->sys.dram_periph_pitch : 1.0;
      const double fd = XML->sys.dram_periph_dyn;
      const double fl = XML->sys.dram_periph_leak;
      /* PIMID 1.11.54 (audit E004): gate leakage is oxide tunnelling, not
       * subthreshold conduction. It used to be scaled by fl -- a ratio of
       * I_off_n rows -- while CACTI tabulates I_g_on_n separately and McPAT
       * computes it with cmos_Ig_leakage. Defaults to fl when the emitter
       * did not supply one (older XMLs), so nothing changes silently. */
      const double fg = (XML->sys.dram_periph_gate_leak > 0.0)
                        ? XML->sys.dram_periph_gate_leak : fl;
      auto applyFam = [&](Component& c, bool scaleArea, const char* who = "?",
                          double areaMul = 1.0) {
          /* PIMID 1.11.32 (user ruling E9): read each basis from ITSELF.
           * This used to take plainLeak from c.power -- the PEAK component --
           * and write it into the RUNTIME fields too, discarding whatever
           * runtime leakage McPAT had computed. The gated fields below already
           * read plainGated and plainGatedRt separately, so the function
           * disagreed with itself: the correct pattern was present, applied to
           * the gated endpoints and not the active ones.
           *
           * Today the two are equal by construction -- McPAT's aggregation
           * uses the same pppm slots 1-3 for leakage in both bases
           * (set_pppm(..., n, n, n) for peak and runtime alike; only slot 0,
           * the dynamic multiplier, differs), and no component assigns
           * rt leakage independently. So this is a latent trap rather than a
           * live error, and the change is numerically inert.
           *
           * WHY THEY DIFFER, root-caused in 1.11.33 rather than left open.
           * The gap is almost entirely the EXECUTION UNIT. Measured per
           * sub-unit on a BANK/HBM3 cell:
           *     ifu    0.001138 / 0.000818    1.39x
           *     lsu    0.001641 / 0.001641    1.00x
           *     exu    0.031331 / 0.001515   20.68x   <- 98.9% of the gap
           *     mmu    0.000417 / 0.000417    1.00x
           *     undiff 0.001668 (added to both)
           *     SUM    0.036196 / 0.006060    5.973x  == the measured ratio
           * lsu, mmu and undiffCore agree EXACTLY, as leakage should. Only exu
           * collapses, because McPAT scales the execution unit's RUNTIME
           * leakage with utilisation: an unexercised ALU or FPU contributes
           * almost none. That is not how leakage works -- a powered idle unit
           * leaks the same as a busy one, and gating is tracked in separate
           * fields -- so the runtime figure is the physically wrong one.
           *
           * PIMID reads leakage from the PEAK basis (extractComponent), which
           * counts every powered device. That is the correct choice; switching
           * to rt_power would silently drop ~86% of core static power. The
           * transform below still gives each basis its own value, so whichever
           * a future consumer reads is transformed consistently.
           *
           * The check below is a REGRESSION GUARD, not a discovery tool. */
          double plainLeak   = c.power.readOp.leakage;
          double plainLeakRt = c.rt_power.readOp.leakage;
          {
              static bool warned_leak_basis = false;
              const double ref = std::max(std::fabs(plainLeak), std::fabs(plainLeakRt));
              if (!warned_leak_basis && ref > 0.0 &&
                  std::fabs(plainLeak - plainLeakRt) > 1e-9 * ref) {
                  warned_leak_basis = true;
                  pimid::debugStream(std::cerr) << "[fam] peak vs runtime leakage on " << who << ": "
                            << plainLeak << " vs " << plainLeakRt
                            << " W. EXPECTED upstream (McPAT scales the execution "
                               "unit's runtime leakage with utilisation); PIMID "
                               "reports the PEAK basis, which is the physical one."
                            << std::endl;
              }
          }
          c.power.readOp.dynamic    *= fd;
          c.rt_power.readOp.dynamic *= fd;
          c.power.readOp.leakage                        = plainLeak * fl;
          c.power.readOp.longer_channel_leakage         = plainLeak * fl;
          c.power.readOp.gate_leakage                  *= fg;   // 1.11.54 (E004)
          c.rt_power.readOp.leakage                     = plainLeakRt * fl;
          c.rt_power.readOp.longer_channel_leakage      = plainLeakRt * fl;
          c.rt_power.readOp.gate_leakage               *= fg;   // 1.11.54 (E004)
          /* 1.11.15 (audit): the GATED endpoints must ride the same family
           * transform, or power gating on a periphery component compares a
           * family-priced active value against a logic-priced gated one and
           * the interpolation collapses to a no-op.
           *
           * 1.11.16 (verification audit): REBASE, do not scale. The active
           * side above deliberately drops McPAT's longer-channel discount
           * (the comm-dram ratio already encodes a long-channel device);
           * scaling power_gated_with_long_channel_leakage in place kept that
           * discount on the gated side only, so the two endpoints were on
           * different bases and PG savings were over-credited ~2.25x --
           * exactly the mismatch 1.11.15 claimed to have ended. Both gated
           * fields rebase on the plain (pre-long-channel) gated value, the
           * mirror of lines 435-436. */
          double plainGated   = c.power.readOp.power_gated_leakage;
          double plainGatedRt = c.rt_power.readOp.power_gated_leakage;
          c.power.readOp.power_gated_leakage                       = plainGated * fl;
          c.power.readOp.power_gated_with_long_channel_leakage     = plainGated * fl;
          c.rt_power.readOp.power_gated_leakage                    = plainGatedRt * fl;
          c.rt_power.readOp.power_gated_with_long_channel_leakage  = plainGatedRt * fl;
          /* 1.11.34 (E11): areaMul carries the CORE-ONLY pitch penalty; it
           * is 1.0 for every other component. */
          if (scaleArea) c.area.set_area(c.area.get_area() * fa * areaMul);
      };
      /* AREA vs DEVICE factors are not the same claim. The dynamic and
       * leakage factors are device physics and apply to every component on
       * the die. The AREA factor is a transistor-PITCH penalty, and it may
       * only be applied where area is transistor-dominated. The fabric's
       * area is not: the 1.10.5 census showed most tree nodes are
       * single-child pass-throughs, i.e. WIRE, and wire pitch on a DRAM die
       * does not follow the device pitch. So the NoC takes the device
       * factors and keeps its logic-process area -- stated, because the
       * alternative (scaling a wire-dominated area by a device ratio) put a
       * 16-PE add-on at 88% of its own HBM3 die. */
      /* 1.11.34 (E10): the scope mask had one reachable value (15, emitted as
       * a literal with no surface), so it is gone and the transform applies
       * unconditionally -- which is what 15 meant. */
      /* PIMID 1.11.50 (L116): the PITCH penalty applies to the core's
       * ON-PITCH LOGIC share, not to its SRAM arrays. The penalty models
       * layout constrained to the array's bitline pitch (Vogelsang MICRO
       * 2010: sense-amp stripes and local wordline drivers); the core's own
       * SRAM arrays -- caches, TLBs, register files, scheduler CAMs -- are
       * placed as macro blocks that need not follow the array's pitch.
       *
       * 1.11.54 (audit E002, user-ruled): the earlier wording justified this
       * with "CELL area CACTI's tables declare device-independent". That
       * MISREADS the tables. area_cell is 146 (292 for the 2-port cell) in
       * EVERY device column at EVERY node -- because it is expressed in F^2,
       * the ITRS convention -- not because a periphery device leaves an SRAM
       * cell unchanged. CACTI simply carries no data for "SRAM built in a
       * DRAM periphery process".
       *
       * Physically an SRAM cell in DRAM periphery DOES suffer: it is six
       * ordinary transistors, and on a periphery process those are the
       * thick-oxide, long-channel, relaxed-rule devices whose larger F the
       * l_phy ratio measures. area_cell = 146*F^2 evaluated at the periphery
       * F IS the fa scaling -- so applying fa to the core's arrays is not an
       * over-reach; it is the same statement CACTI's own cell model makes.
       * fa therefore stays on the WHOLE core, arrays included.
       *
       * The PITCH penalty survives as a separate, narrower claim: it asks
       * whether a circuit must be DRAWN ON the array's bitline pitch, which
       * is layout geometry rather than device size, and a macro block placed
       * away from the array is free of that constraint whatever transistors
       * it is made of. The on-pitch share is measured
       * from McPAT's own breakdown: execution unit MINUS its register-file
       * and scheduler arrays, plus pipeline and the undifferentiated core.
       * (Decode logic inside the IFU stays classed with the IFU's arrays --
       * a stated approximation, the units are not separable there.) */
      double pitch_core_mul = fpitch;
      if (fpitch != 1.0 && numCore > 0) {
          double logic_frac = onPitchLogicShare();
          pitch_core_mul = 1.0 + (fpitch - 1.0) * logic_frac;
          /* stderr: the fork's stdout is /dev/null (see wrapper). */
          pimid::debugStream(std::cerr) << "  [fam] pitch penalty " << fpitch
                    << " applies to the on-pitch logic share ("
                    << logic_frac << " of core area) -> effective x"
                    << pitch_core_mul << "; SRAM macro blocks keep their"
                    << " cell-table area." << std::endl;
      }
      applyFam(core, true, "core", pitch_core_mul);
      /* PIMID 1.12.18: the clock share rides its component's transform. */
      applyFam(clk_core, false, "core clock");
      applyFam(clk_l2, false, "l2 clock"); applyFam(clk_l3, false, "l3 clock");
      clku_core *= fd; clku_l2 *= fd; clku_l3 *= fd;
      clkg_core *= fd; clkg_l2 *= fd; clkg_l3 *= fd;   /* PIMID 1.12.18 (ruling #4) */
      applyFam(l2, true, "l2"); applyFam(l3, true, "l3");
      {
          /* PIMID 1.11.50 (L74 + L103), replacing the blanket transform.
           *
           * L74: the family used to apply to EVERY NoC level in the run,
           * but a multi-level build spans the physical hierarchy and only
           * the levels ON the DRAM die are made of periphery transistors.
           * Each XML NoC instance now carries on_dram_die from the same
           * placement matrix that decides the family (subarray..chip on
           * die; channel on die only for channel-centric parts; rank and
           * system are buffer-die/host-board logic). Off-die levels keep
           * their logic pricing entirely.
           *
           * L103: within an on-die level, the family's DYNAMIC factor is
           * device physics (CACTI's comm-dram vs hp columns, dominated by
           * gate capacitance) -- it transforms the ROUTER's buffers,
           * crossbars and arbiters, but the WIRE share of dynamic is metal
           * capacitance and does not follow the device. The link/bus share
           * of dynamic is therefore restored to its metal price after the
           * transform. LEAKAGE stays transformed for both shares: link
           * DRIVERS are periphery devices and leak as such. A bus-type
           * level (subarray/bank) is all link, so its dynamic is untouched
           * -- which is exactly the pass-through-wire fabric the 1.10.5
           * census flagged. */
          bool any_on_die = false;
          for (int ni = 0; ni < numNOC; ni++) {
              if (!XML->sys.NoC[ni].on_dram_die) {
                  pimid::debugStream(std::cerr) << "  [fam] noc" << ni << ": off-die fabric"
                            << " (rank/channel/system tier) stays logic-priced."
                            << std::endl;
                  continue;
              }
              any_on_die = true;
              NoC* n = nocs[ni];
              double link_tdp_dyn = 0.0, link_rt_dyn = 0.0;
              if (n->link_bus_exist) {
                  link_tdp_dyn = n->link_bus_tot_per_Router.power.readOp.dynamic
                               * n->nocdynp.total_nodes;
                  link_rt_dyn  = n->link_bus->rt_power.readOp.dynamic;
              }
              applyFam(*n, false, "noc");
              applyFam(n->clk, false, "noc clock");   /* PIMID 1.12.18: its share, same transform */
              n->power.readOp.dynamic    += link_tdp_dyn * (1.0 - fd);
              n->rt_power.readOp.dynamic += link_rt_dyn  * (1.0 - fd);
          }
          (void)any_on_die;
          /* Rebuild the aggregate from the (now mixed-family) levels with
           * the SAME pppm arithmetic the constructor's accumulation used,
           * so the total and the per-level breakdown cannot disagree. The
           * chip totals are rebuilt from components further down. */
          noc.power.reset();
          clk_noc.power.reset();      /* PIMID 1.12.18 */
          clk_noc.rt_power.reset();
          clku_noc = 0;
          for (int ni = 0; ni < numNOC; ni++)
              clku_noc += nocs[ni]->clk_rt_ungated / nocs[ni]->nocdynp.executionTime
                        * (XML->sys.NoC[ni].on_dram_die ? fd : 1.0);   /* rt aggregation: 1/executionTime only */
          clkg_noc = 0;   /* PIMID 1.12.18 (ruling #4): the same aggregation */
          for (int ni = 0; ni < numNOC; ni++)
              clkg_noc += nocs[ni]->clk_rt_global / nocs[ni]->nocdynp.executionTime
                        * (XML->sys.NoC[ni].on_dram_die ? fd : 1.0);
          noc.rt_power.reset();
          for (int ni = 0; ni < numNOC; ni++) {
              double pppm_agg[4];
              if (procdynp.homoNOC) {
                  set_pppm(pppm_agg, procdynp.numNOC*nocs[ni]->nocdynp.clockRate,
                           procdynp.numNOC, procdynp.numNOC, procdynp.numNOC);
                  noc.power = noc.power + nocs[ni]->power*pppm_agg;
                  clk_noc.power = clk_noc.power + nocs[ni]->clk.power*pppm_agg;   /* PIMID 1.12.18 */
                  set_pppm(pppm_agg, 1/nocs[ni]->nocdynp.executionTime,
                           procdynp.numNOC, procdynp.numNOC, procdynp.numNOC);
                  noc.rt_power = noc.rt_power + nocs[ni]->rt_power*pppm_agg;
                  clk_noc.rt_power = clk_noc.rt_power + nocs[ni]->clk.rt_power*pppm_agg;   /* PIMID 1.12.18 */
              } else {
                  set_pppm(pppm_agg, nocs[ni]->nocdynp.clockRate, 1, 1, 1);
                  noc.power = noc.power + nocs[ni]->power*pppm_agg;
                  clk_noc.power = clk_noc.power + nocs[ni]->clk.power*pppm_agg;   /* PIMID 1.12.18 */
                  set_pppm(pppm_agg, 1/nocs[ni]->nocdynp.executionTime, 1, 1, 1);
                  noc.rt_power = noc.rt_power + nocs[ni]->rt_power*pppm_agg;
                  clk_noc.rt_power = clk_noc.rt_power + nocs[ni]->clk.rt_power*pppm_agg;   /* PIMID 1.12.18 */
              }
          }
      }
      /* PIMID 1.11.50 (L104): the MC's logic backend (frontend + transaction
       * engine) is device-priced McPAT logic and takes the family; the MC
       * PHY, when built, is an EMPIRICAL fit -- its area is an IO-inclusive
       * die-photo curve and its dynamic an mW/Gb/s IO figure (Niagara
       * 1/2-derived; interposer tier from O'Connor MICRO-50 2017). A device
       * -column ratio cannot transform a measured curve, so the PHY's
       * dynamic and area are restored after the transform. Its LEAKAGE is
       * device-computed (gate counts x Isub) and stays transformed. At the
       * D2/D3 on-die tiers (subarray..chip) withPHY=0 and no PHY exists, so
       * this fires only for the interposer/off-package tiers that still run
       * under the family (channel-centric parts). */
      {
          double phy_tdp_dyn = 0.0, phy_rt_dyn = 0.0, phy_area_tot = 0.0;
          if (mc && mc->mcp.withPHY && mc->PHY) {
              const double n_mcs = XML->sys.mc.number_mcs;
              phy_tdp_dyn  = mc->PHY->power.readOp.dynamic
                           * n_mcs * mc->mcp.clockRate;
              phy_rt_dyn   = mc->PHY->rt_power.readOp.dynamic
                           / mc->mcp.executionTime;
              phy_area_tot = mc->PHY->area.get_area() * n_mcs;
          }
          applyFam(mcs, true, "mcs");
          applyFam(clk_mcs, false, "mc clock");   /* PIMID 1.12.18 */
          clku_mcs *= fd;
          clkg_mcs *= fd;   /* PIMID 1.12.18 (ruling #4) */
          if (phy_tdp_dyn > 0.0 || phy_area_tot > 0.0) {
              mcs.power.readOp.dynamic    += phy_tdp_dyn * (1.0 - fd);
              mcs.rt_power.readOp.dynamic += phy_rt_dyn  * (1.0 - fd);
              mcs.area.set_area(mcs.area.get_area()
                                + phy_area_tot * (1.0 - fa));
              pimid::debugStream(std::cerr) << "  [fam] MC PHY empirical dynamic/area kept at"
                        << " their measured values; family applies to the"
                        << " MC's logic backend and to leakage." << std::endl;
          }
      }
      /* Rebuild the processor totals from the transformed components so the
       * aggregate and the parts agree -- the failure mode 1.11.0 found in the
       * NoC census and 1.11.4 found in the CoreBreakdown split. */
      /* PIMID 1.11.57 (latent E017): the FIRST of two blocks that both
       * narrated the E8 rollup fix is deleted here, because only the second
       * one describes the code. Both opened with the same sentence about
       * l1dir/l2dir/nius/flashcontrollers; the first ended at "COMPLETE the
       * rollup" (add them all) and the second at "the fix is not to add them
       * unconditionally ... each is gated on its configured count", which is
       * what addIf() below actually does. A reader who stopped at the first
       * would have believed the aggregate included four components it does
       * not. The surviving facts from the deleted block are kept below: what
       * upstream adds, and that PIMID never reads this aggregate. */
      power = core.power + l2.power + l3.power + noc.power + mcs.power;
      rt_power = core.rt_power + l2.rt_power + l3.rt_power
               + noc.rt_power + mcs.rt_power;
      area.set_area(core.area.get_area() + l2.area.get_area()
                  + l3.area.get_area() + noc.area.get_area()
                  + mcs.area.get_area());
      /* PIMID 1.11.29 (user ruling E8, as refined): include a component ONLY
       * when the user configured it. The rollup listed core+l2+l3+noc+mcs by
       * hand and silently omitted four classes upstream McPAT does add
       * (l1dir ~208, l2dir ~237, flashcontrollers ~279, nius ~295) -- but the
       * fix is not to add them unconditionally, because reporting a component
       * nobody asked for is its own error. Each is gated on its configured
       * count, exactly as pcies already is below.
       *
       * flashcontrollers is kept rather than dropped: storage and
       * storage-class compute are on the roadmap, and a controller that only
       * contributes when configured costs nothing until then.
       *
       * Verified before changing it: PIMID never reads this aggregate --
       * ComponentType::FULL_SYSTEM is declared and never populated, and the
       * wrapper extracts CORE/NOC/PCIE/L2/L3/MC individually. So this moves
       * no reported number today. It is completed rather than deleted so the
       * aggregate is correct if anything ever consumes it. (1.11.50: audit
       * finding L115 -- "the rebuilt totals are dead stores" -- is that same
       * fact: kept, correct, and consciously unread.) */
      auto addIf = [&](bool configured, const Component& c) {
          if (!configured) return;
          power = power + c.power;
          rt_power = rt_power + c.rt_power;
          area.set_area(area.get_area() + c.area.get_area());
      };
      addIf(XML->sys.number_of_L1Directories > 0, l1dir);
      addIf(XML->sys.number_of_L2Directories > 0, l2dir);
      addIf(XML->sys.niu.number_units       > 0, nius);
      addIf(XML->sys.flashc.number_mcs > 0, flashcontrollers);   // system_mc: number_mcs
      if (XML->sys.pcie.number_units > 0) {
          power = power + pcies.power;
          rt_power = rt_power + pcies.rt_power;
          area.set_area(area.get_area() + pcies.area.get_area());
      }
  } else if (XML->sys.core_pitch_factor > 1.0 && numCore > 0) {
      /* PIMID 1.11.51 (N1, user-raised at E7): PITCH IS GEOMETRY, NOT
       * FAMILY. A SUBARRAY-placed PE beside an SRAM/NVM array is pressed
       * against that array's pitch exactly as a DRAM-placed one is, but the
       * penalty used to live inside the DRAM-periphery transform and could
       * never fire here. Same on-pitch boundary as L116: the penalty lands
       * on the measured logic share; SRAM macro blocks keep their cell-
       * table area. Area only -- the device factors are family physics and
       * correctly stay off a logic die. */
      double lf = onPitchLogicShare();
      double p = XML->sys.core_pitch_factor;
      double mul = 1.0 + (p - 1.0) * lf;
      double before = core.area.get_area();
      core.area.set_area(before * mul);
      area.set_area(area.get_area() + core.area.get_area() - before);
      pimid::debugStream(std::cerr) << "  [fam] SUBARRAY pitch x" << p
                << " on a logic-family die: applied to the on-pitch logic "
                << "share (" << lf << " of core area) -> core area x"
                << mul << "." << std::endl;
  }

//  //clock power
//  globalClock.init_wire_external(is_default, &interface_ip);
//  globalClock.clk_area           =area*1e6; //change it from mm^2 to um^2
//  globalClock.end_wiring_level   =5;//toplevel metal
//  globalClock.start_wiring_level =5;//toplevel metal
//  globalClock.l_ip.with_clock_grid=false;//global clock does not drive local final nodes
//  globalClock.optimize_wire();
  /* PIMID 1.12.18: the chip-level tree that would join the components'
   * trees (globalClock above) is NOT priced: its leaves are the component
   * tree roots, and which component it would be charged to is a reporting
   * choice nobody has ruled on. Each component's tree (logic.h
   * ClockNetwork) starts at its own root. The per-aggregate clock share
   * (Wattch clock model, D. Brooks, V. Tiwari, M. Martonosi, ISCA 2000): */
  {
	  /* PIMID 1.12.18 (ruling 2026-10-23 #4): the charged runtime dynamic is
	   * GLOBAL (every powered cycle; for cores with idle halt, every RUNNING
	   * cycle) + LOCAL (gated by measured activity). The per-die trunk joining
	   * these roots is built just below (its own line, in no component). */
	  auto rep = [&](const char *who, const Component &c, const Component &tot, double ung, double glob) {
		  if (c.power.readOp.dynamic == 0 && c.power.readOp.leakage == 0 && c.rt_power.readOp.dynamic == 0) return;
		  pimid::debugStream(std::cerr) << "[mcpat clock] aggregate " << who << ": runtime dynamic " << c.rt_power.readOp.dynamic
		            << " W = GLOBAL " << glob << " W (H-tree levels but the last, every powered cycle) + LOCAL "
		            << c.rt_power.readOp.dynamic - glob << " W (last level + pins, gated by measured activity)"
		            << " (ungated reference " << ung
		            << " W) of the component's " << tot.rt_power.readOp.dynamic << " W; subthreshold leakage "
		            << c.power.readOp.leakage << " W, gate " << c.power.readOp.gate_leakage << " W; peak dynamic "
		            << c.power.readOp.dynamic << " W (of " << tot.power.readOp.dynamic << " W)" << std::endl;
	  };
	  rep("core", clk_core, core, clku_core, clkg_core); rep("L2", clk_l2, l2, clku_l2, clkg_l2); rep("L3", clk_l3, l3, clku_l3, clkg_l3);
	  rep("L1dir", clk_l1dir, l1dir, clku_l1dir, clkg_l1dir); rep("L2dir", clk_l2dir, l2dir, clku_l2dir, clkg_l2dir);
	  rep("memory controllers", clk_mcs, mcs, clku_mcs, clkg_mcs); rep("NoC", clk_noc, noc, clku_noc, clkg_noc);
  }

  /* PIMID 1.12.18 (user ruling "each die has its own clock tree ... avoid
   * double counting"): THE PER-DIE CLOCK TRUNK, at the hook the component
   * trees left (logic.h ClockNetwork). One die-level H-tree per die and clock
   * domain (a domain is a clock frequency: the controllers and the cores of
   * one die are separate trees), built with the same Wattch/CACTI model as
   * the component trees:
   *   leaves  N = the component tree ROOTS on that die (core, cache,
   *           controller and router/link instances that have a tree); the
   *           trunk ENDS at each root, whose first repeater is inside the
   *           component's own tree, so there are no register pins here and
   *           nothing is charged twice;
   *   area    the die: the DRAM die's area as PIMID describes it (the
   *           calibrated die, pimid_dram_die_area_mm2), the logic die's when
   *           given, otherwise the area McPAT itself gives the components on
   *           that die; L = 1.5 D (sqrt(N) - 1), CACTI Global wire at the
   *           die's node (ip_logic_die / interface_ip);
   *   runtime E x f x on, on = the largest on-fraction of its leaves (the
   *           trunk switches while ANY component on the die is clocked; a
   *           component that keeps serving -- controller, router, cache --
   *           is on every powered cycle, so the trunk is off only on a die
   *           whose cores are all halted). With the max, the union of the
   *           leaves' on-times is bounded below; it is exact whenever one leaf
   *           is always on (every die with a controller or router).
   *   The DRAM die kind holds pimid_dram_die_count dies sharing its leaves
   *   evenly; its trunk is priced per die and multiplied by the count.
   *   Family transform: a trunk on the DRAM die takes the same comm-dram
   *   periphery factors as the clock trees beside it. */
  trunk_dies.clear();
  clk_trunk.power.reset();
  clk_trunk.rt_power.reset();
  if (XML->sys.pimid_clock_trunk && XML->sys.pimid_clock_network)
  {
	  struct TLeaf { int die; double f_hz; double n; double on; const char *who; };
	  std::vector<TLeaf> tl;
	  double dram_area_um2 = 0;
	  /* f: the clock the component's OWN tree runs at (ClockNetwork::clock_hz;
	   * McPAT's controller carries mcp.clockRate = 2 x its clock, the DDR
	   * convention, while its tree is built at the controller clock). */
	  auto addLeaf = [&](int die, double f, double n, double on, const char *who) {
		  if (n > 0 && f > 0) tl.push_back({die, f, n, on, who});
	  };
	  const int dcore = pimid_fam1 ? 1 : 0;
	  for (i = 0; i < numCore; i++)
	  {
		  const double n = procdynp.homoCore ? procdynp.numCore : 1;
		  addLeaf(dcore, cores[i]->clockNetwork.clock_hz, n, cores[i]->clockNetwork.trunk_on(), "core");
		  if (XML->sys.Private_L2 && cores[i]->l2cache)
			  addLeaf(dcore, cores[i]->l2cache->clockNetwork.clock_hz, n, cores[i]->l2cache->clockNetwork.global_on(), "private L2");
	  }
	  if (!XML->sys.Private_L2)
		  for (i = 0; i < (int)l2array.size(); i++)
			  addLeaf(dcore, l2array[i]->clockNetwork.clock_hz, procdynp.homoL2 ? procdynp.numL2 : 1, l2array[i]->clockNetwork.global_on(), "L2");
	  for (i = 0; i < (int)l3array.size(); i++)
		  addLeaf(dcore, l3array[i]->clockNetwork.clock_hz, procdynp.homoL3 ? procdynp.numL3 : 1, l3array[i]->clockNetwork.global_on(), "L3");
	  for (i = 0; i < (int)l1dirarray.size(); i++)
		  addLeaf(dcore, l1dirarray[i]->clockNetwork.clock_hz, procdynp.homoL1Dir ? procdynp.numL1Dir : 1, l1dirarray[i]->clockNetwork.global_on(), "L1dir");
	  for (i = 0; i < (int)l2dirarray.size(); i++)
		  addLeaf(dcore, l2dirarray[i]->clockNetwork.clock_hz, procdynp.homoL2Dir ? procdynp.numL2Dir : 1, l2dirarray[i]->clockNetwork.global_on(), "L2dir");
	  if (mc)
		  addLeaf(dcore, mc->clockNetwork.clock_hz, XML->sys.mc.number_mcs, mc->clockNetwork.global_on(), "memory controller");
	  if (pimid_fam1)
		  dram_area_um2 = core.area.get_area() + l2.area.get_area() + l3.area.get_area()
		                + l1dir.area.get_area() + l2dir.area.get_area() + mcs.area.get_area();
	  for (i = 0; i < (int)nocs.size(); i++)
	  {
		  NoC *n = nocs[i];
		  const int d = (pimid_fam1 && XML->sys.NoC[i].on_dram_die) ? 1 : 0;
		  const double sets  = procdynp.homoNOC ? procdynp.numNOC : 1;
		  const double nodes = n->nocdynp.total_nodes;
		  const double links = n->link_bus_exist ? (double)n->nocdynp.global_linked_ports * nodes : 0.0;
		  const double roots = XML->sys.NoC[i].clock_root_shared ? 0.0   /* its stops are another level's roots */
		                     : ((n->clk_router.built && n->clk_router.n_sinks > 0) ? nodes : 0.0)
		                     + ((n->clk_link.built && n->clk_link.n_sinks > 0) ? links : 0.0);
		  addLeaf(d, (n->clk_router.built && n->clk_router.clock_hz > 0) ? n->clk_router.clock_hz
		          : (n->clk_link.built && n->clk_link.clock_hz > 0) ? n->clk_link.clock_hz : n->nocdynp.clockRate,
		          roots * sets, n->clk_router.global_on(), "NoC router/link");
		  if (d == 1)
		  {
			  dram_area_um2 += n->area.get_area() * sets;   // its links included (NoC::init_link_bus adds them to its area)
		  }
	  }
	  const double fd_d  = pimid_fam1 ? XML->sys.dram_periph_dyn : 1.0;
	  const double fl_d  = pimid_fam1 ? XML->sys.dram_periph_leak : 1.0;
	  const double fg_d  = (pimid_fam1 && XML->sys.dram_periph_gate_leak > 0.0) ? XML->sys.dram_periph_gate_leak : fl_d;
	  const int    ndram = (XML->sys.pimid_dram_die_count > 0) ? XML->sys.pimid_dram_die_count : 1;
	  for (int d = 0; d < 2; d++)
	  {
		  /* the leaves of this die kind, grouped by clock domain */
		  std::vector<std::pair<double, std::pair<double, double> > > dom;   /* f -> (N, on) */
		  std::string whos;
		  for (const auto &l : tl)
		  {
			  if (l.die != d) continue;
			  bool found = false;
			  for (auto &e : dom)
				  if (e.first == l.f_hz) { e.second.first += l.n; e.second.second = std::max(e.second.second, l.on); found = true; }
			  if (!found) dom.push_back(std::make_pair(l.f_hz, std::make_pair(l.n, l.on)));
			  if (whos.find(l.who) == std::string::npos) whos += std::string(whos.empty() ? "" : ", ") + l.who;
		  }
		  if (dom.empty()) continue;
		  const int    cnt = (d == 1) ? ndram : 1;
		  double a_die_um2;
		  std::string a_src;
		  if (d == 1)
		  {
			  if (XML->sys.pimid_dram_die_area_mm2 > 0) { a_die_um2 = XML->sys.pimid_dram_die_area_mm2 * 1e6; a_src = "the DRAM die as described (pimid_dram_die_area_mm2)"; }
			  else { a_die_um2 = dram_area_um2 / cnt; a_src = "McPAT's area of the DRAM-die components / dies"; }
		  }
		  else
		  {
			  if (XML->sys.pimid_logic_die_area_mm2 > 0) { a_die_um2 = XML->sys.pimid_logic_die_area_mm2 * 1e6; a_src = "the logic die as described (pimid_logic_die_area_mm2)"; }
			  else { a_die_um2 = area.get_area() - dram_area_um2; a_src = "McPAT's area of the logic-die components"; }
		  }
		  if (a_die_um2 < 0) a_die_um2 = 0;
		  InputParameter tip = (d == 1) ? interface_ip : *pimid_ip(false);
		  tip.wt = Global;
		  PimidTrunkDie td;
		  td.name = (d == 1) ? "dram_die" : "logic_die";
		  td.count = cnt; td.area_mm2 = a_die_um2 * 1e-6; td.leaves = 0; td.node_nm = tip.F_sz_nm; td.on = 0;
		  td.peak_w = td.rt_w = td.ungated_w = td.leak_w = td.gate_w = 0;
		  Component dsum;
		  for (const auto &e : dom)
		  {
			  const double f = e.first, n_die = e.second.first / cnt, on = e.second.second;
			  ClockNetwork tr;
			  std::ostringstream own, src;
			  own << "trunk " << td.name << " " << f*1e-6 << " MHz";
			  src << "component tree roots on this die in this clock domain: " << e.second.first << " / " << cnt << " die(s)";
			  tr.compute(&tip, own.str(), a_die_um2, n_die, src.str(), 0.0, f, XML->sys.Embedded,
			             Uncore_device, Inorder, true);
			  const double e_cyc = tr.e_tree_cycle;   /* J per cycle per die, wire + repeaters + sc */
			  tr.set_power(e_cyc * f * cnt, e_cyc * f * on * cnt, e_cyc * f * cnt, cnt);
			  if (d == 1 && pimid_fam1)
			  {
				  tr.power.readOp.dynamic *= fd_d; tr.rt_power.readOp.dynamic *= fd_d; tr.rt_ungated *= fd_d;
				  tr.power.readOp.leakage *= fl_d; tr.power.readOp.longer_channel_leakage *= fl_d;
				  tr.power.readOp.power_gated_leakage *= fl_d; tr.power.readOp.power_gated_with_long_channel_leakage *= fl_d;
				  tr.power.readOp.gate_leakage *= fg_d;
				  tr.rt_power.readOp.leakage *= fl_d; tr.rt_power.readOp.longer_channel_leakage *= fl_d;
				  tr.rt_power.readOp.power_gated_leakage *= fl_d; tr.rt_power.readOp.power_gated_with_long_channel_leakage *= fl_d;
				  tr.rt_power.readOp.gate_leakage *= fg_d;
			  }
			  dsum.power = dsum.power + tr.power;
			  dsum.rt_power = dsum.rt_power + tr.rt_power;
			  td.leaves += e.second.first / cnt;
			  td.on = std::max(td.on, on);
			  td.peak_w += tr.power.readOp.dynamic;
			  td.rt_w += tr.rt_power.readOp.dynamic;
			  td.ungated_w += tr.rt_ungated;
			  pimid::debugStream(std::cerr) << "[mcpat clock] " << own.str() << ": " << cnt << " die(s) of " << a_die_um2*1e-6 << " mm^2 (" << a_src
			            << "), " << n_die << " root(s) per die, L " << tr.tree_len_um << " um at " << tip.F_sz_nm
			            << " nm; " << e_cyc << " J/cycle/die x " << f*1e-6 << " MHz x on " << on << " x " << cnt
			            << (d == 1 && pimid_fam1 ? " x periphery dyn factor" : "") << " = " << tr.rt_power.readOp.dynamic
			            << " W runtime (ungated " << tr.rt_ungated << " W); leakage " << tr.power.readOp.leakage
			            << " W, gate " << tr.power.readOp.gate_leakage << " W" << std::endl;
		  }
		  td.leak_w = XML->sys.longer_channel_device ? dsum.power.readOp.longer_channel_leakage : dsum.power.readOp.leakage;
		  td.gate_w = dsum.power.readOp.gate_leakage;
		  clk_trunk.power = clk_trunk.power + dsum.power;
		  clk_trunk.rt_power = clk_trunk.rt_power + dsum.rt_power;
		  power = power + dsum.power;
		  rt_power = rt_power + dsum.rt_power;
		  trunk_dies.push_back(td);
		  pimid::debugStream(std::cerr) << "[mcpat clock] trunk " << td.name << " (x" << cnt << "; leaves: " << whos << "): runtime dynamic "
		            << td.rt_w << " W (on " << td.on << "), ungated " << td.ungated_w << " W, peak " << td.peak_w
		            << " W; leakage " << td.leak_w << " W + gate " << td.gate_w << " W" << std::endl;
	  }
  }
  else
	  std::cerr << "[mcpat clock] per-die trunk NOT priced ("
	            << (XML->sys.pimid_clock_network ? "power.models.clock_die_trunk: false or not described" : "clock network off")
	            << ")" << std::endl;

}

void Processor::displayDeviceType(int device_type_, uint32_t indent)
{
	string indent_str(indent, ' ');

	switch ( device_type_ ) {

	  case 0 :
		  cout <<indent_str<<"Device Type= "<<"ITRS high performance device type"<<endl;
	    break;
	  case 1 :
		  cout <<indent_str<<"Device Type= "<<"ITRS low standby power device type"<<endl;
	    break;
	  case 2 :
		  cout <<indent_str<<"Device Type= "<<"ITRS low operating power device type"<<endl;
	    break;
	  case 3 :
		  cout <<indent_str<<"Device Type= "<<"LP-DRAM device type"<<endl;
	    break;
	  case 4 :
		  cout <<indent_str<<"Device Type= "<<"COMM-DRAM device type"<<endl;
	    break;
	  default :
		  {
			  cout <<indent_str<<"Unknown Device Type"<<endl;
			  exit(0);
		  }
	}
}

void Processor::displayInterconnectType(int interconnect_type_, uint32_t indent)
{
	string indent_str(indent, ' ');

	switch ( interconnect_type_ ) {

	  case 0 :
		  cout <<indent_str<<"Interconnect metal projection= "<<"aggressive interconnect technology projection"<<endl;
	    break;
	  case 1 :
		  cout <<indent_str<<"Interconnect metal projection= "<<"conservative interconnect technology projection"<<endl;
	    break;
	  default :
		  {
			  cout <<indent_str<<"Unknown Interconnect Projection Type"<<endl;
			  exit(0);
		  }
	}
}

void Processor::displayEnergy(uint32_t indent, int plevel, bool is_tdp)
{
	int i;
	bool long_channel = XML->sys.longer_channel_device;
	bool power_gating = XML->sys.power_gating;
	string indent_str(indent, ' ');
	string indent_str_next(indent+2, ' ');
	if (is_tdp)
	{

		if (plevel<5)
		{
			cout<<"\nMcPAT (version "<< VER_MAJOR <<"."<< VER_MINOR
					<< " of " << VER_UPDATE << ") results (current print level is "<< plevel
			<<", please increase print level to see the details in components): "<<endl;
		}
		else
		{
			cout<<"\nMcPAT (version "<< VER_MAJOR <<"."<< VER_MINOR
								<< " of " << VER_UPDATE << ") results  (current print level is 5)"<< endl;
		}
		cout <<"*****************************************************************************************"<<endl;
		cout <<indent_str<<"Technology "<<XML->sys.core_tech_node<<" nm"<<endl;
		//cout <<indent_str<<"Device Type= "<<XML->sys.device_type<<endl;
		if (long_channel)
			cout <<indent_str<<"Using Long Channel Devices When Appropriate"<<endl;
		//cout <<indent_str<<"Interconnect metal projection= "<<XML->sys.interconnect_projection_type<<endl;
		displayInterconnectType(XML->sys.interconnect_projection_type, indent);
		cout <<indent_str<<"Core clock Rate(MHz) "<<XML->sys.core[0].clock_rate<<endl;
    	cout <<endl;
		cout <<"*****************************************************************************************"<<endl;
		cout <<"Processor: "<<endl;
		cout << indent_str << "Area = " << area.get_area()*1e-6<< " mm^2" << endl;
		cout << indent_str << "Peak Power = " << power.readOp.dynamic +
			(long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) + power.readOp.gate_leakage <<" W" << endl;
		cout << indent_str << "Total Leakage = " <<
			(long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) + power.readOp.gate_leakage <<" W" << endl;
		cout << indent_str << "Peak Dynamic = " << power.readOp.dynamic << " W" << endl;
		cout << indent_str << "Subthreshold Leakage = " << (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
		if (power_gating) cout << indent_str << "Subthreshold Leakage with power gating = "
				<< (long_channel? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)  << " W" << endl;
		cout << indent_str << "Gate Leakage = " << power.readOp.gate_leakage << " W" << endl;
		cout << indent_str << "Runtime Dynamic = " << rt_power.readOp.dynamic << " W" << endl;
		cout <<endl;
		if (numCore >0){
		cout <<indent_str<<"Total Cores: "<<XML->sys.number_of_cores << " cores "<<endl;
		displayDeviceType(XML->sys.device_type,indent);
		cout << indent_str_next << "Area = " << core.area.get_area()*1e-6<< " mm^2" << endl;
		cout << indent_str_next << "Peak Dynamic = " << core.power.readOp.dynamic << " W" << endl;
		cout << indent_str_next << "Subthreshold Leakage = "
			<< (long_channel? core.power.readOp.longer_channel_leakage:core.power.readOp.leakage) <<" W" << endl;
		if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
				<< (long_channel? core.power.readOp.power_gated_with_long_channel_leakage : core.power.readOp.power_gated_leakage)  << " W" << endl;
		cout << indent_str_next << "Gate Leakage = " << core.power.readOp.gate_leakage << " W" << endl;
		cout << indent_str_next << "Runtime Dynamic = " << core.rt_power.readOp.dynamic << " W" << endl;
		cout <<endl;
		}
		if (!XML->sys.Private_L2)
		{
			if (numL2 >0){
				cout <<indent_str<<"Total L2s: "<<endl;
				displayDeviceType(XML->sys.L2[0].device_type,indent);
				cout << indent_str_next << "Area = " << l2.area.get_area()*1e-6<< " mm^2" << endl;
				cout << indent_str_next << "Peak Dynamic = " << l2.power.readOp.dynamic << " W" << endl;
				cout << indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? l2.power.readOp.longer_channel_leakage:l2.power.readOp.leakage) <<" W" << endl;
				if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
						<< (long_channel? l2.power.readOp.power_gated_with_long_channel_leakage : l2.power.readOp.power_gated_leakage)  << " W" << endl;
				cout << indent_str_next << "Gate Leakage = " << l2.power.readOp.gate_leakage << " W" << endl;
				cout << indent_str_next << "Runtime Dynamic = " << l2.rt_power.readOp.dynamic << " W" << endl;
				cout <<endl;
			}
		}
		if (numL3 >0){
			cout <<indent_str<<"Total L3s: "<<endl;
			displayDeviceType(XML->sys.L3[0].device_type, indent);
			cout << indent_str_next << "Area = " << l3.area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << l3.power.readOp.dynamic << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? l3.power.readOp.longer_channel_leakage:l3.power.readOp.leakage) <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? l3.power.readOp.power_gated_with_long_channel_leakage : l3.power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << l3.power.readOp.gate_leakage << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << l3.rt_power.readOp.dynamic << " W" << endl;
			cout <<endl;
		}
		if (numL1Dir >0){
			cout <<indent_str<<"Total First Level Directory: "<<endl;
			displayDeviceType(XML->sys.L1Directory[0].device_type, indent);
			cout << indent_str_next << "Area = " << l1dir.area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << l1dir.power.readOp.dynamic << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? l1dir.power.readOp.longer_channel_leakage:l1dir.power.readOp.leakage) <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? l1dir.power.readOp.power_gated_with_long_channel_leakage : l1dir.power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << l1dir.power.readOp.gate_leakage << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << l1dir.rt_power.readOp.dynamic << " W" << endl;
			cout <<endl;
		}
		if (numL2Dir >0){
			cout <<indent_str<<"Total Second Level Directory: "<<endl;
			displayDeviceType(XML->sys.L1Directory[0].device_type, indent);
			cout << indent_str_next << "Area = " << l2dir.area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << l2dir.power.readOp.dynamic << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? l2dir.power.readOp.longer_channel_leakage:l2dir.power.readOp.leakage) <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? l2dir.power.readOp.power_gated_with_long_channel_leakage : l2dir.power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << l2dir.power.readOp.gate_leakage << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << l2dir.rt_power.readOp.dynamic << " W" << endl;
			cout <<endl;
		}
		if (numNOC >0){
			cout <<indent_str<<"Total NoCs (Network/Bus): "<<endl;
			displayDeviceType(XML->sys.device_type, indent);
			cout << indent_str_next << "Area = " << noc.area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << noc.power.readOp.dynamic << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? noc.power.readOp.longer_channel_leakage:noc.power.readOp.leakage) <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? noc.power.readOp.power_gated_with_long_channel_leakage : noc.power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << noc.power.readOp.gate_leakage << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << noc.rt_power.readOp.dynamic << " W" << endl;
			cout <<endl;
		}
		if (XML->sys.mc.number_mcs >0 && XML->sys.mc.memory_channels_per_mc>0)
		{
			cout <<indent_str<<"Total MCs: "<<XML->sys.mc.number_mcs << " Memory Controllers "<<endl;
			displayDeviceType(XML->sys.device_type, indent);
			cout << indent_str_next << "Area = " << mcs.area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << mcs.power.readOp.dynamic << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? mcs.power.readOp.longer_channel_leakage:mcs.power.readOp.leakage)  <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? mcs.power.readOp.power_gated_with_long_channel_leakage : mcs.power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << mcs.power.readOp.gate_leakage << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << mcs.rt_power.readOp.dynamic << " W" << endl;
			cout <<endl;
		}
		if (XML->sys.flashc.number_mcs >0)
		{
			cout <<indent_str<<"Total Flash/SSD Controllers: "<<flashcontroller->fcp.num_mcs << " Flash/SSD Controllers "<<endl;
			displayDeviceType(XML->sys.device_type, indent);
			cout << indent_str_next << "Area = " << flashcontrollers.area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << flashcontrollers.power.readOp.dynamic << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? flashcontrollers.power.readOp.longer_channel_leakage:flashcontrollers.power.readOp.leakage)  <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? flashcontrollers.power.readOp.power_gated_with_long_channel_leakage : flashcontrollers.power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << flashcontrollers.power.readOp.gate_leakage << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << flashcontrollers.rt_power.readOp.dynamic << " W" << endl;
			cout <<endl;
		}
		if (XML->sys.niu.number_units >0 )
		{
			cout <<indent_str<<"Total NIUs: "<<niu->niup.num_units << " Network Interface Units "<<endl;
			displayDeviceType(XML->sys.device_type, indent);
			cout << indent_str_next << "Area = " << nius.area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << nius.power.readOp.dynamic << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? nius.power.readOp.longer_channel_leakage:nius.power.readOp.leakage)  <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? nius.power.readOp.power_gated_with_long_channel_leakage : nius.power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << nius.power.readOp.gate_leakage << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << nius.rt_power.readOp.dynamic << " W" << endl;
			cout <<endl;
		}
		if (XML->sys.pcie.number_units >0 && XML->sys.pcie.num_channels>0)
				{
					cout <<indent_str<<"Total PCIes: "<<pcie->pciep.num_units << " PCIe Controllers "<<endl;
					displayDeviceType(XML->sys.device_type, indent);
					cout << indent_str_next << "Area = " << pcies.area.get_area()*1e-6<< " mm^2" << endl;
					cout << indent_str_next << "Peak Dynamic = " << pcies.power.readOp.dynamic << " W" << endl;
					cout << indent_str_next << "Subthreshold Leakage = "
						<< (long_channel? pcies.power.readOp.longer_channel_leakage:pcies.power.readOp.leakage)  <<" W" << endl;
					if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
							<< (long_channel? pcies.power.readOp.power_gated_with_long_channel_leakage : pcies.power.readOp.power_gated_leakage)  << " W" << endl;
					cout << indent_str_next << "Gate Leakage = " << pcies.power.readOp.gate_leakage << " W" << endl;
					cout << indent_str_next << "Runtime Dynamic = " << pcies.rt_power.readOp.dynamic << " W" << endl;
					cout <<endl;
				}
		cout <<"*****************************************************************************************"<<endl;
		if (plevel >1)
		{
			for (i = 0;i < numCore; i++)
			{
				cores[i]->displayEnergy(indent+4,plevel,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}
			if (!XML->sys.Private_L2)
			{
				for (i = 0;i < numL2; i++)
				{
					l2array[i]->displayEnergy(indent+4,is_tdp);
					cout <<"*****************************************************************************************"<<endl;
				}
			}
			for (i = 0;i < numL3; i++)
			{
				l3array[i]->displayEnergy(indent+4,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}
			for (i = 0;i < numL1Dir; i++)
			{
				l1dirarray[i]->displayEnergy(indent+4,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}
			for (i = 0;i < numL2Dir; i++)
			{
				l2dirarray[i]->displayEnergy(indent+4,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}
			if (XML->sys.mc.number_mcs >0 && XML->sys.mc.memory_channels_per_mc>0)
			{
				mc->displayEnergy(indent+4,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}
			if (XML->sys.flashc.number_mcs >0 && XML->sys.flashc.memory_channels_per_mc>0)
			{
				flashcontroller->displayEnergy(indent+4,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}
			if (XML->sys.niu.number_units >0 )
			{
				niu->displayEnergy(indent+4,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}
			if (XML->sys.pcie.number_units >0 && XML->sys.pcie.num_channels>0)
			{
				pcie->displayEnergy(indent+4,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}

			for (i = 0;i < numNOC; i++)
			{
				nocs[i]->displayEnergy(indent+4,plevel,is_tdp);
				cout <<"*****************************************************************************************"<<endl;
			}
		}
	}
	else
	{

	}

}

void Processor::set_proc_param()
{
	bool debug = false;

	procdynp.homoCore = bool(debug?1:XML->sys.homogeneous_cores);
	procdynp.homoL2   = bool(debug?1:XML->sys.homogeneous_L2s);
	procdynp.homoL3   = bool(debug?1:XML->sys.homogeneous_L3s);
	procdynp.homoNOC  = bool(debug?1:XML->sys.homogeneous_NoCs);
	procdynp.homoL1Dir  = bool(debug?1:XML->sys.homogeneous_L1Directories);
	procdynp.homoL2Dir  = bool(debug?1:XML->sys.homogeneous_L2Directories);

	procdynp.numCore = XML->sys.number_of_cores;
	procdynp.numL2   = XML->sys.number_of_L2s;
	procdynp.numL3   = XML->sys.number_of_L3s;
	procdynp.numNOC  = XML->sys.number_of_NoCs;
	procdynp.numL1Dir  = XML->sys.number_of_L1Directories;
	procdynp.numL2Dir  = XML->sys.number_of_L2Directories;
	procdynp.numMC = XML->sys.mc.number_mcs;
	procdynp.numMCChannel = XML->sys.mc.memory_channels_per_mc;

//	if (procdynp.numCore<1)
//	{
//		cout<<" The target processor should at least have one core on chip." <<endl;
//		exit(0);
//	}

	//  if (numNOCs<0 || numNOCs>2)
	//    {
	//  	  cout <<"number of NOCs must be 1 (only global NOCs) or 2 (both global and local NOCs)"<<endl;
	//  	  exit(0);
	//    }

	/* Basic parameters*/
	interface_ip.data_arr_ram_cell_tech_type    = debug?0:XML->sys.device_type;
	interface_ip.data_arr_peri_global_tech_type = debug?0:XML->sys.device_type;
	interface_ip.tag_arr_ram_cell_tech_type     = debug?0:XML->sys.device_type;
	interface_ip.tag_arr_peri_global_tech_type  = debug?0:XML->sys.device_type;

	interface_ip.specific_hp_vdd = false;
	interface_ip.specific_lop_vdd = false;
	interface_ip.specific_lstp_vdd = false;

	interface_ip.specific_vcc_min = false;

	interface_ip.ic_proj_type     = debug?0:XML->sys.interconnect_projection_type;
	interface_ip.delay_wt                = 100;//Fixed number, make sure timing can be satisfied.
	interface_ip.area_wt                 = 0;//Fixed number, This is used to exhaustive search for individual components.
	interface_ip.dynamic_power_wt        = 100;//Fixed number, This is used to exhaustive search for individual components.
	interface_ip.leakage_power_wt        = 0;
	interface_ip.cycle_time_wt           = 0;

	interface_ip.delay_dev                = 10000;//Fixed number, make sure timing can be satisfied.
	interface_ip.area_dev                 = 10000;//Fixed number, This is used to exhaustive search for individual components.
	interface_ip.dynamic_power_dev        = 10000;//Fixed number, This is used to exhaustive search for individual components.
	interface_ip.leakage_power_dev        = 10000;
	interface_ip.cycle_time_dev           = 10000;

	interface_ip.ed                       = 2;
	interface_ip.burst_len      = 1;//parameters are fixed for processor section, since memory is processed separately
	interface_ip.int_prefetch_w = 1;
	interface_ip.page_sz_bits   = 0;
	interface_ip.temp = debug?360: XML->sys.temperature;
	interface_ip.F_sz_nm         = debug?90:XML->sys.core_tech_node;//XML->sys.core_tech_node;
	interface_ip.F_sz_um         = interface_ip.F_sz_nm / 1000;

	//***********This section of code does not have real meaning, they are just to ensure all data will have initial value to prevent errors.
	//They will be overridden  during each components initialization
	interface_ip.cache_sz            =64;
	interface_ip.line_sz             = 1;
	interface_ip.assoc               = 1;
	interface_ip.nbanks              = 1;
	interface_ip.out_w               = interface_ip.line_sz*8;
	interface_ip.specific_tag        = 1;
	interface_ip.tag_w               = 64;
	interface_ip.access_mode         = 2;

	interface_ip.obj_func_dyn_energy = 0;
	interface_ip.obj_func_dyn_power  = 0;
	interface_ip.obj_func_leak_power = 0;
	interface_ip.obj_func_cycle_t    = 1;

	interface_ip.is_main_mem     = false;
	interface_ip.rpters_in_htree = true ;
	interface_ip.ver_htree_wires_over_array = 0;
	interface_ip.broadcast_addr_din_over_ver_htrees = 0;

	interface_ip.num_rw_ports        = 1;
	interface_ip.num_rd_ports        = 0;
	interface_ip.num_wr_ports        = 0;
	interface_ip.num_se_rd_ports     = 0;
	interface_ip.num_search_ports    = 1;
	interface_ip.nuca                = 0;
	interface_ip.nuca_bank_count     = 0;
	interface_ip.is_cache            =true;
	interface_ip.pure_ram            =false;
	interface_ip.pure_cam            =false;
	interface_ip.force_cache_config  =false;
	interface_ip.power_gating  		 =XML->sys.power_gating;

	if (XML->sys.Embedded)
		{
		interface_ip.wt                  =Global_30;
		interface_ip.wire_is_mat_type = 0;
		interface_ip.wire_os_mat_type = 0;
		}
	else
		{
		interface_ip.wt                  =Global;
		interface_ip.wire_is_mat_type = 2;
		interface_ip.wire_os_mat_type = 2;
		}
	interface_ip.force_wiretype      = false;
	interface_ip.print_detail        = 1;
	interface_ip.add_ecc_b_          =true;
}

Processor::~Processor(){
	while (!cores.empty())
	{
		delete cores.back();
		cores.pop_back();
	}
	while (!l2array.empty())
	{
		delete l2array.back();
		l2array.pop_back();
	}
	while (!l3array.empty())
	{
		delete l3array.back();
		l3array.pop_back();
	}
	while (!nocs.empty())
	{
		delete nocs.back();
		nocs.pop_back();
	}
	while (!l1dirarray.empty())
	{
		delete l1dirarray.back();
		l1dirarray.pop_back();
	}
	while (!l2dirarray.empty())
	{
		delete l2dirarray.back();
		l2dirarray.pop_back();
	}
	if (mc)
	{
		delete mc;
		mc=0;
	}
	if (niu)
	{
		delete niu;
		niu =0;
	}
	if (pcie)
	{
		delete pcie;
		pcie=0;
	}
	if (flashcontroller)
	{
		delete flashcontroller;
		flashcontroller = 0;
	}
};
