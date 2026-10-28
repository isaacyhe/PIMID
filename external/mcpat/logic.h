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
#ifndef LOGIC_H_
#define LOGIC_H_

#include "const.h"
#include "component.h"
#include "basic_components.h"
#include "basic_circuit.h"
#include "cacti_interface.h"
#include "decoder.h"
#include "parameter.h"
#include "xmlParser.h"
#include "XML_Parse.h"
#include "arch_const.h"
#include <cstring>
#include <iostream>
#include <cmath>
#include <cassert>


using namespace std;

class selection_logic : public Component{
public:
	selection_logic(bool _is_default, int    win_entries_,
		            int  issue_width_, const InputParameter *configure_interface,
		            enum Device_ty device_ty_=Core_device,
		            enum Core_type core_ty_=Inorder);//, const ParseXML *_XML_interface);
	bool is_default;
	InputParameter l_ip;
	uca_org_t local_result;
	const ParseXML *XML_interface;
	int win_entries;
	int issue_width;
	int num_threads;
	enum Device_ty device_ty;
	enum Core_type core_ty;

	void selection_power();
	void leakage_feedback(double temperature); // TODO	
};

class dep_resource_conflict_check : public Component{
public:
	dep_resource_conflict_check(const InputParameter *configure_interface, const CoreDynParam & dyn_p_, int compare_bits_, bool _is_default=true);
	InputParameter l_ip;
	uca_org_t local_result;
	double WNORn, WNORp, Wevalinvp, Wevalinvn, Wcompn, Wcompp, Wcomppreequ;
	CoreDynParam  coredynp;
	int compare_bits;
	bool is_default;
	statsDef       tdp_stats;
	statsDef       rtp_stats;
	statsDef       stats_t;
	powerDef       power_t;

	void conflict_check_power();
	double compare_cap();
	~dep_resource_conflict_check(){
		local_result.cleanup();
	}

	void leakage_feedback(double temperature);
};

class inst_decoder: public Component{
public:
	inst_decoder(bool _is_default, const InputParameter *configure_interface,
			int opcode_length_,
			int num_decoders_,
			bool x86_,
			enum Device_ty device_ty_=Core_device,
			enum Core_type core_ty_=Inorder,
			double out_fanout_=-1.0);   /* PIMID (1.12.28): > 0 = DERIVED output load (minimum-size gates) */
	inst_decoder();
	bool is_default;
	int  opcode_length;
	int  num_decoders;
	bool x86;
	int  num_decoder_segments;
	int  num_decoded_signals;
	InputParameter l_ip;
	uca_org_t local_result;
	enum Device_ty device_ty;
	enum Core_type core_ty;

	Decoder * final_dec;
	Predec *  pre_dec;

	statsDef       tdp_stats;
	statsDef       rtp_stats;
	statsDef       stats_t;
	powerDef       power_t;
	void inst_decoder_delay_power();
	~inst_decoder();
	void leakage_feedback(double temperature);
};

class DFFCell : public Component {
public:
	DFFCell(bool _is_dram, double _WdecNANDn, double _WdecNANDp,double _cell_load,
			  const InputParameter *configure_interface);
	InputParameter l_ip;
	bool is_dram;
	double cell_load;
	double WdecNANDn;
	double WdecNANDp;
	double clock_cap;
	int    model;
	int    n_switch;
	int    n_keep_1;
	int    n_keep_0;
	int    n_clock;
	powerDef e_switch;
	powerDef e_keep_1;
	powerDef e_keep_0;
	powerDef e_clock;

	double fpfp_node_cap(unsigned int fan_in, unsigned int fan_out);
	void compute_DFF_cell(void);
	bool published;   /* PIMID (1.12.31): clock/toggle energies from the published libraries (no sckt) */
	};

