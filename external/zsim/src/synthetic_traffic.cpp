/**
 * synthetic_traffic.cpp — Standalone synthetic traffic injection for Garnet
 *
 * Adapted from gem5 Ruby Tester (Wisconsin Multifacet Project).
 * Supports 8 traffic patterns: uniform random, bit-complement, tornado,
 * neighbor, transpose, bit-reverse, bit-rotation, shuffle.
 * No coherence protocol needed — pure network-layer injection.
 */

#include "zsim_trace_api.h"
#include "garnet_network.h"

int zsim_synthetic_traffic(const char* cfgPath,
                           const char* topology, const char* routing,
                           uint32_t rows, uint32_t cols,
                           int trafficPattern, double injectionRate,
                           uint64_t numPackets, int warmupPackets,
                           uint32_t routerLat, uint32_t linkLat,
                           uint32_t vcsPerVnet, uint32_t buffersPerVc,
                           double clockMhz, uint32_t flitSizeBits) {
    SyntheticTrafficResult result;
    int rc = zsim_synthetic_traffic_ex(topology, routing, rows, cols,
                                        trafficPattern, injectionRate,
                                        numPackets, warmupPackets,
                                        routerLat, linkLat,
                                        vcsPerVnet, buffersPerVc,
                                        clockMhz, flitSizeBits, &result);
    printf("\n── Synthetic Traffic Results ──\n");
    printf("  Delivered:      %lu packets\n", result.totalPackets);
    printf("  Total cycles:   %lu\n", result.totalCycles);
    printf("  Avg latency:    %.1f cycles\n", result.avgLatency);
    printf("  Min latency:    %lu cycles\n", result.minLatency);
    printf("  Max latency:    %lu cycles\n", result.maxLatency);
    printf("  Throughput:     %.6f pkt/cycle/node\n", result.throughput);
    return rc;
}

int zsim_synthetic_traffic_ex(const char* topology, const char* routing,
                              uint32_t rows, uint32_t cols,
                              int trafficPattern, double injectionRate,
                              uint64_t numPackets, int warmupPackets,
                              uint32_t routerLat, uint32_t linkLat,
                              uint32_t vcsPerVnet, uint32_t buffersPerVc,
                              double clockMhz, uint32_t flitSizeBits,
                              SyntheticTrafficResult* outResult) {

    NoCTopology topo = parseNoCTopology(topology);
    NoCRouting  rout = parseNoCRouting(routing);

    GarnetNetwork net(topo, rows, cols,
                      routerLat, linkLat,
                      true,  // cycle-accurate
                      rout, vcsPerVnet, buffersPerVc,
                      clockMhz, flitSizeBits);

#ifdef HAVE_GARNET
    auto result = net.runSyntheticTraffic(trafficPattern, injectionRate,
                                          numPackets, warmupPackets);

    if (outResult) {
        outResult->totalPackets = result.totalPackets;
        outResult->routerFlitTraversals = result.routerFlitTraversals;   // 1.11.94 (H25)
        outResult->totalLatency = result.totalLatency;
        outResult->avgLatency = result.avgLatency;
        outResult->throughput = result.throughput;
        outResult->minLatency = result.minLatency;
        outResult->maxLatency = result.maxLatency;
        outResult->totalCycles = result.totalCycles;
        outResult->numNodes = result.numNodes;
        outResult->numRouters = result.numRouters;
        outResult->numRows = result.numRows;
        outResult->numCols = result.numCols;
        outResult->flitSizeBits = result.flitSizeBits;
        outResult->clockMhz = result.clockMhz;
        outResult->injectionRate = injectionRate;
    }

    return (result.totalPackets > 0) ? 0 : 1;
#else
    fprintf(stderr, "Error: synthetic traffic requires Garnet (HAVE_GARNET)\n");
    return 1;
#endif
}
