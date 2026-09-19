#ifndef PIMID_MEMORY_MODEL_H
#define PIMID_MEMORY_MODEL_H

#include "common/types.h"
#include <string>

namespace pimid {

/**
 * Abstract base class for memory models
 * Provides standardized interface for different memory technologies
 */
class MemoryModel {
public:
    MemoryModel(MemoryTechnology tech, const std::string& config_path)
        : technology_(tech), config_path_(config_path) {}

    virtual ~MemoryModel() = default;

    // Initialization
    virtual void initialize() = 0;
    virtual void loadConfig(const std::string& config_path) = 0;

    // Memory operations
    virtual Cycle access(const MemoryRequest& req) = 0;
    virtual bool canAccept(const MemoryRequest& req) = 0;
    virtual void tick() = 0;

    /* Energy modeling.
     *
     * 1.11.57 (latent D043): THESE FOUR HAVE NO AGREED UNITS TODAY, and the
     * interface said nothing, so a consumer holding a MemoryModel* could not
     * know what it was being handed. The three bases actually in the tree:
     *   SRAMModel        getReadEnergy() = nJ PER ACCESS (CACTI's
     *                    getDynamicReadEnergy)
     *   DRAMModel        getReadEnergy() = CUMULATIVE nJ over all accesses so
     *                    far (total_reads_ x per-access)
     *   STTMRAM/PCM/ReRAM getReadEnergy() = pJ PER BYTE (the architecture
     *                    object's read_energy_per_byte; their own printStats
     *                    label it " pJ/byte")
     * Three quantities, one signature: per-access energy, an accumulating
     * total, and an energy density differing by the access size and by 1000.
     * The single consumer that flattens them,
     * CompositePowerModel::estimatePower, reads all three as "nJ per access"
     * -- so an NVM device's read energy would be understated by 1000x and a
     * DRAM device's would grow without bound as the run proceeded.
     *
     * WHY NOTHING IS WRONG TODAY: CompositePowerModel is never constructed
     * (nothing calls addMemoryModel()), and no other caller reads these four
     * through the base class. The reported memory energy comes from the
     * RamulatorWrapper/CACTI/NVSim paths directly.
     *
     * THE CONTRACT, stated here so the next implementation has one to follow:
     * getReadEnergy()/getWriteEnergy() are NANOJOULES PER ACCESS at the
     * model's configured access width, getLeakagePower() is WATTS, and
     * getTotalEnergy() is NANOJOULES accumulated since the last resetStats().
     * 1.11.58: THE FOUR IMPLEMENTATIONS NOW OBEY IT. This was the gated
     * change this paragraph asked for. What was wrong: the three NVM models
     * assigned read_energy_/write_energy_ from TWO different units depending
     * on which path ran -- pJ per byte from the architecture object, nJ per
     * access from NVSim -- so the field's meaning depended on whether a
     * characterization was available; and all four added a JOULES leakage
     * term to a picojoule or nanojoule dynamic sum, then returned the result
     * under this header's nanojoule contract. Members are normalised to
     * nJ/access at assignment and getTotalEnergy() returns nanojoules.
     * A polymorphic consumer is now safe.
     *
     * 1.11.60 (audit round 4, C003): THAT LAST SENTENCE WAS PREMATURE, and it
     * is true now. 1.11.58 changed two of the three quantities this paragraph
     * lists -- the NVM density and the leakage unit -- and declared the third,
     * DRAMModel's accumulating total, closed along with them. It was not:
     * DRAMModel::getReadEnergy() still returned the wrapper's cumulative
     * total_reads_ x per-access, copied in on every tick(), and its
     * resetStats() zeroed it, which is accumulating-total behaviour and not
     * per-access behaviour. So the contract paragraph asserted a property of
     * an implementation that did not hold it, which is worse than the original
     * silence: a reader who trusted this text would have had DRAM's read
     * energy grow without bound while the three NVM models returned a
     * constant. DRAMModel now takes the wrapper's INTENSIVE
     * getArrayReadEnergyNJ()/getArrayWriteEnergyNJ() and does its own
     * accumulation inside getTotalEnergy(). All four obey; the claim is
     * checked against each of the four rather than inferred from the release
     * that changed two of them. */
    virtual double getReadEnergy() const = 0;
    virtual double getWriteEnergy() const = 0;
    virtual double getLeakagePower() const = 0;
    virtual double getTotalEnergy() const = 0;

