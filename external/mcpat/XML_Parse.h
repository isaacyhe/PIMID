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

#ifndef XML_PARSE_H_
#define XML_PARSE_H_


//#ifdef WIN32
//#define _CRT_SECURE_NO_DEPRECATE
//#endif

#include <stdio.h>
#include "xmlParser.h"
#include <string.h>
#include <iostream>
using namespace std;

/*
void myfree(char *t); // {free(t);}
ToXMLStringTool tx,tx2;
*/
//all subnodes at the level of system.core(0-n)
//cache_policy is added into cache property arrays;//0 no write or write-though with non-write allocate;1 write-back with write-allocate

typedef struct{
	int prediction_width;
	char prediction_scheme[20];
	int predictor_size;
	int predictor_entries;
	int local_predictor_size[20];
	int local_predictor_entries;
	/* PIMID 1.11.93 (F6): entry count of the SECOND local level (the pattern
	 * table). Upstream sizes both local levels with local_predictor_entries,
	 * which describes an Alpha-21264 local predictor (1024 histories, 1024
	 * counters). zsim's PAg has 2048 histories and 16384 counters, so the
	 * second level needs its own count. 0 = upstream behaviour. */
	int local_predictor_l2_entries;
	int global_predictor_entries;
	int global_predictor_bits;
	int chooser_predictor_entries;
	int chooser_predictor_bits;
	double predictor_accesses;
} predictor_systemcore;
typedef struct{
	int number_entries;
	/* PIMID 1.12.18: the TLB's associativity. 0 = upstream (a fully
	 * associative CAM searched on every access); N > 0 = an N-way
	 * set-associative array read on every access (core.cc MemManU). */
	int pimid_assoc;
	int cache_policy;//0 no write or write-though with non-write allocate;1 write-back with write-allocate
	double total_hits;
	double total_accesses;
	double total_misses;
	double conflicts;
} itlb_systemcore;
typedef struct{
	//params
	double icache_config[20];
	int buffer_sizes[20];
	int cache_policy;//0 no write or write-though with non-write allocate;1 write-back with write-allocate
	//stats
	double total_accesses;
	double read_accesses;
	double read_misses;
	double replacements;
	double read_hits;
	double total_hits;
	double total_misses;
	double miss_buffer_access;
	double fill_buffer_accesses;
	double prefetch_buffer_accesses;
	double prefetch_buffer_writes;
	double prefetch_buffer_reads;
	double prefetch_buffer_hits;
	double conflicts;
} icache_systemcore;
typedef struct{
	//params
	int number_entries;
	int pimid_assoc;   /* PIMID 1.12.18: as itlb_systemcore */
	int pimid_stlb_entries;   /* PIMID (1.12.129): entries of the second-level TLB behind this DTLB (0 = none); same tag/data width and pimid_assoc */
	int cache_policy;//0 no write or write-though with non-write allocate;1 write-back with write-allocate
	//stats
	double total_accesses;
	double read_accesses;
	double write_accesses;
	double write_hits;
	double read_hits;
	double read_misses;
	double write_misses;
	double total_hits;
	double total_misses;
	double conflicts;
} dtlb_systemcore;
typedef struct{
	//params
	double dcache_config[20];
	int buffer_sizes[20];
	int cache_policy;//0 no write or write-though with non-write allocate;1 write-back with write-allocate
	//stats
	double total_accesses;
	double read_accesses;
	double write_accesses;
	double total_hits;
	double total_misses;
	double read_hits;
	double write_hits;
	double read_misses;
	double write_misses;
	double replacements;
	double write_backs;
	double miss_buffer_access;
	double fill_buffer_accesses;
	double prefetch_buffer_accesses;
	double prefetch_buffer_writes;
	double prefetch_buffer_reads;
	double prefetch_buffer_hits;
	double wbb_writes;
	double wbb_reads;
	double conflicts;
} dcache_systemcore;
typedef struct{
	//params
	int BTB_config[20];
	//stats
	double total_accesses;
	double read_accesses;
	double write_accesses;
	double total_hits;
	double total_misses;
	double read_hits;
	double write_hits;
	double read_misses;
	double write_misses;
	double replacements;
} BTB_systemcore;
typedef struct{
	//all params at the level of system.core(0-n)
	int clock_rate;
	bool opt_local;
	bool x86;
	int machine_bits;
	int virtual_address_width;
	int physical_address_width;
	int opcode_width;
	int micro_opcode_width;
	int instruction_length;
	int machine_type;
	int internal_datapath_width;
	int number_hardware_threads;
	int fetch_width;
	int number_instruction_fetch_ports;
	int pimid_dma_engine;   /* PIMID 1.12.18: line bytes of the scratchpad DMA engine's buffer (0 = no engine) */
	int pimid_ib_entries;   /* PIMID 1.12.18 (ruling 2026-10-23 #3): instructions the TIMING core's front-end buffer holds (0 = not given: McPAT's instruction_buffer_size) */
	/* PIMID 1.12.18 (user ruling 2026-10-23, idle halt): the MEASURED fraction
	 * of the run a thread was runnable on the core (device PE: its busy
	 * cycles; host core: zsim's unhalted cycles), mean and max over the cores
	 * this template stands for. < 0 = not measured. */
	double pimid_running_fraction, pimid_running_fraction_max;
	int decode_width;
	int issue_width;
	int peak_issue_width;
	int commit_width;
	int pipelines_per_core[20];
	int pipeline_depth[20];
	char FPU[20];
	char divider_multiplier[20];
	int ALU_per_core;
	double FPU_per_core;
	int MUL_per_core;
	int instruction_buffer_size;
	int decoded_stream_buffer_size;
	int instruction_window_scheme;
	int instruction_window_size;
	int fp_instruction_window_size;
	int ROB_size;
	int archi_Regs_IRF_size;
	int archi_Regs_FRF_size;
	int phy_Regs_IRF_size;
	int phy_Regs_FRF_size;
	int rename_scheme;
	int checkpoint_depth;
	int register_windows_size;
	char LSU_order[20];
	int store_buffer_size;
	int load_buffer_size;
	int memory_ports;
	char Dcache_dual_pump[20];
	int RAS_size;
	int fp_issue_width;
	int prediction_width;
	int number_of_BTB;
	int number_of_BPT;

	//all stats at the level of system.core(0-n)
	double total_instructions;
	double int_instructions;
	double fp_instructions;
	double branch_instructions;
	double branch_mispredictions;
	/* PIMID 1.11.97 (R2476): MEASURED predictor table writes (value-changing
	 * updates counted by the timing core): level-1 local history table and
	 * level-2 local counter table. -1 = absent; the runtime path then keeps
	 * the upstream estimate (mispredictions + 10% of branches). */
	double predictor_l1_writes;
	double predictor_l2_writes;
	double committed_instructions;
	double committed_int_instructions;
	double committed_fp_instructions;
	double load_instructions;
	double pimid_dma_lines;  /* PIMID 1.12.18: lines the DMA engine moved */
	double pimid_dma_cmd_reads, pimid_dma_cmd_writes;   /* PIMID 1.12.18: its command-register accesses */
	/* PIMID 1.12.18 (ruling 2026-10-23 #2): measured FP operations by type and
	 * precision (PIMID's decoder census, all cores), instructions and scalar
	 * lane operations; categories pimid_fpc_name[] below. */
	double pimid_fp_ins[9], pimid_fp_lanes[9];
	/* PIMID 1.12.18 (ruling 2026-10-23 #3): measured scalar GPR integer
	 * operations by class and operand width (PIMID's x86 decoder census, all
	 * cores); categories pimid_ic_name[] below. */
	double pimid_int_ops[9];
	double store_instructions;
	double total_cycles;
	double idle_cycles;
	double busy_cycles;
	double instruction_buffer_reads;
	double instruction_buffer_write;
	double ROB_reads;
	double ROB_writes;
	double rename_accesses;
	double fp_rename_accesses;
	double rename_reads;
	double rename_writes;
	double fp_rename_reads;
	double fp_rename_writes;
	double inst_window_reads;
	double inst_window_writes;
	double inst_window_wakeup_accesses;
	double inst_window_selections;
	double fp_inst_window_reads;
	double fp_inst_window_writes;
	double fp_inst_window_wakeup_accesses;
	double fp_inst_window_selections;
	double archi_int_regfile_reads;
	double archi_float_regfile_reads;
	double phy_int_regfile_reads;
	double phy_float_regfile_reads;
	double phy_int_regfile_writes;
	double phy_float_regfile_writes;
	double archi_int_regfile_writes;
	double archi_float_regfile_writes;
	double int_regfile_reads;
	double float_regfile_reads;
	double int_regfile_writes;
	double float_regfile_writes;
	double windowed_reg_accesses;
	double windowed_reg_transports;
	double function_calls;
	double context_switches;
	double ialu_accesses;
	double fpu_accesses;
	double mul_accesses;
	double cdb_alu_accesses;
	double cdb_mul_accesses;
	double cdb_fpu_accesses;
	double load_buffer_reads;
	double load_buffer_writes;
	double load_buffer_cams;
	double store_buffer_reads;
	double store_buffer_writes;
	double store_buffer_cams;
	double store_buffer_forwards;
	double main_memory_access;
	double main_memory_read;
	double main_memory_write;
	double pipeline_duty_cycle;

	double IFU_duty_cycle ;
	double BR_duty_cycle ;
	double LSU_duty_cycle ;
	double MemManU_I_duty_cycle;
	double MemManU_D_duty_cycle ;
	double ALU_duty_cycle ;
	double MUL_duty_cycle ;
	double FPU_duty_cycle ;
	double ALU_cdb_duty_cycle ;
	double MUL_cdb_duty_cycle ;
	double FPU_cdb_duty_cycle ;

	double vdd;
	double power_gating_vcc;

	//all subnodes at the level of system.core(0-n)
	predictor_systemcore predictor;
	itlb_systemcore itlb;
	icache_systemcore icache;
	dtlb_systemcore dtlb;
	dcache_systemcore dcache;
	BTB_systemcore BTB;

} system_core;
typedef struct{
	//params
	int Directory_type;
	double Dir_config[20];
	int buffer_sizes[20];
	int clockrate;
	int ports[20];
	int device_type;
	int cache_policy;//0 no write or write-though with non-write allocate;1 write-back with write-allocate
	char threeD_stack[20];
	double vdd;
	double power_gating_vcc;
	//stats
	double total_accesses;
	double read_accesses;
	double write_accesses;
	double read_misses;
	double write_misses;
	double conflicts;
	double duty_cycle;
} system_L1Directory;
typedef struct{
	//params
	int Directory_type;
	double Dir_config[20];
	int buffer_sizes[20];
	int clockrate;
	int ports[20];
	int device_type;
	int cache_policy;//0 no write or write-though with non-write allocate;1 write-back with write-allocate
	char threeD_stack[20];
	double vdd;
	double power_gating_vcc;
	//stats
	double total_accesses;
	double read_accesses;
	double write_accesses;
	double read_misses;
	double write_misses;
	double conflicts;
	double duty_cycle;
} system_L2Directory;
typedef struct{
	//params
	double L2_config[20];
	int clockrate;
	int ports[20];
	int device_type;
	int cache_policy;//0 no write or write-though with non-write allocate;1 write-back with write-allocate
	char threeD_stack[20];
	int buffer_sizes[20];
	double vdd;
	double power_gating_vcc;
	//stats
	double total_accesses;
	double read_accesses;
	double write_accesses;
	double total_hits;
	double total_misses;
	double read_hits;
	double write_hits;
	double read_misses;
	double write_misses;
	double replacements;
	double write_backs;
	double miss_buffer_accesses;
	double fill_buffer_accesses;
	double prefetch_buffer_accesses;
	double prefetch_buffer_writes;
	double prefetch_buffer_reads;
	double prefetch_buffer_hits;
	double wbb_writes;
	double wbb_reads;
	double conflicts;
	double duty_cycle;

	bool   merged_dir;
	/* PIMID 1.12.18 (user ruling "sharer bits from the real child count"):
	 * > 0 = the children this cache's SBT directory tracks per line (the
	 * caches below it); 0 (default) = upstream, number_of_cores. */
	int    dir_sharers;
	double homenode_read_accesses;
	double homenode_write_accesses;
	double homenode_read_hits;
	double homenode_write_hits;
	double homenode_read_misses;
	double homenode_write_misses;
	double dir_duty_cycle;
} system_L2;
typedef struct{
	//params
	double L3_config[20];
	int clockrate;
	int ports[20];
	int device_type;
	int cache_policy;//0 no write or write-though with non-write allocate;1 write-back with write-allocate
	char threeD_stack[20];
	int buffer_sizes[20];
	double vdd;
	double power_gating_vcc;
	//stats
	double total_accesses;
	double read_accesses;
	double write_accesses;
	double total_hits;
	double total_misses;
	double read_hits;
	double write_hits;
	double read_misses;
	double write_misses;
	double replacements;
	double write_backs;
	double miss_buffer_accesses;
	double fill_buffer_accesses;
	double prefetch_buffer_accesses;
	double prefetch_buffer_writes;
	double prefetch_buffer_reads;
	double prefetch_buffer_hits;
	double wbb_writes;
	double wbb_reads;
	double conflicts;
	double duty_cycle;

	bool   merged_dir;
	/* PIMID 1.12.18 (user ruling "sharer bits from the real child count"):
	 * > 0 = the children this cache's SBT directory tracks per line (the
	 * caches below it); 0 (default) = upstream, number_of_cores. */
	int    dir_sharers;
	double homenode_read_accesses;
	double homenode_write_accesses;
	double homenode_read_hits;
	double homenode_write_hits;
	double homenode_read_misses;
	double homenode_write_misses;
	double dir_duty_cycle;
} system_L3;
typedef struct{
	//params
	int number_of_inputs_of_crossbars;
	int number_of_outputs_of_crossbars;
	int flit_bits;
	int input_buffer_entries_per_port;
	int ports_of_input_buffer[20];
	//stats
	double crossbar_accesses;
} xbar0_systemNoC;
typedef struct{
	//params
	int clockrate;
	bool type;
	bool has_global_link;
	char topology[20];
	int horizontal_nodes;
	int vertical_nodes;
	int link_throughput;
	int link_latency;
	int input_ports;
	int output_ports;
	int virtual_channel_per_port;
	int flit_bits;
	int input_buffer_entries_per_vc;
	int ports_of_input_buffer[20];
	int dual_pump;
	int number_of_crossbars;
	char crossbar_type[20];
	char crosspoint_type[20];
	xbar0_systemNoC xbar0;
	int arbiter_type;
	double chip_coverage;
	double vdd;
	double power_gating_vcc;
	/* PIMID 1.11.50 (L74): whether THIS fabric level sits on the DRAM die.
	 * The dram_periph_family transform used to apply to every NoC instance
	 * in the run, but a multi-level build spans the physical hierarchy --
	 * subarray..chip fabrics live on the die while rank/channel/system
	 * fabrics live on a buffer die or the host board, i.e. logic silicon
	 * per the same placement matrix that decides the family itself.
	 * Default 1 keeps unannotated XMLs (single on-die fabric) unchanged. */
	int on_dram_die;
	/* PIMID 1.12.18: LINK pricing inputs. Upstream McPAT derives a link's
	 * length from the die area, sqrt(area * chip_coverage) / ((h + v) / 2)
	 * / 2, i.e. a mesh pitch, and charges the link runtime dynamic with the
	 * ROUTER access count. PIMID's fabric is a tree whose link count,
	 * length and traffic it knows, so it can say them:
	 *   routers_present  1 (default) = upstream; 0 = a LINKS-ONLY instance
	 *                    (no router is built: a pass-through level, or the
	 *                    up-links of a router level priced on their own).
	 *   link_kind        0 (default) = planar wire (McPAT interconnect,
	 *                    CACTI wire model); 1 = TSV (CACTI-3DD TSV, Coarse
	 *                    = "industry reported" column, cacti_interface.h).
	 *   link_span_area_mm2, link_parent_regions
	 *                    the STRUCTURE a tree level's links span: the die
	 *                    area the level lies on and the number of regions
	 *                    (parent nodes) it is cut into. When both are > 0
	 *                    McPAT derives the length of ONE link from them
	 *                    (H-tree geometry, processor.cc) and uses it as is
	 *                    (no mesh division, no die-growth stretch); else the
	 *                    upstream derivation.
	 *   link_traversals  (stat) >= 0: flit traversals of this instance's
	 *                    wire links, the link runtime dynamic's activity;
	 *                    < 0 (default) = upstream (total_accesses).
	 *   tsv_die_crossings (stat, link_kind 1) >= 0: flit x DIE crossings,
	 *                    i.e. each transfer's flits times the number of dies
	 *                    it actually crossed in the stack; the TSV runtime
	 *                    dynamic is the per-flit TSV energy times this. */
	int routers_present;
	int link_kind;
	double link_span_area_mm2;
	int link_parent_regions;
	/* 1.12.23: 1 = the links span the PROCESSOR'S OWN area (known only once
	 * McPAT has built the chip): len = 0.5 x sqrt(area x chip_coverage /
	 * link_parent_regions), the same H-tree rule as link_span_area_mm2, used
	 * as is. A host's uncore crossbar: one port wire from each agent to the
	 * crossbar at the die centre. 0 (default) = upstream / explicit span.
	 * 2 = a ring loop, 2 x sqrt(area) / stops. (1.12.150) 3 = a flat 2-D mesh's
	 * tile pitch, sqrt(link_span_area_mm2 / link_parent_regions), the regions
	 * being the tiles (the processor's area x chip_coverage when no span). */
	int link_span_processor;
	/* PIMID (user ruling 2026-10-25): link_kind 2 = an off-package link
	 * riding the DRAM channel, priced per bit crossed at this energy (pJ/bit,
	 * given by PIMID from the part's DQ crossing); < 0 = not set. */
	double link_io_pj_per_bit;
	/* the supply (V) a TSV link switches at (the stacked part's core VDD);
	 * <= 0 = the run's logic Vdd. */
	double tsv_vdd;
	/* PIMID: measured 0->1 transitions per wire-bit per traversal of this
	 * instance's wires (link, bus or TSV); < 0 = not given (McPAT's full C V^2 per bit). */
	double link_activity;
	/* PIMID 1.12.18: > 0 = the process node (nm) THIS instance is priced
	 * at; 0 (default) = the processor's core_tech_node. A tree level's
	 * links lie on the DRAM die or on logic, and each takes its own
	 * process's CACTI tables. */
	int tech_node;
	//stats
	double total_accesses;
	double link_traversals;
	double tsv_die_crossings;
	/* PIMID 1.12.18 (user ruling "pass-through routers as wire"): (stat)
	 * > 0 = the part of total_accesses that went through routers with one
	 * router child and no endpoint (a single in/out path). They are wire:
	 * their flits are priced by the level's links-only instances, so this
	 * instance's routers are charged total_accesses - passthrough_accesses
	 * and its duty_cycle is scaled by the same ratio. <= 0 (default -1) =
	 * upstream. */
	double passthrough_accesses;
	double duty_cycle;
	double route_over_perc;
} system_NoC;
typedef struct{
	//params
	int mem_tech_node;
	int device_clock;
	int peak_transfer_rate;
	int internal_prefetch_of_DRAM_chip;
	int capacity_per_channel;
	int number_ranks;
	int num_banks_of_DRAM_chip;
	int Block_width_of_DRAM_chip;
	int output_width_of_DRAM_chip;
	int page_size_of_DRAM_chip;
	int burstlength_of_DRAM_chip;
	//stats
	double memory_accesses;
	double memory_reads;
	double memory_writes;
} system_mem;
typedef struct{
	//params
    //Common Param for mc and fc
	double peak_transfer_rate;
	int number_mcs;
	bool withPHY;
	double pimid_command_ops;   /* PIMID 1.12.18: command-only operations the controller issued (counted refresh commands) */
	double background_fraction;   /* PIMID 1.11.98 (row 28): the routine-job share of peak dynamic power per cycle; McPAT's 0.1 unless the run measures it */
	int type;

	//FCParam
	//stats
	double duty_cycle;
	double total_load_perc;

	//McParam
	int mc_clock;
    int llc_line_length;
	int memory_channels_per_mc;
	int number_ranks;
	int req_window_size_per_channel;
	int IO_buffer_size_per_channel;
	int databus_width;
	int addressbus_width;
	bool LVDS;
	int phy_class;   /* PIMID 1.11.19 (D2/D3): 0=off-package DDR, 1=interposer/TSV */
	double phy_pj_bit_rd, phy_pj_bit_wr;   /* PIMID (1.12.132): a published PHY energy per bit; > 0 replaces the PHY dynamic */
	double vdd;
	double power_gating_vcc;

	//stats
	double memory_accesses;
	double memory_reads;
	double memory_writes;
} system_mc;

