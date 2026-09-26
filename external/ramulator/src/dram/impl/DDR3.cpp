#include "dram/dram.h"
#include "dram/lambdas.h"
#include "dram/pimid_bank_open.h"   // PIMID 1.11.91 (R8-7)

namespace Ramulator {

class DDR3 : public IDRAM, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAM, DDR3, "DDR3", "DDR3 Device Model")

  public:
  /* PIMID 1.9.10: datasheet IDD/VDD presets (Micron 4Gb DDR3L-1600 (no VPP)) -- relocated energy layer.
     Intensive per-access model lives in dram/pimid_energy.h (active source); these
     mirror it in upstream convention for Ramulator2's command-driven power path. */
    inline static const std::map<std::string, std::vector<double>> voltage_presets = {
      {"Default",     {1.35,  0.0}},
    };
    inline static const std::map<std::string, std::vector<double>> current_presets = {
      // name         IDD0 IDD2N IDD3N IDD4R IDD4W IDD5B  IPP0 IPP2N IPP3N IPP4R IPP4W IPP5B
      {"Default",     {60,32,45,175,180,210, 3,3,3,3,3,60}},
    };

    inline static const std::map<std::string, Organization> org_presets = {
      //   name         density  DQ   Ch Ra Ba   Ro     Co
      {"DDR3_1Gb_x4",   {1<<10,  4,  {1, 1, 8, 1<<14, 1<<11}}},
      {"DDR3_1Gb_x8",   {1<<10,  8,  {1, 1, 8, 1<<14, 1<<10}}},
      {"DDR3_1Gb_x16",  {1<<10,  16, {1, 1, 8, 1<<13, 1<<10}}},
      {"DDR3_2Gb_x4",   {2<<10,  4,  {1, 1, 8, 1<<15, 1<<11}}},
      {"DDR3_2Gb_x8",   {2<<10,  8,  {1, 1, 8, 1<<15, 1<<10}}},
      {"DDR3_2Gb_x16",  {2<<10,  16, {1, 1, 8, 1<<14, 1<<10}}},
      {"DDR3_4Gb_x4",   {4<<10,  4,  {1, 1, 8, 1<<16, 1<<11}}},
      {"DDR3_4Gb_x8",   {4<<10,  8,  {1, 1, 8, 1<<16, 1<<10}}},
      {"DDR3_4Gb_x16",  {4<<10,  16, {1, 1, 8, 1<<15, 1<<10}}},
      /* 1.11.63 (calibration): the two 8 Gb narrow-DQ rows re-shaped to JEDEC.
       * SOURCE: JESD79-3D section 2.11.5 "8Gb", printed p.16 (PDF p.30):
       *     Configuration    2Gb x 4              1Gb x 8         512Mb x 16
       *     # of Banks       8                    8               8
       *     Row Address      A0 - A15             A0 - A15        A0 - A15
       *     Column Address   A0 - A9, A11, A13    A0 - A9, A11    A0 - A9
       *     Page size        2 KB                 2 KB            2 KB
       * so at 8 Gb every configuration has 16 row-address bits (65536 rows)
       * and a 2 KB page; JEDEC reaches 8 Gb from 4 Gb by DOUBLING THE PAGE,
       * not the row count. The old rows reached it by doubling the 4 Gb part's
       * rows (1<<17) and keeping the 4 Gb page, giving a bank with twice the
       * rows and half the page of the standard's part -- directly wrong for a
       * PIM row-buffer/subarray model. DDR3_4Gb_x8 in this same map is
       * {1<<16, 1<<10} and matches sec 2.11.4 exactly, which is what shows the
       * 8 Gb rows to be a per-density error rather than a units convention.
       *   x4:  A0-A9,A11,A13 = 12 column bits = 4096 columns; 4096 x 4 / 8 = 2 KB.
       *   x8:  A0-A9,A11     = 11 column bits = 2048 columns; 2048 x 8 / 8 = 2 KB.
       * Density still closes: 8 x 65536 x 4096 x 4 = 8 x 65536 x 2048 x 8
       * = 8,589,934,592 bits = 8192 Mb (the set_organization() assertion below
       * re-checks it). DDR3_8Gb_x16 was already right (65536 rows, A0-A9 =
       * 1024 columns x 16 = 2 KB) and is untouched.
       * CONSEQUENTIAL: the 2 KB page changes which tRRD/tFAW row of Table 66
       * this part takes -- see the page-size-keyed tables in set_timing_vals(). */
      {"DDR3_8Gb_x4",   {8<<10,  4,  {1, 1, 8, 1<<16, 1<<12}}},
      {"DDR3_8Gb_x8",   {8<<10,  8,  {1, 1, 8, 1<<16, 1<<11}}},
      {"DDR3_8Gb_x16",  {8<<10,  16, {1, 1, 8, 1<<16, 1<<10}}},
    };

    /* 1.11.63 (calibration): nCWL is now per speed bin, and the tCK_ps column
     * mirrors the derivation.
     *
     * nCWL: every one of the fourteen rows carried the blanket constant 9.
     * SOURCE: JESD79-3D Figure 11 "MR2 Definition", clause 3.4.4, printed p.30
     * (PDF p.44), the A5/A4/A3 CAS Write Latency field, verbatim:
     *     000  5  (tCK(avg) >= 2.5 ns)
     *     001  6  (2.5 ns  > tCK(avg) >= 1.875 ns)
     *     010  7  (1.875 ns > tCK(avg) >= 1.5 ns)
     *     011  8  (1.5 ns  > tCK(avg) >= 1.25 ns)
     *     100  9  (1.25 ns > tCK(avg) >= 1.07 ns)
     *     101  10 (1.07 ns > tCK(avg) >= 0.935 ns)
     * CWL is a function of tCK alone, so each bin has exactly one legal value:
     *     800  -> tCK 2.500 ns -> CWL 5     1600 -> tCK 1.250 ns -> CWL 8
     *     1066 -> tCK 1.876 ns -> CWL 6     1866 -> tCK 1.071 ns -> CWL 9
     *     1333 -> tCK 1.500 ns -> CWL 7     2133 -> tCK 0.937 ns -> CWL 10
     * The audited row, DDR3_1600H, is corroborated a second time by JESD79-3D
     * Table 63, printed p.161 (PDF p.175): DDR3-1600's "Supported CWL Settings"
     * are "5, 6, 7, 8" -- 9 is not among them. The old 9 was a real one-cycle
     * error in write latency at 1600 and a four-cycle one at 800.
     *
     * tCK_ps: 1066 1875 -> 1876. set_timing_vals() derives tCK = 2e6/rate and
     * overwrites this column; 2e6/1066 = 1876.2 ps. The column is now a
     * checked mirror of the derivation (see the equality check below). The
     * 1333 (1500) and 2133 (937) columns were already the 2e6/rate values and
     * are unchanged -- what changed under them is the derivation itself, which
     * used to truncate rate/2 and produce 1501 and 938.
     *
     * Untouched, and stated so: nRRD/nFAW carry -1 here and are filled from
     * the page-size-keyed tables below; nCS = 2 is a Ramulator controller
     * constant with no JESD79-3D counterpart (UNCHECKABLE). */
    inline static const std::map<std::string, std::vector<int>> timing_presets = {
      //   name       rate    nBL  nCL  nRCD  nRP   nRAS  nRC   nWR  nRTP nCWL nCCD  nRRD  nWTR  nFAW  nRFC nREFI  nCS  tCK_ps
      {"DDR3_800D",   {800,    4,   5,   5,    5,    15,  20,    6,   4,   5,    4,   -1,    4,   -1,  -1,   -1,    2,  2500}},
      {"DDR3_800E",   {800,    4,   5,   5,    5,    15,  20,    6,   4,   5,    4,   -1,    4,   -1,  -1,   -1,    2,  2500}},
      {"DDR3_1066E",  {1066,   4,   6,   6,    6,    20,  26,    8,   4,   6,    4,   -1,    4,   -1,  -1,   -1,    2,  1876}},
      {"DDR3_1066F",  {1066,   4,   7,   7,    7,    20,  27,    8,   4,   6,    4,   -1,    4,   -1,  -1,   -1,    2,  1876}},
      {"DDR3_1066G",  {1066,   4,   8,   8,    8,    20,  28,    8,   4,   6,    4,   -1,    4,   -1,  -1,   -1,    2,  1876}},
      {"DDR3_1333G",  {1333,   4,   8,   8,    8,    24,  32,   10,   5,   7,    4,   -1,    5,   -1,  -1,   -1,    2,  1500}},
      {"DDR3_1333H",  {1333,   4,   9,   9,    9,    24,  33,   10,   5,   7,    4,   -1,    5,   -1,  -1,   -1,    2,  1500}},
      {"DDR3_1600H",  {1600,   4,   9,   9,    9,    28,  37,   12,   6,   8,    4,   -1,    6,   -1,  -1,   -1,    2,  1250}},
      {"DDR3_1600J",  {1600,   4,  10,  10,   10,    28,  38,   12,   6,   8,    4,   -1,    6,   -1,  -1,   -1,    2,  1250}},
      {"DDR3_1600K",  {1600,   4,  11,  11,   11,    28,  39,   12,   6,   8,    4,   -1,    6,   -1,  -1,   -1,    2,  1250}},
      {"DDR3_1866K",  {1866,   4,  11,  11,   11,    32,  43,   14,   7,   9,    4,   -1,    7,   -1,  -1,   -1,    2,  1071}},
      {"DDR3_1866L",  {1866,   4,  12,  12,   12,    32,  44,   14,   7,   9,    4,   -1,    7,   -1,  -1,   -1,    2,  1071}},
      {"DDR3_2133L",  {2133,   4,  12,  12,   12,    36,  48,   16,   8,  10,    4,   -1,    8,   -1,  -1,   -1,    2,  937}},
      {"DDR3_2133M",  {2133,   4,  13,  13,   13,    36,  49,   16,   8,  10,    4,   -1,    8,   -1,  -1,   -1,    2,  937}},
    };

  /************************************************
   *                Organization
   ***********************************************/   
    const int m_internal_prefetch_size = 8;

    inline static constexpr ImplDef m_levels = {
      "channel", "rank", "bank", "row", "column",    
    };


  /************************************************
   *             Requests & Commands
   ***********************************************/
    inline static constexpr ImplDef m_commands = {
      "ACT", 
      "PRE", "PREA",
      "RD",  "WR",  "RDA",  "WRA",
      "REFab",
    };

    inline static const ImplLUT m_command_scopes = LUT (
      m_commands, m_levels, {
        {"ACT",   "row"},
        {"PRE",   "bank"},   {"PREA",   "rank"},
        {"RD",    "column"}, {"WR",     "column"}, {"RDA",   "column"}, {"WRA",   "column"},
        {"REFab", "rank"},
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
      }
    );

    inline static constexpr ImplDef m_requests = {
      "read", "write", "all-bank-refresh",
    };

    inline static const ImplLUT m_request_translations = LUT (
      m_requests, m_commands, {
        {"read", "RD"}, {"write", "WR"}, {"all-bank-refresh", "REFab"},
      }
    );

   
  /************************************************
   *                   Timing
   ***********************************************/
    inline static constexpr ImplDef m_timings = {
      "rate", 
      "nBL", "nCL", "nRCD", "nRP", "nRAS", "nRC", "nWR", "nRTP", "nCWL",
      "nCCD",
      "nRRD",
      "nWTR",
      "nFAW",
      "nRFC", "nREFI",
      "nCS",
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
        {"rank",      "PowerUp"},
        {"bank",      "Closed"},
        {"row",       "Closed"},
        {"column",    "N/A"},
      }
    );

  public:
    struct Node : public DRAMNodeBase<DDR3> {
      Node(DDR3* dram, Node* parent, int level, int id) : DRAMNodeBase<DDR3>(dram, parent, level, id) {};
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
      size_t _density = size_t(m_organization.count[m_levels["bank"]]) *
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
       * 1500, DDR3-2133 938 instead of 937); it is now the exact 2e6/rate the
       * column documents. DDR3 is a DDR-clocked interface -- one data beat per
       * CK edge, 2 bits/pin per CK -- so 2e6/rate is the CK period. */
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

      /* 1.11.63 (calibration): tRRD/tFAW are keyed on PAGE SIZE, not DQ width.
       * SOURCE: JESD79-3D Table 66 "Timing Parameters by Speed Bin", printed
       * p.170 (PDF p.184), which tabulates each of the two parameters twice:
       *   "ACTIVE to ACTIVE command period for 1KB page size  tRRD"
       *   "ACTIVE to ACTIVE command period for 2KB page size  tRRD"
       *   "Four activate window for 1KB page size  tFAW"
       *   "Four activate window for 2KB page size  tFAW"
       * The old tables were indexed by DQ width. That was a working PROXY only
       * because, before the 8 Gb org fix above, every x4/x8 preset had a 1 KB
       * page and every x16 preset a 2 KB page. The corrected DDR3_8Gb_x4 and
       * DDR3_8Gb_x8 are 2 KB parts, so the proxy now selects the wrong row for
       * exactly the org PIMID simulates: DDR3_8Gb_x8 would take tRRD 5 / tFAW
       * 24 (the 1 KB figures) where JESD79-3D gives it the 2 KB figures.
       * The two surviving rows are the file's own former x4/x8 row (= the 1 KB
       * row of Table 66) and its former x16 row (= the 2 KB row), verified
       * cell by cell against the standard for the four bins it covers:
       *   1 KB: tRRD max(4nCK,10ns/7.5/6/6) -> 4,4,4,5 ; tFAW 40/37.5/30/30 ns
       *         -> 16,20,20,24  at tCK 2.5/1.876/1.5/1.25 ns
       *   2 KB: tRRD max(4nCK,10ns/10/7.5/7.5) -> 4,6,5,6 ; tFAW 50/50/45/40 ns
       *         -> 20,27,30,32
       * The 1866 and 2133 columns are outside Table 66 (JESD79-3D splits the
       * upper bins into a separate table) and are carried over unchanged;
       * they are not the audited bin. */
      int page_id = [](int page_size_B) -> int {
        switch (page_size_B) {
          case 1024: return 0;   // 1 KB page row of Table 66
          case 2048: return 1;   // 2 KB page row of Table 66
          default:   return -1;
        }
      }(m_organization.count[m_levels["column"]] * m_organization.dq / 8);
      if (page_id < 0) {
        throw ConfigurationError(
          "In \"{}\", the organization's page size of {} B is neither 1 KB nor "
          "2 KB, and JESD79-3D Table 66 (printed p.170) tabulates tRRD/tFAW "
          "for those two page sizes only!",
          get_name(), m_organization.count[m_levels["column"]] * m_organization.dq / 8);
      }

      int rate_id = [](int rate) -> int {
        switch (rate) {
          case  800:  return 0;
          case 1066:  return 1;
          case 1333:  return 2;
          case 1600:  return 3;
          case 1866:  return 4;
          case 2133:  return 5;
          default:    return -1;
        }
      }(m_timing_vals("rate"));

      constexpr int nRRD_TABLE[2][6] = {
      // 800   1066  1333  1600  1866  2133
        { 4,    4,    4,    5,    5,    6},   // 1 KB page (Table 66 p.170)
        { 4,    6,    5,    6,    6,    7},   // 2 KB page (Table 66 p.170)
      };
      constexpr int nFAW_TABLE[2][6] = {
      // 800   1066  1333  1600  1866  2133
        { 16,   20,   20,   24,   26,   27},  // 1 KB page (Table 66 p.170)
        { 20,   27,   30,   32,   33,   34},  // 2 KB page (Table 66 p.170)
      };

      if (rate_id != -1) {
        m_timing_vals("nRRD") = nRRD_TABLE[page_id][rate_id];
        m_timing_vals("nFAW") = nFAW_TABLE [page_id][rate_id];
      }

      // Refresh timings
      // tRFC table (unit is nanosecond!)
      /* 1.11.63 (calibration): tRFC[4Gb] 260 -> 300 ns.
       * SOURCE: JESD79-3D Table 59 "Refresh parameters by device density",
       * clause 12.2, printed p.156 (PDF p.170), verbatim:
       *   Parameter                Symbol  512Mb  1Gb  2Gb  4Gb  8Gb  Units
       *   REF command to ACT or
       *   REF command time         tRFC    90     110  160  300  350  ns
       *   Average periodic
       *   refresh interval         tREFI   7.8    7.8  7.8  7.8  7.8  us
       * The 1/2/8 Gb columns and tREFI_BASE below were already right; only the
       * 4 Gb column was low, by 40 ns (13%). Not the density PIMID simulates
       * (it emits DDR3_8Gb_x8) but wrong, and it is the number the tree's own
       * energy layer had copied -- see the matching pimid_energy.h fix. */
      constexpr int tRFC_TABLE[4] = {
      // 1Gb   2Gb   4Gb   8Gb
         110,  160,  300,  350,
      };

      // tREFI(base) table (unit is nanosecond!)
      constexpr int tREFI_BASE = 7800;
      int density_id = [](int density_Mb) -> int { 
        switch (density_Mb) {
          case 1024:  return 0;
          case 2048:  return 1;
          case 4096:  return 2;
          case 8192:  return 3;
          default:    return -1;
        }
      }(m_organization.density);
      /* PIMID 1.11.59 (audit F037-class): the lambda returns -1 for any
       * density outside the four tabulated ones, and that -1 then indexed
       * tRFC_TABLE. An out-of-bounds read one element BEFORE a table of
       * plausible nanosecond figures does not crash -- it yields whatever
       * adjacent constant the linker put there, and the run continues with a
       * refresh time nobody can trace, which is why an unrecognised density
       * silently produced a plausible-looking timing set instead of an error.
       * Checked before adding this: the twelve shipped org presets cover
       * 1/2/4/8 Gb only (DDR3_1Gb_*, DDR3_2Gb_*, DDR3_4Gb_*, DDR3_8Gb_*) and
       * the switch handles exactly those four, so no shipped preset reaches
       * the throw -- only a hand-written org/density override does. Refuse at
       * construction, in the same form as the unrecognized-preset check
       * above. */
      if (density_id < 0) {
        throw ConfigurationError(
          "In \"{}\", organization density {} Mb has no tabulated refresh "
          "timing (tRFC is tabulated for 1/2/4/8 Gb only)!",
          get_name(), m_organization.density);
      }

      m_timing_vals("nRFC")  = JEDEC_rounding(tRFC_TABLE[density_id], tCK_ps);
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

          /*** Rank ***/ 
          // CAS <-> CAS
          {.level = "rank", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA"}, .latency = V("nCCD")},
          {.level = "rank", .preceding = {"WR", "WRA"}, .following = {"WR", "WRA"}, .latency = V("nCCD")},
          /// RD <-> WR, Minimum Read to Write, Assuming tWPRE = 1 tCK                          
          {.level = "rank", .preceding = {"RD", "RDA"}, .following = {"WR", "WRA"}, .latency = V("nCL") + V("nBL") + 2 - V("nCWL")},
          /// WR <-> RD, Minimum Read after Write
          {.level = "rank", .preceding = {"WR", "WRA"}, .following = {"RD", "RDA"}, .latency = V("nCWL") + V("nBL") + V("nWTR")},
          /// CAS <-> CAS between sibling ranks, nCS (rank switching) is needed for new DQS
          {.level = "rank", .preceding = {"RD", "RDA"}, .following = {"RD", "RDA", "WR", "WRA"}, .latency = V("nBL") + V("nCS"), .is_sibling = true},
          {.level = "rank", .preceding = {"WR", "WRA"}, .following = {"RD", "RDA"}, .latency = V("nCL")  + V("nBL") + V("nCS") - V("nCWL"), .is_sibling = true},
          /// CAS <-> PREab
          {.level = "rank", .preceding = {"RD"}, .following = {"PREA"}, .latency = V("nRTP")},
          {.level = "rank", .preceding = {"WR"}, .following = {"PREA"}, .latency = V("nCWL") + V("nBL") + V("nWR")},          
          /// RAS <-> RAS
          {.level = "rank", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRRD")},          
          {.level = "rank", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nFAW"), .window = 4},          
          {.level = "rank", .preceding = {"ACT"}, .following = {"PREA"}, .latency = V("nRAS")},          
          {.level = "rank", .preceding = {"PREA"}, .following = {"ACT"}, .latency = V("nRP")},          
          /// RAS <-> REF
          {.level = "rank", .preceding = {"ACT"}, .following = {"REFab"}, .latency = V("nRC")},          
          {.level = "rank", .preceding = {"PRE", "PREA"}, .following = {"REFab"}, .latency = V("nRP")},          
          {.level = "rank", .preceding = {"RDA"}, .following = {"REFab"}, .latency = V("nRP") + V("nRTP")},          
          {.level = "rank", .preceding = {"WRA"}, .following = {"REFab"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},          
          {.level = "rank", .preceding = {"REFab"}, .following = {"ACT"}, .latency = V("nRFC")},          

          /*** Bank ***/ 
          {.level = "bank", .preceding = {"ACT"}, .following = {"ACT"}, .latency = V("nRC")},  
          {.level = "bank", .preceding = {"ACT"}, .following = {"RD", "RDA", "WR", "WRA"}, .latency = V("nRCD")},  
          {.level = "bank", .preceding = {"ACT"}, .following = {"PRE"}, .latency = V("nRAS")},  
          {.level = "bank", .preceding = {"PRE"}, .following = {"ACT"}, .latency = V("nRP")},  
          {.level = "bank", .preceding = {"RD"},  .following = {"PRE"}, .latency = V("nRTP")},  
          {.level = "bank", .preceding = {"WR"},  .following = {"PRE"}, .latency = V("nCWL") + V("nBL") + V("nWR")},  
          {.level = "bank", .preceding = {"RDA"}, .following = {"ACT"}, .latency = V("nRTP") + V("nRP")},  
          {.level = "bank", .preceding = {"WRA"}, .following = {"ACT"}, .latency = V("nCWL") + V("nBL") + V("nWR") + V("nRP")},  
        }
      );
      #undef V

    };

    void set_actions() {
      m_actions.resize(m_levels.size(), std::vector<ActionFunc_t<Node>>(m_commands.size()));

      // Rank Actions
      m_actions[m_levels["rank"]][m_commands["PREA"]] = Lambdas::Action::Rank::PREab<DDR3>;

      // Bank actions
      m_actions[m_levels["bank"]][m_commands["ACT"]] = Lambdas::Action::Bank::ACT<DDR3>;
      m_actions[m_levels["bank"]][m_commands["PRE"]] = Lambdas::Action::Bank::PRE<DDR3>;
      m_actions[m_levels["bank"]][m_commands["RDA"]] = Lambdas::Action::Bank::PRE<DDR3>;
      m_actions[m_levels["bank"]][m_commands["WRA"]] = Lambdas::Action::Bank::PRE<DDR3>;
    };

    void set_preqs() {
      m_preqs.resize(m_levels.size(), std::vector<PreqFunc_t<Node>>(m_commands.size()));

      // Rank Actions
      m_preqs[m_levels["rank"]][m_commands["REFab"]] = Lambdas::Preq::Rank::RequireAllBanksClosed<DDR3>;

      // Bank actions
      m_preqs[m_levels["bank"]][m_commands["RD"]] = Lambdas::Preq::Bank::RequireRowOpen<DDR3>;
      m_preqs[m_levels["bank"]][m_commands["WR"]] = Lambdas::Preq::Bank::RequireRowOpen<DDR3>;
    };

    void set_rowhits() {
      m_rowhits.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowhits[m_levels["bank"]][m_commands["RD"]] = Lambdas::RowHit::Bank::RDWR<DDR3>;
      m_rowhits[m_levels["bank"]][m_commands["WR"]] = Lambdas::RowHit::Bank::RDWR<DDR3>;
    }


    void set_rowopens() {
      m_rowopens.resize(m_levels.size(), std::vector<RowhitFunc_t<Node>>(m_commands.size()));

      m_rowopens[m_levels["bank"]][m_commands["RD"]] = Lambdas::RowOpen::Bank::RDWR<DDR3>;
      m_rowopens[m_levels["bank"]][m_commands["WR"]] = Lambdas::RowOpen::Bank::RDWR<DDR3>;
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
