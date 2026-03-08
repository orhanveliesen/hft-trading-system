/**
 * Test suite for FundingScheduler
 *
 * Tests funding phase detection and entry/exit logic for futures funding farming.
 */

#include "../include/execution/funding_scheduler.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace hft::execution;

// Helper: Convert minutes to milliseconds
constexpr uint64_t minutes_to_ms(int minutes) {
    return static_cast<uint64_t>(minutes) * 60 * 1000;
}

// Helper: Convert minutes to nanoseconds
constexpr uint64_t minutes_to_ns(int minutes) {
    return static_cast<uint64_t>(minutes) * 60 * 1'000'000'000;
}

void test_normal_phase_far_from_funding() {
    std::cout << "Test: Normal phase (far from funding)..." << std::endl;

    // Next funding in 2 hours
    uint64_t next_funding_ms = 10000000 + minutes_to_ms(120);
    uint64_t current_ns = 10000000ULL * 1'000'000; // Convert to ns

    auto phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::Normal);

    std::cout << "  ✓ 2 hours before funding → Normal" << std::endl;
}

void test_prefunding_14min_before() {
    std::cout << "Test: PreFunding phase (14 min before)..." << std::endl;

    // Next funding in 14 minutes
    uint64_t next_funding_ms = 10000000 + minutes_to_ms(14);
    uint64_t current_ns = 10000000ULL * 1'000'000;

    auto phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::PreFunding);

    std::cout << "  ✓ 14 min before funding → PreFunding" << std::endl;
}

void test_prefunding_1min_before() {
    std::cout << "Test: PreFunding phase (1 min before)..." << std::endl;

    // Next funding in 1 minute
    uint64_t next_funding_ms = 10000000 + minutes_to_ms(1);
    uint64_t current_ns = 10000000ULL * 1'000'000;

    auto phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::PreFunding);

    std::cout << "  ✓ 1 min before funding → PreFunding" << std::endl;
}

void test_postfunding_1min_after() {
    std::cout << "Test: PostFunding phase (1 min after)..." << std::endl;

    // Funding happened 1 minute ago (negative time_to_funding)
    uint64_t next_funding_ms = 10000000 - minutes_to_ms(1); // Past time
    uint64_t current_ns = 10000000ULL * 1'000'000;

    auto phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::PostFunding);

    std::cout << "  ✓ 1 min after funding → PostFunding" << std::endl;
}

void test_postfunding_14min_after() {
    std::cout << "Test: PostFunding phase (14 min after)..." << std::endl;

    // Funding happened 14 minutes ago
    uint64_t next_funding_ms = 10000000 - minutes_to_ms(14);
    uint64_t current_ns = 10000000ULL * 1'000'000;

    auto phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::PostFunding);

    std::cout << "  ✓ 14 min after funding → PostFunding" << std::endl;
}

void test_normal_phase_20min_after() {
    std::cout << "Test: Normal phase (20 min after funding)..." << std::endl;

    // Funding happened 20 minutes ago (outside PostFunding window)
    uint64_t next_funding_ms = 10000000 - minutes_to_ms(20);
    uint64_t current_ns = 10000000ULL * 1'000'000;

    auto phase = FundingScheduler::get_phase(next_funding_ms, current_ns);
    assert(phase == FundingPhase::Normal);

    std::cout << "  ✓ 20 min after funding → Normal" << std::endl;
}

void test_can_exit_farming_postfunding() {
    std::cout << "Test: Can exit farming during PostFunding..." << std::endl;

    uint64_t position_age_ns = minutes_to_ns(10); // Position age (not used yet)
    bool can_exit = FundingScheduler::can_exit_farming(FundingPhase::PostFunding, position_age_ns);
    assert(can_exit == true);

    std::cout << "  ✓ can_exit_farming(PostFunding) → true" << std::endl;
}

void test_cannot_exit_farming_prefunding() {
    std::cout << "Test: Cannot exit farming during PreFunding..." << std::endl;

    uint64_t position_age_ns = minutes_to_ns(10);
    bool can_exit = FundingScheduler::can_exit_farming(FundingPhase::PreFunding, position_age_ns);
    assert(can_exit == false);

    std::cout << "  ✓ can_exit_farming(PreFunding) → false" << std::endl;
}

int main() {
    std::cout << "\n=== FundingScheduler Tests ===" << std::endl;

    test_normal_phase_far_from_funding();
    test_prefunding_14min_before();
    test_prefunding_1min_before();
    test_postfunding_1min_after();
    test_postfunding_14min_after();
    test_normal_phase_20min_after();
    test_can_exit_farming_postfunding();
    test_cannot_exit_farming_prefunding();

    std::cout << "\n✓ All 8 tests passed!" << std::endl;
    return 0;
}
