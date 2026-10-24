/* pimid_spm.h -- the processing element's scratchpad (1.12.18).
 *
 * A compute-unit PE (pe_type alu_core) has a small private RAM next to its
 * datapath, sized by pim.pe.spm_bytes (default 4096; 0 = none). Its loads and
 * stores inside that RAM cost the RAM's access time (CACTI, at the logic node)
 * instead of a DRAM access. Nothing reaches it automatically: as on a real PIM
 * element (UPMEM's WRAM, an HBM-PIM register file), the KERNEL moves data in
 * and out, a block at a time:
 *
 *     pimid_spm_t s;  pimid_spm_open(&s);          once per thread, before the ROI
 *     const float* a = (const float*)pimid_spm_in(&s, off, src, bytes);   DRAM -> RAM
 *     float* o = (float*)pimid_spm_out(&s, off, dst);                     a RAM area to fill
 *     pimid_spm_flush(&s, dst, o, bytes);                                 RAM -> DRAM
 *     pimid_spm_close(&s);
 *
 * pim.pe.spm_staging selects who moves the block. dma (the default): the
 * simulator's per-PE DMA engine fetches the block's lines from DRAM in order
 * and copies them; the PE waits and executes no copy instructions. core: the
 * PE copies it itself with 8-byte loads and stores, each one priced.
 *
 * Where there is no scratchpad -- spm_bytes 0, a PE with caches (in-order /
 * out-of-order), the host, or a native run outside the simulator --
 * pimid_spm_open() reports capacity 0, pimid_spm_in() returns the source
 * pointer itself, pimid_spm_out() returns the destination, and flush does
 * nothing: the kernel runs on DRAM exactly as it did before 1.12.18, through
 * the same code path. pimid_spm_tile() then returns the whole range.
 */
#ifndef PIMID_SPM_H_
#define PIMID_SPM_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These need to be in sync with the simulator (qemu_zsim_plugin.cpp). */
#define ZSIM_MAGIC_OP_SPM_QUERY (2061)   /* -> capacity bytes, staging mode */
#define ZSIM_MAGIC_OP_SPM_BIND  (2062)   /* {base, bytes, cmd}: this thread's RAM and DMA command block; {0, 0} releases */
#define ZSIM_MAGIC_OP_SPM_DMA   (2063)   /* the command block {dst, src, bytes, dir}: one block transfer */

#define PIMID_SPM_DMA  1
#define PIMID_SPM_CORE 2

typedef struct __attribute__((aligned(64))) {
    uint64_t a, b, c, d;      /* guest -> simulator */
    uint64_t ret0, ret1;      /* simulator -> guest */
} pimid_spm_msg_t;

typedef struct {
    unsigned char* buf;       /* the RAM (NULL when there is none) */
    size_t cap;               /* its capacity in bytes (0 = none) */
    int mode;                 /* PIMID_SPM_DMA or PIMID_SPM_CORE */
    pimid_spm_msg_t* cmd;     /* the DMA engine's command registers (64 bytes after the RAM) */
} pimid_spm_t;

static inline void pimid__spm_op(uint64_t op, pimid_spm_msg_t* m) {
#ifdef __x86_64__
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op), "d"(m) : "memory");
#else
    (void)op; (void)m;
#endif
}

/* The PE copies a block itself: 8-byte words, then the tail bytes. Written out
 * (not memcpy) so the priced access stream does not depend on the C library's
 * string routines. */
static inline void pimid__spm_core_copy(void* dst, const void* src, size_t n) {
    volatile uint64_t* d = (volatile uint64_t*)dst;
    const volatile uint64_t* s = (const volatile uint64_t*)src;
    size_t w = n / 8, i;
    for (i = 0; i < w; i++) d[i] = s[i];
    for (i = w * 8; i < n; i++) ((volatile unsigned char*)dst)[i] = ((const volatile unsigned char*)src)[i];
}

/* dir 0 = DRAM -> RAM, 1 = RAM -> DRAM. */
static inline void pimid__spm_move(pimid_spm_t* s, void* dst, const void* src, size_t n, int dir) {
    if (n == 0) return;
    if (s->mode == PIMID_SPM_DMA) {
        /* The transfer is described in the engine's command registers, not in
         * memory: the simulator prices these stores as register accesses. */
        pimid_spm_msg_t* m = s->cmd;
        m->a = (uint64_t)(uintptr_t)dst; m->b = (uint64_t)(uintptr_t)src; m->c = (uint64_t)n; m->d = (uint64_t)dir; m->ret0 = 0;
        pimid__spm_op(ZSIM_MAGIC_OP_SPM_DMA, m);
        if (m->ret0 == 1) return;   /* the engine moved it */
        fprintf(stderr, "pimid_spm: the simulator did not perform a DMA transfer it advertised\n");
        abort();
    }
    pimid__spm_core_copy(dst, src, n);
}

