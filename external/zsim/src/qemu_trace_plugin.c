/**
 * QEMU TCG Plugin for PIMID Trace Generation
 *
 * A lightweight, pure-C plugin that generates PIMID-format binary traces
 * from QEMU user-mode emulation. No ZSim dependency -- outputs the same
 * 48-byte TraceEvent format used by --method trace replay.
 *
 * Usage:
 *   qemu-x86_64 -plugin libpimid_trace.so,output=file.pimtrace -- ./workload
 *
 * Plugin arguments:
 *   output=<path>   Output trace file path (required)
 *   batch=<N>       Write buffer size in events (default: 4096)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>

#include "qemu/qemu-plugin.h"

/* Must be defined for QEMU to recognize the plugin */
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* ---- PIMID Trace Format (must match trace_format.h) ---- */

#define TRACE_MAGIC   0x50494D54  /* "PIMT" */
#define TRACE_VERSION 0x0100      /* 1.0 */

/* Event types (subset used for tracing) */
#define EVT_MEM_READ    0x0001
#define EVT_MEM_WRITE   0x0002
#define EVT_COMPUTE_INT 0x0030
#define EVT_BARRIER     0x0040

/* 48-byte trace event -- matches pimid::trace::TraceEvent exactly */
typedef struct {
    uint64_t cycle;       /* monotonic instruction counter as proxy */
    uint64_t address;     /* virtual address */
    uint64_t aux_data;    /* event-specific: size for mem, count for compute */
    uint32_t pe_id;       /* vcpu index */
    uint32_t size;        /* data size in bytes */
    uint32_t src_node;    /* unused in trace-gen */
    uint32_t dst_node;    /* unused in trace-gen */
    uint32_t reserved;
    uint16_t event_type;
    uint16_t flags;
} TraceEvent;

/* 64-byte header */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_flags;
    uint64_t num_events;
    uint64_t header_size;     /* offset to first event */
    uint64_t num_pes;
    uint64_t first_cycle;
    uint64_t last_cycle;
    uint32_t reserved[4];
} TraceHeader;

/* ---- Plugin State ---- */

static FILE *trace_file;
static char *output_path;
static uint32_t batch_size = 4096;

/* Per-vcpu counters */
#define MAX_VCPUS 256
static uint64_t insn_count[MAX_VCPUS];
static uint64_t total_events;
static uint64_t first_cycle_val;
static uint64_t last_cycle_val;
static uint32_t max_vcpu_seen;

/* Write buffer */
static TraceEvent *write_buf;
static uint32_t buf_pos;
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;

/* ROI state -- set by mov $op, %rcx + xchg %rcx, %rcx (zsim_hooks.h magic ops) */
static _Atomic bool in_roi = true;            /* true = record; default on for non-ROI workloads */
static _Atomic uint64_t roi_transition_count; /* number of ROI begin/end transitions */

/* ZSim magic op codes (must match zsim_hooks.h) */
#define ZSIM_MAGIC_OP_ROI_BEGIN   1025
#define ZSIM_MAGIC_OP_ROI_END     1026
#define ZSIM_MAGIC_OP_WORK_BEGIN  1029
#define ZSIM_MAGIC_OP_WORK_END    1030

/* Co-simulation event types (must match trace_format.h) */
#define EVT_OFFLOAD_START   0x0070
#define EVT_OFFLOAD_END     0x0071
/* PIMID 1.11.100 (review H36): the ROI markers, written so the replay can
 * rebase its counters at roi_begin and freeze them at roi_end as the
 * execution path does. */
#define EVT_ROI_BEGIN       0x0080
#define EVT_ROI_END         0x0081

/* Co-simulation domain flag (must match trace_format.h) */
#define FLAG_DEVICE_DOMAIN  0x0020

/* Co-simulation: per-vCPU domain tracking */
enum { DOMAIN_HOST = 0, DOMAIN_DEVICE = 1 };
static _Atomic int thread_domain[MAX_VCPUS];   /* default HOST */
static _Atomic uint64_t offload_count;

