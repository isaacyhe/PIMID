#include "scheduler/scheduler.h"
#include "common/format_scope.h"  // 1.12.15: scoped stream formats
#include <iostream>
#include <algorithm>
#include <limits>
#include <iomanip>

namespace pimid {

//=============================================================================
// PEScheduler Base Class
//=============================================================================

PEScheduler::PEScheduler(SchedulingPolicy policy, PEPlacementManager* pe_manager)
    : policy_(policy)
    , pe_manager_(pe_manager) {
}

//=============================================================================
// NearestPEScheduler Implementation
//=============================================================================

NearestPEScheduler::NearestPEScheduler(PEPlacementManager* pe_manager)
    : PEScheduler(SchedulingPolicy::NEAREST_PE, pe_manager) {
}

void NearestPEScheduler::submitTask(const PIMTask& task) {
    task_queue_.push(task);
}

uint32_t NearestPEScheduler::scheduleTask(const PIMTask& task) {
    uint32_t pe_id = findNearestPE(task.data_addr);

    // Update statistics
    stats_.total_tasks_scheduled++;
    if (pe_id < 256) {
        stats_.tasks_per_pe[pe_id]++;
    }

    return pe_id;
}

bool NearestPEScheduler::hasPendingTasks() const {
    return !task_queue_.empty();
}

PIMTask NearestPEScheduler::getNextTask() {
    if (task_queue_.empty()) {
        return PIMTask();
    }
    PIMTask task = task_queue_.front();
    task_queue_.pop();
    return task;
}

bool NearestPEScheduler::isPEAvailable(uint32_t pe_id) const {
    auto it = pe_busy_until_.find(pe_id);
    if (it == pe_busy_until_.end()) {
        return true;
    }
    // Note: We don't have current cycle here, so just check if marked busy
    return false;
}

void NearestPEScheduler::markPEBusy(uint32_t pe_id, Cycle until_cycle) {
    pe_busy_until_[pe_id] = until_cycle;
}

void NearestPEScheduler::markPEIdle(uint32_t pe_id) {
    pe_busy_until_.erase(pe_id);
}

NearestPEScheduler::SchedulerStats NearestPEScheduler::getStats() const {
    return stats_;
}

void NearestPEScheduler::resetStats() {
    stats_ = SchedulerStats();
}

void NearestPEScheduler::printStats() const {
    std::cout << "\n=== Nearest PE Scheduler Statistics ===" << std::endl;
    std::cout << "Total Tasks Scheduled: " << stats_.total_tasks_scheduled << std::endl;
    std::cout << "Avg Wait Time: " << stats_.avg_wait_time << " cycles" << std::endl;
    std::cout << "==========================================\n" << std::endl;
}

uint32_t NearestPEScheduler::findNearestPE(Address data_addr) const {
    if (!pe_manager_) {
        // Fallback: simple hash
        return static_cast<uint32_t>(data_addr % 16);
    }

    // Use PE placement manager to find PE responsible for this address
    return pe_manager_->getPEForAddress(data_addr);
}

//=============================================================================
// RoundRobinScheduler Implementation
//=============================================================================

RoundRobinScheduler::RoundRobinScheduler(PEPlacementManager* pe_manager)
    : PEScheduler(SchedulingPolicy::ROUND_ROBIN, pe_manager)
    , next_pe_index_(0) {
}

void RoundRobinScheduler::submitTask(const PIMTask& task) {
    task_queue_.push(task);
}

uint32_t RoundRobinScheduler::scheduleTask(const PIMTask& task) {
    uint32_t pe_id = selectNextPE();

    // Update statistics
    stats_.total_tasks_scheduled++;
    if (pe_id < 256) {
        stats_.tasks_per_pe[pe_id]++;
    }

    return pe_id;
}

bool RoundRobinScheduler::hasPendingTasks() const {
    return !task_queue_.empty();
}

PIMTask RoundRobinScheduler::getNextTask() {
    if (task_queue_.empty()) {
        return PIMTask();
    }
    PIMTask task = task_queue_.front();
    task_queue_.pop();
    return task;
}

bool RoundRobinScheduler::isPEAvailable(uint32_t pe_id) const {
    auto it = pe_busy_until_.find(pe_id);
    return (it == pe_busy_until_.end());
}

void RoundRobinScheduler::markPEBusy(uint32_t pe_id, Cycle until_cycle) {
    pe_busy_until_[pe_id] = until_cycle;
}

void RoundRobinScheduler::markPEIdle(uint32_t pe_id) {
    pe_busy_until_.erase(pe_id);
}

RoundRobinScheduler::SchedulerStats RoundRobinScheduler::getStats() const {
    return stats_;
}

void RoundRobinScheduler::resetStats() {
    stats_ = SchedulerStats();
    next_pe_index_ = 0;
}

void RoundRobinScheduler::printStats() const {
    std::cout << "\n=== Round Robin Scheduler Statistics ===" << std::endl;
    std::cout << "Total Tasks Scheduled: " << stats_.total_tasks_scheduled << std::endl;
    std::cout << "Next PE Index: " << next_pe_index_ << std::endl;
    std::cout << "==========================================\n" << std::endl;
}

uint32_t RoundRobinScheduler::selectNextPE() {
    uint32_t total_pes = pe_manager_ ? pe_manager_->getTotalPEs() : 16;
    uint32_t pe_id = next_pe_index_;
    next_pe_index_ = (next_pe_index_ + 1) % total_pes;
    return pe_id;
}

//=============================================================================
// LoadBalancedScheduler Implementation
//=============================================================================

LoadBalancedScheduler::LoadBalancedScheduler(PEPlacementManager* pe_manager)
    : PEScheduler(SchedulingPolicy::LOAD_BALANCED, pe_manager) {
}

void LoadBalancedScheduler::submitTask(const PIMTask& task) {
    task_queue_.push(task);
}

uint32_t LoadBalancedScheduler::scheduleTask(const PIMTask& task) {
    uint32_t pe_id = selectLeastLoadedPE();

    // Update load tracking
    pe_total_tasks_[pe_id]++;

    // Update statistics
    stats_.total_tasks_scheduled++;
    if (pe_id < 256) {
        stats_.tasks_per_pe[pe_id]++;
    }

    // Calculate load balance factor
    if (stats_.total_tasks_scheduled > 0) {
        uint32_t total_pes = pe_manager_ ? pe_manager_->getTotalPEs() : 16;
        double ideal_tasks_per_pe = static_cast<double>(stats_.total_tasks_scheduled) / total_pes;
        double variance = 0.0;
        for (uint32_t i = 0; i < total_pes && i < 256; i++) {
            double diff = stats_.tasks_per_pe[i] - ideal_tasks_per_pe;
            variance += diff * diff;
        }
        stats_.load_balance_factor = 1.0 - (variance / (stats_.total_tasks_scheduled * stats_.total_tasks_scheduled));
    }

    return pe_id;
}

bool LoadBalancedScheduler::hasPendingTasks() const {
    return !task_queue_.empty();
}

PIMTask LoadBalancedScheduler::getNextTask() {
    if (task_queue_.empty()) {
        return PIMTask();
    }
    PIMTask task = task_queue_.front();
    task_queue_.pop();
    return task;
}

bool LoadBalancedScheduler::isPEAvailable(uint32_t pe_id) const {
    auto it = pe_busy_until_.find(pe_id);
    return (it == pe_busy_until_.end());
}

void LoadBalancedScheduler::markPEBusy(uint32_t pe_id, Cycle until_cycle) {
    pe_busy_until_[pe_id] = until_cycle;
}

void LoadBalancedScheduler::markPEIdle(uint32_t pe_id) {
    pe_busy_until_.erase(pe_id);
}

LoadBalancedScheduler::SchedulerStats LoadBalancedScheduler::getStats() const {
    return stats_;
}

void LoadBalancedScheduler::resetStats() {
    stats_ = SchedulerStats();
    pe_total_tasks_.clear();
}

void LoadBalancedScheduler::printStats() const {
    std::cout << "\n=== Load Balanced Scheduler Statistics ===" << std::endl;
    std::cout << "Total Tasks Scheduled: " << stats_.total_tasks_scheduled << std::endl;
    pimid::StreamFormatGuard fmt_guard_248(std::cout);  // 1.12.15: restores at the end of this block
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "Load Balance Factor: " << stats_.load_balance_factor << std::endl;
    std::cout << "==========================================\n" << std::endl;
}

uint32_t LoadBalancedScheduler::selectLeastLoadedPE() {
    uint32_t total_pes = pe_manager_ ? pe_manager_->getTotalPEs() : 16;
    uint32_t min_pe = 0;
    uint64_t min_tasks = std::numeric_limits<uint64_t>::max();

    for (uint32_t i = 0; i < total_pes; i++) {
        uint64_t tasks = 0;
        auto it = pe_total_tasks_.find(i);
        if (it != pe_total_tasks_.end()) {
            tasks = it->second;
        }

        if (tasks < min_tasks) {
            min_tasks = tasks;
            min_pe = i;
        }
    }

    return min_pe;
}

//=============================================================================
// SchedulerFactory Implementation
//=============================================================================

std::unique_ptr<PEScheduler> SchedulerFactory::createScheduler(
    SchedulingPolicy policy,
    PEPlacementManager* pe_manager) {

    switch (policy) {
        case SchedulingPolicy::NEAREST_PE:
        case SchedulingPolicy::DATA_AWARE:
            return std::make_unique<NearestPEScheduler>(pe_manager);

        case SchedulingPolicy::ROUND_ROBIN:
            return std::make_unique<RoundRobinScheduler>(pe_manager);

        case SchedulingPolicy::LOAD_BALANCED:
        case SchedulingPolicy::PRIORITY_BASED:
            return std::make_unique<LoadBalancedScheduler>(pe_manager);

        default:
            std::cerr << "Unknown scheduling policy, using Round Robin" << std::endl;
            return std::make_unique<RoundRobinScheduler>(pe_manager);
    }
}

} // namespace pimid
