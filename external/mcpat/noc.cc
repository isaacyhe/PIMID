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

#include "io.h"
#include "parameter.h"
#include "const.h"
#include "basic_circuit.h"
#include <iostream>
#include <algorithm>
#include "XML_Parse.h"
#include <string>
#include <cmath>
#include <assert.h>
#include "noc.h"
#include <set>
#include <sstream>

/* Clamp NaN/infinity/negative power values to 0.
 * PIMID 1.11.90: no longer silent. Every clamped field prints one line (the
 * first time for that router and field) and is counted in the substitution
 * ledger (basic_components.h); PIMID refuses the run unless
 * PIMID_ALLOW_ARRAY_CLAMP=1. */
static inline double sanitize_power(double v, const std::string& router,
                                    const char* field) {
	if (std::isnan(v) || std::isinf(v) || v < 0) {
		std::ostringstream os;
		os << "[mcpat] WARNING: router " << router << " " << field << " was "
		   << v << ", clamped to 0 (CACTI solve failure or degenerate geometry)";
		pimid_note_substitution(PIMID_SUBST_NOC, router + "|" + field, os.str());
		return 0;
	}
	return v;
}



NoC::NoC(ParseXML *XML_interface, int ithNoC_, InputParameter* interface_ip_, double M_traffic_pattern_, double link_len_)
:XML(XML_interface),
ithNoC(ithNoC_),
interface_ip(*interface_ip_),
router(0),
link_bus(0),
link_bus_exist(false),
router_exist(false),
M_traffic_pattern(M_traffic_pattern_)
{
	/*
	 * initialize, compute and optimize individual components.
	 */

	if (XML->sys.Embedded)
			{
			interface_ip.wt                  =Global_30;
			interface_ip.wire_is_mat_type = 0;
			interface_ip.wire_os_mat_type = 1;
			}
		else
			{
			interface_ip.wt                  =Global;
			interface_ip.wire_is_mat_type = 2;
			interface_ip.wire_os_mat_type = 2;
			}
	set_noc_param();
	local_result=init_interface(&interface_ip);
	scktRatio = g_tp.sckt_co_eff;

	if (nocdynp.type)
	{/*
		 * if NOC compute router, router links must be computed separately
		 * and called from external
		 * since total chip area must be known first
		 */
		init_router();
	}
	else
	{
		init_link_bus(link_len_); //if bus compute bus
	}

	//  //clock power
	//  clockNetwork.init_wire_external(is_default, &interface_ip);
	//  clockNetwork.clk_area           =area*1.1;//10% of placement overhead. rule of thumb
	//  clockNetwork.end_wiring_level   =5;//toplevel metal
	//  clockNetwork.start_wiring_level =5;//toplevel metal
	//  clockNetwork.num_regs           = corepipe.tot_stage_vector;
	//  clockNetwork.optimize_wire();
}

