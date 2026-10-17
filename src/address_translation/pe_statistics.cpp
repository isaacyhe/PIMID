#include "address_translation/pe_statistics.h"
#include "common/format_scope.h"  // 1.12.15: scoped stream formats
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iomanip>

namespace pimid {

PEStatisticsManager::PEStatisticsManager() : current_cycle_(0) {}

void PEStatisticsManager::registerPE(uint32_t pe_id) {
    if (pe_stats_.find(pe_id) != pe_stats_.end()) {
        std::cerr << "[PEStatistics] Warning: PE " << pe_id << " already registered" << std::endl;
        return;
    }

    PEStats stats;
    stats.pe_id = pe_id;
    pe_stats_[pe_id] = stats;
    system_stats_.total_pes++;

    std::cout << "[PEStatistics] Registered PE " << pe_id << std::endl;
}

void PEStatisticsManager::unregisterPE(uint32_t pe_id) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        pe_stats_.erase(it);
        system_stats_.total_pes--;
    }
}

void PEStatisticsManager::recordTaskCompletion(uint32_t pe_id, Cycle execution_cycles) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        it->second.tasks_completed++;
        it->second.total_execution_cycles += execution_cycles;
        system_stats_.total_tasks_completed++;
    }
}

void PEStatisticsManager::recordTaskFailure(uint32_t pe_id) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        it->second.tasks_failed++;
        system_stats_.total_tasks_failed++;
    }
}

void PEStatisticsManager::recordLocalAccess(uint32_t pe_id, uint64_t bytes) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        it->second.local_accesses++;
        it->second.total_bytes_accessed += bytes;
        system_stats_.total_local_accesses++;
        system_stats_.total_bytes_transferred += bytes;
    }
}

void PEStatisticsManager::recordRemoteAccess(uint32_t pe_id, uint64_t bytes) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        it->second.remote_accesses++;
        it->second.total_bytes_accessed += bytes;
        system_stats_.total_remote_accesses++;
        system_stats_.total_bytes_transferred += bytes;
    }
}

void PEStatisticsManager::recordBandwidthUtilization(uint32_t pe_id, double utilization_percent) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        // Update running average
        uint64_t total_samples = it->second.tasks_completed + 1;
        it->second.avg_bandwidth_utilization =
            (it->second.avg_bandwidth_utilization * (total_samples - 1) + utilization_percent) / total_samples;

        updatePeakBandwidth(pe_id, utilization_percent);

        // Track bandwidth-constrained cycles
        if (utilization_percent > 90.0) {
            it->second.bandwidth_constrained_cycles++;
        }
    }
}

void PEStatisticsManager::recordBusContention(uint32_t pe_id, Cycle contention_delay) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        it->second.bus_contention_events++;
        it->second.total_contention_delay += contention_delay;
    }
}

void PEStatisticsManager::recordEnergyConsumption(uint32_t pe_id, double compute_j, double memory_j) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        it->second.compute_energy_j += compute_j;
        it->second.memory_energy_j += memory_j;
        it->second.total_energy_j += (compute_j + memory_j);
    }
}

void PEStatisticsManager::recordIdleCycles(uint32_t pe_id, Cycle cycles) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        it->second.idle_cycles += cycles;
    }
}

const PEStats& PEStatisticsManager::getPEStats(uint32_t pe_id) const {
    static PEStats empty_stats;
    auto it = pe_stats_.find(pe_id);
    return (it != pe_stats_.end()) ? it->second : empty_stats;
}

SystemPEStats PEStatisticsManager::getSystemStats() const {
    return system_stats_;
}

void PEStatisticsManager::updateSimulationCycle(Cycle current_cycle) {
    current_cycle_ = current_cycle;
    system_stats_.simulation_cycles = current_cycle;
}

void PEStatisticsManager::calculateAggregateStats() {
    if (pe_stats_.empty()) return;

    // Calculate system-wide totals
    system_stats_.total_energy_j = 0.0;
    double total_utilization = 0.0;
    double total_bandwidth = 0.0;
    uint64_t max_tasks = 0;
    uint64_t min_tasks = UINT64_MAX;

    for (const auto& pair : pe_stats_) {
        const PEStats& stats = pair.second;

        // Energy totals
        system_stats_.total_energy_j += stats.total_energy_j;

        // Utilization
        double pe_util = calculateUtilization(stats);
        total_utilization += pe_util;

        // Bandwidth
        total_bandwidth += stats.avg_bandwidth_utilization;

        // Load balance tracking
        if (stats.tasks_completed > max_tasks) {
            max_tasks = stats.tasks_completed;
            system_stats_.most_loaded_pe = stats.pe_id;
        }
        if (stats.tasks_completed < min_tasks) {
            min_tasks = stats.tasks_completed;
            system_stats_.least_loaded_pe = stats.pe_id;
        }
    }

    // Calculate averages
    uint32_t num_pes = pe_stats_.size();
    system_stats_.avg_pe_utilization = total_utilization / num_pes;
    system_stats_.avg_bandwidth_utilization = total_bandwidth / num_pes;

    // Calculate locality ratio
    uint64_t total_accesses = system_stats_.total_local_accesses + system_stats_.total_remote_accesses;
    if (total_accesses > 0) {
        system_stats_.avg_locality_ratio =
            static_cast<double>(system_stats_.total_local_accesses) / total_accesses;
    }

    // Calculate load balance factor (inverse of coefficient of variation)
    if (max_tasks > 0) {
        system_stats_.load_balance_factor = static_cast<double>(min_tasks) / max_tasks;
    }
}

