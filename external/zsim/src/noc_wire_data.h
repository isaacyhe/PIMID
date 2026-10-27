/** $lic$
 * PIMID: measured data activity of the on-chip wires (NoC links, buses, TSVs).
 */

#ifndef NOC_WIRE_DATA_H_
#define NOC_WIRE_DATA_H_

/* Measured wire activity (user ruling 2026-10-25, "Measured"). A NoC link,
 * a bus or a TSV bundle is a W-bit bus carrying successive flits; its dynamic
 * energy is C V^2 per 0->1 transition of a wire (the DSENT / ORION 2.0 physical
 * convention: a 0->1 edge draws C V^2 from the supply, a 1->0 edge draws none).
 * This counts those transitions on the real data, per W:
 *
 *   what crosses   one packet per fabric access: an 8 B header + the 64 B line
 *                  (the 576 b the fabric pricing cuts into flits). The line's
 *                  bytes are the ones the access READ once for the measured DQ
 *                  statistics (one read per access) (QEMU user
 *                  mode, guest_base 0, one process: the line address is an
 *                  address of this process). The header's real bits are not
 *                  modelled by Garnet (no payload): its 64 bits are taken to be
 *                  the access's byte address (an ASSUMPTION, stated).
 *   flit cut       for W in {8, 16, 32, 64, 128, 256}: ceil(576 / W) flits, the
 *                  last one zero-padded (the fabric's own flit count).
 *   order          PER SOURCE PORT in transfer order: each source keeps its last
 *                  flit, and the next packet's first flit is compared with it.
 *                  Garnet carries no payload, so the true per-link order (other
 *                  sources' flits interleaved on a shared link) is not known;
 *                  per-source order is the closest measurable proxy (stated).
 *   counted        nocWire<W>Bits   wire-bits carried (flits x W, padding included)
 *                  nocWire<W>Rise   0->1 transitions between successive flits
 *                  nocWire<W>Toggles all transitions (0->1 and 1->0)
 *                  nocWirePackets / nocWireUnread  packets counted / not readable
 *   activity       a01(W) = Rise / Bits: 0->1 transitions per wire-bit carried;
 *                  random data gives 0.25. McPAT charges C V^2 x a01 per bit.
 *
 * The host fabric uses the same counter with its own 256-bit data path (two flits
 * per 64 B line, no header) and one stream per sending stop.
 *
 * Determinism: the counts are sums; a serial run reads the same lines in the same
 * order every time. With several PEs on one source port (RANK/CHANNEL placement,
 * OpenMP) the per-source order follows the threads' interleaving. The read never
 * changes simulated state. */

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <mutex>
#include "stats.h"

/* The guest-memory read and its readability flag are the measured DQ data
 * statistics' (dq_data_stats.h): one helper, one rule. */
#include "dq_data_stats.h"

class NocWireData {
    public:
        static const int NW = 6;
        static uint32_t width(int i) { static const uint32_t w[NW] = {8, 16, 32, 64, 128, 256}; return w[i]; }

        /* One stream's last flit per width (the previous packet's final flit). */
        struct Stream {
            uint8_t last[NW][32];
            bool have = false;
            uint32_t nextTag = 0;   // the source port's next outstanding-access tag (user ruling 2026-10-26: PIMID's own packet header)
            std::mutex mu;
        };

        Counter packets, unread;
        Counter bits[NW], rise[NW], toggles[NW];

        void initStats(AggregateStat* parent, const char* prefix, const char* desc) {
            AggregateStat* s = new AggregateStat();
            s->init(prefix, desc);
            char n[64], d[160];
            snprintf(n, sizeof(n), "%sPackets", prefix); packets.init(strdup(n), "packets whose data was read"); s->append(&packets);
            snprintf(n, sizeof(n), "%sUnread", prefix); unread.init(strdup(n), "packets whose data could not be read (not counted)"); s->append(&unread);
            snprintf(n, sizeof(n), "%sWrPackets", prefix); wrPackets.init(strdup(n), "data messages that are write requests (header + address + line)"); s->append(&wrPackets);
            snprintf(n, sizeof(n), "%sHdrBits", prefix); hdrBits.init(strdup(n), "header bits sent (PIMID's own header format)"); s->append(&hdrBits);
            snprintf(n, sizeof(n), "%sCtrlPackets", prefix); ctrlPackets.init(strdup(n), "control messages (read requests, write acks: the header word alone)"); s->append(&ctrlPackets);
            for (int i = 0; i < NW; i++) {
                snprintf(n, sizeof(n), "%s%uBits", prefix, width(i));
                snprintf(d, sizeof(d), "wire-bits carried at %u b per flit (flits x width)", width(i));
                bits[i].init(strdup(n), strdup(d)); s->append(&bits[i]);
                snprintf(n, sizeof(n), "%s%uRise", prefix, width(i));
                snprintf(d, sizeof(d), "0->1 wire transitions between successive %u-bit flits (per source)", width(i));
                rise[i].init(strdup(n), strdup(d)); s->append(&rise[i]);
                snprintf(n, sizeof(n), "%s%uToggles", prefix, width(i));
                snprintf(d, sizeof(d), "all wire transitions between successive %u-bit flits (per source)", width(i));
                toggles[i].init(strdup(n), strdup(d)); s->append(&toggles[i]);
            }
            parent->append(s);
            agg_ = s;
        }
        AggregateStat* stat() const { return agg_; }

