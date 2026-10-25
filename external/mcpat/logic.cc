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

#include "logic.h"
#include <vector>      /* PIMID 1.12.18: guard lists */
#include <algorithm>
#include <set>       /* PIMID (1.12.31): print each published register once */
#include "wire.h"   /* PIMID 1.12.18: ClockNetwork prices its tree with CACTI wires */
void init_tech_params(double tech, bool is_tag);   /* PIMID 1.12.18: cacti/technology.cc (Ucache.h) */

/* PIMID 1.12.18 (ruling 2026-10-23 #2): switched capacitance of CACTI's
 * minimum inverter under the CURRENT technology tables (g_tp): gate of the
 * NMOS + PMOS pair (min_w_nmos_, McPAT's P/N sizing ratio) plus both drain
 * junctions -- the per-transition load of a minimum gate driving its twin. A
 * ratio of two of these (same device type, two nodes) times the Vdd^2 ratio
 * is the switched-energy scaling of a logic block between nodes. */
static double pimid_min_inv_switch_cap()
{
	const double wn = g_tp.min_w_nmos_;
	const double wp = wn * pmos_to_nmos_sz_ratio();
	return gate_C(wn + wp, 0) + drain_C_(wn, NCH, 1, 1, g_tp.cell_h_def) + drain_C_(wp, PCH, 1, 1, g_tp.cell_h_def);
}

/* PIMID (1.12.28): see logic.h. Coefficients transcribed from Stillmaker & Baas 2017 Table 5. */
int pimid_sb17_energy_factors(int nm, double v, double f[2], const char *type[2])
{
	struct Row { int nm; const char *t; double a2, a1, a0; };
	static const Row rows[] = {
		{45, "HP", 1.018, -0.3107, 0.1539}, {45, "LP", 1.103, -0.362, 0.2767},
		{32, "HP", 0.8367, -0.4341, 0.1701}, {32, "LP", 0.9559, -0.7823, 0.471},
		{20, "HP", 0.373, -0.1582, 0.04104}, {20, "LSTP", 0.2632, -0.14, 0.06841},
	};
	int n = 0;
	for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]) && n < 2; i++)
		if (rows[i].nm == nm) { f[n] = rows[i].a2*v*v + rows[i].a1*v + rows[i].a0; type[n] = rows[i].t; n++; }
	return n;
}

/* PIMID (next, user ruling 2026-10-25 A): THE FLIP-FLOP FROM PUBLISHED OPEN LIBRARIES, node-scaled.
 * Two published standard-cell D flip-flops (liberty files, typical corner, smallest input slew and output load
 * table entry; capacitance in fF, internal energy in fJ = V x fF):
 *  - Nangate 45 nm Open Cell Library (FreePDK45), NangateOpenCellLibrary_typical.lib, cell DFF_X1, 1.10 V:
 *    CK pin 0.949653 fF, D pin 1.140290 fF; CK internal energy per cycle with Q held (rise + fall, mean of the
 *    two held states) 6.916056 fJ; with Q toggling (fall only; the rising edge's energy is on Q/QN) 7.559952 fJ;
 *    Q + QN internal energy per output transition (related CK) 8.334629 fJ; D internal energy per D transition
 *    (mean over the CK states and edges) 1.879522 fJ.
 *  - ASAP7 7.5-track RVT (Clark et al., Microelectronics Journal 53, 2016), asap7sc7p5t_SEQ_RVT_TT_nldm_220123.lib,
 *    cell DFFHQNx1_ASAP7_75t_R, 0.70 V (VDD rows): CLK 0.474736 fF, D 0.558822 fF; CLK held 0.604924 fJ; CLK
 *    toggling 0.535838 fJ; QN per transition 0.497347 fJ; D per transition 0.097081 fJ.
 * Scaling (Stillmaker & Baas, Integration 58 (2017) 74-81, Eq. (6)/(9), Table 5): an energy E_t = E_s x
 * EF_t(V_t) / EF_s(V_s); a capacitance C_t = C_s x (EF_t / V_t^2) / (EF_s / V_s^2). A node without a Table 5 row
 * uses the NEAREST row (22 nm -> 20 nm). Every (source, target) device-type row pair is evaluated; the value
 * APPLIED is the midpoint of the pair range of the library NEAREST the run's node (log distance; 22 and 32 nm:
 * Nangate 45 nm; below sqrt(45 x 7) = 17.7 nm: ASAP7), and the bracket over BOTH libraries is printed (DERIVED
 * rule, stated). Per register per cycle: e_clock = C_CK V^2 + E_CK(held); a toggling cycle adds
 * e_switch = E_CK(toggling) - E_CK(held) + E_Q + E_D + 0.5 C_D V^2 (the next register's D pin). Liberty internal
 * energy includes short-circuit, so no sckt_co_eff is applied to it. Leakage and area stay McPAT's. */
static int pimid_sb17_rows_nearest(double nm, double v, double f[2], int *row_nm)
{
	struct Row { int nm; double a2, a1, a0; };
	static const Row rows[] = {
		{180, 0.0, 24.64, -17.98}, {130, 7.171, -6.709, 2.904}, {90, 4.762, -4.781, 2.092}, {65, 3.755, -4.398, 1.975},
		{45, 1.018, -0.3107, 0.1539}, {45, 1.103, -0.362, 0.2767}, {32, 0.8367, -0.4341, 0.1701}, {32, 0.9559, -0.7823, 0.471},
		{20, 0.373, -0.1582, 0.04104}, {20, 0.2632, -0.14, 0.06841}, {16, 0.2958, -0.1241, 0.03024}, {16, 0.2139, -0.1187, 0.05639},
		{14, 0.2363, -0.09675, 0.02239}, {14, 0.1556, -0.06472, 0.03066}, {10, 0.2068, -0.09311, 0.02375}, {10, 0.1261, -0.0518, 0.02769},
		{7, 0.1776, -0.09097, 0.02447}, {7, 0.09365, -0.03409, 0.02043},
	};
	int best = 0; double bd = 1e9;
	for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++)
	{
		const double d = fabs(log(nm / rows[i].nm));
		if (d < bd - 1e-12) { bd = d; best = rows[i].nm; }
	}
	int n = 0;
	for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]) && n < 2; i++)
		if (rows[i].nm == best) { f[n++] = rows[i].a2*v*v + rows[i].a1*v + rows[i].a0; }
	if (row_nm) *row_nm = best;
	return n;
}

bool pimid_dff_published(double nm, double v, PimidDffPub &o)
{
	struct Lib { const char *name; double nm, v, c_ck, c_d, e_held, e_tog, e_q, e_d; };
	static const Lib libs[2] = {
		{"Nangate 45 nm OCL DFF_X1", 45, 1.10, 0.949653e-15, 1.140290e-15, 6.916056e-15, 7.559952e-15, 8.334629e-15, 1.879522e-15},
		{"ASAP7 DFFHQNx1 (RVT)", 7, 0.70, 0.474736e-15, 0.558822e-15, 0.604924e-15, 0.535838e-15, 0.497347e-15, 0.097081e-15},
	};
	if (!(nm > 0) || !(v > 0)) return false;
	double ft[2]; int trow = 0;
	const int nt = pimid_sb17_rows_nearest(nm, v, ft, &trow);
	if (nt < 1) return false;
	const int pick = (fabs(log(nm / libs[0].nm)) <= fabs(log(nm / libs[1].nm))) ? 0 : 1;
	o.c_ck_lo = o.e_clock_lo = 1e9; o.c_ck_hi = o.e_clock_hi = 0;
	for (int li = 0; li < 2; li++)
	{
		double fs[2]; int srow = 0;
		const int ns = pimid_sb17_rows_nearest(libs[li].nm, libs[li].v, fs, &srow);
		double rlo = 1e9, rhi = 0;
		for (int a = 0; a < ns; a++) for (int b = 0; b < nt; b++)
		{
			const double r = ft[b] / fs[a];
			rlo = std::min(rlo, r); rhi = std::max(rhi, r);
		}
		const double ec_s = libs[li].c_ck * libs[li].v * libs[li].v + libs[li].e_held;
		const double cr = (libs[li].v * libs[li].v) / (v * v);   /* C = E / V^2 */
		o.c_ck_lo = std::min(o.c_ck_lo, libs[li].c_ck * rlo * cr); o.c_ck_hi = std::max(o.c_ck_hi, libs[li].c_ck * rhi * cr);
		o.e_clock_lo = std::min(o.e_clock_lo, ec_s * rlo); o.e_clock_hi = std::max(o.e_clock_hi, ec_s * rhi);
		if (li == pick)
		{
			const double r = 0.5 * (rlo + rhi);
			o.lib = libs[li].name; o.lib_nm = libs[li].nm; o.row_nm = trow; o.ratio = r; o.ratio_lo = rlo; o.ratio_hi = rhi;
			o.c_ck = libs[li].c_ck * r * cr;
			o.c_d  = libs[li].c_d * r * cr;
			o.e_clock  = o.c_ck * v * v + libs[li].e_held * r;
			o.e_switch = (libs[li].e_tog - libs[li].e_held + libs[li].e_q + libs[li].e_d) * r + 0.5 * o.c_d * v * v;
		}
	}
	return true;
}

/* PIMID (next, user ruling 2026-10-25, Z12): the PUBLISHED-SILICON node ratio for the published per-operation LOGIC
 * energies (FPU / ALU / MUL per-op, the DMA adder): DeepScaleTool (S. Sarangi, B. Baas, "DeepScaleTool: A Tool for the
 * Accurate Estimation of Technology Scaling in the Deep-Submicron Era", ISCAS 2021), a fit to Intel silicon (Holt,
 * ISSCC 2016 plenary), switching energy relative to 65 nm: 130 2.52, 90 1.51, 65 1, 45 0.63, 40 0.55, 32 0.44,
 * 28 0.37, 22 0.30, 14 0.19, 10 0.15, 7 0.11 (the tool's Energy table). The fit is VOLTAGE-LESS (each node at Intel's
 * nominal Vdd): the source Vdd is not used. A node between two table rows is interpolated log-log (DERIVED); a node
 * outside 7-130 nm has NO PUBLISHED SOURCE FOUND and keeps the CACTI ratio (pimid_node_energy_scale), said so.
 * CACTI arrays and wires keep CACTI's Vdd-aware tables. */
static bool pimid_dst_energy(double nm, double &e)
{
	static const double t[][2] = {{7,0.11},{10,0.15},{14,0.19},{22,0.30},{28,0.37},{32,0.44},{40,0.55},{45,0.63},{65,1.0},{90,1.51},{130,2.52}};
	const int n = sizeof(t)/sizeof(t[0]);
	if (!(nm >= t[0][0] && nm <= t[n-1][0])) return false;
	for (int i = 0; i < n - 1; i++)
		if (nm >= t[i][0] && nm <= t[i+1][0])
		{
			if (nm == t[i][0]) { e = t[i][1]; return true; }
			const double a = log(nm / t[i][0]) / log(t[i+1][0] / t[i][0]);
			e = exp(log(t[i][1]) + a * (log(t[i+1][1]) - log(t[i][1])));
			return true;
		}
	e = t[n-1][1]; return true;
}

double pimid_logic_op_scale(double src_nm, double src_vdd, bool *silicon)
{
	double es = 0, er = 0;
	const double run_nm = g_ip ? g_ip->F_sz_nm : 0.0;
	if (pimid_dst_energy(src_nm, es) && pimid_dst_energy(run_nm, er) && es > 0)
	{
		if (silicon) *silicon = true;
		return er / es;
	}
	if (silicon) *silicon = false;
	return pimid_node_energy_scale(src_nm, src_vdd);
}

/* PIMID 1.12.18: see logic.h. */
double pimid_node_energy_scale(double src_nm, double src_vdd, double *c_src_o, double *c_run_o,
                               double *v_run_o, double *v_src_cacti_o)
{
	const double c_run = pimid_min_inv_switch_cap();
	const double v_run = g_tp.peri_global.Vdd;
	const TechnologyParameter tp_saved = g_tp;
	/* PIMID 1.12.24: the source node's minimum inverter is drawn at the SOURCE node's feature size.
	 * TechnologyParameter::init sizes every device from g_ip->F_sz_um (min_w_nmos_ = 1.5 F,
	 * cell_h_def = 50 F, parameter.cc), not from the table it loads, so the source inverter used to
	 * be the run node's width with the source node's per-um capacitance: the linear shrink
	 * F_run/F_src was missing from k (45 -> 32 nm 0.876 instead of ~0.62; 45 -> 22 nm 0.541 instead
	 * of ~0.26), and C_inv(45 nm) came out 0.371 fF in a 32 nm run but 0.252 fF in a 22 nm run. */
	const double f_um_saved = g_ip->F_sz_um, f_nm_saved = g_ip->F_sz_nm;
	g_ip->F_sz_um = src_nm/1000.0; g_ip->F_sz_nm = src_nm;
	init_tech_params(src_nm/1000.0, false);
	const double c_src = pimid_min_inv_switch_cap();
	const double v_src_cacti = g_tp.peri_global.Vdd;
	g_ip->F_sz_um = f_um_saved; g_ip->F_sz_nm = f_nm_saved;
	g_tp = tp_saved;   /* the run's tables, exactly as they were */
	if (c_src_o) *c_src_o = c_src;
	if (c_run_o) *c_run_o = c_run;
	if (v_run_o) *v_run_o = v_run;
	if (v_src_cacti_o) *v_src_cacti_o = v_src_cacti;
	if (!(c_src > 0) || !(src_vdd > 0)) return 0.0;
	return (c_run / c_src) * (v_run / src_vdd) * (v_run / src_vdd);
}

/* PIMID 1.12.18: the guard rule (user ruling 2026-10-23): the most accurate
 * value goes in the total; the other sources/tools are printed beside it,
 * and a WARNING is printed when the value lies outside the range the
 * guards span (two or more guards), or below a published lower bound. One
 * plain guard spans no range: its ratio is printed, nothing is invented. */
struct PimidGuard { std::string label; double e; bool lower_bound; };
static void pimid_guard_report(const char *tag, int core, const std::string &what, double e,
                               const std::vector<PimidGuard> &g)
{
	double lo = 0, hi = 0; int nr = 0;
	for (size_t j = 0; j < g.size(); j++)
	{
		std::cerr << tag << " core" << core << " guard for " << what << ": " << g[j].label << " = "
		          << g[j].e*1e12 << " pJ at the run node" << (g[j].lower_bound ? " (lower bound)" : "")
		          << "; used value / guard = " << ((g[j].e > 0) ? e/g[j].e : 0.0) << std::endl;
		if (g[j].lower_bound) {
			if (e < g[j].e)
				std::cerr << tag << " core" << core << " WARNING: " << what << " " << e*1e12
				          << " pJ is BELOW the published lower bound " << g[j].label << std::endl;
			continue;
		}
		if (nr == 0) { lo = hi = g[j].e; } else { lo = std::min(lo, g[j].e); hi = std::max(hi, g[j].e); }
		nr++;
	}
	if (nr >= 2) {
		if (e < lo || e > hi)
			std::cerr << tag << " core" << core << " WARNING: " << what << " " << e*1e12 << " pJ is OUTSIDE the guard range ["
			          << lo*1e12 << ", " << hi*1e12 << "] pJ" << std::endl;
		else
			std::cerr << tag << " core" << core << " " << what << " " << e*1e12 << " pJ is inside the guard range ["
			          << lo*1e12 << ", " << hi*1e12 << "] pJ" << std::endl;
	}
}