/* PIMID 1.11.100 (review C6 / H37): one COMPUTE event per EXECUTED basic
 * block, carrying the block's address, instruction count and byte size, and
 * a side file "<output>.blocks" with each translated block's instruction
 * bytes so the replay can decode them with the same decoder the execution
 * path uses (x86_decoder.h) -- the in-order and out-of-order cores then
 * simulate the real instruction stream instead of one synthetic block per
 * vcpu at exit. Block records are written at translation; a block can be
 * translated more than once (the replay keeps the first). */
typedef struct {
    uint64_t vaddr;
    uint32_t n_insns;
    uint32_t bytes;
} TbRec;
static void emit_event(const TraceEvent *evt);   /* defined below */
static FILE *blocks_file;
static uint64_t blocks_written;
static pthread_mutex_t blocks_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t compute_events;

/* PIMID 1.11.100 (review H36): the ROI marker goes into the trace, inside
 * the recorded window on both sides (begin after opening, end before
 * closing), from both magic-op entry points. */
static void emit_roi_event(unsigned int vcpu_index, uint64_t op) {
    uint32_t idx = vcpu_index < MAX_VCPUS ? vcpu_index : 0;
    if (op == ZSIM_MAGIC_OP_ROI_BEGIN) atomic_store(&in_roi, true);
    TraceEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.cycle = insn_count[idx];
    evt.pe_id = vcpu_index;
    evt.event_type = (op == ZSIM_MAGIC_OP_ROI_BEGIN) ? EVT_ROI_BEGIN : EVT_ROI_END;
    if (atomic_load(&thread_domain[idx]) == DOMAIN_DEVICE) evt.flags |= FLAG_DEVICE_DOMAIN;
    emit_event(&evt);
    if (op == ZSIM_MAGIC_OP_ROI_END) atomic_store(&in_roi, false);
}

static void tb_exec_cb(unsigned int vcpu_index, void *userdata) {
    if (!in_roi) return;
    const TbRec *rec = (const TbRec *)userdata;
    uint32_t idx = vcpu_index < MAX_VCPUS ? vcpu_index : 0;
    TraceEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.cycle = insn_count[idx];
    evt.address = rec->vaddr;
    evt.aux_data = rec->n_insns;
    evt.size = rec->bytes;
    evt.pe_id = vcpu_index;
    evt.event_type = EVT_COMPUTE_INT;
    if (atomic_load(&thread_domain[idx]) == DOMAIN_DEVICE) evt.flags |= FLAG_DEVICE_DOMAIN;
    emit_event(&evt);
    compute_events++;
}

/* Per-vCPU pending magic op from mov $imm, %rcx -- persists across TB
 * boundaries.  This handles the rare case where mov and xchg are in different
 * TBs (e.g., page boundary between them).  Per-vCPU to avoid races when
 * multiple vCPUs translate TBs concurrently under MTTCG. */
static _Atomic uint64_t pending_magic_op[MAX_VCPUS];

/* ---- Helpers ---- */

static void flush_buffer(void) {
    if (buf_pos > 0 && trace_file) {
        fwrite(write_buf, sizeof(TraceEvent), buf_pos, trace_file);
        buf_pos = 0;
    }
}

/* PIMID 1.11.100 (H40): the instructions between the ROI markers, counted in
 * FILE ORDER under the write lock (a COMPUTE event's aux_data is its block's
 * instruction count), so the replay's own count over the same file matches
 * it exactly; printed at exit. */
static bool roi_open_in_file;
static uint64_t roi_insns_in_file;
static void emit_event(const TraceEvent *evt) {
    pthread_mutex_lock(&write_lock);
    write_buf[buf_pos++] = *evt;
    total_events++;
    if (evt->event_type == EVT_ROI_BEGIN) roi_open_in_file = true;
    else if (evt->event_type == EVT_ROI_END) roi_open_in_file = false;
    else if (evt->event_type == EVT_COMPUTE_INT && roi_open_in_file) roi_insns_in_file += evt->aux_data;
    last_cycle_val = evt->cycle;
    if (total_events == 1) {
        first_cycle_val = evt->cycle;
    }
    if (buf_pos >= batch_size) {
        flush_buffer();
    }
    pthread_mutex_unlock(&write_lock);
}

/* ---- QEMU Callbacks ---- */

/**
 * Memory access callback -- emits MEM_READ or MEM_WRITE events.
 */
