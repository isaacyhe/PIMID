/** $lic$
 * PIMID: measured data statistics of the lines crossing a DRAM interface.
 */

#ifndef DQ_DATA_STATS_H_
#define DQ_DATA_STATS_H_

/* Measured DQ data statistics (user ruling 2026-10-25: measured termination
 * duty). At the point where an interface counts a DRAM line read or
 * writeback (host: SimpleMemory, device: PEMemoryInterface), the line's bytes
 * are read from the guest memory -- QEMU user mode runs with guest_base 0, so
 * a guest address is an address of this process -- and counted per byte, per
 * direction (Rd = line read, Wr = line writeback / store):
 *
 *   dq<D>Lines         lines whose data was read
 *   dq<D>Unread        lines whose data could not be read (address not mapped
 *                      in this process, a multi-process run, a trace-driven
 *                      run with no guest memory); counted in nothing else
 *   dq<D>Ones          1 bits of the raw data. Raw driven-LOW pins are
 *                      8 x lineBytes x lines - ones; raw driven-HIGH are ones.
 *   dq<D>LowPodDbi     pins driven LOW with POD data bus inversion per byte:
 *                      a byte with more than four 0 bits is inverted and its
 *                      DBI_n pin is driven LOW (and counted); otherwise DBI_n
 *                      is HIGH. Micron MT40A 8Gb DDR4 data sheet, "DBI During
 *                      a READ Operation" (PDF p.180); the write rule is the
 *                      controller's mirror of it ("DBI During a WRITE
 *                      Operation", p.179).
 *   dq<D>HighLvstlDbi  pins driven HIGH with LVSTL DBI-DC per byte: a byte
 *                      with more than four 1 bits is inverted and its DMI pin
 *                      is driven HIGH (and counted). JESD209-5C 7.4.10,
 *                      "DBI data output" (PDF p.314) and "DBI data input"
 *                      (p.313).
 *   dq<D>InvPod, dq<D>InvLvstl
 *                      bytes inverted under each rule (= DBI pins asserted)
 *   dq<D>Toggles, dq<D>TogglesPodDbi, dq<D>TogglesLvstlDbi
 *                      pin transitions between consecutive beats INSIDE the
 *                      line (one beat = dqBeatBytes bytes, the interface's
 *                      channel width), raw and under each DBI rule (the DBI
 *                      pin's own transitions included). Transitions across
 *                      lines are not counted.
 *
 * The counts are sums over accesses, so they do not depend on the order in
 * which the accesses are counted; a serial run counts the same lines with the
 * same contents every time. The read never changes simulated state. */

#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include "stats.h"

/* True when the line addresses this process sees are its own guest addresses:
 * a QEMU-driven, single-process run (procIdx 0, numProcs 1). Set by the
 * driver at init; false in the trace-driven driver. */
extern bool dqDataReadable;

/* Reads n bytes of this process's memory at addr (a guest address under
 * guest_base 0). process_vm_readv on the process itself: an unmapped or
 * unreadable address returns false instead of faulting. Shared by every
 * counter that reads the data of a line, so one access reads its line once. */
/* (next, user ruling 2026-10-27): set by the QEMU plugin: reads the guest's memory through QEMU's own guest page table
 * (qemu_plugin_read_memory_vaddr) on a vCPU thread, with no system call. process_vm_readv cost ~1-2 us per line, and
 * the reads of the host's lines run inside the shared L3's lock, so a 16-thread run queued on it. Returns 1 read,
 * 0 not readable, -1 cannot read on this thread (process_vm_readv is used). nullptr: process_vm_readv.
 * Edge case (documented, ruled): QEMU reads a page the guest mapped but made unreadable (PROT_NONE / write-only, e.g.
 * a heap region given back with mmap(PROT_NONE)) through /proc/self/mem, so a line still cached from such a page is
 * counted from its contents; process_vm_readv refused it and the line was counted as unread (dq*Unread). A page the
 * guest unmapped is unreadable in both. */
extern int (*pimidGuestReadHook)(uint64_t addr, uint8_t* dst, uint32_t n);
static inline bool pimidReadGuestBytes(uint64_t addr, uint8_t* dst, uint32_t n) {
    if (pimidGuestReadHook) {
        const int r = pimidGuestReadHook(addr, dst, n);
        if (r >= 0) return r == 1;
    }
    static const pid_t self = getpid();
    struct iovec l = {dst, n}, r = {(void*)(uintptr_t)addr, n};
    return process_vm_readv(self, &l, 1, &r, 1, 0) == (ssize_t)n;
}

/* (1.12.127): the bits set in a byte, by table: the same counts as __builtin_popcount, which compiles to a libgcc call
 * per byte here (the target flags carry no POPCNT); every DRAM line and fabric data message counts 64-256 bytes */
struct PimidPop8 { uint8_t v[256]; PimidPop8() { for (int i = 0; i < 256; i++) { int c = 0; for (int k = 0; k < 8; k++) c += (i >> k) & 1; v[i] = (uint8_t)c; } } };
static inline uint32_t pimidPop8(uint8_t x) { static const PimidPop8 t; return t.v[x]; }

class DqDataStats {
    public:
        Counter lines[2], unread[2], ones[2], lowPodDbi[2], highLvstlDbi[2];
        Counter invPod[2], invLvstl[2], toggles[2], togglesPodDbi[2], togglesLvstlDbi[2];

        void setBeatBytes(uint32_t b) { beatBytes_ = b; }

