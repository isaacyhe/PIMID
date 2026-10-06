#include "address_translation/address_translator.h"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace pimid {

//=============================================================================
// AddressTranslator Implementation (TLB + Page Table Walker)
//=============================================================================

AddressTranslator::AddressTranslator(const AddressTranslationConfig& config)
    : config_(config) {

    // Calculate page bits if not provided
    if (config_.page_bits == 0) {
        config_.page_bits = static_cast<uint32_t>(std::log2(config_.page_size_bytes));
    }

    // Validate configuration
    if (config_.tlb_entries == 0) {
        std::cerr << "WARNING: TLB entries is 0, setting to default 64" << std::endl;
        config_.tlb_entries = 64;
    }

    if (config_.tlb_associativity == 0 ||
        config_.tlb_associativity > config_.tlb_entries ||
        config_.tlb_entries % config_.tlb_associativity != 0) {
        std::cerr << "WARNING: Invalid TLB associativity, setting to fully associative"
                  << std::endl;
        config_.tlb_associativity = config_.tlb_entries;
    }

    std::cout << "AddressTranslator initialized:" << std::endl;
    std::cout << "  Page size: " << config_.page_size_bytes << " bytes" << std::endl;
    std::cout << "  Page bits: " << config_.page_bits << std::endl;
    std::cout << "  TLB entries: " << config_.tlb_entries << std::endl;
    std::cout << "  TLB associativity: " << config_.tlb_associativity << std::endl;
    std::cout << "  TLB hit latency: " << config_.tlb_hit_latency << " cycles" << std::endl;
    std::cout << "  Page walk latency: " << config_.page_walk_latency << " cycles" << std::endl;
}

Address AddressTranslator::translate(Address virtual_addr, uint32_t pe_id,
                                     bool& hit, Cycle& latency) {
    const bool collect_stats = stats_enabled_.load();
    auto& whole_stats = whole_program_pe_stats_[pe_id];
    whole_stats.total_translations++;
    if (collect_stats) pe_stats_[pe_id].total_translations++;

    // Extract page number and offset
    Address virtual_page = getPageNumber(virtual_addr);
    Address page_offset = getPageOffset(virtual_addr);

    // Try TLB lookup first
    Address physical_page;
    if (lookupTLB(pe_id, virtual_page, physical_page)) {
        // TLB hit!
        whole_stats.tlb_hits++;
        if (collect_stats) pe_stats_[pe_id].tlb_hits++;
        hit = true;
        latency = config_.tlb_hit_latency;
        whole_stats.translation_cycles += latency;
        if (collect_stats) pe_stats_[pe_id].translation_cycles += latency;

        // Reconstruct physical address
        return (physical_page << config_.page_bits) | page_offset;
    }

    // TLB miss - perform page walk
    whole_stats.tlb_misses++;
    if (collect_stats) pe_stats_[pe_id].tlb_misses++;
    hit = false;

    bool found = false;
    performPageWalk(virtual_page, pe_id, physical_page, found);

    if (found) {
        // Page found in page table
        latency = config_.tlb_hit_latency + config_.page_walk_latency;
        whole_stats.translation_cycles += latency;
        if (collect_stats) pe_stats_[pe_id].translation_cycles += latency;

        // Update TLB for future accesses
        updateTLB(pe_id, virtual_page, physical_page);

        // Reconstruct physical address
        return (physical_page << config_.page_bits) | page_offset;
    } else {
        // Page fault - this is a critical error in simulation
        // In a real system, this would trigger page fault handler
        std::cerr << "ERROR: Page fault for virtual address 0x"
                  << std::hex << virtual_addr << std::dec
                  << " (page 0x" << std::hex << virtual_page << std::dec
                  << ") on PE " << pe_id << std::endl;

        // For simulation purposes, create identity mapping
        physical_page = virtual_page;
        mapPage(virtual_page, physical_page, pe_id);
        updateTLB(pe_id, virtual_page, physical_page);

        latency = config_.tlb_hit_latency + config_.page_walk_latency + 100; // +100 for page fault handling
        whole_stats.translation_cycles += latency;
        if (collect_stats) pe_stats_[pe_id].translation_cycles += latency;

        return (physical_page << config_.page_bits) | page_offset;
    }
}

