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
#include "logic.h"
#include "basic_circuit.h"
#include <iostream>
#include <algorithm>
#include "XML_Parse.h"
#include <string>
#include <cmath>
#include <assert.h>
#include "iocontrollers.h"
#include "basic_components.h"
void init_tech_params(double tech, bool is_tag);   /* PIMID 1.12.18: cacti/technology.cc */

/* PIMID 1.12.18 (two-node UCIe cross-check): switched capacitance of CACTI's
 * minimum inverter under the CURRENT technology tables (g_tp) -- the same
 * definition as logic.cc's pimid_min_inv_switch_cap (gate of the NMOS + PMOS
 * pair at McPAT's P/N ratio plus both drain junctions). */
static double pimid_io_min_inv_switch_cap()
{
	const double wn = g_tp.min_w_nmos_;
	const double wp = wn * pmos_to_nmos_sz_ratio();
	return gate_C(wn + wp, 0) + drain_C_(wn, NCH, 1, 1, g_tp.cell_h_def) + drain_C_(wp, PCH, 1, 1, g_tp.cell_h_def);
}


/*
SUN Niagara 2 I/O power analysis:
total signal bits: 711
Total FBDIMM bits: (14+10)*2*8= 384
PCIe bits:         (8 + 8)*2 = 32
10Gb NIC:          (4*2+4*2)*2 = 32
Debug I/Os:        168
Other I/Os:        711- 32-32 - 384 - 168 = 95

According to "Implementation of an 8-Core, 64-Thread, Power-Efficient SPARC Server on a Chip"
90% of I/Os are SerDers (the calucaltion is 384+64/(711-168)=83% about the same as the 90% reported in the paper)
--> around 80Pins are common I/Os.
Common I/Os consumes 71mW/Gb/s according to Cadence ChipEstimate @65nm
Niagara 2 I/O clock is 1/4 of core clock. --> 87pin (<--((711-168)*17%)) * 71mW/Gb/s *0.25*1.4Ghz = 2.17W

Total dynamic power of FBDIMM, NIC, PCIe = 84*0.132 + 84*0.049*0.132 = 11.14 - 2.17 = 8.98
Further, if assuming I/O logic power is about 50% of I/Os then Total energy of FBDIMM, NIC, PCIe = 11.14 - 2.17*1.5 = 7.89
 */

/*
 * A bug in Cadence ChipEstimator: After update the clock rate in the clock tab, a user
 * need to re-select the IP clock (the same clk) and then click Estimate. if not reselect
 * the new clock rate may not be propogate into the IPs.
 *
 */

NIUController::NIUController(ParseXML *XML_interface,InputParameter* interface_ip_)
:XML(XML_interface),
 interface_ip(*interface_ip_)
 {


	  double frontend_area, phy_area, mac_area, SerDer_area;
      double frontend_dyn, mac_dyn, SerDer_dyn;
      double frontend_gates, mac_gates, SerDer_gates = 0.;
	  double pmos_to_nmos_sizing_r = pmos_to_nmos_sz_ratio();
	  double NMOS_sizing, PMOS_sizing;

	  set_niu_param();
	  local_result = init_interface(&interface_ip);

	  if (niup.type == 0) //high performance NIU
	  {
		  //Area estimation based on average of die photo from Niagara 2 and Cadence ChipEstimate using 65nm.
		  mac_area = (1.53 + 0.3)/2 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  //Area estimation based on average of die photo from Niagara 2, ISSCC "An 800mW 10Gb Ethernet Transceiver in 0.13μm CMOS"
		  //and"A 1.2-V-Only 900-mW 10 Gb Ethernet Transceiver and XAUI Interface With Robust VCO Tuning Technique" Frontend is PCS
		  frontend_area = (9.8 + (6 + 18)*65/130*65/130)/3 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  //Area estimation based on average of die photo from Niagara 2 and Cadence ChipEstimate hard IP @65nm.
		  //SerDer is very hard to scale
		  SerDer_area = (1.39 + 0.36) * (interface_ip.F_sz_um/0.065);//* (interface_ip.F_sz_um/0.065);
		  phy_area = frontend_area + SerDer_area;
		  //total area
		  area.set_area((mac_area + frontend_area + SerDer_area)*1e6);
		  //Power
		  //Cadence ChipEstimate using 65nm (mac, front_end are all energy. E=P*T = P/F = 1.37/1Ghz = 1.37e-9);
		  mac_dyn      = 2.19e-9*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);//niup.clockRate; //2.19W@1GHz fully active according to Cadence ChipEstimate @65nm
		  //Cadence ChipEstimate using 65nm soft IP;
		  frontend_dyn = 0.27e-9*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);//niup.clockRate;
		  //according to "A 100mW 9.6Gb/s Transceiver in 90nm CMOS..." ISSCC 2006
		  //SerDer_dyn is power not energy, scaling from 10mw/Gb/s @90nm
		  SerDer_dyn   = 0.01*10*sqrt(interface_ip.F_sz_um/0.09)*g_tp.peri_global.Vdd/1.2*g_tp.peri_global.Vdd/1.2;
		  SerDer_dyn   /= niup.clockRate;//covert to energy per clock cycle of whole NIU

		  //Cadence ChipEstimate using 65nm
		  mac_gates       = 111700;
		  frontend_gates  = 320000;
		  SerDer_gates    = 200000;
		  NMOS_sizing 	  = 5*g_tp.min_w_nmos_;
		  PMOS_sizing	  = 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r;


	  }
	  else
	  {//Low power implementations are mostly from Cadence ChipEstimator; Ignore the multiple IP effect
		  // ---When there are multiple IP (same kind or not) selected, Cadence ChipEstimator results are not
		  // a simple summation of all IPs. Ignore this effect
		  mac_area      = 0.24 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  frontend_area = 0.1  * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);//Frontend is the PCS layer
		  SerDer_area   = 0.35 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  //Compare 130um implementation in "A 1.2-V-Only 900-mW 10 Gb Ethernet Transceiver and XAUI Interface With Robust VCO Tuning Technique"
		  //and the ChipEstimator XAUI PHY hard IP, confirm that even PHY can scale perfectly with the technology
		  //total area
		  area.set_area((mac_area + frontend_area + SerDer_area)*1e6);
		  //Power
		  //Cadence ChipEstimate using 65nm (mac, front_end are all energy. E=P*T = P/F = 1.37/1Ghz = 1.37e-9);
		  mac_dyn      = 1.257e-9*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);//niup.clockRate; //2.19W@1GHz fully active according to Cadence ChipEstimate @65nm
		  //Cadence ChipEstimate using 65nm soft IP;
		  frontend_dyn = 0.6e-9*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);//niup.clockRate;
		  //SerDer_dyn is power not energy, scaling from 216mw/10Gb/s @130nm
		  SerDer_dyn   = 0.0216*10*(interface_ip.F_sz_um/0.13)*g_tp.peri_global.Vdd/1.2*g_tp.peri_global.Vdd/1.2;
		  SerDer_dyn   /= niup.clockRate;//covert to energy per clock cycle of whole NIU

		  mac_gates       = 111700;
		  frontend_gates  = 52000;
		  SerDer_gates    = 199260;

		  NMOS_sizing 	  = g_tp.min_w_nmos_;
		  PMOS_sizing	  = g_tp.min_w_nmos_*pmos_to_nmos_sizing_r;

	  }

	  power_t.readOp.dynamic = mac_dyn + frontend_dyn + SerDer_dyn;
	  power_t.readOp.leakage = (mac_gates + frontend_gates + frontend_gates)*cmos_Isub_leakage(NMOS_sizing, PMOS_sizing, 2, nand)*g_tp.peri_global.Vdd;//unit W
	  double long_channel_device_reduction = longer_channel_device_reduction(Uncore_device);
	  double pg_reduction = power_gating_leakage_reduction(false);
	  power_t.readOp.longer_channel_leakage = power_t.readOp.leakage * long_channel_device_reduction;
	  power_t.readOp.power_gated_leakage = power_t.readOp.leakage * pg_reduction;
	  power_t.readOp.power_gated_with_long_channel_leakage = power_t.readOp.power_gated_leakage * long_channel_device_reduction;

	  power_t.readOp.gate_leakage = (mac_gates + frontend_gates + frontend_gates)*cmos_Ig_leakage(NMOS_sizing, PMOS_sizing, 2, nand)*g_tp.peri_global.Vdd;//unit W
 }

