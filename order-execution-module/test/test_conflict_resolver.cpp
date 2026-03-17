#include <gtest/gtest.h>
#include "../src/core/order_manager.h"
#include "../src/gateway/mock_cex_gateway.h"
#include "../src/gateway/mock_dex_gateway.h"
#include "dummy_signal_generator.h"
#include <thread>
#include <chrono>

using namespace oem;

class ConflictResolverTest : public ::testing::Test {
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
        hl_gw_ = std::make_unique<MockDexGateway>();

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

TEST_F(ConflictResolverTest, TC3_BuyPendingSellSignal) {
    auto mgr = make_manager();

    binance_gw_->set_config(MockCexGateway::Config{
        .exchange_id = Exchange::BINANCE,
        .fill_latency_ms = 200
    });

    auto signals = DummySignalGenerator::conflict_buy_then_sell();

    auto result1 = mgr->process_signal(signals[0].first);
    EXPECT_TRUE(result1.success) << "BUY should succeed: " << result1.reason;

    std::this_thread::sleep_for(std::chrono::milliseconds(signals[1].second));

    auto result2 = mgr->process_signal(signals[1].first);
    EXPECT_TRUE(result2.success) << "SELL should succeed after BUY cancel: " << result2.reason;

    mgr->wait_for_completion(3000);
}

TEST_F(ConflictResolverTest, SameSideNoConflict) {
    auto mgr = make_manager();

    auto now = now_ns();
    Signal buy1;
    buy1.set_signal_id(UuidGenerator::signal_id());
    buy1.set_symbol("ETH-PERP");
    buy1.side = Side::BUY;
    buy1.instrument_type = InstrumentType::FUTURES;
    buy1.exchange = Exchange::BINANCE;
    buy1.signal_source = SignalSource::BACKTEST_VALIDATED;
    buy1.quantity = 1.0;
    buy1.price = 3000.0;
    buy1.timestamp_ns = now;

    Signal buy2;
    buy2.set_signal_id(UuidGenerator::signal_id());
    buy2.set_symbol("ETH-PERP");
    buy2.side = Side::BUY;
    buy2.instrument_type = InstrumentType::FUTURES;
    buy2.exchange = Exchange::BINANCE;
    buy2.signal_source = SignalSource::BACKTEST_VALIDATED;
    buy2.quantity = 0.5;
    buy2.price = 3000.0;
    buy2.timestamp_ns = now;

    auto result1 = mgr->process_signal(buy1);
    EXPECT_TRUE(result1.success);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto result2 = mgr->process_signal(buy2);
    EXPECT_TRUE(result2.success) << "Same-side order should not conflict: " << result2.reason;

    mgr->wait_for_completion(3000);
}

TEST_F(ConflictResolverTest, CancelFailureRejectsNew) {
    ConflictResolver resolver(ConflictStrategy::REJECT_NEW);

    Order existing;
    existing.set_order_id("ORD-000001");
    existing.set_exchange_order_id("EX-ORD-000001");
    existing.signal.set_symbol("BTC-PERP");
    existing.signal.side = Side::BUY;
    existing.signal.exchange = Exchange::BINANCE;
    existing.status = OrderStatus::SENT;

    resolver.register_order(existing);

    Signal sell_signal;
    sell_signal.set_signal_id("SIG-002");
    sell_signal.set_symbol("BTC-PERP");
    sell_signal.side = Side::SELL;
    sell_signal.exchange = Exchange::BINANCE;

    auto result = resolver.check(sell_signal, gateways_);
    EXPECT_FALSE(result.can_proceed);
    EXPECT_TRUE(std::string(result.reason).find("conflict") != std::string::npos);
}
