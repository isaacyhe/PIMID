/** Simplified BBL decoder for QEMU-based instrumentation.
 *
 * Creates BblInfo from instruction count and byte size, which is sufficient
 * for a core that reads only instrs/bytes (ALUCore, NullCore; and the
 * injected timing charges every core takes). The decoded cores (OOO,
 * in-order) get their micro-ops from x86_decoder.h; a block built here has
 * none and runs their synthetic fallback. (1.12.1: SimpleCore, the IPC=1
 * model this was written for, is retired.)
 *
 * This replaces decoder.h's dependency on Pin/XED when building with
 * ZSIM_USE_QEMU.
 */

#ifndef DECODER_SIMPLE_H_
#define DECODER_SIMPLE_H_

#include <stdint.h>
#include <cstddef>
#include "zsim_types.h"

// Minimal uop types (needed by core.h / BblInfo)
enum UopType : uint8_t {UOP_GENERAL, UOP_LOAD, UOP_STORE, UOP_STORE_ADDR, UOP_FENCE};

#define MAX_UOP_SRC_REGS 2
#define MAX_UOP_DST_REGS 2

struct DynUop {
    uint16_t rs[MAX_UOP_SRC_REGS];
    uint16_t rd[MAX_UOP_DST_REGS];
    uint16_t lat;
    uint16_t decCycle;
    UopType type;
    uint8_t portMask;
    uint8_t extraSlots;
    uint8_t pad;

    void clear() {
        rs[0] = rs[1] = 0;
        rd[0] = rd[1] = 0;
        lat = 0;
        decCycle = 0;
        type = UOP_GENERAL;
        portMask = 0;
        extraSlots = 0;
        pad = 0;
    }
};

struct DynBbl {
    uint64_t addr;
    uint32_t uops;
    /* 1.12.18 (ruling 2026-10-23 #2): approxInstrs and repInstrs are
     * counts of instructions in ONE decoded block, which the plugin caps at
     * 1024 instructions (qemu_zsim_plugin.cpp, numInsns <= 1024; larger TBs
     * take the simple path with no DynBbl body), so 16 bits hold them
     * exactly. The two freed bytes x 2 carry fpCensusIdx below, so
     * offsetof(DynBbl, uop) stays 20, every decoded block's allocation size
     * is unchanged and the global-heap layout is untouched. */
    uint16_t approxInstrs;
    // Number of REP-prefixed string instructions (rep movs/stos/lods) in this
    // BBL. Their per-iteration memory accesses are intentionally NOT decoded
    // into static uops (the dynamic iteration count varies per execution of
    // the same cached TB); the cores drain those accesses through the cache at
    // serial L1 throughput and, when repInstrs > 0, classify the drained
    // accesses as expected rep-string traffic instead of decode mismatches.
    uint16_t repInstrs;
    /* 1.12.18: index of this block's FP-operation census
     * (FpOpCensus, below) in the process-local census table; 0 = the block
     * has no FP add/mul/div/sqrt/FMA/x87 operation (or the table is full).
     * PIMID 1.12.18: nor any counted integer operation. */
    uint32_t fpCensusIdx;
    DynUop uop[1];

    static uint32_t bytes(uint32_t uops) {
        return offsetof(DynBbl, uop) + sizeof(DynUop) * uops;
    }

    void init(uint64_t _addr, uint32_t _uops, uint32_t _approxInstrs) {
        uops = _uops;
        approxInstrs = _approxInstrs;
        repInstrs = 0;
    }
};

static_assert(offsetof(DynBbl, uop) == 20, "1.12.18: DynBbl header must stay 20 bytes (fpCensusIdx packing)");