void AddressTranslator::invalidateTLB(uint32_t pe_id) {
    auto it = pe_tlbs_.find(pe_id);
    if (it != pe_tlbs_.end()) {
        for (auto& entry : it->second) {
            entry.valid = false;
        }
    }
}

void AddressTranslator::invalidatePage(Address virtual_page) {
    // Invalidate page in page table
    auto it = page_table_.find(virtual_page);
    if (it != page_table_.end()) {
        it->second.valid = false;
    }

    // Invalidate in all TLBs
    for (auto& pe_tlb_pair : pe_tlbs_) {
        for (auto& entry : pe_tlb_pair.second) {
            if (entry.valid && entry.virtual_page == virtual_page) {
                entry.valid = false;
            }
        }
    }
}

void AddressTranslator::mapPage(Address virtual_page, Address physical_page,
                                uint32_t pe_id) {
    PageTableEntry entry;
    entry.virtual_page = virtual_page;
    entry.physical_page = physical_page;
    entry.valid = true;
    entry.dirty = false;
    entry.referenced = false;
    entry.owner_pe = pe_id;

    page_table_[virtual_page] = entry;
}

void AddressTranslator::unmapPage(Address virtual_page) {
    // Invalidate page
    invalidatePage(virtual_page);

    // Remove from page table
    page_table_.erase(virtual_page);
}

bool AddressTranslator::isPageMapped(Address virtual_page) const {
    auto it = page_table_.find(virtual_page);
    return (it != page_table_.end() && it->second.valid);
}

void AddressTranslator::insertTLB(uint32_t pe_id, Address virtual_page,
                                  Address physical_page) {
    updateTLB(pe_id, virtual_page, physical_page);
}

bool AddressTranslator::lookupTLB(uint32_t pe_id, Address virtual_page,
                                  Address& physical_page) {
    // Ensure TLB exists for this PE
    if (pe_tlbs_.find(pe_id) == pe_tlbs_.end()) {
        pe_tlbs_[pe_id].resize(config_.tlb_entries);
        return false;
    }

    auto& tlb = pe_tlbs_[pe_id];

    // Get TLB set for this virtual page
    uint32_t set_index = getTLBSet(virtual_page);
    uint32_t entries_per_set = config_.tlb_associativity;
    uint32_t set_start = set_index * entries_per_set;

    // Search within the set
    for (uint32_t i = 0; i < entries_per_set && (set_start + i) < tlb.size(); i++) {
        uint32_t idx = set_start + i;
        if (tlb[idx].valid && tlb[idx].virtual_page == virtual_page) {
            // TLB hit!
            physical_page = tlb[idx].physical_page;
            tlb[idx].last_access = ++tlb_clock_;
            return true;
        }
    }

    // TLB miss
    return false;
}

void AddressTranslator::flushTLB(uint32_t pe_id) {
    invalidateTLB(pe_id);
}

AddressTranslator::TranslationStats
AddressTranslator::getStats(uint32_t pe_id) const {
    auto it = pe_stats_.find(pe_id);
    if (it != pe_stats_.end()) {
        return it->second;
    }
    return TranslationStats();
}