void NIUController::computeEnergy(bool is_tdp)
{
	if (is_tdp)
    {


		power	= power_t;
    	power.readOp.dynamic *= niup.duty_cycle;

    }
    else
    {
    	rt_power = power_t;
    	rt_power.readOp.dynamic *= niup.perc_load;
    }
}

void NIUController::displayEnergy(uint32_t indent,int plevel,bool is_tdp)
{
	string indent_str(indent, ' ');
	string indent_str_next(indent+2, ' ');
	bool long_channel = XML->sys.longer_channel_device;
	bool power_gating = XML->sys.power_gating;

	if (is_tdp)
	{
		cout << "NIU:" << endl;
		cout << indent_str<< "Area = " << area.get_area()*1e-6<< " mm^2" << endl;
		cout << indent_str << "Peak Dynamic = " << power.readOp.dynamic*niup.clockRate  << " W" << endl;
		cout << indent_str<< "Subthreshold Leakage = "
			<< (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
		if (power_gating) cout << indent_str << "Subthreshold Leakage with power gating = "
				<< (long_channel? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)  << " W" << endl;
		cout << indent_str<< "Gate Leakage = " << power.readOp.gate_leakage << " W" << endl;
		cout << indent_str << "Runtime Dynamic = " << rt_power.readOp.dynamic*niup.clockRate << " W" << endl;
		cout<<endl;
	}
	else
	{

	}

}

void NIUController::set_niu_param()
{
	niup.clockRate       = XML->sys.niu.clockrate;
	niup.clockRate       *= 1e6;
	niup.num_units       = XML->sys.niu.number_units;
	niup.duty_cycle      = XML->sys.niu.duty_cycle;
	niup.perc_load       = XML->sys.niu.total_load_perc;
	niup.type            = XML->sys.niu.type;
	if ( XML->sys.niu.vdd>0)
	{
		interface_ip.specific_hp_vdd = true;
		interface_ip.specific_lop_vdd = true;
		interface_ip.specific_lstp_vdd = true;
		interface_ip.hp_Vdd   = XML->sys.niu.vdd;
		interface_ip.lop_Vdd  = XML->sys.niu.vdd;
		interface_ip.lstp_Vdd = XML->sys.niu.vdd;
	}

	if ( XML->sys.niu.power_gating_vcc > -1)
	{
		interface_ip.specific_vcc_min = true;
		interface_ip.user_defined_vcc_min   = XML->sys.niu.power_gating_vcc;

	}
//	  niup.executionTime   = XML->sys.total_cycles/(XML->sys.target_core_clockrate*1e6);
}

PCIeController::PCIeController(ParseXML *XML_interface,InputParameter* interface_ip_)
:XML(XML_interface),
 interface_ip(*interface_ip_)
 {

	  double frontend_area, phy_area, ctrl_area, SerDer_area;
      double ctrl_dyn, frontend_dyn, SerDer_dyn;
      double ctrl_gates,frontend_gates, SerDer_gates=0.;
	  double pmos_to_nmos_sizing_r = pmos_to_nmos_sz_ratio();
	  double NMOS_sizing, PMOS_sizing;

	  /* Assuming PCIe is bit-slice based architecture
	   * This is the reason for /8 in both area and power calculation
	   * to get per lane numbers
	   */
	  set_pcie_param();
	  local_result = init_interface(&interface_ip);

	  if (pciep.type == 0) //high performance NIU
	  {
		  //Area estimation based on average of die photo from Niagara 2 and Cadence ChipEstimate @ 65nm.
		  ctrl_area = (5.2 + 0.5)/2 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  //Area estimation based on average of die photo from Niagara 2, and Cadence ChipEstimate @ 65nm.
		  frontend_area = (5.2 + 0.1)/2 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  //Area estimation based on average of die photo from Niagara 2 and Cadence ChipEstimate hard IP @65nm.
		  //SerDer is very hard to scale
		  SerDer_area = (3.03 + 0.36) * (interface_ip.F_sz_um/0.065);//* (interface_ip.F_sz_um/0.065);
		  phy_area = frontend_area + SerDer_area;
		  //total area
		  //Power
		  //Cadence ChipEstimate using 65nm the controller includes everything: the PHY, the data link and transaction layer
		  ctrl_dyn      = 3.75e-9/8*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);
		  //	  //Cadence ChipEstimate using 65nm soft IP;
		  //	  frontend_dyn = 0.27e-9/8*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);
		  //SerDer_dyn is power not energy, scaling from 10mw/Gb/s @90nm
		  /* PIMID 1.11.29 (user ruling, step 3): the lane rate is a PARAMETER.
		   * It was the literal 4 -- PCIe 2.0's 4 Gb/s per lane, a 2007
		   * standard -- applied to every link class PIMID can configure:
		   * gen5 (32 GT/s, 8x), CXL (gen5/6 PHY), NVLink, UALink (200G-class,
		   * ~50x). The 0.01 is 10 mW per Gb/s at 90nm, so the product is
		   * literally "mW/(Gb/s) x rate": parameterising the rate is exact,
		   * not a fudge. serdes_lane_gbps <= 0 keeps the historical 4. */
		  const double lane_gbps = (pciep.serdes_lane_gbps > 0.0)
		                         ? pciep.serdes_lane_gbps : 4.0;
		  SerDer_dyn   = 0.01*lane_gbps*(interface_ip.F_sz_um/0.09)*g_tp.peri_global.Vdd/1.2*g_tp.peri_global.Vdd/1.2;
		  SerDer_dyn   /= pciep.clockRate;//covert to energy per clock cycle

		  //power_t.readOp.dynamic = (ctrl_dyn)*pciep.num_channels;
		  //Cadence ChipEstimate using 65nm
		  ctrl_gates       = 900000/8*pciep.num_channels;
		  //	  frontend_gates   = 120000/8;
		  /* PIMID 1.11.60 (audit round 4, D009): THE HIGH-PERFORMANCE ARM GAVE
		   * THE SerDes AREA AND DYNAMIC AND ZERO LEAKAGE.
		   *
		   * SerDer_gates is declared `= 0.` at the top of this constructor and
		   * was assigned ONLY in the low-power arm below; upstream left the
		   * assignment here commented out (the line that stood immediately
		   * below this block). The two totals at the bottom of the constructor
		   * then read (ctrl_gates + (withPHY? SerDer_gates:0)), so on a type=0
		   * link with withPHY=1 the SerDes was charged for area and for
		   * switching and credited with EXACTLY ZERO leakage and zero gate
		   * leakage. That is the arm E008's "AREA and LEAKAGE keep it" rests
		   * on, and the corrected wording of that note is below.
		   *
		   * Invisible for two compounding reasons: `SerDer_gates=0.` at the
		   * declaration makes the unassigned path look initialised rather than
		   * missing, and the withPHY ternary makes a zero contribution look
		   * like a deliberate configuration choice instead of a dropped
		   * assignment. Nothing warns, and the arm is dead for PIMID's own
		   * numbers (the wrapper emits sys.pcie.type = 1, and CMakeLists
		   * excludes main.cc so nothing consumes the reference XMLs) -- but
		   * 1.11.59's withPHY pass set withPHY=1 on the eight type=0 reference
		   * inputs (Alpha21364, Niagara1, the three Niagara1_sharing_*,
		   * Niagara2, Penryn, Xeon), which is precisely the configuration that
		   * exercises it.
		   *
		   * The figure is NOT invented: 200000/8 is McPAT's own count, written
		   * into this arm and commented out here, and it is the same count the
		   * low-power arm uses live. The *pciep.num_channels factor is added
		   * to match the convention of every other gate count that reaches the
		   * leakage sum (ctrl_gates carries it on both arms, and so does the
		   * low-power SerDer_gates); the commented line predates that factor.
		   * Be clear about what this does and does not claim: the type=0
		   * SerDes has ~9x the area of the low-power one here (3.39 vs 0.36
		   * scaled mm^2), so reusing the low-power gate count is if anything
		   * an UNDER-estimate of its leakage. The alternative was to leave it
		   * at exactly zero, which is the one value we know to be wrong. */
		  SerDer_gates     = 200000/8*pciep.num_channels;
		  NMOS_sizing 	  = 5*g_tp.min_w_nmos_;
		  PMOS_sizing	  = 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r;
	  }
	  else
	  {
		  ctrl_area = 0.412 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  //Area estimation based on average of die photo from Niagara 2, and Cadence ChipEstimate @ 65nm.
     	  SerDer_area = 0.36 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  //total area
		  //Power
		  //Cadence ChipEstimate using 65nm the controller includes everything: the PHY, the data link and transaction layer
		  ctrl_dyn      = 2.21e-9/8*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);
		  //	  //Cadence ChipEstimate using 65nm soft IP;
		  //	  frontend_dyn = 0.27e-9/8*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);
		  //SerDer_dyn is power not energy, scaling from 10mw/Gb/s @90nm
		  /* PIMID 1.11.29 step 3: same parameterisation on the low-power arm. */
		  const double lane_gbps_lp = (pciep.serdes_lane_gbps > 0.0)
		                            ? pciep.serdes_lane_gbps : 4.0;
		  SerDer_dyn   = 0.01*lane_gbps_lp*(interface_ip.F_sz_um/0.09)*g_tp.peri_global.Vdd/1.2*g_tp.peri_global.Vdd/1.2;
		  SerDer_dyn   /= pciep.clockRate;//covert to energy per clock cycle

		  //Cadence ChipEstimate using 65nm
		  ctrl_gates       = 200000/8*pciep.num_channels;
		  //	  frontend_gates   = 120000/8;
		  SerDer_gates     = 200000/8*pciep.num_channels;
		  NMOS_sizing 	  = g_tp.min_w_nmos_;
		  PMOS_sizing	  = g_tp.min_w_nmos_*pmos_to_nmos_sizing_r;

	  }
	  SerDer_dyn_saved = SerDer_dyn;   // PIMID 1.11.54 (E008)
	  area.set_area(((ctrl_area + (pciep.withPHY? SerDer_area:0))/8*pciep.num_channels)*1e6);
	  power_t.readOp.dynamic = (ctrl_dyn + (pciep.withPHY? SerDer_dyn:0))*pciep.num_channels;
	  ctrl_dyn_per_cycle = ctrl_dyn*pciep.num_channels;   // PIMID 1.12.18: kept for audit
	  if (ucie) {
		  /* PIMID 1.12.18: THE UCIe DYNAMIC BASIS. [UCIe11]'s pJ/b is the
		   * energy of the D2D adapter and PHY -- the stack below the FDI,
		   * "FDI to bump and back" in the same table. McPAT's ctrl_dyn above
		   * is Cadence ChipEstimate's PCIe controller ("the controller
		   * includes everything: the PHY, the data link and transaction
		   * layer"), modelled bit-sliced PER LANE (the /8 and x num_channels
		   * above). Its PHY and data-link share is exactly what the adapter
		   * (CRC/retry, link management) and the UCIe PHY replace and what the
		   * pJ/b prices; no published figure splits the Cadence number into
		   * PHY / data link / transaction layer. Charging both would pay for
		   * the adapter+PHY twice, so the controller DYNAMIC is excluded and
		   * the per-bit term stands alone. Area and leakage stay McPAT's (real
		   * silicon that leaks; the pJ/b prices switching only).
		   * NO PUBLISHED SOURCE FOUND: dynamic energy of the protocol layer
		   * ABOVE the FDI (the transaction layer) separable from the Cadence
		   * figure -- it is therefore not priced, said at runtime.
		   * Peak (TDP) dynamic: the link at full rate, lanes x GT/s x pJ/b,
		   * at the pricing end only (the link is priced once). */
		  const double peak_w = XML->sys.pcie.link_energy_end
		      ? XML->sys.pcie.lanes * XML->sys.pcie.lane_rate_gts * 1e9 * ucie_pj_per_bit * 1e-12
		      : 0.0;
		  power_t.readOp.dynamic = peak_w / pciep.clockRate;   // J per controller cycle
		  /* PIMID 1.12.18 (two-node rows): the per-bit term is now analog PHY
		   * + digital interface (set_pcie_param). The digital interface of
		   * [MS26] (flow control, calibration, error correction, repair) is
		   * the adapter-level logic McPAT's controller term also prices, so
		   * the controller DYNAMIC stays excluded (no double count).
		   * CROSS-CHECK 1 (user: "verify with 3"), printed, NEVER charged:
		   * McPAT's own controller dynamic for this link, per bit:
		   *   ctrl_dyn_per_cycle [J/cycle, all lanes] / (lanes x GT/s x 1e9 /
		   *   clock [Hz]) [bits/cycle]. */
		  const double bits_per_cycle = XML->sys.pcie.lanes * XML->sys.pcie.lane_rate_gts * 1e9 / pciep.clockRate;
		  ucie_ref_ctrl_pj = (bits_per_cycle > 0.0) ? ctrl_dyn_per_cycle / bits_per_cycle * 1e12 : 0.0;
		  pimid::debugStream(std::cerr) << "[link] CROSS-CHECK (McPAT PIMID 1.12.18, reference only, NOT charged): McPAT's own"
		               " link controller dynamic (Cadence ChipEstimate 65 nm PCIe controller, scaled by McPAT"
		               " to " << interface_ip.F_sz_nm << " nm, Vdd " << g_tp.peri_global.Vdd
		            << " V) for this link (" << XML->sys.pcie.lanes << " lanes x " << XML->sys.pcie.lane_rate_gts
		            << " GT/s, clock " << clock_mhz << " MHz) = " << ctrl_dyn_per_cycle << " J/cycle / "
		            << bits_per_cycle << " bit/cycle = " << ucie_ref_ctrl_pj << " pJ/bit, beside the applied digital"
		               " interface " << ucie_digital_pj << " pJ/bit (" << ucie_node_nm << " nm row"
		            << (ucie_user_pj ? ", user value" : "") << "): McPAT/digital = "
		            << ((ucie_digital_pj > 0.0) ? ucie_ref_ctrl_pj / ucie_digital_pj : 0.0)
		            << "; McPAT/(analog+digital) = "
		            << ((ucie_pj_per_bit > 0.0) ? ucie_ref_ctrl_pj / ucie_pj_per_bit : 0.0) << std::endl;
	  }
	  power_t.readOp.leakage = (ctrl_gates + (pciep.withPHY? SerDer_gates:0))*cmos_Isub_leakage(NMOS_sizing, PMOS_sizing, 2, nand)*g_tp.peri_global.Vdd;//unit W
	  double long_channel_device_reduction = longer_channel_device_reduction(Uncore_device);
	  double pg_reduction = power_gating_leakage_reduction(false);
	  power_t.readOp.longer_channel_leakage = power_t.readOp.leakage * long_channel_device_reduction;
  	  power_t.readOp.power_gated_leakage = power_t.readOp.leakage * pg_reduction;
	  power_t.readOp.power_gated_with_long_channel_leakage = power_t.readOp.power_gated_leakage * long_channel_device_reduction;
	  power_t.readOp.gate_leakage = (ctrl_gates + (pciep.withPHY? SerDer_gates:0))*cmos_Ig_leakage(NMOS_sizing, PMOS_sizing, 2, nand)*g_tp.peri_global.Vdd;//unit W
 }

