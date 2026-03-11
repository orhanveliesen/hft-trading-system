#include "../include/config/shared_config.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace hft::config;

#define TEST(name) void name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        name();                                                                                                        \
        std::cout << "PASSED\n";                                                                                       \
    } while (0)

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(x) assert(x)
#define ASSERT_FALSE(x) assert(!(x))

constexpr const char* TEST_SHM_NAME = "/trader_config_test";

TEST(config_create_and_defaults) {
    // Create
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);
    ASSERT_TRUE(config != nullptr);

    // Check defaults
    ASSERT_TRUE(config->is_valid());
    ASSERT_EQ(config->magic, SharedConfig::MAGIC);
    ASSERT_EQ(config->version, SharedConfig::VERSION);
    ASSERT_FALSE(config->kill_switch.load());
    ASSERT_TRUE(config->trading_enabled.load());
    ASSERT_EQ(config->max_position.load(), 1000);
    ASSERT_EQ(config->order_size.load(), 100u);

    // Cleanup
    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_open_existing) {
    // Create first
    SharedConfig* owner = SharedConfigManager::create(TEST_SHM_NAME);
    owner->max_position.store(500);

    // Open from "another process"
    SharedConfig* client = SharedConfigManager::open(TEST_SHM_NAME);
    ASSERT_TRUE(client != nullptr);
    ASSERT_EQ(client->max_position.load(), 500);

    // Cleanup
    SharedConfigManager::close(client);
    SharedConfigManager::close(owner);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_atomic_updates) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Atomic update
    config->max_position.store(2000);
    ASSERT_EQ(config->max_position.load(), 2000);

    // Fetch-add
    config->sequence.store(0);
    config->sequence.fetch_add(1);
    ASSERT_EQ(config->sequence.load(), 1u);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_kill_switch) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Initially off
    ASSERT_FALSE(config->kill_switch.load());

    // Activate
    config->kill_switch.store(true);
    ASSERT_TRUE(config->kill_switch.load());

    // Deactivate
    config->kill_switch.store(false);
    ASSERT_FALSE(config->kill_switch.load());

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_cross_thread_visibility) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);
    config->max_position.store(100);

    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    int64_t observed_value = 0;

    // Reader thread
    std::thread reader([&]() {
        SharedConfig* cfg = SharedConfigManager::open(TEST_SHM_NAME);
        ready.store(true);

        // Wait for writer
        while (!done.load()) {
            observed_value = cfg->max_position.load();
            std::this_thread::yield();
        }

        SharedConfigManager::close(cfg);
    });

    // Wait for reader ready
    while (!ready.load()) {
        std::this_thread::yield();
    }

    // Write new value
    config->max_position.store(999);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    done.store(true);

    reader.join();

    // Reader should have seen the new value
    ASSERT_EQ(observed_value, 999);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(scoped_config_raii) {
    // Create with RAII
    {
        ScopedSharedConfig owner(true, TEST_SHM_NAME);
        ASSERT_TRUE(owner);
        owner->max_position.store(777);

        // Open another handle
        ScopedSharedConfig client(false, TEST_SHM_NAME);
        ASSERT_TRUE(client);
        ASSERT_EQ(client->max_position.load(), 777);
    }
    // owner destroyed, should have cleaned up

    // Try to open again - should fail
    SharedConfig* should_be_null = SharedConfigManager::open(TEST_SHM_NAME);
    ASSERT_TRUE(should_be_null == nullptr);
}

TEST(config_readonly_access) {
    SharedConfig* owner = SharedConfigManager::create(TEST_SHM_NAME);
    owner->order_size.store(250);

    const SharedConfig* reader = SharedConfigManager::open_readonly(TEST_SHM_NAME);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_EQ(reader->order_size.load(), 250u);

    // Reader can still read atomic values
    owner->order_size.store(300);
    ASSERT_EQ(reader->order_size.load(), 300u);

    SharedConfigManager::close(reader);
    SharedConfigManager::close(owner);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_size_and_alignment) {
    // Should fit in 2 cache lines
    ASSERT_TRUE(sizeof(SharedConfig) <= 128);

    // Should be cache-line aligned
    ASSERT_EQ(alignof(SharedConfig), 64u);
}

// ============================================================================
// Additional Coverage Tests
// ============================================================================

