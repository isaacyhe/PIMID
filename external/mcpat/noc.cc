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
#include <map>
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
link_wire(0),
link_tsv(0),
link_is_tsv(false),
link_bus_exist(false),
router_exist(false),
M_traffic_pattern(M_traffic_pattern_),
clk_rt_ungated(0), clk_rt_global(0),
router_accesses(0)
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
	/* PIMID 1.12.18: an instance priced at its own process node (a link
	 * level on the DRAM die vs on logic). Every CACTI table this instance
	 * reads -- wire RC, repeaters, TSV geometry, device leakage -- follows. */
	if (XML->sys.NoC[ithNoC].tech_node > 0)
	{
		interface_ip.F_sz_nm = XML->sys.NoC[ithNoC].tech_node;
		interface_ip.F_sz_um = interface_ip.F_sz_nm / 1000;
	}
	local_result=init_interface(&interface_ip);
	scktRatio = g_tp.sckt_co_eff;

	if (nocdynp.type)
	{/*
		 * if NOC compute router, router links must be computed separately
		 * and called from external
		 * since total chip area must be known first
		 */
		/* PIMID 1.12.18: a links-only instance (routers_present=0) builds
		 * no router: it prices the wires of a level whose routers are
		 * priced elsewhere, or that has none worth a crossbar. */
		if (XML->sys.NoC[ithNoC].routers_present) init_router();
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
	/* PIMID 1.12.18: THE ROUTER'S CLOCK NETWORK (Wattch, D. Brooks,
	 * V. Tiwari, M. Martonosi, ISCA 2000, clocking section; logic.h
	 * ClockNetwork). The disabled block above (a core's pipeline count and
	 * a 1.1 placement preset) is not used.
	 *   Tree area: one router's CACTI area.
	 *   Sinks: the priority flip-flops of the router's matrix arbiters,
	 *   the arbiters CACTI prices (cacti/router.cc: one vc_count-input
	 *   arbiter per input port, one I-input arbiter per output port; a
	 *   matrix arbiter of n inputs holds n(n-1)/2 state flip-flops -- W. J.
	 *   Dally, B. Towles, "Principles and Practices of Interconnection
	 *   Networks", Morgan Kaufmann 2004, matrix arbiter; CACTI's arbiter.cc
	 *   notes their switching is ignored, so their pins are charged nowhere
	 *   else). The flit buffers are CACTI arrays priced per access. */
	if (nocdynp.type && router)
	{
		const double I = nocdynp.input_ports, O = nocdynp.output_ports, V = router->vc_count;
		const double ff = I*V*(V-1)/2 + O*I*(I-1)/2;
		std::ostringstream src;
		src << "matrix-arbiter flip-flops " << I << "x" << V << "(" << V << "-1)/2 + " << O << "x" << I
		    << "(" << I << "-1)/2";
		pimid::debugStream(std::cerr) << "NO PUBLISHED SOURCE FOUND: router pipeline/output register count (CACTI's router model "
		             "counts no pipeline register); the router clock tree is priced over its arbiter flip-flops "
		             "only -- a lower bound." << std::endl;
		clk_router.compute(&interface_ip, name + std::to_string(ithNoC) + " router", router->area.get_area(), ff,
		                   src.str(), -1.0, nocdynp.clockRate, XML->sys.Embedded, Uncore_device, Inorder,
		                   XML->sys.pimid_clock_network != 0);
	}
	if (!nocdynp.type) build_link_clock();
}

/* PIMID 1.12.18: a link or bus segment's clock network (Wattch, D. Brooks,
 * V. Tiwari, M. Martonosi, ISCA 2000, clocking section; logic.h
 * ClockNetwork). Sinks: the pipeline registers McPAT's interconnect model
 * inserts when the wire is slower than the link throughput
 * (interconnect.cc: num_pipe_stages = ceil(delay / throughput); the
 * registers between stages are (num_pipe_stages - 1) x data width, a count
 * that model computes but does not price). A segment with no inserted stage
 * has no register of its own -- its endpoints' registers belong to the
 * components it joins -- and so no tree. */
void NoC::build_link_clock()
{
	/* PIMID 1.12.18 (merge): link_bus is a wire (interconnect) or a TSV
	 * segment; only a wire inserts pipeline registers. */
	if (!link_bus || XML->sys.NoC[ithNoC].link_kind == 1 || XML->sys.NoC[ithNoC].link_kind == 2) return;   // 1 = TSV segment, 2 = channel I/O: no on-die pipeline register
	interconnect* wire = static_cast<interconnect*>(link_bus);
	const double st = (wire->num_pipe_stages > 1) ? wire->num_pipe_stages - 1 : 0;
	std::ostringstream src;
	src << "link pipeline registers (" << wire->num_pipe_stages << " stage(s) - 1) x "
	    << nocdynp.flit_size << " bits";
	clk_link.compute(&interface_ip, name + std::to_string(ithNoC) + " link", link_bus->area.get_area(),
	                 st * nocdynp.flit_size, src.str(), -1.0, nocdynp.clockRate, XML->sys.Embedded,
	                 Uncore_device, Inorder, XML->sys.pimid_clock_network != 0);
}

void init_tech_params(double tech, bool is_tag);   /* PIMID (1.12.150): cacti/technology.cc */