class Pipeline : public Component{
public:
	Pipeline(const InputParameter *configure_interface, const CoreDynParam & dyn_p_, enum Device_ty device_ty_=Core_device, bool _is_core_pipeline=true, bool _is_default=true);
	InputParameter l_ip;
	uca_org_t local_result;
	CoreDynParam  coredynp;
	enum Device_ty device_ty;
	bool is_core_pipeline, is_default;
	double num_piperegs;
//	int pipeline_stages;
//	int tot_stage_vector, per_stage_vector;
	bool process_ind;
	double WNANDn ;
	double WNANDp;
	double load_per_pipeline_stage;
	/* PIMID 1.12.18: the clock-pin energy of ONE register per cycle as this
	 * pipeline prices it (DFFCell e_clock x sckt_co_eff, see compute()).
	 * The clock network reads it so that the pin energy McPAT already
	 * charges here is not charged twice. */
	double clock_energy_per_reg;
	/* PIMID 1.12.18: McPAT's register transistor widths (the Pipeline
	 * constructor's own rule, factored out so the clock network sizes the
	 * registers of other components identically). */
	static void reg_widths(bool process_ind, double F_sz_um, double &wn, double &wp);
	/* PIMID 1.12.18: clock-pin energy per register per cycle with McPAT's
	 * register (DFFCell) at the widths above, x sckt_co_eff. Uses the g_tp
	 * of the last init_interface. */
	static double reg_clock_energy(bool embedded, double F_sz_um);
//	int  Hthread,  num_thread, fetchWidth, decodeWidth, issueWidth, commitWidth, instruction_length;
//	int  PC_width, opcode_length, num_arch_reg_tag, data_width,num_phsical_reg_tag, address_width;
//	bool thread_clock_gated;
//	bool in_order, multithreaded;
	void compute_stage_vector();
	void compute();
	~Pipeline(){
		local_result.cleanup();
	};

};

