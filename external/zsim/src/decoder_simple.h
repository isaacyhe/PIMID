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
    uint32_t approxInstrs;
    // Number of REP-prefixed string instructions (rep movs/stos/lods) in this
    // BBL. Their per-iteration memory accesses are intentionally NOT decoded
    // into static uops (the dynamic iteration count varies per execution of
    // the same cached TB); the cores drain those accesses through the cache at
    // serial L1 throughput and, when repInstrs > 0, classify the drained
    // accesses as expected rep-string traffic instead of decode mismatches.
    uint32_t repInstrs;
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