TEST(config_all_default_values) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Test all default values
    ASSERT_EQ(config->max_daily_loss.load(), 100000);
    ASSERT_EQ(config->threshold_bps.load(), 5u);
    ASSERT_EQ(config->lookback_ticks.load(), 10u);
    ASSERT_EQ(config->cooldown_ms.load(), 0u);
    ASSERT_EQ(config->sequence.load(), 0u);
    ASSERT_EQ(config->last_update_ns.load(), 0u);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_all_atomic_fields) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Test all atomic fields
    config->max_daily_loss.store(50000);
    ASSERT_EQ(config->max_daily_loss.load(), 50000);

    config->threshold_bps.store(10);
    ASSERT_EQ(config->threshold_bps.load(), 10u);

    config->lookback_ticks.store(20);
    ASSERT_EQ(config->lookback_ticks.load(), 20u);

    config->cooldown_ms.store(100);
    ASSERT_EQ(config->cooldown_ms.load(), 100u);

    config->last_update_ns.store(123456789);
    ASSERT_EQ(config->last_update_ns.load(), 123456789u);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_trading_enabled_toggle) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Initially enabled
    ASSERT_TRUE(config->trading_enabled.load());

    // Disable trading
    config->trading_enabled.store(false);
    ASSERT_FALSE(config->trading_enabled.load());

    // Re-enable
    config->trading_enabled.store(true);
    ASSERT_TRUE(config->trading_enabled.load());

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_sequence_increments) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    config->sequence.store(0);
    config->sequence.fetch_add(1);
    ASSERT_EQ(config->sequence.load(), 1u);

    config->sequence.fetch_add(5);
    ASSERT_EQ(config->sequence.load(), 6u);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_open_nonexistent) {
    // Try to open non-existent shared memory
    const char* fake_name = "/nonexistent_shm_config_test";
    SharedConfigManager::destroy(fake_name);

    SharedConfig* config = SharedConfigManager::open(fake_name);
    ASSERT_TRUE(config == nullptr);
}

TEST(config_open_invalid_magic) {
    // Create with valid config
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Corrupt the magic number
    config->magic = 0xDEADBEEF;

    // Close and reopen - should fail validation
    SharedConfigManager::close(config);

    SharedConfig* reopened = SharedConfigManager::open(TEST_SHM_NAME);
    ASSERT_TRUE(reopened == nullptr);

    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_readonly_nonexistent) {
    const char* fake_name = "/nonexistent_readonly_test";
    SharedConfigManager::destroy(fake_name);

    const SharedConfig* config = SharedConfigManager::open_readonly(fake_name);
    ASSERT_TRUE(config == nullptr);
}

TEST(config_readonly_invalid_magic) {
    // Create and corrupt
    SharedConfig* owner = SharedConfigManager::create(TEST_SHM_NAME);
    owner->magic = 0xBADBAD;
    SharedConfigManager::close(owner);

    // Try read-only open - should fail
    const SharedConfig* reader = SharedConfigManager::open_readonly(TEST_SHM_NAME);
    ASSERT_TRUE(reader == nullptr);

    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_close_nullptr) {
    // Closing nullptr should not crash
    SharedConfigManager::close(static_cast<SharedConfig*>(nullptr));
    SharedConfigManager::close(static_cast<const SharedConfig*>(nullptr));

    ASSERT_TRUE(true); // Should reach here
}

TEST(scoped_config_move_constructor) {
    ScopedSharedConfig owner1(true, TEST_SHM_NAME);
    ASSERT_TRUE(owner1);
    owner1->max_position.store(555);

    // Move construct
    ScopedSharedConfig owner2(std::move(owner1));
    ASSERT_TRUE(owner2);
    ASSERT_FALSE(owner1); // owner1 should be null after move

    // owner2 should have the config
    ASSERT_EQ(owner2->max_position.load(), 555);

    // owner2 destructor will clean up
}

TEST(scoped_config_bool_operator) {
    // Create owner
    ScopedSharedConfig owner(true, TEST_SHM_NAME);
    ASSERT_TRUE(owner);

    // Try to open non-existent
    ScopedSharedConfig invalid(false, "/nonexistent_scoped_test");
    ASSERT_FALSE(invalid);

    // owner destructor will clean up
}

TEST(scoped_config_const_get) {
    ScopedSharedConfig owner(true, TEST_SHM_NAME);
    owner->order_size.store(123);

    // Test const get
    const ScopedSharedConfig& const_ref = owner;
    const SharedConfig* cfg = const_ref.get();
    ASSERT_TRUE(cfg != nullptr);
    ASSERT_EQ(cfg->order_size.load(), 123u);

    // Test const operator->
    ASSERT_EQ(const_ref->order_size.load(), 123u);

    // owner destructor will clean up
}

