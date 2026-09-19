#ifndef PIMID_PCM_MODEL_H
#define PIMID_PCM_MODEL_H

#include "memory/memory_model.h"
#include "memory/nvsim_wrapper.h"
#include "memory/pcm_architecture.h"
#include <queue>
#include <memory>
#include <map>
#include <unordered_map>

namespace pimid {

/**
 * PCM (Phase-Change Memory) model using NVSim
 * Provides timing, area, and power modeling with inner-bank timing details
 *
 * CRITICAL: PCM has VERY slow writes (50-150ns SET, 10-50ns RESET)
 * Only suitable for read-heavy PIM workloads!
 */
class PCMModel : public MemoryModel {
public:
    explicit PCMModel(const std::string& config_path);
    ~PCMModel() override = default;

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
     * initialize() set from a literal ratio of the capacity (capacity / 200) --
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

    // PCM-specific queries
    double getArea() const { return area_mm2_; }
    uint64_t getEndurance() const { return endurance_; }

    // Inner-bank timing queries (NEW!)
    double getSubarrayReadLatency() const;
    double getBankReadLatency() const;
    double getChipReadLatency() const;
    double getSubarraySetWriteLatency() const;   // Crystallization (SLOW!)
    double getBankSetWriteLatency() const;
    double getChipSetWriteLatency() const;
    double getSubarrayResetWriteLatency() const; // Amorphization (faster)
    double getBankResetWriteLatency() const;
    double getChipResetWriteLatency() const;
    double getInnerBankReadLatency() const;

    // PIM support queries
    bool supportsBankPIM() const;
    bool supportsSubarrayPIM() const;
    bool isReadOnlyPIM() const { return true; }  // Only read-heavy workloads!

private:
    // PCM-specific configuration
    struct PCMConfig {
        uint64_t capacity;
        uint32_t banks;
        uint32_t read_write_ports;
        uint32_t tech_node_nm;
        /* 1.11.56 (audit D054): NANOSECONDS, and named so. These three used to
         * be `Cycle`, filled from NVSim under a comment reading "assumes 1 GHz:
         * 1 ns = 1 cycle" -- but NVSim returns SECONDS and this model has no
         * clock to convert them with (nothing here ever reads a frequency).
         * The 1 GHz was not an approximation of the run's clock, it was the
         * absence of one, and initialize() then printed the result as "cycles".
         * At the sweep's 1-2 GHz PE clocks that label was wrong by the clock
         * ratio. Carry the time the tool actually reported; the legacy
         * Cycle-returning access()/getLatency() convert at one stated point. */
        double read_latency_ns;
        double set_write_latency_ns;    // SET (crystallization)
        double reset_write_latency_ns;  // RESET (amorphization)
        uint64_t endurance;
        bool is_pim_enabled;
    };

    PCMConfig pcm_config_;

    /* 1.11.56 (audit D045): true when NVSim did not resolve
     * FunctionUnit::resetLatency and reset_write_latency_ns therefore HOLDS
     * THE SET PATH. The old code filled the gap with write * 0.3 and printed
     * the product as a RESET measurement; the substitution is now recorded so
     * the printout can name it instead of hiding it. */
    bool reset_latency_is_set_path_ = false;

    // NVSim wrapper instance
    std::unique_ptr<NVSimWrapper> nvsim_wrapper_;

    // PCM architecture specifications (NEW!)
    std::unique_ptr<memory::PCMArchitecture> pcm_arch_;

    // Request queue
    std::queue<MemoryRequest> pending_requests_;

    // Statistics
    uint64_t total_reads_;
    uint64_t total_set_writes_;    // SET operations
    /* 1.11.57 (latent D056): total_reset_writes_ is DELETED. It was
     * initialised, summed into every write total and printed as "Total
     * RESET Writes", and NOTHING ever incremented it -- access() counts
     * every WRITE and ATOMIC as a SET, because MemoryRequestType has no
     * RESET to distinguish. So the counter reported a hard zero under a
     * label that implied a measurement, and the "Total Writes" line it
     * fed was silently SET-only. It cannot be made to work from this
     * interface, so it is removed rather than left as a zero that looks
     * like a count. If RESET traffic ever needs counting, split it at
     * the request type first (Op::RESET already exists on the tier
     * query) and reinstate the counter behind that. */
    uint64_t write_cycles_;

    // Energy and area from NVSim
    double read_energy_;
    /* 1.11.60 (audit round 4, C015): nJ per access, and it is the SET energy,
     * not the SET/RESET average this comment used to claim. Every write is
     * charged as a SET -- the counter above, the latency in access(), and now
     * the energy -- because MemoryRequestType has no RESET to separate them
     * on. See the assignment in initialize(). */
    double write_energy_;
    double leakage_power_;
    double area_mm2_;

    // Current state
    Cycle current_cycle_;
    uint64_t capacity_;
    uint64_t bandwidth_;
    uint64_t endurance_;

    // NVSim initialization (NEW!)
    void initializeNVSim();

    // Endurance tracking with wear-leveling support
    void updateEndurance(Address addr);
    void reportWearImbalance() const;

    // Per-bank, per-page, and per-cell write counts for wear tracking
    // PCM has limited endurance (~10^8), so tracking is critical!
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

#endif // PIMID_PCM_MODEL_H
