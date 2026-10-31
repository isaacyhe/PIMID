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
#ifndef IOCONTROLLERS_H_
#define IOCONTROLLERS_H_


#endif /* IOCONTROLLERS_H_ */

#include "XML_Parse.h"
#include "parameter.h"
//#include "io.h"
#include "array.h"
//#include "Undifferentiated_Core_Area.h"
#include <vector>
#include "basic_components.h"

class NIUController : public Component {
  public:
	ParseXML *XML;
	InputParameter interface_ip;
    NIUParam  niup;
    powerDef power_t;
    uca_org_t local_result;
    NIUController(ParseXML *XML_interface,InputParameter* interface_ip_);
    void set_niu_param();
    void computeEnergy(bool is_tdp=true);
    void displayEnergy(uint32_t indent = 0,int plevel = 100, bool is_tdp=true);
    ~NIUController(){};
};

class PCIeController : public Component {
  public:
	ParseXML *XML;
	InputParameter interface_ip;
    PCIeParam  pciep;
    powerDef power_t;
    uca_org_t local_result;
    /* PIMID 1.11.54 (audit E008): the SerDes DYNAMIC term McPAT computed, kept
     * so the runtime basis can remove it when PIMID supplies the measured
     * SerDes energy (bytes x pJ/bit) for the same silicon. Area and leakage
     * keep their SerDes share; only switching would be double-counted. */
    double SerDer_dyn_saved = 0.0;
    /* PIMID 1.12.18: UCIe link class state, read back by the wrapper so the
     * report can carry the link.controller / link.phy split.
     *   ucie                  link_class == 1
     *   ucie_pj_per_bit       applied adapter+PHY energy, pJ/bit
     *   ucie_phy_joules       measured bytes x 8 x ucie_pj_per_bit (J)
     *   ucie_phy_w            ucie_phy_joules / measured execution time (W)
     *   ctrl_dyn_per_cycle    McPAT's controller dynamic, J/cycle, all
     *                         channels (excluded from the UCIe runtime basis)
     *   ctrl_dyn_excluded_w   that term in W at the measured load, the amount
     *                         NOT charged (reported for audit)
     *   clock_mhz             controller clock in force; clock_derived says
     *                         whether McPAT derived it from the UCIe params */
    bool   ucie = false;
    double ucie_pj_per_bit = 0.0;
    double ucie_phy_joules = 0.0;
    double ucie_phy_w = 0.0;
    double ctrl_dyn_per_cycle = 0.0;
    /* PIMID (user ruling 2026-10-28: link controller logic priced): the published node ratio for logic
     * energy from 16 nm to this McPAT run's node (pimid_logic_op_scale), set at construction */
    double pimid_ctrl_scale = 1.0;
    bool   pimid_ctrl_scale_silicon = false;
    double ctrl_dyn_excluded_w = 0.0;
    double clock_mhz = 0.0;
    bool   clock_derived = false;
    /* PIMID 1.12.18 (user rulings 2026-10-23, two-node UCIe rows; sources in
     * iocontrollers.cc set_pcie_param):
     *   ucie_node_nm      process node of the row applied (22 or 3)
     *   ucie_analog_pj    analog PHY pJ/bit applied
     *   ucie_digital_pj   digital interface pJ/bit applied (ucie_pj_per_bit =
     *                     analog + digital)
     *   ucie_user_pj      phy_pj_per_bit (user) replaced both terms
     *   ucie_ref_ctrl_pj  CROSS-CHECK only, never charged: McPAT's own link
     *                     controller dynamic expressed per bit of this link
     *                     at this run's node */
    int    ucie_node_nm = 0;
    double ucie_analog_pj = 0.0;
    double ucie_digital_pj = 0.0;
    double ucie_tl_pj = 0.0;        /* transaction layer above the FDI, pJ/bit: the digital
                                     * interface's figure (0 with a user per-bit value) */
    bool   ucie_user_pj = false;
    double ucie_ref_ctrl_pj = 0.0;
    PCIeController(ParseXML *XML_interface,InputParameter* interface_ip_);
    void set_pcie_param();
    void computeEnergy(bool is_tdp=true);
    void displayEnergy(uint32_t indent = 0,int plevel = 100, bool is_tdp=true);
    ~PCIeController(){};
};

class FlashController : public Component {
  public:
	ParseXML *XML;
	InputParameter interface_ip;
    MCParam  fcp;
    powerDef power_t;
    uca_org_t local_result;
    FlashController(ParseXML *XML_interface,InputParameter* interface_ip_);
    void set_fc_param();
    void computeEnergy(bool is_tdp=true);
    void displayEnergy(uint32_t indent = 0,int plevel = 100, bool is_tdp=true);
    ~FlashController(){};
};

