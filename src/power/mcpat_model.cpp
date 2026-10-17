#include "power/power_model.h"
#include "common/format_scope.h"  // 1.12.15: scoped stream formats
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <cmath>
#include <yaml-cpp/yaml.h>

namespace pimid {

//=============================================================================
// McPATModel Implementation
//=============================================================================

McPATModel::McPATModel(const TechnologyParams& params)
    : PowerModel(params)
    , mcpat_instance_(nullptr) {

    std::cout << "[McPATModel] Creating McPAT-based power model" << std::endl;
}

McPATModel::~McPATModel() {
#ifdef HAVE_MCPAT
    // Clean up McPAT instance
    if (mcpat_instance_) {
        // McPAT cleanup happens through finalize()
        mcpat_instance_ = nullptr;
    }
#endif
}

void McPATModel::initialize() {
    std::cout << "[McPATModel] Initializing McPAT power model..." << std::endl;

    // Initialize default configurations for different components
    initializeDefaultConfigs();

#ifdef HAVE_MCPAT
    // Initialize McPAT instance
    try {
        // Generate McPAT configuration XML based on technology params
        std::string mcpat_config = generateMcPATConfigXML();

        // Write config to temp file for McPAT
        std::ofstream config_file("/tmp/mcpat_config.xml");
        if (config_file.is_open()) {
            config_file << mcpat_config;
            config_file.close();
        }

        // Note: Actual McPAT initialization would be done via:
        // mcpat_instance_ = new McPATProcessor(xml_config);
        // For now, we use analytical models that match McPAT's methodology
        std::cout << "[McPATModel] McPAT-compatible configuration generated" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[McPATModel] McPAT init error: " << e.what() << std::endl;
        std::cerr << "[McPATModel] Using analytical power model" << std::endl;
    }
#else
    std::cout << "[McPATModel] McPAT not linked, using analytical power model" << std::endl;
#endif

    std::cout << "[McPATModel] Technology parameters:" << std::endl;
    std::cout << "  Node: " << tech_params_.tech_node_nm << " nm" << std::endl;
    std::cout << "  Device: " << tech_params_.device_type << std::endl;
    std::cout << "  Temperature: " << tech_params_.temperature_k << " K" << std::endl;
    std::cout << "  Frequency: " << tech_params_.frequency_ghz << " GHz" << std::endl;

    std::cout << "[McPATModel] Initialization complete" << std::endl;
}

void McPATModel::loadConfig(const std::string& config_path) {
    std::cout << "[McPATModel] Loading configuration from: " << config_path << std::endl;

    try {
        YAML::Node config = YAML::LoadFile(config_path);

        if (!config["power_model"]) {
            std::cout << "[McPATModel] No 'power_model' section found, using defaults" << std::endl;
            return;
        }

        YAML::Node power_cfg = config["power_model"];

        // Load technology parameters
        if (power_cfg["technology"]) {
            auto tech = power_cfg["technology"];
            if (tech["node_nm"]) {
                tech_params_.tech_node_nm = tech["node_nm"].as<int>();
            }
            if (tech["device_type"]) {
                tech_params_.device_type = tech["device_type"].as<std::string>();
            }
            if (tech["temperature_k"]) {
                tech_params_.temperature_k = tech["temperature_k"].as<int>();
            }
            if (tech["frequency_ghz"]) {
                tech_params_.frequency_ghz = tech["frequency_ghz"].as<double>();
            }
            if (tech["vdd"]) {
                tech_params_.supply_voltage_v = tech["vdd"].as<double>();
            }
            if (tech["supply_voltage_v"]) {
                tech_params_.supply_voltage_v = tech["supply_voltage_v"].as<double>();
            }
        }

        // Load core configuration
        if (power_cfg["core"]) {
            auto core = power_cfg["core"];
            CoreConfig core_cfg;
            if (core["pipeline_depth"]) core_cfg.pipeline_depth = core["pipeline_depth"].as<int>();
            if (core["issue_width"]) core_cfg.issue_width = core["issue_width"].as<int>();
            if (core["alu_count"]) core_cfg.alu_count = core["alu_count"].as<int>();
            if (core["mul_count"]) core_cfg.mul_count = core["mul_count"].as<int>();
            if (core["fpu_count"]) core_cfg.fpu_count = core["fpu_count"].as<int>();
            core_configs_[PowerComponent::CORE] = core_cfg;
        }

        // Load cache configurations
        auto loadCacheConfig = [&](const std::string& key, PowerComponent comp) {
            if (power_cfg[key]) {
                auto cache = power_cfg[key];
                CacheConfig cache_cfg;
                if (cache["size_kb"]) cache_cfg.size_bytes = cache["size_kb"].as<uint64_t>() * 1024;
                if (cache["line_size"]) cache_cfg.line_size = cache["line_size"].as<uint32_t>();
                if (cache["associativity"]) cache_cfg.associativity = cache["associativity"].as<uint32_t>();
                if (cache["banks"]) cache_cfg.banks = cache["banks"].as<uint32_t>();
                if (cache["replacement"]) cache_cfg.replacement_policy = cache["replacement"].as<std::string>();
                cache_configs_[comp] = cache_cfg;
            }
        };

        loadCacheConfig("l1_cache", PowerComponent::L1_CACHE);
        loadCacheConfig("l2_cache", PowerComponent::L2_CACHE);
        loadCacheConfig("l3_cache", PowerComponent::L3_CACHE);

        // Load PE configuration (similar to core but potentially simpler)
        if (power_cfg["pe"]) {
            auto pe = power_cfg["pe"];
            CoreConfig pe_cfg;
            if (pe["pipeline_depth"]) pe_cfg.pipeline_depth = pe["pipeline_depth"].as<int>();
            if (pe["issue_width"]) pe_cfg.issue_width = pe["issue_width"].as<int>();
            if (pe["alu_count"]) pe_cfg.alu_count = pe["alu_count"].as<int>();
            if (pe["mul_count"]) pe_cfg.mul_count = pe["mul_count"].as<int>();
            if (pe["fpu_count"]) pe_cfg.fpu_count = pe["fpu_count"].as<int>();
            core_configs_[PowerComponent::PE] = pe_cfg;
        }

        std::cout << "[McPATModel] Successfully loaded configuration from YAML" << std::endl;

    } catch (const YAML::Exception& e) {
        std::cerr << "[McPATModel] YAML parsing error: " << e.what() << std::endl;
        std::cerr << "[McPATModel] Using default configuration" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[McPATModel] Error loading config: " << e.what() << std::endl;
        std::cerr << "[McPATModel] Using default configuration" << std::endl;
    }
}

PowerMetrics McPATModel::estimatePower(PowerComponent component,
                                        const ActivityStats& stats) {
    PowerMetrics metrics;

    // Calculate power based on component type and activity
    switch (component) {
        case PowerComponent::CORE:
            metrics = estimateCorePower(stats);
            break;

        case PowerComponent::L1_CACHE:
        case PowerComponent::L2_CACHE:
        case PowerComponent::L3_CACHE:
            metrics = estimateCachePower(component, stats);
            break;

        case PowerComponent::MEMORY_CONTROLLER:
            metrics = estimateMemoryControllerPower(stats);
            break;

        case PowerComponent::PE:
            metrics = estimatePEPower(stats);
            break;

        case PowerComponent::NETWORK_ROUTER:
        case PowerComponent::NETWORK_LINK:
        case PowerComponent::MEMORY:
            // These are handled by specialized models
            break;
    }

    // Update stored metrics
    component_metrics_[component] = metrics;
    activity_stats_[component] = stats;

    return metrics;
}

double McPATModel::getDynamicPower(PowerComponent component) const {
    auto it = component_metrics_.find(component);
    if (it != component_metrics_.end()) {
        return it->second.dynamic_power_w;
    }
    return 0.0;
}

double McPATModel::getLeakagePower(PowerComponent component) const {
    auto it = component_metrics_.find(component);
    if (it != component_metrics_.end()) {
        return it->second.leakage_power_w;
    }
    return 0.0;
}

double McPATModel::getTotalPower() const {
    double total = 0.0;
    for (const auto& pair : component_metrics_) {
        total += pair.second.total_power_w;
    }
    return total;
}

double McPATModel::getEnergy(PowerComponent component) const {
    auto it = component_metrics_.find(component);
    if (it != component_metrics_.end()) {
        return it->second.total_energy_j;
    }
    return 0.0;
}

double McPATModel::getTotalEnergy() const {
    double total = 0.0;
    for (const auto& pair : component_metrics_) {
        total += pair.second.total_energy_j;
    }
    return total;
}

void McPATModel::updateActivity(PowerComponent component,
                                 const ActivityStats& stats) {
    activity_stats_[component] = stats;

    // Re-estimate power with new activity
    auto metrics = estimatePower(component, stats);

    // Accumulate energy
    double time_s = static_cast<double>(stats.total_cycles) /
                   (tech_params_.frequency_ghz * 1e9);
    component_metrics_[component].total_energy_j +=
        metrics.total_power_w * time_s;
}

void McPATModel::printStats() const {
    std::cout << "\n=== McPAT Power Model Statistics ===" << std::endl;

    pimid::StreamFormatGuard fmt_guard_256(std::cout);  // 1.12.15: restores at the end of this block
    std::cout << std::fixed << std::setprecision(3);

    for (const auto& pair : component_metrics_) {
        std::cout << "\nComponent: " << componentToString(pair.first) << std::endl;
        std::cout << "  Dynamic Power: " << pair.second.dynamic_power_w << " W" << std::endl;
        std::cout << "  Leakage Power: " << pair.second.leakage_power_w << " W" << std::endl;
        std::cout << "  Total Power: " << pair.second.total_power_w << " W" << std::endl;
        std::cout << "  Total Energy: " << pair.second.total_energy_j << " J" << std::endl;

        // Print activity stats if available
        auto stats_it = activity_stats_.find(pair.first);
        if (stats_it != activity_stats_.end()) {
            const auto& stats = stats_it->second;
            std::cout << "  Activity:" << std::endl;
            std::cout << "    Total Cycles: " << stats.total_cycles << std::endl;
            std::cout << "    Total Instructions: " << stats.total_instructions << std::endl;
            if (stats.total_cycles > 0) {
                double ipc = static_cast<double>(stats.total_instructions) / stats.total_cycles;
                std::cout << "    IPC: " << ipc << std::endl;
            }
        }
    }

    std::cout << "\n--- Summary ---" << std::endl;
    std::cout << "Total Power: " << getTotalPower() << " W" << std::endl;
    std::cout << "Total Energy: " << getTotalEnergy() << " J" << std::endl;
    std::cout << "=====================================\n" << std::endl;
}

void McPATModel::resetStats() {
    component_metrics_.clear();
    activity_stats_.clear();
}

//=============================================================================
// Private Helper Functions
//=============================================================================

void McPATModel::initializeDefaultConfigs() {
    // Default core configuration (out-of-order)
    CoreConfig core_cfg;
    core_cfg.pipeline_depth = 14;
    core_cfg.issue_width = 4;
    core_cfg.alu_count = 4;
    core_cfg.mul_count = 2;
    core_cfg.fpu_count = 2;
    core_configs_[PowerComponent::CORE] = core_cfg;
    core_configs_[PowerComponent::PE] = core_cfg;  // PEs use similar config

    // Cache configurations
    CacheConfig l1_cfg;
    l1_cfg.size_bytes = 32 * 1024;  // 32 KB
    l1_cfg.line_size = 64;
    l1_cfg.associativity = 8;
    l1_cfg.banks = 2;
    l1_cfg.replacement_policy = "LRU";
    cache_configs_[PowerComponent::L1_CACHE] = l1_cfg;

    CacheConfig l2_cfg;
    l2_cfg.size_bytes = 256 * 1024;  // 256 KB
    l2_cfg.line_size = 64;
    l2_cfg.associativity = 8;
    l2_cfg.banks = 4;
    l2_cfg.replacement_policy = "LRU";
    cache_configs_[PowerComponent::L2_CACHE] = l2_cfg;

    CacheConfig l3_cfg;
    l3_cfg.size_bytes = 8 * 1024 * 1024;  // 8 MB
    l3_cfg.line_size = 64;
    l3_cfg.associativity = 16;
    l3_cfg.banks = 16;
    l3_cfg.replacement_policy = "LRU";
    cache_configs_[PowerComponent::L3_CACHE] = l3_cfg;
}

PowerMetrics McPATModel::estimateCorePower(const ActivityStats& stats) {
    PowerMetrics metrics;

    // Technology-dependent base power (simplified model)
    double tech_factor = 45.0 / tech_params_.tech_node_nm;  // Normalized to 45nm
    double freq_factor = tech_params_.frequency_ghz / 2.0;   // Normalized to 2 GHz

    // Calculate dynamic power based on activity
    double utilization = 0.0;
    if (stats.total_cycles > 0) {
        utilization = static_cast<double>(stats.total_instructions) / stats.total_cycles;
        utilization = std::min(utilization, 4.0) / 4.0;  // Normalize to issue width
    }

    // Base dynamic power for active core (typical 22nm @ 2GHz)
    double base_dynamic = 5.0 * tech_factor * freq_factor;
    metrics.dynamic_power_w = base_dynamic * utilization;

    // Leakage power (scales with temperature)
    double temp_factor = std::exp((tech_params_.temperature_k - 350.0) / 50.0);
    metrics.leakage_power_w = 1.0 * tech_factor * temp_factor;

    metrics.total_power_w = metrics.dynamic_power_w + metrics.leakage_power_w;

    return metrics;
}

PowerMetrics McPATModel::estimateCachePower(PowerComponent component,
                                              const ActivityStats& stats) {
    PowerMetrics metrics;

    auto it = cache_configs_.find(component);
    if (it == cache_configs_.end()) {
        return metrics;
    }

    const auto& cfg = it->second;

    // Technology scaling
    double tech_factor = 45.0 / tech_params_.tech_node_nm;

    // Calculate accesses based on component
    uint64_t reads = 0, writes = 0;
    switch (component) {
        case PowerComponent::L1_CACHE:
            reads = stats.l1_reads;
            writes = stats.l1_writes;
            break;
        case PowerComponent::L2_CACHE:
            reads = stats.l2_reads;
            writes = stats.l2_writes;
            break;
        case PowerComponent::L3_CACHE:
            reads = stats.memory_reads;
            writes = stats.memory_writes;
            break;
        default:
            break;
    }

    // Energy per access (simplified CACTI-based model)
    double size_mb = cfg.size_bytes / (1024.0 * 1024.0);
    double read_energy_nj = 0.1 * size_mb * tech_factor;
    double write_energy_nj = 0.15 * size_mb * tech_factor;

    // Dynamic power = (energy per access * accesses) / time
    if (stats.total_cycles > 0) {
        double time_s = stats.total_cycles / (tech_params_.frequency_ghz * 1e9);
        double total_energy_j = (reads * read_energy_nj + writes * write_energy_nj) * 1e-9;
        metrics.dynamic_power_w = total_energy_j / time_s;
    }

    // Leakage power (scales with cache size)
    metrics.leakage_power_w = 0.05 * size_mb * tech_factor;

    metrics.total_power_w = metrics.dynamic_power_w + metrics.leakage_power_w;

    return metrics;
}

PowerMetrics McPATModel::estimateMemoryControllerPower(const ActivityStats& stats) {
    PowerMetrics metrics;

    double tech_factor = 45.0 / tech_params_.tech_node_nm;

    // Dynamic power based on memory traffic
    uint64_t total_accesses = stats.memory_reads + stats.memory_writes;
    if (stats.total_cycles > 0 && total_accesses > 0) {
        double access_rate = static_cast<double>(total_accesses) / stats.total_cycles;
        metrics.dynamic_power_w = 2.0 * access_rate * tech_factor;
    }

    // Leakage power
    metrics.leakage_power_w = 0.5 * tech_factor;

    metrics.total_power_w = metrics.dynamic_power_w + metrics.leakage_power_w;

    return metrics;
}

PowerMetrics McPATModel::estimatePEPower(const ActivityStats& stats) {
    // Processing elements use similar model to cores but may be simpler
    PowerMetrics metrics = estimateCorePower(stats);

    // PEs might have lower complexity
    metrics.dynamic_power_w *= 0.7;  // 70% of full core
    metrics.leakage_power_w *= 0.7;
    metrics.total_power_w = metrics.dynamic_power_w + metrics.leakage_power_w;

    return metrics;
}

void McPATModel::generateMcPATInput(PowerComponent component,
                                     const ActivityStats& stats) {
    // Generate XML input file for McPAT
    std::ofstream xml_file("mcpat_input.xml");

    if (!xml_file.is_open()) {
        std::cerr << "[McPATModel] ERROR: Could not create mcpat_input.xml" << std::endl;
        return;
    }

    xml_file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml_file << "<component id=\"root\" name=\"root\">\n";
    xml_file << "  <param name=\"number_of_cores\" value=\"" << getNumCores(component) << "\"/>\n";
    xml_file << "  <param name=\"number_of_L1Directories\" value=\"0\"/>\n";
    xml_file << "  <param name=\"number_of_L2Directories\" value=\"0\"/>\n";
    xml_file << "  <param name=\"number_of_L2s\" value=\"" << (component == PowerComponent::L2_CACHE ? 1 : 0) << "\"/>\n";
    xml_file << "  <param name=\"number_of_L3s\" value=\"" << (component == PowerComponent::L3_CACHE ? 1 : 0) << "\"/>\n";
    xml_file << "  <param name=\"number_of_NoCs\" value=\"0\"/>\n";
    xml_file << "  <param name=\"homogeneous_cores\" value=\"1\"/>\n";
    xml_file << "  <param name=\"homogeneous_L2s\" value=\"1\"/>\n";
    xml_file << "  <param name=\"homogeneous_L1s\" value=\"1\"/>\n";
    xml_file << "  <param name=\"homogeneous_L3s\" value=\"1\"/>\n";
    xml_file << "  <param name=\"homogeneous_ccs\" value=\"1\"/>\n";
    xml_file << "  <param name=\"homogeneous_NoCs\" value=\"1\"/>\n";
    xml_file << "  <param name=\"core_tech_node\" value=\"" << tech_params_.tech_node_nm << "\"/>\n";
    xml_file << "  <param name=\"target_core_clockrate\" value=\"" << static_cast<int>(tech_params_.frequency_ghz * 1000) << "\"/>\n";
    xml_file << "  <param name=\"temperature\" value=\"" << static_cast<int>(tech_params_.temperature_k - 273.15) << "\"/>\n";
    xml_file << "  <param name=\"number_cache_levels\" value=\"3\"/>\n";
    xml_file << "  <param name=\"interconnect_projection_type\" value=\"0\"/>\n";
    xml_file << "  <param name=\"device_type\" value=\"0\"/>\n";
    xml_file << "  <param name=\"longer_channel_device\" value=\"1\"/>\n";
    xml_file << "  <param name=\"power_gating\" value=\"0\"/>\n";
    xml_file << "  <param name=\"machine_bits\" value=\"64\"/>\n";
    xml_file << "  <param name=\"virtual_address_width\" value=\"64\"/>\n";
    xml_file << "  <param name=\"physical_address_width\" value=\"48\"/>\n";
    xml_file << "  <param name=\"virtual_memory_page_size\" value=\"4096\"/>\n";

    // Add statistics based on component type and activity
    xml_file << "  <stat name=\"total_cycles\" value=\"" << stats.total_cycles << "\"/>\n";
    xml_file << "  <stat name=\"idle_cycles\" value=\"" << (stats.total_cycles - stats.total_instructions) << "\"/>\n";
    xml_file << "  <stat name=\"busy_cycles\" value=\"" << stats.total_instructions << "\"/>\n";

    if (component == PowerComponent::CORE || component == PowerComponent::PE) {
        generateCoreXML(xml_file, stats);
    } else if (component == PowerComponent::L1_CACHE) {
        generateCacheXML(xml_file, "L1", stats);
    } else if (component == PowerComponent::L2_CACHE) {
        generateCacheXML(xml_file, "L2", stats);
    } else if (component == PowerComponent::L3_CACHE) {
        generateCacheXML(xml_file, "L3", stats);
    } else if (component == PowerComponent::MEMORY_CONTROLLER) {
        generateMemoryControllerXML(xml_file, stats);
    }

    xml_file << "</component>\n";
    xml_file.close();

    std::cout << "[McPATModel] Generated McPAT XML input: mcpat_input.xml" << std::endl;
}

PowerMetrics McPATModel::parseMcPATOutput() {
    // Parse McPAT XML output file
    PowerMetrics metrics;

    std::ifstream xml_file("mcpat_output.xml");
    if (!xml_file.is_open()) {
        std::cerr << "[McPATModel] WARNING: Could not open mcpat_output.xml" << std::endl;
        return metrics;
    }

    std::string line;
    double total_leakage = 0.0;
    double total_dynamic = 0.0;
    double total_area = 0.0;

    // Parse XML output (simple text-based parsing)
    while (std::getline(xml_file, line)) {
        // Look for key power metrics in XML
        if (line.find("Total Leakage") != std::string::npos) {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                std::string value_str = line.substr(pos + 1);
                // Extract numerical value (format: = X.XXX W)
                size_t w_pos = value_str.find("W");
                if (w_pos != std::string::npos) {
                    value_str = value_str.substr(0, w_pos);
                    try {
                        total_leakage += std::stod(value_str);
                    } catch (...) {}
                }
            }
        }
        else if (line.find("Runtime Dynamic") != std::string::npos ||
                 line.find("Total Dynamic") != std::string::npos) {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                std::string value_str = line.substr(pos + 1);
                size_t w_pos = value_str.find("W");
                if (w_pos != std::string::npos) {
                    value_str = value_str.substr(0, w_pos);
                    try {
                        total_dynamic += std::stod(value_str);
                    } catch (...) {}
                }
            }
        }
        else if (line.find("Area") != std::string::npos) {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                std::string value_str = line.substr(pos + 1);
                size_t mm_pos = value_str.find("mm");
                if (mm_pos != std::string::npos) {
                    value_str = value_str.substr(0, mm_pos);
                    try {
                        total_area += std::stod(value_str);
                    } catch (...) {}
                }
            }
        }
    }

    xml_file.close();

    // Populate metrics
    metrics.leakage_power_w = total_leakage;
    metrics.dynamic_power_w = total_dynamic;
    metrics.total_power_w = total_leakage + total_dynamic;

    std::cout << "[McPATModel] Parsed McPAT output:" << std::endl;
    std::cout << "  Dynamic Power: " << metrics.dynamic_power_w << " W" << std::endl;
    std::cout << "  Leakage Power: " << metrics.leakage_power_w << " W" << std::endl;
    std::cout << "  Total Power: " << metrics.total_power_w << " W" << std::endl;
    std::cout << "  Area: " << total_area << " mm^2" << std::endl;

    return metrics;
}

