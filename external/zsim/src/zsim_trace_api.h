#ifndef ZSIM_TRACE_API_H_
#define ZSIM_TRACE_API_H_
#include <stdint.h>

int zsim_trace_run(const char* cfgPath, const char* tracePath, const char* outputDir);

// Synthetic traffic result (C++ only)
#ifdef __cplusplus
struct SyntheticTrafficResult {
    uint64_t totalPackets = 0;
    uint64_t routerFlitTraversals = 0;   // 1.11.94 (H25): measured router flit traversals, post-warmup
    uint64_t totalLatency = 0;
    double avgLatency = 0.0;
    double throughput = 0.0;
    uint64_t minLatency = 0;
    uint64_t maxLatency = 0;
    uint64_t totalCycles = 0;
    uint32_t numNodes = 0;
    uint32_t numRouters = 0;
    uint32_t numRows = 0;
    uint32_t numCols = 0;
    uint32_t flitSizeBits = 128;
    double clockMhz = 1000.0;
    double injectionRate = 0.0;
};
#endif

// Synthetic traffic injection for network-only testing.
// Returns 0 on success.  Results printed to stdout.
int zsim_synthetic_traffic(const char* cfgPath,
                           const char* topology, const char* routing,
                           uint32_t rows, uint32_t cols,
                           int trafficPattern, double injectionRate,
                           uint64_t numPackets, int warmupPackets,
                           uint32_t routerLat, uint32_t linkLat,
                           uint32_t vcsPerVnet, uint32_t buffersPerVc,
                           double clockMhz, uint32_t flitSizeBits);

#ifdef __cplusplus
// Extended version that returns detailed results for power analysis
int zsim_synthetic_traffic_ex(const char* topology, const char* routing,
                              uint32_t rows, uint32_t cols,
                              int trafficPattern, double injectionRate,
                              uint64_t numPackets, int warmupPackets,
                              uint32_t routerLat, uint32_t linkLat,
                              uint32_t vcsPerVnet, uint32_t buffersPerVc,
                              double clockMhz, uint32_t flitSizeBits,
                              SyntheticTrafficResult* outResult);
#endif

#endif