//selection_logic
selection_logic::selection_logic(
    bool   _is_default,
    int    win_entries_,
    int    issue_width_,
    const InputParameter *configure_interface,
    enum Device_ty device_ty_,
    enum Core_type core_ty_)
    //const ParseXML *_XML_interface)
 :is_default(_is_default),
  win_entries(win_entries_),
  issue_width(issue_width_),
  device_ty(device_ty_),
  core_ty(core_ty_)
 {
    	//uca_org_t result2;
    	l_ip=*configure_interface;
    	local_result = init_interface(&l_ip);
    	//init_tech_params(l_ip.F_sz_um, false);
    	//win_entries=numIBEntries;//IQentries;
		//issue_width=issueWidth;
    	selection_power();
    	double sckRation = g_tp.sckt_co_eff;
    	power.readOp.dynamic *= sckRation;
    	power.writeOp.dynamic *= sckRation;
    	power.searchOp.dynamic *= sckRation;

    	double long_channel_device_reduction = longer_channel_device_reduction(device_ty,core_ty);
    	power.readOp.longer_channel_leakage	= power.readOp.leakage*long_channel_device_reduction;

    	double pg_reduction = power_gating_leakage_reduction(false);
    	power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;
  	    power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * long_channel_device_reduction;

	 }

void selection_logic::selection_power()
{//based on cost effective superscalar processor TR pp27-31
  double Ctotal, Cor, Cpencode;
  int num_arbiter;
  double WSelORn, WSelORprequ, WSelPn, WSelPp, WSelEnn, WSelEnp;

  //TODO: the 0.8um process data is used.
  WSelORn    	=  	12.5 * l_ip.F_sz_um;//this was 10 micron for the 0.8 micron process
  WSelORprequ   = 	50 * l_ip.F_sz_um;//this was 40 micron for the 0.8 micron process
  WSelPn     	= 	12.5 * l_ip.F_sz_um;//this was 10mcron for the 0.8 micron process
  WSelPp     	=  	18.75 * l_ip.F_sz_um;//this was 15 micron for the 0.8 micron process
  WSelEnn    	=  	6.25 * l_ip.F_sz_um;//this was 5 micron for the 0.8 micron process
  WSelEnp    	=  	12.5 * l_ip.F_sz_um;//this was 10 micron for the 0.8 micron process


  Ctotal=0;
  num_arbiter=1;
  while(win_entries > 4)
    {
      win_entries = (int)ceil((double)win_entries / 4.0);
      num_arbiter += win_entries;
    }
  //the 4-input OR logic to generate anyreq
  Cor = 4 * drain_C_(WSelORn,NCH,1,1, g_tp.cell_h_def) + drain_C_(WSelORprequ,PCH,1,1, g_tp.cell_h_def);
  power.readOp.gate_leakage = cmos_Ig_leakage(WSelORn, WSelORprequ, 4, nor)*g_tp.peri_global.Vdd;

  //The total capacity of the 4-bit priority encoder
  Cpencode = drain_C_(WSelPn,NCH,1, 1, g_tp.cell_h_def) + drain_C_(WSelPp,PCH,1, 1, g_tp.cell_h_def) +
    2*drain_C_(WSelPn,NCH,1, 1, g_tp.cell_h_def) + drain_C_(WSelPp,PCH,2, 1, g_tp.cell_h_def) +
    3*drain_C_(WSelPn,NCH,1, 1, g_tp.cell_h_def) + drain_C_(WSelPp,PCH,3, 1, g_tp.cell_h_def) +
    4*drain_C_(WSelPn,NCH,1, 1, g_tp.cell_h_def) + drain_C_(WSelPp,PCH,4, 1, g_tp.cell_h_def) +//precompute priority logic
    2*4*gate_C(WSelEnn+WSelEnp,20.0)+
    4*drain_C_(WSelEnn,NCH,1, 1, g_tp.cell_h_def) + 2*4*drain_C_(WSelEnp,PCH,1, 1, g_tp.cell_h_def)+//enable logic
    (2*4+2*3+2*2+2)*gate_C(WSelPn+WSelPp,10.0);//requests signal

  Ctotal += issue_width * num_arbiter*(Cor+Cpencode);

  power.readOp.dynamic = Ctotal*g_tp.peri_global.Vdd*g_tp.peri_global.Vdd*2;//2 means the abitration signal need to travel round trip
  power.readOp.leakage = issue_width * num_arbiter *
      (cmos_Isub_leakage(WSelPn, WSelPp, 2, nor)/*approximate precompute with a nor gate*///grant1p
       + cmos_Isub_leakage(WSelPn, WSelPp, 3, nor)//grant2p
       + cmos_Isub_leakage(WSelPn, WSelPp, 4, nor)//grant3p
       + cmos_Isub_leakage(WSelEnn, WSelEnp, 2, nor)*4//enable logic
       + cmos_Isub_leakage(WSelEnn, WSelEnp, 1, inv)*2*3//for each grant there are two inverters, there are 3 grant sIsubnals
		  )*g_tp.peri_global.Vdd;
  power.readOp.gate_leakage = issue_width * num_arbiter *
      (cmos_Ig_leakage(WSelPn, WSelPp, 2, nor)/*approximate precompute with a nor gate*///grant1p
       + cmos_Ig_leakage(WSelPn, WSelPp, 3, nor)//grant2p
       + cmos_Ig_leakage(WSelPn, WSelPp, 4, nor)//grant3p
       + cmos_Ig_leakage(WSelEnn, WSelEnp, 2, nor)*4//enable logic
       + cmos_Ig_leakage(WSelEnn, WSelEnp, 1, inv)*2*3//for each grant there are two inverters, there are 3 grant signals
        )*g_tp.peri_global.Vdd;
}


dep_resource_conflict_check::dep_resource_conflict_check(
	const InputParameter *configure_interface,
	const CoreDynParam & dyn_p_,
	int   compare_bits_,
    bool   _is_default)
 :  l_ip(*configure_interface),
    coredynp(dyn_p_),
    compare_bits(compare_bits_),
	is_default(_is_default)
{
    	Wcompn    	=  	25 * l_ip.F_sz_um;//this was 20.0 micron for the 0.8 micron process
    	Wevalinvp   = 	25 * l_ip.F_sz_um;//this was 20.0 micron for the 0.8 micron process
    	Wevalinvn   = 	100 * l_ip.F_sz_um;//this was 80.0 mcron for the 0.8 micron process
    	Wcomppreequ =  	50 * l_ip.F_sz_um;//this was 40.0  micron for the 0.8 micron process
    	WNORn    	=  	6.75 * l_ip.F_sz_um;//this was 5.4 micron for the 0.8 micron process
    	WNORp    	=  	38.125 * l_ip.F_sz_um;//this was 30.5 micron for the 0.8 micron process

    	local_result = init_interface(&l_ip);

    	if (coredynp.core_ty==Inorder)
   		    compare_bits += 16 + 8 + 8;//TODO: opcode bits + log(shared resources) + REG TAG BITS-->opcode comparator
    	else
    		compare_bits += 16 + 8 + 8;

   		conflict_check_power();
    	double sckRation = g_tp.sckt_co_eff;
    	power.readOp.dynamic *= sckRation;
    	power.writeOp.dynamic *= sckRation;
    	power.searchOp.dynamic *= sckRation;

}

void dep_resource_conflict_check::conflict_check_power()
{
	double Ctotal;
	int num_comparators;
	num_comparators = 3*((coredynp.decodeW) * (coredynp.decodeW)-coredynp.decodeW);//2(N*N-N) is used for source to dest comparison, (N*N-N) is used for dest to dest comparision.
	//When decode-width ==1, no dcl logic

	Ctotal = num_comparators * compare_cap();
	//printf("%i,%s\n",XML_interface->sys.core[0].predictor.predictor_entries,XML_interface->sys.core[0].predictor.prediction_scheme);

	power.readOp.dynamic=Ctotal*/*CLOCKRATE*/g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/*AF*/;
	power.readOp.leakage=num_comparators*compare_bits*2*simplified_nmos_leakage(Wcompn,  false);

	double long_channel_device_reduction = longer_channel_device_reduction(Core_device, coredynp.core_ty);
	power.readOp.longer_channel_leakage	= power.readOp.leakage*long_channel_device_reduction;
	power.readOp.gate_leakage=num_comparators*compare_bits*2*cmos_Ig_leakage(Wcompn, 0, 2, nmos);

	double pg_reduction = power_gating_leakage_reduction(false);
	power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;
	power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * long_channel_device_reduction;



}

/* estimate comparator power consumption (this comparator is similar
   to the tag-match structure in a CAM */
double dep_resource_conflict_check::compare_cap()
{
  double c1, c2;

  WNORp = WNORp * compare_bits/2.0;//resize the big NOR gate at the DCL according to fan in.
  /* bottom part of comparator */
  c2 = (compare_bits)*(drain_C_(Wcompn,NCH,1,1, g_tp.cell_h_def)+drain_C_(Wcompn,NCH,2,1, g_tp.cell_h_def))+
  drain_C_(Wevalinvp,PCH,1,1, g_tp.cell_h_def) + drain_C_(Wevalinvn,NCH,1,1, g_tp.cell_h_def);

  /* top part of comparator */
  c1 = (compare_bits)*(drain_C_(Wcompn,NCH,1,1, g_tp.cell_h_def)+drain_C_(Wcompn,NCH,2,1, g_tp.cell_h_def)+
		  drain_C_(Wcomppreequ,NCH,1,1, g_tp.cell_h_def)) +  gate_C(WNORn + WNORp,10.0) +
		  drain_C_(WNORp,NCH,2,1, g_tp.cell_h_def) + compare_bits*drain_C_(WNORn,NCH,2,1, g_tp.cell_h_def);
  return(c1 + c2);

}

void dep_resource_conflict_check::leakage_feedback(double temperature)
{
  l_ip.temp = (unsigned int)round(temperature/10.0)*10;
  uca_org_t init_result = init_interface(&l_ip); // init_result is dummy

  // This is part of conflict_check_power()
  int num_comparators = 3*((coredynp.decodeW) * (coredynp.decodeW)-coredynp.decodeW);//2(N*N-N) is used for source to dest comparison, (N*N-N) is used for dest to dest comparision.
  power.readOp.leakage=num_comparators*compare_bits*2*simplified_nmos_leakage(Wcompn,  false);

  double long_channel_device_reduction = longer_channel_device_reduction(Core_device, coredynp.core_ty);
  power.readOp.longer_channel_leakage	= power.readOp.leakage*long_channel_device_reduction;
  power.readOp.gate_leakage=num_comparators*compare_bits*2*cmos_Ig_leakage(Wcompn, 0, 2, nmos);

  double pg_reduction = power_gating_leakage_reduction(false);
  power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;
  power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * long_channel_device_reduction;


}

//TODO: add inverter and transmission gate base DFF.

DFFCell::DFFCell(
		bool _is_dram,
		double _WdecNANDn,
		double _WdecNANDp,
		double _cell_load,
		const InputParameter *configure_interface)
:is_dram(_is_dram),
cell_load(_cell_load),
WdecNANDn(_WdecNANDn),
WdecNANDp(_WdecNANDp),
published(false)
{//this model is based on the NAND2 based DFF.
			l_ip=*configure_interface;
//			area.set_area(730*l_ip.F_sz_um*l_ip.F_sz_um);
			area.set_area(5*compute_gate_area(NAND, 2,WdecNANDn,WdecNANDp, g_tp.cell_h_def)
				+ compute_gate_area(NAND, 2,WdecNANDn,WdecNANDn, g_tp.cell_h_def));


}


double DFFCell::fpfp_node_cap(unsigned int fan_in, unsigned int fan_out)
{
  double Ctotal = 0;
  //printf("WdecNANDn = %E\n", WdecNANDn);

  /* part 1: drain cap of NAND gate */
  Ctotal += drain_C_(WdecNANDn, NCH, 2, 1, g_tp.cell_h_def, is_dram) + fan_in * drain_C_(WdecNANDp, PCH, 1, 1, g_tp.cell_h_def, is_dram);

  /* part 2: gate cap of NAND gates */
  Ctotal += fan_out * gate_C(WdecNANDn + WdecNANDp, 0, is_dram);

  return Ctotal;
}


void DFFCell::compute_DFF_cell()
{
	double c1, c2, c3, c4, c5, c6;
	   /* node 5 and node 6 are identical to node 1 in capacitance */
	   c1 = c5 = c6 = fpfp_node_cap(2, 1);
	   c2 = fpfp_node_cap(2, 3);
	   c3 = fpfp_node_cap(3, 2);
	   c4 = fpfp_node_cap(2, 2);

	   //cap-load of the clock signal in each Dff, actually the clock signal only connected to one NAND2
	   clock_cap= 2 * gate_C(WdecNANDn + WdecNANDp, 0, is_dram);
	   e_switch.readOp.dynamic += (c4 + c1 + c2 + c3 + c5 + c6 + 2*cell_load)*0.5*g_tp.peri_global.Vdd * g_tp.peri_global.Vdd;;

	   /* no 1/2 for e_keep and e_clock because clock signal switches twice in one cycle */
	   e_keep_1.readOp.dynamic += c3 * g_tp.peri_global.Vdd * g_tp.peri_global.Vdd ;
	   e_keep_0.readOp.dynamic += c2 * g_tp.peri_global.Vdd * g_tp.peri_global.Vdd ;
	   e_clock.readOp.dynamic += clock_cap* g_tp.peri_global.Vdd * g_tp.peri_global.Vdd;;

	   /* PIMID (next, user ruling 2026-10-25 A): the clock pin, the clock energy and the toggle energy from the
	    * published libraries, node-scaled (pimid_dff_published above); McPAT's NAND2 values are printed beside. */
	   {
		   PimidDffPub pub;
		   if (g_ip && pimid_dff_published(g_ip->F_sz_nm, g_tp.peri_global.Vdd, pub))
		   {
			   const double v2 = g_tp.peri_global.Vdd * g_tp.peri_global.Vdd;
			   static std::set<std::pair<long, long> > shown;
			   const std::pair<long, long> key((long)(g_ip->F_sz_nm * 100), (long)(g_tp.peri_global.Vdd * 1000));
			   if (shown.insert(key).second)
				   std::cerr << "[mcpat dff] register at " << g_ip->F_sz_nm << " nm, " << g_tp.peri_global.Vdd << " V: "
				             << pub.lib << " (the nearest published library), Stillmaker & Baas row " << pub.row_nm
				             << " nm, energy ratio " << pub.ratio << " (pair range [" << pub.ratio_lo << ", " << pub.ratio_hi
				             << "], midpoint): CK pin " << pub.c_ck*1e15 << " fF, D pin " << pub.c_d*1e15
				             << " fF, clock " << pub.e_clock*1e15 << " fJ/cycle, toggle +" << pub.e_switch*1e15
				             << " fJ; bracket over both libraries: CK pin [" << pub.c_ck_lo*1e15 << ", " << pub.c_ck_hi*1e15
				             << "] fF, clock [" << pub.e_clock_lo*1e15 << ", " << pub.e_clock_hi*1e15 << "] fJ/cycle."
				             << " McPAT's NAND2 register was: CK pin " << clock_cap*1e15 << " fF, clock "
				             << clock_cap*v2*g_tp.sckt_co_eff*1e15 << " fJ/cycle (x sckt), toggle/keep "
				             << e_switch.readOp.dynamic*g_tp.sckt_co_eff*1e15 << " / " << e_keep_0.readOp.dynamic*g_tp.sckt_co_eff*1e15
				             << " / " << e_keep_1.readOp.dynamic*g_tp.sckt_co_eff*1e15 << " fJ" << std::endl;
			   clock_cap = pub.c_ck;
			   e_clock.readOp.dynamic  = pub.e_clock;
			   e_switch.readOp.dynamic = pub.e_switch;
			   e_keep_0.readOp.dynamic = 0;   /* a held cycle's energy is all in e_clock */
			   e_keep_1.readOp.dynamic = 0;
			   published = true;
		   }
	   }
	   /* static power */
	   e_switch.readOp.leakage +=  (cmos_Isub_leakage(WdecNANDn, WdecNANDp, 2, nand)*5//5 NAND2 and 1 NAND3 in a DFF
                                           + cmos_Isub_leakage(WdecNANDn, WdecNANDn, 3, nand))*g_tp.peri_global.Vdd;
	   e_switch.readOp.gate_leakage += (cmos_Ig_leakage(WdecNANDn, WdecNANDp, 2, nand)*5//5 NAND2 and 1 NAND3 in a DFF
                                           + cmos_Ig_leakage(WdecNANDn, WdecNANDn, 3, nand))*g_tp.peri_global.Vdd;
	   //printf("leakage =%E\n",cmos_Ileak(1, is_dram) );
}