// Helper functions for XML generation
void McPATModel::generateCoreXML(std::ofstream& xml, const ActivityStats& stats) {
    xml << "  <component id=\"system.core0\" name=\"core0\">\n";
    xml << "    <param name=\"clock_rate\" value=\"" << static_cast<int>(tech_params_.frequency_ghz * 1000) << "\"/>\n";
    xml << "    <param name=\"instruction_length\" value=\"32\"/>\n";
    xml << "    <param name=\"opcode_width\" value=\"7\"/>\n";
    xml << "    <param name=\"x86\" value=\"0\"/>\n";
    xml << "    <param name=\"micro_opcode_width\" value=\"8\"/>\n";
    xml << "    <param name=\"machine_type\" value=\"0\"/>\n";
    xml << "    <param name=\"number_hardware_threads\" value=\"1\"/>\n";
    xml << "    <param name=\"fetch_width\" value=\"4\"/>\n";
    xml << "    <param name=\"number_instruction_fetch_ports\" value=\"1\"/>\n";
    xml << "    <param name=\"decode_width\" value=\"4\"/>\n";
    xml << "    <param name=\"issue_width\" value=\"4\"/>\n";
    xml << "    <param name=\"commit_width\" value=\"4\"/>\n";
    xml << "    <stat name=\"total_instructions\" value=\"" << stats.total_instructions << "\"/>\n";
    xml << "    <stat name=\"int_instructions\" value=\"" << static_cast<uint64_t>(stats.total_instructions * 0.7) << "\"/>\n";
    xml << "    <stat name=\"fp_instructions\" value=\"" << static_cast<uint64_t>(stats.total_instructions * 0.3) << "\"/>\n";
    xml << "    <stat name=\"load_instructions\" value=\"" << static_cast<uint64_t>(stats.total_instructions * 0.3) << "\"/>\n";
    xml << "    <stat name=\"store_instructions\" value=\"" << static_cast<uint64_t>(stats.total_instructions * 0.15) << "\"/>\n";
    xml << "    <stat name=\"committed_instructions\" value=\"" << stats.total_instructions << "\"/>\n";
    xml << "  </component>\n";
}

