#include "dram/dram.h"
#include "dram/lambdas.h"

namespace Ramulator {

class HBM3 : public IDRAM, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAM, HBM3, "HBM3", "HBM3 Device Model")

  public:
  /* PIMID 1.9.10: datasheet IDD/VDD presets (JESD238 HBM3) -- relocated energy layer.
     Intensive per-access model lives in dram/pimid_energy.h (active source); these
     mirror it in upstream convention for Ramulator2's command-driven power path. */
    inline static const std::map<std::string, std::vector<double>> voltage_presets = {
      {"Default",     {1.1,  1.8}},
    };
    inline static const std::map<std::string, std::vector<double>> current_presets = {
      // name         IDD0 IDD2N IDD3N IDD4R IDD4W IDD5B  IPP0 IPP2N IPP3N IPP4R IPP4W IPP5B
      {"Default",     {30,18,22,90,100,70, 3,3,3,3,3,40}},
    };

    inline static const std::map<std::string, Organization> org_presets = {
      //   name     density   DQ    Ch Pch  Bg Ba   Ro     Co
      {"HBM3_2Gb",   {2<<10,  128,  {1, 2,  4,  4, 1<<13, 1<<6}}},
      {"HBM3_4Gb",   {4<<10,  128,  {1, 2,  4,  4, 1<<14, 1<<6}}},
      {"HBM3_8Gb",   {8<<10,  128,  {1, 2,  4,  4, 1<<15, 1<<6}}},  // density label fix (6->8 Gb); banks 32 = JEDEC HBM3
    };

    inline static const std::map<std::string, std::vector<int>> timing_presets = {
      //   name       rate   nBL  nCL  nRCDRD  nRCDWR  nRP  nRAS  nRC  nWR  nRTPS  nRTPL  nCWL  nCCDS  nCCDL  nRRDS  nRRDL  nWTRS  nWTRL  nRTW  nFAW  nRFC  nRFCSB  nREFI  nREFISB  nRREFD  tCK_ps
      // HBM3 @ 6.4 Gb/s (JESD238 launch rate) with JEDEC spec-minimum physical
      // timings (tRCD=tRP=16ns, tRAS=33ns, tRC=49ns, tCL=16ns, tWR=16ns,
      // tFAW=16ns, tREFI=3.9us).
      /* 1.11.63 (calibration): "Cycle counts at tCK=312ps" -- the line that
       * used to close the sentence above -- is GONE. 312 ps is tWDQS, not tCK;
       * the cycle counts are now at tCK = 625 ps, the CK period JESD238B.01
       * Table 92 (printed p.160) gives for the 6.4 Gbps/pin bin. See the long
       * note on the preset row below. */
      // Replaces the earlier placeholder "HBM3_2Gbps" preset, which used the
      // wrong data rate (2.0 vs 6.4 Gb/s) and impossible ~7ns row timings.
      /* PIMID 1.11.59 (audit F036): the nRFC/nRFCSB/nREFISB columns are -1,
       * because get_timing_vals() overwrites all three from the per-density
       * tables further down before anything reads them. This line used to
       * carry nRFC=706 with a comment claiming "tRFC=220ns @4Gb" -- a value
       * and a source that no run ever used: the table supplies 260 ns at the
       * emitted HBM3_4Gb org, so the refresh occupancy this model simulates
       * is 18% longer than the preset's own comment stated. The number was
       * dead and the comment beside it read as the authority, which is the
       * worse half. -1 is this file's idiom for "supplied elsewhere" and the
       * completeness check below still catches a column nothing fills. */
      /* PIMID 1.11.63 (calibration): THE WHOLE ROW IS RE-DERIVED IN THE CK
       * DOMAIN. This is a FIRST-ORDER change: it uncaps HBM3 bandwidth.
       *
       * THE ERROR. The row ran at tCK = 312 ps, which is tWDQS -- the write
       * strobe period -- not the command clock.
       * SOURCE: JESD238B.01 Table 92 "Timings Parameters (Part 1)", clause 10
       * "AC Timings", printed p.160 (PDF p.174), the 6.4 Gbps/pin column,
       * verbatim:
       *     CK clock frequency   fCK     50 - 1600   MHz
       *     CK clock period      tCK     0.625 - 20  ns
       *     WDQS clock period    tWDQS   0.312 - 10  ns
       * so at 6.4 Gb/s the command clock is at most 1600 MHz and its period at
       * least 0.625 ns; 0.312 ns is the strobe. The model was running the CK
       * domain at 2x the standard's maximum, and a cycle in the model was half
       * a JESD238B cycle. The tCK derivation in set_timing_vals() is corrected
       * to 4e6/rate (HBM3 moves 4 bits/pin per CK: 6.4 Gb/s / 1.6 GHz).
       *
       * WHAT MOVES, AND WHY IT IS FIRST-ORDER. Everything JESD238B states in
       * CYCLES was being read at the wrong cycle length. The worst was nCCDS.
       * Table 93, printed p.164 (PDF p.178), verbatim:
       *     "RD/WR bank A to RD/WR bank B command delay different bank group |
       *      tCCDS | 2 | - | nCK"
       *     "RD/WR bank A to RD/WR bank B command delay same bank group |
       *      tCCDL | Max (4, 2.5 ns/tCK) | - | nCK"
       * At tCK 0.625 ns those are 1.25 ns and Max(4, 4) = 4 nCK = 2.5 ns. A
       * BL8 burst at 6.4 Gb/s occupies 8 bits/pin / 6.4 Gb/s = 1.25 ns, so
       * tCCDS = 2 nCK is exactly back-to-back at 100% DQ utilisation. The old
       * row had nBL 4 and nCCDS 7, i.e. it left the bus idle 3 cycles in every
       * 7 and CAPPED each pseudochannel at nBL/nCCDS = 4/7 = 57% of the DQ
       * line rate. Peak modelled HBM3 bandwidth therefore rises by 1.75x, to
       * the full line rate, before any workload effect.
       *   nRREFD 8 -> 13. Table 93 printed p.166 (PDF p.180): "PER BANK
       *     REFRESH command period (different bank) ... | tRREFD |
       *     MAX(3 x tCK, 8) | - | ns". The standard states NANOSECONDS and the
       *     preset was carrying the figure as CYCLES. MAX(1.875, 8) = 8 ns =
       *     13 nCK at 0.625 ns.
       *   nREFI 12500 -> 6240. Table 93 printed p.166: "Average periodic
       *     refresh interval for REFRESH command | tREFI | - | 3.9 | us"
       *     (and Table 4 printed p.6 "Refresh Period 3.9 us" at every
       *     density). 3900 / 0.625 = 6240 nCK. Same 3.9 us, new cycle length.
       *
       * THE REST OF THE ROW: a pure unit conversion, ns preserved. Table 93
       * publishes tRC, tRAS, tRCDRD, tRCDWR, tRRDL, tRRDS, tFAW, tRTP, tRP,
       * tWR, tWTRL, tWTRS and tRTW with the MIN cell BLANK, and HBM3 has no
       * "CL" at all (RL is MR2 OP[7:0], vendor-selected), so the standard
       * cannot supply them and none is invented. They keep the NANOSECONDS
       * this row was built from -- the header comment above names them:
       * tRCD = tRP = tCL = tWR = tFAW = 16 ns, tRAS = 33 ns, tRC = 49 ns --
       * re-expressed at 0.625 ns instead of 0.3125 ns:
       *     nCL, nRCDRD, nRCDWR, nRP, nWR, nFAW  16 ns  -> ceil(16/0.625)  = 26
       *     nRAS                                 33 ns  -> ceil(33/0.625)  = 53
       *     nRC                                  49 ns  -> ceil(49/0.625)  = 79
       *     nBL   BL8 at 6.4 Gb/s                1.25ns ->      1.25/0.625 = 2
       *     nRTPS 13 x 0.3125 = 4.0625 ns               -> ceil(4.0625/.625)= 7
       *     nRTPL, nCWL, nWTRL, nRTW  26 x 0.3125 = 8.125 ns        -> 13
       *     nRRDS 10 x 0.3125 = 3.125 ns                            ->  5
       *     nRRDL, nWTRS      13 x 0.3125 = 4.0625 ns               ->  7
       * Identities hold afterwards: nRC 79 = nRAS 53 + nRP 26; nCCDS 2 >= nBL
       * 2; nCCDL 4 > nCCDS 2; nWTRL 13 > nWTRS 7; nRRDL 7 > nRRDS 5.
       * nRFC/nRFCSB/nREFISB stay -1 (supplied by the per-density tables).
       *
       * 1.11.64 -- VENDOR SILICON CONFIRMS THE TWO LOAD-BEARING CLAIMS ABOVE,
       * from two independent vendors (papers now in misc/):
       *  (a) THE CK DOMAIN, tCK = data_rate / 4. This release-1.11.63 fix was
       *      derived from JESD238B.01 Table 92 and is now stated outright in
       *      silicon papers by both HBM3 makers:
       *        Ryu et al., Samsung, JSSC 58(4) Apr 2023 p.1052 -- the 2 nCK
       *          GBUS window is "equivalent to 1 ns at 8 Gb/s/pin", i.e.
       *          tCK = 0.5 ns against an 8 Gb/s pin = rate/4.
       *        Park et al., SK hynix, JSSC 58(1) Jan 2023 p.259 -- "the data
       *          timing window as wide as 1tCK that corresponds to 571.4 ps
       *          for the 7-Gb/s operation", i.e. 4/7 ns = rate/4.
       *      (Much of the open-source community models HBM3 CK at rate/2,
       *      which these two measurements rule out.)
       *  (b) nCCDS = 2 and nCCDL = 4, EXACTLY the values in the row below:
       *      Ryu p.1052 gives tCCDS = 2 nCK "to any bank group" and
       *      tCCDL = 4 nCK "to the same bank group ... determined by
       *      considering core cycle time and OD-ECC computation time".
       *      At 8 Gb/s/pin those are 1 ns and 2 ns. First vendor
       *      corroboration of the 1.11.63 CK-domain re-derivation.
       *  Structural confirmation of the family split, from the ISCA 2025
       *  tutorial slide 56 (Woo/Elsasser, Rambus): tCCD_L is ~5 ns where the
       *  IO sense amp sits at the end of the bank (DDR/LPDDR) and ~2.5 ns
       *  where it sits mid-bank (GDDR/HBM) -- the physical reason HBM's
       *  column-to-column spacing is half the DDR figure. */
      {"HBM3_6.4Gbps", {6400, 2, 26, 26, 26, 26, 53, 79, 26, 7, 13, 13, 2, 4, 5, 7, 7, 13, 13, 26, -1, -1, 6240, -1, 13, 625}},
    };


  /************************************************
   *                Organization
   ***********************************************/   
    const int m_internal_prefetch_size = 2;

    inline static constexpr ImplDef m_levels = {
      "channel", "pseudochannel", "bankgroup", "bank", "row", "column",    
    };


  /************************************************
   *             Requests & Commands
   ***********************************************/
    inline static constexpr ImplDef m_commands = {
      "ACT", 
      "PRE", "PREA",
      "RD",  "WR",  "RDA",  "WRA",
      "REFab", "REFsb",
      "RFMab", "RFMsb"
    };

    inline static const ImplLUT m_command_scopes = LUT (
      m_commands, m_levels, {
        {"ACT",   "row"},
        {"PRE",   "bank"},    {"PREA",   "channel"},
        {"RD",    "column"},  {"WR",     "column"}, {"RDA",   "column"}, {"WRA",   "column"},
        {"REFab", "channel"}, {"REFsb",  "bank"},
        {"RFMab", "channel"}, {"RFMsb",  "bank"},
      }
    );

    inline static const ImplLUT m_command_meta = LUT<DRAMCommandMeta> (
      m_commands, {
                // open?   close?   access?  refresh?
        {"ACT",   {true,   false,   false,   false}},
        {"PRE",   {false,  true,    false,   false}},
        {"PREA",  {false,  true,    false,   false}},
        {"RD",    {false,  false,   true,    false}},
        {"WR",    {false,  false,   true,    false}},
        {"RDA",   {false,  true,    true,    false}},
        {"WRA",   {false,  true,    true,    false}},
        {"REFab", {false,  false,   false,   true }},
        {"REFsb", {false,  false,   false,   true }},
        {"RFMab", {false,  false,   false,   true }},
        {"RFMsb", {false,  false,   false,   true }},
      }
    );

    inline static constexpr ImplDef m_requests = {
      "read", "write", "all-bank-refresh", "per-bank-refresh", "all-bank-rfm", "per-bank-rfm"
    };

    inline static const ImplLUT m_request_translations = LUT (
      m_requests, m_commands, {
        {"read", "RD"}, {"write", "WR"}, {"all-bank-refresh", "REFab"}, {"per-bank-refresh", "REFsb"}, 
        {"all-bank-rfm", "RFMab"}, {"per-bank-rfm", "RFMsb"}, 
      }
    );

   
  /************************************************
   *                   Timing
   ***********************************************/
    inline static constexpr ImplDef m_timings = {
      "rate", 
      "nBL", "nCL", "nRCDRD", "nRCDWR", "nRP", "nRAS", "nRC", "nWR", "nRTPS", "nRTPL", "nCWL",
      "nCCDS", "nCCDL",
      "nRRDS", "nRRDL",
      "nWTRS", "nWTRL",
      "nRTW",
      "nFAW",
      "nRFC", "nRFCSB", "nREFI", "nREFISB", "nRREFD",
      "tCK_ps"
    };


  /************************************************
   *                 Node States
   ***********************************************/
    inline static constexpr ImplDef m_states = {
       "Opened", "Closed", "N/A", "Refreshing"
    };

    inline static const ImplLUT m_init_states = LUT (
      m_levels, m_states, {
        {"channel",       "N/A"}, 
        {"pseudochannel", "N/A"}, 
        {"bankgroup",     "N/A"},
        {"bank",          "Closed"},
        {"row",           "Closed"},
        {"column",        "N/A"},
      }
    );

  public:
    struct Node : public DRAMNodeBase<HBM3> {
      Node(HBM3* dram, Node* parent, int level, int id) : DRAMNodeBase<HBM3>(dram, parent, level, id) {};
    };
    std::vector<Node*> m_channels;
    
    FuncMatrix<ActionFunc_t<Node>>  m_actions;
    FuncMatrix<PreqFunc_t<Node>>    m_preqs;
    FuncMatrix<RowhitFunc_t<Node>>  m_rowhits;
    FuncMatrix<RowopenFunc_t<Node>> m_rowopens;


  public:
    void tick() override {
      m_clk++;
    };

    void init() override {
      RAMULATOR_DECLARE_SPECS();
      set_organization();
      set_timing_vals();

      set_actions();
      set_preqs();
      set_rowhits();
      set_rowopens();
      
      create_nodes();
    };

    void issue_command(int command, const AddrVec_t& addr_vec) override {
      int channel_id = addr_vec[m_levels["channel"]];
      m_channels[channel_id]->update_timing(command, addr_vec, m_clk);
      m_channels[channel_id]->update_states(command, addr_vec, m_clk);
    };

    int get_preq_command(int command, const AddrVec_t& addr_vec) override {
      int channel_id = addr_vec[m_levels["channel"]];
      return m_channels[channel_id]->get_preq_command(command, addr_vec, m_clk);
    };

    bool check_ready(int command, const AddrVec_t& addr_vec) override {
      int channel_id = addr_vec[m_levels["channel"]];
      return m_channels[channel_id]->check_ready(command, addr_vec, m_clk);
    };

    bool check_rowbuffer_hit(int command, const AddrVec_t& addr_vec) override {
      int channel_id = addr_vec[m_levels["channel"]];
      return m_channels[channel_id]->check_rowbuffer_hit(command, addr_vec, m_clk);
    };
    
    bool check_node_open(int command, const AddrVec_t& addr_vec) override {
      int channel_id = addr_vec[m_levels["channel"]];
      return m_channels[channel_id]->check_node_open(command, addr_vec, m_clk);
    };

  private:
    void set_organization() {
      // Channel width
      m_channel_width = param_group("org").param<int>("channel_width").default_val(64);

      // Organization
      m_organization.count.resize(m_levels.size(), -1);

      // Load organization preset if provided
      if (auto preset_name = param_group("org").param<std::string>("preset").optional()) {
        if (org_presets.count(*preset_name) > 0) {
          m_organization = org_presets.at(*preset_name);
        } else {
          throw ConfigurationError("Unrecognized organization preset \"{}\" in {}!", *preset_name, get_name());
        }
      }

      // Override the preset with any provided settings
      if (auto dq = param_group("org").param<int>("dq").optional()) {
        m_organization.dq = *dq;
      }

      for (int i = 0; i < m_levels.size(); i++){
        auto level_name = m_levels(i);
        if (auto sz = param_group("org").param<int>(level_name).optional()) {
          m_organization.count[i] = *sz;
        }
      }

      if (auto density = param_group("org").param<int>("density").optional()) {
        m_organization.density = *density;
      }

      // Sanity check: is the calculated channel density the same as the provided one?
      size_t _density = size_t(m_organization.count[m_levels["pseudochannel"]]) *
                        size_t(m_organization.count[m_levels["bankgroup"]]) *
                        size_t(m_organization.count[m_levels["bank"]]) *
                        size_t(m_organization.count[m_levels["row"]]) *
                        size_t(m_organization.count[m_levels["column"]]) *
                        size_t(m_organization.dq);
      _density >>= 20;
      if (m_organization.density != _density) {
        throw ConfigurationError(
            "Calculated {} channel density {} Mb does not equal the provided density {} Mb!", 
            get_name(),
            _density, 
            m_organization.density
        );
      }

    };

    void set_timing_vals() {
      m_timing_vals.resize(m_timings.size(), -1);

      // Load timing preset if provided
      bool preset_provided = false;
      if (auto preset_name = param_group("timing").param<std::string>("preset").optional()) {
        if (timing_presets.count(*preset_name) > 0) {
          m_timing_vals = timing_presets.at(*preset_name);
          preset_provided = true;
        } else {
          throw ConfigurationError("Unrecognized timing preset \"{}\" in {}!", *preset_name, get_name());
        }
      }

      // Check for rate (in MT/s), and if provided, calculate and set tCK (in picosecond)
      if (auto dq = param_group("timing").param<int>("rate").optional()) {
        if (preset_provided) {
          throw ConfigurationError("Cannot change the transfer rate of {} when using a speed preset !", get_name());
        }
        m_timing_vals("rate") = *dq;
      }
      /* 1.11.63 (calibration): ONE AUTHORITY FOR tCK, and for HBM3 the divisor
       * itself was wrong.
       *
       * The upstream form, `1E6 / (rate / 2)`, assumes a classic DDR bus whose
       * data rate is twice the COMMAND clock. HBM3 is not that.
       * SOURCE: JESD238B.01 Table 92, clause 10 "AC Timings", printed p.160
       * (PDF p.174), 6.4 Gbps/pin column: fCK 50-1600 MHz, tCK 0.625-20 ns,
       * tWDQS 0.312-10 ns. The data rate is FOUR times fCK (6.4 Gb/s at
       * 1.6 GHz), so
       *     tCK = 4e6 / rate = 625 ps at 6400 Mb/s
       * and the old form's 312 ps was tWDQS, the strobe period -- half a
       * JESD238B CK. Nine bins are tabulated (4.8 through 8.0 Gbps/pin) and
       * fCK is rate/4 in every one of them, so the relation is the standard's,
       * not a fit to one bin.
       *
       * LIVE EFFECT: tCK is the memory system's cycle length
       * (generic_DRAM_system::get_tCK()) and the divisor that turns the
       * per-density refresh tables (ns) into cycles. With the row re-derived
       * in the CK domain above, the model now runs HBM3's command bus at its
       * real 1.6 GHz and honours JESD238B's cycle-stated parameters -- tCCDS
       * = 2 nCK above all, which is what uncaps DQ utilisation from 57% to
       * 100%.
       *
       * The tCK_ps column in the preset row above is a MIRROR of this
       * derivation and is checked against it below, so the two cannot silently
       * disagree again. */
      int preset_tCK_ps = m_timing_vals("tCK_ps");
      int tCK_ps = 4E6 / m_timing_vals("rate");
      m_timing_vals("tCK_ps") = tCK_ps;
      if (preset_provided && preset_tCK_ps != tCK_ps) {
        throw ConfigurationError(
          "In \"{}\", the timing preset's tCK_ps column says {} ps but the "
          "rate of {} Mb/s derives {} ps (tCK = 4e6/rate: JESD238B.01 Table 92 "
          "printed p.160 gives fCK = rate/4, e.g. 1600 MHz at 6.4 Gbps/pin). "
          "The derivation wins at run time, so the column must mirror it -- "
          "fix the preset!",
          get_name(), preset_tCK_ps, m_timing_vals("rate"), tCK_ps);
      }

      // Refresh timings
      // tRFC table (unit is nanosecond!)
      constexpr int tRFC_TABLE[1][4] = {
      //  2Gb   4Gb   8Gb  16Gb
        { 160,  260,  350,  450},
      };
      /* 1.11.63 (calibration): tRFCpb table added. Same index (per-CHANNEL
       * density) as tRFC_TABLE above; -1 means "the standard does not give a
       * value here".
       * SOURCE: JESD238B.01 Table 93, printed p.166 (PDF p.180), verbatim:
       *     PER BANK REFRESH command period   8 Gb / die  tRFCpb  TBD  - ns 28
       *     (same bank)                      16 Gb / die          200  -
       *                                      24 Gb / die          240  -
       *                                      32 Gb / die          TBD  -
       * with NOTE 28 "Density is given per die" -- unlike tRFCab, which Table
       * 93 printed p.165 keys on density PER CHANNEL. The two must therefore
       * be bridged, and this model's org determines how: it carries no SID
       * level and 16 banks per pseudochannel, which is JESD238B's 4-High
       * realisation (the 8-High rows of Table 4 carry "SID, BA[3:0]"), so
       *     die density = 4 x channel density.
       * Only one column of the four lands on a tabulated value:
       *      2 Gb/channel ->  8 Gb/die  -> TBD
       *      4 Gb/channel -> 16 Gb/die  -> 200 ns   <- the density PIMID emits
       *      8 Gb/channel -> 32 Gb/die  -> TBD
       *     16 Gb/channel -> 64 Gb/die  -> not in Table 93 at all
       * so the other three keep the documented UPPER BOUND that 1.11.51 (N9)
       * put there -- tRFCab, since refreshing one bank cannot take longer than
       * refreshing them all -- and say so at the point of use below. */
      constexpr int tRFCPB_TABLE[1][4] = {
      //  2Gb   4Gb   8Gb  16Gb      (per-CHANNEL density; -1 = TBD in Table 93)
        {  -1,  200,   -1,   -1},
      };

      /* PIMID 1.11.59 (audit F034, HBM2's twin): this header said "tRFC
       * table" over the same-bank refresh INTERVAL table. */
      // tREFIsb table (same-bank refresh INTERVAL, unit is nanosecond!)
      /* 1.11.63 (calibration): the four constants {4875, 4875, 2438, 2438} are
       * GONE, replaced by the standard's own formula. They were 20x to 40x too
       * long, i.e. per-bank refresh was being issued 20-40x too rarely.
       * SOURCE: JESD238B.01 Table 93, printed p.166 (PDF p.180), verbatim:
       *     Average periodic refresh interval for   4-High  tREFIpb  -
       *     PER BANK REFRESH command                                 tREFI/16  us
       *                                             8-High  -        tREFI/32
       *                                            12-High  -        tREFI/48
       *                                            16-High  -        tREFI/64
       *     NOTE 24  tREFIPB = tREFI / N; N = no. of banks.
       * NOTE 24 is the general rule and the stack-height rows are that rule
       * evaluated for 16/32/48/64 banks. This model's org has no SID level and
       * carries bankgroup x bank banks per pseudochannel (16 in all three
       * shipped org presets), so N is read from the organization rather than
       * hard-coded, and the 4-High row is what 16 banks reproduces:
       *     tREFIpb = 3900 ns / 16 = 243.75 ns   (vs the old 4875)
       * tREFI itself is Table 93 printed p.166, "tREFI | - | 3.9 | us", and
       * Table 4 printed p.6 "Refresh Period 3.9 us" at every density -- it is
       * density-independent, which is why one expression replaces four
       * per-density constants.
       * These values are INERT on the configuration PIMID emits (the refresh
       * manager is AllBank, so REFsb is never issued), but they were wrong. */
      constexpr float tREFI_ns = 3900.0f;
      const int banks_per_pc = m_organization.count[m_levels["bankgroup"]] *
                               m_organization.count[m_levels["bank"]];
      const float tREFIsb_ns = tREFI_ns / (float) banks_per_pc;

      int density_id = [](int density_Mb) -> int {
        switch (density_Mb) {
          case 2048:  return 0;
          case 4096:  return 1;
          case 8192:  return 2;
          case 16384: return 3;
          default:    return -1;
        }
      }(m_organization.density);
      /* PIMID 1.11.59 (audit F037): the lambda returns -1 for any density
       * outside the four tabulated ones, and that -1 then indexed three
       * constexpr arrays. An out-of-bounds read one element BEFORE a table of
       * plausible nanosecond figures does not crash -- it yields whatever
       * adjacent constant the linker put there, and the run continues with a
       * refresh time nobody can trace. Every org preset this tree emits is
       * tabulated, so the read never happened; a hand-written org of 1 Gb or
       * 32 Gb would have reached it silently. Refuse at construction instead,
       * in the same form as the unrecognized-preset check above. */
      if (density_id < 0) {
        throw ConfigurationError(
          "In \"{}\", organization density {} Mb has no tabulated refresh "
          "timing (tRFC/tREFIsb are tabulated for 2/4/8/16 Gb only)!",
          get_name(), m_organization.density);
      }

      m_timing_vals("nRFC")  = JEDEC_rounding(tRFC_TABLE[0][density_id], tCK_ps);
      /* PIMID 1.11.51 (N9): two fixes to the refresh fill.
       * (1) nREFISB was assigned from tRFC_TABLE -- the refresh TIME table --
       *     while the tREFISB_TABLE (the same-bank refresh INTERVAL, defined
       *     just above and never used) sat unread: an upstream copy-paste
       *     that made same-bank refresh ~7x too frequent had anything
       *     consumed it. It now reads its own table.
       * (2) nRFCSB was never filled at all, so EVERY instantiation of this
       *     model died in the -1 completeness check below ("timing nRFCSB is
       *     not specified!") -- the HOST_MC SIGABRT of audit finding N9. No
       *     public per-density tRFCsb is available to us (JESD238 tables are
       *     paywalled), so it is filled with tRFC as a stated UPPER BOUND:
       *     refreshing one bank cannot take longer than refreshing them all.
       *     Our emitted configs use the AllBank refresh manager, which never
       *     consumes nRFCSB; a SameBank manager would over-price refresh
       *     time by the bound's slack -- conservative, and stated here.
       * 1.11.63 (calibration) SUPERSEDES half of (2): "JESD238 tables are
       *     paywalled" is no longer true of this tree -- misc/JESD238B.01.pdf
       *     is held, and its Table 93 printed p.166 DOES publish tRFCpb, at
       *     200 ns for a 16 Gb die (= the 4 Gb/channel org PIMID emits) and
       *     240 ns for 24 Gb, with 8 Gb and 32 Gb marked TBD. That value is
       *     now used where the standard supplies one; the tRFCab upper bound
       *     survives only for the columns the standard marks TBD. */
      m_timing_vals("nREFISB") = JEDEC_rounding(tREFIsb_ns, tCK_ps);
      /* PIMID 1.11.59 (audit F035): the `if (nRFCSB == -1)` this replaces was
       * unconditionally true -- the one shipped preset sets that column to -1
       * -- while nRFC and nREFISB immediately above were overwritten with no
       * guard at all. The guard therefore documented a preset-wins precedence
       * rule that the other two columns did not follow and that nothing ever
       * exercised. All three are unconditional now, and the preset carries -1
       * in all three columns to say so (F036). */
      /* 1.11.63 (calibration): nRFCSB is tRFCpb where JESD238B.01 Table 93
       * (printed p.166) gives one, and only falls back to the tRFCab upper
       * bound where the standard says TBD. At the density PIMID emits
       * (HBM3_4Gb = 4 Gb/channel = a 16 Gb die 4-High) that is 200 ns instead
       * of the 260 ns tRFCab bound -- the bound was 30% loose. */
      if (tRFCPB_TABLE[0][density_id] > 0) {
        m_timing_vals("nRFCSB") = JEDEC_rounding(tRFCPB_TABLE[0][density_id], tCK_ps);
      } else {
        m_timing_vals("nRFCSB") = JEDEC_rounding(tRFC_TABLE[0][density_id], tCK_ps);
      }

      // Overwrite timing parameters with any user-provided value
      // Rate and tCK should not be overwritten
      for (int i = 1; i < m_timings.size() - 1; i++) {
        auto timing_name = std::string(m_timings(i));

        if (auto provided_timing = param_group("timing").param<int>(timing_name).optional()) {
          // Check if the user specifies in the number of cycles (e.g., nRCD)
          m_timing_vals(i) = *provided_timing;
        } else if (auto provided_timing = param_group("timing").param<float>(timing_name.replace(0, 1, "t")).optional()) {
          // Check if the user specifies in nanoseconds (e.g., tRCD)
          m_timing_vals(i) = JEDEC_rounding(*provided_timing, tCK_ps);
        }
      }

      // Check if there is any uninitialized timings
      for (int i = 0; i < m_timing_vals.size(); i++) {
        if (m_timing_vals(i) == -1) {
          throw ConfigurationError("In \"{}\", timing {} is not specified!", get_name(), m_timings(i));
        }
      }      

      // Set read latency
      m_read_latency = m_timing_vals("nCL") + m_timing_vals("nBL");

      // Populate the timing constraints
      #define V(timing) (m_timing_vals(timing))
      populate_timingcons(this, {
          /*** Channel ***/ 
          /// 2-cycle ACT command (for row commands)
          {.level = "channel", .preceding = {"ACT"}, .following = {"ACT", "PRE", "PREA", "REFab", "REFsb", "RFMab", "RFMsb"}, .latency = 2},

          /*** Pseudo Channel (Table 3 -- Array Access Timings Counted Individually Per Pseudo Channel, JESD-235C) ***/ 
          // RAS <-> RAS
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRRDS")},
          /// 4-activation window restriction
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nFAW"), .window = 4},

          /// ACT actually happens on the 2-nd cycle of ACT, so +1 cycle to nRRD
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"REFsb", "RFMsb"}, .latency = V("nRRDS") + 1},
          /// nRREFD is the latency between REFsb <-> REFsb to *different* banks
          {.level = "pseudochannel", .preceding = {"REFsb", "RFMsb"}, .following = {"REFsb", "RFMsb"}, .latency = V("nRREFD")},
          /// nRREFD is the latency between REFsb <-> ACT to *different* banks. -1 as ACT happens on its 2nd cycle
          {.level = "pseudochannel", .preceding = {"REFsb", "RFMsb"}, .following = {"ACT"}, .latency = V("nRREFD") - 1},

          // CAS <-> CAS
          /// Data bus occupancy
          {.level = "pseudochannel", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA"}, .latency = V("nBL")},
          {.level = "pseudochannel", .preceding = {"WR", "WRA"}, .following = {"WR", "WRA"}, .latency = V("nBL")},

          // CAS <-> CAS
          /// nCCDS is the minimal latency for column commands 
          {.level = "pseudochannel", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA"}, .latency = V("nCCDS")},
          {.level = "pseudochannel", .preceding = {"WR", "WRA"}, .following = {"WR", "WRA"}, .latency = V("nCCDS")},
          /// RD <-> WR, Minimum Read to Write, Assuming tWPRE = 1 tCK                          
          {.level = "pseudochannel", .preceding = {"RD", "RDA"}, .following = {"WR", "WRA"}, .latency = V("nCL") + V("nBL") + 2 - V("nCWL")},
          /// WR <-> RD, Minimum Read after Write
          {.level = "pseudochannel", .preceding = {"WR", "WRA"}, .following = {"RD", "RDA"}, .latency = V("nCWL") + V("nBL") + V("nWTRS")},
          /// CAS <-> PREab
          {.level = "pseudochannel", .preceding = {"RD"}, .following = {"PREA"}, .latency = V("nRTPS")},
          {.level = "pseudochannel", .preceding = {"WR"}, .following = {"PREA"}, .latency = V("nCWL") + V("nBL") + V("nWR")},          
          /// RAS <-> RAS
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRRDS")},          
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nFAW"), .window = 4},          
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"PREA"}, .latency = V("nRAS")},          
          {.level = "pseudochannel", .preceding = {"PREA"}, .following = {"ACT"}, .latency = V("nRP")},          
          /// RAS <-> REF
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"REFab", "RFMab"}, .latency = V("nRC")},          
          {.level = "pseudochannel", .preceding = {"PRE", "PREA"}, .following = {"REFab", "RFMab"}, .latency = V("nRP")},          
          {.level = "pseudochannel", .preceding = {"RDA"}, .following = {"REFab", "RFMab"}, .latency = V("nRP") + V("nRTPS")},          
          {.level = "pseudochannel", .preceding = {"WRA"}, .following = {"REFab", "RFMab"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},          
          {.level = "pseudochannel", .preceding = {"REFab", "RFMab"}, .following = {"ACT", "REFsb", "RFMsb"}, .latency = V("nRFC")},          

          /*** Same Bank Group ***/ 
          /// CAS <-> CAS
          {.level = "bankgroup", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA"}, .latency = V("nCCDL")},          
          {.level = "bankgroup", .preceding = {"WR", "WRA"}, .following = {"WR", "WRA"}, .latency = V("nCCDL")},          
          {.level = "bankgroup", .preceding = {"WR", "WRA"}, .following = {"RD", "RDA"}, .latency = V("nCWL") + V("nBL") + V("nWTRL")},
          /// RAS <-> RAS
          {.level = "bankgroup", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRRDL")},  
          {.level = "bankgroup", .preceding = {"ACT"}, .following = {"REFsb", "RFMsb"}, .latency = V("nRRDL") + 1},  
          {.level = "bankgroup", .preceding = {"REFsb", "RFMsb"}, .following = {"ACT"}, .latency = V("nRRDL") - 1},  

          {.level = "bank", .preceding = {"RD"},  .following = {"PRE"}, .latency = V("nRTPS")},  


          /*** Bank ***/ 
          {.level = "bank", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRC")},  
          {.level = "bank", .preceding = {"ACT"}, .following = {"RD", "RDA"}, .latency = V("nRCDRD")},  
          {.level = "bank", .preceding = {"ACT"}, .following = {"WR", "WRA"}, .latency = V("nRCDWR")},  
          {.level = "bank", .preceding = {"ACT"}, .following = {"PRE"}, .latency = V("nRAS")},  
          {.level = "bank", .preceding = {"PRE"}, .following = {"ACT"}, .latency = V("nRP")},  
          {.level = "bank", .preceding = {"RD"},  .following = {"PRE"}, .latency = V("nRTPL")},  
          {.level = "bank", .preceding = {"WR"},  .following = {"PRE"}, .latency = V("nCWL") + V("nBL") + V("nWR")},  
          {.level = "bank", .preceding = {"RDA"}, .following = {"ACT", "REFsb", "RFMsb"}, .latency = V("nRTPL") + V("nRP")},  
          {.level = "bank", .preceding = {"WRA"}, .following = {"ACT", "REFsb", "RFMsb"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},  
        }
      );
      #undef V

    };

    void set_actions() {
      m_actions.resize(m_levels.size(), std::vector<ActionFunc_t<Node>>(m_commands.size()));

      // Channel Actions
      m_actions[m_levels["channel"]][m_commands["PREA"]] = Lambdas::Action::Channel::PREab<HBM3>;

      // Bank actions
      m_actions[m_levels["bank"]][m_commands["ACT"]] = Lambdas::Action::Bank::ACT<HBM3>;
      m_actions[m_levels["bank"]][m_commands["PRE"]] = Lambdas::Action::Bank::PRE<HBM3>;
      m_actions[m_levels["bank"]][m_commands["RDA"]] = Lambdas::Action::Bank::PRE<HBM3>;
      m_actions[m_levels["bank"]][m_commands["WRA"]] = Lambdas::Action::Bank::PRE<HBM3>;
    };

    void set_preqs() {
      m_preqs.resize(m_levels.size(), std::vector<PreqFunc_t<Node>>(m_commands.size()));

      // Channel Actions
      m_preqs[m_levels["channel"]][m_commands["REFab"]] = Lambdas::Preq::Channel::RequireAllBanksClosed<HBM3>;

      // Bank actions
      m_preqs[m_levels["bank"]][m_commands["REFsb"]] = Lambdas::Preq::Bank::RequireBankClosed<HBM3>;
      m_preqs[m_levels["bank"]][m_commands["RD"]] = Lambdas::Preq::Bank::RequireRowOpen<HBM3>;
      m_preqs[m_levels["bank"]][m_commands["WR"]] = Lambdas::Preq::Bank::RequireRowOpen<HBM3>;
    };

    void set_rowhits() {
      m_rowhits.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowhits[m_levels["bank"]][m_commands["RD"]] = Lambdas::RowHit::Bank::RDWR<HBM3>;
      m_rowhits[m_levels["bank"]][m_commands["WR"]] = Lambdas::RowHit::Bank::RDWR<HBM3>;
    }


    void set_rowopens() {
      m_rowopens.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowopens[m_levels["bank"]][m_commands["RD"]] = Lambdas::RowOpen::Bank::RDWR<HBM3>;
      m_rowopens[m_levels["bank"]][m_commands["WR"]] = Lambdas::RowOpen::Bank::RDWR<HBM3>;
    }


    void create_nodes() {
      int num_channels = m_organization.count[m_levels["channel"]];
      for (int i = 0; i < num_channels; i++) {
        Node* channel = new Node(this, nullptr, 0, i);
        m_channels.push_back(channel);
      }
    };
};


}        // namespace Ramulator