void PEStatisticsManager::resetStats() {
    for (auto& pair : pe_stats_) {
        resetPEStats(pair.first);
    }
    system_stats_ = SystemPEStats();
    current_cycle_ = 0;
}

void PEStatisticsManager::resetPEStats(uint32_t pe_id) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        uint32_t id = it->second.pe_id;
        it->second = PEStats();
        it->second.pe_id = id;
    }
}

void PEStatisticsManager::printPEStats(uint32_t pe_id) const {
    auto it = pe_stats_.find(pe_id);
    if (it == pe_stats_.end()) {
        std::cout << "PE " << pe_id << " not found" << std::endl;
        return;
    }

    const PEStats& stats = it->second;
    std::cout << "\n=== PE " << pe_id << " Statistics ===" << std::endl;
    std::cout << "Tasks: " << stats.tasks_completed << " completed, "
              << stats.tasks_failed << " failed" << std::endl;
    std::cout << "Execution: " << stats.total_execution_cycles << " cycles, "
              << stats.idle_cycles << " idle" << std::endl;
    pimid::fmt_scoped(std::cout) << "Utilization: " << std::fixed << std::setprecision(2)
              << calculateUtilization(stats) << "%" << std::endl;
    std::cout << "Memory: " << stats.local_accesses << " local, "
              << stats.remote_accesses << " remote" << std::endl;
    pimid::fmt_scoped(std::cout) << "Locality: " << std::fixed << std::setprecision(2)
              << (calculateLocalityRatio(stats) * 100.0) << "%" << std::endl;
    std::cout << "Bandwidth: avg=" << stats.avg_bandwidth_utilization << "%, "
              << "peak=" << stats.peak_bandwidth_utilization << "%" << std::endl;
    std::cout << "Bus contention: " << stats.bus_contention_events << " events, "
              << stats.total_contention_delay << " cycles delay" << std::endl;
    std::cout << "Energy: " << stats.total_energy_j << " J (compute="
              << stats.compute_energy_j << " J, memory="
              << stats.memory_energy_j << " J)" << std::endl;
}

void PEStatisticsManager::printSystemStats() const {
    std::cout << "\n=== System-Wide PE Statistics ===" << std::endl;
    std::cout << "Total PEs: " << system_stats_.total_pes << std::endl;
    std::cout << "Simulation cycles: " << system_stats_.simulation_cycles << std::endl;
    std::cout << "Total tasks: " << system_stats_.total_tasks_completed << " completed, "
              << system_stats_.total_tasks_failed << " failed" << std::endl;
    std::cout << "Total accesses: " << system_stats_.total_local_accesses << " local, "
              << system_stats_.total_remote_accesses << " remote" << std::endl;
    pimid::fmt_scoped(std::cout) << "Average PE utilization: " << std::fixed << std::setprecision(2)
              << system_stats_.avg_pe_utilization << "%" << std::endl;
    pimid::fmt_scoped(std::cout) << "Average bandwidth utilization: " << std::fixed << std::setprecision(2)
              << system_stats_.avg_bandwidth_utilization << "%" << std::endl;
    pimid::fmt_scoped(std::cout) << "Average locality ratio: " << std::fixed << std::setprecision(2)
              << (system_stats_.avg_locality_ratio * 100.0) << "%" << std::endl;
    pimid::fmt_scoped(std::cout) << "Load balance factor: " << std::fixed << std::setprecision(2)
              << system_stats_.load_balance_factor << std::endl;
    std::cout << "Most loaded PE: " << system_stats_.most_loaded_pe << std::endl;
    std::cout << "Least loaded PE: " << system_stats_.least_loaded_pe << std::endl;
    std::cout << "Total energy: " << system_stats_.total_energy_j << " J" << std::endl;
    std::cout << "Total bytes transferred: " << system_stats_.total_bytes_transferred << std::endl;
}

void PEStatisticsManager::printDetailedReport() const {
    printSystemStats();
    std::cout << "\n=== Per-PE Details ===" << std::endl;
    for (const auto& pair : pe_stats_) {
        printPEStats(pair.first);
    }
}