    // Configuration queries
    virtual uint64_t getCapacity() const = 0;
    virtual uint64_t getBandwidth() const = 0;
    virtual Cycle getLatency(MemoryRequestType type) const = 0;

    // Statistics
    virtual void printStats() const = 0;
    virtual void resetStats() = 0;

    //=========================================================================
    // 1.11.24: THE MEMORY PLUGIN CONTRACT -- the uniform tier query.
    //
    // Every model already resolved subarray/bank/chip, but under different
    // names and arities: getSubarrayAccessLatency (DRAM) vs
    // getSubarrayReadLatency (SRAM/NVM), and PCM alone carries SET/RESET.
    // That divergence is why the factory was never wired: main.cpp could not
    // ask a MemoryModel for a placement latency without knowing which
    // technology it was talking to -- which defeats the point of a factory.
    //
    // One query, technology-agnostic. Adding a memory technology becomes
    // "implement this contract and bind your tool"; no main.cpp edit.
    //
    // hasTier() is load-bearing, not decoration: SRAM and NVM are NOT
    // DRAM-like. Bank groups and ranks collapse to 1 for them and no
    // architecture header even declares those fields, so a tier that does not
    // exist must be REPORTABLE AS ABSENT rather than silently collapsed onto
    // its neighbour or invented from a multiplier. That invention is exactly
    // what 1.11.23 found and removed.
    //
    // latencySourceNs() returns the provenance of the number, per tier and
    // per operation. A model that cannot source a tier returns a negative
    // latency and a reason; callers REFUSE rather than substitute. This is
    // the vendorArrayFraction() discipline applied to timing.
    //=========================================================================
    /* 1.11.25: the ARRAY the model must characterize. Without this a model
     * built through the factory uses its own compiled-in default -- STTMRAMModel
     * defaults to 256 MB -- while the live path deliberately characterizes the
     * 64 KB per-bank unit a PE actually owns ("each PE reads/writes its OWN
     * local 64KB bank"). Those differ by 148x in read latency on our own
     * cached characterization, so a tier ladder taken from an unconfigured
     * model describes a different device than the one being simulated.
     * 0 = leave the model's default. */
    virtual void setArrayCapacityBytes(uint64_t /*bytes*/) {}

    /* 1.11.25: and the ACCESS GEOMETRY. Capacity alone was not enough -- the
     * models default to a 64-bit word while the live path characterizes a
     * 512-bit access ("one full 64 B line per access, matching the CACTI/SRAM
     * path"), which selects a DIFFERENT pregenerated cache entry and a
     * different latency. Two knobs, one lesson: a model reached through the
     * factory must be configured identically to the path it replaces, or the
     * ladder describes a device nobody is simulating. 0 = keep the default. */
    virtual void setAccessWidthBits(uint32_t /*bits*/) {}

    /* 1.11.51 (L70, plugin leg): characterize at the RUN's technology node.
     * Each model shipped its own compiled-in default (SRAM/STT 22, ReRAM 32,
     * PCM 90 nm) that nothing ever set -- the per-model config-map knobs had
     * no writer -- so tier timing was priced at a node the energy path did
     * not use. Must precede initialize(). <=0 keeps the model default. */
    virtual void setTechNodeNm(int /*nm*/) {}

    /* 1.11.52 (audit D055): the RUN's operating temperature. Every NVM model
     * hardcoded 350 K, so with power.temperature_k set the cores and caches
     * were priced hot while every memory array stayed at 77 C -- one machine,
     * two temperatures, silently. <=0 keeps the model default. */
    virtual void setTemperatureK(int /*k*/) {}

    enum class Tier { SUBARRAY, BANK, BANKGROUP, CHIP, RANK, CHANNEL };
    enum class Op   { READ, WRITE, SET, RESET };

    /* Latency in ns for one tier/op, or <0 when this model cannot source it.
     * NEVER returns a fabricated value: absence is reported, not filled. */
    virtual double getTierLatencyNs(Tier tier, Op op) const = 0;

