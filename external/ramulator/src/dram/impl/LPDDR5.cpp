#include "dram/dram.h"
#include "dram/lambdas.h"
#include "dram/pimid_bank_open.h"   // PIMID 1.11.91 (R8-7)

namespace Ramulator {

class LPDDR5 : public IDRAM, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAM, LPDDR5, "LPDDR5", "LPDDR5 Device Model")

  public:
  /* PIMID 1.9.10: datasheet IDD/VDD presets (Micron 12Gb LPDDR5-6400) -- relocated energy layer.
     Intensive per-access model lives in dram/pimid_energy.h (active source); these
     mirror it in upstream convention for Ramulator2's command-driven power path. */
    inline static const std::map<std::string, std::vector<double>> voltage_presets = {
      {"Default",     {1.05,  1.8}},
    };
    inline static const std::map<std::string, std::vector<double>> current_presets = {
      // name         IDD0 IDD2N IDD3N IDD4R IDD4W IDD5B  IPP0 IPP2N IPP3N IPP4R IPP4W IPP5B
      {"Default",     {32,18,24,110,120,90, 3,3,3,3,3,40}},
    };

    inline static const std::map<std::string, Organization> org_presets = {
      //   name           density   DQ   Ch Ra Bg Ba   Ro     Co
      {"LPDDR5_2Gb_x16",  {2<<10,   16, {1, 1, 4, 4, 1<<13, 1<<10}}},
      {"LPDDR5_4Gb_x16",  {4<<10,   16, {1, 1, 4, 4, 1<<14, 1<<10}}},
      {"LPDDR5_8Gb_x16",  {8<<10,   16, {1, 1, 4, 4, 1<<15, 1<<10}}},
      {"LPDDR5_16Gb_x16", {16<<10,  16, {1, 1, 4, 4, 1<<16, 1<<10}}},
      {"LPDDR5_32Gb_x16", {32<<10,  16, {1, 1, 4, 4, 1<<17, 1<<10}}},
    };

    /* 1.11.63 (calibration): THE LPDDR5 CLOCK DOMAIN, and three fields with it.
     *
     * This row had three different clock domains in it at once. The audit
     * (misc/-sourced, JESD209-5 not held) established which one it is:
     *
     *   The row's own timings imply tCK = 1.25 ns: nRCD 15 -> 18.75 ns,
     *   nRPpb 15 -> 18.75 ns, nRPab 17 -> 21.25 ns, nRAS 34 -> 42.5 ns,
     *   nWR 28 -> 35 ns. Those are recognisable LPDDR5 minima and they are the
     *   CK domain at WCK:CK = 4:1, i.e. CK = 6400/8 = 800 MHz.
     *   nBL16 = 4 implied tCK = 0.625 ns (WCK:CK = 2:1) instead.
     *   set_timing_vals() computed 1e6/(6400/2) = 312 ps and used THAT, a
     *   domain in which nRCD would be 4.7 ns and nRAS 10.6 ns -- physically
     *   impossible for any DRAM.
     *
     * The CK domain (1.25 ns) is the one adopted, because it is the one the
     * thirteen UNCHECKABLE nXX fields were authored in and JEDEC states LPDDR5
     * command timings in CK cycles. The derivation in set_timing_vals() is
     * corrected to match (8 bits/pin per CK, not 2), so this column's written
     * 1250 is now also the value actually used. `rate` stays 6400 -- a real
     * Micron speed grade [1.11.91: the sheet that showed it is ruled not
     * citable (confidential marking); uncited cross-check] -- and the thirteen CK-domain
     * nXX fields keep their cycle counts, which RESTORES their intended
     * nanoseconds instead of changing them.
     *
     * The two fields that were NOT in the CK domain are re-based, ns value
     * preserved:
     *   nBL16 4 -> 2. A BL16 burst on a x16 channel at 6400 Mb/s occupies
     *     16 bits/pin / 6.4 Gb/s = 2.5 ns of the DQ bus = 2 CK at 1.25 ns.
     *     DERIVED-FROM-IDENTITY (burst length vs data rate), not sourced.
     *   nCCD 4 -> 2. Authored as the matched pair of nBL16 in the 0.625 ns
     *     domain (both = one BL16 burst = 2.5 ns); re-based to CK it is 2, so
     *     its NANOSECOND value is unchanged at 2.5 ns. At the time this was a
     *     domain conversion with JESD209-5 not held. SUPERSEDED same release:
     *     once the standard arrived, the single field was split into
     *     nCCDS 2 / nCCDL 4 (see the 1.11.63 block below) -- the 2 survives
     *     as the different-bank-group value, which is the one that carries
     *     the 12.8 GB/s streaming-rate argument.
     *
     *   nRC 30 -> 49. DERIVED-FROM-IDENTITY. tRC >= tRAS + tRP is definitional
     *     and provable without JESD209-5: nRAS 34 + nRPpb 15 = 49. The old 30
     *     was 19 cycles short and shorter than nRAS alone. Set from the row's
     *     own nRAS and nRPpb; no bin value invented.
     *
     * The header's first column name is also corrected: it read "nBL" where
     * m_timings (below) says "nBL16". Position was already right.
     *
     * 1.11.63 (calibration, JESD209-5C acquired 2026-08-24 -- misc/): the
     * previously-UNCHECKABLE AC fields were verified against the standard
     * itself. Governing tables at 6400 Mb/s: the part is in BG MODE (cl.2.2.3
     * p.4: 16B mode is legal only <= 3200 Mb/s), so core timings come from
     * Table 381 (x16, BG mode, DVFSC off) p.519, latencies from the
     * 6000 < rate <= 6400 row "1011B" of Tables 225 (RL) p.261 and 229 (WL)
     * p.265, and burst occupancy from Table 339 p.482. Rounding is plain
     * RU() (Tables 225/230 notes) -- at tCK = 1250 ps identical to
     * JEDEC_rounding()'s DDR-style guardband for every value here.
     * VERIFIED UNCHANGED (JEDEC min at tCK 1.25 ns): nRCD 15 (max(18ns,2nCK)),
     * nRPab 17 (21ns), nRPpb 15 (18ns), nRAS 34 (42ns), nWR 28 (max(34ns,
     * 3nCK); Table 230 row 1011B tabulates 28 exactly), nRTP 4 (tRBTP
     * max(7.5ns,2nCK)-2nCK at 4:1; Table 225 row 1011B tabulates 4), nRRD 4
     * (5ns), nWTRS 5 (6.25ns), nWTRL 10 (12ns), nFAW 16 (20ns), nPPD 2
     * (given in nCK), nBL16 2 (Table 339 4:1 BL16: BL/n_min = 2*tCK).
     * CORRECTED (all previously read one frequency bin too high -- the old
     * values are exactly row "1100B"'s):
     *   nCL  20 -> 17  (Table 225 row 1011B, RL Set 0 -- no Byte Mode/DBI);
     *   nCWL 11 ->  9  (Table 229 row 1011B, WL Set A -- MR3 OP[5]=0 default);
     *   nCS   2 ->  3  (Table 377 p.515: RD->RD different rank >=
     *                   2*BL/n_min + 1 + RU((tRPST+tRPRE-0.5*tWCK)/tCK) >= 5
     *                   CK; the model charges nBL16 + nCS, so nCS >= 3.
     *                   Inert in shipped presets -- Ra = 1).
     * SPLIT: nCCD -> nCCDS 2 / nCCDL 4. In BG mode Table 339 gives
     * column-to-column BL/n = 2*tCK to a DIFFERENT bank group and 4*tCK
     * (BL/n_max) within the SAME bank group (Tables 340-342 pp.483-484). The
     * single nCCD = 2 let same-BG columns cycle at twice the JEDEC rate; the
     * pre-63a nCCD = 4 had the mirror error (different-BG half speed). The
     * peak-channel-rate argument in the note above holds for nCCDS:
     * different-BG streaming sustains 12.8 GB/s, as the Micron front page
     * states.
     * nRC 49 stays: the standard gives only the formula tRAS + tRPpb
     * (per-bank precharge, Table 381); summing the already-rounded CK values
     * (34 + 15) gives 49 vs 48 if summed in ns first -- conservative by one
     * CK, kept. */
    inline static const std::map<std::string, std::vector<int>> timing_presets = {
      //   name         rate  nBL16 nCL  nRCD  nRPab  nRPpb   nRAS  nRC   nWR  nRTP nCWL nCCDS nCCDL nRRD nWTRS nWTRL nFAW  nPPD  nRFCab nRFCpb nREFI nPBR2PBR nPBR2ACT nCS,  tCK_ps
      {"LPDDR5_6400",  {6400,  2,   17,   15,    17,   15,     34,   49,   28,   4,   9,   2,    4,    4,   5,    10,   16,  2,   -1,      -1,   -1,   -1,        -1,    3,   1250}},
    };


  /************************************************
   *                Organization
   ***********************************************/   
    const int m_internal_prefetch_size = 8;

    inline static constexpr ImplDef m_levels = {
      "channel", "rank", "bankgroup", "bank", "row", "column",    
    };


  /************************************************
   *             Requests & Commands
   ***********************************************/
    inline static constexpr ImplDef m_commands = {
      "ACT-1",  "ACT-2",
      "PRE",    "PREA",
      "CASRD",  "CASWR",   // WCK2CK Sync
      "RD16",   "WR16",   "RD16A",   "WR16A",
      "REFab",  "REFpb",
      "RFMab",  "RFMpb",
    };

    inline static const ImplLUT m_command_scopes = LUT (
      m_commands, m_levels, {
        {"ACT-1", "row"},    {"ACT-2",  "row"},
        {"PRE",   "bank"},   {"PREA",   "rank"},
        {"CASRD", "rank"},   {"CASWR",  "rank"},
        {"RD16",  "column"}, {"WR16",   "column"}, {"RD16A", "column"}, {"WR16A", "column"},
        {"REFab", "rank"},   {"REFpb",  "rank"},
        {"RFMab", "rank"},   {"RFMpb",  "rank"},
      }
    );

    inline static const ImplLUT m_command_meta = LUT<DRAMCommandMeta> (
      m_commands, {
                // open?   close?   access?  refresh?
        {"ACT-1",  {false,  false,   false,   false}},
        {"ACT-2",  {true,   false,   false,   false}},
        {"PRE",    {false,  true,    false,   false}},
        {"PREA",   {false,  true,    false,   false}},
        {"CASRD",  {false,  false,   false,   false}},
        {"CASWR",  {false,  false,   false,   false}},
        {"RD16",   {false,  false,   true,    false}},
        {"WR16",   {false,  false,   true,    false}},
        {"RD16A",  {false,  true,    true,    false}},
        {"WR16A",  {false,  true,    true,    false}},
        {"REFab",  {false,  false,   false,   true }},
        {"REFpb",  {false,  false,   false,   true }},
        {"RFMab",  {false,  false,   false,   true }},
        {"RFMpb",  {false,  false,   false,   true }},
      }
    );

    inline static constexpr ImplDef m_requests = {
      "read16", "write16",
      "all-bank-refresh", "per-bank-refresh"
    };

    inline static const ImplLUT m_request_translations = LUT (
      m_requests, m_commands, {
        {"read16", "RD16"}, {"write16", "WR16"}, 
        {"all-bank-refresh", "REFab"}, {"per-bank-refresh", "REFpb"},
      }
    );

   
  /************************************************
   *                   Timing
   ***********************************************/
    inline static constexpr ImplDef m_timings = {
      "rate", 
      "nBL16", "nCL", "nRCD", "nRPab", "nRPpb", "nRAS", "nRC", "nWR", "nRTP", "nCWL",
      "nCCDS", "nCCDL",
      "nRRD",
      "nWTRS", "nWTRL",
      "nFAW",
      "nPPD",
      "nRFCab", "nRFCpb","nREFI",
      "nPBR2PBR", "nPBR2ACT",
      "nCS",
      "tCK_ps"
    };


  /************************************************
   *                 Node States
   ***********************************************/
    inline static constexpr ImplDef m_states = {
    //    ACT-1       ACT-2
       "Pre-Opened", "Opened", "Closed", "PowerUp", "N/A", "Refreshing"
    };

    inline static const ImplLUT m_init_states = LUT (
      m_levels, m_states, {
        {"channel",   "N/A"}, 
        {"rank",      "PowerUp"},
        {"bankgroup", "N/A"},
        {"bank",      "Closed"},
        {"row",       "Closed"},
        {"column",    "N/A"},
      }
    );

  public:
    struct Node : public DRAMNodeBase<LPDDR5> {
      Clk_t m_final_synced_cycle = -1; // Extra CAS Sync command needed for RD/WR after this cycle

      Node(LPDDR5* dram, Node* parent, int level, int id) : DRAMNodeBase<LPDDR5>(dram, parent, level, id) {};
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
      m_channel_width = param_group("org").param<int>("channel_width").default_val(32);

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
      size_t _density = size_t(m_organization.count[m_levels["bankgroup"]]) *
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
      /* 1.11.63 (calibration): ONE AUTHORITY FOR tCK, and for LPDDR5 the
       * divisor itself was wrong.
       *
       * The upstream form, `1E6 / (rate / 2)`, assumes a classic DDR bus where
       * the data rate is twice the COMMAND clock. LPDDR5 is not that: it has a
       * separate WCK strobe running at WCK:CK = 4:1 (or 2:1), and every timing
       * in the preset row above is stated in CK cycles. At 6400 Mb/s with
       * WCK:CK = 4:1 the pin moves 8 bits per CK, so
       *     CK  = 6400 / 8 = 800 MHz
       *     tCK = 8e6 / rate = 1250 ps
       * which is exactly the domain the row's nRCD/nRPpb/nRPab/nRAS/nWR were
       * authored in (18.75 / 18.75 / 21.25 / 42.5 / 35 ns). The old form
       * produced 312 ps -- the WCK/2 period -- and the model ran the command
       * domain 4x fast.
       *
       * LIVE EFFECT: tCK is the divisor that turns the per-density refresh
       * tables (in ns) into cycles, so refresh was being priced 4x long
       * RELATIVE to every other timing in the same row: nRFCab was
       * JEDEC_rounding(210 ns, 312 ps) = 674 cycles beside an ACT-to-ACT of
       * 30, where the row's own 1.25 ns gives 168 beside 49. It is also the
       * memory system's cycle length (generic_DRAM_system::get_tCK()), so the
       * whole LPDDR5 channel now ticks at its real 800 MHz command rate.
       *
       * The tCK_ps column in the preset row above is a MIRROR of this
       * derivation and is checked against it below, so the two cannot silently
       * disagree again -- which is precisely how 1250 and 312 coexisted. */
      int preset_tCK_ps = m_timing_vals("tCK_ps");
      int tCK_ps = 8E6 / m_timing_vals("rate");
      m_timing_vals("tCK_ps") = tCK_ps;
      if (preset_provided && preset_tCK_ps != tCK_ps) {
        throw ConfigurationError(
          "In \"{}\", the timing preset's tCK_ps column says {} ps but the "
          "rate of {} Mb/s derives {} ps (tCK = 8e6/rate: LPDDR5 moves 8 "
          "bits/pin per CK at WCK:CK = 4:1). The derivation wins at run time, "
          "so the column must mirror it -- fix the preset!",
          get_name(), preset_tCK_ps, m_timing_vals("rate"), tCK_ps);
      }

      // Load the organization specific timings
      int dq_id = [](int dq) -> int {
        switch (dq) {
          case 16: return 0;
          default: return -1;
        }
      }(m_organization.dq);

      int rate_id = [](int rate) -> int {
        switch (rate) {
          case 6400:  return 0;
          default:    return -1;
        }
      }(m_timing_vals("rate"));


      // Refresh timings
      // tRFC table (unit is nanosecond!)
      constexpr int tRFCab_TABLE[4] = {
      //  2Gb   4Gb   8Gb  16Gb
          130,  180,  210,  280, 
      };

      constexpr int tRFCpb_TABLE[4] = {
      //  2Gb   4Gb   8Gb  16Gb
          60,   90,   120,  140, 
      };

      constexpr int tPBR2PBR_TABLE[4] = {
      //  2Gb   4Gb   8Gb  16Gb
          60,   90,   90,  90, 
      };

      /* 1.11.63 (calibration): tPBR2ACT at 8 Gb and 16 Gb, 8 -> 7.5 ns.
       * SOURCES (both held in misc/):
       *   8 Gb die  -- [1.11.91: the 8 Gb sheet first cited here is ruled
       *     not citable (confidential marking); it remains an uncited
       *     cross-check that agrees: 7.5 ns in BG and 16B mode.] The org
       *     this model uses is BG mode (4 bank groups x 4 banks -- see
       *     org_presets), so the BG-mode value applies.
       *   16 Gb die -- Micron LPDDR5X Y52P (Rev. H 03/2025), Table 6 "Refresh
       *     Requirement Parameters", p.11: "tPBR2ACT | 7.5 | ns" (BG and 16B
       *     Mode). The 12 Gb Y4BM part gives the same 7.5.
       * The 2 Gb and 4 Gb columns were UNCHECKABLE while only Micron die
       * datasheets (8/12/16 Gb) were held; JESD209-5C Table 240 p.287 settles
       * them: tpbR2act = 7.5 ns in EVERY density column (2..32 Gb, BG/16B
       * mode; 10 ns exists only in Table 241's 8-bank mode). 8.0 appears
       * nowhere in the standard -- all four entries are now 7.5.
       * The array is float because 7.5 is not an integer; JEDEC_rounding()
       * already takes a float first argument. */
      constexpr float tPBR2ACT_TABLE[4] = {
      //  2Gb   4Gb   8Gb   16Gb
          7.5f, 7.5f, 7.5f, 7.5f,
      };

      // tREFI(base) table (unit is nanosecond!)
      constexpr int tREFI_BASE = 3906;
      int density_id = [](int density_Mb) -> int { 
        switch (density_Mb) {
          case 2048:  return 0;
          case 4096:  return 1;
          case 8192:  return 2;
          case 16384: return 3;
          default:    return -1;
        }
      }(m_organization.density);
      /* PIMID 1.11.59 (audit F037-class): the lambda returns -1 for any
       * density outside the four tabulated ones, and that -1 then indexed
       * tRFCab_TABLE, tRFCpb_TABLE, tPBR2PBR_TABLE and tPBR2ACT_TABLE. An
       * out-of-bounds read one element BEFORE a table of plausible nanosecond
       * figures does not crash -- it yields whatever adjacent constant the
       * linker put there, and the run continues with a refresh time nobody
       * can trace, which is why an unrecognised density silently produced a
       * plausible-looking timing set instead of an error.
       *
       * BEHAVIOUR CHANGE, stated deliberately: unlike the DDR3/DDR4/DDR5/HBM
       * models, this file's org presets are NOT all covered by the switch.
       * The five shipped presets are 2/4/8/16/32 Gb (LPDDR5_2Gb_x16 ...
       * LPDDR5_32Gb_x16); the switch and all four tables stop at 16 Gb. So
       * LPDDR5_32Gb_x16 hit the [-1] read on every instantiation and is the
       * one preset this throw now refuses. The alternative -- adding a 32 Gb
       * column -- would require a tRFCab/tRFCpb/tPBR2PBR figure for a 32 Gb
       * LPDDR5 die, and no such figure exists in any source this tree holds:
       * the Micron LPDDR5/LPDDR5X datasheets in misc/ tabulate 8 Gb
       * (210/120/90), 12 Gb (280/140/90) and 16 Gb (280/140/90) dies and stop
       * there, because 32 Gb LPDDR5 parts are multi-die packages rather than
       * a 32 Gb die with its own refresh row. Inventing that column is not
       * allowed, so this preset refuses loudly instead of continuing on a
       * number read off the front of the array. It never produced a defined
       * timing, so nothing correct is being taken away. */
      if (density_id < 0) {
        throw ConfigurationError(
          "In \"{}\", organization density {} Mb has no tabulated refresh "
          "timing (tRFCab/tRFCpb/tPBR2PBR/tPBR2ACT are tabulated for "
          "2/4/8/16 Gb only; the shipped LPDDR5_32Gb_x16 org preset is "
          "outside that range and has no sourced refresh figures)!",
          get_name(), m_organization.density);
      }

      m_timing_vals("nRFCab")    = JEDEC_rounding_RU(tRFCab_TABLE[density_id], tCK_ps);
      m_timing_vals("nRFCpb")    = JEDEC_rounding_RU(tRFCpb_TABLE[density_id], tCK_ps);
      m_timing_vals("nPBR2PBR")  = JEDEC_rounding_RU(tPBR2PBR_TABLE[density_id], tCK_ps);
      m_timing_vals("nPBR2ACT")  = JEDEC_rounding_RU(tPBR2ACT_TABLE[density_id], tCK_ps);
      m_timing_vals("nREFI") = JEDEC_rounding_RU(tREFI_BASE, tCK_ps);   // 1.11.63: LPDDR5 rounds by plain RU() -- JESD209-5C Tbl 225/230 notes

      // Overwrite timing parameters with any user-provided value
      // Rate and tCK should not be overwritten
      for (int i = 1; i < m_timings.size() - 1; i++) {
        auto timing_name = std::string(m_timings(i));

        if (auto provided_timing = param_group("timing").param<int>(timing_name).optional()) {
          // Check if the user specifies in the number of cycles (e.g., nRCD)
          m_timing_vals(i) = *provided_timing;
        } else if (auto provided_timing = param_group("timing").param<float>(timing_name.replace(0, 1, "t")).optional()) {
          // Check if the user specifies in nanoseconds (e.g., tRCD)
          m_timing_vals(i) = JEDEC_rounding_RU(*provided_timing, tCK_ps);   // 1.11.63: LPDDR5 rounds by plain RU()
        }
      }

      // Check if there is any uninitialized timings
      for (int i = 0; i < m_timing_vals.size(); i++) {
        if (m_timing_vals(i) == -1) {
          throw ConfigurationError("In \"{}\", timing {} is not specified!", get_name(), m_timings(i));
        }
      }      

      // Set read latency
      m_read_latency = m_timing_vals("nCL") + m_timing_vals("nBL16");

      // Populate the timing constraints
      #define V(timing) (m_timing_vals(timing))
      populate_timingcons(this, {
          /*** Channel ***/ 
          // CAS <-> CAS
          /// Data bus occupancy
          {.level = "channel", .preceding = {"RD16", "RD16A"}, .following = {"RD16", "RD16A"}, .latency = V("nBL16")},
          {.level = "channel", .preceding = {"WR16", "WR16A"}, .following = {"WR16", "WR16A"}, .latency = V("nBL16")},

          /*** Rank (or different BankGroup) ***/ 
          // CAS <-> CAS
          // 1.11.63: different bank group -> BL/n = 2*tCK (JESD209-5C Tbl 339 p.482, Tbl 342 p.484)
          {.level = "rank", .preceding = {"RD16", "RD16A"}, .following = {"RD16", "RD16A"}, .latency = V("nCCDS")},
          {.level = "rank", .preceding = {"WR16", "WR16A"}, .following = {"WR16", "WR16A"}, .latency = V("nCCDS")},
          /// RD <-> WR, Minimum Read to Write, Assuming tWPRE = 1 tCK                          
          {.level = "rank", .preceding = {"RD16", "RD16A"}, .following = {"WR16", "WR16A"}, .latency = V("nCL") + V("nBL16") + 2 - V("nCWL")},
          /// WR <-> RD, Minimum Read after Write
          {.level = "rank", .preceding = {"WR16", "WR16A"}, .following = {"RD16", "RD16A"}, .latency = V("nCWL") + V("nBL16") + V("nWTRS")},
          /// CAS <-> CAS between sibling ranks, nCS (rank switching) is needed for new DQS
          {.level = "rank", .preceding = {"RD16", "RD16A"}, .following = {"RD16", "RD16A", "WR16", "WR16A"}, .latency = V("nBL16") + V("nCS"), .is_sibling = true},
          {.level = "rank", .preceding = {"WR16", "WR16A"}, .following = {"RD16", "RD16A"}, .latency = V("nCL")  + V("nBL16") + V("nCS") - V("nCWL"), .is_sibling = true},
          /// CAS <-> PREab
          // 1.11.63: RD->PRE = BL/n_min + RU(tRBTP/tCK); WR->PRE = WL + BL/n_min + 1 + RU(tWR/tCK) (JESD209-5C Tbl 340 p.483)
          {.level = "rank", .preceding = {"RD16"}, .following = {"PREA"}, .latency = V("nBL16") + V("nRTP")},
          {.level = "rank", .preceding = {"WR16"}, .following = {"PREA"}, .latency = V("nCWL") + V("nBL16") + 1 + V("nWR")},          
          /// RAS <-> RAS
          {.level = "rank", .preceding = {"ACT-1"}, .following = {"ACT-1", "REFpb"}, .latency = V("nRRD")},          
          {.level = "rank", .preceding = {"ACT-1"}, .following = {"ACT-1"}, .latency = V("nFAW"), .window = 4},          
          {.level = "rank", .preceding = {"ACT-1"}, .following = {"PREA"}, .latency = V("nRAS")},          
          {.level = "rank", .preceding = {"PREA"}, .following = {"ACT-1"}, .latency = V("nRPab")},          
          /// RAS <-> REF
          {.level = "rank", .preceding = {"ACT-1"}, .following = {"REFab"}, .latency = V("nRC")},          
          {.level = "rank", .preceding = {"PRE"}, .following = {"REFab"}, .latency = V("nRPpb")},          
          {.level = "rank", .preceding = {"PREA"}, .following = {"REFab"}, .latency = V("nRPab")},          
          {.level = "rank", .preceding = {"RD16A"}, .following = {"REFab"}, .latency = V("nBL16") + V("nRTP") + V("nRPpb")},          
          {.level = "rank", .preceding = {"WR16A"}, .following = {"REFab"}, .latency = V("nCWL") + V("nBL16") + 1 + V("nWR") + V("nRPpb")},          
          {.level = "rank", .preceding = {"REFab"}, .following = {"REFab", "ACT-1", "REFpb"}, .latency = V("nRFCab")},          
          {.level = "rank", .preceding = {"ACT-1"},   .following = {"REFpb"}, .latency = V("nPBR2ACT")},  
          {.level = "rank", .preceding = {"REFpb"}, .following = {"REFpb"}, .latency = V("nPBR2PBR")},  

          /*** Same Bank Group ***/ 
          /// CAS <-> CAS
          // 1.11.63: same bank group -> BL/n = 4*tCK = BL/n_max (JESD209-5C Tbl 339 p.482, Tbls 340/341 pp.483-484)
          {.level = "bankgroup", .preceding = {"RD16", "RD16A"}, .following = {"RD16", "RD16A"}, .latency = V("nCCDL")},          
          {.level = "bankgroup", .preceding = {"WR16", "WR16A"}, .following = {"WR16", "WR16A"}, .latency = V("nCCDL")},          
          // 1.11.63: WL + BL/n_max + RU(tWTR_L/tCK) -- BL/n_max = 2*BL/n_min in BG mode (Tbl 340)
          {.level = "bankgroup", .preceding = {"WR16", "WR16A"}, .following = {"RD16", "RD16A"}, .latency = V("nCWL") + 2 * V("nBL16") + V("nWTRL")},
          /// RAS <-> RAS
          {.level = "bankgroup", .preceding = {"ACT-1"}, .following = {"ACT-1"}, .latency = V("nRRD")},  

          /*** Bank ***/ 
          {.level = "bank", .preceding = {"ACT-1"}, .following = {"ACT-1"}, .latency = V("nRC")},  
          {.level = "bank", .preceding = {"ACT-1"}, .following = {"RD16", "RD16A", "WR16", "WR16A"}, .latency = V("nRCD")},  
          {.level = "bank", .preceding = {"ACT-1"}, .following = {"PRE"}, .latency = V("nRAS")},  
          {.level = "bank", .preceding = {"PRE"}, .following = {"ACT-1"}, .latency = V("nRPpb")},  
          // 1.11.63: same two formulas as rank scope (JESD209-5C Tbl 340 p.483)
          {.level = "bank", .preceding = {"RD16"},  .following = {"PRE"}, .latency = V("nBL16") + V("nRTP")},  
          {.level = "bank", .preceding = {"WR16"},  .following = {"PRE"}, .latency = V("nCWL") + V("nBL16") + 1 + V("nWR")},  
          {.level = "bank", .preceding = {"RD16A"}, .following = {"ACT-1"}, .latency = V("nBL16") + V("nRTP") + V("nRPpb")},  
          {.level = "bank", .preceding = {"WR16A"}, .following = {"ACT-1"}, .latency = V("nCWL") + V("nBL16") + 1 + V("nWR") + V("nRPpb")},  
        }
      );
      #undef V

    };

    void set_actions() {
      m_actions.resize(m_levels.size(), std::vector<ActionFunc_t<Node>>(m_commands.size()));

      // Rank Actions
      m_actions[m_levels["rank"]][m_commands["PREA"]] = Lambdas::Action::Rank::PREab<LPDDR5>;
      m_actions[m_levels["rank"]][m_commands["CASRD"]] = [] (Node* node, int cmd, int target_id, Clk_t clk) {
        node->m_final_synced_cycle = clk + m_timings["nCL"] + m_timings["nBL16"] + 1; 
      };
      m_actions[m_levels["rank"]][m_commands["CASWR"]] = [] (Node* node, int cmd, int target_id, Clk_t clk) {
        node->m_final_synced_cycle = clk + m_timings["nCWL"] + m_timings["nBL16"] + 1; 
      };
      m_actions[m_levels["rank"]][m_commands["RD16"]] = [] (Node* node, int cmd, int target_id, Clk_t clk) {
        node->m_final_synced_cycle = clk + m_timings["nCL"] + m_timings["nBL16"]; 
      };
      m_actions[m_levels["rank"]][m_commands["WR16"]] = [] (Node* node, int cmd, int target_id, Clk_t clk) {
        node->m_final_synced_cycle = clk + m_timings["nCWL"] + m_timings["nBL16"]; 
      };
      // Bank actions
      m_actions[m_levels["bank"]][m_commands["ACT-1"]] = [] (Node* node, int cmd, int target_id, Clk_t clk) {
        node->m_state = m_states["Pre-Opened"];
        node->m_row_state[target_id] = m_states["Pre-Opened"];
      };
      m_actions[m_levels["bank"]][m_commands["ACT-2"]] = Lambdas::Action::Bank::ACT<LPDDR5>;
      m_actions[m_levels["bank"]][m_commands["PRE"]]   = Lambdas::Action::Bank::PRE<LPDDR5>;
      m_actions[m_levels["bank"]][m_commands["RD16A"]] = Lambdas::Action::Bank::PRE<LPDDR5>;
      m_actions[m_levels["bank"]][m_commands["WR16A"]] = Lambdas::Action::Bank::PRE<LPDDR5>;
    };

    void set_preqs() {
      m_preqs.resize(m_levels.size(), std::vector<PreqFunc_t<Node>>(m_commands.size()));

      // Rank Preqs
      m_preqs[m_levels["rank"]][m_commands["REFab"]] = Lambdas::Preq::Rank::RequireAllBanksClosed<LPDDR5>;
      m_preqs[m_levels["rank"]][m_commands["RFMab"]] = Lambdas::Preq::Rank::RequireAllBanksClosed<LPDDR5>;

      m_preqs[m_levels["rank"]][m_commands["REFpb"]] = [this] (Node* node, int cmd, const AddrVec_t& addr_vec, Clk_t clk) {

        for (auto bg : node->m_child_nodes) {
          for (auto bank : bg->m_child_nodes) {
            int num_banks_per_bg = m_organization.count[m_levels["bank"]];
            int flat_bankid = bank->m_node_id + bg->m_node_id * num_banks_per_bg;
            if (flat_bankid == addr_vec[LPDDR5::m_levels["bank"]] || flat_bankid == addr_vec[LPDDR5::m_levels["bank"]] + 8) {
              switch (node->m_state) {
                case m_states["Pre-Opened"]: return m_commands["PRE"];
                case m_states["Opened"]: return m_commands["PRE"];
              }
            }
          }
        }

        return cmd;
      };
      
      m_preqs[m_levels["rank"]][m_commands["RFMpb"]] = m_preqs[m_levels["rank"]][m_commands["REFpb"]];

      // Bank Preqs
      m_preqs[m_levels["bank"]][m_commands["RD16"]] = [] (Node* node, int cmd, const AddrVec_t& addr_vec, Clk_t clk) {
        switch (node->m_state) {
          case m_states["Closed"]: return m_commands["ACT-1"];
          case m_states["Pre-Opened"]: return m_commands["ACT-2"];
          case m_states["Opened"]: {
            if (node->m_row_state.find(0) != node->m_row_state.end()) {
              Node* rank = node->m_parent_node->m_parent_node;
              if (rank->m_final_synced_cycle < clk) {
                return m_commands["CASRD"];
              } else {
                return cmd;
              }
            } else {
              return m_commands["PRE"];
            }
          }    
          default: {
            spdlog::error("[Preq::Bank] Invalid bank state for an RD/WR command!");
            std::exit(-1);      
          } 
        }
      };
      m_preqs[m_levels["bank"]][m_commands["WR16"]] = [] (Node* node, int cmd, const AddrVec_t& addr_vec, Clk_t clk) {
        switch (node->m_state) {
          case m_states["Closed"]: return m_commands["ACT-1"];
          case m_states["Pre-Opened"]: return m_commands["ACT-2"];
          case m_states["Opened"]: {
            if (node->m_row_state.find(0) != node->m_row_state.end()) {
              Node* rank = node->m_parent_node->m_parent_node;
              if (rank->m_final_synced_cycle < clk) {
                return m_commands["CASWR"];
              } else {
                return cmd;
              }
            } else {
              return m_commands["PRE"];
            }
          }    
          default: {
            spdlog::error("[Preq::Bank] Invalid bank state for an RD/WR command!");
            std::exit(-1);      
          } 
        }
      };
    };

    void set_rowhits() {
      m_rowhits.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowhits[m_levels["bank"]][m_commands["RD16"]] = Lambdas::RowHit::Bank::RDWR<LPDDR5>;
      m_rowhits[m_levels["bank"]][m_commands["WR16"]] = Lambdas::RowHit::Bank::RDWR<LPDDR5>;
    }


    void set_rowopens() {
      m_rowopens.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowopens[m_levels["bank"]][m_commands["RD16"]] = Lambdas::RowOpen::Bank::RDWR<LPDDR5>;
      m_rowopens[m_levels["bank"]][m_commands["WR16"]] = Lambdas::RowOpen::Bank::RDWR<LPDDR5>;
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