        /* A packet of n bytes (n <= 96) sent by stream st, cut at every width in
         * [wlo, whi] (indices into width()). */
        void countPacket(Stream& st, const uint8_t* pkt, uint32_t n, int wlo, int whi) {
            uint64_t b[NW] = {0}, r[NW] = {0}, t[NW] = {0};
            std::lock_guard<std::mutex> g(st.mu);
            for (int i = wlo; i <= whi; i++) {
                const uint32_t wb = width(i) / 8;
                const uint32_t f = (n + wb - 1) / wb;
                uint8_t prev[32];
                if (st.have) memcpy(prev, st.last[i], wb); else memset(prev, 0, wb);
                uint8_t cur[32];
                for (uint32_t k = 0; k < f; k++) {
                    for (uint32_t j = 0; j < wb; j++) {
                        const uint32_t o = k * wb + j;
                        cur[j] = (o < n) ? pkt[o] : 0;
                        r[i] += pimidPop8((uint8_t)(~prev[j] & cur[j]));   // (1.12.127): table, as dq_data_stats.h
                        t[i] += pimidPop8((uint8_t)(prev[j] ^ cur[j]));
                    }
                    memcpy(prev, cur, wb);
                }
                memcpy(st.last[i], prev, wb);
                b[i] = (uint64_t)f * width(i);
            }
            st.have = true;
            packets.atomicInc();
            for (int i = wlo; i <= whi; i++) { bits[i].atomicInc(b[i]); rise[i].atomicInc(r[i]); toggles[i].atomicInc(t[i]); }
        }

        /* PIMID's packet header (user ruling 2026-10-26: PIMID's own packet header), bounded to ONE 64-bit word -- the
         * noc.header_bits (64) the fabric timing already moves with every data message, so
         * timing and energy see the same 576-bit message. (user ruling 2026-10-26: control messages): a remote
         * access is TWO messages -- a read: a READ REQUEST (control: the header word alone, to
         * the target) + a READ RESPONSE (the line, back to the requester); a write: a WRITE
         * REQUEST (the line, to the target) + a WRITE ACK (control, back) -- the 64-bit
         * control_message_bits Garnet moves for its Control class. Fields, LSB first:
         *   dst    nodeBits   destination node (Garnet node id): routing needs only this
         *   src    nodeBits   source node: where the response / write ack returns
         *   type   2          0 read request, 1 write request (+data), 2 read response (+data),
         *                     3 write ack. The message class (virtual network) and the payload
         *                     length are IMPLIED by the type (class per type; length = 0 or
         *                     one line of the run's line size), so they take no bits.
         *   tag    tagBits    the request's outstanding-access tag at the source port: the
         *                     requesting PE's slot at the port (blocking cores, one request
         *                     each), or the port's allocation order (OOO cores)
         *   addr   the rest   WRITE REQUESTS (and read requests) only: the line address
         *                     relative to the destination device's window, 64 - 2 nodeBits -
         *                     2 - tagBits bits (34 at the defaults: windows up to 1 TiB).
         *                     The caller subtracts the window base: a declared device
         *                     window's start, else the device address map's span (the
         *                     units' contiguous blocks, pagesPerUnit x units)
         * A read response carries no address (matched by tag); the unused bits are zero. */
        struct HeaderLayout { uint32_t nodeBits = 12, tagBits = 4; bool oooTags = false;
                              uint32_t addrBits() const { return 64 - 2 * nodeBits - 2 - tagBits; } };
        HeaderLayout hdr;
        Counter wrPackets, hdrBits, ctrlPackets;

