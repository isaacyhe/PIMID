#include "common/simulation_engine.h"
#include "common/format_scope.h"  // 1.12.15: scoped stream formats
#include "memory/memory_model.h"
#include <iostream>
#include <iomanip>

namespace pimid {

SimulationEngine::SimulationEngine(SimulationDomain domain, const PIMIDConfig& config)
    : domain_(domain), config_(config), current_cycle_(0) {
    stats_ = PIMIDStats();
}

void SimulationEngine::synchronize(Cycle target_cycle) {
    // Default implementation: advance to target cycle
    if (target_cycle > current_cycle_) {
        current_cycle_ = target_cycle;
    }
}

void SimulationEngine::issueMemoryRequest(const MemoryRequest& req) {
    std::cout << "[" << (domain_ == SimulationDomain::HOST ? "Host" : "Device")
              << "] Memory request: addr=0x" << std::hex << req.addr
              << std::dec << " size=" << req.size << std::endl;

    // Update statistics
    stats_.memory_accesses++;

    // Forward to memory model
    if (memory_model_) {
        Cycle latency = memory_model_->access(req);
        handleMemoryResponse(req, latency);
    }
}

void SimulationEngine::handleMemoryResponse(const MemoryRequest& req, Cycle latency) {
    std::cout << "[" << (domain_ == SimulationDomain::HOST ? "Host" : "Device")
              << "] Memory response: addr=0x" << std::hex << req.addr
              << std::dec << " latency=" << latency << " cycles" << std::endl;

    // Could invoke callbacks here
}

void SimulationEngine::resetStats() {
    stats_ = PIMIDStats();
    std::cout << "[" << (domain_ == SimulationDomain::HOST ? "Host" : "Device")
              << "] Statistics reset" << std::endl;
}

void SimulationEngine::printStats() const {
    std::cout << "\n=== " << (domain_ == SimulationDomain::HOST ? "Host" : "Device")
              << " Engine Statistics ===" << std::endl;
    std::cout << "Total Cycles:       " << stats_.total_cycles << std::endl;
    std::cout << "Total Instructions: " << stats_.total_instructions << std::endl;
    std::cout << "Memory Accesses:    " << stats_.memory_accesses << std::endl;
    std::cout << "Network Packets:    " << stats_.network_packets << std::endl;
    pimid::StreamFormatGuard fmt_guard_56(std::cout);  // 1.12.15: restores at the end of this block
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Total Energy:       " << stats_.total_energy_j << " J" << std::endl;
    std::cout << "  Compute:          " << stats_.compute_energy_j << " J" << std::endl;
    std::cout << "  Memory:           " << stats_.memory_energy_j << " J" << std::endl;
    std::cout << "  Network:          " << stats_.network_energy_j << " J" << std::endl;
    std::cout << "==========================================\n" << std::endl;
}

} // namespace pimid
