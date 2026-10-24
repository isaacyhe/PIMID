/** Minimal x86-64 instruction decoder for QEMU-driven OOO simulation.
 *
 * Purpose (PIMID "option B"): under QEMU/TCG the plugin only sees raw
 * instruction bytes, not a decoded micro-op stream with register operands.
 * ZSim's OOOCore, however, contains a full dependency-driven pipeline that
 * consumes DynUop descriptors (source/dest register sets, latency class,
 * functional-unit port masks, load/store markers). This decoder recovers just
 * enough per-instruction semantics to build those DynUops so the REAL OOO path
 * (ROB + dataflow-order issue + port contention + load/store queues) runs,
 * instead of the synthetic 1-CPI fallback.
 *
 * Scope: this is NOT a full x86 decoder. It classifies the common integer,
 * SSE, and AVX(VEX) forms that dominate compiled compute kernels (movs, ALU,
 * imul/idiv, FP add/mul/div/fma, load-op, store, rmw, lea, push/pop, branches)
 * and produces faithful dependency + latency + memory-access uops for them.
 * Anything it does not recognize is emitted as a single generic ALU uop with
 * no memory uop; any memory accesses such an instruction actually performs are
 * absorbed by the OOO core's tolerant load/store drain (see ooo_core.cpp), so
 * counts never desync and the simulator never asserts. Instructions decoded
 * approximately are tallied in BblInfo->oooBbl[0].approxInstrs.
 *
 * Register numbering: we only need a consistent injective map from
 * architectural registers to scoreboard indices in [1, REG_LAST). It need NOT
 * match XED's enum. All widths of a GPR alias to one id (al/ax/eax/rax share).
 *
 * This header is included ONLY by qemu_zsim_plugin.cpp (static linkage), so no
 * build-system or ODR concerns. No external decoder library is required.
 *
 * ASCII only. SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef X86_DECODER_H_
#define X86_DECODER_H_

#include <stdint.h>
#include <string.h>
#include <vector>
#include "core.h"
#include "decoder_simple.h"
#include "galloc.h"

namespace x86dec {

/* ---- Scoreboard register ids (all < REG_LAST == 512) ---- */
static const uint16_t SB_NONE     = 0;    /* "no register" (scoreboard[0] is
                                             reset to curCycle every uop) */
static const uint16_t SB_GPR_BASE = 1;    /* rax..r15 -> 1..16 */
static const uint16_t SB_FLAGS    = 17;   /* x86 RFLAGS (single lumped reg) */
static const uint16_t SB_XMM_BASE = 64;   /* xmm0..xmm31 -> 64..95 */

static inline uint16_t gprId(int r) { return (uint16_t)(SB_GPR_BASE + (r & 31)); }
static inline uint16_t xmmId(int r) { return (uint16_t)(SB_XMM_BASE + (r & 31)); }

/* ---- Functional-unit ports (bit per port; see ooo_core WindowStructure) ---- */
static const uint8_t P0 = 0x01, P1 = 0x02, P2 = 0x04 /*load*/,
                     P3 = 0x08, P4 = 0x10 /*store*/, P5 = 0x20;
static const uint8_t PORTS_ALU   = P0 | P1 | P5;  /* 3 integer ALU ports */
static const uint8_t PORTS_FADD  = P1;
static const uint8_t PORTS_FMUL  = P0;
static const uint8_t PORTS_FMA   = P0 | P1;
static const uint8_t PORTS_FDIV  = P0;
static const uint8_t PORTS_IMUL  = P0;
static const uint8_t PORTS_LOAD  = P2;
static const uint8_t PORTS_STORE = P4;
static const uint8_t PORTS_BR    = P5;

/* ---- Latency/port class ---- */
enum OpClass {
    C_ALU, C_MOV, C_LEA, C_IMUL, C_IDIV,
    C_FADD, C_FMUL, C_FDIV, C_FMA, C_VECMOV, C_VECALU,
    C_BRANCH, C_NOP, C_GENERIC
};

struct ClassInfo { uint8_t lat; uint8_t port; uint8_t extraSlots; };

static inline ClassInfo classInfo(OpClass c) {
    switch (c) {
        case C_ALU:    return {1,  PORTS_ALU,   0};
        case C_MOV:    return {1,  PORTS_ALU,   0};
        case C_LEA:    return {1,  P0 | P1,     0};
        case C_IMUL:   return {3,  PORTS_IMUL,  0};
        case C_IDIV:   return {20, PORTS_IMUL,  9};
        case C_FADD:   return {3,  PORTS_FADD,  0};
        case C_FMUL:   return {5,  PORTS_FMUL,  0};
        case C_FDIV:   return {20, PORTS_FDIV,  9};
        case C_FMA:    return {5,  PORTS_FMA,   0};
        case C_VECMOV: return {1,  PORTS_ALU,   0};
        case C_VECALU: return {3,  PORTS_FADD,  0};
        case C_BRANCH: return {1,  PORTS_BR,    0};
        case C_NOP:    return {1,  PORTS_ALU,   0};
        default:       return {1,  PORTS_ALU,   0};
    }
}

/* Memory role of the (single) memory operand, if any. */
enum MemRole { MEM_NONE, MEM_LOAD, MEM_STORE, MEM_RMW, MEM_LEA /*addr only, no access*/ };

/* Register-file of a register operand. */
enum RegFile { RF_GPR, RF_VEC };

/* Fully-resolved per-instruction decode result used to emit uops. */
struct Decoded {
    OpClass  cls;
    MemRole  mem;
    bool     hasModrm;
    bool     isPureMove;   /* value moves mem<->reg with no computation */
    /* 1.11.47 (FIX-PRE-FLEET L201): vector-INTEGER marker. The census used to
     * collapse C_VECALU/C_VECMOV (and packed-int C_FMUL) into the FP bucket,
     * so paddb/pxor-class integer SIMD was priced as floating point AND
     * charged soft-float emulation on FPU-less elements. Timing classes are
     * untouched -- this flag corrects CLASSIFICATION only. */
    bool     vecInt;
    bool     regIsSrc, regIsDst;
    bool     rmIsSrc,  rmIsDst;
    bool     readsFlags, writesFlags;
    bool     approx;
    bool     fence;        /* lock-prefixed / atomic / explicit fence -> serialize */
    /* 1.12.18: FP-operation census category + 1 (FpOpCensus,
     * decoder_simple.h; 0 = not a classified FP operation) and the scalar
     * operations the instruction performs (lanes). */
    uint8_t  fpCat1;
    uint8_t  fpLanes;
    /* PIMID 1.12.18: (ruling 2026-10-23 #3) operand size of a GPR
     * instruction from the prefixes: REX.W (VEX.W on VEX forms) 64, 66h 16,
     * else 32; the byte opcode forms are recognised in intCensusCat. */
    uint8_t  intW;
    /* grp3 (0xF6/0xF7 /4../7) widening-mul / divide rdx:rax pair semantics:
     * 0 = not grp3 mul/div; 1 = widening mul (reads rax, writes rdx:rax);
     * 2 = div/idiv (reads rdx:rax AND rm, writes rdx:rax -> needs 2 uops);
     * 3 = 8-bit form (al/ah/ax all alias the rax scoreboard id -> the
     *     single-register model is already exact). */
    uint8_t  gp3;
    /* String ops: 0 = none, 1 = movs, 2 = stos, 3 = lods. repPrefixed set for
     * F3/F2 forms (dynamic iteration count -> memory uops intentionally not
     * emitted; the cores drain the actual QEMU-delivered accesses at serial
     * L1 throughput and classify them as expected rep traffic). */
    uint8_t  strKind;
    bool     repPrefixed;
    /* resolved register ids (SB_NONE if absent) */
    uint16_t regId;        /* ModRM.reg operand */
    uint16_t rmRegId;      /* ModRM.rm operand when it is a register (mod==3) */
    uint16_t baseId;       /* memory base GPR (address computation) */
    uint16_t indexId;      /* memory index GPR */
    uint16_t vvvvId;       /* VEX 3rd operand source (SB_NONE if none) */
    /* profiling signature (for PIMID_OOO_DUMP): opcode map/byte/reg-ext */
    uint8_t  dbgMapIdx;    /* 0=1B, 1=0F, 2=0F38, 3=0F3A */
    uint8_t  dbgOp;        /* primary opcode byte */
    uint8_t  dbgReg;       /* ModRM.reg (group extension), 0 if none */
};

