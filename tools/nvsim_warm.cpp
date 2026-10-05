// 1.11.25: NVSim cache warmer.
//
// The pregenerated NVSim set is what every run actually reads -- characterization
// costs minutes, so a value absent from the cache does not exist for the corpus.
// The wrapper exposes the mat latency as a cacheable sub-bank timing metric.
//
// Reverse-engineering which simulation config produces a given cache entry is
// guesswork; this drives NVSimWrapper directly for an explicit tuple, which is
// exactly what the cache key is.
//
// WHY A MISS IS EXPENSIVE (measured 1.11.52): a characterization explores
// NVSim's full design space -- the 13-deep BIGFOR nest over mat/subarray
// partitioning, mux levels and buffer-area levels -- and the runs in this tree
// report 584,492 to 1,160,943 VALID design points, each one a complete bank
// build plus circuit solve. That is why the cache exists, and why every key
// axis must be warmable from here.
//
// 1.11.52: the DEVICE CORNER is a key axis (added to the key by 1.11.49, since
// an LSTP query served an HP entry) and this tool could not set it, so 8 of the
// 9 (type x corner) combinations the fleet can ask for were unwarmable and
// would each pay a full search inside a simulation job. Now settable.
//
// 1.11.57 (audit C013): TEMPERATURE is the other key axis, added to the key by
// the same release that added the corner (1.11.52, since power.temperature_k
// became a real resolved knob), and this tool could not set it either -- so it
// could only ever produce the 350 K entries and every non-350 K NVM cell paid a
// full design-space search inside the simulation job, at each NVMConfig site in
// the run. The file's own contract two paragraphs up is "every key axis must be
// warmable from here"; it now is.
//
// Usage: nvsim_warm <type> <capacity_bytes> <node_nm> <width_bits> [corner] [temp_k]
//        type:   0=STTRAM 1=PCRAM 2=RERAM
//        corner: 0=HP (default) 1=LSTP 2=LOP   (ITRS roadmap column)
//        temp_k: operating temperature in Kelvin (default 350, ~77 degC)
#include "memory/nvsim_wrapper.h"
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 5 || argc > 7) {
        std::fprintf(stderr,
            "usage: %s <type 0|1|2> <capacity_bytes> <node_nm> <width_bits> "
            "[corner 0|1|2] [temp_k 300..400 step 10]\n", argv[0]);
        return 2;
    }
    const int type = std::atoi(argv[1]);
    const int corner = (argc >= 6) ? std::atoi(argv[5]) : 0;
    const int temp_k = (argc >= 7) ? std::atoi(argv[6]) : 350;
    if (corner < 0 || corner > 2) {
        std::fprintf(stderr, "[nvsim_warm] corner must be 0 (HP), 1 (LSTP) or 2 (LOP)\n");
        return 2;
    }
    /* 1.11.57 (audit C013): the SAME range the simulator's validator enforces
     * (main.cpp, power.temperature_k). NVSim indexes a 300..400 K table with no
     * bounds check of its own, so warming an out-of-range entry would be an
     * out-of-bounds read, and warming an off-step one would produce a cache
     * entry no legal run can ever ask for. */
    if (temp_k < 300 || temp_k > 400 || (temp_k % 10) != 0) {
        std::fprintf(stderr,
            "[nvsim_warm] temp_k must be 300..400 K in steps of 10 "
            "(CACTI's checked range; NVSim's tables are indexed unchecked)\n");
        return 2;
    }
    pimid::NVSimWrapper::NVMConfig cfg;
    cfg.nvm_type = (type == 1) ? pimid::NVSimWrapper::NVMType::PCRAM
                 : (type == 2) ? pimid::NVSimWrapper::NVMType::RERAM
                               : pimid::NVSimWrapper::NVMType::STTRAM;
    cfg.capacity_bytes   = std::strtoull(argv[2], nullptr, 10);
    cfg.process_node_nm  = std::atoi(argv[3]);
    cfg.word_width_bits  = std::atoi(argv[4]);
    cfg.device_corner    = corner;   // 1.11.52: the 1.11.49 key axis
    cfg.temperature_k    = temp_k;   // 1.11.57 (C013): the 1.11.52 key axis

    pimid::NVSimWrapper w(cfg);
    w.initialize();
    if (!w.isValid()) {
        std::fprintf(stderr,
                     "[nvsim_warm] characterization FAILED for t%d c%s n%s w%s dc%d t%d\n",
                     type, argv[2], argv[3], argv[4], corner, temp_k);
        return 1;
    }
    std::printf("[nvsim_warm] t%d c%s n%s w%s dc%d t%d: read=%.6e s  "
                "mat=%.6e s  leak=%.4f mW\n",
                type, argv[2], argv[3], argv[4], corner, temp_k,
                w.getReadLatency(), w.getMatLatency(),
                w.getLeakagePower());
    return 0;
}
