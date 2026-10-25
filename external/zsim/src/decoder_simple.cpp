/** Simplified BBL decoder implementation for QEMU-based instrumentation.
 *
 * Allocates BblInfo structures from the ZSim global heap. These are cached
 * by the QEMU plugin to avoid re-allocation for the same translation block.
 */

#include "decoder_simple.h"
#include "core.h"
#include "galloc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <mutex>

/* 1.12.18: the FP-operation census table (decoder_simple.h). Chunks
 * of 4096 records, never moved or freed, so a reader holding an index needs no
 * lock; writers (block translation) take the mutex. Table and chunks come from
 * the zsim GLOBAL heap (gm_calloc), NOT from static storage or the process
 * heap: under QEMU user mode the guest shares this address space, and a
 * larger plugin .bss or a fresh host mmap shifts every later guest mapping
 * (setarch -R makes the layout deterministic, so the shift is a deterministic
 * cycle drift on cache-sensitive cells -- measured: a 512 KB static table
 * moved the 1-core host baseline by 185 cycles). The global heap is one
 * region reserved at start-up, so allocating in it moves no mapping.
 * (Since (1.12.62) the launch reserves the guest address space (QEMU -B 0 -R), so
 * a host mapping no longer moves a guest mapping either.) */
const char* const kFpCensusNames[FP_CENSUS_CATS] = {
    "AddS", "MulS", "AddD", "MulD", "FmaS", "FmaD", "DivS", "DivD", "X87"};
/* PIMID 1.12.18: (ruling 2026-10-23 #3) integer categories (decoder_simple.h). */
const char* const kIntCensusNames[INT_CENSUS_CATS] = {
    "Alu8", "Alu16", "Alu32", "Alu64", "Mul8", "Mul16", "Mul32", "Mul64", "Div"};
namespace {
const uint32_t kFpcChunkBits = 12;
const uint32_t kFpcChunk = 1u << kFpcChunkBits;
const uint32_t kFpcMaxChunks = 1u << 16;
FpOpCensus** g_fpcChunks = nullptr;   // gm_calloc'd on first use
uint32_t g_fpcNext = 1;
uint64_t g_fpcDropped = 0;
std::mutex g_fpcMu;
}
uint32_t fpCensusAdd(const FpOpCensus& c) {
    std::lock_guard<std::mutex> lk(g_fpcMu);
    const uint32_t idx = g_fpcNext;
    const uint32_t ch = idx >> kFpcChunkBits;
    if (ch >= kFpcMaxChunks) {
        if (g_fpcDropped++ == 0)
            fprintf(stderr, "[zsim] WARNING: FP-operation census table full (%u blocks); further blocks'"
                            " FP operations stay unclassified (priced at McPAT's per-op FPU energy)\n",
                    (unsigned)(idx - 1));
        return 0;
    }
    if (!g_fpcChunks) g_fpcChunks = gm_calloc<FpOpCensus*>(kFpcMaxChunks);
    if (!g_fpcChunks[ch]) {
        g_fpcChunks[ch] = gm_calloc<FpOpCensus>(kFpcChunk);
        if (!g_fpcChunks[ch]) { fprintf(stderr, "[zsim] FATAL: FP census allocation failed\n"); abort(); }
    }
    g_fpcChunks[ch][idx & (kFpcChunk - 1)] = c;
    g_fpcNext++;
    return idx;
}
const FpOpCensus& fpCensusAt(uint32_t idx) {
    return g_fpcChunks[idx >> kFpcChunkBits][idx & (kFpcChunk - 1)];
}

BblInfo* createSimpleBblInfo(uint32_t instrs, uint32_t bytes, bool synth) {
    // Allocate BblInfo + one DynBbl header so that oooBbl[0].uops is valid.
    // OOO/Timing cores access oooBbl[0] even for synthetic BBLs; without the
    // extra space, they read past the allocation (undefined behaviour / segfault).
    // gm_calloc zeros the memory, so oooBbl[0].uops == 0 automatically.
    size_t total = sizeof(BblInfo) + sizeof(DynBbl);
    BblInfo* bbl = static_cast<BblInfo*>(__gm_calloc(1, total));
    bbl->instrs = instrs;
    bbl->bytes = bytes;
    bbl->synth = synth ? 1 : 0;   // 1.11.16: injected-charge marker
    return bbl;
}