/* Pack a decode signature into a 16-bit key: [mapIdx:2][op:8][reg:3]. */
static inline uint16_t dbgKey(const Decoded& d) {
    return (uint16_t)(((d.dbgMapIdx & 3) << 11) | ((uint16_t)d.dbgOp << 3) | (d.dbgReg & 7));
}
static inline void dbgUnpack(uint16_t k, int& mapIdx, int& op, int& reg) {
    mapIdx = (k >> 11) & 3; op = (k >> 3) & 0xFF; reg = k & 7;
}

/* ---- Small helper to append a uop ---- */
static inline void pushUop(std::vector<DynUop>& v, UopType type, uint16_t rs0,
                           uint16_t rs1, uint16_t rd0, uint16_t rd1,
                           uint16_t lat, uint8_t port, uint8_t extraSlots,
                           uint32_t decCycle) {
    DynUop u; u.clear();
    u.rs[0] = rs0; u.rs[1] = rs1;
    u.rd[0] = rd0; u.rd[1] = rd1;
    u.lat = lat;
    u.type = type;
    u.portMask = port ? port : PORTS_ALU;  /* never 0 (would hang scheduler) */
    u.extraSlots = extraSlots;
    u.decCycle = (uint16_t)decCycle;
    v.push_back(u);
}

/* ==================================================================== *
 *  Instruction classifier
 *  Parses prefixes/REX/VEX + opcode + ModRM from the raw bytes (length
 *  is authoritative from QEMU, so we only parse up to the ModRM/SIB we
 *  need). Fills `d`. Returns true if a ModRM was consumed.
 * ==================================================================== */