void PCIeController::computeEnergy(bool is_tdp)
{
	if (is_tdp)
    {


		power	= power_t;
    	power.readOp.dynamic *= pciep.duty_cycle;

    }
    else
    {
    	rt_power = power_t;
    	rt_power.readOp.dynamic *= pciep.perc_load;
    	if (ucie) {
    		/* PIMID 1.12.18: link.phy = measured bytes x 8 x pJ/b, the
    		 * analog PHY + digital interface figure of the node row
    		 * (set_pcie_param); link.controller dynamic is NOT
    		 * charged (see the constructor), and perc_load does not scale the
    		 * per-bit term: the bytes ARE the activity. Expressed as energy per
    		 * cycle so the Processor's (number_units x clockRate) aggregation
    		 * restores watts exactly. */
    		rt_power.readOp.dynamic = 0.0;
    		const double units = (XML->sys.pcie.number_units > 0) ?
    		                     (double)XML->sys.pcie.number_units : 1.0;
    		ctrl_dyn_excluded_w = ctrl_dyn_per_cycle * pciep.perc_load * pciep.clockRate * units;
    		double execTimeSec = 0.0;
    		if (XML->sys.total_cycles > 0 && XML->sys.target_core_clockrate > 0)
    			execTimeSec = XML->sys.total_cycles / (XML->sys.target_core_clockrate * 1e6);
    		const double bytes = XML->sys.pcie.link_energy_end ? XML->sys.pcie.transferred_bytes : 0.0;
    		ucie_phy_joules = bytes * 8.0 * ucie_pj_per_bit * 1e-12;
    		if (bytes > 0.0) {
    			if (execTimeSec <= 0.0) {
    				std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): UCIe link carries "
    				          << bytes << " measured bytes but the run has no measured time"
    				             " (total_cycles / target_core_clockrate); its energy cannot"
    				             " be expressed as power." << std::endl;
    				exit(2);
    			}
    			ucie_phy_w = ucie_phy_joules / execTimeSec;
    			rt_power.readOp.dynamic = ucie_phy_joules / execTimeSec / (units * pciep.clockRate);
    		}
    		pimid::debugStream(std::cerr) << "[link] UCIe (McPAT PIMID 1.12.18): link.phy = " << bytes
    		          << " B x 8 x (analog " << ucie_analog_pj << " + digital " << ucie_digital_pj
    		          << " = " << ucie_pj_per_bit << " pJ/bit) = " << ucie_phy_joules
    		          << " J over " << execTimeSec << " s = " << ucie_phy_w << " W"
    		          << (XML->sys.pcie.link_energy_end ? "" : " [far end: the link is priced at the other end]")
    		          << "; link.controller dynamic EXCLUDED (" << ctrl_dyn_excluded_w
    		          << " W at measured load " << pciep.perc_load << ", clock " << clock_mhz
    		          << " MHz): the analog PHY + digital interface pJ/bit covers the PHY and the"
    		             " adapter-level logic (flow control, calibration, error correction, repair), which"
    		             " McPAT's per-lane controller term also prices; link.controller area"
    		             " and leakage kept. NO PUBLISHED SOURCE FOUND: transaction-layer"
    		             " dynamic above the FDI, not priced." << std::endl;
    		return;
    	}
    	/* PIMID 1.11.7 (#85): activity-driven link energy. When PIMID
    	 * supplies measured crossing bytes and a per-link-type pJ/bit
    	 * (PCIe gen3/4/5, CXL, NVLink -- selected and cited PIMID-side),
    	 * runtime dynamic is computed FROM the transfer: total joules =
    	 * bytes*8*pJ/bit, expressed as energy-per-cycle so the Processor's
    	 * (number_units x clockRate) aggregation restores watts exactly.
    	 * Zero traffic therefore reports zero link dynamic. The legacy
    	 * perc_load scaling above remains for inputs without measurement. */
    	if (XML->sys.pcie.transferred_bytes > 0 &&
    	    XML->sys.pcie.link_pj_per_bit > 0 &&
    	    XML->sys.total_cycles > 0 && XML->sys.target_core_clockrate > 0)
    	{
    		double execTimeSec = XML->sys.total_cycles /
    		                     (XML->sys.target_core_clockrate * 1e6);
    		double joules = XML->sys.pcie.transferred_bytes * 8.0 *
    		                XML->sys.pcie.link_pj_per_bit * 1e-12;
    		double units = (XML->sys.pcie.number_units > 0) ?
    		               (double)XML->sys.pcie.number_units : 1.0;
    		double denom = units * ((pciep.clockRate > 0) ? pciep.clockRate : 1.0);
    		/* PIMID 1.11.41 (audit E20, user ruling): ADD, do not replace.
    		 *
    		 * This used to ASSIGN, discarding the perc_load result above it on
    		 * the rationale that pJ/bit is "an end-to-end link figure". The
    		 * figures actually sourced in 1.11.40 do not support that reading:
    		 * they are SerDes measurements -- "a 32Gb/s NRZ 37dB SerDes in 10nm
    		 * CMOS", "PCIe Gen5/SAS4 SerDes IP on Samsung 8LPP", the 11.4 one
    		 * explicitly "including PLL and clocking". PHY-side numbers.
    		 *
    		 * What McPAT computes here is the DIGITAL protocol engine: LTSSM,
    		 * replay buffers, flit assembly, DMA. Different silicon from the
    		 * SerDes, so charging both is not double-counting -- the same
    		 * argument that put the coherence flush on the link AND in the DRAM
    		 * array in E18. Assigning deleted the controller's dynamic power on
    		 * every co-sim run while leaving its area and leakage in place, so
    		 * the reported controller had area, leaked, and did no work. */
    		/* PIMID 1.11.54 (audit E008): DO NOT PAY FOR THE SerDes TWICE.
    		 *
    		 * The 1.11.41 note above is right that McPAT's ctrl term is the
    		 * digital protocol engine and our pJ/bit figures are PHY-side --
    		 * but power_t.readOp.dynamic above ALSO contains SerDer_dyn
    		 * whenever withPHY is set, and PIMID sets withPHY for every link
    		 * class except interposer. So for pcie gen3/4/5, CXL, NVLink and
    		 * UALink the run was charging McPAT's SerDes model AND the
    		 * measured SerDes energy for the same transistors. Measured at
    		 * 22 nm: 0.01 * lane_gbps * (F_sz/0.09) * (Vdd/1.2)^2 is ~34.7
    		 * mW/channel/unit at gen5 and ~217 mW at UALink -- comparable to
    		 * the whole reported link-controller figure.
    		 *
    		 * The measured band is the better-sourced of the two (it names
    		 * its process and whether clocking is included), so it stands and
    		 * McPAT's SerDes DYNAMIC is removed from the runtime basis. AREA
    		 * and LEAKAGE keep it: the SerDes is real silicon that really
    		 * leaks, and our pJ/bit figure prices switching only.
    		 *
    		 * PIMID 1.11.60 (audit round 4, D009): "AREA AND LEAKAGE KEEP IT"
    		 * WAS A CLAIM ABOUT THE ARGUMENT, NOT ABOUT THE CODE. Area kept
    		 * it on both arms, but leakage kept it only on the LOW-POWER arm
    		 * (pciep.type != 0): SerDer_gates was declared `= 0.` and assigned
    		 * nowhere in the high-performance arm, so with withPHY=1 on a
    		 * type=0 link the leakage term this justification leans on was
    		 * identically zero -- the dynamic was removed on the stated ground
    		 * that leakage still carried the SerDes, and it did not. The
    		 * sentence was true of PIMID's own runs (the wrapper emits
    		 * sys.pcie.type = 1, the low-power arm) and false of the eight
    		 * type=0 reference inputs 1.11.59's withPHY pass had just made
    		 * live, and nothing in either place printed a leakage number small
    		 * enough to notice. The type=0 arm now assigns McPAT's own
    		 * commented-out gate count (see the note at that assignment), so
    		 * the sentence above and the stderr line below describe the code on
    		 * BOTH arms. PIMID's numbers do not move: it never took the type=0
    		 * arm, and nothing in this tree builds main.cc to run the reference
    		 * XMLs. */
    		if (pciep.withPHY) {
    			double serdes_rt = SerDer_dyn_saved * pciep.num_channels
    			                 * pciep.perc_load;
    			rt_power.readOp.dynamic -= serdes_rt;
    			if (rt_power.readOp.dynamic < 0.0) rt_power.readOp.dynamic = 0.0;
    			/* Report the correction so it is auditable. Its size scales
    			 * with the MEASURED duty: on a nearly idle link (duty 0.002 on
    			 * the reference co-sim cell) it is invisible at the printed
    			 * precision, while on a link-heavy run it is the dominant term
    			 * -- 34.7 mW/channel at gen5 and 217 mW at UALink, against a
    			 * controller term of a few mW. Stderr: the parent's stdout is
    			 * /dev/null in the forked child. */
    			/* PIMID 1.11.57 (audit D009): PRINT WATTS UNDER THE " W".
    			 *
    			 * serdes_rt is ENERGY PER CLOCK CYCLE, not power: SerDer_dyn is
    			 * divided by pciep.clockRate at construction ("covert to energy
    			 * per clock cycle") and clockRate is in Hz. Every consumer that
    			 * wants watts multiplies it back -- displayEnergy does
    			 * rt_power.readOp.dynamic*pciep.clockRate, and Processor's
    			 * set_pppm passes number_units*pcie->pciep.clockRate -- but this
    			 * diagnostic did not, so it printed J/cycle with a " W" suffix.
    			 * Wrong by the link clock: the comment directly above promises
    			 * "34.7 mW/channel at gen5" while the line printed ~1.7e-11 at a
    			 * 2 GHz link clock. Invisible because it is the ONLY audit trail
    			 * for the correction -- there is no second place printing the
    			 * same quantity to disagree with, and a number nine orders of
    			 * magnitude small reads as "negligible", which is the direction
    			 * a reader is already half-expecting on an idle link. */
    			const double serdes_rt_w = serdes_rt * pciep.clockRate;
    			static bool said = false;
    			if (!said) {
    				said = true;
    				pimid::debugStream(std::cerr) << "[link] E008: removed McPAT's SerDes dynamic ("
    				          << serdes_rt_w << " W at measured duty "
    				          << pciep.perc_load << ") from the runtime basis;"
    				             " the measured pJ/bit term prices that same"
    				             " silicon. Area and leakage keep it."
    				          << std::endl;
    			}
    		}
    		rt_power.readOp.dynamic += joules / execTimeSec / denom;
    	}
    }
}