/* 1.12.18 (user ruling 2026-10-23 #2): STATIC census of a decoded
 * block's floating-point OPERATIONS by type and precision, from the x86
 * decoder (x86_decoder.h decodeOne: opcode, the 66/F2/F3 or VEX.pp prefix
 * that selects ps/pd/ss/sd, VEX.L for 256-bit, VEX.W for FMA precision).
 * ins[] counts instructions, lanes[] the scalar operations they perform
 * (a packed addps does 4 single-precision adds). Categories:
 *   0 AddS  add/sub, single      1 MulS  multiply, single
 *   2 AddD  add/sub, double      3 MulD  multiply, double
 *   4 FmaS  fused mul-add, single  5 FmaD  fused mul-add, double
 *   6 DivS  divide or sqrt, single 7 DivD  divide or sqrt, double
 *   8 X87   x87 stack FP (precision set at run time by the control word)
 * min/max, compares, converts and shuffles are none of these (they stay in
 * the decoder's FP class only). x86 SSE/AVX2 has no half-precision
 * arithmetic, so there is no FP16 category.
 * The census lives OUTSIDE BblInfo (which must keep its 24-byte layout,
 * core.h static_assert): a chunked table in the zsim global heap that never
 * moves a record once written, indexed by DynBbl::fpCensusIdx (see
 * decoder_simple.cpp for why the global heap). */
#define FP_CENSUS_CATS 9
/* PIMID 1.12.18: (user ruling 2026-10-23 #3) the same record also
 * carries the block's STATIC census of scalar GPR integer operations by
 * class and operand width (x86_decoder.h intCensusCat):
 *   0 Alu8  1 Alu16  2 Alu32  3 Alu64   add/sub/adc/sbb/cmp/and/or/xor/test/
 *                                       not/neg/inc/dec/shift/rotate/bit ops
 *   4 Mul8  5 Mul16  6 Mul32  7 Mul64   imul/mul
 *   8 Div                               div/idiv, any width
 * Width: the byte opcode forms are 8 bit; otherwise REX.W (VEX.W for the
 * BMI forms) 64, a 66 prefix 16, else 32 (Intel SDM Vol. 2, 2.2.1 / 3.1.1.1
 * operand-size attribute). Moves, LEA, NOPs and packed-integer SIMD are not
 * counted (no published per-op value applies). A subset of mixInt / mixMul.
 * BblInfo and the DynBbl header are unchanged (the record is reached through
 * the same fpCensusIdx). */
#define INT_CENSUS_CATS 9
struct FpOpCensus {
    uint32_t ins[FP_CENSUS_CATS];
    uint32_t lanes[FP_CENSUS_CATS];
    uint32_t iops[INT_CENSUS_CATS];   /* PIMID 1.12.18 */
};
/* Store a census; returns its index (>= 1), or 0 if the table is full. */
uint32_t fpCensusAdd(const FpOpCensus& c);
/* The census at a non-zero index returned by fpCensusAdd. */
const FpOpCensus& fpCensusAt(uint32_t idx);
/* Stat-name suffixes of the categories (fpIns<Name>, fpLanes<Name>). */
extern const char* const kFpCensusNames[FP_CENSUS_CATS];
/* PIMID 1.12.18: stat-name suffixes of the integer categories (intOps<Name>). */
extern const char* const kIntCensusNames[INT_CENSUS_CATS];

struct BblInfo;  // defined in core.h

// Temporary register offsets (same formulas as decoder.h)
#define MAX_INSTR_LOADS 4
#define MAX_INSTR_STORES 4
#define REG_LOAD_TEMP (REG_LAST + 1)
#define REG_STORE_TEMP (REG_LOAD_TEMP + MAX_INSTR_LOADS)
#define REG_STORE_ADDR_TEMP (REG_STORE_TEMP + MAX_INSTR_STORES)
#define REG_EXEC_TEMP (REG_STORE_ADDR_TEMP + MAX_INSTR_STORES)
#define MAX_REGISTERS (REG_EXEC_TEMP + 64)

/** Create a BblInfo for a basic block with given instruction count and byte size.
 *
 * This allocates from the global heap (gm_malloc) so it persists across
 * phases. The BblInfo is suitable for a core that only uses instrs/bytes.
 *
 * 1.11.16: synth=true marks an INJECTED timing charge (barrier latency, PCIe
 * launch/transfer, drain trailer) whose "instrs" are cycles, not code; cores
 * count them into syntheticInstrs so the power model can subtract them from
 * the retired base. Real-code fallbacks (undecoded large TBs, trace replay)
 * keep the default false. */
BblInfo* createSimpleBblInfo(uint32_t instrs, uint32_t bytes, bool synth = false);

#endif  // DECODER_SIMPLE_H_