/* PIMID 1.12.18: CLOCK DISTRIBUTION NETWORK.
 *
 * McPAT 1.x ships its clock-network calls commented out (core.cc,
 * memoryctrl.cc, noc.cc, sharedcache.cc, processor.cc) and the class they
 * name (clock_network) does not exist, so no component carried clock-tree
 * power. This class supplies it following the clock model of
 *   D. Brooks, V. Tiwari, M. Martonosi, "Wattch: A Framework for
 *   Architectural-Level Power Analysis and Optimizations", ISCA 2000
 *   (Sec. 3, clocking: global clock wiring, clock buffers and clocked node
 *   load, P = C Vdd^2 f with activity 1 for an ungated clock).
 *
 * Per instance and per clock cycle:
 *   E_cycle = E_tree + E_pins
 *   E_tree  = CACTI repeated-wire energy of an H-tree of total length L
 *             (wire capacitance + repeater input/output capacitance + the
 *             short-circuit term, cacti/wire.cc delay_optimal_wire), x the
 *             sckt_co_eff McPAT applies to every wire (interconnect.cc).
 *             The repeaters are the clock buffers; their leakage is the
 *             same wire model's leakage.
 *   L       = 1.5 D (sqrt(N) - 1): an H-tree over a square of side
 *             D = sqrt(component area) whose 4^n leaves reach each of the
 *             N clocked sinks (H-tree clock distribution: H. B. Bakoglu,
 *             "Circuits, Interconnections, and Packaging for VLSI",
 *             Addison-Wesley 1990, ch. 8). Derivation: H level k
 *             (k = 1..n) is 4^(k-1) H's in squares of side D/2^(k-1), each
 *             1.5 x that side long, i.e. 1.5 D 2^(k-1); summed,
 *             L = 1.5 D (2^n - 1) with 2^n = sqrt(N). Wattch's own
 *             clocklinelength (10.5 x die length) is this sum at n = 3.
 *   E_pins  = N x DFFCell clock-pin energy (McPAT's register model, itself
 *             Wattch's fpfp clock capacitance), activity 1 (ungated).
 * N, the clocked sinks, comes only from structure counts McPAT/CACTI
 * already make (pipeline registers, array port registers, arbiter
 * flip-flops, link pipeline registers); the caller names the source.
 *
 * GATING (user ruling 2026-10-23: gate by measured activity). Runtime
 * energy follows Wattch's conditional-clocking style cc2 (Brooks, Tiwari,
 * Martonosi, ISCA 2000, and the Wattch 1.02 simulator's cc2 mode: a unit's
 * clocked power scales linearly with its measured port usage, zero when
 * unused; cc3's fixed idle fraction is a preset and is not used): the
 * per-cycle energy is charged for activity x cycles, the activity being
 * the component's MEASURED utilisation that PIMID already emits (core:
 * busy fraction = pipeline_duty_cycle; cache, NoC: the measured
 * duty_cycle stat; memory controller: measured requests per controller
 * cycle per channel). The ungated value is kept beside it as a reference
 * line. A component with no measured activity is reported and priced
 * ungated (no duty is invented). Peak power stays ungated (all active).
 * Repeater leakage is not gated: clock gating stops switching, not
 * leakage.
 *
 * PIMID 1.12.18: GLOBAL / LOCAL SPLIT (user ruling 2026-10-23 #4). Within a
 * component the GLOBAL part of the tree -- the H-tree levels that carry the
 * clock down to the unit boundaries -- switches EVERY cycle while the
 * component is powered; only the LOCAL part -- the last level to the
 * registers and the register clock pins themselves -- is gated by the
 * measured activity. Sources: Wattch (Brooks, Tiwari, Martonosi, ISCA 2000):
 * Sec. 2.2.1, conditional clocking "disable[s] all or part of a hardware
 * unit" (the unit's clock, not the global network); Sec. 3.2 and Tables
 * 4/5: Alpha 21264 total clock 34.4% "includes all clock capacitance
 * including the clock nodes within individual units", Pentium Pro global
 * clock 7.9% ("only counts clock power as the global clock network"; Intel
 * figure from L. Gwennap, Microprocessor Report, Feb. 1995). Primary
 * Alpha source: M. Gowan, L. Biro, D. Jackson, "Power considerations in the
 * design of the Alpha 21264 microprocessor", DAC 1998, Table 2 (clock 32%,
 * "hierarchical clocking scheme with conditional clocks"). Most clock
 * power sits below the global distribution, where gating acts.
 * RULE (from the levels the tree above already builds): level k of n spans
 * 1.5 D 2^(k-1); the GLOBAL part is every level but the last,
 *   L_global = 1.5 D (2^(n-1) - 1) = 1.5 D (sqrt(N)/2 - 1)  (0 when N <= 4,
 *              a one-level tree is all local),
 *   L_local  = L - L_global = 1.5 D sqrt(N)/2 (the last level),
 * and the CACTI repeated-wire energy is per unit length times length
 * (cacti/wire.cc, power.readOp.dynamic = global.power x wire_length), so
 * E_global = E_tree x L_global / L exactly. Runtime per cycle:
 *   E = E_global x powered + (E_local + E_pins) x activity,
 * powered = 1 - (measured power-gated residency) when the described design
 * power-gates the component (XML sys pimid_clock_pg_*; off by default), 1
 * otherwise: power gating is the only thing that stops the global part.
 * The ungated reference (every part every cycle) is kept.
 *
 * PIMID 1.12.18: IDLE HALT (user ruling 2026-10-23, XML sys pimid_idle_halt,
 * YAML power.models.idle_halt). A core with nothing to run is HALTED as on
 * real chips (x86 HLT / C1: Intel SDM vol. 3, "HLT ... stops instruction
 * execution and places the processor in a HALT state", with the core clock
 * stopped in C1; ARM WFI: Arm ARM, "Wait For Interrupt" suspends execution
 * and the core clock may be gated): its whole tree, global part included,
 * is off while halted. Leakage stays (power gating, off by default, is the
 * only thing that removes it). The running fraction is MEASURED by the
 * timing model (set_running): the global part is then charged
 *   E_global x min(powered, running),
 * the local part stays gated by the measured activity. Only cores halt:
 * memory controllers, routers and caches keep serving (refresh, traffic,
 * snoops) and keep global-on / local-gated.
 *
 * PIMID 1.12.18: PER-DIE TRUNK (user ruling "each die has its own clock
 * tree"). The die-level trunk that joins the components' tree roots is
 * built in processor.cc (Processor, per die and clock domain) with this same
 * class: an H-tree over the die whose leaves are the component trees' roots
 * (n_sinks = component instances, no register pins: the trunk ENDS at each
 * component's clock entry, whose first repeater is already inside that
 * component's tree), so the two never overlap. */