/* PIMID (1.12.150): THE ROUTER MODEL, CALIBRATED ON PUBLISHED ROUTERS, PER SHAPE.
 *
 * CACTI's router (cacti/router.cc) builds an input buffer, a matrix crossbar
 * and the arbiters, and nothing else. Its area is compared with every
 * published router whose shape (node, ports, flit width, buffering) and
 * router area are both given, computed with the same model at that shape and
 * node in this instance's own context (wire type, temperature, device type).
 * Low radix (<= 8 ports), buffered, router area (clock f, f x L in GHz nm):
 *   [C] Celerity, 16 nm, 1.4 GHz, 5 p, 80 b, one 2-entry FIFO: 1881 um^2
 *       (Rovinski et al., VLSI 2019); no 16 nm CACTI solution at 2 entries:
 *       the model at 22 nm x (16/22)^2 (DERIVED);
 *   [O] Piton, 32 nm, 500 MHz, 5 p, 64 b, 4-entry FIFO: 0.98% of a
 *       1.17459 mm^2 tile = 0.0115 mm^2 (DERIVED; McKeown et al., HPCA 2018;
 *       OpenPiton microarchitecture specification);
 *   [S] MIT SWIFT, 90 nm, 400 MHz, 5 p, 64 b, 2 VCs, 8 buffers per port:
 *       0.150 mm^2 (Krishna et al., ICCD 2010; Krishna, PhD thesis, 2014);
 *   [F] MIT 16-node chip, 45 nm SOI, 1 GHz, 5 p, 64 b, 6 VCs, 10 buffers per
 *       port, full-swing crossbar: 0.2272 mm^2 (Park et al., DAC 2012);
 *   [D] DSENT validation router, 45 nm SOI, 1 GHz, 6 p, 64 b, 8 VCs, 16
 *       buffers per port: 0.070 mm^2 post-P&R (Sun et al., NOCS 2012);
 *   [K] single-cycle router, 65 nm, 3.6 GHz, 5 p, 128 b, 15 VCs, 32 buffers
 *       per port: 1.19 mm^2 layout estimate (Kumar et al., ICCD 2007);
 *   [P] Intel 80-tile, 65 nm, 4 GHz, 5 p, 39 b, 2 lanes x 16 flits: 0.34 mm^2
 *       (Vangal et al., ISSCC 2007 5.2), full-custom.
 * The published / model ratio spans 0.5 .. 9 and rises with the clock the
 * router was built for relative to its node: the synthesized ~1 GHz routers
 * ([C][O][S][F][D], f x L 16..45) against the 3.6..4 GHz ones ([K][P],
 * 234..260). k(fL) = the geometric mean of the first group's ratios at
 * f x L <= its geometric-mean f x L, the second group's at >= theirs,
 * log-log interpolated between (DERIVED), at this instance's clock and node.
 * High radix (>= 16 ports): the crossbar scaled by kW, the geometric mean of
 * four published crossbars' published / model ratios, plus the input buffers
 * scaled by k(fL); the model's bounding box (ports x buffer height x
 * (buffer + crossbar width)) is not used there:
 *   [W1] Swizzle-Switch 64x64 x 128 b, 45 nm, 559 MHz: 4.06 mm^2 (Satpathy
 *        et al., ISSCC 2012); [W2] 32x32 x 64 b, 65 nm, 1026 MHz: 0.35 mm^2
 *        (Satpathy et al., VLSI 2011); [W3] Swizzle-Switch 64x64 x 128 b,
 *        32 nm layout: 1.1 mm^2 (K. Sewell, PhD thesis, U. Michigan 2012);
 *        [W4] mux crossbar 16x16 x 72 b, 40 nm, 2.5 GHz post-layout:
 *        0.32 mm^2 (C. Cakir, PhD thesis, CMU 2016), the model at 45 nm x
 *        (40/45)^2 (DERIVED).
 * 9..15 ports and 2-port datapaths wider than 128 b have no published point:
 * priced by the low-radix rule; the run says NO PUBLISHED SOURCE FOUND.
 * Leakage is taken proportional to area, per part (DERIVED). Cross-check:
 * [P]'s estimated router leakage, 82 mW at 1.2 V and 110 C (Hot Chips 19).
 * Dynamic energy per flit is not scaled (judged per bit-hop by the power
 * checker against [P] and the Intel SCC router). */