void NoC::init_router()
{
	router  = new Router(nocdynp.flit_size,
			nocdynp.virtual_channel_per_port*nocdynp.input_buffer_entries_per_vc,
			nocdynp.virtual_channel_per_port, &(g_tp.peri_global),
			nocdynp.input_ports,nocdynp.output_ports, M_traffic_pattern);
	//router->print_router();

	/* PIMID 1.11.81 (audit round 6, R6-10): CROSS-CHECK THE BUFFER AGAINST
	 * ITS OWN CROSSBAR, AND SAY SO WHEN IT IS ABSURD.
	 *
	 * History. Round 6 measured the router input buffer at 10.4x its own
	 * crossbar on the 22 nm table and 6445x on the 32 nm table (buffer
	 * 2.53e-11 against 3.20e-08 J/access, crossbar and arbiter scaling a
	 * physical ~2x), which is how DDR3 -- the one technology pinned to the
	 * 32 nm table -- reported ~100x its fabric power. 1.11.87 found the
	 * cause: Router::buffer_stats() handed the Mat an UNINITIALISED wire type
	 * (and the full rail as the bitline sense voltage). With both corrected
	 * the buffer Mat converges at every node measured (22, 32 and 65 nm,
	 * both routers), this check clears on DDR3 (gate 1196A), and a
	 * non-finite Mat result now refuses the run (cacti/router.cc).
	 *
	 * What this block is now. A CROSS-CHECK, not a known-defect report: it
	 * should not fire on any supported configuration, and if it does, that
	 * is a NEW imbalance in the buffer solve and worth reporting with the
	 * config. It WARNS; it does not clamp, scale or refuse.
	 *
	 * The threshold is a TRIPWIRE, not a model: 100x sits an order of
	 * magnitude above an ordinary router (buffer ~10x its crossbar) and well
	 * below the 6445x the unset wire type produced. */
	{
		static std::set<int> warned_nodes;
		const double buf = router->buffer.power.readOp.dynamic;
		const double xbar = router->crossbar.power.readOp.dynamic;
		const double kAbsurdRatio = 100.0;
		if (xbar > 0.0 && buf / xbar > kAbsurdRatio &&
		    warned_nodes.insert(g_ip->F_sz_nm).second) {
			std::cerr
			  << "[NoC] WARNING: at " << g_ip->F_sz_nm << " nm the router's"
			     " input BUFFER is priced at " << (buf / xbar)
			  << "x its own crossbar (buffer " << buf << " J/access,"
			     " crossbar " << xbar << ", arbiter "
			  << router->arbiter.power.readOp.dynamic << "). A buffer more"
			     " than " << kAbsurdRatio << "x the crossbar it feeds is not"
			     " a physical router, so this level's dynamic power -- and"
			     " every total and peak built on it -- is NOT usable."
			     " Since 1.11.87 (router wire type set, sense voltage"
			     " corrected) the buffer converges at 22, 32 and 65 nm and"
			     " this check clears on DDR3, so this is a NEW imbalance in"
			     " the buffer solve, not the R6-10 defect; please report"
			     " it with the config."
			  << std::endl;
		}
	}

	area.set_area(area.get_area()+ router->area.get_area()*nocdynp.total_nodes);

	double long_channel_device_reduction = longer_channel_device_reduction(Uncore_device);
	double pg_reduction_retain = power_gating_leakage_reduction(false);//state retaining array structure;
	double pg_reduction_nonretain = power_gating_leakage_reduction(false);//non-state-retaining array structure;

	router->power.readOp.longer_channel_leakage          = router->power.readOp.leakage * long_channel_device_reduction;
	router->buffer.power.readOp.longer_channel_leakage   = router->buffer.power.readOp.leakage * long_channel_device_reduction;
	router->crossbar.power.readOp.longer_channel_leakage = router->crossbar.power.readOp.leakage * long_channel_device_reduction;
	router->arbiter.power.readOp.longer_channel_leakage  = router->arbiter.power.readOp.leakage * long_channel_device_reduction;

	router->buffer.power.readOp.power_gated_leakage   = router->buffer.power.readOp.leakage * pg_reduction_retain;//TODO: this is a simplified version; should use the power_gated_leakage generated in buff
	router->crossbar.power.readOp.power_gated_leakage = router->crossbar.power.readOp.leakage * pg_reduction_nonretain;
	router->arbiter.power.readOp.power_gated_leakage  = router->arbiter.power.readOp.leakage * pg_reduction_nonretain;
	router->power.readOp.power_gated_leakage          = router->buffer.power.readOp.power_gated_leakage
														+ router->crossbar.power.readOp.power_gated_leakage
														+ router->arbiter.power.readOp.power_gated_leakage;

	router->buffer.power.readOp.power_gated_with_long_channel_leakage   = router->buffer.power.readOp.power_gated_leakage * long_channel_device_reduction;//TODO: this is a simplified version; should use the power_gated_leakage generated in buff
	router->crossbar.power.readOp.power_gated_with_long_channel_leakage = router->crossbar.power.readOp.power_gated_leakage * long_channel_device_reduction;
	router->arbiter.power.readOp.power_gated_with_long_channel_leakage  = router->arbiter.power.readOp.power_gated_leakage * long_channel_device_reduction;
	router->power.readOp.power_gated_with_long_channel_leakage          = router->buffer.power.readOp.power_gated_with_long_channel_leakage
														+ router->crossbar.power.readOp.power_gated_with_long_channel_leakage
														+ router->arbiter.power.readOp.power_gated_with_long_channel_leakage;

	// Sanitize router power values to prevent NaN/infinity propagation
	// (PIMID 1.11.90: printed and counted, see sanitize_power above).
	if (pimid_fault("router")) router->power.readOp.leakage = NAN;
	const std::string rname = (name.empty() ? std::string("NoC") : name)
	                          + "[" + std::to_string(ithNoC) + "] router";
	router->power.readOp.leakage = sanitize_power(router->power.readOp.leakage, rname, "power.readOp.leakage");
	router->power.readOp.longer_channel_leakage = sanitize_power(router->power.readOp.longer_channel_leakage, rname, "power.readOp.longer_channel_leakage");
	router->power.readOp.power_gated_leakage = sanitize_power(router->power.readOp.power_gated_leakage, rname, "power.readOp.power_gated_leakage");
	router->power.readOp.power_gated_with_long_channel_leakage = sanitize_power(router->power.readOp.power_gated_with_long_channel_leakage, rname, "power.readOp.power_gated_with_long_channel_leakage");
	router->power.readOp.dynamic = sanitize_power(router->power.readOp.dynamic, rname, "power.readOp.dynamic");
	router->buffer.power.readOp.leakage = sanitize_power(router->buffer.power.readOp.leakage, rname, "buffer.power.readOp.leakage");
	router->buffer.power.readOp.longer_channel_leakage = sanitize_power(router->buffer.power.readOp.longer_channel_leakage, rname, "buffer.power.readOp.longer_channel_leakage");
	router->buffer.power.readOp.power_gated_leakage = sanitize_power(router->buffer.power.readOp.power_gated_leakage, rname, "buffer.power.readOp.power_gated_leakage");
	router->buffer.power.readOp.power_gated_with_long_channel_leakage = sanitize_power(router->buffer.power.readOp.power_gated_with_long_channel_leakage, rname, "buffer.power.readOp.power_gated_with_long_channel_leakage");
	router->crossbar.power.readOp.leakage = sanitize_power(router->crossbar.power.readOp.leakage, rname, "crossbar.power.readOp.leakage");
	router->crossbar.power.readOp.power_gated_leakage = sanitize_power(router->crossbar.power.readOp.power_gated_leakage, rname, "crossbar.power.readOp.power_gated_leakage");
	router->crossbar.power.readOp.power_gated_with_long_channel_leakage = sanitize_power(router->crossbar.power.readOp.power_gated_with_long_channel_leakage, rname, "crossbar.power.readOp.power_gated_with_long_channel_leakage");
	router->arbiter.power.readOp.leakage = sanitize_power(router->arbiter.power.readOp.leakage, rname, "arbiter.power.readOp.leakage");
	router->arbiter.power.readOp.power_gated_leakage = sanitize_power(router->arbiter.power.readOp.power_gated_leakage, rname, "arbiter.power.readOp.power_gated_leakage");
	router->arbiter.power.readOp.power_gated_with_long_channel_leakage = sanitize_power(router->arbiter.power.readOp.power_gated_with_long_channel_leakage, rname, "arbiter.power.readOp.power_gated_with_long_channel_leakage");

	router_exist = true;
}