class ClockNetwork : public Component {
public:
	ClockNetwork();
	/* Build the tree of ONE instance. e_pin_per_reg < 0 means: compute it
	 * here with Pipeline::reg_clock_energy at this interface's technology. */
	void compute(const InputParameter *configure_interface, const string &owner_,
	             double tree_area_um2_, double n_sinks_, const string &sink_source_,
	             double e_pin_per_reg_, double clock_hz_, bool embedded_,
	             enum Device_ty device_ty_, enum Core_type core_ty_, bool enabled_);
	/* Set this network's own power/rt_power. tdp_dyn: J per (owner) cycle,
	 * ungated; rt_gated / rt_ungated: J over the run with and without the
	 * measured-activity gating; n_leak_instances: instances whose repeater
	 * leakage the owner's leakage convention carries. */
	void set_power(double tdp_dyn, double rt_gated, double rt_ungated_, double n_leak_instances);
	/* The measured activity (fraction of cycles, cc2) and where it is read;
	 * a < 0 means the run carried none. Returns the activity to apply. */
	double gate(double a, const string &src);
	/* PIMID 1.12.18 (ruling #4): the measured power-gated residency of the
	 * component (0 = always powered) -- the only thing that stops the
	 * global part. */
	void set_powered(double r_gated, const string &src);
	/* PIMID 1.12.18 (ruling #4): J per cycle per instance charged at
	 * runtime: global x global_on() + (local + pins_e) x a. */
	double gated_cycle(double a, double pins_e) const;
	/* PIMID 1.12.18 (idle halt): halt_on = power.models.idle_halt; r_mean /
	 * r_max = the MEASURED running fraction of the cores this instance stands
	 * for (mean, max; < 0 = not measured: the global part stays every powered
	 * cycle and the run says so). */
	void set_running(bool halt_on, double r_mean, double r_max, const string &src);
	/* fraction of cycles the GLOBAL part switches: powered, or, with idle halt,
	 * min(powered, running). trunk_on(): the same with the MAX running
	 * fraction (the die trunk runs while any core on the die runs). */
	double global_on() const;
	double trunk_on() const;
	bool halt;                                        /* PIMID 1.12.18 (idle halt) */
	double running, running_max;                      /* measured running fraction (mean, max) */
	string running_source;
	double rt_halted;                                 /* J over the run: the global part NOT charged (halted) */
	void report_runtime(const string &what, double seconds) const;
	double activity, rt_ungated;
	double global_len_um, local_len_um;              /* PIMID 1.12.18 (ruling #4) */
	double e_tree_global_cycle, e_tree_local_cycle;  /* J per cycle per instance */
	double powered;                                  /* fraction of cycles powered */
	string powered_source;
	double rt_global;                                /* J over the run: the global part charged */
	/* PIMID 1.12.18 (ruling #4): XML sys pimid_clock_split (YAML
	 * power.models.clock_global_local); false = the whole tree gated by the
	 * measured activity (the pre-ruling model). Set once per McPAT run
	 * (Processor constructor), before any component is built. */
	static bool split_enabled;
	static double leaf_cluster_sinks;   /* PIMID (1.12.30, F2): sinks per local clock buffer (power.clock.leaf_cluster_sinks) */
	string activity_source;
	bool built, enabled;   /* enabled: XML sys.pimid_clock_network (YAML power.models.clock_network) */
	string owner, sink_source;
	double tree_area_um2, side_um, n_sinks, h_levels, tree_len_um;
	double c_wire_per_um;      /* F/um: CACTI wire_cap of this wire type */
	double c_eff_per_um;       /* F/um: E_tree / (L Vdd^2): wire + repeaters + short circuit, x sckt */
	double repeater_spacing_um, repeater_size;
	double e_tree_cycle;       /* J per cycle per instance */
	double e_pin_reg;          /* J per register per cycle (x sckt) */
	double c_pin;              /* F: DFFCell clock_cap */
	double n_clusters, a_reg_um2, cluster_side_um, c_local_per_um, wn, wp;   /* PIMID (1.12.30, F2) */
	bool   span_is_clocked_logic;   /* PIMID (1.12.30): set by the owner when the tree area is clocked logic (a core) */
	double cluster_area_um2, c_lcb_in, c_lcb_out, lcb_wn, lcb_wp;   /* PIMID (1.12.30): local nets, local clock buffers */
	string c_local_source;     /* PIMID (1.12.67, CLK1): where c_local_per_um comes from */
	double vdd, clock_hz, sckt;
	double leak, gate_leak;    /* W per instance (repeaters) */
	double lc_reduction, pg_reduction;
};

