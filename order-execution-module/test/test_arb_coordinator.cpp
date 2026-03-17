#include <gtest/gtest.h>
#include "../src/core/order_manager.h"
#include "../src/gateway/mock_cex_gateway.h"
#include "../src/gateway/mock_dex_gateway.h"
#include "dummy_signal_generator.h"
#include <thread>
#include <chrono>
#include <cmath>

using namespace oem;

class ArbCoordinatorTest : public ::testing::Test {
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

    std::unique_ptr<OrderManager> make_manager() {
        OrderManager::Config config{
            .signal_config = {.backtest_freshness_ms = 5000, .direct_freshness_ms = 200},
            .portfolio_config = {.max_leverage = 10.0, .initial_cash = 100000.0},
            .arb_config = {.assembly_deadline_ms = 100, .unwind_timeout_ms = 3000, .max_risk_window_ms = 2000}
        };
        return std::make_unique<OrderManager>(config, gateways_);
    }

    OrderManager::GatewayMap gateways_;
    std::unique_ptr<MockCexGateway> binance_gw_;
    std::unique_ptr<MockCexGateway> bybit_gw_;
    std::unique_ptr<MockDexGateway> hl_gw_;
};

TEST_F(ArbCoordinatorTest, TC7_ArbBothLegsFill) {
    auto mgr = make_manager();
    auto signals = DummySignalGenerator::arb_success();

    for (auto& [sig, delay_ms] : signals) {
        auto result = mgr->process_signal(sig);
        EXPECT_TRUE(result.success) << "Signal failed: " << result.reason;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    auto pair = mgr->arb_coordinator().get_pair("arb-001");
    EXPECT_EQ(pair.status, ArbPairStatus::COMPLETED)
        << "Expected COMPLETED, got " << to_string(pair.status);

    ASSERT_TRUE(pair.has_cex_leg);
    ASSERT_TRUE(pair.has_dex_leg);
    EXPECT_EQ(pair.cex_leg.status, OrderStatus::FILLED);
    EXPECT_EQ(pair.dex_leg.status, OrderStatus::FILLED);

    double risk_window_ms = pair.risk_window_ns / 1e6;
    EXPECT_GT(risk_window_ms, 500.0) << "Risk window too small: " << risk_window_ms << "ms";
    EXPECT_LT(risk_window_ms, 2000.0) << "Risk window too large: " << risk_window_ms << "ms";

    EXPECT_NEAR(pair.expected_spread_bps, 5.0, 0.1);
    EXPECT_NEAR(pair.realized_spread_bps, 5.0, 0.5);
}

TEST_F(ArbCoordinatorTest, TC8_ArbDexFailure) {
    hl_gw_->set_config(MockDexGateway::Config{
        .block_time_ms = 1000,
        .signing_latency_ms = 5,
        .fill_probability = 0.0,
        .reject_probability = 1.0
    });

    auto mgr = make_manager();
    auto signals = DummySignalGenerator::arb_dex_failure();

    for (auto& [sig, delay_ms] : signals) {
        auto result = mgr->process_signal(sig);
        EXPECT_TRUE(result.success) << "Signal failed: " << result.reason;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    auto pair = mgr->arb_coordinator().get_pair("arb-002");

    EXPECT_TRUE(pair.status == ArbPairStatus::COMPLETED ||
                pair.status == ArbPairStatus::UNWINDING ||
                pair.status == ArbPairStatus::FAILED)
        << "Expected terminal arb status, got " << to_string(pair.status);

    ASSERT_TRUE(pair.has_cex_leg);
    EXPECT_EQ(pair.cex_leg.status, OrderStatus::FILLED);

    ASSERT_TRUE(pair.has_dex_leg);
    EXPECT_EQ(pair.dex_leg.status, OrderStatus::REJECTED);
}

TEST_F(ArbCoordinatorTest, TC9_ArbLatencyAsymmetry) {
    hl_gw_->set_config(MockDexGateway::Config{
        .block_time_ms = 1200,
        .signing_latency_ms = 5,
        .fill_probability = 1.0,
        .reject_probability = 0.0,
        .slippage_bps = 20.0
    });

    auto mgr = make_manager();
    auto signals = DummySignalGenerator::arb_latency_asymmetry();

    for (auto& [sig, delay_ms] : signals) {
        auto result = mgr->process_signal(sig);
        EXPECT_TRUE(result.success) << "Signal failed: " << result.reason;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    auto pair = mgr->arb_coordinator().get_pair("arb-003");
    EXPECT_EQ(pair.status, ArbPairStatus::COMPLETED)
        << "Expected COMPLETED, got " << to_string(pair.status);

    EXPECT_NEAR(pair.expected_spread_bps, 10.0, 0.1);

    EXPECT_LT(pair.realized_spread_bps, pair.expected_spread_bps)
        << "Realized spread should be less than expected due to slippage";

    double risk_window_ms = pair.risk_window_ns / 1e6;
    EXPECT_GT(risk_window_ms, 800.0) << "Risk window: " << risk_window_ms << "ms";
}