        void initStats(AggregateStat* s) {
            lines[0].init("dqRdLines", "line reads whose data was read for the DQ statistics"); s->append(&lines[0]);
            unread[0].init("dqRdUnread", "line reads whose data could not be read (not mapped / multi-process / trace-driven)"); s->append(&unread[0]);
            ones[0].init("dqRdOnes", "1 bits in the data of the line reads (raw, no DBI)"); s->append(&ones[0]);
            lowPodDbi[0].init("dqRdLowPodDbi", "pins driven LOW in the line reads with POD DBI per byte (DBI_n included)"); s->append(&lowPodDbi[0]);
            highLvstlDbi[0].init("dqRdHighLvstlDbi", "pins driven HIGH in the line reads with LVSTL DBI-DC per byte (DMI included)"); s->append(&highLvstlDbi[0]);
            invPod[0].init("dqRdInvPod", "bytes inverted by POD DBI in the line reads"); s->append(&invPod[0]);
            invLvstl[0].init("dqRdInvLvstl", "bytes inverted by LVSTL DBI-DC in the line reads"); s->append(&invLvstl[0]);
            toggles[0].init("dqRdToggles", "pin transitions between beats inside the line reads (raw)"); s->append(&toggles[0]);
            togglesPodDbi[0].init("dqRdTogglesPodDbi", "pin transitions between beats inside the line reads with POD DBI (DBI_n included)"); s->append(&togglesPodDbi[0]);
            togglesLvstlDbi[0].init("dqRdTogglesLvstlDbi", "pin transitions between beats inside the line reads with LVSTL DBI-DC (DMI included)"); s->append(&togglesLvstlDbi[0]);
            lines[1].init("dqWrLines", "line writebacks whose data was read for the DQ statistics"); s->append(&lines[1]);
            unread[1].init("dqWrUnread", "line writebacks whose data could not be read (not mapped / multi-process / trace-driven)"); s->append(&unread[1]);
            ones[1].init("dqWrOnes", "1 bits in the data of the line writebacks (raw, no DBI)"); s->append(&ones[1]);
            lowPodDbi[1].init("dqWrLowPodDbi", "pins driven LOW in the line writebacks with POD DBI per byte (DBI_n included)"); s->append(&lowPodDbi[1]);
            highLvstlDbi[1].init("dqWrHighLvstlDbi", "pins driven HIGH in the line writebacks with LVSTL DBI-DC per byte (DMI included)"); s->append(&highLvstlDbi[1]);
            invPod[1].init("dqWrInvPod", "bytes inverted by POD DBI in the line writebacks"); s->append(&invPod[1]);
            invLvstl[1].init("dqWrInvLvstl", "bytes inverted by LVSTL DBI-DC in the line writebacks"); s->append(&invLvstl[1]);
            toggles[1].init("dqWrToggles", "pin transitions between beats inside the line writebacks (raw)"); s->append(&toggles[1]);
            togglesPodDbi[1].init("dqWrTogglesPodDbi", "pin transitions between beats inside the line writebacks with POD DBI (DBI_n included)"); s->append(&togglesPodDbi[1]);
            togglesLvstlDbi[1].init("dqWrTogglesLvstlDbi", "pin transitions between beats inside the line writebacks with LVSTL DBI-DC (DMI included)"); s->append(&togglesLvstlDbi[1]);
        }

        /* One line of lineBytes bytes at guest line address lineAddr (the
         * address >> lineBits) crossing the DQ pins in direction isWrite. */
        void count(uint64_t lineAddr, uint32_t lineBits, bool isWrite) {
            const uint32_t n = 1u << lineBits;
            uint8_t b[256];
            if (!dqDataReadable || n > sizeof(b) || !pimidReadGuestBytes(lineAddr << lineBits, b, n)) {
                unread[isWrite ? 1 : 0].atomicInc();
                return;
            }
            countBytes(b, n, isWrite);
        }

        /* The same count over a line already read (b: n bytes, n <= 256). */
        void countBytes(const uint8_t* b, uint32_t n, bool isWrite) {
            const int d = isWrite ? 1 : 0;
            if (n > 256) { unread[d].atomicInc(); return; }
            uint64_t o = 0, lp = 0, hl = 0, ip = 0, il = 0, t = 0, tp = 0, tl = 0;
            uint8_t ep[256], el[256]; bool fp[256], fl[256];
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t c = pimidPop8(b[i]);
                o += c;
                fp[i] = (8 - c) > 4;   // POD: more than four 0 bits -> invert, DBI_n LOW
                fl[i] = c > 4;         // LVSTL DBI-DC: more than four 1 bits -> invert, DMI HIGH
                ep[i] = fp[i] ? (uint8_t)~b[i] : b[i];
                el[i] = fl[i] ? (uint8_t)~b[i] : b[i];
                lp += fp[i] ? (c + 1) : (8 - c);
                hl += fl[i] ? ((8 - c) + 1) : c;
                ip += fp[i]; il += fl[i];
            }
            const uint32_t w = (beatBytes_ > 0 && beatBytes_ <= n && (n % beatBytes_) == 0) ? beatBytes_ : 8;
            for (uint32_t i = 0; i + w < n; i++) {
                t  += pimidPop8((uint8_t)(b[i] ^ b[i + w]));
                tp += pimidPop8((uint8_t)(ep[i] ^ ep[i + w])) + (fp[i] != fp[i + w]);
                tl += pimidPop8((uint8_t)(el[i] ^ el[i + w])) + (fl[i] != fl[i + w]);
            }
            lines[d].atomicInc(); ones[d].atomicInc(o); lowPodDbi[d].atomicInc(lp); highLvstlDbi[d].atomicInc(hl);
            invPod[d].atomicInc(ip); invLvstl[d].atomicInc(il);
            toggles[d].atomicInc(t); togglesPodDbi[d].atomicInc(tp); togglesLvstlDbi[d].atomicInc(tl);
        }

    private:
        uint32_t beatBytes_ = 8;

};

#endif  // DQ_DATA_STATS_H_