static inline bool decodeOne(const uint8_t* b, uint32_t len, Decoded& d) {
    memset(&d, 0, sizeof(d));
    d.cls = C_GENERIC;
    d.mem = MEM_NONE;
    d.regId = d.rmRegId = d.baseId = d.indexId = d.vvvvId = SB_NONE;

    uint32_t i = 0;
    bool p66 = false, pF2 = false, pF3 = false, pLock = false;
    int rex = 0;
    bool vex = false; int vexMap = 1; int vvvv = 0; bool vexW = false;
    bool vexL = false;   /* 1.12.18: VEX.L, 256-bit vector length */

    /* legacy prefixes */
    while (i < len) {
        uint8_t c = b[i];
        if (c == 0x66) { p66 = true; i++; }
        else if (c == 0xF2) { pF2 = true; i++; }
        else if (c == 0xF3) { pF3 = true; i++; }
        else if (c == 0xF0) { pLock = true; i++; }  /* LOCK -> fenced rmw */
        else if (c == 0x67 || c == 0x2E || c == 0x36 || c == 0x3E ||
                 c == 0x26 || c == 0x64 || c == 0x65) { i++; }
        else break;
    }
    if (i >= len) { d.cls = C_NOP; return false; }

    /* VEX / REX */
    int map = 1;  /* 1 == one-byte, 0x0F, 0x0F38, 0x0F3A distinguished below */
    uint8_t op = 0;
    if (b[i] == 0xC5 && i + 1 < len) {           /* 2-byte VEX */
        vex = true; uint8_t v1 = b[i + 1];
        vvvv = (~(v1 >> 3)) & 0xF;
        vexL = ((v1 >> 2) & 1) != 0;   /* 1.12.18 */
        int pp = v1 & 3; p66 = (pp == 1); pF3 = (pp == 2); pF2 = (pp == 3);
        vexMap = 1; map = 0x0F;
        if ((v1 >> 7) == 0) rex |= 4; /* REX.R */
        i += 2; if (i >= len) return false; op = b[i++];
    } else if (b[i] == 0xC4 && i + 2 < len) {    /* 3-byte VEX */
        vex = true; uint8_t v1 = b[i + 1], v2 = b[i + 2];
        vexMap = v1 & 0x1F;
        vvvv = (~(v2 >> 3)) & 0xF;
        vexW = (v2 >> 7) & 1; (void)vexW;
        vexL = ((v2 >> 2) & 1) != 0;   /* 1.12.18 */
        int pp = v2 & 3; p66 = (pp == 1); pF3 = (pp == 2); pF2 = (pp == 3);
        if ((v1 >> 7) == 0) rex |= 4;  /* REX.R = ~bit7 */
        if (((v1 >> 6) & 1) == 0) rex |= 2;  /* REX.X = ~bit6 */
        if (((v1 >> 5) & 1) == 0) rex |= 1;  /* REX.B = ~bit5 */
        map = (vexMap == 2) ? 0x0F38 : (vexMap == 3) ? 0x0F3A : 0x0F;
        i += 3; if (i >= len) return false; op = b[i++];
    } else {
        /* REX prefix: low nibble carries B(bit0)/X(bit1)/R(bit2)/W(bit3). */
        if (i < len && b[i] >= 0x40 && b[i] <= 0x4F) { rex = b[i] & 0xF; i++; }
        if (i >= len) return false;
        if (b[i] == 0x0F) {
            i++;
            if (i >= len) return false;
            if (b[i] == 0x38) { map = 0x0F38; i++; }
            else if (b[i] == 0x3A) { map = 0x0F3A; i++; }
            else map = 0x0F;
            if (i >= len) return false;
            op = b[i++];
        } else {
            map = 1; op = b[i++];
        }
    }
    (void)p66; (void)pF2; (void)vex; (void)vexMap;
    d.intW = (uint8_t)(vex ? (vexW ? 64 : 32) : ((rex & 8) ? 64 : (p66 ? 16 : 32)));   /* PIMID 1.12.18: */
    /* 1.12.18 (ruling 2026-10-23 #2): SSE/AVX FP arithmetic format
     * from the mandatory prefix (or VEX.pp): F3 = ss (1 single), F2 = sd
     * (1 double), 66 = pd (2 doubles), none = ps (4 singles); VEX.L doubles
     * the packed lane count (256-bit). kind: 0 add/sub, 1 mul, 2 div/sqrt. */
    auto sseFp = [&](int kind) {
        const bool dbl = pF2 || (p66 && !pF3);
        const bool scal = pF3 || pF2;
        d.fpLanes = (uint8_t)(scal ? 1 : (dbl ? 2 : 4) * (vexL ? 2 : 1));
        const int cat = (kind == 0) ? (dbl ? 2 : 0) : (kind == 1) ? (dbl ? 3 : 1) : (dbl ? 7 : 6);
        d.fpCat1 = (uint8_t)(cat + 1);
    };
    bool rexR = (rex & 4) != 0;
    bool rexX = (rex & 2) != 0;
    bool rexB = (rex & 1) != 0;
    if (vvvv) d.vvvvId = xmmId(vvvv);  /* VEX 3rd operand is a vector reg for the
                                          FP forms we model */

    /* ---- Does this opcode have a ModRM? (for the forms we classify) ---- */
    bool hasModrm = true;
    if (map == 1) {
        /* one-byte opcodes without ModRM that we may encounter */
        if ((op >= 0x50 && op <= 0x5F) ||      /* push/pop reg */
            (op >= 0xB0 && op <= 0xBF) ||      /* mov reg, imm */
            op == 0x68 || op == 0x6A ||        /* push imm */
            op == 0xE8 || op == 0xE9 || op == 0xEB || /* call/jmp rel */
            (op >= 0x70 && op <= 0x7F) ||      /* jcc rel8 */
            op == 0xC3 || op == 0xC2 ||        /* ret */
            op == 0xC9 || op == 0x90 ||        /* leave / nop */
            op == 0x99 || op == 0x98 ||        /* cltd / cltq */
            (op >= 0x04 && op <= 0x05) ||      /* al/eax imm arith share below via &7 */
            op == 0xCC)
            hasModrm = false;
    } else if (map == 0x0F) {
        if (op >= 0x80 && op <= 0x8F) hasModrm = false; /* jcc rel32 */
        if (op == 0x05 || op == 0x0B || op == 0x31 || op == 0x77 ||
            op == 0xA2 || op == 0x0E) hasModrm = false; /* syscall/ud2/rdtsc/... */
    }
    d.hasModrm = hasModrm;

    /* ---- Parse ModRM/SIB to resolve operands ---- */
    int mod = 3, reg = 0, rm = 0;
    bool memOperand = false;
    if (hasModrm && i < len) {
        uint8_t modrm = b[i++];
        mod = modrm >> 6;
        reg = ((modrm >> 3) & 7) | (rexR ? 8 : 0);
        rm  = (modrm & 7) | (rexB ? 8 : 0);
        memOperand = (mod != 3);
        if (memOperand) {
            if ((modrm & 7) == 4) {           /* SIB */
                if (i < len) {
                    uint8_t sib = b[i++];
                    int idx = ((sib >> 3) & 7) | (rexX ? 8 : 0);
                    int bs  = (sib & 7) | (rexB ? 8 : 0);
                    if (((sib >> 3) & 7) != 4) d.indexId = gprId(idx); /* rsp=no index */
                    if (mod == 0 && (sib & 7) == 5) d.baseId = SB_NONE; /* disp32 */
                    else d.baseId = gprId(bs);
                }
            } else if (mod == 0 && (modrm & 7) == 5) {
                d.baseId = SB_NONE;            /* RIP-relative */
            } else {
                d.baseId = gprId(rm);
            }
        }
    }

    /* Record profiling signature (map/opcode/reg-extension). */
    d.dbgMapIdx = (map == 1) ? 0 : (map == 0x0F) ? 1 : (map == 0x0F38) ? 2 : 3;
    d.dbgOp = op;
    d.dbgReg = (uint8_t)(reg & 7);
    d.fence = pLock;   /* LOCK prefix => the rmw serializes as a fence */

    /* Default operand register files */
    RegFile regF = RF_GPR, rmF = RF_GPR;

    /* Convenience: default roles for a two-operand op using direction bit.
     * For the classic arithmetic/mov encodings, bit1 of the opcode selects
     * RM (dest=reg) vs MR (dest=rm). */
    auto setRegIds = [&](RegFile rf_reg, RegFile rf_rm) {
        regF = rf_reg; rmF = rf_rm;
        d.regId = (rf_reg == RF_VEC) ? xmmId(reg) : gprId(reg);
        if (!memOperand) d.rmRegId = (rf_rm == RF_VEC) ? xmmId(rm) : gprId(rm);
    };

    /* ============================ classify ============================ */
    if (map == 1) {
        uint8_t hi = op & 0xF8;
        uint8_t lo = op & 0x07;
        if (op == 0x8D) {                      /* lea */
            d.cls = C_LEA; setRegIds(RF_GPR, RF_GPR);
            d.regIsDst = true; d.mem = MEM_LEA;
            return hasModrm;
        }
        if (hi == 0x00 || hi == 0x08 || hi == 0x10 || hi == 0x18 ||
            hi == 0x20 || hi == 0x28 || hi == 0x30 || hi == 0x38) {
            /* arithmetic group: add/or/adc/sbb/and/sub/xor/cmp */
            bool isCmp = (hi == 0x38);
            d.writesFlags = true;
            if (hi == 0x10 || hi == 0x18) d.readsFlags = true; /* adc/sbb */
            if (lo == 4 || lo == 5) {          /* al/eax, imm : no modrm */
                d.cls = C_ALU; d.hasModrm = false;
                d.regId = gprId(0); d.regIsSrc = true; d.regIsDst = !isCmp;
                return false;
            }
            setRegIds(RF_GPR, RF_GPR);
            bool rmIsDest = (lo == 0 || lo == 1);   /* MR form */
            d.cls = C_ALU;
            if (rmIsDest) {
                d.regIsSrc = true;
                d.rmIsSrc = true;                     /* rmw reads dest too */
                d.rmIsDst = !isCmp;
                if (memOperand) d.mem = isCmp ? MEM_LOAD : MEM_RMW;
            } else {                                   /* RM form: dest=reg */
                d.regIsDst = !isCmp; d.regIsSrc = true;
                d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
            }
            return hasModrm;
        }
        switch (op) {
            case 0x88: case 0x89:              /* mov r/m, r  (MR) */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_MOV; d.isPureMove = true;
                d.regIsSrc = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_STORE;
                return hasModrm;
            case 0x8A: case 0x8B:              /* mov r, r/m  (RM) */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_MOV; d.isPureMove = true;
                d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x80: case 0x81: case 0x83: {  /* GROUP 1: <alu> r/m, imm
                                                 * (reg field selects the op).
                                                 * THE loop counter/bound ops. */
                int opidx = reg & 7;             /* 0 add..7 cmp */
                bool isCmp = (opidx == 7);
                d.cls = C_ALU; d.writesFlags = true;
                if (opidx == 2 || opidx == 3) d.readsFlags = true;  /* adc/sbb */
                /* ModRM.reg is the opcode extension here, NOT a register. */
                d.regId = SB_NONE;
                d.rmRegId = memOperand ? SB_NONE : gprId(rm);
                d.rmIsSrc = true;
                d.rmIsDst = !isCmp;
                if (memOperand) d.mem = isCmp ? MEM_LOAD : MEM_RMW;
                return hasModrm;
            }
            case 0x69: case 0x6B:              /* imul r, r/m, imm */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_IMUL; d.writesFlags = true;
                d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0xA8: case 0xA9:              /* test al/eax, imm (no modrm) */
                d.cls = C_ALU; d.hasModrm = false; d.writesFlags = true;
                d.regId = gprId(0); d.regIsSrc = true;
                return false;
            case 0xB0: case 0xB1: case 0xB2: case 0xB3:
            case 0xB4: case 0xB5: case 0xB6: case 0xB7:  /* mov r8, imm8 */
            case 0xB8: case 0xB9: case 0xBA: case 0xBB:
            case 0xBC: case 0xBD: case 0xBE: case 0xBF:  /* mov r32/64, imm */
                d.cls = C_MOV; d.isPureMove = true;
                d.regId = gprId((op & 7) | (rexB ? 8 : 0)); d.regIsDst = true;
                return false;
            case 0x86: case 0x87: {            /* xchg r/m, r (implicitly atomic
                                                * when mem -> fenced rmw) */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_MOV;
                d.regIsSrc = true; d.regIsDst = true;
                d.rmIsSrc = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_RMW;
                return hasModrm;
            }
            case 0x63:                         /* movsxd r64, r/m32 (very common
                                                * array-index sign-extend in loops) */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_MOV; d.isPureMove = true;
                d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x84: case 0x85:              /* test r/m, r */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_ALU; d.writesFlags = true;
                d.regIsSrc = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0xC6: case 0xC7:              /* mov r/m, imm */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_MOV; d.isPureMove = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_STORE;
                return hasModrm;
            case 0xC0: case 0xC1: case 0xD0: case 0xD1:
            case 0xD2: case 0xD3:              /* shift/rotate r/m */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_ALU; d.writesFlags = true;
                d.rmIsSrc = true; d.rmIsDst = true;
                if (op == 0xD2 || op == 0xD3) d.readsFlags = false;
                if (memOperand) d.mem = MEM_RMW;
                return hasModrm;
            case 0xF6: case 0xF7: {            /* grp3: test/not/neg/mul/imul/div/idiv */
                setRegIds(RF_GPR, RF_GPR);
                int ext = reg & 7;
                if (ext == 0 || ext == 1) {   /* test r/m, imm */
                    d.cls = C_ALU; d.writesFlags = true; d.rmIsSrc = true;
                    if (memOperand) d.mem = MEM_LOAD;
                } else if (ext == 2 || ext == 3) { /* not/neg */
                    d.cls = C_ALU; d.rmIsSrc = true; d.rmIsDst = true;
                    if (ext == 3) d.writesFlags = true;
                    if (memOperand) d.mem = MEM_RMW;
                } else {                       /* mul/imul/div/idiv: rdx:rax pair */
                    d.cls = (ext == 6 || ext == 7) ? C_IDIV : C_IMUL;
                    d.rmIsSrc = true;
                    /* Exact pair semantics (emitUops consumes d.gp3):
                     *  - 16/32/64-bit widening mul (F7 /4,/5): reads rax + rm,
                     *    writes BOTH rax (low) and rdx (high) -> gp3=1.
                     *  - 16/32/64-bit div/idiv (F7 /6,/7): reads rdx:rax + rm,
                     *    writes BOTH rax (quotient) and rdx (remainder). Three
                     *    sources exceed the 2-slot uop -> merge uop + div uop
                     *    (gp3=2).
                     *  - 8-bit forms (F6): operate on al/ah/ax, which ALL alias
                     *    the single rax scoreboard id -> one uop reading
                     *    {rm, rax} and writing rax is exact (gp3=3).
                     * FLAGS after mul/div are undefined (mul's OF/CF are defined
                     * but in practice never consumed); dropping the FLAGS write
                     * frees the 2nd dest slot for rdx. */
                    d.gp3 = (op == 0xF6) ? 3 : ((ext == 6 || ext == 7) ? 2 : 1);
                    d.regId = gprId(0);        /* rax */
                    d.regIsSrc = true; d.regIsDst = true;
                    if (memOperand) d.mem = MEM_LOAD;
                }
                return hasModrm;
            }
            case 0xFF: {                       /* grp5: inc/dec/call/jmp/push r/m */
                setRegIds(RF_GPR, RF_GPR);
                int ext = reg & 7;
                if (ext == 0 || ext == 1) {   /* inc/dec */
                    d.cls = C_ALU; d.writesFlags = true;
                    d.rmIsSrc = true; d.rmIsDst = true;
                    if (memOperand) d.mem = MEM_RMW;
                } else if (ext == 6) {        /* push r/m (mem-operand: load value +
                                               * store to stack = 1 load + 1 store) */
                    d.cls = C_MOV; d.rmIsSrc = true;
                    if (memOperand) d.mem = MEM_RMW; else d.mem = MEM_STORE;
                } else {                       /* call/jmp indirect */
                    d.cls = C_BRANCH; d.rmIsSrc = true;
                    if (memOperand) d.mem = MEM_LOAD;
                    if (ext == 2) d.mem = memOperand ? MEM_RMW : MEM_STORE; /* call pushes retaddr */
                }
                return hasModrm;
            }
            case 0x50: case 0x51: case 0x52: case 0x53:
            case 0x54: case 0x55: case 0x56: case 0x57: /* push reg */
                d.cls = C_MOV; d.isPureMove = true; d.mem = MEM_STORE;
                d.regId = gprId((op & 7) | (rexB ? 8 : 0)); d.regIsSrc = true;
                return false;
            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F: /* pop reg */
                d.cls = C_MOV; d.isPureMove = true; d.mem = MEM_LOAD;
                d.regId = gprId((op & 7) | (rexB ? 8 : 0)); d.regIsDst = true;
                return false;
            case 0x68: case 0x6A:              /* push imm */
                d.cls = C_MOV; d.isPureMove = true; d.mem = MEM_STORE; return false;
            case 0xC3: case 0xC2:              /* ret */
                d.cls = C_BRANCH; d.mem = MEM_LOAD; return false;
            case 0xC9:                          /* leave: pop rbp */
                d.cls = C_MOV; d.isPureMove = true; d.mem = MEM_LOAD;
                d.regId = gprId(5); d.regIsDst = true; return false;
            case 0xE8:                          /* call rel: push retaddr */
                d.cls = C_BRANCH; d.mem = MEM_STORE; return false;
            case 0xE9: case 0xEB:              /* jmp rel */
                d.cls = C_BRANCH; return false;
            case 0x90:                          /* nop / pause */
                d.cls = C_NOP; return false;
            case 0x91: case 0x92: case 0x93:   /* xchg r, eax (0x90+r) */
            case 0x94: case 0x95: case 0x96: case 0x97:
                d.cls = C_MOV;
                d.regId = gprId((op & 7) | (rexB ? 8 : 0));
                d.regIsSrc = d.regIsDst = true;
                d.rmRegId = gprId(0);          /* eax */
                d.rmIsSrc = d.rmIsDst = true;
                return false;
            case 0x98: case 0x99:              /* cltq/cltd */
                d.cls = C_ALU; d.regId = gprId(0); d.regIsSrc = d.regIsDst = true;
                return false;
            /* --- string ops (no ModRM). cmps (A6/A7) / scas (AE/AF) remain
             * generic: zero dynamic occurrences in the 5-kernel dump. --- */
            case 0xA4: case 0xA5:              /* movs[bwdq]: [rsi] -> [rdi] */
                d.cls = C_MOV; d.hasModrm = false;
                d.strKind = 1; d.repPrefixed = (pF3 || pF2);
                return false;
            case 0xAA: case 0xAB:              /* stos: rax -> [rdi] */
                d.cls = C_MOV; d.hasModrm = false;
                d.strKind = 2; d.repPrefixed = (pF3 || pF2);
                return false;
            case 0xAC: case 0xAD:              /* lods: [rsi] -> rax */
                d.cls = C_MOV; d.hasModrm = false;
                d.strKind = 3; d.repPrefixed = (pF3 || pF2);
                return false;
            default:
                if (op >= 0x70 && op <= 0x7F) { /* jcc rel8 */
                    d.cls = C_BRANCH; d.readsFlags = true; return false;
                }
                d.cls = C_GENERIC; d.approx = true;
                if (memOperand) d.mem = MEM_NONE; /* unknown mem role -> let drain handle */
                return hasModrm;
        }
    }

    if (map == 0x0F) {
        /* SSE / two-byte forms. Pick register file: most of these are vector. */
        switch (op) {
            case 0x10: case 0x28: case 0x6F:  /* movss/sd/ups/upd, movaps/apd, movdqa/u : RM */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECMOV; d.isPureMove = true;
                d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x11: case 0x29: case 0x7F:  /* store forms MR */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECMOV; d.isPureMove = true;
                d.regIsSrc = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_STORE;
                return hasModrm;
            case 0xD6:                         /* movq xmm -> r/m (store) */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECMOV; d.isPureMove = true;
                d.regIsSrc = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_STORE;
                return hasModrm;
            case 0x2E: case 0x2F:              /* ucomiss/comiss: reads, sets flags */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECALU; d.writesFlags = true;
                d.regIsSrc = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x54: case 0x55: case 0x56: case 0x57: /* andps/andnps/orps/xorps */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECMOV;             /* logic: cheap */
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x51:                         /* sqrt */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_FDIV; d.regIsDst = true; d.rmIsSrc = true;
                sseFp(2);   /* 1.12.18 */
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x58: case 0x5C: case 0x5D: case 0x5F: /* add/sub/min/max */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_FADD;
                if (op == 0x58 || op == 0x5C) sseFp(0);   /* 1.12.18: add/sub only; min/max are compares */
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x59:                         /* mul */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_FMUL; d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                sseFp(1);   /* 1.12.18 */
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x5E:                         /* div */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_FDIV; d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                sseFp(2);   /* 1.12.18 */
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x5A: case 0x5B:              /* cvt ps<->pd / dq */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECALU; d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0xAF:                         /* imul r, r/m */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_IMUL; d.writesFlags = true;
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0xB6: case 0xB7: case 0xBE: case 0xBF: /* movzx/movsx */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_MOV; d.isPureMove = true;
                d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x1F:                         /* multi-byte NOP (no mem access!) */
                d.cls = C_NOP; return hasModrm;
            case 0x2A:                         /* cvtsi2ss/sd : gpr/mem -> xmm */
                setRegIds(RF_VEC, RF_GPR);
                d.cls = C_VECALU; d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x2C: case 0x2D:              /* cvtss2si etc : xmm/mem -> gpr */
                setRegIds(RF_GPR, RF_VEC);
                d.cls = C_VECALU; d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x6E:                         /* movd/movq gpr/mem -> xmm */
                setRegIds(RF_VEC, RF_GPR);
                d.cls = C_VECMOV; d.isPureMove = true;
                d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0x7E:                         /* movd/movq xmm -> gpr/mem (or F3: xmm<-mem) */
                if (pF3) {                     /* movq xmm, xmm/m64 (load form) */
                    setRegIds(RF_VEC, RF_VEC);
                    d.cls = C_VECMOV; d.isPureMove = true;
                    d.regIsDst = true; d.rmIsSrc = true;
                    if (memOperand) d.mem = MEM_LOAD;
                } else {
                    setRegIds(RF_VEC, RF_GPR);
                    d.cls = C_VECMOV; d.isPureMove = true;
                    d.regIsSrc = true; d.rmIsDst = true;
                    if (memOperand) d.mem = MEM_STORE;
                }
                return hasModrm;
            case 0x12: case 0x13: case 0x16: case 0x17: /* movlps/movhps */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECMOV; d.isPureMove = true;
                if (op == 0x13 || op == 0x17) { d.regIsSrc = true; d.rmIsDst = true;
                    if (memOperand) d.mem = MEM_STORE; }
                else { d.regIsDst = true; d.rmIsSrc = true;
                    if (memOperand) d.mem = MEM_LOAD; }
                return hasModrm;
            /* --- hint / NOP forms (0x18..0x1F): prefetch, endbr, multi-byte nop.
             * QEMU does not raise a memory callback for these, so NO mem uop. */
            case 0x18: case 0x19: case 0x1A: case 0x1B:
            case 0x1C: case 0x1D: case 0x1E:
                d.cls = C_NOP; return hasModrm;
            case 0x77:                         /* emms */
                d.cls = C_NOP; return false;
            /* --- SSE2 packed-integer logic (cheap) --- */
            case 0xEF: case 0xEB: case 0xDB: case 0xDF: /* pxor/por/pand/pandn */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECMOV; d.vecInt = true;   /* L201 */
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            /* --- SSE2 packed-integer compare / add / sub (vector ALU) --- */
            case 0x74: case 0x75: case 0x76:   /* pcmpeqb/w/d */
            case 0x64: case 0x65: case 0x66:   /* pcmpgtb/w/d */
            case 0xFC: case 0xFD: case 0xFE: case 0xD4: /* paddb/w/d/q */
            case 0xF8: case 0xF9: case 0xFA: case 0xFB: /* psubb/w/d/q */
            case 0xEC: case 0xED: case 0xDC: case 0xDD: /* padds/paddus */
            case 0xE8: case 0xE9: case 0xD8: case 0xD9: /* psubs/psubus */
            case 0xDA: case 0xDE: case 0xEA: case 0xEE: /* pmin/pmax ub/sw */
            case 0xF6:                         /* psadbw */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECALU; d.vecInt = true;   /* L201 */
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            /* --- SSE2 packed-integer multiply (higher latency) --- */
            case 0xD5: case 0xE4: case 0xE5: case 0xF4: /* pmullw/pmulhuw/pmulhw/pmuludq */
            case 0xF5:                         /* pmaddwd */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_FMUL; d.vecInt = true;     /* L201: integer multiply on the FP unit */
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            /* --- SSE2 packed shifts by xmm/mem --- */
            case 0xD1: case 0xD2: case 0xD3:   /* psrlw/d/q */
            case 0xE1: case 0xE2:              /* psraw/d */
            case 0xF1: case 0xF2: case 0xF3:   /* psllw/d/q */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECALU;
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            /* --- SSE2 packed shifts by imm (reg field = ext; rm is the xmm) --- */
            case 0x71: case 0x72: case 0x73:
                d.cls = C_VECALU;
                d.regId = SB_NONE;
                d.rmRegId = memOperand ? SB_NONE : xmmId(rm);
                d.rmIsSrc = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            /* --- unpack / pack / shuffle (vector ALU) --- */
            case 0x60: case 0x61: case 0x62: case 0x63:
            case 0x67: case 0x68: case 0x69: case 0x6A:
            case 0x6B: case 0x6C: case 0x6D:   /* punpck and pack ops */
            case 0x70:                         /* pshufd/pshuflw/pshufhw (imm) */
            case 0xC6:                         /* shufps/shufpd */
            case 0x14: case 0x15:              /* unpcklps/unpckhps */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECALU;
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            /* --- xmm mask extraction -> GPR --- */
            case 0xD7:                         /* pmovmskb r32, xmm */
            case 0x50:                         /* movmskps/pd r32, xmm */
                setRegIds(RF_GPR, RF_VEC);
                d.cls = C_MOV; d.regIsDst = true; d.rmIsSrc = true;
                /* source is a register (xmm); no memory form */
                return hasModrm;
            /* --- bit scan / count --- */
            case 0xBC: case 0xBD:              /* bsf/bsr (F3: tzcnt/lzcnt) */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_ALU; d.writesFlags = true;
                d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0xB8:                         /* popcnt (F3) r, r/m */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_ALU; d.writesFlags = true;
                d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            /* --- atomics --- */
            case 0xB0: case 0xB1:              /* cmpxchg r/m, r
                                                * (rax implicit operand approximated
                                                * away; explicit reg + rm modeled) */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_ALU; d.writesFlags = true;
                d.regIsSrc = true;              /* the source reg */
                d.rmIsSrc = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_RMW;
                if (pLock || memOperand) d.fence = true;
                return hasModrm;
            case 0xC0: case 0xC1:              /* xadd r/m, r */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_ALU; d.writesFlags = true;
                d.regIsSrc = true; d.regIsDst = true;
                d.rmIsSrc = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_RMW;
                if (pLock || memOperand) d.fence = true;
                return hasModrm;
            /* --- bit test group --- */
            case 0xA3:                         /* bt  r/m, r (read only) */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_ALU; d.writesFlags = true;
                d.regIsSrc = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                return hasModrm;
            case 0xAB: case 0xB3: case 0xBB:   /* bts/btr/btc r/m, r (rmw) */
                setRegIds(RF_GPR, RF_GPR);
                d.cls = C_ALU; d.writesFlags = true;
                d.regIsSrc = true; d.rmIsSrc = true; d.rmIsDst = true;
                if (memOperand) d.mem = MEM_RMW;
                if (pLock) d.fence = true;
                return hasModrm;
            case 0xBA:                         /* grp8: bt/bts/btr/btc r/m, imm8 */
                d.cls = C_ALU; d.writesFlags = true;
                d.regId = SB_NONE;
                d.rmRegId = memOperand ? SB_NONE : gprId(rm);
                d.rmIsSrc = true;
                if ((reg & 7) >= 5) { d.rmIsDst = true; if (memOperand) d.mem = MEM_RMW; }
                else if (memOperand) d.mem = MEM_LOAD;   /* bt = read only */
                if (pLock) d.fence = true;
                return hasModrm;
            default:
                if (op >= 0x40 && op <= 0x4F) {           /* cmovcc */
                    setRegIds(RF_GPR, RF_GPR);
                    d.cls = C_MOV; d.readsFlags = true;
                    d.regIsDst = true; d.regIsSrc = true; d.rmIsSrc = true;
                    if (memOperand) d.mem = MEM_LOAD;
                    return hasModrm;
                }
                if (op >= 0x90 && op <= 0x9F) {           /* setcc r/m8 */
                    setRegIds(RF_GPR, RF_GPR);
                    d.cls = C_MOV; d.readsFlags = true; d.rmIsDst = true;
                    if (memOperand) d.mem = MEM_STORE;
                    return hasModrm;
                }
                if (op >= 0x80 && op <= 0x8F) {           /* jcc rel32 */
                    d.cls = C_BRANCH; d.readsFlags = true; return false;
                }
                /* other SSE ALU-ish (pxor, packed int, shuffles): treat as
                 * vector ALU with load if mem. */
                setRegIds(RF_VEC, RF_VEC);
                d.cls = C_VECALU; d.vecInt = true;   /* L201: packed-int-ish fallback */
                d.regIsSrc = d.regIsDst = true; d.rmIsSrc = true;
                if (memOperand) d.mem = MEM_LOAD;
                d.approx = true;
                return hasModrm;
        }
    }

    if (map == 0x0F38) {
        if (op == 0xF5) {                 /* BMI2: bzhi (no pp) / pext (F3) /
                                           * pdep (F2): GPR dst = f(rm, vvvv) */
            setRegIds(RF_GPR, RF_GPR);
            d.cls = (pF3 || pF2) ? C_IMUL : C_ALU;  /* pdep/pext ~3cy, bzhi 1cy */
            d.regIsDst = true; d.rmIsSrc = true;
            if (d.vvvvId) d.vvvvId = gprId(d.vvvvId - SB_XMM_BASE); /* vvvv is a GPR here */
            d.writesFlags = (!pF3 && !pF2);          /* bzhi writes flags */
            if (memOperand) d.mem = MEM_LOAD;
            return hasModrm;
        }
        if (op == 0x78 || op == 0x79) {   /* vpbroadcastb/w: pure vec move */
            setRegIds(RF_VEC, RF_VEC);
            d.cls = C_VECMOV; d.isPureMove = true;
            d.regIsDst = true; d.rmIsSrc = true;
            if (memOperand) d.mem = MEM_LOAD;
            return hasModrm;
        }
        setRegIds(RF_VEC, RF_VEC);
        if (op >= 0x96 && op <= 0xBF) {   /* vfmadd/vfmsub family (VEX) */
            d.cls = C_FMA;
            {   /* 1.12.18: precision is VEX.W (W1 = double); the
                 * scalar forms are the odd low nibbles 9/B/D/F (vfmadd132ss
                 * 0x99, ...), the rest packed (VEX.L = 256-bit). */
                const uint8_t lo = op & 0xF;
                const bool scal = (lo == 0x9 || lo == 0xB || lo == 0xD || lo == 0xF);
                d.fpLanes = (uint8_t)(scal ? 1 : (vexW ? 2 : 4) * (vexL ? 2 : 1));
                d.fpCat1 = (uint8_t)((vexW ? 5 : 4) + 1);
            }
            d.regIsDst = d.regIsSrc = true;   /* accumulator dest is also a src */
            d.rmIsSrc = true;
            if (memOperand) d.mem = MEM_LOAD;
            return hasModrm;
        }
        d.cls = C_VECALU; d.regIsDst = true; d.rmIsSrc = true;
        if (memOperand) d.mem = MEM_LOAD;
        d.approx = true;
        return hasModrm;
    }
    if (map == 0x0F3A) {
        setRegIds(RF_VEC, RF_VEC);
        d.cls = C_VECALU; d.regIsDst = true; d.rmIsSrc = true;
        if (memOperand) d.mem = MEM_LOAD;
        d.approx = true;
        return hasModrm;
    }

    /* 1.11.47 (FIX-PRE-FLEET L210): x87 (D8-DF, one-byte map) is REAL
     * floating point and used to fall through to C_GENERIC -> the census's
     * integer bucket -- FP work priced as integer AND escaping the soft-float
     * charge on FPU-less elements, the mirror image of L201. Classed as FP;
     * still approx (no per-op x87 uop model, and none is invented). */
    if (map == 1 && op >= 0xD8 && op <= 0xDF) {
        d.cls = C_FADD; d.approx = true;
        d.fpCat1 = 8 + 1; d.fpLanes = 1;   /* 1.12.18: x87, precision unknown statically */
        if (memOperand) d.mem = MEM_LOAD;
        return hasModrm;
    }
    d.cls = C_GENERIC; d.approx = true;
    return hasModrm;
}

