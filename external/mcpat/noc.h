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

#ifndef NOC_H_
#define NOC_H_
#include "XML_Parse.h"
#include "logic.h"
#include "parameter.h"
#include "array.h"
#include "interconnect.h"
#include "basic_components.h"
#include "router.h"
#include "TSV.h"

class NoC :public Component {
  public:

	ParseXML *XML;
	int  ithNoC;
	InputParameter interface_ip;
	double link_len;
	double executionTime;
	double scktRatio, chip_PR_overhead, macro_PR_overhead;
	Router * router;
	/* PIMID 1.12.18: link_bus is the per-link price, whichever model built
	 * it: link_wire (McPAT interconnect over the CACTI wire model, the
	 * upstream path) or link_tsv_flit (a CACTI-3DD TSV x flit bits). A view
	 * only: link_wire and link_tsv are the owners. */
	Component * link_bus;
	interconnect * link_wire;
	TSV * link_tsv;
	Component link_tsv_flit;
	Component link_io_flit;   /* an off-package channel link, energy per bit x flit bits */
	bool link_is_tsv;
	NoCParam  nocdynp;
	uca_org_t local_result;
	statsDef       tdp_stats;
	statsDef       rtp_stats;
	statsDef       stats_t;
	powerDef       power_t;
	Component      link_bus_tot_per_Router;
	bool link_bus_exist;
	bool router_exist;
	string name, link_name;
	double M_traffic_pattern;
	/* PIMID 1.12.18: clock networks (logic.h ClockNetwork; Wattch,
	 * D. Brooks, V. Tiwari, M. Martonosi, ISCA 2000, clocking): one per
	 * router, one per link/bus segment. clk is their sum for this NoC. */
	ClockNetwork clk_router, clk_link;
	Component clk;
	double clk_rt_ungated;   /* J, the ungated reference of clk.rt_power */
	double clk_rt_global;    /* PIMID 1.12.18 (ruling #4): J, the global part of clk.rt_power */
	/* PIMID 1.12.18 (user ruling "pass-through routers as wire"): the
	 * router accesses this instance charges, total_accesses less the
	 * pass-through routers' accesses (set_noc_param). */
	double router_accesses;
	void build_link_clock();
	NoC(ParseXML *XML_interface, int ithNoC_, InputParameter* interface_ip_, double M_traffic_pattern_ = 0.6,double link_len_=0);
	void set_noc_param();
	void computeEnergy(bool is_tdp=true);
	void displayEnergy(uint32_t indent = 0,int plevel = 100, bool is_tdp=true);
	void init_link_bus(double link_len_);
	void init_router();
	void computeEnergy_link_bus(bool is_tdp=true);
	void displayEnergy_link_bus(uint32_t indent = 0,int plevel = 100, bool is_tdp=true);
	~NoC();
};

#endif /* NOC_H_ */