//class core_pipeline :public pipeline{
//public:
//	int  Hthread,  num_thread, fetchWidth, decodeWidth, issueWidth, commitWidth, instruction_length;
//	int  PC_width, opcode_length, num_arch_reg_tag, data_width,num_phsical_reg_tag, address_width;
//	bool thread_clock_gated;
//	bool in_order, multithreaded;
//	core_pipeline(bool _is_default, const InputParameter *configure_interface);
//	virtual void compute_stage_vector();
//
//};

/* PIMID 1.12.18 (rulings 2026-10-23 #1-#3, #6): node scaling of a published
 * energy with CACTI's technology tables. Returns
 *   k = [C_inv(run) / C_inv(src_nm)] x (Vdd(run) / src_vdd)^2,
 * C_inv = switched capacitance of CACTI's minimum inverter (gate of the NMOS
 * + PMOS pair at McPAT's P/N ratio + both drain junctions), both nodes from
 * the same CACTI device type (the run's), Vdd(run) = the run's CACTI
 * peripheral/global Vdd, src_vdd = the source's PRINTED supply. The run's
 * g_tp must be current (the caller's init_interface); it is restored.
 * Optional outputs: C_inv at both nodes (F), the run's Vdd, CACTI's own Vdd
 * at the source node. */
/* PIMID (1.12.28): Stillmaker & Baas, Integration 58 (2017) 74-81, Eq. (6) energy factor
 * ae2 V^2 + ae1 V + ae0 with the Table 5 coefficients of node nm (45/32 nm: HP and LP rows;
 * 20 nm: HP and LSTP rows). Returns the number of rows written to f[] (0: no Table 5 row). */
int pimid_sb17_energy_factors(int nm, double v, double f[2], const char *type[2]);
/* PIMID (1.12.31, user ruling 2026-10-25 A): the published flip-flop (Nangate 45 nm DFF_X1, ASAP7 DFFHQNx1), node-scaled
 * with Stillmaker & Baas 2017 (logic.cc). Energies in J, capacitances in F. */