void AddressTranslator::printStats() const {
    std::cout << "\n=== Address Translation Statistics (ROI) ===" << std::endl;

    if (pe_stats_.empty() && whole_program_pe_stats_.empty()) {
        std::cout << "No translation statistics available." << std::endl;
        return;
    }

    // Print per-PE statistics
    uint64_t total_translations = 0;
    uint64_t total_tlb_hits = 0;
    uint64_t total_tlb_misses = 0;
    uint64_t total_page_walks = 0;
    uint64_t total_translation_cycles = 0;

    std::cout << "\nPer-PE Statistics:" << std::endl;
    for (const auto& pe_stat_pair : pe_stats_) {
        uint32_t pe_id = pe_stat_pair.first;
        const auto& stats = pe_stat_pair.second;

        total_translations += stats.total_translations;
        total_tlb_hits += stats.tlb_hits;
        total_tlb_misses += stats.tlb_misses;
        total_page_walks += stats.page_walks;
        total_translation_cycles += stats.translation_cycles;

        double hit_rate = 0.0;
        if (stats.total_translations > 0) {
            hit_rate = (stats.tlb_hits * 100.0) / stats.total_translations;
        }

        std::cout << "  PE " << pe_id << ":" << std::endl;
        std::cout << "    Total translations: " << stats.total_translations << std::endl;
        std::cout << "    TLB hits: " << stats.tlb_hits
                  << " (" << hit_rate << "%)" << std::endl;
        std::cout << "    TLB misses: " << stats.tlb_misses << std::endl;
        std::cout << "    Page walks: " << stats.page_walks << std::endl;
        std::cout << "    Translation cycles: " << stats.translation_cycles << std::endl;
    }

    // Print aggregate statistics
    std::cout << "\nAggregate Statistics:" << std::endl;
    std::cout << "  Total translations: " << total_translations << std::endl;
    std::cout << "  Total TLB hits: " << total_tlb_hits << std::endl;
    std::cout << "  Total TLB misses: " << total_tlb_misses << std::endl;
    std::cout << "  Total page walks: " << total_page_walks << std::endl;
    std::cout << "  Total translation cycles: " << total_translation_cycles << std::endl;
    if (total_translations > 0) {
        std::cout << "  Average translation latency: "
                  << (static_cast<double>(total_translation_cycles) / total_translations)
                  << " cycles/translation" << std::endl;
    }

    if (total_translations > 0) {
        double overall_hit_rate = (total_tlb_hits * 100.0) / total_translations;
        std::cout << "  Overall TLB hit rate: " << overall_hit_rate << "%" << std::endl;
    }

    // Whole-program counters are cumulative and survive ROI rebasing.
    std::cout << "\n=== Address Translation Statistics (Whole Program) ===" << std::endl;
    uint64_t whole_translations = 0, whole_hits = 0, whole_misses = 0;
    uint64_t whole_walks = 0, whole_cycles = 0;
    std::cout << "\nPer-PE Statistics:" << std::endl;
    for (const auto& pe_stat_pair : whole_program_pe_stats_) {
        const uint32_t pe_id = pe_stat_pair.first;
        const auto& stats = pe_stat_pair.second;
        whole_translations += stats.total_translations;
        whole_hits += stats.tlb_hits;
        whole_misses += stats.tlb_misses;
        whole_walks += stats.page_walks;
        whole_cycles += stats.translation_cycles;
        const double hit_rate = stats.total_translations
            ? (stats.tlb_hits * 100.0) / stats.total_translations : 0.0;
        std::cout << "  PE " << pe_id << ":" << std::endl;
        std::cout << "    Total translations: " << stats.total_translations << std::endl;
        std::cout << "    TLB hits: " << stats.tlb_hits << " (" << hit_rate << "%)" << std::endl;
        std::cout << "    TLB misses: " << stats.tlb_misses << std::endl;
        std::cout << "    Page walks: " << stats.page_walks << std::endl;
        std::cout << "    Translation cycles: " << stats.translation_cycles << std::endl;
    }
    std::cout << "\nAggregate Statistics:" << std::endl;
    std::cout << "  Total translations: " << whole_translations << std::endl;
    std::cout << "  Total TLB hits: " << whole_hits << std::endl;
    std::cout << "  Total TLB misses: " << whole_misses << std::endl;
    std::cout << "  Total page walks: " << whole_walks << std::endl;
    std::cout << "  Total translation cycles: " << whole_cycles << std::endl;
    if (whole_translations) {
        std::cout << "  Average translation latency: "
                  << static_cast<double>(whole_cycles) / whole_translations
                  << " cycles/translation" << std::endl;
        std::cout << "  Overall TLB hit rate: "
                  << (whole_hits * 100.0) / whole_translations << "%" << std::endl;
    }

    // Print page table statistics
    std::cout << "\nPage Table Statistics:" << std::endl;
    std::cout << "  Total mapped pages: " << page_table_.size() << std::endl;

    // Count valid pages
    uint32_t valid_pages = 0;
    for (const auto& entry_pair : page_table_) {
        if (entry_pair.second.valid) {
            valid_pages++;
        }
    }
    std::cout << "  Valid pages: " << valid_pages << std::endl;

    // Print TLB statistics
    std::cout << "\nTLB Configuration:" << std::endl;
    std::cout << "  TLB entries: " << config_.tlb_entries << std::endl;
    std::cout << "  TLB associativity: " << config_.tlb_associativity << std::endl;
    std::cout << "  Page size: " << config_.page_size_bytes << " bytes" << std::endl;
    std::cout << "  TLB hit latency: " << config_.tlb_hit_latency << " cycles" << std::endl;
    std::cout << "  Page walk latency: " << config_.page_walk_latency << " cycles" << std::endl;
}