void PCIeController::displayEnergy(uint32_t indent,int plevel,bool is_tdp)
{
	string indent_str(indent, ' ');
	string indent_str_next(indent+2, ' ');
	bool long_channel = XML->sys.longer_channel_device;
	bool power_gating = XML->sys.power_gating;

	if (is_tdp)
	{
		cout << "PCIe:" << endl;
		cout << indent_str<< "Area = " << area.get_area()*1e-6<< " mm^2" << endl;
		cout << indent_str << "Peak Dynamic = " << power.readOp.dynamic*pciep.clockRate  << " W" << endl;
		cout << indent_str<< "Subthreshold Leakage = "
			<< (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
		if (power_gating) cout << indent_str << "Subthreshold Leakage with power gating = "
				<< (long_channel? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)  << " W" << endl;
		cout << indent_str<< "Gate Leakage = " << power.readOp.gate_leakage << " W" << endl;
		cout << indent_str << "Runtime Dynamic = " << rt_power.readOp.dynamic*pciep.clockRate << " W" << endl;
		cout<<endl;
	}
	else
	{

	}

}

void PCIeController::set_pcie_param()
{
	  pciep.clockRate       = XML->sys.pcie.clockrate;
	  pciep.clockRate       *= 1e6;
	  pciep.num_units       = XML->sys.pcie.number_units;
	  pciep.num_channels    = XML->sys.pcie.num_channels;
	  pciep.duty_cycle      = XML->sys.pcie.duty_cycle;
	  pciep.perc_load       = XML->sys.pcie.total_load_perc;
	  pciep.type            = XML->sys.pcie.type;
	  pciep.withPHY         = XML->sys.pcie.withPHY;
	  pciep.serdes_lane_gbps = XML->sys.pcie.serdes_lane_gbps;   // PIMID 1.11.29

	  /* PIMID 1.12.18: UCIe link class and the controller clock.
	   *
	   * CLOCK (user ruling 2026-10-23: "parameter as it is but default to
	   * published number"). A given clockrate (> 0) is used as is. When none
	   * is given (clockrate <= 0) and the link describes a D2D interface
	   * (lanes, lane_rate_gts, fdi_width_bytes), the D2D adapter clock is the
	   * rate at which the FDI moves one fdi_width_bytes word:
	   *   clock = lanes x lane_rate_gts / (8 bits x fdi_width_bytes)
	   * Sources: lane rates and module widths from [UCIe11] (table "UCIe
	   * 1.0/1.1: Characteristics and Key Metrics": 4/8/12/16/24/32 GT/s;
	   * 16 lanes standard, 64 advanced); the 64 B FDI width from [HC23] ("All
	   * examples show 64B data width on FDI"; "Protocol Layer presents 64B of
	   * the Flit on FDI"). 64 lanes x 16 GT/s / (8 x 64) = 2000 MHz.
	   * With no clock and no D2D description there is no published number:
	   * refuse, rather than assume one. */
	  ucie = (XML->sys.pcie.link_class == 1);
	  if (XML->sys.pcie.link_class != 0 && XML->sys.pcie.link_class != 1) {
		  std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): system.pcie link_class "
		            << XML->sys.pcie.link_class << " is not 0 (legacy) or 1 (UCIe)." << std::endl;
		  exit(2);
	  }
	  if (pciep.clockRate <= 0.0) {
		  if (XML->sys.pcie.lanes > 0 && XML->sys.pcie.lane_rate_gts > 0.0 &&
		      XML->sys.pcie.fdi_width_bytes > 0) {
			  clock_mhz = XML->sys.pcie.lanes * XML->sys.pcie.lane_rate_gts * 1000.0
			            / (8.0 * XML->sys.pcie.fdi_width_bytes);
			  clock_derived = true;
			  pciep.clockRate = clock_mhz * 1e6;
			  std::cerr << "[link] controller clock " << clock_mhz << " MHz DERIVED in McPAT = "
			            << XML->sys.pcie.lanes << " lanes x " << XML->sys.pcie.lane_rate_gts
			            << " GT/s / (8 bits x " << XML->sys.pcie.fdi_width_bytes
			            << " B FDI) [UCIe 1.1 white paper (Das Sharma 2023) rates/widths;"
			               " Hot Chips 2023 UCIe Tutorial: 64B on FDI]" << std::endl;
		  } else {
			  std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): NO PUBLISHED SOURCE FOUND:"
			               " controller clock for this link (no clockrate given, and no"
			               " lanes/lane_rate_gts/fdi_width_bytes to derive a D2D clock from)."
			               " Supply the clock (PIMID: power.link.controller_clock_mhz)." << std::endl;
			  exit(2);
		  }
	  } else {
		  clock_mhz = pciep.clockRate / 1e6;
	  }
	  if (ucie) {
		  /* PIMID 1.12.18: UCIe has no SerDes: its PHY is a parallel
		   * single-ended AFE whose energy [UCIe11] states per bit. A SerDes
		   * term on top would price a block that does not exist. */
		  if (pciep.withPHY) {
			  std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): link_class 1 (UCIe) with"
			               " withPHY=1: UCIe has no SerDes; its PHY is priced per bit." << std::endl;
			  exit(2);
		  }
		  const int pkg = XML->sys.pcie.ucie_package;
		  if (pkg != 1 && pkg != 2) {
			  std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): NO PUBLISHED SOURCE FOUND:"
			               " UCIe package for this link (ucie_package must be 1 = advanced or"
			               " 2 = standard; the [UCIe11] targets differ by package)." << std::endl;
			  exit(2);
		  }
		  /* [UCIe11] widths per cluster: 64 advanced, 16 standard. */
		  const int module_w = (pkg == 1) ? 64 : 16;
		  if (XML->sys.pcie.lanes <= 0 || (XML->sys.pcie.lanes % module_w) != 0) {
			  std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): UCIe lanes "
			            << XML->sys.pcie.lanes << " is not a positive multiple of the "
			            << module_w << "-lane module of the "
			            << (pkg == 1 ? "advanced" : "standard")
			            << " package [UCIe 1.1 white paper, Das Sharma 2023]." << std::endl;
			  exit(2);
		  }
		  /* [UCIe11] data rates: 4, 8, 12, 16, 24, 32 GT/s. */
		  const double r = XML->sys.pcie.lane_rate_gts;
		  if (!(r == 4.0 || r == 8.0 || r == 12.0 || r == 16.0 || r == 24.0 || r == 32.0)) {
			  std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): UCIe lane_rate_gts " << r
			            << " is not a UCIe 1.0/1.1 data rate (4, 8, 12, 16, 24, 32 GT/s)"
			               " [UCIe 1.1 white paper, Das Sharma 2023]." << std::endl;
			  exit(2);
		  }
		  /* PIMID 1.12.18 (user rulings 2026-10-23): TWO-NODE UCIe ENERGY ROWS.
		   * The link is priced per bit as ANALOG PHY + DIGITAL INTERFACE, from
		   * published measurements, for exactly two process nodes: 22 and 3 nm
		   * (user: "just provide 2 techs ... 22 and 3"). Sources:
		   *  [CDN26] D. T. Melek et al. (Cadence), "A 0.29 pJ/b 5.27 Tb/s/mm
		   *          UCIe Advanced Package Link With 2.5-D CoWoS Interposer and a
		   *          0.52 pJ/b 0.448 Tb/s/mm UCIe Standard Package Link With
		   *          Organic Substrate in 3 nm FinFET", IEEE JSSC vol. 61, no. 7,
		   *          July 2026, DOI 10.1109/JSSC.2026.3651425: "The link operates
		   *          under a core supply (VDD) of 0.75 V, and a high supply (VDDH)
		   *          of 1.2 V, and a TX driver supply (VCCIO) of 0.45 V. One
		   *          UCIe-AP module consumes a total of 297 mW of measured analog
		   *          power, which corresponds to 0.29 pJ/bit analog power
		   *          efficiency" (64 lanes x 16 Gb/s; the PLL is in the TX common
		   *          block); standard package: "the PHY exhibits an analog power
		   *          efficiency of 0.52 pJ/bit running at 16 Gb/s/pin for a 25 mm
		   *          channel".
		   *  [INT26] S. Mondal et al. (Intel), "A 48Gb/s/lane 1.24Tb/s/mm
		   *          UCIe-Compliant Die-to-Die Link Over 30mm Standard Package",
		   *          ISSCC 2026, paper 8.1, DOI 10.1109/ISSCC49663.2026.11409120:
		   *          "Fabricated in 22nm FinFET CMOS"; "At VDD=0.7V (VDDH=0.85V
		   *          for TX pre-driver)"; "energy efficiency of 1.2pJ/b (0.45,
		   *          0.69, and 0.06pJ/b in TX, RX, and PI+QH, respectively; a PLL
		   *          is not integrated in this work)".
		   *  [MS26]  R. Shivnaraine et al. (Microsoft), "A 0.23pJ/b 24Gb/s
		   *          Modular D2D Interface With Zero Wake Penalty Clock Gating in
		   *          3nm", ISSCC 2026, paper 8.3, DOI
		   *          10.1109/ISSCC49663.2026.11409276: "0.33pJ/b (0.23pJ/b PHY
		   *          only) at 24Gb/s - power is quoted at 0.75V for 24Gb/s"; the
		   *          digital interface "manages traffic flow control (credit
		   *          based), performs calibration tasks during initialization,
		   *          houses error correction, and redundancy/repair logic".
		   * Rows (pJ/bit):
		   *   3 nm  advanced  analog 0.29 [CDN26]            digital 0.10 [MS26]
		   *   3 nm  standard  analog 0.52 [CDN26]            digital 0.10 [MS26]
		   *   22 nm standard  analog 1.2  [INT26]            digital DERIVED
		   *   22 nm advanced  analog DERIVED 1.2 x 0.29/0.52 digital DERIVED
		   * digital, 3 nm = 0.33 - 0.23 = 0.10 [MS26]: a UCIe-LIKE digital
		   *   interface, not the UCIe D2D adapter itself.
		   * digital, 22 nm DERIVED (user ruling 2026-10-23: "use the most accurate
		   *   one, the other two as internal reference, calibrations, and guards")
		   *   = 0.10 x E22/E7, the PUBLISHED DIGITAL CHAIN:
		   *     E22/E32 = CACTI C_inv x Vdd^2, 22 nm / 32 nm tables (same device
		   *               type; pimid_io_min_inv_switch_cap), then
		   *     E32/E7  = [SB17] A. Stillmaker and B. Baas, "Scaling equations
		   *               for the accurate prediction of CMOS device performance
		   *               from 180 nm to 7 nm", Integration, the VLSI Journal,
		   *               vol. 58, pp. 74-81, 2017, Table 2 (FO4-chain inverter,
		   *               energy per transition): 32 nm High-k 0.97 V 0.51 fJ /
		   *               7 nm Multi-Gate 0.8 V 0.111 fJ ([SB17] has no 22 nm
		   *               row, hence the CACTI step at 32 nm).
		   *   It is a LOWER BOUND: the published chain stops at 7 nm and 3 nm
		   *   logic costs no more than 7 nm logic (the 7 -> 3 nm step is taken
		   *   as 1). Two INTERNAL REFERENCES are printed beside it, never applied
		   *   (user ruling 2026-10-24; the former TSMC-press reference was derived
		   *   from the applied value itself, i.e. circular, and is removed):
		   *     (a) the analog node ratio, 0.10 x 1.2 / 0.52 [INT26]/[CDN26]
		   *         = 0.231 pJ/bit;
		   *     (b) [SB17] Table 2's own 20 nm row (Multi-Gate, 0.9 V, 0.198 fJ),
		   *         the nearest published node to 22 nm, in place of the CACTI
		   *         step: 0.10 x 0.198 / 0.111 = 0.178 pJ/bit.
		   *   GUARD: a WARNING (not fatal) when the applied value falls outside
		   *   [min, max] of the two references.
		   * analog, 22 nm advanced DERIVED: Cadence's same-node AP/SP ratio
		   *   applied to Intel's 22 nm SP (caveats printed at runtime).
		   * NODE: link_node_nm (PIMID: power.link.process_node_nm); <= 0 = the
		   *   node of the die the link sits on, i.e. this McPAT run's node. Any
		   *   node other than 22 or 3 is REFUSED. A user phy_pj_per_bit (>= 0)
		   *   replaces the whole per-bit figure (analog + digital) and needs no
		   *   node row. */
		  ucie_user_pj = (XML->sys.pcie.phy_pj_per_bit >= 0.0);
		  ucie_node_nm = (XML->sys.pcie.link_node_nm > 0) ? XML->sys.pcie.link_node_nm
		                                                  : (int)(interface_ip.F_sz_nm + 0.5);
		  const char* pkg_s = (pkg == 1) ? "advanced" : "standard";
		  const double cdn_ap = 0.29, cdn_sp = 0.52;   /* [CDN26], 3 nm, 16 Gb/s */
		  const double int_sp = 1.2;                   /* [INT26], 22 nm, 48 Gb/s */
		  const double ms_total = 0.33, ms_phy = 0.23;  /* [MS26], 3 nm, 24 Gb/s, 0.75 V */
		  const double ms_dig = ms_total - ms_phy;
		  const double node_ratio = int_sp / cdn_sp;
		  if (ucie_user_pj) {
			  ucie_analog_pj  = XML->sys.pcie.phy_pj_per_bit;
			  ucie_digital_pj = 0.0;
			  pimid::debugStream(std::cerr) << "[link] UCIe link energy (McPAT PIMID 1.12.18): " << ucie_analog_pj
			            << " pJ/bit [user value: power.link.pj_per_bit_override] replaces the whole per-bit"
			               " figure (analog PHY + digital interface); no node row applied." << std::endl;
		  } else {
			  if (ucie_node_nm != 22 && ucie_node_nm != 3) {
				  std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): the UCIe link model supports exactly two"
				               " process nodes, 22 nm and 3 nm (published measurements: Intel ISSCC 2026 8.1 at"
				               " 22 nm; Cadence JSSC 2026 and Microsoft ISSCC 2026 8.3 at 3 nm); the link's node is "
				            << ucie_node_nm << " nm"
				            << ((XML->sys.pcie.link_node_nm > 0) ? " (link_node_nm)" : " (the die's node)")
				            << ". Set power.link.process_node_nm to 22 or 3, or give"
				               " power.link.pj_per_bit_override." << std::endl;
				  exit(2);
			  }
			  if (ucie_node_nm == 3) {
				  ucie_analog_pj  = (pkg == 1) ? cdn_ap : cdn_sp;
				  ucie_digital_pj = ms_dig;
				  pimid::debugStream(std::cerr) << "[link] UCIe 3 nm " << pkg_s << " package (McPAT PIMID 1.12.18): analog PHY "
				            << ucie_analog_pj << " pJ/bit MEASURED [Melek et al. (Cadence), JSSC 2026,"
				               " DOI 10.1109/JSSC.2026.3651425: " << (pkg == 1
				                   ? "\"297 mW of measured analog power ... 0.29 pJ/bit analog power efficiency\","
				                     " 64 lanes x 16 Gb/s, VDD 0.75 V, VDDH 1.2 V, VCCIO 0.45 V, PLL included]"
				                   : "\"analog power efficiency of 0.52 pJ/bit running at 16 Gb/s/pin for a 25 mm"
				                     " channel\", VDD 0.75 V, VDDH 1.2 V, PLL included]") << std::endl;
				  pimid::debugStream(std::cerr) << "[link] UCIe 3 nm digital interface: " << ucie_digital_pj << " pJ/bit MEASURED-DIFFERENCE"
				               " = 0.33 - 0.23 [Shivnaraine et al. (Microsoft), ISSCC 2026 8.3, DOI"
				               " 10.1109/ISSCC49663.2026.11409276: \"0.33pJ/b (0.23pJ/b PHY only) at 24Gb/s\", 0.75 V];"
				               " flow control, calibration, error correction, repair: a UCIe-LIKE digital interface,"
				               " not the UCIe D2D adapter itself." << std::endl;
			  } else {
				  ucie_analog_pj  = (pkg == 1) ? int_sp * cdn_ap / cdn_sp : int_sp;
				  /* PIMID 1.12.18: the published digital chain (rows comment above).
				   * set_pcie_param runs BEFORE the constructor's init_interface, so the
				   * global CACTI input (g_ip) is set from this component first --
				   * init_tech_params reads it; the constructor re-initialises after. */
				  init_interface(&interface_ip);
				  const TechnologyParameter tp_saved = g_tp;
				  /* PIMID (next): each node's minimum inverter at that node's OWN feature size. CACTI's
				   * TechnologyParameter::init sizes every device from g_ip->F_sz_um (min_w_nmos_ = 1.5 F,
				   * cell_h_def = 50 F; external/cacti/parameter.cc), not from the table it loads, so both
				   * inverters used to be drawn at the run's F and the 22/32 nm ratio lacked the 22/32 linear
				   * shrink (the defect logic.cc pimid_node_energy_scale had, fixed there before this). */
				  const double f_um_saved = g_ip->F_sz_um, f_nm_saved = g_ip->F_sz_nm;
				  g_ip->F_sz_um = 0.022; g_ip->F_sz_nm = 22;
				  init_tech_params(0.022, false);
				  const double c22 = pimid_io_min_inv_switch_cap(), v22 = g_tp.peri_global.Vdd;
				  g_ip->F_sz_um = 0.032; g_ip->F_sz_nm = 32;
				  init_tech_params(0.032, false);
				  const double c32 = pimid_io_min_inv_switch_cap(), v32 = g_tp.peri_global.Vdd;
				  g_ip->F_sz_um = f_um_saved; g_ip->F_sz_nm = f_nm_saved;
				  g_tp = tp_saved;   /* the run's tables, exactly as they were */
				  const double k_cacti = (c32 > 0 && v32 > 0) ? (c22 * v22 * v22) / (c32 * v32 * v32) : 0.0;
				  const double k_sb = 0.51 / 0.111;              /* [SB17] Table 2, 32 nm / 7 nm */
				  const double k_22_7 = k_cacti * k_sb;
				  if (!(k_22_7 > 0.0)) {
					  std::cerr << "[link] FATAL (McPAT PIMID 1.12.18): CACTI's 22/32 nm tables gave no switched"
					               " capacitance for the digital node chain." << std::endl;
					  exit(2);
				  }
				  ucie_digital_pj = ms_dig * k_22_7;
				  const double ref_analog = ms_dig * node_ratio;
				  const double ref_sb20 = ms_dig * (0.198 / 0.111);   /* [SB17] Table 2: 20 nm 0.198 fJ / 7 nm 0.111 fJ */
				  if (pkg == 1)
					  pimid::debugStream(std::cerr) << "[link] UCIe 22 nm advanced package (McPAT PIMID 1.12.18): analog PHY "
					            << ucie_analog_pj << " pJ/bit DERIVED = 1.2 pJ/b [Mondal et al. (Intel), ISSCC 2026"
					               " 8.1, DOI 10.1109/ISSCC49663.2026.11409120, 22nm FinFET standard package, 0.45 TX +"
					               " 0.69 RX + 0.06 PI+QH] x (0.29 / 0.52) [Melek et al. (Cadence), JSSC 2026: same-node"
					               " 3 nm advanced/standard package pair]. CAVEATS: the package ratio is Cadence's design"
					               " (16 Gb/s, PLL included, 25 mm SP channel) applied to Intel's (48 Gb/s, PLL NOT"
					               " integrated, 30 mm SP channel), across vendors; this link runs at "
					            << XML->sys.pcie.lane_rate_gts << " GT/s." << std::endl;
				  else
					  pimid::debugStream(std::cerr) << "[link] UCIe 22 nm standard package (McPAT PIMID 1.12.18): analog PHY "
					            << ucie_analog_pj << " pJ/bit MEASURED [Mondal et al. (Intel), ISSCC 2026 8.1, DOI"
					               " 10.1109/ISSCC49663.2026.11409120: \"1.2pJ/b (0.45, 0.69, and 0.06pJ/b in TX, RX, and"
					               " PI+QH ...; a PLL is not integrated in this work)\", 48 Gb/s, VDD 0.7 V, 30 mm];"
					               " this link runs at " << XML->sys.pcie.lane_rate_gts << " GT/s." << std::endl;
				  pimid::debugStream(std::cerr) << "[link] UCIe 22 nm digital interface: " << ucie_digital_pj << " pJ/bit DERIVED (APPLIED)"
				               " = 0.10 pJ/b [Microsoft ISSCC 2026 8.3, 3 nm: 0.33 - 0.23] x E22/E7 " << k_22_7
				            << " = CACTI C_inv x Vdd^2 22 nm (" << c22 * 1e15 << " fF, " << v22 << " V) / 32 nm ("
				            << c32 * 1e15 << " fF, " << v32 << " V) = " << k_cacti
				            << " x Stillmaker & Baas, Integration 58 (2017) 74-81, Table 2 FO4 inverter energy 32 nm"
				               " 0.51 fJ (0.97 V) / 7 nm 0.111 fJ (0.8 V) = " << k_sb
				            << ". LOWER BOUND: the published chain stops at 7 nm (7 -> 3 nm taken as 1)." << std::endl;
				  pimid::debugStream(std::cerr) << "[link] UCIe 22 nm digital interface, INTERNAL REFERENCE (a), not applied: analog node"
				               " ratio 0.10 x (1.2 / 0.52 = " << node_ratio << ") = " << ref_analog
				            << " pJ/bit [Intel ISSCC 2026 8.1 SP / Cadence JSSC 2026 SP]" << std::endl;
				  pimid::debugStream(std::cerr) << "[link] UCIe 22 nm digital interface, INTERNAL REFERENCE (b), not applied: Stillmaker & Baas,"
				               " Integration 58 (2017) 74-81, Table 2's own 20 nm row (Multi-Gate, 0.9 V, FO4 inverter 0.198 fJ),"
				               " the nearest published node to 22 nm: 0.10 x (0.198 / 0.111) = " << ref_sb20 << " pJ/bit" << std::endl;
				  const double ref_min = std::min(ref_analog, ref_sb20);
				  const double ref_max = std::max(ref_analog, ref_sb20);
				  if (ucie_digital_pj < ref_min || ucie_digital_pj > ref_max)
					  pimid::debugStream(std::cerr) << "[link] WARNING (McPAT PIMID 1.12.18, guard): applied 22 nm digital interface "
					            << ucie_digital_pj << " pJ/bit is OUTSIDE the internal references [" << ref_min
					            << ", " << ref_max << "] pJ/bit" << std::endl;
				  else
					  pimid::debugStream(std::cerr) << "[link] guard (McPAT PIMID 1.12.18): applied 22 nm digital interface "
					            << ucie_digital_pj << " pJ/bit lies within the internal references [" << ref_min
					            << ", " << ref_max << "] pJ/bit" << std::endl;
			  }
		  }
		  ucie_pj_per_bit = ucie_analog_pj + ucie_digital_pj;
		  pimid::debugStream(std::cerr) << "[link] UCIe link model (McPAT PIMID 1.12.18): " << pkg_s << " package, "
		            << XML->sys.pcie.lanes << " lanes x " << r << " GT/s, "
		            << (ucie_user_pj ? std::string("user value") : std::to_string(ucie_node_nm) + " nm row")
		            << ": analog PHY " << ucie_analog_pj << " + digital interface " << ucie_digital_pj
		            << " = " << ucie_pj_per_bit << " pJ/bit, charged once per crossing bit" << std::endl;
	  }

		if ( XML->sys.pcie.vdd>0)
		{
			interface_ip.specific_hp_vdd = true;
			interface_ip.specific_lop_vdd = true;
			interface_ip.specific_lstp_vdd = true;
			interface_ip.hp_Vdd   = XML->sys.pcie.vdd;
			interface_ip.lop_Vdd  = XML->sys.pcie.vdd;
			interface_ip.lstp_Vdd = XML->sys.pcie.vdd;
		}

		if ( XML->sys.pcie.power_gating_vcc > -1)
		{
			interface_ip.specific_vcc_min = true;
			interface_ip.user_defined_vcc_min   = XML->sys.pcie.power_gating_vcc;

		}