typedef struct{
	//params
    int clockrate;
	int number_units;
	int type;
	double vdd;
	double power_gating_vcc;
	//stats
	double duty_cycle;
	double total_load_perc;
} system_niu;

typedef struct{
	//params
    int clockrate;
	int number_units;
	int num_channels;
	int type;
	bool withPHY;
	double serdes_lane_gbps;   /* PIMID 1.11.29: per-lane SerDes rate, Gb/s */
	double vdd;
	double power_gating_vcc;
	//stats
	double duty_cycle;
	double total_load_perc;
	/* PIMID 1.11.7 (#85): activity-driven link energy. transferred_bytes is
	 * the MEASURED crossing byte count; link_pj_per_bit the per-link-type
	 * transfer energy (selected by PIMID from its cited table: PCIe gen3/4/5,
	 * CXL = gen5 PHY + coherence delta, NVLink, or user override). When
	 * transferred_bytes > 0 the SerDes dynamic is bytes*8*pJ/bit -- real
	 * activity, zero traffic = zero energy -- replacing the PCIe 2.0-era
	 * constant-load scaling. Both default 0 = legacy behavior. */
	double transferred_bytes;
	double link_pj_per_bit;
	/* PIMID 1.12.18: UCIe link class (user rulings 2026-10-23). Sources:
	 *  [UCIe11] D. Das Sharma, "The UCIe 1.1 Specification: Future
	 *           Applications of Chiplets", UCIe Consortium, 2023, table
	 *           "UCIe 1.0/1.1: Characteristics and Key Metrics".
	 *  [HC23]   UCIe Consortium, "UCIe Protocol", Hot Chips 2023 UCIe
	 *           Tutorial ("Protocol Layer presents 64B of the Flit on FDI").
	 * link_class      0 = legacy (SerDes/PIPE classes), 1 = UCIe.
	 * ucie_package    1 = advanced, 2 = standard, 0 = not given (refused
	 *                 for link_class 1).
	 * lanes           data lanes of the link (module width x modules).
	 * lane_rate_gts   per-lane data rate, GT/s (one of the [UCIe11] rates).
	 * fdi_width_bytes FDI data width, bytes; default 64 per [HC23].
	 * phy_pj_per_bit  <0 = the [UCIe11] power-efficiency target for the
	 *                 package; >=0 = a user value (printed as such).
	 * link_energy_end 1 = this end prices the link's per-bit energy (the
	 *                 link is priced once, at one end); 0 = the far end.
	 * link_node_nm    PIMID 1.12.18 (user ruling 2026-10-23, two nodes):
	 *                 process node (nm) of the die the link sits on, which
	 *                 selects the UCIe energy row; only 22 and 3 are priced
	 *                 (iocontrollers.cc). <= 0 = this McPAT run's node.
	 * clockrate <= 0 asks McPAT to DERIVE the D2D adapter / controller clock
	 * as lanes x lane_rate_gts / (8 bits x fdi_width_bytes). */
	int link_class;
	int ucie_package;
	int lanes;
	double lane_rate_gts;
	int fdi_width_bytes;
	double phy_pj_per_bit;
	int link_energy_end;
	int link_node_nm;   /* PIMID 1.12.18: see above */
	/* PIMID (user ruling 2026-10-27: serial links calibrated): 1 = price the link when it carries no
	 * traffic: a SerDes class in ASPM L1 between transfers, in L0 for its entry
	 * timer and its L1 exits (link_l0_idle_s, link_l1_exits x link_l1_exit_s,
	 * measured by the plugin); a UCIe link clock-gated (0.05 of 0.33 pJ/b,
	 * Shivnaraine et al. ISSCC 2026 8.3). 0 (default) = bytes only (upstream). */
	int link_idle_l0;
	double link_l0_idle_s;
	double link_l1_exits;
	double link_l1_exit_s;
} system_pcie;