Pipeline::Pipeline(
		const InputParameter *configure_interface,
		const CoreDynParam & dyn_p_,
		enum Device_ty device_ty_,
		bool _is_core_pipeline,
		bool _is_default)
: l_ip(*configure_interface),
  coredynp(dyn_p_),
  device_ty(device_ty_),
  is_core_pipeline(_is_core_pipeline),
  is_default(_is_default),
  num_piperegs(0.0),
  clock_energy_per_reg(0.0)

  {
	local_result = init_interface(&l_ip);
	if (!coredynp.Embedded)
		process_ind = true;
	else
		process_ind = false;
	reg_widths(process_ind, l_ip.F_sz_um, WNANDn, WNANDp);   /* PIMID 1.12.18: the rule below, factored out */
	load_per_pipeline_stage = 2*gate_C(WNANDn + WNANDp, 0, false);
	compute();

}

/* PIMID 1.12.18: McPAT's register widths, unchanged (moved from the
 * constructor so the clock network sizes other components' registers the
 * same way). */
void Pipeline::reg_widths(bool process_ind, double F_sz_um, double &wn, double &wp)
{
	wn = (process_ind)? 25 *   F_sz_um : g_tp.min_w_nmos_ ;//this was  20 micron for the 0.8 micron process
	wp = (process_ind)? 37.5 * F_sz_um : g_tp.min_w_nmos_*pmos_to_nmos_sz_ratio();//this was  30 micron for the 0.8 micron process
}

/* PIMID 1.12.18: clock-pin energy of one McPAT register per cycle, exactly
 * as Pipeline::compute() prices it (DFFCell e_clock = clock_cap x Vdd^2,
 * the clock switching twice per cycle, x sckt_co_eff). */
double Pipeline::reg_clock_energy(bool embedded, double F_sz_um)
{
	double wn, wp;
	reg_widths(!embedded, F_sz_um, wn, wp);
	InputParameter dummy;   /* DFFCell keeps a copy only */
	DFFCell reg(false, wn, wp, 2*gate_C(wn + wp, 0, false), &dummy);
	reg.compute_DFF_cell();
	return reg.e_clock.readOp.dynamic * (reg.published ? 1.0 : g_tp.sckt_co_eff);   /* (1.12.31): liberty energy includes sc */
}

void Pipeline::compute()
{
	compute_stage_vector();
	DFFCell pipe_reg(false, WNANDn,WNANDp, load_per_pipeline_stage, &l_ip);
	pipe_reg.compute_DFF_cell();

	double clock_power_pipereg = num_piperegs * pipe_reg.e_clock.readOp.dynamic;
	clock_energy_per_reg = pipe_reg.e_clock.readOp.dynamic * (pipe_reg.published ? 1.0 : g_tp.sckt_co_eff);   /* PIMID 1.12.18: as scaled below */
	//******************pipeline power: currently, we average all the possibilities of the states of DFFs in the pipeline. A better way to do it is to consider
	//the harming distance of two consecutive signals, However McPAT does not have plan to do this in near future as it focuses on worst case power.
	double pipe_reg_power = num_piperegs * (pipe_reg.e_switch.readOp.dynamic+pipe_reg.e_keep_0.readOp.dynamic+pipe_reg.e_keep_1.readOp.dynamic)/3+clock_power_pipereg;
	double pipe_reg_leakage = num_piperegs * pipe_reg.e_switch.readOp.leakage;
	double pipe_reg_gate_leakage = num_piperegs * pipe_reg.e_switch.readOp.gate_leakage;
	power.readOp.dynamic	+=pipe_reg_power;
	power.readOp.leakage	+=pipe_reg_leakage;
	power.readOp.gate_leakage	+=pipe_reg_gate_leakage;
	area.set_area(num_piperegs * pipe_reg.area.get_area());

	double long_channel_device_reduction = longer_channel_device_reduction(device_ty, coredynp.core_ty);
	power.readOp.longer_channel_leakage	= power.readOp.leakage*long_channel_device_reduction;

	double pg_reduction = power_gating_leakage_reduction(false);
	power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;
	power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * long_channel_device_reduction;




	double sckRation = pipe_reg.published ? 1.0 : g_tp.sckt_co_eff;   /* (1.12.31): published liberty energy includes sc */
	power.readOp.dynamic *= sckRation;
	power.writeOp.dynamic *= sckRation;
	power.searchOp.dynamic *= sckRation;
	double macro_layout_overhead = g_tp.macro_layout_overhead;
	if (!coredynp.Embedded)
		area.set_area(area.get_area()*macro_layout_overhead);
}

void Pipeline::compute_stage_vector()
{
	double num_stages, tot_stage_vector, per_stage_vector;
	int opcode_length = coredynp.x86? coredynp.micro_opcode_length:coredynp.opcode_length;
	//Hthread = thread_clock_gated? 1:num_thread;

  if (!is_core_pipeline)
  {
	num_piperegs=l_ip.pipeline_stages*l_ip.per_stage_vector;//The number of pipeline stages are calculated based on the achievable throughput and required throughput
  }
  else
  {
	if (coredynp.core_ty==Inorder)
	{
		/* assume 6 pipe stages and try to estimate bits per pipe stage */
		/* pipe stage 0/IF */
		num_piperegs += coredynp.pc_width*2*coredynp.num_hthreads;
		/* pipe stage IF/ID */
		num_piperegs += coredynp.fetchW*(coredynp.instruction_length + coredynp.pc_width)*coredynp.num_hthreads;
		/* pipe stage IF/ThreadSEL */
		if (coredynp.multithreaded) num_piperegs += coredynp.num_hthreads*coredynp.perThreadState; //8 bit thread states
		/* PIMID (next, user rulings 2026-10-25): each later stage register is DERIVED from what it carries,
		 * the field set of the textbook pipelined datapath (Patterson & Hennessy, Computer Organization and
		 * Design, the 5-stage pipeline-register figure: ID/EX = control + PC + two source operands + the
		 * sign-extended immediate + register specifiers; EX/MEM = control + branch target + zero flag + ALU
		 * result + store data + destination register; MEM/WB = control + load data + ALU result +
		 * destination register), with the control field the ENCODED micro-op (opcode_length bits, the
		 * micro_opcode_width on x86) and the data fields this core's int_data_width. Upstream McPAT carried
		 * the whole instruction into ID/EXE and 8*2*int_data_width extra bits (16 data words, unsourced) into
		 * each of EXE/MEM and MEM/WB, i.e. 2048 of a 64-bit core's latches. RI5CY (Gautschi et al., TVLSI 2017)
		 * is an IF/ID/EX/WB pipeline; McPAT's six-stage count is kept. */
		/* pipe stage ID/EXE: control, PC, 2 source operands + immediate, 3 register specifiers (rs1, rs2, rd) */
		num_piperegs += coredynp.decodeW*(opcode_length + coredynp.pc_width + 3*coredynp.int_data_width + 3*coredynp.arch_ireg_width)*coredynp.num_hthreads;
		/* pipe stage EXE/MEM: control, branch target, zero flag, ALU result, store data, destination register */
		num_piperegs += coredynp.issueW*(opcode_length + coredynp.pc_width + 1 + 2*coredynp.int_data_width + coredynp.arch_ireg_width);
		/* pipe stage MEM/WB: control, load data, ALU result, destination register */
		num_piperegs += coredynp.issueW*(opcode_length + 2*coredynp.int_data_width + coredynp.arch_ireg_width);
//		/* pipe stage 5/6 */
//		num_piperegs += issueWidth*(data_width + powers (2,opcode_length)/*+2*powers (2,reg_length)*/);
//		/* pipe stage 6/7 */
//		num_piperegs += issueWidth*(data_width + powers (2,opcode_length)/*+2*powers (2,reg_length)*/);
//		/* pipe stage 7/8 */
//		num_piperegs += issueWidth*(data_width + powers (2,opcode_length)/**2*powers (2,reg_length)*/);
//		/* assume 50% extra in control signals (rule of thumb) */
		num_stages=6;

	}
	else
	{
		/* assume 12 stage pipe stages and try to estimate bits per pipe stage */
		/*OOO: Fetch, decode, rename, IssueQ, dispatch, regread, EXE, MEM, WB, CM */

		/* pipe stage 0/1F*/
		num_piperegs += coredynp.pc_width*2*coredynp.num_hthreads ;//PC and Next PC
		/* pipe stage IF/ID */
		num_piperegs += coredynp.fetchW*(coredynp.instruction_length + coredynp.pc_width)*coredynp.num_hthreads;//PC is used to feed branch predictor in ID
		/* pipe stage 1D/Renaming*/
		num_piperegs += coredynp.decodeW*(coredynp.instruction_length + coredynp.pc_width)*coredynp.num_hthreads;//PC is for branch exe in later stage.
		/* pipe stage Renaming/wire_drive */
		num_piperegs += coredynp.decodeW*(coredynp.instruction_length + coredynp.pc_width);
		/* pipe stage Renaming/IssueQ */
		num_piperegs += coredynp.issueW*(coredynp.instruction_length  + coredynp.pc_width + 3*coredynp.phy_ireg_width)*coredynp.num_hthreads;//3*coredynp.phy_ireg_width means 2 sources and 1 dest
		/* pipe stage IssueQ/Dispatch */
		num_piperegs += coredynp.issueW*(coredynp.instruction_length + 3 * coredynp.phy_ireg_width);
		/* pipe stage Dispatch/EXE */

		/* PIMID (1.12.30): encoded micro-op field, as in the in-order case */
		num_piperegs += coredynp.issueW*(3 * coredynp.phy_ireg_width + coredynp.pc_width + opcode_length/*+2*powers (2,reg_length)*/);
		/* 2^opcode_length means the total decoded signal for the opcode*/
		num_piperegs += coredynp.issueW*(2*coredynp.int_data_width + opcode_length/*+2*powers (2,reg_length)*/);
		/*2 source operands in EXE; Assume 2EXE stages* since we do not really distinguish OP*/
		num_piperegs += coredynp.issueW*(2*coredynp.int_data_width + opcode_length/*+2*powers (2,reg_length)*/);
		/* pipe stage EXE/MEM, data need to be read/write, address*/
		num_piperegs += coredynp.issueW*(coredynp.int_data_width + coredynp.v_address_width + opcode_length/*+2*powers (2,reg_length)*/);//memory Opcode still need to be passed
		/* pipe stage MEM/WB; result data, writeback regs */
		num_piperegs += coredynp.issueW*(coredynp.int_data_width + coredynp.phy_ireg_width /* powers (2,opcode_length) + (2,opcode_length)+2*powers (2,reg_length)*/);
		/* pipe stage WB/CM ; result data, regs need to be updated, address for resolve memory ops in ROB's top*/
		num_piperegs += coredynp.commitW*(coredynp.int_data_width + coredynp.v_address_width + coredynp.phy_ireg_width/*+ powers (2,opcode_length)*2*powers (2,reg_length)*/)*coredynp.num_hthreads;
//		if (multithreaded)
//		{
//
//		}
		num_stages=12;

	}

	/* PIMID (next, user ruling 2026-10-25): upstream McPAT multiplied the count by 1.5 here ("assume 50% extra
	 * in control registers and interrupt registers (rule of thumb)"), an unsourced factor; removed. The
	 * count is the stage fields above. */
	tot_stage_vector=num_piperegs;
	per_stage_vector=tot_stage_vector/num_stages;

	if (coredynp.core_ty==Inorder)
	{
		if (coredynp.pipeline_stages>6)
			num_piperegs= per_stage_vector*coredynp.pipeline_stages;
	}
	else//OOO
	{
		if (coredynp.pipeline_stages>12)
			num_piperegs= per_stage_vector*coredynp.pipeline_stages;
	}
  }

}

/* PIMID 1.12.18: CLOCK DISTRIBUTION NETWORK -- see logic.h for the model
 * and its sources (Wattch, D. Brooks, V. Tiwari, M. Martonosi, ISCA 2000,
 * clocking section; H-tree length after Bakoglu 1990 ch. 8). Every number
 * here is read from CACTI's technology tables and wire model, or from the
 * structure counts the caller passes. */
ClockNetwork::ClockNetwork()
: built(false), enabled(true), tree_area_um2(0), side_um(0), n_sinks(0), h_levels(0), tree_len_um(0),
  c_wire_per_um(0), c_eff_per_um(0), repeater_spacing_um(0), repeater_size(0),
  e_tree_cycle(0), e_pin_reg(0), c_pin(0), vdd(0), clock_hz(0), sckt(0),
  n_clusters(0), a_reg_um2(0), cluster_side_um(0), c_local_per_um(0), wn(0), wp(0),   /* PIMID (next, F2) */
  span_is_clocked_logic(false), cluster_area_um2(0), c_lcb_in(0), c_lcb_out(0), lcb_wn(0), lcb_wp(0),
  leak(0), gate_leak(0), lc_reduction(0), pg_reduction(0), activity(1), rt_ungated(0),
  global_len_um(0), local_len_um(0), e_tree_global_cycle(0), e_tree_local_cycle(0),
  powered(1), powered_source("always powered (no power gating)"), rt_global(0),   /* PIMID 1.12.18 (ruling #4) */
  halt(false), running(1), running_max(1), running_source("no idle halt"), rt_halted(0)   /* PIMID 1.12.18 (idle halt) */
{
}