void NoC ::init_link_bus(double link_len_)
{


//	if (nocdynp.min_ports==1 )
	if (nocdynp.type)
		link_name = "Links";
	else
		link_name = "Bus";

	link_len=link_len_;
	assert(link_len>0);

	interface_ip.throughput = nocdynp.link_throughput/nocdynp.clockRate;
	interface_ip.latency = nocdynp.link_latency/nocdynp.clockRate;

	link_len /= (nocdynp.horizontal_nodes + nocdynp.vertical_nodes)/2;

	if (nocdynp.total_nodes >1) link_len /=2; //All links are shared by neighbors
	link_bus = new interconnect(name, Uncore_device, 1, 1, nocdynp.flit_size,
				  link_len, &interface_ip, 3, true/*pipelinable*/, nocdynp.route_over_perc);

	link_bus_tot_per_Router.area.set_area(link_bus_tot_per_Router.area.get_area()+ link_bus->area.get_area()
			* nocdynp.global_linked_ports);

	area.set_area(area.get_area()+ link_bus_tot_per_Router.area.get_area()* nocdynp.total_nodes);
	link_bus_exist = true;
}
void NoC::computeEnergy(bool is_tdp)
{
	//power_point_product_masks
	double pppm_t[4]    = {1,1,1,1};
	double M=nocdynp.duty_cycle;
	if (is_tdp)
	    {
	    	//init stats for TDP
	    	stats_t.readAc.access  = M;
            tdp_stats = stats_t;
            if (router_exist)
            {
            	set_pppm(pppm_t, 1*M, 1, 1, 1);//reset traffic pattern
                router->power = router->power*pppm_t;
                set_pppm(pppm_t, nocdynp.total_nodes, nocdynp.total_nodes, nocdynp.total_nodes, nocdynp.total_nodes);
          	    power     = power + router->power*pppm_t;
            }
            if (link_bus_exist)
            {
            	if (nocdynp.type)
            		set_pppm(pppm_t, 1*M_traffic_pattern*M*(nocdynp.min_ports -1), nocdynp.global_linked_ports,
            			nocdynp.global_linked_ports, nocdynp.global_linked_ports);
            	    //reset traffic pattern; local port do not have router links
            	else
            		set_pppm(pppm_t, 1*M_traffic_pattern*M*(nocdynp.min_ports), nocdynp.global_linked_ports,
            		            			nocdynp.global_linked_ports, nocdynp.global_linked_ports);//reset traffic pattern

            	link_bus_tot_per_Router.power = link_bus->power*pppm_t;

                set_pppm(pppm_t, nocdynp.total_nodes,
                		         nocdynp.total_nodes,
                		         nocdynp.total_nodes,
                		         nocdynp.total_nodes);
            	power     = power + link_bus_tot_per_Router.power*pppm_t;

            }
	    }
	    else
	    {
	    	//init stats for runtime power (RTP)
	    	stats_t.readAc.access  = XML->sys.NoC[ithNoC].total_accesses;
            rtp_stats = stats_t;
        	set_pppm(pppm_t, 1, 0 , 0, 0);
        	/* PIMID 1.11.33: RUNTIME LEAKAGE MISSED THE STRUCTURAL MULTIPLIERS.
        	 *
        	 * The peak branch above scales router leakage by total_nodes, and
        	 * link_bus leakage by global_linked_ports THEN total_nodes. The
        	 * runtime branch scaled both by ONE, so it reported the leakage of a
        	 * single router and a single link where the peak reported the whole
        	 * fabric.
        	 *
        	 * That asymmetry is CORRECT for dynamic and WRONG for leakage.
        	 * Runtime dynamic comes from rtp_stats.readAc.access, a measured
        	 * count that already covers every router in the fabric, so it must
        	 * NOT be multiplied again. Leakage is static and per-router: N
        	 * routers leak N times one router, whatever the traffic. Leakage
        	 * cannot depend on activity at all -- that is what makes this a bug
        	 * and not a convention.
        	 *
        	 * Measured before the fix on a BANK/HBM3 cell: peak 0.0361957 W
        	 * against runtime 0.0060599 W, 5.97x apart. Not an integer because
        	 * the two contributors carry different peak multipliers, so the
        	 * ratio is total_nodes * (R + L*glp) / (R + L).
        	 *
        	 * Upstream McPAT defect, found via the leakage-basis assertion added
        	 * in 1.11.32. It has never moved a PIMID number: extractComponent
        	 * reads leakage from the PEAK basis, which is the correct one. */
        	double pppm_lkg_nodes[4] = {0,
        	                            (double)nocdynp.total_nodes,
        	                            (double)nocdynp.total_nodes,
        	                            0};
        	if (router_exist)
        	{
            	router->buffer.rt_power.readOp.dynamic = (router->buffer.power.readOp.dynamic + router->buffer.power.writeOp.dynamic)*rtp_stats.readAc.access ;
            	router->crossbar.rt_power.readOp.dynamic = router->crossbar.power.readOp.dynamic*rtp_stats.readAc.access ;
            	router->arbiter.rt_power.readOp.dynamic = router->arbiter.power.readOp.dynamic*rtp_stats.readAc.access ;

        		router->rt_power = router->rt_power + (router->buffer.rt_power + router->crossbar.rt_power + router->arbiter.rt_power)*pppm_t +
					router->power*pppm_lkg_nodes;//TDP power must be calculated first!
        		rt_power     = rt_power + router->rt_power;
        	}
        	if (link_bus_exist)
        	{
        		/* dynamic from the measured access count; leakage by the same
        		 * structure the peak path uses: per-router links x nodes. */
        		const double glp_nodes =
        		    (double)nocdynp.global_linked_ports * (double)nocdynp.total_nodes;
        		set_pppm(pppm_t, rtp_stats.readAc.access, glp_nodes, glp_nodes,
        		         rtp_stats.readAc.access);
        		link_bus->rt_power = link_bus->power * pppm_t;
        		rt_power = rt_power + link_bus->rt_power;
        	}

	    }
}