/* ==================================================================== *
 *  Emit uops for one decoded instruction into `v`.
 *  li/si: running load/store temp indices reset per instruction.
 *  decCycle: front-end decode cycle for this instruction's uops.
 *  Returns via loads/stores the count of memory-access uops emitted.
 * ==================================================================== */
static inline void emitUops(const Decoded& d, std::vector<DynUop>& v,
                            uint32_t decCycle, uint32_t& loads, uint32_t& stores) {
    ClassInfo ci = classInfo(d.cls);
    const uint16_t LT = REG_LOAD_TEMP;    /* per-instruction load temp */
    bool doLoad  = (d.mem == MEM_LOAD || d.mem == MEM_RMW);
    bool doStore = (d.mem == MEM_STORE || d.mem == MEM_RMW);

    /* ---- string ops (movs/stos/lods) ---- */
    if (d.strKind) {
        const uint16_t RAX = gprId(0), RCX = gprId(1), RSI = gprId(6), RDI = gprId(7);
        if (d.repPrefixed) {
            /* REP form: the iteration count is dynamic (rcx), so the memory
             * uops are intentionally NOT emitted statically -- the cores drain
             * the ACTUAL QEMU-delivered accesses through the cache at serial
             * L1 throughput (block-copy cost model) and classify them as rep
             * traffic (DynBbl.repInstrs > 0). Here we only model the register
             * side: pointer/count updates depending on rcx. */
            if (d.strKind == 1) {           /* rep movs: rsi, rdi, rcx */
                pushUop(v, UOP_GENERAL, RCX, RSI, RSI, SB_NONE, 1, PORTS_ALU, 0, decCycle);
                pushUop(v, UOP_GENERAL, RCX, RDI, RDI, RCX, 1, PORTS_ALU, 0, decCycle);
            } else if (d.strKind == 2) {    /* rep stos: rax value, rdi, rcx */
                pushUop(v, UOP_GENERAL, RCX, RDI, RDI, RCX, 1, PORTS_ALU, 0, decCycle);
            } else {                        /* rep lods (rare): rsi, rcx, rax */
                pushUop(v, UOP_GENERAL, RCX, RSI, RSI, RAX, 1, PORTS_ALU, 0, decCycle);
            }
            return;
        }
        /* Non-REP single-shot forms: exactly one load and/or store -- emit
         * matching memory uops so runtime callback counts line up exactly. */
        if (d.strKind == 1) {               /* movs: [rsi] -> [rdi], ptr updates */
            pushUop(v, UOP_LOAD, RSI, SB_NONE, LT, RSI, 0, PORTS_LOAD, 0, decCycle);
            pushUop(v, UOP_STORE, LT, RDI, RDI, SB_NONE, 0, PORTS_STORE, 0, decCycle);
            loads++; stores++;
        } else if (d.strKind == 2) {        /* stos: rax -> [rdi] */
            pushUop(v, UOP_STORE, RAX, RDI, RDI, SB_NONE, 0, PORTS_STORE, 0, decCycle);
            stores++;
        } else {                            /* lods: [rsi] -> rax */
            pushUop(v, UOP_LOAD, RSI, SB_NONE, RAX, RSI, 0, PORTS_LOAD, 0, decCycle);
            loads++;
        }
        return;
    }

    /* ---- grp3 widening mul / div with correct rdx:rax pair semantics ---- */
    if (d.gp3) {
        const uint16_t RAX = gprId(0), RDX = gprId(2);
        uint16_t src = d.rmRegId;           /* rm register source... */
        if (doLoad) {                       /* ...or loaded memory operand */
            pushUop(v, UOP_LOAD, d.baseId, d.indexId, LT, SB_NONE,
                    0, PORTS_LOAD, 0, decCycle);
            loads++;
            src = LT;
        }
        if (d.gp3 == 2) {
            /* div/idiv: reads rdx:rax + src (3 sources > 2 slots) -> merge the
             * rdx:rax pair into an exec temp, then divide. Both rax (quotient)
             * and rdx (remainder) become ready at divide completion. */
            pushUop(v, UOP_GENERAL, RAX, RDX, REG_EXEC_TEMP, SB_NONE,
                    1, PORTS_ALU, 0, decCycle);
            pushUop(v, UOP_GENERAL, REG_EXEC_TEMP, src, RAX, RDX,
                    ci.lat, ci.port, ci.extraSlots, decCycle);
        } else if (d.gp3 == 1) {
            /* widening mul: rdx:rax = rax * src (2 sources, 2 dests: exact) */
            pushUop(v, UOP_GENERAL, RAX, src, RAX, RDX,
                    ci.lat, ci.port, ci.extraSlots, decCycle);
        } else {
            /* 8-bit form: al/ah/ax all alias the rax scoreboard id */
            pushUop(v, UOP_GENERAL, RAX, src, RAX, SB_NONE,
                    ci.lat, ci.port, ci.extraSlots, decCycle);
        }
        return;
    }

    /* Collect up to 2 source and 2 dest registers for the compute uop. */
    uint16_t src[2] = {SB_NONE, SB_NONE}; int ns = 0;
    uint16_t dst[2] = {SB_NONE, SB_NONE}; int nd = 0;
    auto addS = [&](uint16_t r){ if (r && ns < 2) src[ns++] = r; };
    auto addD = [&](uint16_t r){ if (r && nd < 2) dst[nd++] = r; };

    /* ---- pure move: mem<->reg with no computation ---- */
    if (d.isPureMove && d.mem == MEM_LOAD) {
        /* value flows mem -> dest reg: single LOAD uop writing the dest reg */
        uint16_t rd = d.regIsDst ? d.regId : (d.rmIsDst ? d.rmRegId : SB_NONE);
        pushUop(v, UOP_LOAD, d.baseId, d.indexId, rd, SB_NONE,
                0, PORTS_LOAD, 0, decCycle);
        loads++; return;
    }
    if (d.isPureMove && d.mem == MEM_STORE) {
        uint16_t val = d.regIsSrc ? d.regId : (d.rmIsSrc ? d.rmRegId : SB_NONE);
        pushUop(v, UOP_STORE, val, d.baseId, SB_NONE, SB_NONE,
                0, PORTS_STORE, 0, decCycle);
        stores++; return;
    }
    if (d.isPureMove && d.mem == MEM_NONE) {
        /* reg<->reg or reg<-imm move */
        if (d.rmIsSrc) addS(d.rmRegId);
        if (d.regIsSrc) addS(d.regId);
        if (d.regIsDst) addD(d.regId);
        if (d.rmIsDst)  addD(d.rmRegId);
        pushUop(v, UOP_GENERAL, src[0], src[1], dst[0], dst[1],
                ci.lat, ci.port, ci.extraSlots, decCycle);
        return;
    }

    /* ---- lea: address computation, no memory access ---- */
    if (d.cls == C_LEA) {
        addS(d.baseId); addS(d.indexId);
        addD(d.regId);
        pushUop(v, UOP_GENERAL, src[0], src[1], dst[0], dst[1],
                ci.lat, ci.port, ci.extraSlots, decCycle);
        return;
    }

    /* ---- general path (ALU / FP / vector, optionally load-op / rmw) ---- */
    uint32_t computeDec = decCycle;
    if (doLoad) {
        pushUop(v, UOP_LOAD, d.baseId, d.indexId, LT, SB_NONE,
                0, PORTS_LOAD, 0, decCycle);
        loads++;
        computeDec = decCycle;  /* same decode group; dep enforced by scoreboard */
    }

    /* compute sources */
    if (d.mem == MEM_LOAD || d.mem == MEM_RMW) addS(LT);  /* loaded value */
    if (d.rmIsSrc && d.mem == MEM_NONE) addS(d.rmRegId);
    if (d.regIsSrc) addS(d.regId);
    /* VEX third operand (vvvv), lowest priority: consumed only when a source
     * slot remains (e.g. BMI2 bzhi/pdep/pext reg forms; FMA's slots are already
     * full with the loaded value + accumulator, so FMA timing is unchanged). */
    if (d.vvvvId) addS(d.vvvvId);
    if (d.readsFlags) addS(SB_FLAGS);

    /* compute dests */
    uint16_t resultReg = SB_NONE;
    if (d.regIsDst) { addD(d.regId); resultReg = d.regId; }
    if (d.rmIsDst && d.mem == MEM_NONE) { addD(d.rmRegId); resultReg = d.rmRegId; }
    if (doStore) {
        /* result must be produced into a temp the store can read */
        resultReg = LT;
        addD(LT);
    }
    if (d.writesFlags) addD(SB_FLAGS);

    UopType ct = (d.cls == C_BRANCH) ? UOP_GENERAL : UOP_GENERAL;
    pushUop(v, ct, src[0], src[1], dst[0], dst[1],
            ci.lat, ci.port, ci.extraSlots, computeDec);

    if (doStore) {
        pushUop(v, UOP_STORE, resultReg, d.baseId, SB_NONE, SB_NONE,
                0, PORTS_STORE, 0, computeDec);
        stores++;
    }

    /* Fence (lock-prefixed atomics / mfence): serialize younger memory ops. */
    if (d.fence) {
        pushUop(v, UOP_FENCE, SB_NONE, SB_NONE, SB_NONE, SB_NONE,
                1, PORTS_ALU, 0, computeDec);
    }
}