void ClockNetwork::compute(const InputParameter *configure_interface, const string &owner_,
		double tree_area_um2_, double n_sinks_, const string &sink_source_,
		double e_pin_per_reg_, double clock_hz_, bool embedded_,
		enum Device_ty device_ty_, enum Core_type core_ty_, bool enabled_)
{
	enabled = enabled_;
	if (!enabled)
	{   /* PIMID 1.12.18: user option power.models.clock_network = false. */
		owner = owner_;
		built = true;
		std::cerr << "[mcpat clock] " << owner << ": clock distribution NOT priced (power.models.clock_network: "
		             "false, user option)" << std::endl;
		return;
	}
	InputParameter l_ip = *configure_interface;
	uca_org_t res = init_interface(&l_ip);   /* g_tp of this owner's technology */
	res.cleanup();
	owner         = owner_;
	sink_source   = sink_source_;
	tree_area_um2 = (tree_area_um2_ > 0) ? tree_area_um2_ : 0;
	n_sinks       = (n_sinks_ > 0) ? n_sinks_ : 0;
	clock_hz      = clock_hz_;
	vdd           = g_tp.peri_global.Vdd;
	sckt          = g_tp.sckt_co_eff;
	side_um       = sqrt(tree_area_um2);
	/* PIMID (next, user ruling 2026-10-25, F2): LEAF CLUSTERS. The sinks are grouped into clusters of
	 * k = leaf_cluster_sinks (power.clock.leaf_cluster_sinks, default 30: the OpenROAD TritonCTS sink-cluster
	 * cap, "default 30"; IBM US 8,458,634 B2: a local clock buffer drives "at most 24, 26, or 32 latches").
	 * The GLOBAL H-tree (Global_30, repeated) spans the tree area and reaches the n_cl = ceil(N / k) cluster
	 * buffers: L_g = 1.5 D (sqrt(n_cl) - 1) (Bakoglu 1990 ch. 8). Each cluster's LOCAL net is unrepeated local
	 * wire (CACTI wire_local) over the cluster's own register area, k_eff x one register cell (McPAT's DFF):
	 * an H-tree to its k_eff sinks, l_c = 1.5 d (sqrt(k_eff) - 1), d = sqrt(k_eff x a_reg).
	 * The die-level trunk (processor.cc; no register pins, e_pin_per_reg_ 0) has component TREE ROOTS as leaves,
	 * not registers: one leaf per cluster, no local nets (its model is unchanged). */
	const double k_leaf = (e_pin_per_reg_ != 0.0) ? leaf_cluster_sinks : 1.0;
	n_clusters    = (n_sinks > 0) ? ceil(n_sinks / k_leaf) : 0;
	h_levels      = (n_clusters > 1) ? log(n_clusters)/log(4.0) : 0;
	tree_len_um   = (n_clusters > 1) ? 1.5*side_um*(sqrt(n_clusters) - 1.0) : 0;   /* the GLOBAL tree */

	Pipeline::reg_widths(!embedded_, l_ip.F_sz_um, wn, wp);
	{   /* DFFCell::compute_DFF_cell clock_cap ((1.12.31): the published, node-scaled CK pin) */
		DFFCell pin_reg(false, wn, wp, 2*gate_C(wn + wp, 0, false), &l_ip);
		pin_reg.compute_DFF_cell();
		c_pin = pin_reg.clock_cap;
	}
	e_pin_reg = (e_pin_per_reg_ >= 0) ? e_pin_per_reg_ : Pipeline::reg_clock_energy(embedded_, l_ip.F_sz_um);

	double e_global = 0.0;
	leak = gate_leak = 0;
	if (tree_len_um > 0)
	{
		Wire w(l_ip.wt, tree_len_um);
		e_global = w.power.readOp.dynamic * sckt;
		leak         = w.power.readOp.leakage;
		gate_leak    = w.power.readOp.gate_leakage;
		repeater_spacing_um = w.repeater_spacing;
		repeater_size       = w.repeater_size;
		c_wire_per_um = w.wire_cap(1e-6, true);
		c_eff_per_um  = e_global / (tree_len_um * vdd * vdd);
	}
	/* the local nets: unrepeated local wire over each cluster's area. (next, user ruling 2026-10-25 on the
	 * local nets) A tree whose area IS clocked logic (a core: F1) has its flops interleaved with that logic,
	 * so each cluster's net spans the cluster's SHARE of the tree area, A / n_cl (the clusters tile the area
	 * the global tree spans). Other trees (caches, controllers, routers, links: area = the whole component,
	 * arrays included) keep k x one McPAT register cell (their registers sit together at the ports). */
	{
		DFFCell cell(false, wn, wp, 2*gate_C(wn + wp, 0, false), &l_ip);
		a_reg_um2 = cell.area.get_area();
	}
	const double k_eff = (n_clusters > 0) ? n_sinks / n_clusters : 0.0;
	cluster_area_um2 = (span_is_clocked_logic && n_clusters > 0) ? tree_area_um2 / n_clusters : k_eff * a_reg_um2;
	cluster_side_um = sqrt(cluster_area_um2);
	local_len_um    = (k_eff > 1) ? n_clusters * 1.5 * cluster_side_um * (sqrt(k_eff) - 1.0) : 0.0;
	c_local_per_um  = g_tp.wire_local.C_per_um;
	const double c_net_local = local_len_um * c_local_per_um;
	/* (1.12.30) the LOCAL CLOCK BUFFER of each cluster, absent until now (the global wire ended unloaded and the
	 * cluster nets were undriven): an inverting driver at stage effort 4 (logical effort's best stage effort
	 * is about 4: Sutherland, Sproull, Harris, "Logical Effort", 1999), DERIVED input C = (net + pins) / 4,
	 * CACTI devices at this node (P/N ratio, drain capacitance, leakage). Its input is the global tree's
	 * load (every powered cycle); its output diffusion switches with the cluster (gated). */
	c_lcb_in = c_lcb_out = 0.0;
	if (k_leaf > 1 && n_clusters > 0 && k_eff > 0)
	{
		const double c_load = (c_net_local + k_eff * n_clusters * c_pin) / n_clusters;
		const double c_per_um = gate_C(1.0, 0, false) - gate_C(0.0, 0, false);
		const double w_tot = (c_per_um > 0) ? (c_load / 4.0) / c_per_um : 0.0;
		const double r = pmos_to_nmos_sz_ratio();
		lcb_wn = w_tot / (1.0 + r); lcb_wp = w_tot * r / (1.0 + r);
		c_lcb_in  = gate_C(lcb_wn + lcb_wp, 0, false);
		c_lcb_out = drain_C_(lcb_wn, NCH, 1, 1, g_tp.cell_h_def) + drain_C_(lcb_wp, PCH, 1, 1, g_tp.cell_h_def);
		leak      += n_clusters * cmos_Isub_leakage(lcb_wn, lcb_wp, 1, inv) * vdd;
		gate_leak += n_clusters * cmos_Ig_leakage(lcb_wn, lcb_wp, 1, inv) * vdd;
	}
	e_global += n_clusters * c_lcb_in * vdd * vdd * sckt;
	const double e_local = (c_net_local + n_clusters * c_lcb_out) * vdd * vdd * sckt;
	global_len_um = tree_len_um;
	tree_len_um   = global_len_um + local_len_um;
	e_tree_cycle  = e_global + e_local;
	/* PIMID 1.12.18 (user ruling 2026-10-23 #4), with (1.12.30) F2: GLOBAL = the H-tree to the cluster
	 * buffers (every powered cycle), LOCAL = the cluster nets + the register pins (gated by activity). */
	if (!split_enabled)
		std::cerr << "[mcpat clock] " << owner << ": global/local split OFF BY USER OPTION (power.models.clock_global_local:"
		          << " false): the whole tree is gated by the measured activity" << std::endl;
	e_tree_global_cycle = split_enabled ? e_global : 0.0;
	e_tree_local_cycle  = e_tree_cycle - e_tree_global_cycle;
	lc_reduction = longer_channel_device_reduction(device_ty_, core_ty_);
	pg_reduction = power_gating_leakage_reduction(false);
	built = true;

	std::cerr << "[mcpat clock] " << owner << ": area " << tree_area_um2*1e-6 << " mm^2 (D "
	          << side_um << " um), sinks N " << n_sinks << " (" << sink_source << "), H-tree "
	          << h_levels << " levels to " << n_clusters << " cluster buffers, L = global + local = " << tree_len_um << " um; global wire "
	          << (l_ip.wt == Global ? "Global" : (l_ip.wt == Global_30 ? "Global_30" : "other"))
	          << " C_wire " << c_wire_per_um*1e15 << " fF/um, repeaters x" << repeater_size
	          << " every " << repeater_spacing_um << " um, C_eff (wire+repeaters+sc, x sckt "
	          << sckt << ") " << c_eff_per_um*1e15 << " fF/um; pin C " << c_pin*1e15
	          << " fF, E_pin " << e_pin_reg << " J; Vdd " << vdd << " V, f " << clock_hz*1e-6
	          << " MHz; per cycle: tree " << e_tree_cycle << " J + pins (ungated) "
	          << n_sinks*e_pin_reg << " J; repeater leakage " << leak << " W, gate "
	          << gate_leak << " W" << std::endl;
	std::cerr << "[mcpat clock] " << owner << ": split (" << ((k_leaf > 1) ? "F2 leaf clusters of k = " : "")
	          << ((k_leaf > 1) ? std::to_string((int)k_leaf) : std::string("no leaf clusters: the leaves are component tree roots, not registers"))
	          << ((k_leaf > 1) ? " sinks [OpenROAD TritonCTS sink-cluster cap 30; IBM US 8,458,634 B2: 24-32 latches per local clock buffer]" : "")
	          << "): GLOBAL H-tree to " << n_clusters << " cluster buffers L 1.5 D (sqrt(N/k) - 1) = " << global_len_um
	          << " um (repeated, wire above) -> " << e_tree_global_cycle << " J/cycle, every powered cycle; LOCAL "
	          << n_clusters << " cluster nets of " << k_eff << " sinks over " << cluster_side_um << " um ("
	          << (span_is_clocked_logic ? "the cluster's share of the clocked-logic area, " : "k register cells, ")
	          << cluster_area_um2 << " um^2; register cell " << a_reg_um2 << " um^2) = " << local_len_um
	          << " um of local wire (" << c_local_per_um*1e15 << " fF/um, unrepeated) + " << n_clusters
	          << " local clock buffers (FO4-sized: in " << c_lcb_in*1e15 << " fF on the global side, out "
	          << c_lcb_out*1e15 << " fF) -> " << e_tree_local_cycle << " J/cycle + pins " << n_sinks*e_pin_reg
	          << " J/cycle, gated by measured activity" << std::endl;
	if (k_leaf > 1)
	{
		/* printed check: local load (cluster nets + register clock pins) vs the global tree's switched
		 * capacitance, against "the effective load-capacitance faced by the LCBs is generally more than
		 * ~10x higher than that of the clock-mesh itself" (arXiv 1401.0015, citing its ref. [23]). */
		/* "global" = the network itself (wire + repeaters), as the observation compares the LCBs' load
		 * with the clock mesh itself; the LCB inputs are printed apart. */
		const double c_lcb_in_all = n_clusters * c_lcb_in;
		const double c_glob = (vdd > 0 && sckt > 0) ? e_global / (sckt * vdd * vdd) - c_lcb_in_all : 0.0;
		const double c_loc  = c_net_local + n_clusters * c_lcb_out + n_sinks * c_pin;
		std::cerr << "[mcpat clock] " << owner << ": local/global load ratio " << ((c_glob > 0) ? c_loc / c_glob : 0.0)
		          << " (local " << c_loc*1e15 << " fF = nets " << c_net_local*1e15 << " + LCB outputs "
		          << n_clusters * c_lcb_out*1e15 << " + " << n_sinks << " pins " << n_sinks*c_pin*1e15
		          << "; global network " << c_glob*1e15 << " fF, LCB inputs " << c_lcb_in_all*1e15
		          << " fF beside it); published observation > ~10x (arXiv 1401.0015)" << ((c_glob > 0 && c_loc / c_glob < 10.0)
		          ? " -- BELOW it (printed, not judged)" : " -- consistent") << std::endl;
	}
}

void ClockNetwork::set_powered(double r_gated, const string &src)
{
	/* PIMID 1.12.18 (ruling #4): power gating (off by default) stops the
	 * global part for the measured gated residency; nothing else does. */
	if (!(r_gated > 0)) { powered = 1.0; powered_source = "always powered (no power gating)"; return; }
	powered = 1.0 - ((r_gated > 1.0) ? 1.0 : r_gated);
	powered_source = src;
}

bool ClockNetwork::split_enabled = true;   /* PIMID 1.12.18 (ruling #4) */
double ClockNetwork::leaf_cluster_sinks = 30.0;   /* PIMID (next, F2): power.clock.leaf_cluster_sinks */

double ClockNetwork::gated_cycle(double a, double pins_e) const
{
	return e_tree_global_cycle * global_on() + (e_tree_local_cycle + pins_e) * a;   /* PIMID 1.12.18 (idle halt) */
}

/* PIMID 1.12.18 (user ruling 2026-10-23, idle halt; logic.h). */
void ClockNetwork::set_running(bool halt_on, double r_mean, double r_max, const string &src)
{
	halt = halt_on;
	running = running_max = 1.0;
	if (!halt_on) { running_source = "idle halt OFF (power.models.idle_halt: false): global part every powered cycle"; return; }
	if (!(r_mean >= 0))
	{
		running_source = "NO MEASURED RUNNING TIME: global part every powered cycle (nothing invented)";
		std::cerr << "[mcpat clock] " << owner << ": idle halt: " << running_source << std::endl;
		return;
	}
	running     = (r_mean > 1.0) ? 1.0 : r_mean;
	running_max = (r_max >= running) ? ((r_max > 1.0) ? 1.0 : r_max) : running;
	running_source = src;
}

double ClockNetwork::global_on() const
{
	return halt ? std::min(powered, running) : powered;
}

double ClockNetwork::trunk_on() const
{
	return halt ? std::min(powered, running_max) : powered;
}

double ClockNetwork::gate(double a, const string &src)
{
	if (!enabled) { activity = 0; activity_source = "not priced (user option)"; return 0; }
	if (!(a >= 0))
	{
		std::cerr << "NO MEASURED ACTIVITY: " << owner << " (" << src
		          << "); its clock tree is priced UNGATED -- no duty is invented." << std::endl;
		activity = 1.0;
		activity_source = "none measured: ungated";
		return 1.0;
	}
	activity = (a > 1.0) ? 1.0 : a;   /* a port-usage fraction cannot exceed every cycle */
	activity_source = src;
	return activity;
}

void ClockNetwork::set_power(double tdp_dyn, double rt_gated, double rt_ungated_, double n_leak_instances)
{
	power.reset();
	rt_power.reset();
	power.readOp.dynamic = tdp_dyn;
	power.readOp.leakage = leak * n_leak_instances;
	power.readOp.gate_leakage = gate_leak * n_leak_instances;
	power.readOp.longer_channel_leakage = power.readOp.leakage * lc_reduction;
	power.readOp.power_gated_leakage = power.readOp.leakage * pg_reduction;
	power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * lc_reduction;
	rt_power = power;
	rt_power.readOp.dynamic = rt_gated;
	rt_ungated = rt_ungated_;
}

void ClockNetwork::report_runtime(const string &what, double seconds) const
{
	const double g = rt_power.readOp.dynamic;
	std::cerr << "[mcpat clock] " << owner << " runtime: ungated " << what << " = " << rt_ungated
	          << " J (" << ((seconds > 0) ? rt_ungated/seconds : 0.0) << " W, reference); GATED (Wattch cc2, activity "
	          << activity << " = " << activity_source << ") " << g << " J over " << seconds << " s = "
	          << ((seconds > 0) ? g/seconds : 0.0) << " W dynamic (charged); leakage "
	          << rt_power.readOp.leakage << " W + gate " << rt_power.readOp.gate_leakage << " W" << std::endl;
	std::cerr << "[mcpat clock] " << owner << " runtime split (ruling #4): GLOBAL (H-tree levels but the last, every powered"
	          << " cycle; powered " << powered << " = " << powered_source << ") " << rt_global << " J = "
	          << ((seconds > 0) ? rt_global/seconds : 0.0) << " W; LOCAL (last level + pins, gated) " << g - rt_global
	          << " J = " << ((seconds > 0) ? (g - rt_global)/seconds : 0.0) << " W" << std::endl;
	if (halt)   /* PIMID 1.12.18 (idle halt) */
		std::cerr << "[mcpat clock] " << owner << " runtime idle halt (power.models.idle_halt): running fraction "
		          << running << " (max " << running_max << "; " << running_source << "), global part on " << global_on()
		          << " of the cycles; HALTED (clock off, not charged) " << rt_halted << " J = "
		          << ((seconds > 0) ? rt_halted/seconds : 0.0) << " W; leakage unchanged" << std::endl;
}