    /* Does this technology physically have that tier?
     *
     * 1.11.57 (latent D025): TWO PREDICATES, TWO QUESTIONS -- stated here
     * because a reader was entitled to conclude the implementations
     * contradicted themselves. DRAMModel::hasTier() returns true for
     * BANKGROUP/RANK/CHANNEL while its getTierLatencyNs() returns -1 for all
     * three. That is not a contradiction, it is the difference between
     *
     *   hasTier(t)             -- the DEVICE has that level of hierarchy
     *   getTierLatencyNs(t,op) -- this MODEL can source a number for it
     *
     * and both answers matter: "DRAM has ranks and we cannot price them" is a
     * different report from "SRAM has no ranks". Never read hasTier() as
     * permission to use the latency; always pair it with the latency being
     * >= 0. The one live consumer does exactly that, and prints "has no
     * sourceable <tier> tier" when the pair splits. */
    virtual bool hasTier(Tier tier) const = 0;

    /* 1.11.73: ONE tier below the bank, per family (ruling 2026-09-20):
     *   SRAM -> subbank  (CACTI: the line of mats activated together for one
     *                     data word; its width is the bank's out_w)
     *   NVM  -> mat      (NVSim: numRowMat x numColumnMat per bank; width =
     *                     mat.numDataBit)
     *   DRAM -> subarray (the local-sense-amp row stripe; unchanged)
     * Tier::SUBARRAY stays the enum for L0 -- renaming it is churn with no
     * information -- and tierName(t, tech) prints the family's word. The two
     * organisation queries answer -1 when the model cannot SOURCE the value
     * (e.g. an NVSim cache entry written before these fields existed); the
     * caller reports that and never substitutes a count. */
    virtual int l0UnitsPerBank() const { return -1; }
    virtual int l0WidthBits() const { return -1; }
    /* The L0 link bandwidth in GB/s, or -1 when not sourceable. SRAM: the
     * subbank IS the access, so out_w per CACTI random-cycle time. NVM: the
     * word is formed by the active mats together, so each mat carries
     * mat.numDataBit of every access -- NVSim bank read bandwidth scaled by
     * mat width / word width. */
    virtual double l0BandwidthGBs() const { return -1.0; }
    static const char* l0Name(MemoryTechnology tech) {
        switch (tech) {
            case MemoryTechnology::SRAM:     return "subbank";
            case MemoryTechnology::STT_MRAM:
            case MemoryTechnology::PCM:
            case MemoryTechnology::ReRAM:    return "mat";
            default:                         return "subarray";
        }
    }
    static const char* tierName(Tier t, MemoryTechnology tech) {
        return (t == Tier::SUBARRAY) ? l0Name(tech) : tierName(t);
    }

    /* Where the number came from -- the tool and the quantity, e.g.
     * "NVSim bank->readLatency" or "Ramulator tRCD+tCAS". Empty when the
     * model cannot source it. */
    virtual std::string tierLatencySource(Tier tier, Op op) const = 0;

    static const char* tierName(Tier t) {
        switch (t) {
            case Tier::SUBARRAY:  return "subarray";
            case Tier::BANK:      return "bank";
            case Tier::BANKGROUP: return "bankgroup";
            case Tier::CHIP:      return "chip";
            case Tier::RANK:      return "rank";
            case Tier::CHANNEL:   return "channel";
        }
        return "?";
    }
    static const char* opName(Op o) {
        switch (o) {
            case Op::READ:  return "read";
            case Op::WRITE: return "write";
            case Op::SET:   return "set";
            case Op::RESET: return "reset";
        }
        return "?";
    }

    MemoryTechnology getTechnology() const { return technology_; }

protected:
    MemoryTechnology technology_;
    std::string config_path_;
};

/**
 * Factory for creating memory models based on technology type
 */
class MemoryModelFactory {
public:
    static std::shared_ptr<MemoryModel> createMemoryModel(
        MemoryTechnology tech,
        const std::string& config_path);
};

} // namespace pimid

#endif // PIMID_MEMORY_MODEL_H