        /* One data message on stream st: the 64-bit header word from the fields' real values,
         * then the line ALREADY READ for this access (line: lineBytes bytes, or nullptr). */
        /* addrOffset: the line's offset in the destination's address window (the caller
         * subtracts the window base); slotTag: the requesting PE's slot at this source port
         * (a blocking core has one outstanding request: its slot IS its tag). With an OOO
         * core (several outstanding requests per core) the tag is the port's allocation
         * order instead. */
        void countDataMessage(Stream& st, uint32_t dst, uint32_t src, bool isWrite, uint64_t addrOffset,
                              uint32_t slotTag, const uint8_t* line, uint32_t lineBytes) {
            countAccess(st, dst, src, isWrite, addrOffset, slotTag, line, lineBytes, false);
        }
        /* One remote access (user ruling 2026-10-26: control messages): with ctrl, its control message
         * (a read's request before its response; a write's ack after its request) on the
         * same stream, in transfer order, under the access's one tag. */
        void countAccess(Stream& st, uint32_t dst, uint32_t src, bool isWrite, uint64_t addrOffset,
                         uint32_t slotTag, const uint8_t* line, uint32_t lineBytes, bool ctrl) {
            const uint64_t lineAddr = addrOffset;
            uint8_t pkt[8 + 128];
            uint32_t tag;
            if (hdr.oooTags) { std::lock_guard<std::mutex> g(st.mu); tag = st.nextTag++; }
            else tag = slotTag;
            const bool lineOk = line && lineBytes <= 128;
            if (ctrl && !isWrite) countControl(st, dst, src, false, lineAddr, tag);
            if (!lineOk) {
                unread.atomicInc();
                if (ctrl && isWrite) countControl(st, dst, src, true, lineAddr, tag);
                return;
            }
            const uint64_t nmask = (1ull << hdr.nodeBits) - 1, tmask = (1ull << hdr.tagBits) - 1;
            const uint32_t ab = hdr.addrBits();
            uint64_t w = 0; uint32_t pos = 0;
            // a response returns to the requester: dst = the requesting port, src = the target
            w |= ((uint64_t)(isWrite ? dst : src) & nmask) << pos; pos += hdr.nodeBits;
            w |= ((uint64_t)(isWrite ? src : dst) & nmask) << pos; pos += hdr.nodeBits;
            w |= (uint64_t)(isWrite ? 1 : 2) << pos; pos += 2;
            w |= ((uint64_t)tag & tmask) << pos; pos += hdr.tagBits;
            if (isWrite && ab > 0) w |= (lineAddr & ((ab >= 64) ? ~0ull : ((1ull << ab) - 1))) << pos;
            const uint32_t hb = isWrite ? 64 : pos;   // bits that carry fields (the rest of the word is zero)
            memcpy(pkt, &w, 8);
            memcpy(pkt + 8, line, lineBytes);
            countPacket(st, pkt, 8 + lineBytes, 0, NW - 1);
            hdrBits.atomicInc(hb);
            if (isWrite) wrPackets.atomicInc();
            if (ctrl && isWrite) countControl(st, dst, src, true, lineAddr, tag);
        }
        /* A control message: the header word alone. A read request (type 0) goes to the
         * target and carries the line address; a write ack (type 3) returns to the writer,
         * matched by its tag (no address). */
        void countControl(Stream& st, uint32_t dst, uint32_t src, bool ack, uint64_t lineAddr, uint32_t tag) {
            const uint64_t nmask = (1ull << hdr.nodeBits) - 1, tmask = (1ull << hdr.tagBits) - 1;
            const uint32_t ab = hdr.addrBits();
            uint64_t w = 0; uint32_t pos = 0;
            w |= ((uint64_t)(ack ? src : dst) & nmask) << pos; pos += hdr.nodeBits;
            w |= ((uint64_t)(ack ? dst : src) & nmask) << pos; pos += hdr.nodeBits;
            w |= (uint64_t)(ack ? 3 : 0) << pos; pos += 2;
            w |= ((uint64_t)tag & tmask) << pos; pos += hdr.tagBits;
            if (!ack && ab > 0) w |= (lineAddr & ((ab >= 64) ? ~0ull : ((1ull << ab) - 1))) << pos;
            uint8_t pkt[8];
            memcpy(pkt, &w, 8);
            countPacket(st, pkt, 8, 0, NW - 1);
            hdrBits.atomicInc(ack ? pos : 64);
            ctrlPackets.atomicInc();
        }

        /* A host fabric data message: the line only, on the 256-bit data path. */
        void countLine(Stream& st, uint64_t addr, uint32_t lineBytes) {
            uint8_t pkt[128];
            if (!dqDataReadable || lineBytes > 128 || !pimidReadGuestBytes(addr, pkt, lineBytes)) {
                unread.atomicInc();
                return;
            }
            countPacket(st, pkt, lineBytes, NW - 1, NW - 1);
        }

    private:
        AggregateStat* agg_ = nullptr;
};

#endif  // NOC_WIRE_DATA_H_
