#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include "../include/config/shared_config.hpp"

using namespace hft::config;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    std::cout << "  " << #name << "... "; \
    name(); \
    std::cout << "PASSED\n"; \
} while(0)

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

    std::cout << "\n=== All Shared Config Tests Passed! ===\n";
    return 0;
}