static void mem_cb(unsigned int vcpu_index,
                   qemu_plugin_meminfo_t info,
                   uint64_t vaddr,
                   void *userdata) {
    if (!in_roi) return;

    TraceEvent evt;
    memset(&evt, 0, sizeof(evt));

    uint32_t idx = vcpu_index < MAX_VCPUS ? vcpu_index : 0;
    evt.cycle = insn_count[idx];
    evt.address = vaddr;
    evt.pe_id = vcpu_index;
    evt.size = qemu_plugin_mem_size(info);
    evt.aux_data = evt.size;
    evt.event_type = qemu_plugin_mem_is_store(info) ? EVT_MEM_WRITE : EVT_MEM_READ;
    if (atomic_load(&thread_domain[idx]) == DOMAIN_DEVICE) {
        evt.flags |= FLAG_DEVICE_DOMAIN;
    }

    emit_event(&evt);
}

/**
 * Per-instruction execution callback -- counts instructions.
 * Called on the first instruction of each translation block.
 */
static void insn_exec_cb(unsigned int vcpu_index, void *userdata) {
    if (!in_roi) return;

    uint64_t n_insns = (uint64_t)(uintptr_t)userdata;
    uint32_t idx = vcpu_index < MAX_VCPUS ? vcpu_index : 0;
    insn_count[idx] += n_insns;

    if (vcpu_index > max_vcpu_seen) {
        max_vcpu_seen = vcpu_index;
    }
}

/**
 * Syscall callback -- emit BARRIER events for thread-creating syscalls.
 * clone/fork/vfork (syscall numbers 56, 57, 58 on x86_64).
 */
static void syscall_cb(qemu_plugin_id_t id,
                       unsigned int vcpu_index,
                       int64_t num,
                       uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6,
                       uint64_t a7, uint64_t a8) {
    /* clone=56, fork=57, vfork=58 on x86_64 */
    if (num == 56 || num == 57 || num == 58) {
        TraceEvent evt;
        memset(&evt, 0, sizeof(evt));
        uint32_t idx = vcpu_index < MAX_VCPUS ? vcpu_index : 0;
        evt.cycle = insn_count[idx];
        evt.pe_id = vcpu_index;
        evt.event_type = EVT_BARRIER;
        emit_event(&evt);
    }
}

/**
 * Magic instruction callback -- dispatches ZSim magic ops detected at
 * translation time.  The opcode (from the preceding mov $imm, %rcx) is
 * passed as userdata.  Only ROI_BEGIN/ROI_END are handled; other magic
 * ops are filtered out at translation time.
 */
static void magic_insn_exec_cb(unsigned int vcpu_index, void *userdata) {
    uint64_t op = (uint64_t)(uintptr_t)userdata;
    uint32_t idx = vcpu_index < MAX_VCPUS ? vcpu_index : 0;

    if (op == ZSIM_MAGIC_OP_ROI_BEGIN || op == ZSIM_MAGIC_OP_ROI_END) {
        emit_roi_event(vcpu_index, op);   /* PIMID 1.11.100 (H36) */
    } else if (op == ZSIM_MAGIC_OP_WORK_BEGIN) {
        atomic_store(&thread_domain[idx], DOMAIN_DEVICE);
        uint64_t cnt = atomic_fetch_add(&offload_count, 1) + 1;
        /* Emit OFFLOAD_START event */
        TraceEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.cycle = insn_count[idx];
        evt.pe_id = vcpu_index;
        evt.event_type = EVT_OFFLOAD_START;
        evt.flags = FLAG_DEVICE_DOMAIN;
        evt.aux_data = cnt;
        emit_event(&evt);
        char msg[128];
        snprintf(msg, sizeof(msg), "PIMID WORK_BEGIN vcpu=%u (offload #%lu)\n",
                 vcpu_index, (unsigned long)cnt);
        qemu_plugin_outs(msg);
        return;
    } else if (op == ZSIM_MAGIC_OP_WORK_END) {
        atomic_store(&thread_domain[idx], DOMAIN_HOST);
        /* Emit OFFLOAD_END event */
        TraceEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.cycle = insn_count[idx];
        evt.pe_id = vcpu_index;
        evt.event_type = EVT_OFFLOAD_END;
        evt.flags = FLAG_DEVICE_DOMAIN;
        emit_event(&evt);
        char msg[128];
        snprintf(msg, sizeof(msg), "PIMID WORK_END vcpu=%u\n", vcpu_index);
        qemu_plugin_outs(msg);
        return;
    } else {
        return;
    }
    uint64_t count = atomic_fetch_add(&roi_transition_count, 1) + 1;
    char msg[128];
    snprintf(msg, sizeof(msg), "PIMID ROI %s (transition #%lu)\n",
             op == ZSIM_MAGIC_OP_ROI_BEGIN ? "BEGIN" : "END",
             (unsigned long)count);
    qemu_plugin_outs(msg);
}