//	  pciep.executionTime   = XML->sys.total_cycles/(XML->sys.target_core_clockrate*1e6);

}

FlashController::FlashController(ParseXML *XML_interface,InputParameter* interface_ip_)
:XML(XML_interface),
 interface_ip(*interface_ip_)
 {

	  double frontend_area, phy_area, ctrl_area, SerDer_area;
      double ctrl_dyn, frontend_dyn, SerDer_dyn;
      double ctrl_gates,frontend_gates, SerDer_gates=0.;
	  double pmos_to_nmos_sizing_r = pmos_to_nmos_sz_ratio();
	  double NMOS_sizing, PMOS_sizing;

	  /* Assuming PCIe is bit-slice based architecture
	   * This is the reason for /8 in both area and power calculation
	   * to get per lane numbers
	   */

	  set_fc_param();
	  local_result = init_interface(&interface_ip);
	  if (fcp.type == 0) //high performance NIU
	  {
		  cout<<"Current McPAT does not support high performance flash contorller since even low power designs are enough for maintain throughput"<<endl;
		  exit(0);
		  NMOS_sizing 	  = 5*g_tp.min_w_nmos_;
		  PMOS_sizing	  = 5*g_tp.min_w_nmos_*pmos_to_nmos_sizing_r;
	  }
	  else
	  {
		  ctrl_area   = 0.243 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
		  //Area estimation based on Cadence ChipEstimate @ 65nm: NANDFLASH-CTRL from CAST
     	  SerDer_area = 0.36/8 * (interface_ip.F_sz_um/0.065)* (interface_ip.F_sz_um/0.065);
     	  //based On PCIe PHY TSMC65GP from Cadence ChipEstimate @ 65nm, it support 8x lanes with each lane
     	  //speed up to 250MB/s (PCIe1.1x) This is already saturate the 200MB/s of the flash controller core above.
		  ctrl_gates      = 129267;
		  SerDer_gates    = 200000/8;
		  NMOS_sizing 	  = g_tp.min_w_nmos_;
		  PMOS_sizing	  = g_tp.min_w_nmos_*pmos_to_nmos_sizing_r;

		  //Power
		  //Cadence ChipEstimate using 65nm the controller 125mW for every 200MB/s This is power not energy!
		  ctrl_dyn      = 0.125*g_tp.peri_global.Vdd/1.1*g_tp.peri_global.Vdd/1.1*(interface_ip.F_sz_nm/65.0);
		  //SerDer_dyn is power not energy, scaling from 10mw/Gb/s @90nm
		  SerDer_dyn   = 0.01*1.6*(interface_ip.F_sz_um/0.09)*g_tp.peri_global.Vdd/1.2*g_tp.peri_global.Vdd/1.2;
		  //max  Per controller speed is 1.6Gb/s (200MB/s)
	  }
	  double number_channel = 1+(fcp.num_channels-1)*0.2;
	  area.set_area((ctrl_area + (fcp.withPHY? SerDer_area:0))*1e6*number_channel);
	  power_t.readOp.dynamic = (ctrl_dyn + (fcp.withPHY? SerDer_dyn:0))*number_channel;
	  power_t.readOp.leakage = ((ctrl_gates + (fcp.withPHY? SerDer_gates:0))*number_channel)*cmos_Isub_leakage(NMOS_sizing, PMOS_sizing, 2, nand)*g_tp.peri_global.Vdd;//unit W
	  double long_channel_device_reduction = longer_channel_device_reduction(Uncore_device);
	  power_t.readOp.longer_channel_leakage = power_t.readOp.leakage * long_channel_device_reduction;
	  double pg_reduction = power_gating_leakage_reduction(false);//array structure all retain state;
	  power_t.readOp.power_gated_leakage = power_t.readOp.leakage * pg_reduction;
	  power_t.readOp.power_gated_with_long_channel_leakage = power_t.readOp.power_gated_leakage * long_channel_device_reduction;
	  power_t.readOp.gate_leakage = ((ctrl_gates + (fcp.withPHY? SerDer_gates:0))*number_channel)*cmos_Ig_leakage(NMOS_sizing, PMOS_sizing, 2, nand)*g_tp.peri_global.Vdd;//unit W
 }