void PEStatisticsManager::exportToJSON(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filename << " for writing" << std::endl;
        return;
    }

    file << "{\n";
    file << "  \"system_stats\": {\n";
    file << "    \"total_pes\": " << system_stats_.total_pes << ",\n";
    file << "    \"simulation_cycles\": " << system_stats_.simulation_cycles << ",\n";
    file << "    \"total_tasks_completed\": " << system_stats_.total_tasks_completed << ",\n";
    file << "    \"avg_pe_utilization\": " << system_stats_.avg_pe_utilization << ",\n";
    file << "    \"avg_locality_ratio\": " << system_stats_.avg_locality_ratio << ",\n";
    file << "    \"load_balance_factor\": " << system_stats_.load_balance_factor << ",\n";
    file << "    \"total_energy_j\": " << system_stats_.total_energy_j << "\n";
    file << "  },\n";
    file << "  \"pe_stats\": [\n";

    bool first = true;
    for (const auto& pair : pe_stats_) {
        if (!first) file << ",\n";
        first = false;

        const PEStats& stats = pair.second;
        file << "    {\n";
        file << "      \"pe_id\": " << stats.pe_id << ",\n";
        file << "      \"tasks_completed\": " << stats.tasks_completed << ",\n";
        file << "      \"local_accesses\": " << stats.local_accesses << ",\n";
        file << "      \"remote_accesses\": " << stats.remote_accesses << ",\n";
        file << "      \"utilization\": " << calculateUtilization(stats) << ",\n";
        file << "      \"locality_ratio\": " << calculateLocalityRatio(stats) << ",\n";
        file << "      \"total_energy_j\": " << stats.total_energy_j << "\n";
        file << "    }";
    }

    file << "\n  ]\n";
    file << "}\n";
    file.close();

    std::cout << "[PEStatistics] Exported to JSON: " << filename << std::endl;
}

void PEStatisticsManager::exportToCSV(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filename << " for writing" << std::endl;
        return;
    }

    // CSV header
    file << "PE_ID,Tasks_Completed,Tasks_Failed,Execution_Cycles,Idle_Cycles,";
    file << "Local_Accesses,Remote_Accesses,Utilization,Locality_Ratio,";
    file << "Avg_Bandwidth,Peak_Bandwidth,Contention_Events,Total_Energy\n";

    // Data rows
    for (const auto& pair : pe_stats_) {
        const PEStats& stats = pair.second;
        file << stats.pe_id << ","
             << stats.tasks_completed << ","
             << stats.tasks_failed << ","
             << stats.total_execution_cycles << ","
             << stats.idle_cycles << ","
             << stats.local_accesses << ","
             << stats.remote_accesses << ","
             << calculateUtilization(stats) << ","
             << calculateLocalityRatio(stats) << ","
             << stats.avg_bandwidth_utilization << ","
             << stats.peak_bandwidth_utilization << ","
             << stats.bus_contention_events << ","
             << stats.total_energy_j << "\n";
    }

    file.close();
    std::cout << "[PEStatistics] Exported to CSV: " << filename << std::endl;
}

std::vector<uint32_t> PEStatisticsManager::getTopLoadedPEs(uint32_t count) const {
    std::vector<std::pair<uint32_t, uint64_t>> pe_loads;
    for (const auto& pair : pe_stats_) {
        pe_loads.push_back({pair.first, pair.second.tasks_completed});
    }

    std::sort(pe_loads.begin(), pe_loads.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<uint32_t> result;
    for (size_t i = 0; i < std::min(static_cast<size_t>(count), pe_loads.size()); i++) {
        result.push_back(pe_loads[i].first);
    }

    return result;
}

std::vector<uint32_t> PEStatisticsManager::getBottleneckPEs() const {
    std::vector<uint32_t> bottlenecks;
    for (const auto& pair : pe_stats_) {
        const PEStats& stats = pair.second;
        // Consider PE a bottleneck if it has high contention or low utilization
        if (stats.bus_contention_events > 100 ||
            (stats.avg_bandwidth_utilization > 90.0 && stats.bandwidth_constrained_cycles > 1000)) {
            bottlenecks.push_back(stats.pe_id);
        }
    }
    return bottlenecks;
}

std::map<uint32_t, double> PEStatisticsManager::getLoadDistribution() const {
    std::map<uint32_t, double> distribution;
    for (const auto& pair : pe_stats_) {
        distribution[pair.first] = calculateUtilization(pair.second);
    }
    return distribution;
}

double PEStatisticsManager::calculateLoadImbalance() const {
    return 1.0 - system_stats_.load_balance_factor;
}

void PEStatisticsManager::updatePeakBandwidth(uint32_t pe_id, double utilization) {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        if (utilization > it->second.peak_bandwidth_utilization) {
            it->second.peak_bandwidth_utilization = utilization;
        }
    }
}

double PEStatisticsManager::calculateUtilization(const PEStats& stats) const {
    Cycle total_cycles = stats.total_execution_cycles + stats.idle_cycles;
    if (total_cycles == 0) return 0.0;
    return (static_cast<double>(stats.total_execution_cycles) / total_cycles) * 100.0;
}

double PEStatisticsManager::calculateLocalityRatio(const PEStats& stats) const {
    uint64_t total = stats.local_accesses + stats.remote_accesses;
    if (total == 0) return 0.0;
    return static_cast<double>(stats.local_accesses) / total;
}

} // namespace pimid