TEST(scoped_config_non_owner) {
    // Create owner first
    SharedConfig* manual = SharedConfigManager::create(TEST_SHM_NAME);
    manual->threshold_bps.store(15);

    // Open as non-owner
    ScopedSharedConfig client(false, TEST_SHM_NAME);
    ASSERT_TRUE(client);
    ASSERT_EQ(client->threshold_bps.load(), 15u);

    // Client destructor should NOT destroy shared memory
    client.~ScopedSharedConfig();

    // Should still be accessible
    SharedConfig* still_there = SharedConfigManager::open(TEST_SHM_NAME);
    ASSERT_TRUE(still_there != nullptr);

    SharedConfigManager::close(manual);
    SharedConfigManager::close(still_there);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_max_position_limits) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Test extreme values
    config->max_position.store(0);
    ASSERT_EQ(config->max_position.load(), 0);

    config->max_position.store(1000000);
    ASSERT_EQ(config->max_position.load(), 1000000);

    config->max_position.store(-500);
    ASSERT_EQ(config->max_position.load(), -500);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_order_size_updates) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    config->order_size.store(1);
    ASSERT_EQ(config->order_size.load(), 1u);

    config->order_size.store(10000);
    ASSERT_EQ(config->order_size.load(), 10000u);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_cooldown_variations) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Test different cooldown values
    config->cooldown_ms.store(0);
    ASSERT_EQ(config->cooldown_ms.load(), 0u);

    config->cooldown_ms.store(50);
    ASSERT_EQ(config->cooldown_ms.load(), 50u);

    config->cooldown_ms.store(1000);
    ASSERT_EQ(config->cooldown_ms.load(), 1000u);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_last_update_timestamp) {
    SharedConfig* config = SharedConfigManager::create(TEST_SHM_NAME);

    // Initially 0
    ASSERT_EQ(config->last_update_ns.load(), 0u);

    // Simulate timestamp update
    uint64_t now_ns = 1234567890123456ULL;
    config->last_update_ns.store(now_ns);
    ASSERT_EQ(config->last_update_ns.load(), now_ns);

    SharedConfigManager::close(config);
    SharedConfigManager::destroy(TEST_SHM_NAME);
}

TEST(config_init_defaults_all_fields) {
    SharedConfig config;

    // Call init_defaults directly
    config.init_defaults();

    // Verify every field
    ASSERT_EQ(config.magic, SharedConfig::MAGIC);
    ASSERT_EQ(config.version, SharedConfig::VERSION);
    ASSERT_FALSE(config.kill_switch.load());
    ASSERT_TRUE(config.trading_enabled.load());
    ASSERT_EQ(config.max_position.load(), 1000);
    ASSERT_EQ(config.order_size.load(), 100u);
    ASSERT_EQ(config.max_daily_loss.load(), 100000);
    ASSERT_EQ(config.threshold_bps.load(), 5u);
    ASSERT_EQ(config.lookback_ticks.load(), 10u);
    ASSERT_EQ(config.cooldown_ms.load(), 0u);
    ASSERT_EQ(config.sequence.load(), 0u);
    ASSERT_EQ(config.last_update_ns.load(), 0u);
}

TEST(config_is_valid_checks) {
    SharedConfig config;
    config.init_defaults();

    // Valid config
    ASSERT_TRUE(config.is_valid());

    // Invalid magic
    config.magic = 0x1234567890ABCDEFULL;
    ASSERT_FALSE(config.is_valid());

    // Restore magic, break version
    config.magic = SharedConfig::MAGIC;
    config.version = 999;
    ASSERT_FALSE(config.is_valid());

    // Restore version
    config.version = SharedConfig::VERSION;
    ASSERT_TRUE(config.is_valid());
}

int main() {
    std::cout << "\n=== Shared Config Tests ===\n\n";

    RUN_TEST(config_create_and_defaults);
    RUN_TEST(config_open_existing);
    RUN_TEST(config_atomic_updates);
    RUN_TEST(config_kill_switch);
    RUN_TEST(config_cross_thread_visibility);
    RUN_TEST(scoped_config_raii);
    RUN_TEST(config_readonly_access);
    RUN_TEST(config_size_and_alignment);

    std::cout << "\nRunning additional coverage tests:\n";

    RUN_TEST(config_all_default_values);
    RUN_TEST(config_all_atomic_fields);
    RUN_TEST(config_trading_enabled_toggle);
    RUN_TEST(config_sequence_increments);
    RUN_TEST(config_open_nonexistent);
    RUN_TEST(config_open_invalid_magic);
    RUN_TEST(config_readonly_nonexistent);
    RUN_TEST(config_readonly_invalid_magic);
    RUN_TEST(config_close_nullptr);
    RUN_TEST(scoped_config_move_constructor);
    RUN_TEST(scoped_config_bool_operator);
    RUN_TEST(scoped_config_const_get);
    RUN_TEST(scoped_config_non_owner);
    RUN_TEST(config_max_position_limits);
    RUN_TEST(config_order_size_updates);
    RUN_TEST(config_cooldown_variations);
    RUN_TEST(config_last_update_timestamp);
    RUN_TEST(config_init_defaults_all_fields);
    RUN_TEST(config_is_valid_checks);

    std::cout << "\n=== All 27 Shared Config Tests Passed! ===\n";
    return 0;
}