/* PIMID 1.12.18: (user ruling 2026-10-23 #3) integer census
 * category + 1 of a decoded instruction (decoder_simple.h; 0 = not
 * counted): scalar GPR add/logic (C_ALU) and multiply (C_IMUL of the
 * one-byte map and 0F AF; the 0F38 F5 pdep/pext is a bit deposit, not a
 * multiply) by operand width, and divide (C_IDIV). Byte forms (8 bit):
 * one-byte map 00-3F with low bits 0/2/4, 80, 82, 84, A8, C0, D0, D2, F6,
 * FE; 0F B0 (cmpxchg), 0F C0 (xadd) (Intel SDM Vol. 2, opcode map A.3). */
static inline int intCensusCat(const Decoded& d) {
    int base;
    if (d.cls == C_ALU) base = 0;
    else if (d.cls == C_IMUL && d.dbgMapIdx != 2) base = 4;
    else if (d.cls == C_IDIV) return 8 + 1;
    else return 0;
    const uint8_t op = d.dbgOp;
    bool byteForm = false;
    if (d.dbgMapIdx == 0)
        byteForm = (op < 0x40 && (op & 1) == 0 && (op & 7) <= 4) || op == 0x80 || op == 0x82 ||
                   op == 0x84 || op == 0xA8 || op == 0xC0 || op == 0xD0 || op == 0xD2 || op == 0xF6 || op == 0xFE;
    else if (d.dbgMapIdx == 1)
        byteForm = (op == 0xB0 || op == 0xC0);
    const int w = byteForm ? 0 : (d.intW == 64 ? 3 : (d.intW == 16 ? 1 : 2));
    return base + w + 1;
}