struct PimidDffPub {
	const char *lib; double lib_nm; int row_nm; double ratio, ratio_lo, ratio_hi;
	double c_ck, c_d, e_clock, e_switch, c_ck_lo, c_ck_hi, e_clock_lo, e_clock_hi;
	/* PIMID (DST, user ruling 2026-10-26): the ratio APPLIED is DeepScaleTool's when it covers the node (dst true);
	 * the Stillmaker & Baas pair midpoint and the voltage-less DeepScaleTool ratio are kept for printing */
	bool dst; double ratio_sb17, ratio_dst_vl;
};
bool pimid_dff_published(double nm, double v, PimidDffPub &o);
double pimid_node_energy_scale(double src_nm, double src_vdd, double *c_src = 0, double *c_run = 0,
                               double *v_run = 0, double *v_src_cacti = 0);
/* PIMID (1.12.33, Z12): the published-silicon (DeepScaleTool) node ratio for published per-op logic energies (logic.cc). */
double pimid_logic_op_scale(double src_nm, double src_vdd, bool *silicon = 0);

class FunctionalUnit :public Component{
public:
	ParseXML *XML;
	int  ithCore;
	InputParameter interface_ip;
	CoreDynParam  coredynp;
	double FU_height;
	double clockRate,executionTime;
	double num_fu;
	double energy, base_energy,per_access_energy, leakage, gate_leakage;
	/* PIMID 1.12.18 (ruling 2026-10-23 #2): published per-operation FP
	 * energies at the run's node (J), FP32 add and FP32 multiply; set only
	 * for an FPU with pimid_fpu_published (logic.cc). */
	bool   pimid_pub = false;
	/* PIMID 1.12.18 (ruling #2, extended): AddS MulS AddD MulD FmaS FmaD,
	 * the census order (XML_Parse.h pimid_fpc_name). */
	double pimid_pub_e[6] = {0, 0, 0, 0, 0, 0};
	/* PIMID 1.12.18 (ruling 2026-10-23 #3): published per-operation integer
	 * energies at the run's node (J): ALU unit {8-bit add, 32-bit add}, MUL
	 * unit {8-bit mult, 32-bit mult}; set with pimid_int_alu_published. */
	bool   pimid_int_pub = false;
	double pimid_int_e[2] = {0, 0};
	double pimid_int_e16 = 0, pimid_int_e64 = 0;   /* PIMID (1.12.32, ruling #1): ALU add/logic linear in width */
	/* PIMID 1.12.29: (ruling 2026-10-25) the published area of ONE unit at
	 * the run's node (um^2, before McPAT's macro layout overhead) that
	 * replaced McPAT's literal; 0 = McPAT's literal (switch off, or no
	 * published source for this unit and width). Reused by leakage_feedback. */
	double pimid_area_pub = 0;
	bool  is_default;
	enum FU_type fu_type;
	statsDef       tdp_stats;
	statsDef       rtp_stats;
	statsDef       stats_t;
	powerDef       power_t;

	FunctionalUnit(ParseXML *XML_interface, int ithCore_, InputParameter* interface_ip_,const CoreDynParam & dyn_p_, enum FU_type fu_type);
    void computeEnergy(bool is_tdp=true);
	void displayEnergy(uint32_t indent = 0,int plevel = 100, bool is_tdp=true);
    void leakage_feedback(double temperature);

};

class UndiffCore :public Component{
public:
	UndiffCore(ParseXML* XML_interface, int ithCore_, InputParameter* interface_ip_, const CoreDynParam & dyn_p_, bool exist_=true, bool embedded_=false);
	ParseXML *XML;
	int  ithCore;
	InputParameter interface_ip;
	CoreDynParam  coredynp;
	double clockRate,executionTime;
	double scktRatio, chip_PR_overhead, macro_PR_overhead;
	enum  Core_type core_ty;
	bool   opt_performance, embedded;
	double pipeline_stage,num_hthreads,issue_width;
	bool   is_default;

    void displayEnergy(uint32_t indent = 0,int plevel = 100, bool is_tdp=true);
	~UndiffCore(){};
	bool exist;


};
#endif /* LOGIC_H_ */