void FlashController::computeEnergy(bool is_tdp)
{
	if (is_tdp)
    {


		power	= power_t;
    	power.readOp.dynamic *= fcp.duty_cycle;

    }
    else
    {
    	rt_power = power_t;
    	rt_power.readOp.dynamic *= fcp.perc_load;
    }
}

void FlashController::displayEnergy(uint32_t indent,int plevel,bool is_tdp)
{
	string indent_str(indent, ' ');
	string indent_str_next(indent+2, ' ');
	bool long_channel = XML->sys.longer_channel_device;
	bool power_gating = XML->sys.power_gating;

	if (is_tdp)
	{
		cout << "Flash Controller:" << endl;
		cout << indent_str<< "Area = " << area.get_area()*1e-6<< " mm^2" << endl;
		cout << indent_str << "Peak Dynamic = " << power.readOp.dynamic << " W" << endl;//no multiply of clock since this is power already
		cout << indent_str<< "Subthreshold Leakage = "
			<< (long_channel? power.readOp.longer_channel_leakage:power.readOp.leakage) <<" W" << endl;
		if (power_gating) cout << indent_str << "Subthreshold Leakage with power gating = "
				<< (long_channel? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)  << " W" << endl;
		cout << indent_str<< "Gate Leakage = " << power.readOp.gate_leakage << " W" << endl;
		cout << indent_str << "Runtime Dynamic = " << rt_power.readOp.dynamic << " W" << endl;
		cout<<endl;
	}
	else
	{

	}

}