/**
 * Execution callback for mov $imm, %rcx at end of a TB -- stores the opcode
 * into the per-vCPU pending slot so the next TB's xchg can consume it.
 */
static void mov_magic_exec_cb(unsigned int vcpu_index, void *userdata) {
    uint64_t op = (uint64_t)(uintptr_t)userdata;
    if (vcpu_index < MAX_VCPUS) {
        atomic_store(&pending_magic_op[vcpu_index], op);
    }
}

/**
 * Execution callback for xchg %rcx, %rcx without a preceding mov in the
 * same TB.  Checks the per-vCPU pending slot for a cross-TB magic op.
 */
static void xchg_pending_exec_cb(unsigned int vcpu_index, void *userdata) {
    (void)userdata;
    if (vcpu_index >= MAX_VCPUS) return;
    uint64_t op = atomic_exchange(&pending_magic_op[vcpu_index], 0);
    if (op == ZSIM_MAGIC_OP_ROI_BEGIN || op == ZSIM_MAGIC_OP_ROI_END) {
        emit_roi_event(vcpu_index, op);   /* PIMID 1.11.100 (H36) */
    } else if (op == ZSIM_MAGIC_OP_WORK_BEGIN) {
        atomic_store(&thread_domain[vcpu_index], DOMAIN_DEVICE);
        uint64_t cnt = atomic_fetch_add(&offload_count, 1) + 1;
        TraceEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.cycle = insn_count[vcpu_index];
        evt.pe_id = vcpu_index;
        evt.event_type = EVT_OFFLOAD_START;
        evt.flags = FLAG_DEVICE_DOMAIN;
        evt.aux_data = cnt;
        emit_event(&evt);
        char msg[128];
        snprintf(msg, sizeof(msg), "PIMID WORK_BEGIN vcpu=%u (offload #%lu)\n",
                 vcpu_index, (unsigned long)cnt);
        qemu_plugin_outs(msg);
        return;
    } else if (op == ZSIM_MAGIC_OP_WORK_END) {
        atomic_store(&thread_domain[vcpu_index], DOMAIN_HOST);
        TraceEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.cycle = insn_count[vcpu_index];
        evt.pe_id = vcpu_index;
        evt.event_type = EVT_OFFLOAD_END;
        evt.flags = FLAG_DEVICE_DOMAIN;
        emit_event(&evt);
        char msg[128];
        snprintf(msg, sizeof(msg), "PIMID WORK_END vcpu=%u\n", vcpu_index);
        qemu_plugin_outs(msg);
        return;
    } else {
        return;
    }
    uint64_t count = atomic_fetch_add(&roi_transition_count, 1) + 1;
    char msg[128];
    snprintf(msg, sizeof(msg), "PIMID ROI %s (transition #%lu)\n",
             op == ZSIM_MAGIC_OP_ROI_BEGIN ? "BEGIN" : "END",
             (unsigned long)count);
    qemu_plugin_outs(msg);
}

/**
 * Translation block callback -- instruments each TB with:
 *   1. An instruction count callback on the first instruction
 *   2. Memory callbacks on each instruction
 */