struct PimidRouterCal { double k_lo, k_hi, fl_lo, fl_hi, kW; double r[7]; };
static PimidRouterCal pimid_router_cal(double M)
{
	static std::map<std::string, PimidRouterCal> cache;
	const std::string key = std::to_string((int)g_ip->wt) + ":" + std::to_string(g_ip->temp) + ":"
	                      + std::to_string(g_ip->data_arr_peri_global_tech_type) + ":" + std::to_string(M);
	auto it = cache.find(key);
	if (it != cache.end()) return it->second;
	const TechnologyParameter tp_saved = g_tp;
	const double f_um = g_ip->F_sz_um, f_nm = g_ip->F_sz_nm;
	auto at = [&](double nm) { g_ip->F_sz_um = nm / 1000.0; g_ip->F_sz_nm = nm; init_tech_params(nm / 1000.0, false); };
	/* low radix: node for the model, node scaling, flit, rows (VCs x depth), VCs, ports, published mm^2, f GHz, published node */
	struct Pt { double nm, flit, rows, vcs, ports, mm2, ghz, pub_nm; };
	const Pt pts[7] = {
		{22, 80, 2, 1, 5, 0.001881, 1.4, 16},   /* [C] */
		{32, 64, 4, 1, 5, 0.0115, 0.5, 32},     /* [O] */
		{90, 64, 8, 2, 5, 0.150431, 0.4, 90},   /* [S] */
		{45, 64, 10, 6, 5, 0.2272, 1.0, 45},    /* [F] */
		{45, 64, 16, 8, 6, 0.070, 1.0, 45},     /* [D] */
		{65, 128, 32, 15, 5, 1.19, 3.6, 65},    /* [K] */
		{65, 39, 32, 2, 5, 0.34, 4.0, 65}};     /* [P] */
	PimidRouterCal c;
	double lk_lo = 0, lf_lo = 0, lk_hi = 0, lf_hi = 0; int n_lo = 0, n_hi = 0;
	for (int i = 0; i < 7; i++) {
		at(pts[i].nm);
		Router rr(pts[i].flit, pts[i].rows, pts[i].vcs, &(g_tp.peri_global), pts[i].ports, pts[i].ports, M);
		const double am = rr.area.get_area() * 1e-6 * (pts[i].pub_nm / pts[i].nm) * (pts[i].pub_nm / pts[i].nm);
		c.r[i] = (am > 0) ? pts[i].mm2 / am : 1.0;
		const double fl = pts[i].ghz * pts[i].pub_nm;
		if (fl < 100) { lk_lo += log(c.r[i]); lf_lo += log(fl); n_lo++; }
		else          { lk_hi += log(c.r[i]); lf_hi += log(fl); n_hi++; }
	}
	c.k_lo = exp(lk_lo / n_lo); c.fl_lo = exp(lf_lo / n_lo);
	c.k_hi = exp(lk_hi / n_hi); c.fl_hi = exp(lf_hi / n_hi);
	at(45); Router rw(128, 1, 1, &(g_tp.peri_global), 64, 64, M);
	const double aW1 = rw.crossbar.area.get_area() * 1e-6;
	Router rw4(72, 1, 1, &(g_tp.peri_global), 16, 16, M);
	const double aW4 = rw4.crossbar.area.get_area() * 1e-6 * (40.0 / 45.0) * (40.0 / 45.0);
	at(65); Router rw2(64, 1, 1, &(g_tp.peri_global), 32, 32, M);
	const double aW2 = rw2.crossbar.area.get_area() * 1e-6;
	at(32); Router rw3(128, 1, 1, &(g_tp.peri_global), 64, 64, M);
	const double aW3 = rw3.crossbar.area.get_area() * 1e-6;
	g_ip->F_sz_um = f_um; g_ip->F_sz_nm = f_nm;
	g_tp = tp_saved;
	c.kW = (aW1 > 0 && aW2 > 0 && aW3 > 0 && aW4 > 0)
	     ? pow((4.06 / aW1) * (0.35 / aW2) * (1.1 / aW3) * (0.32 / aW4), 0.25) : 1.0;
	cache[key] = c;
	return c;
}
static double pimid_router_k(const PimidRouterCal& c, double fl)
{
	if (fl <= c.fl_lo) return c.k_lo;
	if (fl >= c.fl_hi) return c.k_hi;
	const double t = log(fl / c.fl_lo) / log(c.fl_hi / c.fl_lo);
	return exp(log(c.k_lo) + t * (log(c.k_hi) - log(c.k_lo)));
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

	{   /* PIMID (1.12.150): the published-router calibration, per shape (pimid_router_cal) */
		const PimidRouterCal c = pimid_router_cal(M_traffic_pattern);
		const double I = nocdynp.input_ports;
		const double fl = (nocdynp.clockRate / 1e9) * g_ip->F_sz_nm;   /* GHz x nm */
		const double k = pimid_router_k(c, fl);
		const bool high = (nocdynp.input_ports >= 16);
		const bool unsourced = (nocdynp.input_ports > 8 && nocdynp.input_ports < 16) ||
		                       (nocdynp.input_ports <= 2 && nocdynp.flit_size > 128);
		const double kx = high ? c.kW : k;
		const double a0 = router->area.get_area() * 1e-6, l0 = router->power.readOp.leakage;
		const double a_buf = router->buffer.area.get_area() * 1e-6 * I, a_x = router->crossbar.area.get_area() * 1e-6;
		const double a1 = high ? (kx * a_x + k * a_buf) : (k * a0);
		const double lb = router->buffer.power.readOp.leakage * I, lx = router->crossbar.power.readOp.leakage,
		             la = router->arbiter.power.readOp.leakage;
		const double lsum = lb + lx + la;
		const double r = (lsum > 0) ? (k * lb + kx * lx + k * la) / lsum : k;
		router->area.h = sqrt(a1 * 1e6);
		router->area.w = sqrt(a1 * 1e6);
		router->power.readOp.leakage          *= r;
		router->power.readOp.gate_leakage     *= r;
		router->buffer.power.readOp.leakage   *= k;
		router->buffer.power.readOp.gate_leakage   *= k;
		router->crossbar.power.readOp.leakage *= kx;
		router->crossbar.power.readOp.gate_leakage *= kx;
		router->arbiter.power.readOp.leakage  *= k;
		router->arbiter.power.readOp.gate_leakage  *= k;
		const double bits = I * nocdynp.virtual_channel_per_port * nocdynp.input_buffer_entries_per_vc * nocdynp.flit_size;
		pimid::debugStream(std::cerr) << "  [noc] noc" << ithNoC << " router (" << nocdynp.input_ports << " ports, "
		          << nocdynp.flit_size << " b, " << nocdynp.virtual_channel_per_port << " VCs x "
		          << nocdynp.input_buffer_entries_per_vc << " entries, " << g_ip->F_sz_nm << " nm, "
		          << nocdynp.clockRate / 1e6 << " MHz): model " << a0 << " mm^2, leakage " << l0 << " W; "
		          << (high ? "high radix: crossbar x kW + buffers x k" : "x k") << "; k " << k << " kW " << c.kW
		          << " = " << a1 << " mm^2, leakage " << router->power.readOp.leakage << " W; buffered bits " << bits
		          << ", " << (bits > 0 ? a1 * 1e6 / bits : 0.0) << " um^2/bit; crossbar " << a_x * kx << " mm^2 for "
		          << nocdynp.input_ports << "x" << nocdynp.output_ports << "x" << nocdynp.flit_size << " b. f x L "
		          << fl << " GHz nm; published/model: Celerity " << c.r[0] << ", Piton " << c.r[1] << ", SWIFT " << c.r[2]
		          << ", MIT 16-node " << c.r[3] << ", DSENT router " << c.r[4] << ", Kumar 2007 " << c.r[5] << ", Intel 80-tile "
		          << c.r[6] << " (k = " << c.k_lo << " at f x L <= " << c.fl_lo << ", " << c.k_hi << " at >= " << c.fl_hi
		          << ", log-log between; DERIVED), high-radix crossbar kW (geomean of 4 published crossbars) " << c.kW
		          << (unsourced ? "; NO PUBLISHED SOURCE FOUND for a router of this shape (priced by the low-radix rule)" : "")
		          << ". Per-flit dynamic not scaled." << std::endl;
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

	/* PIMID 1.12.18: an EXPLICIT length is the length of one link and is
	 * used as is. The two divisions below turn a die span into a MESH
	 * pitch (span / nodes per side / 2); applying them to a length that is
	 * already per link would shrink it a second time. */
	const bool explicit_len = (XML->sys.NoC[ithNoC].link_span_area_mm2 > 0 &&
	                           XML->sys.NoC[ithNoC].link_parent_regions > 0)
	                          || XML->sys.NoC[ithNoC].link_span_processor;   /* 1.12.23 */
	if (!explicit_len)
	{
	link_len /= (nocdynp.horizontal_nodes + nocdynp.vertical_nodes)/2;

	if (nocdynp.total_nodes >1) link_len /=2; //All links are shared by neighbors
	}
	if (XML->sys.NoC[ithNoC].link_kind == 2)
	{
		/* PIMID (user ruling 2026-10-25): an OFF-PACKAGE device-tree link (a DRAM chip's up-link to
		 * the rank, and the rank / channel / system levels of a non-stacked part) rides the DRAM channel
		 * on the DIMM or board, not on-die wire. It is priced per bit crossed at the energy PIMID passes
		 * (the part's own DQ crossing per bit: termination + IDDQ of a read crossing, the memory model's
		 * DQ rule), x the level's flit bits per traversal. No on-die wire, no repeater leakage, no area
		 * here: the DQ drivers and receivers are the DRAM's and the buffer's own I/O. */
		const double bits = nocdynp.flit_size;
		const double pj = XML->sys.NoC[ithNoC].link_io_pj_per_bit;
		if (!(pj > 0.0)) {
			std::cerr << "[link] FATAL (McPAT PIMID): noc" << ithNoC << " link_kind 2 (channel I/O) without a"
			             " positive link_io_pj_per_bit (" << pj << ")." << std::endl;
			exit(2);
		}
		link_io_flit.power.reset();
		link_io_flit.power.readOp.dynamic = pj * 1e-12 * bits;
		link_io_flit.area.set_area(0.0);
		link_bus = &link_io_flit;
		link_is_tsv = false;
		pimid::debugStream(std::cerr) << "  [link] noc" << ithNoC << " channel I/O (off-package link over the DRAM channel): "
		          << pj << " pJ/bit x " << bits << " b = " << link_io_flit.power.readOp.dynamic
		          << " J per flit crossing; no on-die wire (McPAT length " << link_len << " um not used), no leakage,"
		          << " no area" << std::endl;
	}
	else if (XML->sys.NoC[ithNoC].link_kind == 1)
	{
		/* PIMID 1.12.18: a die crossing, priced as a CACTI-3DD TSV
		 * (Chen et al., "CACTI-3DD: Architecture-level modeling for 3D
		 * die-stacked DRAM main memory", DATE 2012; external/cacti/TSV.cc):
		 * R and C from the TSV geometry of the technology table, a
		 * logical-effort driver chain, dynamic = sum C x Vdd^2 per bit per
		 * crossing, leakage of the driver chain, area max(pitch^2, driver
		 * + metal). The COARSE column is the one CACTI labels "Industry
		 * reported in 2010" (cacti_interface.h); the fine one is the ITRS
		 * projection. CACTI's input defaults put the fine column in BOTH
		 * slots (tsv_os_bank_type = 0), so the coarse slot is selected
		 * here, and g_tp is re-initialised from this instance's interface
		 * immediately before the TSV is built: g_tp is global and the
		 * component built last may have left another node's table in it.
		 * A link is flit_bits TSVs; num_die_3d = 1 makes each TSV ONE die
		 * thick (CACTI multiplies its length by the die count), and the
		 * runtime charges it once per die a flit actually crosses (the
		 * tsv_die_crossings stat), so the die count lives in the activity,
		 * not in the TSV. */
		interface_ip.tsv_is_subarray_type = Fine;
		interface_ip.tsv_os_bank_type = Coarse;
		interface_ip.num_die_3d = 1;
		init_interface(&interface_ip);
		/* PIMID (user ruling 2026-10-25): the TSV RE-SOURCED from published HBM silicon.
		 *  pitch   30 um: "two-dimensional TSV arrays of signal, power, and ground whose pitch is 30 um"
		 *          (Samsung HBM2E, JSSC 56(1), Jan. 2021, Sec. III) -> area per TSV = pitch^2;
		 *  length  ~30 um: "The thickness of the core dies ... is approximately about 30 um" (SK hynix
		 *          192-Gb 12-high HBM3, JSSC 58(1), 2023, Sec. VIII) -- printed; the load below is the
		 *          measured one, so the length enters no number;
		 *  load    C per die-crossing 118.5 fF DERIVED from Cho et al. (SK hynix), ISSCC 2018 12.3,
		 *          Fig. 12.3.1: IDD per TSV 880 uA at 1.0 V, 3.3 Gb/s PRBS, conventional multi-drop TSV of
		 *          an 8-high stack (9 TX/RX loads: 8 core dies + base). Energy per bit 880 uA x 1.0 V /
		 *          3.3 Gb/s = 0.267 pJ; a PRBS bit charges the line (0 -> 1) with probability 1/4, so
		 *          C_column = 4 x 0.267 pJ / (1.0 V)^2 = 1.07 pF, / 9 loads = 118.5 fF per die segment
		 *          (TSV + micro-bump + one die's TX/RX). It includes the drivers' own switching, so the
		 *          CACTI driver chain is not charged on top; the chain (sized for this load) keeps its
		 *          leakage. The spiral point-to-point TSV of the same paper draws 610 uA (-30%), printed
		 *          as the alternative structure.
		 *  supply  the stacked part's core VDD (tsv_vdd, HBM3 1.1 V, part record), else the run's Vdd.
		 * Dynamic per bit per die crossing = C x V^2: the fork's every-bit-charges convention, the
		 * basis of the CACTI TSV and wires it replaces (a PRBS bit costs 1/4 of it). */
		const double pimid_tsv_c_load = 4.0 * 880e-6 * 1.0 / 3.3e9 / (1.0 * 1.0) / 9.0;   // F, DERIVED
		const double pimid_tsv_c_spiral = 4.0 * 610e-6 * 1.0 / 3.3e9 / (1.0 * 1.0) / 9.0;  // F, reference
		const double pimid_tsv_pitch_um = 30.0, pimid_tsv_len_um = 30.0;
		const double cacti_c_table = g_tp.tsv_parasitic_capacitance_coarse;
		const double cacti_area_table = g_tp.tsv_minimum_area_coarse;
		const double cacti_pitch_table = g_tp.tsv_pitch, cacti_len_table = g_tp.tsv_length;
		g_tp.tsv_parasitic_capacitance_coarse = pimid_tsv_c_load;
		g_tp.tsv_minimum_area_coarse = pimid_tsv_pitch_um * pimid_tsv_pitch_um;
		link_tsv = new TSV(Coarse);
		const double cacti_chain_dyn = link_tsv->power.readOp.dynamic;   // chain + load at the logic Vdd (reference)
		g_tp.tsv_parasitic_capacitance_coarse = cacti_c_table;
		g_tp.tsv_minimum_area_coarse = cacti_area_table;
		const double tsv_v = (XML->sys.NoC[ithNoC].tsv_vdd > 0.0) ? XML->sys.NoC[ithNoC].tsv_vdd
		                                                          : g_tp.peri_global.Vdd;   // the TSV driver default device (TSV.h)
		link_tsv->power.readOp.dynamic = pimid_tsv_c_load * tsv_v * tsv_v;
		const double bits = nocdynp.flit_size;
		const double lcr = longer_channel_device_reduction(Uncore_device);
		const double pgr = power_gating_leakage_reduction(false);
		link_tsv_flit.power.reset();
		link_tsv_flit.power.readOp.dynamic = link_tsv->power.readOp.dynamic * bits;
		link_tsv_flit.power.readOp.leakage = link_tsv->power.readOp.leakage * bits;
		link_tsv_flit.power.readOp.gate_leakage = link_tsv->power.readOp.gate_leakage * bits;
		link_tsv_flit.power.readOp.longer_channel_leakage = link_tsv_flit.power.readOp.leakage * lcr;
		link_tsv_flit.power.readOp.power_gated_leakage = link_tsv_flit.power.readOp.leakage * pgr;
		link_tsv_flit.power.readOp.power_gated_with_long_channel_leakage =
			link_tsv_flit.power.readOp.power_gated_leakage * lcr;
		link_tsv_flit.area.set_area(link_tsv->area.get_area() * bits);
		link_bus = &link_tsv_flit;
		link_is_tsv = true;
		/* stderr: the fork's stdout is /dev/null (see wrapper). */
		pimid::debugStream(std::cerr) << "  [link] noc" << ithNoC << " TSV RE-SOURCED: pitch " << pimid_tsv_pitch_um
		          << " um [HBM2E JSSC 2021], length " << pimid_tsv_len_um << " um [HBM3 JSSC 2023 core die],"
		          << " load " << pimid_tsv_c_load * 1e15 << " fF per die crossing DERIVED [Cho ISSCC 2018 Fig."
		          << " 12.3.1: 880 uA/TSV, 1.0 V, 3.3 Gb/s PRBS, 9 loads; spiral P2P 610 uA -> "
		          << pimid_tsv_c_spiral * 1e15 << " fF], supply " << tsv_v << " V"
		          << ((XML->sys.NoC[ithNoC].tsv_vdd > 0.0) ? " (the part's core VDD)" : " (the run's logic Vdd)")
		          << ": " << link_tsv->power.readOp.dynamic << " J per bit per crossing (C V^2; a PRBS bit "
		          << 0.25 * link_tsv->power.readOp.dynamic << " J); CACTI-3DD table before: C "
		          << cacti_c_table * 1e15 << " fF, pitch " << cacti_pitch_table << " um, length "
		          << cacti_len_table << " um; CACTI chain + new load at the logic Vdd " << cacti_chain_dyn
		          << " J (reference, not charged)" << std::endl;
		pimid::debugStream(std::cerr) << "  [link] noc" << ithNoC << " TSV driver chain and table geometry (CACTI-3DD, Coarse = industry"
		          << " reported, interconnect projection " << g_ip->ic_proj_type
		          << ", " << g_ip->F_sz_nm << " nm table): pitch " << g_tp.tsv_pitch
		          << " um, diameter " << g_tp.tsv_diameter << " um, length "
		          << g_tp.tsv_length << " um; C " << link_tsv->cap * 1e15
		          << " fF, R " << link_tsv->res * 1e3 << " mOhm; driver stages "
		          << link_tsv->num_gates << "; per TSV: dynamic "
		          << link_tsv->power.readOp.dynamic << " J/crossing, leakage "
		          << link_tsv->power.readOp.leakage << " W, gate leakage "
		          << link_tsv->power.readOp.gate_leakage << " W, area "
		          << link_tsv->area.get_area() << " um^2; x " << bits
		          << " b = " << link_tsv_flit.power.readOp.dynamic
		          << " J per flit crossing" << std::endl;
	}
	else
	{
		/* PIMID (user ruling 2026-10-25): NoC links and buses on ENERGY-EFFICIENT repeaters, CACTI
		 * wire type Global_ho: 0.8x the delay-optimal repeater size at 1.4x its spacing, the published
		 * design point of R. Ho (PhD thesis, Stanford, 2003, Sec. 4.2: a 4% delay penalty for ~20% less
		 * energy). The delay-optimal Global wire put 1.44x the wire's own capacitance in repeaters (22 nm),
		 * and the DSENT authors list min-delay link sizing among the causes of a 5X+ link power
		 * overestimate (Sun et al., NOCS 2012 talk). CACTI's Global_30 (lowest cost within a 30% delay
		 * penalty, on a 100 um spacing grid) was the other candidate: a larger delay penalty than any
		 * published NoC link rule, so not used. */
		link_wire = new interconnect(name, Uncore_device, 1, 1, nocdynp.flit_size,
				  link_len, &interface_ip, 3, true/*pipelinable*/, nocdynp.route_over_perc,
				  true, Inorder, Global_ho);
		link_bus = link_wire;
		/* PIMID 1.12.18: the per-link price, printed for the hand check
		 * (energy per bit-mm = flit energy / bits / length). A bus level keeps
		 * its upstream behaviour; its line is printed below (user ruling
		 * 2026-10-25, print only). */
		if (nocdynp.type)
		{
			const double bits = nocdynp.flit_size;
			pimid::debugStream(std::cerr) << "  [link] noc" << ithNoC << " wire (McPAT interconnect, CACTI "
			          << "wire model, type Global_ho (repeaters 0.8x size, 1.4x spacing of delay-optimal; Ho 2003), interconnect projection "
			          << g_ip->ic_proj_type << ", " << g_ip->F_sz_nm << " nm table): "
			          << "length " << link_len << " um ("
			          << ((XML->sys.NoC[ithNoC].link_span_processor == 2)
			                  ? "2 x sqrt(die area) / stops, ring loop"   /* PIMID (user ruling 2026-10-25) */
			                  : (XML->sys.NoC[ithNoC].link_span_processor == 3)
			                  ? "sqrt(die area / tiles), mesh tile pitch"   /* PIMID (1.12.150) */
			                  : explicit_len ? "0.5 x sqrt(span / parent regions), H-tree"
			                                 : "upstream mesh derivation")
			          << "), " << bits << " b; per flit: dynamic "
			          << link_wire->power.readOp.dynamic << " J, leakage "
			          << link_wire->power.readOp.leakage << " W, gate leakage "
			          << link_wire->power.readOp.gate_leakage << " W, area "
			          << link_wire->area.get_area() << " um^2; dynamic per bit-mm "
			          << ((bits > 0 && link_len > 0)
			                  ? link_wire->power.readOp.dynamic / bits / (link_len * 1e-3)
			                  : 0.0)
			          << " J" << std::endl;
		}
		else
		{
			/* PIMID (user ruling 2026-10-25): a BUS level's wire, printed for the hand check like the
			 * links above (print only; nothing priced changes). Its length is McPAT's upstream bus
			 * derivation: sqrt(processor area x chip_coverage) (x sqrt(dram_periph_area) on the DRAM
			 * die), / ((H+V)/2), / 2 when the bus has more than one node; McPAT charges it per access. */
			const double bits = nocdynp.flit_size;
			pimid::debugStream(std::cerr) << "  [link] noc" << ithNoC << " bus wire (McPAT interconnect, CACTI "
			          << "wire model, type Global_ho (repeaters 0.8x size, 1.4x spacing of delay-optimal; Ho 2003), interconnect projection "
			          << g_ip->ic_proj_type << ", " << g_ip->F_sz_nm << " nm table): "
			          << "length " << link_len << " um (upstream bus derivation over "
			          << nocdynp.horizontal_nodes << "x" << nocdynp.vertical_nodes << " nodes), "
			          << bits << " b; per access: dynamic "
			          << link_wire->power.readOp.dynamic << " J, leakage "
			          << link_wire->power.readOp.leakage << " W, gate leakage "
			          << link_wire->power.readOp.gate_leakage << " W, area "
			          << link_wire->area.get_area() << " um^2; dynamic per bit-mm "
			          << ((bits > 0 && link_len > 0)
			                  ? link_wire->power.readOp.dynamic / bits / (link_len * 1e-3)
			                  : 0.0)
			          << " J" << std::endl;
		}
	}

	link_bus_tot_per_Router.area.set_area(link_bus_tot_per_Router.area.get_area()+ link_bus->area.get_area()
			* nocdynp.global_linked_ports);

	area.set_area(area.get_area()+ link_bus_tot_per_Router.area.get_area()* nocdynp.total_nodes);
	/* PIMID: the measured activity and the effective energy, for the hand check. */
	if (XML->sys.NoC[ithNoC].link_activity >= 0.0 && XML->sys.NoC[ithNoC].link_kind != 2 && nocdynp.flit_size > 0)
	{
		const double a = XML->sys.NoC[ithNoC].link_activity;
		const double bits = nocdynp.flit_size;
		if (link_is_tsv)
			pimid::debugStream(std::cerr) << "  [link] noc" << ithNoC << " measured wire activity a01 " << a
			          << " (0->1 transitions per wire-bit per crossing; random data 0.25): effective dynamic per bit per"
			          << " crossing " << a << " x " << link_bus->power.readOp.dynamic / bits << " = "
			          << a * link_bus->power.readOp.dynamic / bits << " J" << std::endl;
		else if (link_len > 0)
			pimid::debugStream(std::cerr) << "  [link] noc" << ithNoC << " measured wire activity a01 " << a
			          << " (0->1 transitions per wire-bit per traversal; random data 0.25): effective dynamic per bit-mm "
			          << a << " x " << link_bus->power.readOp.dynamic / bits / (link_len * 1e-3) << " = "
			          << a * link_bus->power.readOp.dynamic / bits / (link_len * 1e-3) << " J at Vdd "
			          << g_tp.peri_global.Vdd << " V" << std::endl;
	}
	link_bus_exist = true;
	if (nocdynp.type) build_link_clock();   /* PIMID 1.12.18: global links, built after the chip area is known */
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
	    	stats_t.readAc.access  = router_accesses;   /* PIMID 1.12.18: total_accesses less pass-through (set_noc_param) */
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
        		/* PIMID 1.12.18 (ruling "pass-through routers as wire"): the
        		 * router dynamic the moved accesses would have cost, at this
        		 * instance's own per-access energy, for the hand check. */
        		const double moved = XML->sys.NoC[ithNoC].total_accesses - router_accesses;
        		if (moved > 0 && nocdynp.executionTime > 0)
        		{
        			const double e_acc = router->buffer.power.readOp.dynamic + router->buffer.power.writeOp.dynamic
        			                   + router->crossbar.power.readOp.dynamic + router->arbiter.power.readOp.dynamic;
        			pimid::debugStream(std::cerr) << "[mcpat noc] NoC " << ithNoC << " runtime: " << moved
        			          << " pass-through accesses NOT charged as router accesses (priced on the links): "
        			          << moved << " x " << e_acc << " J = " << moved*e_acc << " J = "
        			          << moved*e_acc/nocdynp.executionTime << " W of router dynamic removed; "
        			          << router_accesses << " router accesses charged = "
        			          << router_accesses*e_acc/nocdynp.executionTime << " W (logic-process router energy;"
        			          << (XML->sys.NoC[ithNoC].on_dram_die ? " this level is on the DRAM die, so processor.cc"
        			                                                   " then scales it by the DRAM-periphery dynamic factor)"
        			                                                 : " off-die, no family transform)")
        			          << std::endl;
        		}
        	}
        	if (link_bus_exist)
        	{
        		/* dynamic from the measured access count; leakage by the same
        		 * structure the peak path uses: per-router links x nodes. */
        		const double glp_nodes =
        		    (double)nocdynp.global_linked_ports * (double)nocdynp.total_nodes;
        		/* PIMID 1.12.18: a link's activity is LINK traversals. The
        		 * upstream charged the router access count, which is links + 1
        		 * per packet on a path and wrong for a links-only instance
        		 * altogether. The stat is used when supplied (>= 0). */
        		/* A TSV link's activity is flit x DIE crossings (each transfer
        		 * charged the dies it actually crossed); a wire link's is its
        		 * flit traversals. */
        		const double link_acc = link_is_tsv
        		    ? std::max(0.0, XML->sys.NoC[ithNoC].tsv_die_crossings)
        		    : ((XML->sys.NoC[ithNoC].link_traversals >= 0)
        		           ? XML->sys.NoC[ithNoC].link_traversals : rtp_stats.readAc.access);
        		/* PIMID (user ruling 2026-10-25, "Measured"): a wire's dynamic energy is C V^2 per
        		 * 0->1 transition (DSENT / ORION 2.0 physical convention); McPAT's per-bit C V^2 is
        		 * scaled by the MEASURED 0->1 transitions per wire-bit per traversal (link_activity;
        		 * random data 0.25). Not for a channel-I/O instance (link_kind 2: the DQ crossing's
        		 * own energy per bit). Runtime only: peak (TDP) keeps the full charge. */
        		const double wact = (XML->sys.NoC[ithNoC].link_activity >= 0.0 && XML->sys.NoC[ithNoC].link_kind != 2)
        		                  ? XML->sys.NoC[ithNoC].link_activity : 1.0;
        		set_pppm(pppm_t, link_acc * wact, glp_nodes, glp_nodes, link_acc * wact);
        		link_bus->rt_power = link_bus->power * pppm_t;
        		rt_power = rt_power + link_bus->rt_power;
        	}

	    }
	/* PIMID 1.12.18: CLOCK NETWORKS, ungated, every cycle of the NoC clock:
	 * total_nodes routers and total_nodes x global_linked_ports link
	 * segments (the structure the area and peak paths above use), times the
	 * NoC count when McPAT folds a homogeneous set into this object (its
	 * runtime activity is the set's, processor.cc). Peak: J per NoC cycle. */
	{
		const double nodes = nocdynp.total_nodes;
		const double links = (link_bus_exist ? (double)nocdynp.global_linked_ports * nodes : 0.0);
		const double sets  = (XML->sys.homogeneous_NoCs==1) ? XML->sys.number_of_NoCs : 1;
		const double er = clk_router.e_tree_cycle + clk_router.n_sinks*clk_router.e_pin_reg;
		const double el = clk_link.e_tree_cycle + clk_link.n_sinks*clk_link.e_pin_reg;
		const double cycles = nocdynp.executionTime * nocdynp.clockRate;
		/* Gated by the fabric's measured utilisation (user ruling
		 * 2026-10-23; Wattch cc2): the duty_cycle stat PIMID emits for this
		 * NoC (measured flit traversals / its peak: cycles x nodes, x ports
		 * for a device router since (1.12.150)). */
		double a = 1.0;
		if (!is_tdp)
		{
			a = clk_router.gate(nocdynp.duty_cycle, "duty_cycle = measured traversals / the instance peak PIMID passes");
			clk_link.gate(nocdynp.duty_cycle, "duty_cycle = measured traversals / the instance peak PIMID passes");
			/* PIMID 1.12.18 (ruling 2026-10-23 #4): global part every powered cycle. */
			clk_router.set_powered(XML->sys.pimid_clock_pg_noc, "1 - measured NoC power-gated residency (pimid_clock_pg_noc)");
			clk_link.set_powered(XML->sys.pimid_clock_pg_noc, "1 - measured NoC power-gated residency (pimid_clock_pg_noc)");
		}
		const double gr = is_tdp ? er : clk_router.gated_cycle(a, clk_router.n_sinks*clk_router.e_pin_reg);   /* PIMID 1.12.18 (ruling #4) */
		const double gl = is_tdp ? el : clk_link.gated_cycle(a, clk_link.n_sinks*clk_link.e_pin_reg);
		clk_router.set_power(er*nodes, gr*nodes*cycles*sets, er*nodes*cycles*sets, nodes);
		clk_link.set_power(el*links, gl*links*cycles*sets, el*links*cycles*sets, links);
		clk_router.rt_global = clk_router.e_tree_global_cycle*clk_router.powered*nodes*cycles*sets;
		clk_link.rt_global   = clk_link.e_tree_global_cycle*clk_link.powered*links*cycles*sets;
		clk_rt_global  = clk_router.rt_global + clk_link.rt_global;
		clk_rt_ungated = clk_router.rt_ungated + clk_link.rt_ungated;
		clk.power    = clk_router.power + clk_link.power;
		clk.rt_power = clk_router.rt_power + clk_link.rt_power;
		if (is_tdp) power = power + clk.power;
		else
		{
			rt_power = rt_power + clk.rt_power;
			std::ostringstream w;
			w << sets << " x (" << nodes << " router(s) x " << er << " J + " << links << " link segment(s) x "
			  << el << " J) x " << cycles << " cycles";
			if (clk_router.built) clk_router.report_runtime(w.str() + " (routers)", nocdynp.executionTime);
			if (clk_link.built && links > 0) clk_link.report_runtime("(links)", nocdynp.executionTime);
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
		/* PIMID 1.12.18: the clock network as its own line. */
		cout << indent_str<< "Clock Network: Peak Dynamic = " << clk.power.readOp.dynamic*nocdynp.clockRate
			<< " W, Subthreshold Leakage = " << clk.power.readOp.leakage << " W, Gate Leakage = "
			<< clk.power.readOp.gate_leakage << " W, Runtime Dynamic = "
			<< clk.rt_power.readOp.dynamic/nocdynp.executionTime << " W" << endl;
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
	/* PIMID 1.12.18 (user ruling "pass-through routers as wire"): a router
	 * with one router child and no endpoint is a single in/out path. It has
	 * nothing to arbitrate and no crossbar choice to make; its flits come up
	 * one link and leave by the other, and the caller prices those links
	 * (links-only instances, routers_present=0). Its accesses are therefore
	 * NOT router accesses: this instance charges its buffers, crossbar and
	 * arbiter total_accesses - passthrough_accesses, and the duty_cycle the
	 * caller derived from total_accesses (accesses / (cycles x nodes)) is
	 * scaled by the same ratio, so the clock gating and the TDP traffic
	 * factor read the routers' own activity. Nothing is added: the moved
	 * accesses are already the links' traversals. The node count is the
	 * caller's (pass-through routers are not in it). */
	router_accesses = XML->sys.NoC[ithNoC].total_accesses;
	if (nocdynp.type && XML->sys.NoC[ithNoC].routers_present &&
	    XML->sys.NoC[ithNoC].passthrough_accesses > 0)
	{
		const double T = XML->sys.NoC[ithNoC].total_accesses;
		const double P = std::min(T, XML->sys.NoC[ithNoC].passthrough_accesses);
		const double d0 = nocdynp.duty_cycle;
		router_accesses = T - P;
		if (T > 0) nocdynp.duty_cycle = d0 * (router_accesses / T);
		pimid::debugStream(std::cerr) << "[mcpat noc] NoC " << ithNoC << ": pass-through routers as wire: router accesses "
		          << T << " - " << P << " pass-through = " << router_accesses
		          << "; duty_cycle " << d0 << " -> " << nocdynp.duty_cycle
		          << " (x " << (T > 0 ? router_accesses / T : 0.0) << ")" << std::endl;
		if (XML->sys.NoC[ithNoC].passthrough_accesses > T)
			std::cerr << "[mcpat noc] NoC " << ithNoC << ": WARNING passthrough_accesses "
			          << XML->sys.NoC[ithNoC].passthrough_accesses << " > total_accesses " << T
			          << ", clamped" << std::endl;
	}
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
	/* PIMID 1.12.18: link_bus is a view; the owners are freed. */
	if(link_wire) 	           {delete link_wire; link_wire = 0;}
	if(link_tsv) 	           {delete link_tsv; link_tsv = 0;}
	link_bus = 0;
}