void McPATModel::generateCacheXML(std::ofstream& xml, const std::string& level, const ActivityStats& stats) {
    std::string cache_name = level + "Cache";
    uint64_t cache_size = (level == "L1") ? 32768 : (level == "L2") ? 262144 : 2097152;

    xml << "  <component id=\"system." << cache_name << "\" name=\"" << cache_name << "\">\n";
    xml << "    <param name=\"size\" value=\"" << cache_size << "\"/>\n";
    xml << "    <param name=\"block_size\" value=\"64\"/>\n";
    xml << "    <param name=\"associativity\" value=\"8\"/>\n";
    xml << "    <param name=\"banks\" value=\"1\"/>\n";
    uint64_t reads = static_cast<uint64_t>(stats.total_instructions * 0.3);
    uint64_t writes = static_cast<uint64_t>(stats.total_instructions * 0.15);
    xml << "    <stat name=\"read_accesses\" value=\"" << reads << "\"/>\n";
    xml << "    <stat name=\"write_accesses\" value=\"" << writes << "\"/>\n";
    xml << "    <stat name=\"read_misses\" value=\"" << static_cast<uint64_t>(reads * 0.05) << "\"/>\n";
    xml << "    <stat name=\"write_misses\" value=\"" << static_cast<uint64_t>(writes * 0.05) << "\"/>\n";
    xml << "  </component>\n";
}