void FlashController::set_fc_param()
{
//	  fcp.clockRate       = XML->sys.flashc.mc_clock;
//	  fcp.clockRate       *= 1e6;
	  fcp.peakDataTransferRate = XML->sys.flashc.peak_transfer_rate;
	  fcp.num_channels    = ceil(fcp.peakDataTransferRate/200);
	  fcp.num_mcs         = XML->sys.flashc.number_mcs;
	  fcp.duty_cycle      = XML->sys.flashc.duty_cycle;
	  fcp.perc_load       = XML->sys.flashc.total_load_perc;
	  fcp.type            = XML->sys.flashc.type;
	  fcp.withPHY         = XML->sys.flashc.withPHY;
//	  flashcp.executionTime   = XML->sys.total_cycles/(XML->sys.target_core_clockrate*1e6);
	  if ( XML->sys.flashc.vdd>0)
	  {
		  interface_ip.specific_hp_vdd = true;
		  interface_ip.specific_lop_vdd = true;
		  interface_ip.specific_lstp_vdd = true;
		  interface_ip.hp_Vdd   = XML->sys.flashc.vdd;
		  interface_ip.lop_Vdd  = XML->sys.flashc.vdd;
		  interface_ip.lstp_Vdd = XML->sys.flashc.vdd;
	  }
		if ( XML->sys.flashc.power_gating_vcc > -1)
		{
			interface_ip.specific_vcc_min = true;
			interface_ip.user_defined_vcc_min   = XML->sys.flashc.power_gating_vcc;

		}

}