void NoC::displayEnergy(uint32_t indent,int plevel,bool is_tdp)
{
	string indent_str(indent, ' ');
	string indent_str_next(indent+2, ' ');
	bool long_channel = XML->sys.longer_channel_device;
	bool power_gating = XML->sys.power_gating;

	double M =M_traffic_pattern*nocdynp.duty_cycle;
	/*only router as a whole has been applied the M_traffic_pattern(0.6 by default) factor in router.cc;
	 * 	When power of crossbars, arbiters, etc need to be displayed, the M_traffic_pattern factor need to
	 * be applied together with McPAT's extra traffic pattern.
	 * */
	if (is_tdp)
	{
		cout << name << endl;
		cout << indent_str << "Area = " << area.get_area()*1e-6<< " mm^2" << endl;
		cout << indent_str<< "Peak Dynamic = " << power.readOp.dynamic*nocdynp.clockRate << " W" << endl;
		cout << indent_str << "Subthreshold Leakage = "
			<< (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
		if (power_gating) cout << indent_str << "Subthreshold Leakage with power gating = "
						<< (power.readOp.power_gated_leakage * (long_channel? power.readOp.longer_channel_leakage/power.readOp.leakage:1) )  << " W" << endl;
		cout << indent_str << "Gate Leakage = " << power.readOp.gate_leakage << " W" << endl;
		cout << indent_str<< "Runtime Dynamic = " << rt_power.readOp.dynamic/nocdynp.executionTime << " W" << endl;
		cout<<endl;

		if (router_exist)
		{
			cout << indent_str << "Router: " << endl;
			cout << indent_str_next << "Area = " << router->area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next<< "Peak Dynamic = " << router->power.readOp.dynamic*nocdynp.clockRate << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
			<< (long_channel? router->power.readOp.longer_channel_leakage:router->power.readOp.leakage)  <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? router->power.readOp.power_gated_with_long_channel_leakage : router->power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << router->power.readOp.gate_leakage << " W" << endl;
			cout << indent_str_next<< "Runtime Dynamic = " << router->rt_power.readOp.dynamic/nocdynp.executionTime << " W" << endl;
			cout<<endl;
			if (plevel >2){
				cout << indent_str<< indent_str << "Virtual Channel Buffer:" << endl;
				cout << indent_str<< indent_str_next << "Area = " << router->buffer.area.get_area()*1e-6*nocdynp.input_ports<< " mm^2" << endl;
				cout << indent_str<< indent_str_next << "Peak Dynamic = " <<(router->buffer.power.readOp.dynamic + router->buffer.power.writeOp.dynamic)
				*nocdynp.min_ports*M*nocdynp.clockRate << " W" << endl;
				cout << indent_str<< indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? router->buffer.power.readOp.longer_channel_leakage*nocdynp.input_ports:router->buffer.power.readOp.leakage*nocdynp.input_ports)  <<" W" << endl;
				if (power_gating) cout << indent_str<< indent_str_next << "Subthreshold Leakage with power gating = "
						<< (long_channel? router->buffer.power.readOp.power_gated_with_long_channel_leakage : router->buffer.power.readOp.power_gated_leakage)  << " W" << endl;
				cout << indent_str<< indent_str_next << "Gate Leakage = " << router->buffer.power.readOp.gate_leakage*nocdynp.input_ports << " W" << endl;
				cout << indent_str<< indent_str_next << "Runtime Dynamic = " << router->buffer.rt_power.readOp.dynamic/nocdynp.executionTime << " W" << endl;
				cout <<endl;
				cout << indent_str<< indent_str<< "Crossbar:" << endl;
				cout << indent_str<< indent_str_next << "Area = " << router->crossbar.area.get_area()*1e-6  << " mm^2" << endl;
				cout << indent_str<< indent_str_next << "Peak Dynamic = " << router->crossbar.power.readOp.dynamic*nocdynp.clockRate*nocdynp.min_ports*M << " W" << endl;
				cout << indent_str<< indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? router->crossbar.power.readOp.longer_channel_leakage:router->crossbar.power.readOp.leakage)  << " W" << endl;
				if (power_gating) cout << indent_str<< indent_str_next << "Subthreshold Leakage with power gating = "
						<< (long_channel? router->crossbar.power.readOp.power_gated_with_long_channel_leakage : router->crossbar.power.readOp.power_gated_leakage)  << " W" << endl;
				cout << indent_str<< indent_str_next << "Gate Leakage = " << router->crossbar.power.readOp.gate_leakage  << " W" << endl;
				cout << indent_str<< indent_str_next << "Runtime Dynamic = " << router->crossbar.rt_power.readOp.dynamic/nocdynp.executionTime << " W" << endl;
				cout <<endl;
				cout << indent_str<< indent_str<< "Arbiter:" << endl;
				cout << indent_str<< indent_str_next << "Peak Dynamic = " << router->arbiter.power.readOp.dynamic*nocdynp.clockRate*nocdynp.min_ports*M  << " W" << endl;
				cout << indent_str<< indent_str_next << "Subthreshold Leakage = "
				<< (long_channel? router->arbiter.power.readOp.longer_channel_leakage:router->arbiter.power.readOp.leakage)  << " W" << endl;
				if (power_gating) cout << indent_str<< indent_str_next << "Subthreshold Leakage with power gating = "
						<< (long_channel? router->arbiter.power.readOp.power_gated_with_long_channel_leakage : router->arbiter.power.readOp.power_gated_leakage)  << " W" << endl;
				cout << indent_str<< indent_str_next << "Gate Leakage = " << router->arbiter.power.readOp.gate_leakage  << " W" << endl;
				cout << indent_str<< indent_str_next << "Runtime Dynamic = " << router->arbiter.rt_power.readOp.dynamic/nocdynp.executionTime << " W" << endl;
				cout <<endl;
			}
		}
		if (link_bus_exist)
		{
			cout << indent_str << (nocdynp.type? "Per Router ":"") << link_name<<": " << endl;
			cout << indent_str_next << "Area = " << link_bus_tot_per_Router.area.get_area()*1e-6<< " mm^2" << endl;
			cout << indent_str_next<< "Peak Dynamic = " << link_bus_tot_per_Router.power.readOp.dynamic*
				nocdynp.clockRate << " W" << endl;
			cout << indent_str_next << "Subthreshold Leakage = "
			<< (long_channel? link_bus_tot_per_Router.power.readOp.longer_channel_leakage:link_bus_tot_per_Router.power.readOp.leakage)
			     <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? link_bus_tot_per_Router.power.readOp.power_gated_with_long_channel_leakage : link_bus_tot_per_Router.power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << link_bus_tot_per_Router.power.readOp.gate_leakage
				<< " W" << endl;
			cout << indent_str_next<< "Runtime Dynamic = " << link_bus->rt_power.readOp.dynamic/nocdynp.executionTime << " W" << endl;
			cout<<endl;

		}
	}
	else
	{
//		cout << indent_str_next << "Instruction Fetch Unit    Peak Dynamic = " << ifu->rt_power.readOp.dynamic*clockRate << " W" << endl;
//		cout << indent_str_next << "Instruction Fetch Unit    Subthreshold Leakage = " << ifu->rt_power.readOp.leakage <<" W" << endl;
//		cout << indent_str_next << "Instruction Fetch Unit    Gate Leakage = " << ifu->rt_power.readOp.gate_leakage << " W" << endl;
//		cout << indent_str_next << "Load Store Unit   Peak Dynamic = " << lsu->rt_power.readOp.dynamic*clockRate  << " W" << endl;
//		cout << indent_str_next << "Load Store Unit   Subthreshold Leakage = " << lsu->rt_power.readOp.leakage  << " W" << endl;
//		cout << indent_str_next << "Load Store Unit   Gate Leakage = " << lsu->rt_power.readOp.gate_leakage  << " W" << endl;
//		cout << indent_str_next << "Memory Management Unit   Peak Dynamic = " << mmu->rt_power.readOp.dynamic*clockRate  << " W" << endl;
//		cout << indent_str_next << "Memory Management Unit   Subthreshold Leakage = " << mmu->rt_power.readOp.leakage  << " W" << endl;
//		cout << indent_str_next << "Memory Management Unit   Gate Leakage = " << mmu->rt_power.readOp.gate_leakage  << " W" << endl;
//		cout << indent_str_next << "Execution Unit   Peak Dynamic = " << exu->rt_power.readOp.dynamic*clockRate  << " W" << endl;
//		cout << indent_str_next << "Execution Unit   Subthreshold Leakage = " << exu->rt_power.readOp.leakage  << " W" << endl;
//		cout << indent_str_next << "Execution Unit   Gate Leakage = " << exu->rt_power.readOp.gate_leakage  << " W" << endl;
	}
}

