#include <gtest/gtest.h>
#include "../src/core/portfolio_guard.h"
#include "../src/util/uuid.h"
#include "../src/util/latency_tracker.h"

using namespace oem;

class PortfolioGuardTest : public ::testing::Test {
protected:
    void SetUp() override {
        UuidGenerator::reset();
        guard_ = std::make_unique<PortfolioGuard>(PortfolioGuard::Config{
            .max_leverage = 10.0,
            .initial_cash = 100000.0
        });
    }

    Signal make_signal(const char* symbol, Side side, double qty, double price) {
        Signal sig;
        sig.set_signal_id(UuidGenerator::signal_id());
        sig.set_symbol(symbol);
        sig.side = side;
        sig.instrument_type = InstrumentType::FUTURES;
        sig.exchange = Exchange::BINANCE;
        sig.signal_source = SignalSource::BACKTEST_VALIDATED;
        sig.quantity = qty;
        sig.price = price;
        sig.timestamp_ns = now_ns();
        return sig;
    }

    std::unique_ptr<PortfolioGuard> guard_;
};

TEST_F(PortfolioGuardTest, ReserveMarginOnSend) {
    auto sig = make_signal("BTC-PERP", Side::BUY, 0.1, 100000.0);

    auto result = guard_->check_and_reserve(sig);
    EXPECT_TRUE(result.approved);
    EXPECT_DOUBLE_EQ(result.required_margin, 1000.0);

    auto state = guard_->get_state();
    EXPECT_DOUBLE_EQ(state.available_margin(), 99000.0);
}

TEST_F(PortfolioGuardTest, ReleaseMarginOnCancel) {
    auto sig = make_signal("BTC-PERP", Side::BUY, 0.1, 100000.0);

    guard_->check_and_reserve(sig);
    EXPECT_DOUBLE_EQ(guard_->get_state().available_margin(), 99000.0);

    guard_->release_margin(sig);
    EXPECT_DOUBLE_EQ(guard_->get_state().available_margin(), 100000.0);
}

TEST_F(PortfolioGuardTest, ReservedToUsedOnFill) {
    auto sig = make_signal("BTC-PERP", Side::BUY, 0.1, 100000.0);

    guard_->check_and_reserve(sig);
    EXPECT_DOUBLE_EQ(guard_->get_state().available_margin(), 99000.0);

    guard_->on_fill(sig, 100000.0, 0.1);

    auto state = guard_->get_state();
    EXPECT_DOUBLE_EQ(state.available_margin(), 99000.0);
    auto* pos = state.positions.find("BTC-PERP");
    ASSERT_NE(pos, nullptr);
    EXPECT_DOUBLE_EQ(*pos, 0.1);
}

TEST_F(PortfolioGuardTest, AtomicArbPairReservation) {
    auto cex_sig = make_signal("BTC-PERP", Side::BUY, 0.1, 100000.0);
    auto dex_sig = make_signal("BTC-PERP", Side::SELL, 0.1, 100050.0);

    auto result = guard_->check_and_reserve_pair(cex_sig, dex_sig);
    EXPECT_TRUE(result.approved);
    EXPECT_NEAR(result.required_margin, 2000.5, 0.01);

    auto state = guard_->get_state();
    EXPECT_NEAR(state.available_margin(), 97999.5, 0.01);
}

TEST_F(PortfolioGuardTest, ArbPairReservationFailsWhenInsufficient) {
    auto big_sig = make_signal("ETH-PERP", Side::BUY, 100.0, 3000.0);
    guard_->check_and_reserve(big_sig);

    auto big_sig2 = make_signal("SOL-PERP", Side::BUY, 4000.0, 150.0);
    guard_->check_and_reserve(big_sig2);

    auto cex_sig = make_signal("BTC-PERP", Side::BUY, 1.0, 100000.0);
    auto dex_sig = make_signal("BTC-PERP", Side::SELL, 1.0, 100050.0);

    auto result = guard_->check_and_reserve_pair(cex_sig, dex_sig);
    EXPECT_FALSE(result.approved);
    EXPECT_TRUE(std::string(result.reason).find("insufficient_margin") != std::string::npos);

    auto state = guard_->get_state();
    EXPECT_DOUBLE_EQ(state.available_margin(), 10000.0);
}

TEST_F(PortfolioGuardTest, TC6_InsufficientMarginExact) {
    auto guard = std::make_unique<PortfolioGuard>(PortfolioGuard::Config{
        .max_leverage = 10.0,
        .initial_cash = 1000.0
    });

    auto sig = make_signal("BTC-PERP", Side::BUY, 10.0, 100000.0);
    auto result = guard->check_and_reserve(sig);
    EXPECT_FALSE(result.approved);
    EXPECT_DOUBLE_EQ(result.required_margin, 100000.0);
    EXPECT_TRUE(std::string(result.reason).find("insufficient_margin") != std::string::npos);
    EXPECT_TRUE(std::string(result.reason).find("100000") != std::string::npos);
    EXPECT_TRUE(std::string(result.reason).find("1000") != std::string::npos);
}
