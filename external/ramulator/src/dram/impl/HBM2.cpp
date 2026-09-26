#include "dram/dram.h"
#include "dram/lambdas.h"
#include "dram/pimid_bank_open.h"   // PIMID 1.11.91 (R8-7)

namespace Ramulator {

class HBM2 : public IDRAM, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAM, HBM2, "HBM2", "HBM2 Device Model")

  public:
  /* PIMID 1.9.10: datasheet IDD/VDD presets (JESD235 HBM2) -- relocated energy layer.
     Intensive per-access model lives in dram/pimid_energy.h (active source); these
     mirror it in upstream convention for Ramulator2's command-driven power path. */
    inline static const std::map<std::string, std::vector<double>> voltage_presets = {
      {"Default",     {1.2,  2.5}},
    };
    inline static const std::map<std::string, std::vector<double>> current_presets = {
      // name         IDD0 IDD2N IDD3N IDD4R IDD4W IDD5B  IPP0 IPP2N IPP3N IPP4R IPP4W IPP5B
      {"Default",     {28,17,21,80,90,65, 3,3,3,3,3,40}},
    };

    /* 1.11.64 (JESD235D, misc/JESD235D.pdf): THE BANK/ROW SPLIT, corrected.
     *
     * The comment this replaces asserted "Density scales via ROWS, not banks".
     * Table 4 ("HBM Channel Addressing"), printed p.6, says JEDEC scales BOTH,
     * and in that order -- banks first, then rows:
     *   2 Gb/channel (1 Gb/PC):  Bank Address BA[2:0] =  8 banks, RA[13:0] = 16384 rows
     *   4 Gb/channel (2 Gb/PC):  Bank Address BA[3:0] = 16 banks, RA[13:0] = 16384 rows
     *   8 Gb/channel (4 Gb/PC):  Bank Address BA[3:0] = 16 banks, RA[14:0] = 32768 rows
     * with "Page Size per PC 1 KB" at every density. Bank groups: Table 5
     * p.8 assigns 16 banks to four groups ("0 and 1 ... 12 to 15 Group D").
     *
     * The 2 Gb row was already right. The 4 Gb and 8 Gb rows each carried
     * HALF the banks and TWICE the rows, which keeps the density product
     * exact -- that is why it survived every completeness check -- while
     * describing a part with half the bank-level parallelism and twice the
     * row space. On a bank-placement PIM model that is a first-order error:
     * it halves the banks available to interleave over and dilutes the
     * row-hit rate at fixed footprint.
     *
     * WHAT DELIBERATELY DOES NOT CHANGE (verified before touching this, the
     * check that caught the equivalent HBM3 proposal as a defect): dq = 128
     * with 64 columns and m_internal_prefetch_size = 2 reproduce JEDEC's
     * per-PC quantities exactly -- page = 64 x 128 = 8192 b = 1 KB/PC, and
     * access granularity = 128 x 2n = 256 b = 32 B, matching Table 4's 1 KB
     * page and the standard's 32 B/256-bit prefetch. JEDEC factors the same
     * products as 64 DQ x 32 columns x 4n. Re-factoring to JEDEC's shape
     * without also moving the prefetch would HALVE the access granularity,
     * so the factorization is left alone and documented instead. */
    inline static const std::map<std::string, Organization> org_presets = {
      //   name     density   DQ    Ch Pch  Bg Ba   Ro     Co
      {"HBM2_2Gb",   {2<<10,  128,  {1, 2,  4,  2, 1<<14, 1<<6}}},   //  8 banks/PC, RA[13:0]
      {"HBM2_4Gb",   {4<<10,  128,  {1, 2,  4,  4, 1<<14, 1<<6}}},   // 16 banks/PC, RA[13:0]
      {"HBM2_8Gb",   {8<<10,  128,  {1, 2,  4,  4, 1<<15, 1<<6}}},   // 16 banks/PC, RA[14:0]
    };

    inline static const std::map<std::string, std::vector<int>> timing_presets = {
      //   name       rate   nBL  nCL  nRCDRD  nRCDWR  nRP  nRAS  nRC  nWR  nRTPS  nRTPL  nCWL  nCCDS  nCCDL  nRRDS  nRRDL  nWTRS  nWTRL  nRTW  nFAW  nRFC  nRFCSB  nREFI  nREFISB  nRREFD  tCK_ps
      // HBM2 @ 2.4 Gb/s with JEDEC JESD235C spec-minimum physical timings
      // (tRCD=tRP=16ns, tRAS=33ns, tRC=49ns, tCL=16ns, tWR=16ns, tFAW=16ns,
      // tREFI=3.9us). Cycle counts derived at tCK=833ps.
      // Replaces the earlier placeholder "HBM2_2Gbps" preset, which encoded
      // physically-impossible ~7ns row timings (~2x faster than any real HBM2).
      /* PIMID 1.11.59 (audit F036): the nRFC/nRFCSB/nREFISB columns are -1,
       * because get_timing_vals() overwrites all three from the per-density
       * tables further down before anything reads them. This line used to
       * carry nRFC=265 with a comment claiming "tRFC=220ns @4Gb" -- a value
       * and a source that no run ever used: the table supplies 260 ns at the
       * emitted HBM2_4Gb org, so the refresh occupancy this model simulates
       * is 18% longer than the preset's own comment stated. The number was
       * dead and the comment beside it read as the authority, which is the
       * worse half. -1 is this file's idiom for "supplied elsewhere" and the
       * completeness check below still catches a column nothing fills. */
      /* 1.11.63 (calibration, JESD235D acquired 2026-08-24 -- misc/): the
       * full HBM1/HBM2 standard replaced the earlier identity-only pass, and
       * two of that pass's fixes turned out to have patched identities onto
       * wrong anchors (details below). Sources are page-exact; printed page
       * = PDF page - 8.
       *
       * ANCHOR: Table 58 p.102 ("Timings used for IDD Measurement-Loop
       * Pattern") is the ONLY place JESD235D prints tRC/tRAS/tRP numbers --
       * Table 68's row-timing MIN cells are deliberately blank (NOTE 2
       * p.111: vendors define speed bins). Table 58 gives tRP 15 ns, tRAS
       * 33 ns, tRC 48 ns, and Table 68's own NOTE makes nRAS + nRP = nRC
       * normative; 33 + 15 = 48 closes exactly. This row's previous comment
       * claimed tRP 16 / tRC 49 "JESD235C spec-minimum" -- neither number
       * appears anywhere in 235D.
       *
       * CORRECTED against the standard:
       *   nBL   4 -> 2. PC mode forces BL4 (Table 12 NOTE 2 p.17) and BL4 =
       *     4 UI = 2 CK; Table 68 p.109 permits the next column command
       *     tCCDS = 2 nCK later, which a 4-CK occupancy could not. (The
       *     63a raise of nCCDS to "the identity floor nBL = 4" was patching
       *     the identity onto a wrong nBL.)
       *   nRP   20 -> 18 (tRP 15 ns, Table 58 p.102; JEDEC_rounding = 18).
       *   nRC   60 -> 58 (tRC 48 ns, same table; = nRAS 40 + nRP 18).
       *   nCCDS  4 -> 2 (Table 68 p.109: "different bank groups BL=4 tCCDS
       *     2 nCK").
       *   nCCDL  5 -> 4 (Table 68 p.109: "same bank group BL=4 tCCDL
       *     MAX(4, 2.8ns/tCK)" = MAX(4, 3.36) = 4).
       *   nRTW  10 -> 17 (Table 68 NOTE 23 p.112 closed form with this
       *     row's RL 20 / WL 10 / BL4, tDQSS(min) -0.2 tCK, tDQSCK(max)
       *     3.5 ns, tDQSQ(max) 71 ps at 2.4 Gbps = 13.82 ns -> 17 nCK.
       *     INERT: populate_timingcons never reads nRTW -- its live RD->WR
       *     rule is nCL + nBL + 2 - nCWL -- kept correct for the
       *     completeness check's sake.)
       *   nRREFD 8 -> 10. UNIT ERROR: Table 68 p.110 gives tRREFD = 8 ns;
       *     the column carried the 8 as CYCLES (6.66 ns).
       *     JEDEC_rounding(8 ns, 833 ps) = 10.
       *   nREFI 4682 -> 4681. tREFI 3.9 us is a MAX (Table 68 p.110), so
       *     the rounding is FLOOR: 3900/0.833 = 4681.87 -> 4681.
       *
       * CALIBRATED, unchanged: rate (2.4 Gbps/pin bin heads Table 67
       * p.107), tCK 833 ps (same table), nRAS 40 (tRAS 33 ns, Table 58).
       *
       * VENDOR-ONLY, unchanged and stated (Table 68 MIN cells blank; RL/WL
       * are MR2 ranges 3..48 / 1..16 nCK, Table 11 p.17): nCL, nCWL,
       * nRCDRD, nRCDWR, nWR, nRTPS, nRTPL, nRRDS, nRRDL, nWTRS, nWTRL,
       * nFAW. All IDD/IPP values likewise (Table 65 p.105 / Table 66 p.106
       * publish EMPTY value columns by construction).
       *
       * KNOWN-DRIFTED, STAGED for the next release (R2 precedent: address-
       * layout movers ship separately for attribution): the ORG presets.
       * Table 4 p.6 at 4 Gb/channel gives BA[3:0] = 16 banks/PC (4 groups x
       * 4) and RA[13:0] = 16384 rows; this org carries 8 banks/PC x 32768
       * rows -- density closes, layout does not. Same table: PC unit is
       * 64 DQ / 4n prefetch / 32 columns x 256 b; this org's 128 DQ x 64
       * columns x 2n factors the same 1 KB page the legacy-mode way. */
      {"HBM2_2.4Gbps", {2400, 2, 20, 20, 20, 18, 40, 58, 20, 5, 10, 10, 2, 4, 4, 5, 5, 10, 17, 20, -1, -1, 4681, -1, 10, 833}},
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
      "REFab", "REFsb"
    };

    inline static const ImplLUT m_command_scopes = LUT (
      m_commands, m_levels, {
        {"ACT",   "row"},
        {"PRE",   "bank"},    {"PREA",   "channel"},
        {"RD",    "column"},  {"WR",     "column"}, {"RDA",   "column"}, {"WRA",   "column"},
        {"REFab", "channel"}, {"REFsb",  "bank"},
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
      }
    );

    inline static constexpr ImplDef m_requests = {
      "read", "write", "all-bank-refresh", "per-bank-refresh"
    };

    inline static const ImplLUT m_request_translations = LUT (
      m_requests, m_commands, {
        {"read", "RD"}, {"write", "WR"}, {"all-bank-refresh", "REFab"}, {"per-bank-refresh", "REFsb"},
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
    struct Node : public DRAMNodeBase<HBM2> {
      Node(HBM2* dram, Node* parent, int level, int id) : DRAMNodeBase<HBM2>(dram, parent, level, id) {};
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
      /* 1.11.63 (calibration, cross-cutting fix -- same block in all 11 impl
       * files): ONE AUTHORITY FOR tCK. This derivation is the authority; the
       * tCK_ps column in the preset row above is a MIRROR of it and is now
       * checked against it, so the two cannot silently disagree again. The
       * divisor was `1E6 / (rate / 2)`, whose integer division threw away the
       * half-MT/s of odd rates; it is now the exact 2e6/rate the column
       * documents. At 2400 MT/s the two forms agree at 833 ps, so nothing
       * moves here -- the check is what is new.
       * HBM2 IS a 2 bits/pin/CK interface (CK 1.2 GHz at 2.4 Gb/s), unlike
       * HBM3, whose CK runs at a quarter of the data rate -- see HBM3.cpp. */
      int preset_tCK_ps = m_timing_vals("tCK_ps");
      int tCK_ps = 2E6 / m_timing_vals("rate");
      m_timing_vals("tCK_ps") = tCK_ps;
      if (preset_provided && preset_tCK_ps != tCK_ps) {
        throw ConfigurationError(
          "In \"{}\", the timing preset's tCK_ps column says {} ps but the "
          "rate of {} MT/s derives {} ps (tCK = 2e6/rate). The derivation "
          "wins at run time, so the column must mirror it -- fix the preset!",
          get_name(), preset_tCK_ps, m_timing_vals("rate"), tCK_ps);
      }

      // Refresh timings
      // tRFC table (unit is nanosecond!)
      constexpr int tRFC_TABLE[1][4] = {
      //  2Gb   4Gb   8Gb  16Gb
        { 160,  260,  350,  450},
      };

      /* PIMID 1.11.59 (audit F034): this header said "tRFC table" over the
       * same-bank refresh INTERVAL table -- the copy-paste that hid the wrong
       * read below for as long as it stood. */
      // tREFIsb table (same-bank refresh INTERVAL, unit is nanosecond!)
      /* 1.11.63 (JESD235D): the row was SHIFTED ONE DENSITY COLUMN -- it
       * encoded JEDEC's 1/2/4/8 Gb-per-channel sequence under 2/4/8/16 Gb
       * labels. Table 68 p.111: 0.4875 us for 1-2 Gb/channel, 0.2438 us for
       * 4-8 Gb/channel; no 16 Gb/channel cell exists, so that entry is
       * DERIVED from NOTE 29 p.112 ("tREFISB = tREFI / N; N = no. of
       * banks") with Table 4's SID+BA[3:0] = 32 banks: 3.9 us / 32 = 1219
       * ns. The NOTE-29 identity reproduces every published cell (3.9/8,
       * 3.9/16), which is the cross-check. Old 4 Gb value was 2x long. */
      constexpr int tREFISB_TABLE[1][4] = {
      //  2Gb    4Gb    8Gb    16Gb
        { 4875,  2438,  2438,  1219},
      };

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
      /* PIMID 1.11.59 (audit F034): nREFISB read tRFC_TABLE -- the refresh
       * TIME table -- while tREFISB_TABLE, the same-bank refresh INTERVAL
       * defined just above, sat unread. HBM3.cpp was corrected in 1.11.51 and
       * this twin was not, so the same upstream copy-paste survived here: at
       * the emitted HBM2_4Gb org the interval came out as 260 ns instead of
       * 4875 ns, 18.75x too frequent. Invisible because nREFISB enters no
       * timing constraint in populate_timingcons and the emitted refresh
       * manager is always AllBank, which never consumes it -- so a value that
       * was wrong by 18.75x sat in a completeness-checked table that reads as
       * though every entry had been derived. */
      m_timing_vals("nREFISB")  = JEDEC_rounding(tREFISB_TABLE[0][density_id], tCK_ps);
      /* PIMID 1.11.51 (N9): nRFCSB was never filled, so every instantiation
       * of this model died in the -1 completeness check. Filled with tRFC as
       * a stated UPPER BOUND (same reasoning as HBM3.cpp; AllBank refresh
       * never consumes it).
       *
       * PIMID 1.11.59 (audit F035): the `if (nRFCSB == -1)` this replaces was
       * unconditionally true -- the one shipped preset sets that column to -1
       * -- while nRFC and nREFISB beside it were overwritten with no guard at
       * all. The guard therefore documented a preset-wins precedence rule
       * that the other two columns did not follow and that nothing exercised.
       * All three are unconditional now, and the preset carries -1 in all
       * three columns to say so (F036). */
      /* 1.11.63 (JESD235D): tRFCSB is a SEPARATE PUBLISHED TABLE, not an
       * upper bound borrowed from tRFC. Table 68 p.110: "SINGLE BANK REFRESH
       * command period (same bank) tRFCSB 160 ns" for 1/2/4/8 Gb PER DIE
       * (NOTE 34 p.112: "Density is given per die"), 200 ns for 12/16 Gb per
       * die. Every shipped org (2/4/8 Gb per CHANNEL, at 1-2 channels per
       * die) lands in the 160 ns row; the 1.11.51 tRFC placeholder was 1.63x
       * high (313 vs 193 cycles at 4 Gb). Still inert under the AllBank
       * refresh manager -- corrected for the table's own integrity. */
      constexpr int tRFCSB_NS = 160;   // JESD235D Tbl 68 p.110, 1-8 Gb/die
      m_timing_vals("nRFCSB") = JEDEC_rounding(tRFCSB_NS, tCK_ps);

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
          {.level = "channel", .preceding = {"ACT"}, .following = {"ACT", "PRE", "PREA", "REFab", "REFsb"}, .latency = 2},

          /*** Pseudo Channel (Table 3 -- Array Access Timings Counted Individually Per Pseudo Channel, JESD-235C) ***/ 
          // RAS <-> RAS
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRRDS")},
          /// 4-activation window restriction
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nFAW"), .window = 4},

          /// ACT actually happens on the 2-nd cycle of ACT, so +1 cycle to nRRD
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"REFsb"}, .latency = V("nRRDS") + 1},
          /// nRREFD is the latency between REFsb <-> REFsb to *different* banks
          {.level = "pseudochannel", .preceding = {"REFsb"}, .following = {"REFsb"}, .latency = V("nRREFD")},
          /// nRREFD is the latency between REFsb <-> ACT to *different* banks. -1 as ACT happens on its 2nd cycle
          {.level = "pseudochannel", .preceding = {"REFsb"}, .following = {"ACT"}, .latency = V("nRREFD") - 1},

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
          {.level = "pseudochannel", .preceding = {"ACT"}, .following = {"REFab"}, .latency = V("nRC")},          
          {.level = "pseudochannel", .preceding = {"PRE", "PREA"}, .following = {"REFab"}, .latency = V("nRP")},          
          {.level = "pseudochannel", .preceding = {"RDA"}, .following = {"REFab"}, .latency = V("nRP") + V("nRTPS")},          
          {.level = "pseudochannel", .preceding = {"WRA"}, .following = {"REFab"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},          
          {.level = "pseudochannel", .preceding = {"REFab"}, .following = {"ACT", "REFsb"}, .latency = V("nRFC")},          

          /*** Same Bank Group ***/ 
          /// CAS <-> CAS
          {.level = "bankgroup", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA"}, .latency = V("nCCDL")},          
          {.level = "bankgroup", .preceding = {"WR", "WRA"}, .following = {"WR", "WRA"}, .latency = V("nCCDL")},          
          {.level = "bankgroup", .preceding = {"WR", "WRA"}, .following = {"RD", "RDA"}, .latency = V("nCWL") + V("nBL") + V("nWTRL")},
          /// RAS <-> RAS
          {.level = "bankgroup", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRRDL")},  
          {.level = "bankgroup", .preceding = {"ACT"}, .following = {"REFsb"}, .latency = V("nRRDL") + 1},  
          {.level = "bankgroup", .preceding = {"REFsb"}, .following = {"ACT"}, .latency = V("nRRDL") - 1},  

          {.level = "bank", .preceding = {"RD"},  .following = {"PRE"}, .latency = V("nRTPS")},  


          /*** Bank ***/ 
          {.level = "bank", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRC")},  
          {.level = "bank", .preceding = {"ACT"}, .following = {"RD", "RDA"}, .latency = V("nRCDRD")},  
          {.level = "bank", .preceding = {"ACT"}, .following = {"WR", "WRA"}, .latency = V("nRCDWR")},  
          {.level = "bank", .preceding = {"ACT"}, .following = {"PRE"}, .latency = V("nRAS")},  
          {.level = "bank", .preceding = {"PRE"}, .following = {"ACT"}, .latency = V("nRP")},  
          {.level = "bank", .preceding = {"RD"},  .following = {"PRE"}, .latency = V("nRTPL")},  
          {.level = "bank", .preceding = {"WR"},  .following = {"PRE"}, .latency = V("nCWL") + V("nBL") + V("nWR")},  
          {.level = "bank", .preceding = {"RDA"}, .following = {"ACT", "REFsb"}, .latency = V("nRTPL") + V("nRP")},  
          {.level = "bank", .preceding = {"WRA"}, .following = {"ACT", "REFsb"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},  
        }
      );
      #undef V

    };

    void set_actions() {
      m_actions.resize(m_levels.size(), std::vector<ActionFunc_t<Node>>(m_commands.size()));

      // Channel Actions
      m_actions[m_levels["channel"]][m_commands["PREA"]] = Lambdas::Action::Channel::PREab<HBM2>;

      // Bank actions
      m_actions[m_levels["bank"]][m_commands["ACT"]] = Lambdas::Action::Bank::ACT<HBM2>;
      m_actions[m_levels["bank"]][m_commands["PRE"]] = Lambdas::Action::Bank::PRE<HBM2>;
      m_actions[m_levels["bank"]][m_commands["RDA"]] = Lambdas::Action::Bank::PRE<HBM2>;
      m_actions[m_levels["bank"]][m_commands["WRA"]] = Lambdas::Action::Bank::PRE<HBM2>;
    };

    void set_preqs() {
      m_preqs.resize(m_levels.size(), std::vector<PreqFunc_t<Node>>(m_commands.size()));

      // Channel Actions
      m_preqs[m_levels["channel"]][m_commands["REFab"]] = Lambdas::Preq::Channel::RequireAllBanksClosed<HBM2>;

      // Bank actions
      m_preqs[m_levels["bank"]][m_commands["REFsb"]] = Lambdas::Preq::Bank::RequireBankClosed<HBM2>;
      m_preqs[m_levels["bank"]][m_commands["RD"]] = Lambdas::Preq::Bank::RequireRowOpen<HBM2>;
      m_preqs[m_levels["bank"]][m_commands["WR"]] = Lambdas::Preq::Bank::RequireRowOpen<HBM2>;
    };

    void set_rowhits() {
      m_rowhits.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowhits[m_levels["bank"]][m_commands["RD"]] = Lambdas::RowHit::Bank::RDWR<HBM2>;
      m_rowhits[m_levels["bank"]][m_commands["WR"]] = Lambdas::RowHit::Bank::RDWR<HBM2>;
    }


    void set_rowopens() {
      m_rowopens.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowopens[m_levels["bank"]][m_commands["RD"]] = Lambdas::RowOpen::Bank::RDWR<HBM2>;
      m_rowopens[m_levels["bank"]][m_commands["WR"]] = Lambdas::RowOpen::Bank::RDWR<HBM2>;
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