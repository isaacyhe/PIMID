#ifndef PIMID_ADDRESS_TRANSLATOR_H
#define PIMID_ADDRESS_TRANSLATOR_H

#include "common/types.h"
#include <map>
#include <vector>
#include <atomic>

namespace pimid {

/**
 * Page table entry
 */
struct PageTableEntry {
    Address virtual_page;
    Address physical_page;
    bool valid;
    bool dirty;
    bool referenced;
    uint32_t owner_pe;  // Which PE owns this page

    PageTableEntry() : virtual_page(0), physical_page(0),
                       valid(false), dirty(false), referenced(false),
                       owner_pe(0) {}
};

/**
 * TLB entry
 */
struct TLBEntry {
    Address virtual_page;
    Address physical_page;
    bool valid;
    Cycle last_access;

    TLBEntry() : virtual_page(0), physical_page(0),
                 valid(false), last_access(0) {}
};

/**
 * Address translation configuration
 */
struct AddressTranslationConfig {
    uint32_t page_size_bytes;
    uint32_t page_bits;
    uint32_t tlb_entries;
    uint32_t tlb_associativity;
    Cycle tlb_hit_latency;
    Cycle page_walk_latency;

    AddressTranslationConfig() : page_size_bytes(4096), page_bits(12),
                                 tlb_entries(64), tlb_associativity(4),
                                 tlb_hit_latency(1), page_walk_latency(20) {}
};

/**
 * Address translator with TLB and page table walker
 * Supports virtual memory for PIM systems
 */
class AddressTranslator {
public:
    AddressTranslator(const AddressTranslationConfig& config);

    // Translation
    Address translate(Address virtual_addr, uint32_t pe_id, bool& hit, Cycle& latency);
    void invalidateTLB(uint32_t pe_id);
    void invalidatePage(Address virtual_page);

    // Page table management
    void mapPage(Address virtual_page, Address physical_page, uint32_t pe_id);
    void unmapPage(Address virtual_page);
    bool isPageMapped(Address virtual_page) const;

    // TLB management
    void insertTLB(uint32_t pe_id, Address virtual_page, Address physical_page);
    bool lookupTLB(uint32_t pe_id, Address virtual_page, Address& physical_page);
    void flushTLB(uint32_t pe_id);

    // Statistics
    struct TranslationStats {
        uint64_t total_translations;
        uint64_t tlb_hits;
        uint64_t tlb_misses;
        uint64_t page_walks;
        uint64_t translation_cycles;

        TranslationStats() : total_translations(0), tlb_hits(0),
                             tlb_misses(0), page_walks(0), translation_cycles(0) {}
    };

    TranslationStats getStats(uint32_t pe_id) const;
    void printStats() const;
    void resetStats();
    void setStatsEnabled(bool enabled) { stats_enabled_.store(enabled); }

private:
    AddressTranslationConfig config_;
    Cycle tlb_clock_ = 0;
    std::atomic<bool> stats_enabled_{true};

    // Page table (shared across all PEs in unified mode)
    std::map<Address, PageTableEntry> page_table_;

    // Per-PE TLBs
    std::map<uint32_t, std::vector<TLBEntry>> pe_tlbs_;

    // Per-PE statistics
    std::map<uint32_t, TranslationStats> pe_stats_;
    // Cumulative counters are never reset at ROI boundaries.
    std::map<uint32_t, TranslationStats> whole_program_pe_stats_;

    // Helper functions
    Address getPageNumber(Address addr) const;
    Address getPageOffset(Address addr) const;
    void performPageWalk(Address virtual_page, uint32_t pe_id,
                         Address& physical_page, bool& found);
    void updateTLB(uint32_t pe_id, Address virtual_page, Address physical_page);
    uint32_t getTLBSet(Address virtual_page) const;
};

} // namespace pimid

#endif // PIMID_ADDRESS_TRANSLATOR_H