/* ==================================================================== *
 *  Build a fully-decoded BblInfo (with populated oooBbl) for a TB.
 *  insnBytes[k] / insnLens[k] describe instruction k. Allocated from the
 *  ZSim global heap so it persists across phases (matches decoder_simple).
 * ==================================================================== */
static inline BblInfo* createDecodedBblInfo(uint64_t bblAddr,
        const uint8_t (*insnBytes)[16], const uint8_t* insnLens,
        uint32_t nInsns, uint32_t tbBytes,
        std::vector<uint16_t>* outApproxKeys = nullptr) {
    std::vector<DynUop> uops;
    uops.reserve(nInsns * 2 + 1);
    uint32_t approx = 0;
    uint32_t repInstrs = 0;
    uint32_t decWidth = 4;  /* front-end decode/issue width for decCycle spread */
    uint32_t emitted = 0;

    /* 1.11.10 (#112): census the classes the decoder already determines, so
     * the instruction mix is COUNTED rather than assumed. Saturating at
     * uint16_t max because a basic block with 65k instructions of one class
     * is not a thing that happens -- if it ever did, clamping is visible in
     * the ratios while wrapping would silently invert them. */
    uint32_t cInt = 0, cMul = 0, cFp = 0, cBr = 0, cLd = 0, cSt = 0;
    uint32_t cVmR = 0, cVmL = 0, cVmS = 0;   /* 1.12.18: pure vector moves (a subset of cFp), by form */
    FpOpCensus fpc; memset(&fpc, 0, sizeof(fpc));   /* 1.12.18: FP operations by type/precision */
    bool anyFpc = false;
    for (uint32_t k = 0; k < nInsns; k++) {
        Decoded d;
        decodeOne(insnBytes[k], insnLens[k], d);
        uint32_t loads = 0, stores = 0;
        uint32_t decCycle = emitted / decWidth;
        emitUops(d, uops, decCycle, loads, stores);
        emitted = (uint32_t)uops.size();
        cLd += loads; cSt += stores;
        switch (d.cls) {
            case C_IMUL: case C_IDIV:                       cMul++; break;
            /* 1.11.47 (L201): vecInt routes packed-INTEGER SIMD to the int
             * (or mul) bucket the work belongs to; only genuinely
             * floating-point vector ops stay FP. */
            case C_FMUL: if (d.vecInt) { cMul++; break; }
                         cFp++; break;
            case C_VECALU: case C_VECMOV:
                         if (d.vecInt) { cInt++; break; }
                         cFp++;
                         /* 1.12.18: (bug 1b) a PURE vector move (movss/movsd/
                          * movaps/movd/movq ...: a load, a store or a register
                          * copy) stays in the FP class -- the instruction counts
                          * and the soft-float timing charge read cFp and are
                          * unchanged -- and is ALSO counted here by form, so the
                          * power model prices it as register-file / load-store
                          * activity and never as an FPU operation. */
                         if (d.cls == C_VECMOV && d.isPureMove) {
                             if (d.mem == MEM_LOAD) cVmL++;
                             else if (d.mem == MEM_STORE) cVmS++;
                             else cVmR++;
                         }
                         break;
            case C_FADD: case C_FDIV: case C_FMA:           cFp++;  break;
            case C_BRANCH:                                  cBr++;  break;
            default:                                        cInt++; break;
        }
        if (d.fpCat1 && !d.vecInt) {   /* 1.12.18: a subset of cFp */
            fpc.ins[d.fpCat1 - 1]++; fpc.lanes[d.fpCat1 - 1] += d.fpLanes; anyFpc = true;
        }
        if (const int ic = intCensusCat(d)) {   /* PIMID 1.12.18: a subset of cInt / cMul */
            fpc.iops[ic - 1]++; anyFpc = true;
        }
        if (d.strKind && d.repPrefixed) repInstrs++;
        if (d.approx || d.cls == C_GENERIC) {
            approx++;
            if (outApproxKeys) outApproxKeys->push_back(dbgKey(d));
        }
    }
    auto sat16 = [](uint32_t v) -> uint16_t {
        return (uint16_t)((v > 0xFFFFu) ? 0xFFFFu : v);
    };

    uint32_t nUops = (uint32_t)uops.size();
    /* Allocate BblInfo + DynBbl with room for nUops (at least 1 for the header). */
    uint32_t hdrUops = nUops ? nUops : 1;
    size_t total = sizeof(BblInfo) + DynBbl::bytes(hdrUops);
    BblInfo* bbl = static_cast<BblInfo*>(__gm_calloc(1, total));
    bbl->instrs = nInsns;
    bbl->bytes = tbBytes;
    bbl->nInt = sat16(cInt); bbl->nMul = sat16(cMul); bbl->nFp = sat16(cFp);
    bbl->nBr  = sat16(cBr);  bbl->nLoad = sat16(cLd); bbl->nStore = sat16(cSt);
    auto sat8 = [](uint32_t v) -> uint8_t { return (uint8_t)((v > 0xFFu) ? 0xFFu : v); };   // 1.12.18
    bbl->nVmovReg = sat8(cVmR); bbl->nVmovLd = sat8(cVmL); bbl->nVmovSt = sat8(cVmS);   // 1.12.18 (bug 1b)
    DynBbl& db = bbl->oooBbl[0];
    db.addr = bblAddr;
    db.uops = nUops;
    db.approxInstrs = sat16(approx);       /* 1.12.18: <= 1024 per block, exact */
    db.repInstrs = sat16(repInstrs);
    db.fpCensusIdx = anyFpc ? fpCensusAdd(fpc) : 0;   /* 1.12.18 */
    for (uint32_t u = 0; u < nUops; u++) db.uop[u] = uops[u];
    return bbl;
}

}  // namespace x86dec

#endif  // X86_DECODER_H_