void McPATModel::generateMemoryControllerXML(std::ofstream& xml, const ActivityStats& stats) {
    xml << "  <component id=\"system.mc\" name=\"mc\">\n";
    xml << "    <param name=\"type\" value=\"0\"/>\n";
    xml << "    <param name=\"mc_clock\" value=\"" << static_cast<int>(tech_params_.frequency_ghz * 1000) << "\"/>\n";
    xml << "    <param name=\"vdd\" value=\"" << 1.2 << "\"/>\n";  // Default DDR voltage
    xml << "    <param name=\"power_gating_vcc\" value=\"-1\"/>\n";
    xml << "    <param name=\"peak_transfer_rate\" value=\"6400\"/>\n";
    uint64_t mem_reads = static_cast<uint64_t>(stats.total_instructions * 0.3);
    uint64_t mem_writes = static_cast<uint64_t>(stats.total_instructions * 0.15);
    xml << "    <stat name=\"memory_accesses\" value=\"" << (mem_reads + mem_writes) << "\"/>\n";
    xml << "    <stat name=\"memory_reads\" value=\"" << mem_reads << "\"/>\n";
    xml << "    <stat name=\"memory_writes\" value=\"" << mem_writes << "\"/>\n";
    xml << "  </component>\n";
}

uint32_t McPATModel::getNumCores(PowerComponent component) const {
    if (component == PowerComponent::CORE || component == PowerComponent::PE) {
        return 1;
    }
    return 0;
}