static void tb_trans_cb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n_insns = qemu_plugin_tb_n_insns(tb);
    bool insn_count_registered = false;
    /* PIMID 1.11.100 (C6 / H37): the block's record for the per-execution
     * COMPUTE event, and its instruction bytes for the side file. */
    {
        TbRec *rec = (TbRec *)calloc(1, sizeof(TbRec));   /* lives as long as the plugin */
        rec->vaddr = qemu_plugin_tb_vaddr(tb);
        rec->n_insns = (uint32_t)n_insns;
        uint32_t total = 0;
        uint8_t lens[1024];
        uint8_t data[1024][16];
        size_t cap = n_insns < 1024 ? n_insns : 1024;
        for (size_t i = 0; i < cap; i++) {
            struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
            size_t sz = qemu_plugin_insn_size(insn);
            total += (uint32_t)sz;
            if (sz > 16) sz = 16;
            lens[i] = (uint8_t)sz;
            memset(data[i], 0, 16);
            qemu_plugin_insn_data(insn, data[i], sz);
        }
        rec->bytes = total;
        if (blocks_file && cap > 0) {
            pthread_mutex_lock(&blocks_lock);
            uint32_t n32 = (uint32_t)cap;
            fwrite(&rec->vaddr, 8, 1, blocks_file);
            fwrite(&n32, 4, 1, blocks_file);
            fwrite(&total, 4, 1, blocks_file);
            fwrite(lens, 1, cap, blocks_file);
            fwrite(data, 16, cap, blocks_file);
            blocks_written++;
            pthread_mutex_unlock(&blocks_lock);
        }
        qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec_cb, QEMU_PLUGIN_CB_NO_REGS, rec);
    }
    /* Track in-TB mov $imm, %rcx -> xchg %rcx, %rcx pairs.
     * prev_magic_op is local (no cross-vCPU race).  Cross-TB communication
     * is deferred to execution time via per-vCPU pending_magic_op[]. */
    uint64_t prev_magic_op = 0;
    struct qemu_plugin_insn *last_magic_mov_insn = NULL;

    for (size_t i = 0; i < n_insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        size_t insn_sz = qemu_plugin_insn_size(insn);
        uint8_t insn_buf[16];
        qemu_plugin_insn_data(insn, insn_buf, sizeof(insn_buf));
        const uint8_t *bytes = insn_buf;

        /* Detect xchg %rcx, %rcx (48 87 c9) */
        if (insn_sz == 3 && bytes[0] == 0x48 && bytes[1] == 0x87
                && bytes[2] == 0xc9) {
            if (prev_magic_op == ZSIM_MAGIC_OP_ROI_BEGIN
                    || prev_magic_op == ZSIM_MAGIC_OP_ROI_END
                    || prev_magic_op == ZSIM_MAGIC_OP_WORK_BEGIN
                    || prev_magic_op == ZSIM_MAGIC_OP_WORK_END) {
                /* In-TB case: mov+xchg in same TB -- opcode known at
                 * translation time, pass directly via userdata */
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, magic_insn_exec_cb, QEMU_PLUGIN_CB_NO_REGS,
                    (void *)(uintptr_t)prev_magic_op);
            } else {
                /* Cross-TB case: no preceding mov in this TB -- check
                 * per-vCPU pending slot at execution time */
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, xchg_pending_exec_cb, QEMU_PLUGIN_CB_NO_REGS,
                    NULL);
            }
            prev_magic_op = 0;
            last_magic_mov_insn = NULL;
            continue;  /* skip normal instrumentation for magic insn */
        }

        /* Check if this instruction is mov $imm, %rcx/%ecx (magic op setup).
         * Three possible encodings:
         *   b9 xx xx xx xx          -- mov $imm32, %ecx  (5 bytes)
         *   48 c7 c1 xx xx xx xx   -- mov $imm32, %rcx  (7 bytes)
         *   48 b9 xx*8             -- movabs $imm64, %rcx (10 bytes) */
        prev_magic_op = 0;
        last_magic_mov_insn = NULL;
        if (insn_sz == 5 && bytes[0] == 0xb9) {
            prev_magic_op = (uint32_t)bytes[1] | ((uint32_t)bytes[2] << 8)
                          | ((uint32_t)bytes[3] << 16) | ((uint32_t)bytes[4] << 24);
        } else if (insn_sz == 7 && bytes[0] == 0x48 && bytes[1] == 0xc7
                   && bytes[2] == 0xc1) {
            prev_magic_op = (uint32_t)bytes[3] | ((uint32_t)bytes[4] << 8)
                          | ((uint32_t)bytes[5] << 16) | ((uint32_t)bytes[6] << 24);
        } else if (insn_sz == 10 && bytes[0] == 0x48 && bytes[1] == 0xb9) {
            prev_magic_op = (uint32_t)bytes[2] | ((uint32_t)bytes[3] << 8)
                          | ((uint32_t)bytes[4] << 16) | ((uint32_t)bytes[5] << 24);
        }
        if (prev_magic_op != 0) {
            last_magic_mov_insn = insn;
        }

        /* Register insn count callback on first non-magic instruction of TB */
        if (!insn_count_registered) {
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, insn_exec_cb, QEMU_PLUGIN_CB_NO_REGS,
                (void *)(uintptr_t)n_insns);
            insn_count_registered = true;
        }

        /* Register memory callback for every instruction */
        qemu_plugin_register_vcpu_mem_cb(
            insn, mem_cb, QEMU_PLUGIN_CB_NO_REGS,
            QEMU_PLUGIN_MEM_RW, NULL);
    }

    /* If the TB ends with a mov $imm, %rcx that wasn't consumed by an xchg
     * in this TB, register an execution callback to store the opcode into
     * the per-vCPU pending slot for the next TB's xchg to pick up. */
    if (prev_magic_op != 0 && last_magic_mov_insn != NULL) {
        qemu_plugin_register_vcpu_insn_exec_cb(
            last_magic_mov_insn, mov_magic_exec_cb, QEMU_PLUGIN_CB_NO_REGS,
            (void *)(uintptr_t)prev_magic_op);
    }
}

