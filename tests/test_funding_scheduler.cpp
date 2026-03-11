#include "../include/execution/funding_scheduler.hpp"

#include <cassert>
#include <iostream>

using namespace hft::execution;

void test_no_funding_data() {
    // When next_funding_time_ms is 0 (no funding data yet)
    // Should return Normal phase
    // This covers line 58
    uint64_t current_time_ns = 1000000000000ULL; // Some arbitrary time
    FundingPhase phase = FundingScheduler::get_phase(0, current_time_ns);
    assert(phase == FundingPhase::Normal);
}

void test_prefunding_phase() {
    // 10 minutes (600 seconds = 600000 ms) before funding
    uint64_t next_funding_ms = 1000000ULL;
    uint64_t current_time_ms = 1000000ULL - 600000ULL; // 600000 ms before
    uint64_t current_time_ns = current_time_ms * 1'000'000;
    FundingPhase phase = FundingScheduler::get_phase(next_funding_ms, current_time_ns);
    assert(phase == FundingPhase::PreFunding);
}

void test_postfunding_phase() {
    // 10 minutes (600 seconds = 600000 ms) after funding
    uint64_t next_funding_ms = 1000000ULL;
    uint64_t current_time_ms = 1000000ULL + 600000ULL; // 600000 ms after
    uint64_t current_time_ns = current_time_ms * 1'000'000;
    FundingPhase phase = FundingScheduler::get_phase(next_funding_ms, current_time_ns);
    assert(phase == FundingPhase::PostFunding);
}

void test_normal_phase_before() {
    // 30 minutes (1800 seconds = 1800000 ms) before funding (outside 15-minute window)
    uint64_t next_funding_ms = 2000000ULL;              // Larger value to avoid negative time
    uint64_t current_time_ms = 2000000ULL - 1800000ULL; // 1800000 ms before
    uint64_t current_time_ns = current_time_ms * 1'000'000;
    FundingPhase phase = FundingScheduler::get_phase(next_funding_ms, current_time_ns);
    assert(phase == FundingPhase::Normal);
}

void test_normal_phase_after() {
    // 30 minutes (1800 seconds = 1800000 ms) after funding (outside 15-minute window)
    uint64_t next_funding_ms = 1000000ULL;
    uint64_t current_time_ms = 1000000ULL + 1800000ULL; // 1800000 ms after
    uint64_t current_time_ns = current_time_ms * 1'000'000;
    FundingPhase phase = FundingScheduler::get_phase(next_funding_ms, current_time_ns);
    assert(phase == FundingPhase::Normal);
}

void test_can_exit_farming() {
    uint64_t position_age = 1000000ULL;

    // Can exit during PostFunding
    assert(FundingScheduler::can_exit_farming(FundingPhase::PostFunding, position_age) == true);

    // Can exit during Normal
    assert(FundingScheduler::can_exit_farming(FundingPhase::Normal, position_age) == true);

    // Cannot exit during PreFunding
    assert(FundingScheduler::can_exit_farming(FundingPhase::PreFunding, position_age) == false);
}

void test_can_enter_farming() {
    // Can enter during any phase
    assert(FundingScheduler::can_enter_farming(FundingPhase::Normal) == true);
    assert(FundingScheduler::can_enter_farming(FundingPhase::PreFunding) == true);
    assert(FundingScheduler::can_enter_farming(FundingPhase::PostFunding) == true);
}

int main() {
    test_no_funding_data();
    test_prefunding_phase();
    test_postfunding_phase();
    test_normal_phase_before();
    test_normal_phase_after();
    test_can_exit_farming();
    test_can_enter_farming();

    std::cout << "All funding_scheduler tests passed!\n";
    return 0;
}