void AddressTranslator::resetStats() {
    pe_stats_.clear();
}

//=============================================================================
// Private Helper Functions
//=============================================================================

Address AddressTranslator::getPageNumber(Address addr) const {
    return addr >> config_.page_bits;
}

Address AddressTranslator::getPageOffset(Address addr) const {
    Address page_mask = (1ULL << config_.page_bits) - 1;
    return addr & page_mask;
}

void AddressTranslator::performPageWalk(Address virtual_page, uint32_t pe_id,
                                        Address& physical_page, bool& found) {
    whole_program_pe_stats_[pe_id].page_walks++;
    if (stats_enabled_.load()) pe_stats_[pe_id].page_walks++;

    // Look up in page table
    auto it = page_table_.find(virtual_page);
    if (it != page_table_.end() && it->second.valid) {
        physical_page = it->second.physical_page;

        // Mark page as referenced
        it->second.referenced = true;

        found = true;
    } else {
        found = false;
    }
}

void AddressTranslator::updateTLB(uint32_t pe_id, Address virtual_page,
                                  Address physical_page) {
    // Ensure TLB exists for this PE
    if (pe_tlbs_.find(pe_id) == pe_tlbs_.end()) {
        pe_tlbs_[pe_id].resize(config_.tlb_entries);
    }

    auto& tlb = pe_tlbs_[pe_id];

    // Get TLB set for this virtual page
    uint32_t set_index = getTLBSet(virtual_page);
    uint32_t entries_per_set = config_.tlb_associativity;
    uint32_t set_start = set_index * entries_per_set;

    // Try to find an invalid entry first
    for (uint32_t i = 0; i < entries_per_set && (set_start + i) < tlb.size(); i++) {
        uint32_t idx = set_start + i;
        if (!tlb[idx].valid) {
            // Found invalid entry - use it
            tlb[idx].virtual_page = virtual_page;
            tlb[idx].physical_page = physical_page;
            tlb[idx].valid = true;
            tlb[idx].last_access = ++tlb_clock_;
            return;
        }
    }

    // All entries valid - use LRU replacement
    // Find entry with oldest last_access
    uint32_t lru_idx = set_start;
    Cycle oldest_access = tlb[set_start].last_access;

    for (uint32_t i = 1; i < entries_per_set && (set_start + i) < tlb.size(); i++) {
        uint32_t idx = set_start + i;
        if (tlb[idx].last_access < oldest_access) {
            oldest_access = tlb[idx].last_access;
            lru_idx = idx;
        }
    }

    // Replace LRU entry
    tlb[lru_idx].virtual_page = virtual_page;
    tlb[lru_idx].physical_page = physical_page;
    tlb[lru_idx].valid = true;
    tlb[lru_idx].last_access = ++tlb_clock_;
}

uint32_t AddressTranslator::getTLBSet(Address virtual_page) const {
    // Calculate number of sets
    uint32_t num_sets = config_.tlb_entries / config_.tlb_associativity;

    if (num_sets == 0) {
        return 0;
    }

    // Use lower bits of virtual page number to index into TLB sets
    return static_cast<uint32_t>(virtual_page % num_sets);
}

} // namespace pimid
