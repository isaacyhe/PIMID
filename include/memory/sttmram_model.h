#ifndef PIMID_STTMRAM_MODEL_H
#define PIMID_STTMRAM_MODEL_H

#include "memory/memory_model.h"
#include "memory/nvsim_wrapper.h"
#include "memory/sttmram_architecture.h"
#include <queue>
#include <memory>
#include <map>
#include <unordered_map>

namespace pimid {

/**
 * STT-MRAM memory model using NVSim
 * Provides timing, area, and power modeling with inner-bank timing details
 */
class STTMRAMModel : public MemoryModel {
public:
    explicit STTMRAMModel(const std::string& config_path);
    ~STTMRAMModel() override = default;

    // MemoryModel interface implementation
    void initialize() override;
    void loadConfig(const std::string& config_path) override;

    Cycle access(const MemoryRequest& req) override;
    bool canAccept(const MemoryRequest& req) override;
    void tick() override;

    // Energy modeling
    double getReadEnergy() const override { return read_energy_; }
    double getWriteEnergy() const override { return write_energy_; }
    double getLeakagePower() const override { return leakage_power_; }
    double getTotalEnergy() const override;

    // Configuration queries
    uint64_t getCapacity() const override { return capacity_; }
    /* 1.11.57 (latent D053): NOT SOURCED. This returned bandwidth_, which
     * initialize() set from a literal ratio of the capacity (capacity / 100) --
     * a fabricated bytes/s figure behind the same contract method the DRAM
     * model answers from Ramulator. It now returns 0, meaning ABSENT, and
     * says so on stderr the first time it is asked, because a silent 0 in a
     * bytes/s field is as easy to mistake for a measurement as the literal
     * was. Defined out of line so the announcement has somewhere to live. */
    uint64_t getBandwidth() const override;
    Cycle getLatency(MemoryRequestType type) const override;

    // Statistics
    void printStats() const override;
    void resetStats() override;

    // STT-MRAM-specific queries
    double getArea() const { return area_mm2_; }
    uint64_t getEndurance() const { return endurance_; }

    // Inner-bank timing queries (NEW!)
    double getSubarrayReadLatency() const;
    double getBankReadLatency() const;
    double getChipReadLatency() const;
    double getSubarrayWriteLatency() const;
    double getBankWriteLatency() const;
    double getChipWriteLatency() const;
    double getInnerBankReadLatency() const;   // Total read path
    double getInnerBankWriteLatency() const;  // Total write path (MTJ switching!)

    // PIM support queries
    bool supportsBankPIM() const;
    bool supportsSubarrayPIM() const;

private:
    // STT-MRAM-specific configuration
    struct STTMRAMConfig {
        uint64_t capacity;
        uint32_t banks;
        uint32_t read_write_ports;
        uint32_t tech_node_nm;
        /* 1.11.56 (audit D054): NANOSECONDS, and named so. These were `Cycle`,
         * filled as `getReadLatency() * freq_hz` with freq_hz hardcoded to
         * 1e9 under the comment "Assume 1 GHz" -- but NVSim returns SECONDS
         * and this model is never given a clock, so the 1 GHz was the absence
         * of one rather than an estimate of it. initialize() then printed the
         * product as "cycles", which is wrong by the clock ratio at the
         * sweep's 1-2 GHz PE clocks. They also silently truncated the YAML
         * knobs stt_mram.timing.read_latency_ns / write_latency_ns, which were
         * always nanoseconds, to integers. Carry time. */
        double read_latency_ns;
        double write_latency_ns;
        uint64_t endurance;
        bool is_pim_enabled;
    };

    STTMRAMConfig mram_config_;

    // NVSim wrapper instance
    std::unique_ptr<NVSimWrapper> nvsim_wrapper_;

    // STT-MRAM architecture specifications (NEW!)
    std::unique_ptr<memory::STTMRAMArchitecture> mram_arch_;

    // Request queue
    std::queue<MemoryRequest> pending_requests_;

    // Statistics
    uint64_t total_reads_;
    uint64_t total_writes_;
    uint64_t write_cycles_;  // Track endurance

    // Energy and area from NVSim
    double read_energy_;
    double write_energy_;
    double leakage_power_;
    double area_mm2_;

    // Current state
    Cycle current_cycle_;
    uint64_t capacity_;
    uint64_t bandwidth_;
    uint64_t endurance_;

    // Endurance tracking
    void updateEndurance(Address addr);
    void initializeNVSim();

    // Per-bank and per-page write counts for endurance tracking
    std::map<uint32_t, uint64_t> bank_write_counts_;
    std::unordered_map<uint64_t, uint64_t> page_write_counts_;

    //=== 1.11.24: the memory plugin contract (see MemoryModel) ===============
    double getTierLatencyNs(Tier tier, Op op) const override;
    bool hasTier(Tier tier) const override;
    int l0UnitsPerBank() const override;   // 1.11.73
    int l0WidthBits() const override;      // 1.11.73
    double l0BandwidthGBs() const override; // 1.11.73
    std::string tierLatencySource(Tier tier, Op op) const override;
    void setArrayCapacityBytes(uint64_t bytes) override;
    void setAccessWidthBits(uint32_t bits) override;
    void setTechNodeNm(int nm) override;   // 1.11.51 (L70)
    void setTemperatureK(int k) override;  // 1.11.52 (D055)
    int temperature_k_ = 350;             // 1.11.52 (D055): run temperature
    uint32_t access_width_bits_ = 0;   // 0 = model default
};

} // namespace pimid

#endif // PIMID_STTMRAM_MODEL_H