typedef struct{
	//All number_of_* at the level of 'system' Ying 03/21/2009
	int number_of_cores;
	int number_of_L1Directories;
	int number_of_L2Directories;
	int number_of_L2s;
	bool Private_L2;
	int number_of_L3s;
	int number_of_NoCs;
	int number_of_dir_levels;
    int domain_size;
    int first_level_dir;
	// All params at the level of 'system'
	int homogeneous_cores;
	int homogeneous_L1Directories;
	int homogeneous_L2Directories;
	double core_tech_node;
	int target_core_clockrate;
	int target_chip_area;
	int temperature;
	int number_cache_levels;
	int L1_property;
	int L2_property;
	int homogeneous_L2s;
	int L3_property;
	int homogeneous_L3s;
	int homogeneous_NoCs;
	int homogeneous_ccs;
	int Max_area_deviation;
	int Max_power_deviation;
	int device_type;
	bool longer_channel_device;
	bool power_gating;
	bool Embedded;
	int  pimid_clock_network;   /* PIMID 1.12.18: 0 = clock distribution not priced (user option power.models.clock_network) */
	int  pimid_layout_energy_overhead;   /* PIMID 1.12.18 (ruling 2026-10-24): 1 = layout area overheads also scale array dynamic energy (upstream) */
	int  pimid_fu_base_energy;  /* PIMID 1.12.18 (ruling 2026-10-23 #1): 0 = the OOO FU fixed base energy is not charged */
	int  pimid_fpu_published;   /* PIMID 1.12.18 (ruling 2026-10-23 #2): 1 = published per-op FP energies (logic.cc) */
	int  pimid_int_alu_published;   /* PIMID 1.12.18 (ruling 2026-10-23 #3): 1 = published per-op integer energies (logic.cc) */
	int  pimid_fu_area_published;   /* PIMID 1.12.29: (ruling 2026-10-25) 1 = published functional-unit areas (logic.cc) */
	/* PIMID 1.12.18 (ruling 2026-10-23 #4): measured power-gated residency
	 * (fraction of the run) of the components the described design gates,
	 * 0 = always powered; stops the GLOBAL part of their clock tree
	 * (logic.h ClockNetwork). */
	double pimid_clock_pg_core, pimid_clock_pg_cache, pimid_clock_pg_noc, pimid_clock_pg_mc;
	int    pimid_clock_leaf_sinks;   /* PIMID (1.12.30, F2): sinks per leaf cluster (power.clock.leaf_cluster_sinks) */
	int    pimid_clock_split;   /* PIMID 1.12.18 (ruling #4): 1 = global part every powered cycle, local gated */
	/* PIMID 1.12.18 (user ruling 2026-10-23, idle halt): 1 = a core with
	 * nothing to run is halted (x86 HLT/C1, ARM WFI): its whole clock tree,
	 * global part included, is off for the halted time (core param
	 * pimid_running_fraction). 0 = upstream (global part every powered cycle). */
	int    pimid_idle_halt;
	/* PIMID 1.12.18 (user ruling, per-die process): the technology of the
	 * LOGIC die the components off the DRAM die sit on (node nm, CACTI device
	 * type). 0 / -1 = the chip's core_tech_node / device_type. */
	int    pimid_logic_die_node, pimid_logic_die_device_type;
	/* PIMID 1.12.18 (user ruling, per-die clock trunk): 1 = one die-level
	 * H-tree trunk per die feeding the component trees on it; the DRAM-die
	 * kind has pimid_dram_die_count dies of pimid_dram_die_area_mm2 each
	 * (0 = the area of the components McPAT puts there); the logic die is
	 * pimid_logic_die_area_mm2 (0 = the area of the components on it). */
	int    pimid_clock_trunk, pimid_dram_die_count;
	double pimid_dram_die_area_mm2, pimid_logic_die_area_mm2;
	bool opt_dynamic_power;
	bool opt_lakage_power;
	bool opt_clockrate;
	bool opt_area;
	int interconnect_projection_type;
	int machine_bits;
	int virtual_address_width;
	int physical_address_width;
	int virtual_memory_page_size;
    double total_cycles;
    double vdd;
    double power_gating_vcc;
	//system.core(0-n):3rd level
	system_core core[64];
	system_L1Directory L1Directory[64];
	system_L2Directory L2Directory[64];
	system_L2 L2[64];
	system_L3 L3[64];
    system_NoC NoC[64];
    system_mem mem;
	system_mc mc;
	system_mc flashc;
	system_niu niu;
	system_pcie pcie;
	/* PIMID 1.11.12: DRAM-PERIPHERY DEVICE FAMILY. McPAT prices every device
	 * in a logic process; silicon on a DRAM die (PEs, their caches, the
	 * on-die fabric and element controllers) is built from the die's
	 * peripheral transistors instead. CACTI cannot price general logic in
	 * its comm-dram device (gate-0, 1.11.3), so the family is a transform of
	 * McPAT's own logic result, applied HERE -- inside the tool that owns
	 * the components -- rather than by post-scaling the results outside
	 * (model-borders rule). 0 = logic = untouched. */
	int    dram_periph_family;      // 0 = logic (default), 1 = DRAM periphery
	/* PIMID 1.11.51 (N1): SUBARRAY pitch penalty independent of family --
	 * consumed by the family-0 core-area path; the family-1 transform keeps
	 * its dram_periph_pitch. Default 1.0. */
	double core_pitch_factor;
	/* PIMID 1.11.54 (E004): gate-leakage ratio, separate from the
	 * subthreshold one. Default 1.0 leaves gate leakage untouched. */
	double dram_periph_gate_leak;
	double dram_periph_area;
	double dram_periph_pitch;   /* PIMID 1.11.34 (E11): core-only pitch penalty */        // area factor
	double dram_periph_dyn;         // dynamic factor
	double dram_periph_leak;        // leakage factor (on PLAIN leakage)
	/* PIMID 1.11.56 (audit C010): dram_periph_scope is DELETED. It was a
	 * bitmask (1=core 2=caches 4=noc 8=mcs) that nobody emitted and nobody
	 * read -- the wrapper stopped writing the parameter and processor.cc
	 * removed the mask -- leaving a declaration, a parse hook and a default
	 * of 0 that between them described a scoping mechanism this fork no
	 * longer has. Per-level scoping is now NoC[i].on_dram_die. */
} root_system;

/* PIMID 1.12.18 (ruling 2026-10-23 #2): FP census categories, in PIMID's
 * order (zsim decoder_simple.h FpOpCensus). */
static const char* const pimid_fpc_name[9] = {"AddS", "MulS", "AddD", "MulD", "FmaS", "FmaD", "DivS", "DivD", "X87"};
/* PIMID 1.12.18 (ruling 2026-10-23 #3): integer census categories, in
 * PIMID's order (zsim decoder_simple.h, IntOp census): add/logic by operand
 * width, multiply by width, divide (all widths). */
static const char* const pimid_ic_name[9] = {"Alu8", "Alu16", "Alu32", "Alu64", "Mul8", "Mul16", "Mul32", "Mul64", "Div"};

class ParseXML
{
public:
	void parse(char* filepath);
    void initialize();
public:
	root_system sys;
};


#endif /* XML_PARSE_H_ */




