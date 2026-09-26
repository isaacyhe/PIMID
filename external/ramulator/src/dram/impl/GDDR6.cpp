#include "dram/dram.h"
#include "dram/lambdas.h"
#include "dram/pimid_bank_open.h"   // PIMID 1.11.91 (R8-7)

namespace Ramulator {

class GDDR6 : public IDRAM, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAM, GDDR6, "GDDR6", "GDDR6 Device Model")

  public:
  /* PIMID 1.9.10: datasheet IDD/VDD presets (Micron 8Gb GDDR6-14000 (no VPP)) -- relocated energy layer.
     Intensive per-access model lives in dram/pimid_energy.h (active source); these
     mirror it in upstream convention for Ramulator2's command-driven power path. */
    /* 1.11.63 (calibration): VPP 0.0 -> 1.8 V. JESD250D Table 62 "DC Operating
     * Conditions", clause 8.6, printed p.144 (PDF p.156): "Pump voltage VPP
     * 1.746 / 1.8 / 1.908 V", identical in the POD135 and POD125 columns. The
     * old 0.0 with its "no VPP" comment asserted GDDR6 has no pump rail; the
     * standard tabulates one. INERT in this tree: nothing outside this file
     * reads voltage_presets/current_presets (the intensive per-access model in
     * dram/pimid_energy.h is the active source), so this moves no simulated
     * number -- it removes a wrong number from the upstream-convention mirror. */
    inline static const std::map<std::string, std::vector<double>> voltage_presets = {
      {"Default",     {1.35,  1.8}},
    };
    inline static const std::map<std::string, std::vector<double>> current_presets = {
      // name         IDD0 IDD2N IDD3N IDD4R IDD4W IDD5B  IPP0 IPP2N IPP3N IPP4R IPP4W IPP5B
      {"Default",     {70,45,60,210,230,180, 3,3,3,3,3,48}},
    };

    inline static const std::map<std::string, Organization> org_presets = { //Table 19 for more info
      //    name         density   DQ   Ch Bg Ba   Ro     Co
      {"GDDR6_8Gb_x8",   {8<<10,    8,  {2, 4, 4, 1<<14, 1<<11}}},
      {"GDDR6_8Gb_x16",  {8<<10,    16, {2, 4, 4, 1<<14, 1<<10}}}, 
      {"GDDR6_16Gb_x8",  {16<<10,   8,  {2, 4, 4, 1<<15, 1<<11}}},
      {"GDDR6_16Gb_x16", {16<<10,   16, {2, 4, 4, 1<<14, 1<<11}}},
      {"GDDR6_32Gb_x8",  {32<<10,   8,  {2, 4, 4, 1<<16, 1<<11}}},
      {"GDDR6_32Gb_x16", {32<<10,   16, {2, 4, 4, 1<<15, 1<<11}}},
    };

    
    /* 1.11.63 (calibration): THREE fixes to this block, all in the trailing
     * columns and the clock.
     *
     * (a) COLUMN-ORDER MISMATCH between this comment header and m_timings.
     *     The header used to read "... nFAW nRFC nRFCpb nRREFD nREFI tCK_ps"
     *     while m_timings (below) reads "... nFAW nRFC nREFI nRREFD nRFCpb
     *     tCK_ps". The vector loads positionally against m_timings, so the run
     *     got nREFI=105 and nRFCpb=3333 while the header claimed nRFCpb=105
     *     and nREFI=3333. The header was the physically sensible reading, and
     *     nRFCpb is LIVE (REFpb -> ACT, see populate_timingcons below), so the
     *     model was blocking a bank for 3333 cycles after a per-bank refresh.
     *     The header now matches m_timings verbatim and the values are
     *     reordered to land under their own names.
     *
     * (b) nRFC and nREFI -> -1. Both are overwritten unconditionally from the
     *     per-density tRFC table and tREFI_BASE in set_timing_vals() before
     *     anything reads them, so the written 210/3333 were dead numbers that
     *     read as authority. -1 is this file family's idiom for "supplied
     *     elsewhere" (HBM2/HBM3, 1.11.59 F036) and the completeness check
     *     still catches a column nothing fills.
     *
     * (c) tCK_ps 570 -> 1000 and nCCDS 4 -> 8 on the two _double rows.
     *     tCK: set_timing_vals() derives tCK = 2e6/rate = 1000 ps at 2000 MT/s
     *     and overwrites this column, so the written 570 never applied; the
     *     column is now a checked mirror of the derivation (see the equality
     *     check in set_timing_vals).
     *     nCCDS: DERIVED-FROM-IDENTITY, not sourced. nCCDS 4 < nBL 8 -- a new
     *     BL16 burst cannot start 4 cycles after one that occupies 8 cycles of
     *     the same channel's DQ bus. JESD250D cl.1 (printed p.1 / PDF p.13)
     *     states the GDDR6 AC timings "were not standardized" and publishes
     *     Tables 71/73 with every MIN cell blank, and this tree holds no GDDR6
     *     vendor datasheet, so no bin value is available: nCCDS is raised to
     *     exactly nBL, the definitional floor, and no further. The _quad rows
     *     carry nBL 4 with nCCDS 4 and already satisfy the identity.
     *
     *     Everything else in these rows (rate, nCL, nRCD*, nRP, nRAS, nRC,
     *     nWR, nRTP, nCWL, nCCDL, nRRD*, nWTR*, nFAW, nRREFD, nRFCpb) stays as
     *     it is: UNCHECKABLE against JESD250D by the standard's own design. */
    inline static const std::map<std::string, std::vector<int>> timing_presets = {
      /* 1.11.66 (round 5, lane C -- CORRECTS A 1.11.63 REGRESSION). GDDR6
       * carries 8 bits/pin per CK (WCK = 4x CK, DDR on WCK -- JESD250D Table 1
       * p.5: CK 1.5 GHz <-> 12 Gbps; Samsung K4Z80325BC Table 91: tCK 0.57 ns
       * AT 14 Gbps), NOT the 2 bits/pin the 1.11.63 derivation assumed.
       * Upstream's tCK 570 ps was right, and the proof is arithmetic: every
       * cycle count in this row reproduces Samsung's 14 Gbps AC set at 0.57 ns
       * within 1-2% -- nRCDRD 26 = 14.82 (tRCDRD 15 ns), nRP 26 = 14.82 (15),
       * nRAS 53 = 30.2 (30), nRC 79 = 45.0 (45), nRCDWD 16 = 9.1 (9), nRFCpb
       * 105 = 59.9 (60), nREFI 3333 = 1.90 us (1.9) -- and reproduces NOTHING
       * at 1.000 ns. The 1.11.63 re-mirror to 1000 ps kept the counts and
       * changed the clock, turning the timing model into a 2 Gbps/pin part
       * (no such GDDR6 exists) with every row timing 1.75x slow; 1.11.65 then
       * derived the DQ turnaround from it (11.0 ns where 4tCK+4 = 6.27 is
       * the vendor figure).
       *
       * The row's "2000" was never a data rate; it named the CK class. The
       * rate column now carries the PIN rate PIMID prices (14000 MT/s, the
       * value CactiIOWrapper::dramRateMTs has held all along), tCK derives as
       * 8E6/rate = 571 ps, and the burst is re-expressed in the 8-bit domain:
       * BL16 on 16 DQ = 32 B per burst = 2 CK, so nBL 8 -> 2 and nCCDS 8 -> 2
       * (back-to-back bursts to different bank groups). Every other count is
       * UNCHANGED -- they were right, in the clock they were written for.
       * The _quad rows are not shipped by PIMID; they receive the same clock
       * correction so the file is self-consistent. */
      //       name                 rate   nBL  nCL  nRCDRD nRCDWD  nRP   nRAS  nRC   nWR  nRTP nCWL nCCDS nCCDL nRRDS nRRDL nWTRS nWTRL nFAW  nRFC nREFI nRREFD nRFCpb tCK_ps
      {"GDDR6_2000_1350mV_double", {14000,  2,  24,    26,     16,  26,   53,   79,   26,   4,   6,   2,    6,    7,    7,   9,    11,   28,    -1,   -1,   14,   105,   571}},
      {"GDDR6_2000_1250mV_double", {14000,  2,  24,    30,     19,  30,   60,   89,   30,   4,   6,   2,    6,   11,   11,   9,    11,   42,    -1,   -1,   21,   105,   571}},
      {"GDDR6_2000_1350mV_quad",   {14000,  2,  24,    26,     16,  26,   53,   79,   26,   4,   6,   2,    6,    7,    7,   9,    11,   28,    -1,   -1,   14,   105,   571}},
      {"GDDR6_2000_1250mV_quad",   {14000,  2,  24,    30,     19,  30,   60,   89,   30,   4,   6,   2,    6,   11,   11,   9,    11,   42,    -1,   -1,   21,   105,   571}},
    };


  /************************************************
   *                Organization
   ***********************************************/   
    const int m_internal_prefetch_size = 8;

    inline static constexpr ImplDef m_levels = {
      "channel", "bankgroup", "bank", "row", "column",    
    };


  /************************************************
   *             Requests & Commands
   ***********************************************/
    inline static constexpr ImplDef m_commands = { //figure 3
      "ACT", 
      "PREA", "PRE",
      "RD",  "WR",  "RDA",  "WRA",
      "REFab", "REFpb", "REFp2b",
    };

    inline static const ImplLUT m_command_scopes = LUT (
      m_commands, m_levels, {
        {"REFab", "channel"},  {"REFp2b",  "channel"},
        {"ACT",   "row"},
        {"PREA", "bank"},   {"PRE",  "bank"},  {"REFpb", "bank"},
        {"RD",    "column"}, {"WR",   "column"},  {"RDA",  "column"}, {"WRA",   "column"},
      }
    );

    inline static const ImplLUT m_command_meta = LUT<DRAMCommandMeta> (
      m_commands, {
                // open?   close?   access?  refresh?
        {"ACT",   {true,   false,   false,   false}},
        {"PREA",  {false,  true,    false,   false}},
        {"PRE",   {false,  true,    false,   false}},
        {"RD",    {false,  false,   true,    false}},
        {"WR",    {false,  false,   true,    false}},
        {"RDA",   {false,  true,    true,    false}},
        {"WRA",   {false,  true,    true,    false}},
        {"REFab", {false,  false,   false,   true }}, //double check
        {"REFpb", {false,  false,   false,   true }},
        {"REFp2b",{false,  false,   false,   true }},
      }
    );

    inline static constexpr ImplDef m_requests = {
      "read", "write", "all-bank-refresh", "PREsb",
    };

    inline static const ImplLUT m_request_translations = LUT (
      m_requests, m_commands, {
        {"read", "RD"}, {"write", "WR"}, {"all-bank-refresh", "REFab"}, {"PREsb", "PRE"}
      }
    );

   
  /************************************************
   *                   Timing
   ***********************************************/
  //delete nCS
    inline static constexpr ImplDef m_timings = {
      "rate", 
      "nBL", "nCL", "nRCDRD", "nRCDWD", "nRP", "nRAS", "nRC", "nWR", "nRTP", "nCWL",
      "nCCDS", "nCCDL",
      "nRRDS", "nRRDL",
      "nWTRS", "nWTRL",
      "nFAW",
      "nRFC","nREFI", "nRREFD", "nRFCpb",
      "tCK_ps"
    };


  /************************************************
   *                 Node States
   ***********************************************/
    inline static constexpr ImplDef m_states = {
       "Opened", "Closed", "PowerUp", "N/A", "Refreshing"
    };

    inline static const ImplLUT m_init_states = LUT (
      m_levels, m_states, {
        {"channel",   "N/A"}, 
        {"bankgroup", "N/A"},
        {"bank",      "Closed"},
        {"row",       "Closed"},
        {"column",    "N/A"},
      }
    );

  public:
    struct Node : public DRAMNodeBase<GDDR6> {
      Node(GDDR6* dram, Node* parent, int level, int id) : DRAMNodeBase<GDDR6>(dram, parent, level, id) {};
    };
    std::vector<Node*> m_channels;
    PimidBankOpenTracker<Node> m_pimid_bank_open;   // PIMID 1.11.91 (R8-7): measured bank-open fraction
    
    FuncMatrix<ActionFunc_t<Node>>  m_actions;
    FuncMatrix<PreqFunc_t<Node>>    m_preqs;
    FuncMatrix<RowhitFunc_t<Node>>  m_rowhits;
    FuncMatrix<RowopenFunc_t<Node>> m_rowopens;


  public:
    void tick() override {
      m_pimid_bank_open.tick(IDRAM::m_pimid_unit_cycles,
                             IDRAM::m_pimid_open_unit_cycles);   // PIMID 1.11.91 (R8-7)
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
      m_pimid_bank_open.init(m_channels, IDRAM::m_levels, IDRAM::m_states);   // PIMID 1.11.91 (R8-7)
      IDRAM::m_pimid_bank_open_tracked = m_pimid_bank_open.ok;
    };

    void issue_command(int command, const AddrVec_t& addr_vec) override {
      int channel_id = addr_vec[m_levels["channel"]];
      m_channels[channel_id]->update_timing(command, addr_vec, m_clk);
      m_channels[channel_id]->update_states(command, addr_vec, m_clk);
      m_pimid_bank_open.touch(addr_vec);   // PIMID 1.11.91 (R8-7)
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

      // Sanity check: is the calculated chip density the same as the provided one?
      size_t _density = size_t(m_organization.count[m_levels["channel"]]) *
                        size_t(m_organization.count[m_levels["bankgroup"]]) *
                        size_t(m_organization.count[m_levels["bank"]]) *
                        size_t(m_organization.count[m_levels["row"]]) *
                        size_t(m_organization.count[m_levels["column"]]) *
                        size_t(m_organization.dq);
      _density >>= 20;
      if (m_organization.density != _density) {
        throw ConfigurationError(
            "Calculated {} chip density {} Mb does not equal the provided density {} Mb!", 
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
      /* 1.11.63 (calibration, cross-cutting fix -- same block in all 11 impl
       * files): ONE AUTHORITY FOR tCK. This derivation is the authority; the
       * tCK_ps column in the preset rows above is a MIRROR of it and is now
       * checked against it, so the two cannot silently disagree again. The
       * audit found the written column dead in every model and contradicting
       * the derivation in two of them (LPDDR5 1250 vs 312, GDDR6 570 vs 1000).
       * The divisor was `1E6 / (rate / 2)`, whose integer division threw away
       * the half-MT/s of odd rates (DDR3-1333 came out 1501 ps instead of
       * 1500, DDR3/DDR4-2133 938 instead of 937).
       * 1.11.66: the sentence that stood here -- "GDDR6 is a DDR-clocked
       * interface, one data beat per CK edge, so 2 bits/pin per CK" -- was
       * WRONG, and the row comment above carries the proof. GDDR6's data
       * strobe WCK runs at 4x CK with DDR on WCK: 8 bits/pin per CK, so
       * tCK = 8e6/rate (Samsung Table 91: 0.57 ns at 14 Gbps; 8e6/14000 =
       * 571). This is the same family shape as LPDDR5 (8e6) and the
       * quarter-rate relation of HBM3 (4e6), not DDR's 2e6. */
      int preset_tCK_ps = m_timing_vals("tCK_ps");
      int tCK_ps = 8E6 / m_timing_vals("rate");
      m_timing_vals("tCK_ps") = tCK_ps;
      if (preset_provided && preset_tCK_ps != tCK_ps) {
        throw ConfigurationError(
          "In \"{}\", the timing preset's tCK_ps column says {} ps but the "
          "rate of {} MT/s derives {} ps (tCK = 8e6/rate). The derivation "
          "wins at run time, so the column must mirror it -- fix the preset!",
          get_name(), preset_tCK_ps, m_timing_vals("rate"), tCK_ps);
      }

      // Load the organization specific timings
      int dq_id = [](int dq) -> int {
        switch (dq) {
          case 8:  return 0;
          case 16: return 1;
          default: return -1;
        }
      }(m_organization.dq);

      /* 1.11.66: keyed on the PIN rate now that the rate column carries it
       * (14000, see the row comment); the old key 2000 would have returned
       * -1 and silently skipped the per-width nRRD/nFAW table below. */
      int rate_id = [](int rate) -> int { //should low voltage operation be added here?
        switch (rate) {
          case 14000: return 0;
          default:    return -1;
        }
      }(m_timing_vals("rate"));

      // Tables for secondary timings determined by the frequency, density, and DQ width.
      // Defined in the JEDEC standard (e.g., Table 169-170, JESD79-4C).

      //update these values
      constexpr int nRRDS_TABLE[2][1] = {
      // 2000
        { 4 },   // x8
        { 5 },   // x16
      };
      constexpr int nRRDL_TABLE[2][1] = {
      // 2000
        { 5 },  // x8
        { 6 },  // x16
      };
      constexpr int nFAW_TABLE[2][1] = {
      // 2000
        { 20 },  // x8
        { 28 },  // x16
      };

      if (dq_id != -1 && rate_id != -1) {
        m_timing_vals("nRRDS") = nRRDS_TABLE[dq_id][rate_id];
        m_timing_vals("nRRDL") = nRRDL_TABLE[dq_id][rate_id];
        m_timing_vals("nFAW")  = nFAW_TABLE [dq_id][rate_id];
      }

      // Refresh timings
      // tRFC table (unit is nanosecond!)
      /* 1.11.66 (round 5, lane C): the table this replaces was DDR4's tRFC1/
       * tRFC2/tRFC4 column set (260/360/550 at 4/8/16 Gb -- JESD79-4's
       * numbers), inherited with the rest of this file and never a GDDR6
       * value. Both held vendor sheets give the all-bank refresh cycle time
       * directly, and they agree:
       *   Samsung K4Z80325BC (8 Gb) Table 92 PDF p.158: tRFCab 120 ns
       *   SK hynix H56G42A (16 Gb) Table 67 PDF p.157:  tRFCab 120 ns
       * -- 120 ns at BOTH densities (GDDR6 refreshes per bank group in
       * parallel; the all-bank figure does not grow with density the way a
       * DDR4 chip's does). The per-bank tRFCpb is 60 ns in both, which the
       * row's nRFCpb 105 x 0.571 = 60 ns already carries. JESD250D itself
       * leaves the AC cell blank (vendor-specified), so the vendor sheets
       * are the authority. With tREFI 1900 ns the refresh occupancy goes
       * 360/1900 = 18.9% -> 120/1900 = 6.3%, which is what a 14 Gb/s GDDR6
       * channel actually loses to refresh. GDDR6 has no fine-granularity
       * refresh modes (no tRFC2/tRFC4 exist in JESD250D), so the table is
       * one row; the 4 Gb column has no shipped part behind it and carries
       * the same 120 with that stated. */
      constexpr int tRFC_TABLE[1][3] = {
      //  4Gb   8Gb  16Gb
        { 120,  120,  120}, // tRFCab: Samsung Tbl 92 / SK hynix Tbl 67, 120 ns at 8 and 16 Gb
      };

      // tREFI(base) table (unit is nanosecond!)
      /* 1.11.63 (calibration): tREFI_BASE 7800 -> 1900 ns.
       * SOURCE, two independent places in the held standard:
       *   JESD250D Table 73 "AC Timings", printed p.162 (PDF p.174):
       *     "Average periodic refresh interval with REFab command | tREFI |
       *      - | 1.9 | us".
       *   JESD250D Table 19 "Addressing Scheme", printed p.18 (PDF p.30):
       *     "Refresh 16K/32 ms", "Refresh period 1.9 us" -- stated identically
       *     for all five densities (8/12/16/24/32 Gb).
       *   JESD250D clause 7.15, printed p.116 (PDF p.128): "The device
       *     requires REFab cycles at an average periodic interval of
       *     tREFI(max). The values of tREFI for different densities are
       *     listed in Table 19."
       * 7800 ns is DDR3/DDR4's 7.8 us and was inherited with the rest of this
       * file's DDR4-derived refresh block; GDDR6 refreshes 4.1x more often.
       * This also RECONCILES the timing layer with the tree's own energy layer,
       * which already carries trefi 1900.0 (dram/pimid_energy.h GDDR6 row) --
       * the two disagreed by 4.1x and the timing side was the wrong one.
       * FIRST-ORDER on simulated bandwidth: at the derived tCK of 1000 ps and
       * the 8 Gb tRFC of 360 ns, refresh occupancy goes 360/7800 = 4.6% ->
       * 360/1900 = 18.9% of the channel. */
      constexpr int tREFI_BASE = 1900;
      int density_id = [](int density_Mb) -> int {
        switch (density_Mb) {
          case 4096:  return 0;
          case 8192:  return 1;
          case 16384: return 2;
          default:    return -1;
        }
      }(m_organization.density);
      /* PIMID 1.11.59 (audit F037-class): the lambda returns -1 for any
       * density outside the three tabulated ones, and that -1 then indexed
       * tRFC_TABLE. An out-of-bounds read one element BEFORE a table of
       * plausible nanosecond figures does not crash -- it yields whatever
       * adjacent constant the linker put there, and the run continues with a
       * refresh time nobody can trace, which is why an unrecognised density
       * silently produced a plausible-looking timing set instead of an error.
       *
       * BEHAVIOUR CHANGE, stated deliberately: unlike the DDR3/DDR4/DDR5/HBM
       * models, this file's org presets are NOT all covered by the switch,
       * and the mismatch runs both ways. The six shipped presets are 8/16/32
       * Gb (GDDR6_8Gb_x8/x16, GDDR6_16Gb_x8/x16, GDDR6_32Gb_x8/x16); the
       * switch handles 4/8/16 Gb. So the 4 Gb column is dead (no preset
       * selects it) and the two 32 Gb presets hit the [-1] read on every
       * instantiation -- those two are what this throw now refuses. The
       * alternative -- adding a 32 Gb column -- cannot be done from any
       * source this tree holds: JESD250D (misc/JESD250D.pdf) leaves tRFCab
       * and tRFCpb as vendor-specific, printing "-" for both minimums in its
       * AC timing table, so there is no JEDEC per-density GDDR6 refresh row
       * to copy. (The three columns present here carry DDR4's tRFC1/tRFC2/
       * tRFC4 figures for 4/8/16 Gb verbatim, which is upstream's own
       * substitution, not a GDDR6 measurement.) Inventing a 32 Gb entry is
       * not allowed, so those presets refuse loudly instead of continuing on
       * a number read off the front of the array. They never produced a
       * defined timing, so nothing correct is being taken away. */
      if (density_id < 0) {
        throw ConfigurationError(
          "In \"{}\", organization density {} Mb has no tabulated refresh "
          "timing (tRFC is tabulated for 4/8/16 Gb only; the shipped "
          "GDDR6_32Gb_x8 / GDDR6_32Gb_x16 org presets are outside that range "
          "and JESD250D leaves tRFC vendor-specific)!",
          get_name(), m_organization.density);
      }

      m_timing_vals("nRFC")  = JEDEC_rounding(tRFC_TABLE[0][density_id], tCK_ps);
      m_timing_vals("nREFI") = JEDEC_rounding(tREFI_BASE, tCK_ps);

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
          // CAS <-> CAS
          /// Data bus occupancy
          {.level = "channel", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA"}, .latency = V("nBL")},
          {.level = "channel", .preceding = {"WR", "WRA"}, .following = {"WR", "WRA"}, .latency = V("nBL")},

          /*** Rank (or different BankGroup) ***/
          // changed from rank to channel, some duplicates, what takes
          // CAS <-> CAS
          /// nCCDS is the minimal latency for column commands 
          {.level = "channel", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA"}, .latency = V("nCCDS")},
          {.level = "channel", .preceding = {"WR", "WRA"}, .following = {"WR", "WRA"}, .latency = V("nCCDS")},
          /// RD <-> WR, Minimum Read to Write, Assuming tWPRE = 1 tCK                          
          {.level = "channel", .preceding = {"RD", "RDA"}, .following = {"WR", "WRA"}, .latency = V("nCL") + V("nBL") + 3 - V("nCWL") + 1}, //+ 1 is assuming bus turn around time
          /// WR <-> RD, Minimum Read after Write
          {.level = "channel", .preceding = {"WR", "WRA"}, .following = {"RD", "RDA"}, .latency = V("nCWL") + V("nBL") + V("nWTRS")},
          /// CAS <-> PREA
          {.level = "channel", .preceding = {"RD"}, .following = {"PREA"}, .latency = V("nRTP")},
          {.level = "channel", .preceding = {"WR"}, .following = {"PREA"}, .latency = V("nCWL") + V("nBL") + V("nWR")},          
          /// RAS <-> RAS
          {.level = "channel", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRRDS")},          
          {.level = "channel", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nFAW"), .window = 4},       
          {.level = "channel", .preceding = {"ACT"}, .following = {"PRE"}, .latency = V("nRAS")},          
          {.level = "channel", .preceding = {"PRE"}, .following = {"ACT"}, .latency = V("nRP")},          
          /// RAS <-> REF
          {.level = "channel", .preceding = {"ACT"}, .following = {"REFab"}, .latency = V("nRC")},          
          {.level = "channel", .preceding = {"PRE"}, .following = {"REFab"}, .latency = V("nRP")},          
          {.level = "channel", .preceding = {"RDA"}, .following = {"REFab"}, .latency = V("nRP") + V("nRTP")},          
          {.level = "channel", .preceding = {"WRA"}, .following = {"REFab"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},          
          {.level = "channel", .preceding = {"REFab"}, .following = {"ACT"}, .latency = V("nRFC")},          
          
          /// RAS <-> REFp2b
          {.level = "channel", .preceding = {"ACT"}, .following = {"REFp2b"}, .latency = V("nRRDL")},          
          {.level = "channel", .preceding = {"PRE"}, .following = {"REFp2b"}, .latency = V("nRP")},          
          {.level = "channel", .preceding = {"RDA"}, .following = {"REFp2b"}, .latency = V("nRP") + V("nRTP")},          
          {.level = "channel", .preceding = {"WRA"}, .following = {"REFp2b"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},          
          {.level = "channel", .preceding = {"REFp2b"}, .following = {"ACT"}, .latency = V("nRREFD")},   

          /// RAS <-> REFpb
          {.level = "channel", .preceding = {"ACT"}, .following = {"REFpb"}, .latency = V("nRRDL")},          
          {.level = "channel", .preceding = {"PRE"}, .following = {"REFpb"}, .latency = V("nRP")},          
          {.level = "channel", .preceding = {"RDA"}, .following = {"REFpb"}, .latency = V("nRP") + V("nRTP")},          
          {.level = "channel", .preceding = {"WRA"}, .following = {"REFpb"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},          
          {.level = "channel", .preceding = {"REFpb"}, .following = {"ACT"}, .latency = V("nRREFD")},   


          /*** Same Bank Group ***/ 
          /// CAS <-> CAS
          {.level = "bankgroup", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA"}, .latency = V("nCCDL")},          
          {.level = "bankgroup", .preceding = {"WR", "WRA"}, .following = {"WR", "WRA"}, .latency = V("nCCDL")},          
          {.level = "bankgroup", .preceding = {"WR", "WRA"}, .following = {"RD", "RDA"}, .latency = V("nCWL") + V("nBL") + V("nWTRL")},
          /// RAS <-> RAS
          {.level = "bankgroup", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRRDL")},  

          /*** Bank ***/ 
          /// CAS <-> RAS
          {.level = "bank", .preceding = {"ACT"}, .following = {"RD", "RDA"}, .latency = V("nRCDRD")}, 
          {.level = "bank", .preceding = {"ACT"}, .following = {"WR", "WRA"}, .latency = V("nRCDWD")},
          {.level = "bank", .preceding = {"ACT"}, .following = {"PRE"}, .latency = V("nRAS")},  
          {.level = "bank", .preceding = {"PRE"}, .following = {"ACT"}, .latency = V("nRP")},  
          {.level = "bank", .preceding = {"RD"},  .following = {"PRE"}, .latency = V("nRTP")},  
          {.level = "bank", .preceding = {"WR"},  .following = {"PRE"}, .latency = V("nCWL") + V("nBL") + V("nWR")},  
          {.level = "bank", .preceding = {"RDA"}, .following = {"ACT"}, .latency = V("nRTP") + V("nRP")},  
          {.level = "bank", .preceding = {"WRA"}, .following = {"ACT"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},  

          /// RAS <-> REFpb
          {.level = "bank", .preceding = {"ACT"}, .following = {"REFpb"}, .latency = V("nRC")},          
          {.level = "bank", .preceding = {"PRE"}, .following = {"REFpb"}, .latency = V("nRP")},          
          {.level = "bank", .preceding = {"RDA"}, .following = {"REFpb"}, .latency = V("nRP") + V("nRTP")},          
          {.level = "bank", .preceding = {"WRA"}, .following = {"REFpb"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},          
          {.level = "bank", .preceding = {"REFpb"}, .following = {"ACT"}, .latency = V("nRFCpb")},   

          /// RAS <-> RAS
          //{.level = "bank", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRC")}, //should this be added?
        }
      );
      #undef V

    };

    void set_actions() {
      m_actions.resize(m_levels.size(), std::vector<ActionFunc_t<Node>>(m_commands.size()));

      // Channel Actions 
      m_actions[m_levels["channel"]][m_commands["PREA"]] = Lambdas::Action::Channel::PREab<GDDR6>; 

      // Bank actions
      m_actions[m_levels["bank"]][m_commands["ACT"]] = Lambdas::Action::Bank::ACT<GDDR6>;
      m_actions[m_levels["bank"]][m_commands["PRE"]] = Lambdas::Action::Bank::PRE<GDDR6>;
      m_actions[m_levels["bank"]][m_commands["RDA"]] = Lambdas::Action::Bank::PRE<GDDR6>;
      m_actions[m_levels["bank"]][m_commands["WRA"]] = Lambdas::Action::Bank::PRE<GDDR6>;
    };

    void set_preqs() {
      m_preqs.resize(m_levels.size(), std::vector<PreqFunc_t<Node>>(m_commands.size()));

      // Channel Actions 
      m_preqs[m_levels["channel"]][m_commands["REFab"]] = Lambdas::Preq::Channel::RequireAllBanksClosed<GDDR6>; 

      // Bank actions
      m_preqs[m_levels["bank"]][m_commands["RD"]] = Lambdas::Preq::Bank::RequireRowOpen<GDDR6>;
      m_preqs[m_levels["bank"]][m_commands["WR"]] = Lambdas::Preq::Bank::RequireRowOpen<GDDR6>;
      //m_preqs[m_levels["channel"]][m_commands["REFpb"]] = Lambdas::Preq::Bank::RequireAllBanksClosed<GDDR6>; // can RequireSameBanksClosed be used, or is RequireBankClosed needed?
      //m_preqs[m_levels["channel"]][m_commands["REFp2b"]] = Lambdas::Preq::Bank::RequireAllBanksClosed<GDDR6>; 
    };

    void set_rowhits() {
      m_rowhits.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowhits[m_levels["bank"]][m_commands["RD"]] = Lambdas::RowHit::Bank::RDWR<GDDR6>;
      m_rowhits[m_levels["bank"]][m_commands["WR"]] = Lambdas::RowHit::Bank::RDWR<GDDR6>;
    }


    void set_rowopens() {
      m_rowopens.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowopens[m_levels["bank"]][m_commands["RD"]] = Lambdas::RowOpen::Bank::RDWR<GDDR6>;
      m_rowopens[m_levels["bank"]][m_commands["WR"]] = Lambdas::RowOpen::Bank::RDWR<GDDR6>;
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

