#ifndef _extio_H_
#define _extio_H_
#include "parameter.h"
#include "component.h"
#include "extio_technology.h"

class Extio : public Component
{
public: 
  
  Extio(IOTechParam *); 
    	
	void extio_area(); 
	void extio_eye(); 
	void extio_power_dynamic(); 
	void extio_power_phy(); 
	void extio_power_term(); 

	/* PIMID 1.11.40: accessors. Upstream CACTI-IO computes these and prints
	 * them, keeping every result private -- usable as a standalone report,
	 * unusable as a library. PIMID harnesses this model for interconnect
	 * power and area, so the results have to be readable. Compute-then-read:
	 * call the extio_*() methods first. */
	double getIOAreaMM2()           const { return io_area; }
	double getIOPowerTermMW()       const { return io_power_term; }
	double getIOPowerDynamicMW()    const { return io_power_dynamic; }
	double getPHYPowerMW()          const { return phy_power; }
	double getPHYStaticPowerMW()    const { return phy_static_power; }
	double getPHYDynamicPowerMW()   const { return phy_dynamic_power; }
	double getTimingMarginUI()      const { return io_tmargin; }
	double getVoltageMarginV()      const { return io_vmargin; }

	/* PIMID (user ruling 2026-10-25, measured termination duty): the
	 * fraction of the time a DATA pin is driven LOW in the DDR4 (POD)
	 * termination term. Upstream writes 0.5 into the formula; the DQS
	 * strobes keep 0.5 (they toggle every UI whatever the data). At the
	 * default 0.5 the term is bit-identical to upstream. */
	double dq_data_low_duty = 0.5;

private:
  IOTechParam *io_param;  

  double io_area;

  double io_power_term;
  double power_termination_write;
  double power_termination_read;
  double power_bias;
  double power_clk_bias;

  double phy_power;
  double phy_wtime;
  double phy_static_power;
  double phy_dynamic_power;

  double io_power_dynamic;

  double power_dq_write, power_dqs_write, power_ca_write,
         power_dq_read, power_dqs_read, power_ca_read, 
         power_clk;

  double io_tmargin, io_vmargin;
 
}; 
 
 
#endif  // _extio_H_ 