/**
 * Plugin exit -- flush remaining events, emit final COMPUTE_INT summary,
 * write header, and close file.
 */
static void plugin_exit(qemu_plugin_id_t id, void *userdata) {
    (void)userdata;
    /* PIMID 1.11.100 (C6): no summary COMPUTE at exit -- every executed block
     * was emitted during the run (tb_exec_cb); the per-vcpu instruction
     * counts go into the metadata below. */
    flush_buffer();
    if (blocks_file) {
        pthread_mutex_lock(&blocks_lock);
        fclose(blocks_file);
        blocks_file = NULL;
        pthread_mutex_unlock(&blocks_lock);
        char bmsg[256];
        snprintf(bmsg, sizeof(bmsg), "PIMID trace: %lu block records (instruction bytes) written to %s.blocks; %lu COMPUTE events\n",
                 (unsigned long)blocks_written, output_path, (unsigned long)compute_events);
        qemu_plugin_outs(bmsg);
    }

    if (!trace_file) return;

    /* Write YAML metadata section after the header */
    /* For simplicity, we embed a minimal YAML string */
    char yaml_buf[512];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", tm);

    int yaml_len = snprintf(yaml_buf, sizeof(yaml_buf),
        "generator: pimid-qemu-trace\n"
        "timestamp: %s\n"
        "workload: qemu-user-mode\n"
        "num_events: %lu\n"
        "num_pes: %u\n"
        "roi_insns: %lu\n",
        time_str,
        (unsigned long)total_events,
        max_vcpu_seen + 1,
        (unsigned long)roi_insns_in_file);   /* PIMID 1.11.100 (H40): the replay and the generator's summary read it back */

    /* Rewrite the header at the beginning of the file */
    TraceHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = TRACE_MAGIC;
    hdr.version = TRACE_VERSION;
    hdr.num_events = total_events;
    /* header_size must match the original reservation (header + 512 bytes
     * for YAML) so readers know where binary events start. */
    hdr.header_size = sizeof(TraceHeader) + 512;
    hdr.num_pes = max_vcpu_seen + 1;
    hdr.first_cycle = first_cycle_val;
    hdr.last_cycle = last_cycle_val;

    fseek(trace_file, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, trace_file);
    fwrite(yaml_buf, 1, (size_t)yaml_len, trace_file);

    fclose(trace_file);
    trace_file = NULL;
    free(write_buf);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "PIMID trace: %lu events written to %s\n",
             (unsigned long)total_events, output_path);
    qemu_plugin_outs(msg);
    snprintf(msg, sizeof(msg),
             "PIMID trace: ROI instructions %lu (the blocks' instruction counts between the ROI markers, file order)\n",
             (unsigned long)roi_insns_in_file);   /* PIMID 1.11.100 (H40) */
    qemu_plugin_outs(msg);

    if (roi_transition_count > 0) {
        snprintf(msg, sizeof(msg),
                 "PIMID ROI: %lu transitions, final state: %s\n",
                 (unsigned long)roi_transition_count,
                 in_roi ? "recording" : "not recording");
        qemu_plugin_outs(msg);
    }

    uint64_t oc = atomic_load(&offload_count);
    if (oc > 0) {
        snprintf(msg, sizeof(msg),
                 "PIMID co-sim: %lu offload transitions\n",
                 (unsigned long)oc);
        qemu_plugin_outs(msg);
    }

    free(output_path);
}