/* Once per thread, before the ROI. Returns the capacity (0 = no scratchpad). */
static inline size_t pimid_spm_open(pimid_spm_t* s) {
    pimid_spm_msg_t m = {0, 0, 0, 0, 0, 0};
    s->buf = NULL; s->cap = 0; s->mode = 0; s->cmd = NULL;
    pimid__spm_op(ZSIM_MAGIC_OP_SPM_QUERY, &m);
    if (m.ret0 == 0) {
        /* Outside the simulator nothing answers the query. PIMID_SPM_CHECK_BYTES
         * then runs the tiled path natively with the PE copying (a correctness
         * check of a kernel's tiling against its untiled result); it is never
         * consulted under the simulator. */
        const char* e = getenv("PIMID_SPM_CHECK_BYTES");
        const long v = e ? atol(e) : 0;
        if (v <= 0) return 0;
        s->cap = (size_t)v;
        s->mode = PIMID_SPM_CORE;
        s->buf = (unsigned char*)aligned_alloc(64, (s->cap + 63) / 64 * 64);
        if (!s->buf) { fprintf(stderr, "pimid_spm: cannot allocate %zu bytes\n", s->cap); abort(); }
        return s->cap;
    }
    s->cap = (size_t)m.ret0;
    s->mode = (int)m.ret1;
    s->buf = (unsigned char*)aligned_alloc(64, s->cap + 64);
    if (!s->buf) { fprintf(stderr, "pimid_spm: cannot allocate %zu bytes\n", s->cap); abort(); }
    s->cmd = (s->mode == PIMID_SPM_DMA) ? (pimid_spm_msg_t*)(s->buf + s->cap) : NULL;
    m.a = (uint64_t)(uintptr_t)s->buf; m.b = (uint64_t)s->cap; m.c = (uint64_t)(uintptr_t)s->cmd; m.ret0 = 0;
    pimid__spm_op(ZSIM_MAGIC_OP_SPM_BIND, &m);
    if (m.ret0 != 1) { fprintf(stderr, "pimid_spm: the simulator refused the scratchpad binding\n"); abort(); }
    return s->cap;
}

static inline void pimid_spm_close(pimid_spm_t* s) {
    if (s->cap) {
        pimid_spm_msg_t m = {0, 0, 0, 0, 0, 0};
        pimid__spm_op(ZSIM_MAGIC_OP_SPM_BIND, &m);   /* releases the binding (a no-op natively) */
        free(s->buf);
    }
    s->buf = NULL; s->cap = 0; s->mode = 0; s->cmd = NULL;
}

static inline void pimid__spm_check(const pimid_spm_t* s, size_t off, size_t n) {
    if (off + n > s->cap) {
        fprintf(stderr, "pimid_spm: block [%zu, %zu) exceeds the %zu-byte scratchpad\n", off, off + n, s->cap);
        abort();
    }
}

/* Bring n bytes at src into the RAM at offset off; returns where to read them. */
static inline const void* pimid_spm_in(pimid_spm_t* s, size_t off, const void* src, size_t n) {
    if (!s->cap) return src;
    pimid__spm_check(s, off, n);
    pimid__spm_move(s, s->buf + off, src, n, 0);
    return s->buf + off;
}

/* Where to write a result block destined for dst. */
static inline void* pimid_spm_out(pimid_spm_t* s, size_t off, void* dst) {
    return s->cap ? (void*)(s->buf + off) : dst;
}

/* Write n bytes of a result block (from pimid_spm_out) back to dst. */
static inline void pimid_spm_flush(pimid_spm_t* s, void* dst, const void* area, size_t n) {
    if (!s->cap) return;
    pimid__spm_check(s, (size_t)((const unsigned char*)area - s->buf), n);
    pimid__spm_move(s, dst, area, n, 1);
}

/* Elements per tile when the RAM is split into nbuf equal buffers of
 * elem-byte elements: a whole number of 64-byte lines per buffer, at least one
 * element. With no scratchpad the tile is unbounded (SIZE_MAX). */
static inline size_t pimid_spm_tile(const pimid_spm_t* s, size_t nbuf, size_t elem) {
    if (!s->cap) return (size_t)-1;
    size_t per = s->cap / (nbuf ? nbuf : 1);
    per -= per % 64;
    size_t t = per / elem;
    if (t == 0) {
        fprintf(stderr, "pimid_spm: a %zu-byte scratchpad cannot hold %zu buffers of %zu-byte elements\n", s->cap, nbuf, elem);
        abort();
    }
    return t;
}

#endif  /* PIMID_SPM_H_ */