void NoC::set_noc_param()
{

	nocdynp.type            = XML->sys.NoC[ithNoC].type;
	nocdynp.clockRate       =XML->sys.NoC[ithNoC].clockrate;
	nocdynp.clockRate       *= 1e6;
	nocdynp.executionTime   = XML->sys.total_cycles/(XML->sys.target_core_clockrate*1e6);

	nocdynp.flit_size     = XML->sys.NoC[ithNoC].flit_bits;
	if (nocdynp.type)
	{
		nocdynp.input_ports   = XML->sys.NoC[ithNoC].input_ports;
		nocdynp.output_ports  = XML->sys.NoC[ithNoC].output_ports;//later minus 1
		nocdynp.min_ports     = min(nocdynp.input_ports,nocdynp.output_ports);
		nocdynp.global_linked_ports = (nocdynp.input_ports-1) + (nocdynp.output_ports-1);
		/*
		 * 	Except local i/o ports, all ports needs links( global_linked_ports);
		 *  However only min_ports can be fully active simultaneously
		 *  since the fewer number of ports (input or output ) is the bottleneck.
		 */
	}
	else
	{
		nocdynp.input_ports   = 1;
		nocdynp.output_ports  = 1;
		nocdynp.min_ports     = min(nocdynp.input_ports,nocdynp.output_ports);
		nocdynp.global_linked_ports = 1;
	}

	nocdynp.virtual_channel_per_port     = XML->sys.NoC[ithNoC].virtual_channel_per_port;
	nocdynp.input_buffer_entries_per_vc  = XML->sys.NoC[ithNoC].input_buffer_entries_per_vc;

	nocdynp.horizontal_nodes  = XML->sys.NoC[ithNoC].horizontal_nodes;
	nocdynp.vertical_nodes    = XML->sys.NoC[ithNoC].vertical_nodes;
	nocdynp.total_nodes       = nocdynp.horizontal_nodes*nocdynp.vertical_nodes;
	nocdynp.duty_cycle        = XML->sys.NoC[ithNoC].duty_cycle;
	nocdynp.has_global_link   = XML->sys.NoC[ithNoC].has_global_link;
	nocdynp.link_throughput   = XML->sys.NoC[ithNoC].link_throughput;
	nocdynp.link_latency      = XML->sys.NoC[ithNoC].link_latency;
	nocdynp.chip_coverage     = XML->sys.NoC[ithNoC].chip_coverage;
	nocdynp.route_over_perc   = XML->sys.NoC[ithNoC].route_over_perc;

	assert (nocdynp.chip_coverage <=1);
	assert (nocdynp.route_over_perc <=1);

	if (nocdynp.type)
		name = "NOC";
	else
		name = "BUSES";

	if ( XML->sys.NoC[ithNoC].vdd>0)
	{
		interface_ip.specific_hp_vdd = true;
		interface_ip.specific_lop_vdd = true;
		interface_ip.specific_lstp_vdd = true;
		interface_ip.hp_Vdd   = XML->sys.NoC[ithNoC].vdd;
		interface_ip.lop_Vdd  = XML->sys.NoC[ithNoC].vdd;
		interface_ip.lstp_Vdd = XML->sys.NoC[ithNoC].vdd;
	}

	if ( XML->sys.NoC[ithNoC].power_gating_vcc > -1)
	{
		interface_ip.specific_vcc_min = true;
		interface_ip.user_defined_vcc_min   = XML->sys.NoC[ithNoC].power_gating_vcc;

	}

}


NoC ::~NoC(){

	if(router) 	               {delete router; router = 0;}
	if(link_bus) 	           {delete link_bus; link_bus = 0;}
}
