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
#ifndef PROCESSOR_H_
#define PROCESSOR_H_

#include "XML_Parse.h"
#include "area.h"
#include "decoder.h"
#include "parameter.h"
#include "array.h"
#include "arbiter.h"
#include <vector>
#include "basic_components.h"
#include "core.h"
#include "memoryctrl.h"
#include "router.h"
#include "sharedcache.h"
#include "noc.h"
#include "iocontrollers.h"

class Processor : public Component
{
  public:
	ParseXML *XML;
	vector<Core *> cores;
    vector<SharedCache *> l2array;
    vector<SharedCache *> l3array;
    vector<SharedCache *> l1dirarray;
    vector<SharedCache *> l2dirarray;
    vector<NoC *>  nocs;
    MemoryController * mc;
    NIUController    * niu;
    PCIeController   * pcie;
    FlashController  * flashcontroller;
    InputParameter interface_ip;
    ProcParam procdynp;
    //wire	globalInterconnect;
    //clock_network globalClock;
    Component core, l2, l3, l1dir, l2dir, noc, mcs, cc, nius, pcies,flashcontrollers;
    /* PIMID 1.12.18: the clock-network share of each aggregate above, built
     * with the same multipliers and the same family transform, so a reader
     * can state it as a line of that component (Wattch clock model, D. Brooks,
     * V. Tiwari, M. Martonosi, ISCA 2000; logic.h ClockNetwork). Already
     * INCLUDED in core, l2, l3, l1dir, l2dir, mcs and noc. */
    Component clk_core, clk_l2, clk_l3, clk_l1dir, clk_l2dir, clk_mcs, clk_noc;
    /* PIMID 1.12.18: the UNGATED runtime clock dynamic (W) of the same
     * aggregates -- the reference beside the measured-activity-gated value
     * the aggregates carry (user ruling 2026-10-23). */
    double clku_core, clku_l2, clku_l3, clku_l1dir, clku_l2dir, clku_mcs, clku_noc;
    /* PIMID 1.12.18 (ruling 2026-10-23 #4): the GLOBAL part (W) of the
     * charged runtime clock of the same aggregates (every powered cycle);
     * the rest of the charged clock is the measured-activity-gated LOCAL part. */
    double clkg_core, clkg_l2, clkg_l3, clkg_l1dir, clkg_l2dir, clkg_mcs, clkg_noc;
    /* PIMID 1.12.18 (user ruling, per-die process): the LOGIC die's
    * technology (interface_ip with XML sys pimid_logic_die_node /
    * pimid_logic_die_device_type). Components on the DRAM die keep
    * interface_ip (core_tech_node + the dram_periph_family transform). */
    InputParameter ip_logic_die;
    bool logic_die_override;
    /* PIMID 1.12.18 (user ruling, per-die clock trunk): one die-level
    * H-tree trunk per die (logic.h ClockNetwork, built in the constructor).
    * clk_trunk is their sum (W: peak dynamic, runtime dynamic, leakage), NOT
    * inside any component above; trunk_dies one entry per die KIND (the
    * DRAM-die kind carries count dies; its watts are the sum over them). */
    struct PimidTrunkDie {
        string name; int count; double area_mm2, leaves, node_nm, on;
        double peak_w, rt_w, ungated_w, leak_w, gate_w;
    };
    std::vector<PimidTrunkDie> trunk_dies;
    Component clk_trunk;
    int  numCore, numL2, numL3, numNOC, numL1Dir, numL2Dir;
    Processor(ParseXML *XML_interface);
    void compute();
    void set_proc_param();
    void displayEnergy(uint32_t indent = 0,int plevel = 100, bool is_tdp=true);
    void displayDeviceType(int device_type_, uint32_t indent = 0);
    void displayInterconnectType(int interconnect_type_, uint32_t indent = 0);
    ~Processor();
};

#endif /* PROCESSOR_H_ */