FunctionalUnit::FunctionalUnit(ParseXML *XML_interface, int ithCore_, InputParameter* interface_ip_,const CoreDynParam & dyn_p_, enum FU_type fu_type_)
:XML(XML_interface),
 ithCore(ithCore_),
 interface_ip(*interface_ip_),
 coredynp(dyn_p_),
 fu_type(fu_type_)
{
    double area_t;//, leakage, gate_leakage;
    double pmos_to_nmos_sizing_r = pmos_to_nmos_sz_ratio();
	clockRate = coredynp.clockRate;
	executionTime = coredynp.executionTime;

	//XML_interface=_XML_interface;
	uca_org_t result2;
	result2 = init_interface(&interface_ip);
	if (XML->sys.Embedded)
	{
		if (fu_type == FPU)
		{
			num_fu=coredynp.num_fpus;
			//area_t = 8.47*1e6*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2
			area_t = 4.47*1e6*(g_ip->F_sz_nm*g_ip->F_sz_nm/90.0/90.0);//this is um^2 The base number
			//4.47 contains both VFP and NEON processing unit, VFP is about 40% and NEON is about 60%
			if (g_ip->F_sz_nm>90)
				area_t = 4.47*1e6*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2
			leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(5*g_tp.min_w_nmos_, 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
			gate_leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(5*g_tp.min_w_nmos_, 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
			//energy = 0.3529/10*1e-9;//this is the energy(nJ) for a FP instruction in FPU usually it can have up to 20 cycles.
//			base_energy = coredynp.core_ty==Inorder? 0: 89e-3*3; //W The base energy of ALU average numbers from Intel 4G and 773Mhz (Wattch)
//			base_energy *=(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);
			base_energy = 0;
			per_access_energy = 1.15/1e9/4/1.3/1.3*g_tp.peri_global.Vdd*g_tp.peri_global.Vdd*(g_ip->F_sz_nm/90.0);//g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);//0.00649*1e-9; //This is per Hz energy(nJ)
			//FPU power from Sandia's processor sizing tech report
			FU_height=(18667*num_fu)*interface_ip.F_sz_um;//FPU from Sun's data
		}
		else if (fu_type == ALU)
		{
			num_fu=coredynp.num_alus;
			area_t = 280*260*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2 ALU + MUl
			leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
			gate_leakage = area_t*(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;
//			base_energy = coredynp.core_ty==Inorder? 0:89e-3; //W The base energy of ALU average numbers from Intel 4G and 773Mhz (Wattch)
//			base_energy *=(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);
			base_energy = 0;
			per_access_energy = 1.15/3/1e9/4/1.3/1.3*g_tp.peri_global.Vdd*g_tp.peri_global.Vdd*(g_ip->F_sz_nm/90.0);//(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);//0.00649*1e-9; //This is per cycle energy(nJ)
			FU_height=(6222*num_fu)*interface_ip.F_sz_um;//integer ALU

		}
		else if (fu_type == MUL)
		{
			num_fu=coredynp.num_muls;
			area_t = 280*260*3*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2 ALU + MUl
			leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
			gate_leakage = area_t*(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;
//			base_energy = coredynp.core_ty==Inorder? 0:89e-3*2; //W The base energy of ALU average numbers from Intel 4G and 773Mhz (Wattch)
//			base_energy *=(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);
			base_energy = 0;
			per_access_energy = 1.15*2/3/1e9/4/1.3/1.3*g_tp.peri_global.Vdd*g_tp.peri_global.Vdd*(g_ip->F_sz_nm/90.0);//(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);//0.00649*1e-9; //This is per cycle energy(nJ), coefficient based on Wattch
			FU_height=(9334*num_fu )*interface_ip.F_sz_um;//divider/mul from Sun's data
		}
		else
		{
			cout<<"Unknown Functional Unit Type"<<endl;
			exit(0);
		}
		per_access_energy *=0.5;//According to ARM data embedded processor has much lower per acc energy
	}
	else
	{
		if (fu_type == FPU)
		{
			num_fu=coredynp.num_fpus;
			//area_t = 8.47*1e6*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2
			area_t = 8.47*1e6*(g_ip->F_sz_nm*g_ip->F_sz_nm/90.0/90.0);//this is um^2
			if (g_ip->F_sz_nm>90)
				area_t = 8.47*1e6*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2
			leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(5*g_tp.min_w_nmos_, 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
			gate_leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(5*g_tp.min_w_nmos_, 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
			//energy = 0.3529/10*1e-9;//this is the energy(nJ) for a FP instruction in FPU usually it can have up to 20 cycles.
			base_energy = coredynp.core_ty==Inorder? 0: 89e-3*3; //W The base energy of ALU average numbers from Intel 4G and 773Mhz (Wattch)
			base_energy *=(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);
			per_access_energy = 1.15*3/1e9/4/1.3/1.3*g_tp.peri_global.Vdd*g_tp.peri_global.Vdd*(g_ip->F_sz_nm/90.0);//g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);//0.00649*1e-9; //This is per op energy(nJ)
			FU_height=(38667*num_fu)*interface_ip.F_sz_um;//FPU from Sun's data
		}
		else if (fu_type == ALU)
		{
			num_fu=coredynp.num_alus;
			area_t = 280*260*2*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2 ALU + MUl
			leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
			gate_leakage = area_t*(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;
			base_energy = coredynp.core_ty==Inorder? 0:89e-3; //W The base energy of ALU average numbers from Intel 4G and 773Mhz (Wattch)
			base_energy *=(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);
			per_access_energy = 1.15/1e9/4/1.3/1.3*g_tp.peri_global.Vdd*g_tp.peri_global.Vdd*(g_ip->F_sz_nm/90.0);//(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);//0.00649*1e-9; //This is per cycle energy(nJ)
			FU_height=(6222*num_fu)*interface_ip.F_sz_um;//integer ALU

		}
		else if (fu_type == MUL)
		{
			num_fu=coredynp.num_muls;
			area_t = 280*260*2*3*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2 ALU + MUl
			leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
			gate_leakage = area_t*(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;
			base_energy = coredynp.core_ty==Inorder? 0:89e-3*2; //W The base energy of ALU average numbers from Intel 4G and 773Mhz (Wattch)
			base_energy *=(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);
			per_access_energy = 1.15*2/1e9/4/1.3/1.3*g_tp.peri_global.Vdd*g_tp.peri_global.Vdd*(g_ip->F_sz_nm/90.0);//(g_tp.peri_global.Vdd*g_tp.peri_global.Vdd/1.2/1.2);//0.00649*1e-9; //This is per cycle energy(nJ), coefficient based on Wattch
			FU_height=(9334*num_fu )*interface_ip.F_sz_um;//divider/mul from Sun's data
		}
		else
		{
			cout<<"Unknown Functional Unit Type"<<endl;
			exit(0);
		}
	}
	/* PIMID 1.12.18 (user ruling 2026-10-23 #1): McPAT's FIXED base energy
	 * per OOO functional-unit type (89 mW ALU / 2 x 89 mW MUL / 3 x 89 mW FPU
	 * at 1.2 V, "average numbers from Intel 4G and 773Mhz (Wattch)", scaled
	 * by Vdd^2 above) is charged as base_energy x executionTime whatever the
	 * activity (computeEnergy). It is the units' idle clocking, which the
	 * gated clock network now prices (logic.h ClockNetwork), so by default
	 * (power.models.mcpat_fu_base_energy: false -> pimid_fu_base_energy 0)
	 * it is removed here, from both the peak and the runtime term. */
	if (base_energy > 0)
	{
		const char* fu_nm = (fu_type == FPU) ? "FPU" : (fu_type == ALU) ? "ALU" : "MUL";
		if (!XML->sys.pimid_fu_base_energy)
		{
			std::cerr << "[mcpat fu] core" << ithCore << " " << fu_nm << ": McPAT OOO fixed base energy "
			          << base_energy << " W (x execution time " << executionTime << " s = "
			          << base_energy*executionTime*g_tp.sckt_co_eff << " J runtime incl. sckt "
			          << g_tp.sckt_co_eff << ") NOT charged (power.models.mcpat_fu_base_energy: false;"
			          << " the gated clock network prices the units' clocking)" << std::endl;
			base_energy = 0;
		}
		else
			std::cerr << "[mcpat fu] core" << ithCore << " " << fu_nm << ": McPAT OOO fixed base energy "
			          << base_energy << " W charged BY USER OPTION (power.models.mcpat_fu_base_energy: true)" << std::endl;
	}

	/* PIMID 1.12.18 (user rulings 2026-10-23 #1, #2): PUBLISHED PER-OPERATION
	 * FP ENERGY, per scalar operation, the source value at its PRINTED node
	 * and supply, scaled to the run node with CACTI's tables
	 * (pimid_node_energy_scale, logic.h).
	 *  FP32 add 0.9 pJ, FP32 mult 3.7 pJ: M. Horowitz, "Computing's energy
	 *   problem (and what we can do about it)", ISSCC 2014, paper 1.1, Fig.
	 *   1.1.9 "Rough energy costs for various operations in 45nm 0.9V" (ruling
	 *   #1: scaled from 0.9 V as printed; there is no 1.0 V path).
	 *  FP32 FMA 14.86 pJ, FP64 FMA 52.98 pJ: S. Galal, "Energy efficient
	 *   floating-point unit design", PhD thesis, Stanford 2012 (Horowitz
	 *   group; vlsiweb.stanford.edu/people/alum/pdf/1211_SamehGalal_FP_Design.pdf),
	 *   Table 4.2 (p. 53), TSMC 45 nm, 6-stage, low Vth, Vdd 0.9 V: the
	 *   table's Power columns are "Leakage" and "Dynamic" (mW); SP leakage 1.2
	 *   + dynamic 30.9 mW at 2.08 GHz, DP leakage 4.6 + dynamic 95.9 mW at
	 *   1.81 GHz. 1.12.18 (user ruling 2026-10-24, "dynamic only"): the
	 *   DYNAMIC power / frequency is the per-operation energy (one FMA per
	 *   cycle): SP 30.9 / 2.08 = 14.86 pJ, DP 95.9 / 1.81 = 52.98 pJ. The
	 *   leakage is not added: McPAT charges the FPU's leakage itself (it was
	 *   counted twice: 15.4 / 55.5 pJ before). The same table's W/Gops column
	 *   (leakage + dynamic, FMA = 2 flops) gives 14.4 / 50.6 pJ: printed.
	 *  FP64 add 5.0 pJ, FP64 mult 19.4 pJ: Galal 2012 Fig. 3.7 (p. 24), DP
	 *   45 nm adder / multiplier, the KNEE (2 Pmin) of the energy vs
	 *   area-per-throughput frontier Pmin/P + Amin/A = 1. Choice: Pmin (2.5 /
	 *   9.68 pJ) is an asymptote (infinite area, vanishing frequency), not a
	 *   realisable unit; the knee is the published point closest to a ~2 GHz
	 *   unit (it coincides with Table 4.2's DP FMA row at 0.81 V, 0.95 GHz),
	 *   and the thesis gives no adder/multiplier point at 2 GHz. FRONTIER
	 *   CAVEAT (printed): the 1.81 GHz DP FMA sits at ~1.7x its knee, so a
	 *   ~2 GHz adder/multiplier is likely ABOVE the knee value used.
	 *  GUARDS (printed, scaled the same way): FPnew, S. Mach et al., IEEE
	 *   TVLSI 2021 (arXiv 2007.01530), GF 22FDX, TT 0.8 V, Table IV scalar
	 *   FMA: FP64 26.7 pJ, FP32 9.44 pJ (CACTI has no FD-SOI table: the bulk
	 *   22 nm table is used); S. Keckler et al., "GPUs and the future of
	 *   parallel computing", IEEE Micro 2011, Table 1, 40 nm 0.9 V: DFMA
	 *   50 pJ; Galal's own Pmin (lower bounds) for FP64 add/mult.
	 *  Divide/sqrt and x87: NO PUBLISHED SOURCE FOUND (Galal and FPnew report
	 *   none): McPAT's per-op energy kept.
	 * Only the per-operation dynamic energy of the operations listed is
	 * replaced here (computeEnergy); the FPU AREA (and with it the leakage)
	 * is set below (pimid_fu_area_published, 1.12.29:). */
	if (fu_type == FPU && XML->sys.pimid_fpu_published)
	{
		struct PubSrc { const char *name; double pj, nm, vdd; const char *cite; };
		static const PubSrc src[6] = {
			{"FP32 add",  0.9,  45, 0.9, "Horowitz ISSCC 2014 Fig. 1.1.9 (45 nm 0.9 V)"},
			{"FP32 mult", 3.7,  45, 0.9, "Horowitz ISSCC 2014 Fig. 1.1.9 (45 nm 0.9 V)"},
			{"FP64 add",  5.0,  45, 0.9, "Galal PhD thesis Stanford 2012 Fig. 3.7, DP adder knee (2 x Pmin 2.5 pJ), 45 nm"},
			{"FP64 mult", 19.4, 45, 0.9, "Galal PhD thesis Stanford 2012 Fig. 3.7, DP multiplier knee (2 x Pmin 9.68 pJ), 45 nm"},
			{"FP32 FMA",  30.9 / 2.08, 45, 0.9, "Galal 2012 Table 4.2, SP FMA 6-stage LVT 0.9 V 2.08 GHz: dynamic 30.9 mW / 2.08 GHz (leakage 1.2 mW is McPAT's)"},
			{"FP64 FMA",  95.9 / 1.81, 45, 0.9, "Galal 2012 Table 4.2, DP FMA 6-stage LVT 0.9 V 1.81 GHz: dynamic 95.9 mW / 1.81 GHz (leakage 4.6 mW is McPAT's)"}};
		double c_src = 0, c_run = 0, v_run = 0, v_src_cacti = 0;
		const double k45_cacti = pimid_node_energy_scale(45.0, 0.9, &c_src, &c_run, &v_run, &v_src_cacti);
		bool sil = false;
		const double k45 = pimid_logic_op_scale(45.0, 0.9, &sil);   /* (next, Z12): published silicon */
		pimid_pub = (k45 > 0);
		std::cerr << "[mcpat fpu] core" << ithCore << " node scaling 45 -> " << interface_ip.F_sz_nm
		          << " nm (CACTI tables, device type of this core): C_inv " << c_src*1e15 << " fF -> " << c_run*1e15
		          << " fF, Vdd 0.9 V (source, as printed) -> " << v_run << " V (CACTI run node; CACTI's own 45 nm Vdd is "
		          << v_src_cacti << " V) => factor " << k45_cacti << " (reference); USED: "
		          << (sil ? "published silicon (DeepScaleTool, Sarangi & Baas ISCAS 2021, Intel silicon; voltage-less) ratio "
		                  : "NO PUBLISHED SOURCE FOUND for this node in the silicon fit: the CACTI ratio ") << k45 << std::endl;
		for (int q = 0; q < 6; q++)
		{
			pimid_pub_e[q] = src[q].pj * 1e-12 * pimid_logic_op_scale(src[q].nm, src[q].vdd);   /* (next, Z12) */
			std::cerr << "[mcpat fpu] core" << ithCore << " published " << src[q].name << ": " << src[q].pj << " pJ ("
			          << src[q].cite << ") -> " << pimid_pub_e[q]*1e12 << " pJ per scalar op at the run node (USED)" << std::endl;
		}
		std::cerr << "[mcpat fpu] core" << ithCore << " FRONTIER CAVEAT (FP64 add/mult): Galal Fig. 3.7 plots energy vs"
		          << " area-per-throughput, not delay; Pmin is an asymptote, the knee (used) matches a ~1 GHz design"
		          << " (Table 4.2 DP FMA 0.81 V 0.95 GHz), and the 1.81 GHz DP FMA sits at ~1.7x its knee: a ~2 GHz"
		          << " adder/multiplier is likely above the value used" << std::endl;
		std::cerr << "[mcpat fpu] core" << ithCore << " NOTE (FMA): Galal Table 4.2's own W/Gops column (FMA = 2 flops)"
		          << " (leakage + dynamic) gives 14.4 pJ (SP) / 50.6 pJ (DP), 3-5% below dynamic power / frequency (used)" << std::endl;
		std::vector<PimidGuard> g;
		g.push_back({"Galal 2012 Fig. 3.7 DP adder Pmin 2.5 pJ (45 nm 0.9 V)", 2.5e-12*k45, true});
		pimid_guard_report("[mcpat fpu]", ithCore, "FP64 add", pimid_pub_e[2], g); g.clear();
		g.push_back({"Galal 2012 Fig. 3.7 DP multiplier Pmin 9.68 pJ (45 nm 0.9 V)", 9.68e-12*k45, true});
		pimid_guard_report("[mcpat fpu]", ithCore, "FP64 mult", pimid_pub_e[3], g); g.clear();
		g.push_back({"FPnew (Mach et al., TVLSI 2021) Table IV FP32 FMA 9.44 pJ (GF 22FDX 0.8 V)", 9.44e-12*pimid_logic_op_scale(22.0, 0.8), false});
		pimid_guard_report("[mcpat fpu]", ithCore, "FP32 FMA", pimid_pub_e[4], g); g.clear();
		g.push_back({"FPnew (Mach et al., TVLSI 2021) FP64 FMA 26.7 pJ (GF 22FDX 0.8 V)", 26.7e-12*pimid_logic_op_scale(22.0, 0.8), false});
		g.push_back({"Keckler et al., IEEE Micro 2011, Table 1, DFMA 50 pJ (40 nm 0.9 V)", 50e-12*pimid_logic_op_scale(40.0, 0.9), false});
		pimid_guard_report("[mcpat fpu]", ithCore, "FP64 FMA", pimid_pub_e[5], g); g.clear();
		std::cerr << "[mcpat fpu] core" << ithCore << " NO PUBLISHED SOURCE FOUND for FP divide/sqrt (single, double;"
		          << " Galal 2012 and FPnew 2021 report none) and x87: those keep McPAT's per-op FPU energy" << std::endl;
	}

	/* PIMID 1.12.18 (user ruling 2026-10-23 #3): PUBLISHED PER-OPERATION
	 * INTEGER ENERGY. Source: Horowitz ISSCC 2014 Fig. 1.1.9 (45 nm, 0.9 V as
	 * printed): Add 8 bit 0.03 pJ, 32 bit 0.1 pJ; Mult 8 bit 0.2 pJ, 32 bit
	 * 3.1 pJ, scaled to the run node as above. The ALU prices the decoder's
	 * scalar GPR add/logic operations (add, sub, adc, sbb, cmp, and, or, xor,
	 * test, not, neg, inc, dec, shifts/rotates, bit scans/tests) of 8 and 32
	 * bits; the MUL unit its 8- and 32-bit multiplies. NO PUBLISHED SOURCE
	 * FOUND for 16- or 64-bit add/logic, 16- or 64-bit multiply, or divide
	 * (Horowitz gives 8 and 32 bit only; Keckler 2011 Table 1 has no integer
	 * row; no peer-reviewed per-op value found), and no published rule
	 * composes a 64-bit value from the 32-bit one, so those keep McPAT's
	 * per-op energy, as do moves, LEA and NOPs (no adder value applies).
	 * The ALU/MUL AREA (and with it the leakage) is set below
	 * (pimid_fu_area_published, 1.12.29:). */
	if ((fu_type == ALU || fu_type == MUL) && XML->sys.pimid_int_alu_published)
	{
		double c_src = 0, c_run = 0, v_run = 0;
		const double k45_cacti = pimid_node_energy_scale(45.0, 0.9, &c_src, &c_run, &v_run);
		bool sil = false;
		const double k45 = pimid_logic_op_scale(45.0, 0.9, &sil);   /* (next, Z12): published silicon, voltage-less */
		pimid_int_pub = (k45 > 0);
		const bool alu = (fu_type == ALU);
		const double mcpat_ref_op = per_access_energy * g_tp.sckt_co_eff;   /* McPAT's per-op, for the prints */
		pimid_int_e[0] = (alu ? 0.03e-12 : 0.2e-12) * k45;
		pimid_int_e[1] = (alu ? 0.1e-12 : 3.1e-12) * k45;
		/* PIMID (next, user ruling 2026-10-25 #1): ADD/LOGIC LINEAR IN WIDTH from Horowitz's two published points
		 * (8 bit 0.03 pJ, 32 bit 0.1 pJ): 16 bit on the line between them, 0.03 + 8 x 0.07/24 = 0.0533 pJ; 64 bit
		 * = 2 x 32 bit (two 32-bit slices), 0.2 pJ (DERIVED, stated). An ALU access the decoder does not classify
		 * by width (moves, LEA, NOP, other) is priced as one machine-word (64-bit) add, and McPAT's Wattch-derived
		 * "1.15 nJ" per-op literal is no longer used for the ALU (nor for the DMA adder, the same unit). */
		if (alu)
		{
			pimid_int_e16 = (0.03e-12 + 8.0 * (0.1e-12 - 0.03e-12) / 24.0) * k45;
			pimid_int_e64 = 2.0 * pimid_int_e[1];
			per_access_energy = pimid_int_e64 / g_tp.sckt_co_eff;   /* x sckt below = the 64-bit add */
			std::cerr << "[mcpat int] core" << ithCore << " ALU add/logic LINEAR in width (DERIVED from the two published"
			          << " points): 16 bit " << pimid_int_e16*1e12 << " pJ, 64 bit = 2 x 32 bit " << pimid_int_e64*1e12
			          << " pJ (USED, also for unclassified ALU accesses); McPAT's per-op literal " << mcpat_ref_op*1e12
			          << " pJ no longer used" << std::endl;
		}
		std::cerr << "[mcpat int] core" << ithCore << " " << (alu ? "ALU" : "MUL") << " published (Horowitz ISSCC 2014"
		          << " Fig. 1.1.9, 45 nm 0.9 V): " << (alu ? "Add 8 bit 0.03 pJ, 32 bit 0.1 pJ" : "Mult 8 bit 0.2 pJ, 32 bit 3.1 pJ")
		          << "; node scaling 45 -> " << interface_ip.F_sz_nm << " nm: C_inv " << c_src*1e15 << " fF -> " << c_run*1e15
		          << " fF, Vdd 0.9 V -> " << v_run << " V => CACTI factor " << k45_cacti << " (reference); USED "
		          << (sil ? "published-silicon ratio (DeepScaleTool, ISCAS 2021; voltage-less) " : "CACTI ratio (node outside the silicon fit) ")
		          << k45 << ": 8 bit " << pimid_int_e[0]*1e12
		          << " pJ, 32 bit " << pimid_int_e[1]*1e12 << " pJ (USED); McPAT per-op (reference) "
		          << mcpat_ref_op*1e12 << " pJ" << std::endl;
		if (!alu)
			std::cerr << "[mcpat int] core" << ithCore << " NO PUBLISHED SOURCE FOUND for 16- and 64-bit multiply and integer"
			          << " divide: McPAT's per-op energy kept" << std::endl;
	}

	/* PIMID 1.12.29: (user ruling 2026-10-25) PUBLISHED FUNCTIONAL-UNIT AREA
	 * (power.models.fu_area_published, sys param pimid_fu_area_published).
	 * McPAT's unit areas above are literals: FPU 4.47 mm^2 (embedded, ARM
	 * VFP+NEON) / 8.47 mm^2 (server) at 90 nm x (F/90)^2, ALU 280 x 260 um^2
	 * (x2 server), MUL 3x the ALU, x logic_scaling_co_eff. At 22 nm the
	 * embedded FPU alone is ~0.29 mm^2, against a whole RI5CY core of
	 * 0.068 mm^2 at 65 nm (Gautschi et al., TVLSI 2017, Table V). Replaced,
	 * per unit, by published areas:
	 *  [M21] S. Mach, F. Schuiki, F. Zaruba, L. Benini, "FPnew: an open-source
	 *   multiformat floating-point unit architecture for energy-proportional
	 *   transprecision computing", IEEE TVLSI 29(4), 2021 (arXiv 2007.01530),
	 *   GF 22FDX, cell area in kGE with 1 GE = 0.199 um^2 (Fig. 4 and Fig. 6
	 *   captions):
	 *   - FP datapath 32 bit: Sec. IV-A.2 / Fig. 4, RI5CY with the TP-FPU
	 *     (W_fpu = 32, Table I: FP32 + FP16 + FP16alt merged FMA, COMP,
	 *     CONV, DIVSQRT disabled): FPU 69 kGE = 13731 um^2.
	 *   - FP datapath 64 bit (device scope): Sec. IV-B.2 / Fig. 6 and Table
	 *     IV, Ariane TP-FPU (W_fpu = 64, Table II: FP64 + FP32 + FP16 +
	 *     FP16alt + FP8, DIVSQRT, COMP, CONV): FPU 247 kGE = 0.049 mm^2.
	 *   - Integer, 32 bit: Fig. 4, RI5CY "Int ALU" 10 kGE = 1990 um^2 (the
	 *     iterative divider lives in the ALU: it reuses the ALU's
	 *     comparators, shifters and adders, 2 kGE, Gautschi 2017 Sec. IV-E.4),
	 *     "Int Mult" 18 kGE = 3582 um^2 (32x32 multiplier incl. the 16b/8b
	 *     dot-product units, Gautschi 2017 Sec. IV-F). The divider is counted
	 *     once, in the ALU.
	 *  NO PUBLISHED SOURCE FOUND: 64-bit integer ALU/MUL (FPnew Fig. 6 does
	 *   not break Ariane's integer units out), and the host (server scope)
	 *   FPU, whose x86-64 FP/SIMD datapath is at least 128 bit (SSE2): no
	 *   FPnew configuration matches. Those keep McPAT's literal, printed.
	 * NODE SCALING: area x (F_run / 22 nm)^2, the feature-size-squared rule
	 * McPAT applies to its own FPU literal (CACTI/McPAT feature-size scaling;
	 * 22FDX taken as 22 nm). McPAT's macro layout overhead is applied after,
	 * as to every unit (the published figures are cell area), and the
	 * DRAM-periphery area factor where the unit sits on DRAM silicon
	 * (processor.cc) -- both are process/layout factors, not FU literals.
	 * LEAKAGE follows the area: McPAT's leakage is area x tx density x
	 * Isub/Ig per unit area, so it is rescaled by the same ratio (nothing
	 * else of McPAT's leakage model changes). FU_height (bypass wire length)
	 * and the per-access energy are unchanged. GUARDS (printed, not used):
	 * Gautschi 2017 Sec. IV-F text, 65 nm, "8.3 kGE ALU, 12.6 kGE multiplier"
	 * with 1 GE = 0.077 mm^2 / 53.5 kGE (Table V, extended core), node-scaled
	 * (F/65)^2; DERIVED FP32-only RI5CY FPU = 69 - (147 - 147/1.29) kGE
	 * ([M21] Sec. IV-A.2: the TP-FPU core is 147 kGE, 29% above the
	 * FP32-only core). */
	if (XML->sys.pimid_fu_area_published)
	{
		const char *fu_nm = (fu_type == FPU) ? "FPU" : (fu_type == ALU) ? "ALU" : "MUL";
		const int    w     = (fu_type == FPU) ? coredynp.fp_data_width : coredynp.int_data_width;
		const double F     = g_ip->F_sz_nm;
		const double ge22  = 0.199;   /* um^2 per GE, GF 22FDX ([M21] Fig. 4, Fig. 6) */
		const double k22   = (F / 22.0) * (F / 22.0);
		double kge = 0;
		const char *cite = "";
		if (fu_type == FPU)
		{
			if (w == 32) { kge = 69.0;  cite = "FPnew (Mach et al., TVLSI 2021) Sec. IV-A.2 / Fig. 4: RI5CY TP-FPU, W_fpu 32 (FP32+FP16+FP16alt FMA, COMP, CONV; no DIVSQRT), 69 kGE, 22FDX"; }
			else if (w == 64 && XML->sys.Embedded) { kge = 247.0; cite = "FPnew (Mach et al., TVLSI 2021) Sec. IV-B.2 / Fig. 6 / Table IV: Ariane TP-FPU, W_fpu 64 (FP64..FP8, DIVSQRT, COMP, CONV), 247 kGE = 0.049 mm^2, 22FDX"; }
		}
		else if (w == 32)
		{
			if (fu_type == ALU) { kge = 10.0; cite = "FPnew (Mach et al., TVLSI 2021) Fig. 4: RI5CY Int ALU (incl. the iterative divider, Gautschi 2017 Sec. IV-E.4), 10 kGE, 22FDX"; }
			else                { kge = 18.0; cite = "FPnew (Mach et al., TVLSI 2021) Fig. 4: RI5CY Int Mult (32x32 + dot-product units), 18 kGE, 22FDX"; }
		}
		if (kge > 0 && area_t > 0)
		{
			const double a_pub = kge * 1e3 * ge22 * k22;   /* um^2, one unit, run node */
			const double r = a_pub / area_t;
			std::cerr << "[mcpat fu area] core" << ithCore << " " << fu_nm << " (" << w << " bit, "
			          << (XML->sys.Embedded ? "embedded" : "server") << " scope): McPAT literal " << area_t*1e-6
			          << " mm^2 per unit (reference) -> PUBLISHED " << kge << " kGE x " << ge22 << " um^2/GE x (" << F
			          << "/22)^2 = " << a_pub*1e-6 << " mm^2 per unit (USED; " << cite << "); ratio " << r
			          << "; leakage " << leakage << " -> " << leakage*r << " W, gate " << gate_leakage << " -> "
			          << gate_leakage*r << " W per unit; x " << num_fu << " units, x macro layout overhead "
			          << g_tp.macro_layout_overhead << std::endl;
			if (fu_type == FPU && w == 32)
				std::cerr << "[mcpat fu area] core" << ithCore << " FPU guard (not used): DERIVED FP32-only RI5CY FPU"
				          << " 69 - (147 - 147/1.29) = " << 69.0 - (147.0 - 147.0/1.29) << " kGE = "
				          << (69.0 - (147.0 - 147.0/1.29))*1e3*ge22*k22*1e-6 << " mm^2 at the run node (the published"
				          << " TP-FPU also carries FP16/FP16alt lanes; it has no DIVSQRT)" << std::endl;
			if (fu_type != FPU)
			{
				const double ge65 = 0.077e6 / 53.5e3;   /* um^2 per GE, Gautschi 2017 Table V (65 nm) */
				const double g = ((fu_type == ALU) ? 8.3 : 12.6) * 1e3 * ge65 * (F / 65.0) * (F / 65.0);
				std::cerr << "[mcpat fu area] core" << ithCore << " " << fu_nm << " guard (not used): Gautschi et al."
				          << " 2017 Sec. IV-F, 65 nm, " << ((fu_type == ALU) ? "8.3" : "12.6") << " kGE x " << ge65
				          << " um^2/GE (Table V) x (" << F << "/65)^2 = " << g*1e-6 << " mm^2 (USED / guard = "
				          << a_pub/g << ")" << std::endl;
			}
			leakage      *= r;
			gate_leakage *= r;
			area_t        = a_pub;
			pimid_area_pub = a_pub;
			/* PIMID (next, user ruling 2026-10-25 #2): the unit's HEIGHT (the bypass buses' length, core.cc)
			 * = sqrt of its published area (a square unit, DERIVED), not McPAT's "Sun's data" literal. */
			const double h_mcpat = FU_height;
			FU_height = sqrt(a_pub) * num_fu;
			std::cerr << "[mcpat fu area] core" << ithCore << " " << fu_nm << " height (bypass length): McPAT literal "
			          << h_mcpat << " um -> sqrt(published area) x " << num_fu << " unit(s) = " << FU_height
			          << " um (USED, DERIVED)" << std::endl;
		}
		else
			std::cerr << "[mcpat fu area] core" << ithCore << " " << fu_nm << " (" << w << " bit, "
			          << (XML->sys.Embedded ? "embedded" : "server") << " scope): NO PUBLISHED SOURCE FOUND ("
			          << ((fu_type == FPU) ? "no FPnew configuration matches this FP datapath; the host x86-64 FP/SIMD datapath is >= 128 bit"
			                               : "64-bit integer units are not broken out in FPnew Fig. 6 or Gautschi 2017")
			          << "): McPAT's area literal kept, " << area_t*1e-6 << " mm^2 per unit, and its height literal "
			          << FU_height << " um (bypass length)" << std::endl;
	}

	//IEXEU, simple ALU and FPU
	//  double C_ALU, C_EXEU, C_FPU; //Lum Equivalent capacitance of IEXEU and FPU. Based on Intel and Sun 90nm process fabracation.
	//
	//  C_ALU	  = 0.025e-9;//F
	//  C_EXEU  = 0.05e-9; //F
	//  C_FPU	  = 0.35e-9;//F
    area.set_area(area_t*num_fu);
    leakage *= num_fu;
    gate_leakage *=num_fu;
	double macro_layout_overhead = g_tp.macro_layout_overhead;
//	if (!XML->sys.Embedded)
		area.set_area(area.get_area()*macro_layout_overhead);
}

void FunctionalUnit::computeEnergy(bool is_tdp)
{
	double pppm_t[4]    = {1,1,1,1};
	double FU_duty_cycle;
	if (is_tdp)
	{


		set_pppm(pppm_t, 2, 2, 2, 2);//2 means two source operands needs to be passed for each int instruction.
		if (fu_type == FPU)
		{
			stats_t.readAc.access = num_fu;
			tdp_stats = stats_t;
			FU_duty_cycle = coredynp.FPU_duty_cycle;
		}
		else if (fu_type == ALU)
		{
			stats_t.readAc.access = 1*num_fu;
			tdp_stats = stats_t;
			FU_duty_cycle = coredynp.ALU_duty_cycle;
		}
		else if (fu_type == MUL)
		{
			stats_t.readAc.access = num_fu;
			tdp_stats = stats_t;
			FU_duty_cycle = coredynp.MUL_duty_cycle;
		}

	    //power.readOp.dynamic = base_energy/clockRate + energy*stats_t.readAc.access;
	    power.readOp.dynamic = per_access_energy*stats_t.readAc.access + base_energy/clockRate;
		double sckRation = g_tp.sckt_co_eff;
		power.readOp.dynamic *= sckRation*FU_duty_cycle;
		power.writeOp.dynamic *= sckRation;
		power.searchOp.dynamic *= sckRation;

	    power.readOp.leakage = leakage;
	    power.readOp.gate_leakage = gate_leakage;
	    double long_channel_device_reduction = longer_channel_device_reduction(Core_device, coredynp.core_ty);
	    power.readOp.longer_channel_leakage	= power.readOp.leakage*long_channel_device_reduction;

    	double pg_reduction = power_gating_leakage_reduction(false);
    	power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;
  	    power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * long_channel_device_reduction;



	}
	else
	{
		if (fu_type == FPU)
		{
			stats_t.readAc.access = XML->sys.core[ithCore].fpu_accesses;
			rtp_stats = stats_t;
		}
		else if (fu_type == ALU)
		{
			stats_t.readAc.access = XML->sys.core[ithCore].ialu_accesses;
			rtp_stats = stats_t;
		}
		else if (fu_type == MUL)
		{
			stats_t.readAc.access = XML->sys.core[ithCore].mul_accesses;
			rtp_stats = stats_t;
		}

	    //rt_power.readOp.dynamic = base_energy*executionTime + energy*stats_t.readAc.access;
	    rt_power.readOp.dynamic = per_access_energy*stats_t.readAc.access + base_energy*executionTime;
		double sckRation = g_tp.sckt_co_eff;
		rt_power.readOp.dynamic *= sckRation;
		rt_power.writeOp.dynamic *= sckRation;
		rt_power.searchOp.dynamic *= sckRation;
		/* PIMID 1.12.18 (rulings 2026-10-23 #1, #2): the classified FP
		 * operations (FP32/FP64 add, mult, FMA) are priced at the published
		 * per-operation energy (constructor) per SCALAR operation (lanes);
		 * divide/sqrt, x87 and the rest keep McPAT's per-op energy. The
		 * published value is a silicon energy, so McPAT's sckt_co_eff is not
		 * applied to it. */
		if (fu_type == FPU && pimid_pub)
		{
			const double* ins = XML->sys.core[ithCore].pimid_fp_ins;
			const double* ln  = XML->sys.core[ithCore].pimid_fp_lanes;
			const double acc = stats_t.readAc.access;
			double rep = 0, pub = 0;
			for (int q = 0; q < 6; q++) { rep += ins[q]; pub += pimid_pub_e[q]*ln[q]; }
			if (rep > 0 && rep <= acc)
			{
				const double before = rt_power.readOp.dynamic;
				rt_power.readOp.dynamic = (per_access_energy*(acc - rep) + base_energy*executionTime)*sckRation + pub;
				std::cerr << "[mcpat fpu] core" << ithCore << " runtime: " << acc << " FPU ops; published (instr / scalar ops x pJ):";
				static const char* const nm[6] = {"FP32 add", "FP32 mult", "FP64 add", "FP64 mult", "FP32 FMA", "FP64 FMA"};
				for (int q = 0; q < 6; q++)
					std::cerr << " " << nm[q] << " " << ins[q] << " / " << ln[q] << " x " << pimid_pub_e[q]*1e12 << ";";
				std::cerr << " = " << pub << " J; McPAT per-op " << per_access_energy*sckRation*1e12 << " pJ x " << (acc - rep)
				          << " other FP ops (div/sqrt " << ins[6] + ins[7] << ", x87 " << ins[8] << ", rest min/max/cmp/cvt/shuffle "
				          << (acc - rep - ins[6]-ins[7]-ins[8]) << "); McPAT-only reference " << before << " J -> FPU dynamic "
				          << rt_power.readOp.dynamic << " J" << std::endl;
			}
			else if (rep > acc)
				std::cerr << "[mcpat fpu] core" << ithCore << " WARNING: classified FP instructions (" << rep
				          << ") exceed the FPU accesses (" << acc << "): bases disagree, McPAT's per-op energy kept" << std::endl;
			else
				std::cerr << "[mcpat fpu] core" << ithCore << " runtime: no classified FP add/mult/FMA counted (" << acc
				          << " FPU ops at McPAT's per-op energy)" << std::endl;
		}
		/* PIMID 1.12.18 (ruling 2026-10-23 #3): the decoder's 8- and 32-bit
		 * integer add/logic (ALU) and multiply (MUL) operations at the
		 * published per-op energy; every other access keeps McPAT's. */
		if ((fu_type == ALU || fu_type == MUL) && pimid_int_pub)
		{
			const double* ops = XML->sys.core[ithCore].pimid_int_ops;
			const bool alu = (fu_type == ALU);
			const double n8 = alu ? ops[0] : ops[4], n32 = alu ? ops[2] : ops[6];
			/* (next, ruling #1): the ALU's 16- and 64-bit add/logic are priced (linear in width) */
			const double n16 = alu ? ops[1] : 0.0, n64 = alu ? ops[3] : 0.0;
			const double nnp = alu ? 0.0 : ops[5] + ops[7] + ops[8];
			const double acc = stats_t.readAc.access;
			const double rep = n8 + n32 + n16 + n64;
			if (rep > 0 && rep <= acc)
			{
				const double before = rt_power.readOp.dynamic;
				const double pub = pimid_int_e[0]*n8 + pimid_int_e[1]*n32 + pimid_int_e16*n16 + pimid_int_e64*n64;
				rt_power.readOp.dynamic = (per_access_energy*(acc - rep) + base_energy*executionTime)*sckRation + pub;
				std::cerr << "[mcpat int] core" << ithCore << " " << (alu ? "ALU" : "MUL") << " runtime: " << acc
				          << " accesses; published: 8 bit " << n8 << " x " << pimid_int_e[0]*1e12 << " pJ + 32 bit " << n32
				          << " x " << pimid_int_e[1]*1e12 << " pJ" << (alu ? " + DERIVED 16 bit " : "") << (alu ? n16 : 0.0)
				          << (alu ? " x " : "") << (alu ? pimid_int_e16*1e12 : 0.0) << (alu ? " pJ + 64 bit " : "") << (alu ? n64 : 0.0)
				          << (alu ? " x " : "") << (alu ? pimid_int_e64*1e12 : 0.0) << " = " << pub << " J; "
				          << (alu ? "unclassified at the 64-bit add " : "McPAT per-op ")
				          << per_access_energy*sckRation*1e12 << " pJ x " << (acc - rep) << " other (" << nnp
				          << (alu ? " -" : " 16/64-bit mult + divide -- NO PUBLISHED SOURCE FOUND") << "; "
				          << (acc - rep - nnp) << (alu ? " moves/LEA/NOP/other" : " other") << "); McPAT-only reference " << before
				          << " J -> " << rt_power.readOp.dynamic << " J" << std::endl;
			}
			else if (rep > acc)
				std::cerr << "[mcpat int] core" << ithCore << " WARNING: classified 8/32-bit integer ops (" << rep
				          << ") exceed the " << (alu ? "ALU" : "MUL") << " accesses (" << acc << "): bases disagree, McPAT's per-op energy kept" << std::endl;
			else
				std::cerr << "[mcpat int] core" << ithCore << " " << (alu ? "ALU" : "MUL") << " runtime: no 8/32-bit operation counted ("
				          << acc << " accesses at McPAT's per-op energy)" << std::endl;
		}

	}


}

void FunctionalUnit::displayEnergy(uint32_t indent,int plevel,bool is_tdp)
{
	string indent_str(indent, ' ');
	string indent_str_next(indent+2, ' ');
	bool long_channel = XML->sys.longer_channel_device;
	bool power_gating = XML->sys.power_gating;

//	cout << indent_str_next << "Results Broadcast Bus Area = " << bypass->area.get_area() *1e-6 << " mm^2" << endl;
	if (is_tdp)
	{
		if (fu_type == FPU)
		{
			cout << indent_str << "Floating Point Units (FPUs) (Count: "<< coredynp.num_fpus <<" ):" << endl;
			cout << indent_str_next << "Area = " << area.get_area()*1e-6  << " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << power.readOp.dynamic*clockRate  << " W" << endl;
//			cout << indent_str_next << "Subthreshold Leakage = " << power.readOp.leakage  << " W" << endl;
			cout << indent_str_next<< "Subthreshold Leakage = "
						<< (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << power.readOp.gate_leakage  << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << rt_power.readOp.dynamic/executionTime << " W" << endl;
			cout <<endl;
		}
		else if (fu_type == ALU)
		{
			cout << indent_str << "Integer ALUs (Count: "<< coredynp.num_alus <<" ):" << endl;
			cout << indent_str_next << "Area = " << area.get_area()*1e-6  << " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << power.readOp.dynamic*clockRate  << " W" << endl;
//			cout << indent_str_next << "Subthreshold Leakage = " << power.readOp.leakage  << " W" << endl;
			cout << indent_str_next<< "Subthreshold Leakage = "
						<< (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << power.readOp.gate_leakage  << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << rt_power.readOp.dynamic/executionTime << " W" << endl;
			cout <<endl;
		}
		else if (fu_type == MUL)
		{
			cout << indent_str << "Complex ALUs (Mul/Div) (Count: "<< coredynp.num_muls <<" ):" << endl;
			cout << indent_str_next << "Area = " << area.get_area()*1e-6  << " mm^2" << endl;
			cout << indent_str_next << "Peak Dynamic = " << power.readOp.dynamic*clockRate  << " W" << endl;
//			cout << indent_str_next << "Subthreshold Leakage = " << power.readOp.leakage  << " W" << endl;
			cout << indent_str_next<< "Subthreshold Leakage = "
						<< (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
			if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
					<< (long_channel? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)  << " W" << endl;
			cout << indent_str_next << "Gate Leakage = " << power.readOp.gate_leakage  << " W" << endl;
			cout << indent_str_next << "Runtime Dynamic = " << rt_power.readOp.dynamic/executionTime << " W" << endl;
			cout <<endl;

		}

	}
	else
	{
	}

}

void FunctionalUnit::leakage_feedback(double temperature)
{
  // Update the temperature and initialize the global interfaces.
  interface_ip.temp = (unsigned int)round(temperature/10.0)*10;

  uca_org_t init_result = init_interface(&interface_ip); // init_result is dummy

  // This is part of FunctionalUnit()
  double area_t, leakage, gate_leakage;
  double pmos_to_nmos_sizing_r = pmos_to_nmos_sz_ratio();

  if (fu_type == FPU)
  {
	area_t = 4.47*1e6*(g_ip->F_sz_nm*g_ip->F_sz_nm/90.0/90.0);//this is um^2 The base number
	if (g_ip->F_sz_nm>90)
		area_t = 4.47*1e6*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2
	if (pimid_area_pub > 0) area_t = pimid_area_pub;   /* PIMID 1.12.29: published area (constructor) */
	leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(5*g_tp.min_w_nmos_, 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
	gate_leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(5*g_tp.min_w_nmos_, 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
  }
  else if (fu_type == ALU)
  {
    area_t = 280*260*2*num_fu*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2 ALU + MUl
    if (pimid_area_pub > 0) area_t = pimid_area_pub;   /* PIMID 1.12.29: published area per unit (constructor) */
    leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
    gate_leakage = area_t*(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;
  }
  else if (fu_type == MUL)
  {
    area_t = 280*260*2*3*num_fu*g_tp.scaling_factor.logic_scaling_co_eff;//this is um^2 ALU + MUl
    if (pimid_area_pub > 0) area_t = pimid_area_pub;   /* PIMID 1.12.29: published area per unit (constructor) */
    leakage = area_t *(g_tp.scaling_factor.core_tx_density)*cmos_Isub_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;//unit W
    gate_leakage = area_t*(g_tp.scaling_factor.core_tx_density)*cmos_Ig_leakage(20*g_tp.min_w_nmos_, 20*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd/2;
  }
  else
  {
    cout<<"Unknown Functional Unit Type"<<endl;
    exit(1);
  }

  power.readOp.leakage = leakage*num_fu;
  power.readOp.gate_leakage = gate_leakage*num_fu;
  power.readOp.longer_channel_leakage = longer_channel_device_reduction(Core_device, coredynp.core_ty)*power.readOp.leakage;

  double pg_reduction = power_gating_leakage_reduction(false);
  power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;

}

UndiffCore::UndiffCore(ParseXML* XML_interface, int ithCore_, InputParameter* interface_ip_, const CoreDynParam & dyn_p_, bool exist_,  bool embedded_)
:XML(XML_interface),
 ithCore(ithCore_),
 interface_ip(*interface_ip_),
 coredynp(dyn_p_),
 core_ty(coredynp.core_ty),
 embedded(XML->sys.Embedded),
 pipeline_stage(coredynp.pipeline_stages),
 num_hthreads(coredynp.num_hthreads),
 issue_width(coredynp.issueW),
 exist(exist_)
// is_default(_is_default)
{
	if (!exist) return;
	double undifferentiated_core=0;
	double core_tx_density=0;
	double pmos_to_nmos_sizing_r = pmos_to_nmos_sz_ratio();
	double undifferentiated_core_coe;
	//XML_interface=_XML_interface;
	uca_org_t result2;
	result2 = init_interface(&interface_ip);

	//Compute undifferentiated core area at 90nm.
	if (embedded==false)
	{
		//Based on the results of polynomial/log curve fitting based on undifferentiated core of Niagara, Niagara2, Merom, Penyrn, Prescott, Opteron die measurements
		if (core_ty==OOO)
		{
			//undifferentiated_core = (0.0764*pipeline_stage*pipeline_stage -2.3685*pipeline_stage + 10.405);//OOO
			undifferentiated_core = (3.57*log(pipeline_stage)-1.2643)>0?(3.57*log(pipeline_stage)-1.2643):0;
		}
		else if (core_ty==Inorder)
		{
			//undifferentiated_core = (0.1238*pipeline_stage + 7.2572)*0.9;//inorder
			undifferentiated_core = (-2.19*log(pipeline_stage)+6.55)>0?(-2.19*log(pipeline_stage)+6.55):0;
		}
		else
		{
			cout<<"invalid core type"<<endl;
			exit(0);
		}
		undifferentiated_core *= (1+ logtwo(num_hthreads)* 0.0716);
	}
	else
	{
		//Based on the results in paper "parametrized processor models" Sandia Labs
		if (XML->sys.opt_clockrate)
			undifferentiated_core_coe = 0.05;
		else
			undifferentiated_core_coe = 0;
		undifferentiated_core = (0.4109* pipeline_stage - 0.776)*undifferentiated_core_coe;
		undifferentiated_core *= (1+ logtwo(num_hthreads)* 0.0426);
	}

	undifferentiated_core 		    *= g_tp.scaling_factor.logic_scaling_co_eff*1e6;//change from mm^2 to um^2
	core_tx_density                 = g_tp.scaling_factor.core_tx_density;
	//undifferentiated_core 		    = 3*1e6;
	//undifferentiated_core			*= g_tp.scaling_factor.logic_scaling_co_eff;//(g_ip->F_sz_um*g_ip->F_sz_um/0.09/0.09)*;
	power.readOp.leakage = undifferentiated_core*(core_tx_density)*cmos_Isub_leakage(5*g_tp.min_w_nmos_, 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd;//unit W
	power.readOp.gate_leakage = undifferentiated_core*(core_tx_density)*cmos_Ig_leakage(5*g_tp.min_w_nmos_, 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, 1, inv)*g_tp.peri_global.Vdd;

	double long_channel_device_reduction = longer_channel_device_reduction(Core_device, coredynp.core_ty);
	power.readOp.longer_channel_leakage	= power.readOp.leakage*long_channel_device_reduction;

	double pg_reduction = power_gating_leakage_reduction(false);
	power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;
	power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * long_channel_device_reduction;



	area.set_area(undifferentiated_core);

	scktRatio = g_tp.sckt_co_eff;
	power.readOp.dynamic *= scktRatio;
	power.writeOp.dynamic *= scktRatio;
	power.searchOp.dynamic *= scktRatio;
	macro_PR_overhead = g_tp.macro_layout_overhead;
	area.set_area(area.get_area()*macro_PR_overhead);



//		double vt=g_tp.peri_global.Vth;
//		double velocity_index=1.1;
//		double c_in=gate_C(g_tp.min_w_nmos_, g_tp.min_w_nmos_*pmos_to_nmos_sizing_r , 0.0, false);
//		double c_out= drain_C_(g_tp.min_w_nmos_, NCH, 2, 1, g_tp.cell_h_def, false) + drain_C_(g_tp.min_w_nmos_*pmos_to_nmos_sizing_r, PCH, 1, 1, g_tp.cell_h_def, false) + c_in;
//		double w_nmos=g_tp.min_w_nmos_;
//		double w_pmos=g_tp.min_w_nmos_*pmos_to_nmos_sizing_r;
//		double i_on_n=1.0;
//		double i_on_p=1.0;
//		double i_on_n_in=1.0;
//		double i_on_p_in=1;
//		double vdd=g_tp.peri_global.Vdd;

//		power.readOp.sc=shortcircuit_simple(vt, velocity_index, c_in, c_out, w_nmos,w_pmos, i_on_n, i_on_p,i_on_n_in, i_on_p_in, vdd);
//		power.readOp.dynamic=c_out*vdd*vdd/2;

//		cout<<power.readOp.dynamic << "dynamic" <<endl;
//		cout<<power.readOp.sc << "sc" << endl;

//		power.readOp.sc=shortcircuit(vt, velocity_index, c_in, c_out, w_nmos,w_pmos, i_on_n, i_on_p,i_on_n_in, i_on_p_in, vdd);
//		power.readOp.dynamic=c_out*vdd*vdd/2;
//
//		cout<<power.readOp.dynamic << "dynamic" <<endl;
//		cout<<power.readOp.sc << "sc" << endl;



}


void UndiffCore::displayEnergy(uint32_t indent,int plevel,bool is_tdp)
{
	string indent_str(indent, ' ');
	string indent_str_next(indent+2, ' ');
	bool long_channel = XML->sys.longer_channel_device;
	bool power_gating = XML->sys.power_gating;

	if (is_tdp)
	{
		cout << indent_str << "UndiffCore:" << endl;
		cout << indent_str_next << "Area = " << area.get_area()*1e-6<< " mm^2" << endl;
		cout << indent_str_next << "Peak Dynamic = " << power.readOp.dynamic*clockRate << " W" << endl;
		//cout << indent_str_next << "Subthreshold Leakage = " << power.readOp.leakage <<" W" << endl;
		cout << indent_str_next<< "Subthreshold Leakage = "
					<< (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
		if (power_gating) cout << indent_str_next << "Subthreshold Leakage with power gating = "
				<< (long_channel? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)  << " W" << endl;
		cout << indent_str_next << "Gate Leakage = " << power.readOp.gate_leakage << " W" << endl;
		//cout << indent_str_next << "Runtime Dynamic = " << rt_power.readOp.dynamic/executionTime << " W" << endl;
		cout <<endl;
	}
	else
	{
		cout << indent_str << "UndiffCore:" << endl;
		cout << indent_str_next << "Area = " << area.get_area()*1e-6<< " mm^2" << endl;
		cout << indent_str_next << "Peak Dynamic = " << power.readOp.dynamic*clockRate << " W" << endl;
		cout << indent_str_next << "Subthreshold Leakage = " << power.readOp.leakage <<" W" << endl;
		cout << indent_str_next << "Gate Leakage = " << power.readOp.gate_leakage << " W" << endl;
		//cout << indent_str_next << "Runtime Dynamic = " << rt_power.readOp.dynamic/executionTime << " W" << endl;
		cout <<endl;
	}

}

inst_decoder::inst_decoder(
		bool   _is_default,
		const InputParameter *configure_interface,
		int opcode_length_,
		int num_decoders_,
		bool x86_,
	    enum Device_ty device_ty_,
	    enum Core_type core_ty_,
	    double out_fanout_)
:is_default(_is_default),
 opcode_length(opcode_length_),
 num_decoders(num_decoders_),
 x86(x86_),
 device_ty(device_ty_),
 core_ty(core_ty_)
 {
			/*
			 * Instruction decoder is different from n to 2^n decoders
			 * that are commonly used in row decoders in memory arrays.
			 * The RISC instruction decoder is typically a very simple device.
			 * We can decode an instruction by simply
			 * separating the machine word into small parts using wire slices
			 * The RISC instruction decoder can be approximate by the n to 2^n decoders,
			 * although this approximation usually underestimate power since each decoded
			 * instruction normally has more than 1 active signal.
			 *
			 * However, decoding a CISC instruction word is much more difficult
			 * than the RISC case. A CISC decoder is typically set up as a state machine.
			 * The machine reads the opcode field to determine
			 * what type of instruction it is,
			 * and where the other data values are.
			 * The instruction word is read in piece by piece,
			 * and decisions are made at each stage as to
			 * how the remainder of the instruction word will be read.
			 * (sequencer and ROM are usually needed)
			 * An x86 decoder can be even more complex since
			 * it involve  both decoding instructions into u-ops and
			 * merge u-ops when doing micro-ops fusion.
			 */
			bool is_dram=false;
			double pmos_to_nmos_sizing_r;
			double load_nmos_width, load_pmos_width;
			double C_driver_load, R_wire_load;
			Area cell;

			l_ip=*configure_interface;
			local_result = init_interface(&l_ip);
			cell.h =g_tp.cell_h_def;
			cell.w =g_tp.cell_h_def;

			num_decoder_segments = (int)ceil(opcode_length/18.0);
			if (opcode_length > 18)	opcode_length = 18;
			num_decoded_signals= (int)pow(2.0,opcode_length);
			pmos_to_nmos_sizing_r = pmos_to_nmos_sz_ratio();
			load_nmos_width=g_tp.max_w_nmos_ /2;
			load_pmos_width= g_tp.max_w_nmos_ * pmos_to_nmos_sizing_r;
			if (out_fanout_ > 0)
			{
				/* PIMID (next, user ruling 2026-10-24): the decoded output's load is DERIVED from the
				 * core's own control structure -- out_fanout_ minimum-size gates (min_w_nmos_ NMOS +
				 * the McPAT P/N ratio PMOS), out_fanout_ = the pipeline-register sink count the clock
				 * tree is built over (core.cc InstFetchU). Upstream McPAT's placeholder,
				 * 1024 x a maximum-width gate (100 F NMOS; "TODO: this number 1024 needs to be
				 * revisited"), put ~68k minimum-transistor widths on every decoded line: about
				 * 4.7 pF at 32 nm, 3.8 pJ per decoder per instruction, more than a whole RI5CY core
				 * (Gautschi et al., TVLSI 2017, Table V: 26.28 uW/MHz at 65 nm) node-scaled. */
				const double wn_min = g_tp.min_w_nmos_;
				C_driver_load = out_fanout_ * gate_C(wn_min + wn_min * pmos_to_nmos_sizing_r, 0, is_dram);
			}
			else
				C_driver_load = 1024*gate_C(load_nmos_width + load_pmos_width, 0, is_dram); //TODO: this number 1024 needs to be revisited
			R_wire_load   = 3000*l_ip.F_sz_um * g_tp.wire_outside_mat.R_per_um;

			final_dec = new Decoder(
					num_decoded_signals,
					false,
					C_driver_load,
					R_wire_load,
					false/*is_fa*/,
					false/*is_dram*/,
					false/*wl_tr*/, //to use peri device
					cell);

			PredecBlk * predec_blk1 = new PredecBlk(
					num_decoded_signals,
					final_dec,
					0,//Assuming predec and dec are back to back
					0,
					1,//Each Predec only drives one final dec
					false/*is_dram*/,
					true);
			PredecBlk * predec_blk2 = new PredecBlk(
					num_decoded_signals,
					final_dec,
					0,//Assuming predec and dec are back to back
					0,
					1,//Each Predec only drives one final dec
					false/*is_dram*/,
					false);

			PredecBlkDrv * predec_blk_drv1 = new PredecBlkDrv(0, predec_blk1, false);
			PredecBlkDrv * predec_blk_drv2 = new PredecBlkDrv(0, predec_blk2, false);

			pre_dec            = new Predec(predec_blk_drv1, predec_blk_drv2);

			double area_decoder = final_dec->area.get_area() * num_decoded_signals * num_decoder_segments*num_decoders;
			//double w_decoder    = area_decoder / area.get_h();
			double area_pre_dec = (predec_blk_drv1->area.get_area() +
					predec_blk_drv2->area.get_area() +
					predec_blk1->area.get_area() +
					predec_blk2->area.get_area())*
					num_decoder_segments*num_decoders;
			area.set_area(area.get_area()+ area_decoder + area_pre_dec);
			double macro_layout_overhead   = g_tp.macro_layout_overhead;
			double chip_PR_overhead        = g_tp.chip_layout_overhead;
			area.set_area(area.get_area()*macro_layout_overhead*chip_PR_overhead);

			inst_decoder_delay_power();

			double sckRation = g_tp.sckt_co_eff;
			power.readOp.dynamic *= sckRation;
			power.writeOp.dynamic *= sckRation;
			power.searchOp.dynamic *= sckRation;

			double long_channel_device_reduction = longer_channel_device_reduction(device_ty,core_ty);
			power.readOp.longer_channel_leakage	= power.readOp.leakage*long_channel_device_reduction;

	    	double pg_reduction = power_gating_leakage_reduction(false);
	    	power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;
	  	    power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * long_channel_device_reduction;



}

void inst_decoder::inst_decoder_delay_power()
{

	double dec_outrisetime;
	double inrisetime=0, outrisetime;
	double pppm_t[4]    = {1,1,1,1};
	double squencer_passes = x86?2:1;

	outrisetime = pre_dec->compute_delays(inrisetime);
	dec_outrisetime = final_dec->compute_delays(outrisetime);
	set_pppm(pppm_t, squencer_passes*num_decoder_segments, num_decoder_segments, squencer_passes*num_decoder_segments, num_decoder_segments);
    power = power + pre_dec->power*pppm_t;
    set_pppm(pppm_t, squencer_passes*num_decoder_segments, num_decoder_segments*num_decoded_signals,
    		num_decoder_segments*num_decoded_signals, squencer_passes*num_decoder_segments);
    power = power + final_dec->power*pppm_t;
}
void inst_decoder::leakage_feedback(double temperature)
{
  l_ip.temp = (unsigned int)round(temperature/10.0)*10;
  uca_org_t init_result = init_interface(&l_ip); // init_result is dummy

  final_dec->leakage_feedback(temperature);
  pre_dec->leakage_feedback(temperature);

  double pppm_t[4]    = {1,1,1,1};
  double squencer_passes = x86?2:1;

  set_pppm(pppm_t, squencer_passes*num_decoder_segments, num_decoder_segments, squencer_passes*num_decoder_segments, num_decoder_segments);
  power = pre_dec->power*pppm_t;

  set_pppm(pppm_t, squencer_passes*num_decoder_segments, num_decoder_segments*num_decoded_signals,num_decoder_segments*num_decoded_signals, squencer_passes*num_decoder_segments);
  power = power + final_dec->power*pppm_t;

  double sckRation = g_tp.sckt_co_eff;

  power.readOp.dynamic *= sckRation;
  power.writeOp.dynamic *= sckRation;
  power.searchOp.dynamic *= sckRation;

  double long_channel_device_reduction = longer_channel_device_reduction(device_ty,core_ty);
  power.readOp.longer_channel_leakage = power.readOp.leakage*long_channel_device_reduction;

  double pg_reduction = power_gating_leakage_reduction(false);
  power.readOp.power_gated_leakage	= power.readOp.leakage*pg_reduction;
  power.readOp.power_gated_with_long_channel_leakage = power.readOp.power_gated_leakage * long_channel_device_reduction;


}

inst_decoder::~inst_decoder()
{
	  local_result.cleanup();

	  delete final_dec;

	  delete pre_dec->blk1;
	  delete pre_dec->blk2;
	  delete pre_dec->drv1;
	  delete pre_dec->drv2;
	  delete pre_dec;
}