double McPATModel::calculateComponentEnergy(PowerComponent component, Cycle cycles) {
    auto it = component_metrics_.find(component);
    if (it != component_metrics_.end()) {
        double time_s = cycles / (tech_params_.frequency_ghz * 1e9);
        return it->second.total_power_w * time_s;
    }
    return 0.0;
}

std::string McPATModel::componentToString(PowerComponent component) const {
    switch (component) {
        case PowerComponent::CORE: return "Core";
        case PowerComponent::L1_CACHE: return "L1 Cache";
        case PowerComponent::L2_CACHE: return "L2 Cache";
        case PowerComponent::L3_CACHE: return "L3 Cache";
        case PowerComponent::MEMORY_CONTROLLER: return "Memory Controller";
        case PowerComponent::MEMORY: return "Memory";
        case PowerComponent::NETWORK_ROUTER: return "Network Router";
        case PowerComponent::NETWORK_LINK: return "Network Link";
        case PowerComponent::PE: return "Processing Element";
        default: return "Unknown";
    }
}

std::string McPATModel::generateMcPATConfigXML() const {
    std::ostringstream xml;

    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<component id=\"root\" name=\"root\">\n";

    // System parameters
    xml << "  <param name=\"number_of_cores\" value=\"1\"/>\n";
    xml << "  <param name=\"number_of_L1Directories\" value=\"0\"/>\n";
    xml << "  <param name=\"number_of_L2Directories\" value=\"0\"/>\n";
    xml << "  <param name=\"number_of_L2s\" value=\"1\"/>\n";
    xml << "  <param name=\"number_of_L3s\" value=\"1\"/>\n";
    xml << "  <param name=\"number_of_NoCs\" value=\"1\"/>\n";
    xml << "  <param name=\"homogeneous_cores\" value=\"1\"/>\n";
    xml << "  <param name=\"homogeneous_L2s\" value=\"1\"/>\n";
    xml << "  <param name=\"homogeneous_L1s\" value=\"1\"/>\n";
    xml << "  <param name=\"homogeneous_L3s\" value=\"1\"/>\n";
    xml << "  <param name=\"homogeneous_ccs\" value=\"1\"/>\n";
    xml << "  <param name=\"homogeneous_NoCs\" value=\"1\"/>\n";

    // Technology parameters
    xml << "  <param name=\"core_tech_node\" value=\"" << tech_params_.tech_node_nm << "\"/>\n";
    xml << "  <param name=\"target_core_clockrate\" value=\""
        << static_cast<int>(tech_params_.frequency_ghz * 1000) << "\"/>\n";
    xml << "  <param name=\"temperature\" value=\""
        << static_cast<int>(tech_params_.temperature_k - 273.15) << "\"/>\n";

    // Architecture parameters
    xml << "  <param name=\"number_cache_levels\" value=\"3\"/>\n";
    xml << "  <param name=\"interconnect_projection_type\" value=\"0\"/>\n";

    // Device type: 0=HP, 1=LSTP, 2=LOP
    int device_type_int = 0;
    if (tech_params_.device_type == "LSTP") device_type_int = 1;
    else if (tech_params_.device_type == "LOP") device_type_int = 2;
    xml << "  <param name=\"device_type\" value=\"" << device_type_int << "\"/>\n";

    xml << "  <param name=\"longer_channel_device\" value=\"1\"/>\n";
    xml << "  <param name=\"power_gating\" value=\"0\"/>\n";
    xml << "  <param name=\"machine_bits\" value=\"64\"/>\n";
    xml << "  <param name=\"virtual_address_width\" value=\"64\"/>\n";
    xml << "  <param name=\"physical_address_width\" value=\"48\"/>\n";
    xml << "  <param name=\"virtual_memory_page_size\" value=\"4096\"/>\n";

    // Core configuration
    auto core_it = core_configs_.find(PowerComponent::CORE);
    if (core_it != core_configs_.end()) {
        const auto& cfg = core_it->second;
        xml << "  <component id=\"system.core0\" name=\"core0\">\n";
        xml << "    <param name=\"clock_rate\" value=\""
            << static_cast<int>(tech_params_.frequency_ghz * 1000) << "\"/>\n";
        xml << "    <param name=\"instruction_length\" value=\"32\"/>\n";
        xml << "    <param name=\"opcode_width\" value=\"7\"/>\n";
        xml << "    <param name=\"x86\" value=\"0\"/>\n";
        xml << "    <param name=\"machine_type\" value=\"0\"/>\n";
        xml << "    <param name=\"number_hardware_threads\" value=\"1\"/>\n";
        xml << "    <param name=\"fetch_width\" value=\"" << cfg.issue_width << "\"/>\n";
        xml << "    <param name=\"decode_width\" value=\"" << cfg.issue_width << "\"/>\n";
        xml << "    <param name=\"issue_width\" value=\"" << cfg.issue_width << "\"/>\n";
        xml << "    <param name=\"commit_width\" value=\"" << cfg.issue_width << "\"/>\n";
        xml << "    <param name=\"pipeline_depth\" value=\"" << cfg.pipeline_depth << "\"/>\n";
        xml << "    <param name=\"ALU_per_core\" value=\"" << cfg.alu_count << "\"/>\n";
        xml << "    <param name=\"MUL_per_core\" value=\"" << cfg.mul_count << "\"/>\n";
        xml << "    <param name=\"FPU_per_core\" value=\"" << cfg.fpu_count << "\"/>\n";
        xml << "  </component>\n";
    }

    // L1 Cache configuration
    auto l1_it = cache_configs_.find(PowerComponent::L1_CACHE);
    if (l1_it != cache_configs_.end()) {
        const auto& cfg = l1_it->second;
        xml << "  <component id=\"system.L1i\" name=\"L1i\">\n";
        xml << "    <param name=\"size\" value=\"" << cfg.size_bytes << "\"/>\n";
        xml << "    <param name=\"block_size\" value=\"" << cfg.line_size << "\"/>\n";
        xml << "    <param name=\"associativity\" value=\"" << cfg.associativity << "\"/>\n";
        xml << "    <param name=\"banks\" value=\"" << cfg.banks << "\"/>\n";
        xml << "  </component>\n";

        xml << "  <component id=\"system.L1d\" name=\"L1d\">\n";
        xml << "    <param name=\"size\" value=\"" << cfg.size_bytes << "\"/>\n";
        xml << "    <param name=\"block_size\" value=\"" << cfg.line_size << "\"/>\n";
        xml << "    <param name=\"associativity\" value=\"" << cfg.associativity << "\"/>\n";
        xml << "    <param name=\"banks\" value=\"" << cfg.banks << "\"/>\n";
        xml << "  </component>\n";
    }

    // L2 Cache configuration
    auto l2_it = cache_configs_.find(PowerComponent::L2_CACHE);
    if (l2_it != cache_configs_.end()) {
        const auto& cfg = l2_it->second;
        xml << "  <component id=\"system.L2\" name=\"L2\">\n";
        xml << "    <param name=\"size\" value=\"" << cfg.size_bytes << "\"/>\n";
        xml << "    <param name=\"block_size\" value=\"" << cfg.line_size << "\"/>\n";
        xml << "    <param name=\"associativity\" value=\"" << cfg.associativity << "\"/>\n";
        xml << "    <param name=\"banks\" value=\"" << cfg.banks << "\"/>\n";
        xml << "  </component>\n";
    }

    // L3 Cache configuration
    auto l3_it = cache_configs_.find(PowerComponent::L3_CACHE);
    if (l3_it != cache_configs_.end()) {
        const auto& cfg = l3_it->second;
        xml << "  <component id=\"system.L3\" name=\"L3\">\n";
        xml << "    <param name=\"size\" value=\"" << cfg.size_bytes << "\"/>\n";
        xml << "    <param name=\"block_size\" value=\"" << cfg.line_size << "\"/>\n";
        xml << "    <param name=\"associativity\" value=\"" << cfg.associativity << "\"/>\n";
        xml << "    <param name=\"banks\" value=\"" << cfg.banks << "\"/>\n";
        xml << "  </component>\n";
    }

    xml << "</component>\n";

    return xml.str();
}

} // namespace pimid
