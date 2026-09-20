#ifndef PIMID_RERAM_MODEL_H
#define PIMID_RERAM_MODEL_H

#include "memory/memory_model.h"
#include "memory/nvsim_wrapper.h"
#include "memory/reram_architecture.h"
#include <queue>
#include <memory>
#include <map>
#include <unordered_map>

namespace pimid {

/**
 * ReRAM (Resistive RAM / Memristor) model using NVSim
 * Provides timing, area, and power modeling with inner-bank timing details
 *
 * UNIQUE: Supports ANALOG COMPUTING in crossbar arrays!
 * Excellent for matrix-vector multiplication in neural networks
 */
class ReRAMModel : public MemoryModel {
public:
    explicit ReRAMModel(const std::string& config_path);
    ~ReRAMModel() override = default;

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
     * initialize() set from a literal ratio of the capacity (capacity / 50) --
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

    // ReRAM-specific queries
    double getArea() const { return area_mm2_; }
    uint64_t getEndurance() const { return endurance_; }

    // Inner-bank timing queries (NEW!)
    double getMatReadLatency() const;
    double getBankReadLatency() const;
    double getChipReadLatency() const;
    double getMatWriteLatency() const;
    double getBankWriteLatency() const;
    double getChipWriteLatency() const;
    double getInnerBankReadLatency() const;
    double getInnerBankWriteLatency() const;

    // ANALOG COMPUTE (unique to ReRAM!)
    double getAnalogComputeLatency() const;
    double getAnalogComputeEnergy() const;
    bool supportsAnalogCompute() const;

    // PIM support queries
    bool supportsBankPIM() const;
    bool supportsMatPIM() const;

private:
    // ReRAM-specific configuration
    struct ReRAMConfig {
        uint64_t capacity;
        uint32_t banks;
        uint32_t read_write_ports;
        uint32_t tech_node_nm;
        /* 1.11.56 (audit D054): NANOSECONDS, and named so. These were `Cycle`,
         * filled from NVSim under a comment reading "assumes 1 GHz: 1 ns = 1
         * cycle" -- but NVSim returns SECONDS and this model has no clock to
         * convert them with (nothing here ever reads a frequency). The 1 GHz
         * was the absence of a clock, not an estimate of one, and initialize()
         * then printed the result as "cycles", wrong by the clock ratio at the
         * sweep's 1-2 GHz PE clocks. Carry the time the tool reported. */
        double read_latency_ns;
        double write_latency_ns;  // Fast writes (5-20 ns)
        Cycle analog_compute_latency;  // Matrix-vector multiply
        uint64_t endurance;
        bool analog_capable;
        bool is_pim_enabled;
    };

    ReRAMConfig reram_config_;

    // NVSim wrapper instance
    std::unique_ptr<NVSimWrapper> nvsim_wrapper_;

    // ReRAM architecture specifications (NEW!)
    std::unique_ptr<memory::ReRAMArchitecture> reram_arch_;

    // Request queue
    std::queue<MemoryRequest> pending_requests_;

    // Statistics
    uint64_t total_reads_;
    uint64_t total_writes_;
    uint64_t total_analog_ops_;  // Analog compute operations
    uint64_t write_cycles_;

    // Energy and area from NVSim
    double read_energy_;
    double write_energy_;
    double analog_compute_energy_;  // Very low for analog ops!
    double leakage_power_;
    double area_mm2_;

    // Current state
    Cycle current_cycle_;
    uint64_t capacity_;
    uint64_t bandwidth_;
    uint64_t endurance_;

    // NVSim initialization (NEW!)
    void initializeNVSim();

    // Endurance tracking
    void updateEndurance(Address addr);
    void reportWearImbalance() const;

    // Per-bank, per-page, and per-cell write counts for wear tracking
    std::map<uint32_t, uint64_t> bank_write_counts_;
    std::unordered_map<uint64_t, uint64_t> page_write_counts_;
    std::unordered_map<uint64_t, uint64_t> cell_write_counts_;

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

#endif // PIMID_RERAM_MODEL_H