/* ---- Plugin Install ---- */

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                            const qemu_info_t *info,
                                            int argc, char **argv) {
    /* Parse arguments */
    for (int i = 0; i < argc; i++) {
        char *arg = argv[i];
        if (strncmp(arg, "output=", 7) == 0) {
            output_path = strdup(arg + 7);
        } else if (strncmp(arg, "batch=", 6) == 0) {
            batch_size = (uint32_t)atoi(arg + 6);
            if (batch_size < 64) batch_size = 64;
            if (batch_size > 65536) batch_size = 65536;
        }
    }

    if (!output_path) {
        qemu_plugin_outs("ERROR: pimid_trace plugin requires output=<path> argument\n");
        return -1;
    }

    /* Initialize counters */
    memset(insn_count, 0, sizeof(insn_count));
    total_events = 0;
    first_cycle_val = 0;
    last_cycle_val = 0;
    max_vcpu_seen = 0;

    /* Allocate write buffer */
    write_buf = (TraceEvent *)calloc(batch_size, sizeof(TraceEvent));
    if (!write_buf) {
        qemu_plugin_outs("ERROR: Failed to allocate write buffer\n");
        return -1;
    }
    buf_pos = 0;

    /* Open trace file and write placeholder header (will be rewritten at exit) */
    trace_file = fopen(output_path, "wb");
    {   /* PIMID 1.11.100 (C6 / H37): the instruction-bytes side file */
        size_t bl = strlen(output_path) + 8;
        char *bpath = (char *)malloc(bl);
        snprintf(bpath, bl, "%s.blocks", output_path);
        blocks_file = fopen(bpath, "wb");
        if (!blocks_file) qemu_plugin_outs("WARNING: pimid_trace could not open the .blocks side file; the replay will use synthetic blocks\n");
        free(bpath);
    }
    if (!trace_file) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "ERROR: Failed to open trace file: %s\n", output_path);
        qemu_plugin_outs(msg);
        free(write_buf);
        return -1;
    }

    /* Write placeholder header + empty YAML section.
     * We reserve space; the real header is written at plugin_exit. */
    TraceHeader placeholder;
    memset(&placeholder, 0, sizeof(placeholder));
    placeholder.magic = TRACE_MAGIC;
    placeholder.version = TRACE_VERSION;
    /* Reserve 512 bytes for YAML metadata */
    placeholder.header_size = sizeof(TraceHeader) + 512;
    fwrite(&placeholder, sizeof(placeholder), 1, trace_file);
    /* Pad to header_size so events start at a known offset */
    char zeros[512];
    memset(zeros, 0, sizeof(zeros));
    fwrite(zeros, 1, 512, trace_file);

    /* Register callbacks */
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans_cb);
    qemu_plugin_register_vcpu_syscall_cb(id, syscall_cb);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "PIMID trace plugin loaded: output=%s, batch=%u\n",
             output_path, batch_size);
    qemu_plugin_outs(msg);

    return 0;
}

/* PIMID 1.11.90: component version stamp. The loader in src/main.cpp reads
 * the "@(#)PIMID_COMPONENT" string from this file and refuses a component
 * whose version differs from the binary's (see locatePimidComponent). */
#ifndef PIMID_VERSION
#error "PIMID_VERSION must be defined for the component version stamp"
#endif
__attribute__((visibility("default"), used))
const char pimid_trace_component_stamp[] = "@(#)PIMID_COMPONENT libpimid_trace.so " PIMID_VERSION;
__attribute__((visibility("default")))
const char* pimid_trace_component_version(void) { return PIMID_VERSION; }
