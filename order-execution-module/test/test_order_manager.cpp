#include <gtest/gtest.h>
#include "../src/core/order_manager.h"
#include "../src/gateway/mock_cex_gateway.h"
#include "../src/gateway/mock_dex_gateway.h"
#include "dummy_signal_generator.h"
#include <thread>
#include <chrono>

using namespace oem;

class OrderManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        UuidGenerator::reset();

        binance_gw_ = std::make_unique<MockCexGateway>(MockCexGateway::Config{
            .exchange_id = Exchange::BINANCE,
            .fill_latency_ms = 50
        });
        bybit_gw_ = std::make_unique<MockCexGateway>(MockCexGateway::Config{
            .exchange_id = Exchange::BYBIT,
            .fill_latency_ms = 50
        });
        hl_gw_ = std::make_unique<MockDexGateway>(MockDexGateway::Config{
            .block_time_ms = 1000,
            .signing_latency_ms = 5
        });

        gateways_[Exchange::BINANCE] = binance_gw_.get();
        gateways_[Exchange::BYBIT] = bybit_gw_.get();
        gateways_[Exchange::HYPERLIQUID] = hl_gw_.get();
    }

    std::unique_ptr<OrderManager> make_manager(double initial_cash = 100000.0) {
        OrderManager::Config config{
            .signal_config = {.backtest_freshness_ms = 5000, .direct_freshness_ms = 200},
            .portfolio_config = {.max_leverage = 10.0, .initial_cash = initial_cash},
            .arb_config = {.assembly_deadline_ms = 100, .unwind_timeout_ms = 3000, .max_risk_window_ms = 2000}
        };
        return std::make_unique<OrderManager>(config, gateways_);
    }

    OrderManager::GatewayMap gateways_;
    std::unique_ptr<MockCexGateway> binance_gw_;
    std::unique_ptr<MockCexGateway> bybit_gw_;
    std::unique_ptr<MockDexGateway> hl_gw_;
};

TEST_F(OrderManagerTest, TC1_SequentialCrossSymbol) {
    auto mgr = make_manager();
    auto signals = DummySignalGenerator::sequential_cross_symbol();

    for (auto& [sig, delay_ms] : signals) {
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        auto result = mgr->process_signal(sig);
        EXPECT_TRUE(result.success) << "Signal " << sig.signal_id << " failed: " << result.reason;
    }

    mgr->wait_for_completion(3000);

    auto orders = mgr->get_all_orders();
    ASSERT_EQ(orders.size(), 2u);

    for (auto& order : orders) {
        EXPECT_EQ(order.status, OrderStatus::FILLED)
            << "Order " << order.order_id << " status: " << to_string(order.status);
    }

    auto state = mgr->portfolio().get_state();
    EXPECT_GT(state.margin_used_size(), 0u);
}

TEST_F(OrderManagerTest, TC2_ConcurrentMultiSymbol) {
    auto mgr = make_manager();
    auto signals = DummySignalGenerator::concurrent_multi_symbol();

    auto start = std::chrono::steady_clock::now();

    for (auto& [sig, delay_ms] : signals) {
        auto result = mgr->process_signal(sig);
        EXPECT_TRUE(result.success) << "Signal " << sig.signal_id << " failed: " << result.reason;
    }

    mgr->wait_for_completion(5000);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto orders = mgr->get_all_orders();
    ASSERT_EQ(orders.size(), 3u);

    for (auto& order : orders) {
        EXPECT_EQ(order.status, OrderStatus::FILLED)
            << "Order " << order.order_id << " status: " << to_string(order.status);
    }

    EXPECT_LT(elapsed, 3000) << "Took too long: " << elapsed << "ms (expected parallel execution)";
}

TEST_F(OrderManagerTest, TC5_StaleSignalRejection) {
    auto mgr = make_manager();
    auto signals = DummySignalGenerator::stale_signal();

    auto result1 = mgr->process_signal(signals[0].first);
    EXPECT_FALSE(result1.success);
    EXPECT_TRUE(std::string(result1.reason).find("stale_signal") != std::string::npos)
        << "Reason: " << result1.reason;
    EXPECT_TRUE(std::string(result1.reason).find("DIRECT") != std::string::npos)
        << "Reason should mention DIRECT: " << result1.reason;

    auto result2 = mgr->process_signal(signals[1].first);
    EXPECT_TRUE(result2.success) << "Backtest signal should pass: " << result2.reason;

    mgr->wait_for_completion(3000);
}

TEST_F(OrderManagerTest, TC6_InsufficientMargin) {
    auto mgr = make_manager(1000.0);
    auto signals = DummySignalGenerator::insufficient_margin();

    auto result = mgr->process_signal(signals[0].first);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(std::string(result.reason).find("insufficient_margin") != std::string::npos)
        << "Reason: " << result.reason;
}
