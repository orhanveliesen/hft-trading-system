cmake_minimum_required(VERSION 3.16)
project(hft VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Get git commit hash for build versioning
execute_process(
    COMMAND git rev-parse --short=8 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "00000000")
endif()
message(STATUS "Build version: ${GIT_COMMIT_HASH}")
add_compile_definitions(HFT_BUILD_HASH="${GIT_COMMIT_HASH}")

# Compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# === Dependencies ===
# libwebsockets - version 4.3+ has different callback enum values
find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets>=4.0)
message(STATUS "libwebsockets version: ${LWS_VERSION}")

# Define LWS version for compile-time checks
if(LWS_VERSION VERSION_GREATER_EQUAL "4.3")
    add_compile_definitions(LWS_VERSION_4_3_PLUS=1)
    message(STATUS "Using lws 4.3+ callback enum compatibility")
endif()

# === Library ===
add_library(hft_core
    src/orderbook/orderbook.cpp
    src/engine/trading_engine.cpp
    src/engine/matching_engine.cpp
    src/benchmark/timer.cpp
)

# === Tests ===
enable_testing()

add_executable(test_orderbook tests/test_orderbook.cpp)
target_link_libraries(test_orderbook hft_core)
add_test(NAME OrderBookTests COMMAND test_orderbook)

add_executable(test_feed_handler tests/test_feed_handler.cpp)
add_test(NAME FeedHandlerTests COMMAND test_feed_handler)

add_executable(test_network tests/test_network.cpp)
add_test(NAME NetworkTests COMMAND test_network)

add_executable(test_integration tests/test_integration.cpp)
target_link_libraries(test_integration hft_core)
add_test(NAME IntegrationTests COMMAND test_integration)

add_executable(test_strategy tests/test_strategy.cpp)
target_link_libraries(test_strategy hft_core)
add_test(NAME StrategyTests COMMAND test_strategy)

add_executable(test_symbol_config tests/test_symbol_config.cpp)
target_link_libraries(test_symbol_config hft_core)
add_test(NAME SymbolConfigTests COMMAND test_symbol_config)

add_executable(test_matching_engine tests/test_matching_engine.cpp)
target_link_libraries(test_matching_engine hft_core)
add_test(NAME MatchingEngineTests COMMAND test_matching_engine)

add_executable(test_trading_simulator tests/test_trading_simulator.cpp)
target_link_libraries(test_trading_simulator hft_core)
add_test(NAME TradingSimulatorTests COMMAND test_trading_simulator)

add_executable(test_backtester tests/test_backtester.cpp)
target_link_libraries(test_backtester hft_core)
add_test(NAME BacktesterTests COMMAND test_backtester)

add_executable(test_market_data_service tests/test_market_data_service.cpp)
target_link_libraries(test_market_data_service hft_core)
add_test(NAME MarketDataServiceTests COMMAND test_market_data_service)

add_executable(test_top_of_book tests/test_top_of_book.cpp)
add_test(NAME TopOfBookTests COMMAND test_top_of_book)

add_executable(test_simple_mr tests/test_simple_mr.cpp)
target_link_libraries(test_simple_mr hft_core)
add_test(NAME SimpleMRTests COMMAND test_simple_mr)

add_executable(test_overfitting tests/test_overfitting.cpp)
add_test(NAME OverfittingTests COMMAND test_overfitting)

add_executable(test_all_strategies tests/test_all_strategies.cpp)
add_test(NAME AllStrategiesTests COMMAND test_all_strategies)

add_executable(test_shared_config tests/test_shared_config.cpp)
target_link_libraries(test_shared_config pthread rt)
add_test(NAME SharedConfigTests COMMAND test_shared_config)

add_executable(test_account tests/test_account.cpp)
add_test(NAME AccountTests COMMAND test_account)

add_executable(test_market_data tests/test_market_data.cpp)
add_test(NAME MarketDataTests COMMAND test_market_data)

add_executable(test_ouch tests/test_ouch.cpp)
add_test(NAME OuchProtocolTests COMMAND test_ouch)

add_executable(test_paper_trading tests/test_paper_trading.cpp)
target_link_libraries(test_paper_trading pthread)
add_test(NAME PaperTradingTests COMMAND test_paper_trading)

add_executable(test_queue_fill_detector tests/test_queue_fill_detector.cpp)
add_test(NAME QueueFillDetectorTests COMMAND test_queue_fill_detector)

add_executable(test_enhanced_risk tests/test_enhanced_risk.cpp)
add_test(NAME EnhancedRiskTests COMMAND test_enhanced_risk)

add_executable(test_arbitrage tests/test_arbitrage.cpp)
add_test(NAME ArbitrageTests COMMAND test_arbitrage)

add_executable(test_paper_portfolio tests/test_paper_portfolio.cpp)
add_test(NAME PaperPortfolioTests COMMAND test_paper_portfolio)

add_executable(test_udp_telemetry tests/test_udp_telemetry.cpp)
target_link_libraries(test_udp_telemetry pthread)
add_test(NAME UdpTelemetryTests COMMAND test_udp_telemetry)

add_executable(test_rolling_sharpe tests/test_rolling_sharpe.cpp)
add_test(NAME RollingSharpeTests COMMAND test_rolling_sharpe)

# === Tools ===
add_executable(hft tools/hft.cpp)
target_link_libraries(hft hft_core websockets pthread rt)

add_executable(hft_control tools/hft_control.cpp)
target_link_libraries(hft_control pthread rt)

add_executable(hft_observer tools/hft_observer.cpp)
target_link_libraries(hft_observer pthread rt)

add_executable(hft_telemetry_collector tools/hft_telemetry_collector.cpp)
target_link_libraries(hft_telemetry_collector pthread)

add_executable(fetch_klines tools/fetch_klines.cpp)
target_link_libraries(fetch_klines curl)

add_executable(run_backtest tools/run_backtest.cpp)
target_link_libraries(run_backtest hft_core)

add_executable(optimize_strategies tools/optimize_strategies.cpp)
target_link_libraries(optimize_strategies hft_core)

# === Benchmarks ===
add_executable(bench_orderbook benchmarks/bench_orderbook.cpp)
target_link_libraries(bench_orderbook hft_core)

add_executable(bench_lockfree benchmarks/bench_lockfree.cpp)
target_link_libraries(bench_lockfree pthread)

# === ImGui Dashboard ===
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
add_executable(hft_dashboard
    tools/hft_dashboard.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(hft_dashboard PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(hft_dashboard glfw GL pthread rt)
