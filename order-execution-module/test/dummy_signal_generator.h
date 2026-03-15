#pragma once
#include "../src/model/signal.h"
#include "../src/util/uuid.h"
#include "../src/util/latency_tracker.h"
#include <utility>
#include <vector>

namespace oem {

// Returns vector of (Signal, delay_ms_before_inject)
using SignalSequence = std::vector<std::pair<Signal, int64_t>>;

class DummySignalGenerator {
public:
    // TC1: Sequential Cross-Symbol Orders
    static SignalSequence sequential_cross_symbol() {
        auto now = now_ns();
        return {
            {make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                         0.1, 100000.0, now), 0},
            {make_signal("ETH-PERP", Side::BUY, InstrumentType::FUTURES,
                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                         1.0, 3000.0, now), 10}
        };
    }

    // TC2: Concurrent Multi-Symbol Orders
    static SignalSequence concurrent_multi_symbol() {
        auto now = now_ns();
        return {
            {make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                         Exchange::HYPERLIQUID, SignalSource::BACKTEST_VALIDATED,
                         0.1, 100000.0, now), 0},
            {make_signal("ETH-PERP", Side::SELL, InstrumentType::FUTURES,
                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                         2.0, 3000.0, now), 0},
            {make_signal("SOL-PERP", Side::BUY, InstrumentType::FUTURES,
                         Exchange::BYBIT, SignalSource::BACKTEST_VALIDATED,
                         10.0, 150.0, now), 0}
        };
    }

    // TC3: Buy Pending → Sell Signal (Conflict)
    static SignalSequence conflict_buy_then_sell() {
        auto now = now_ns();
        return {
            {make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                         0.1, 100000.0, now), 0},
            {make_signal("BTC-PERP", Side::SELL, InstrumentType::FUTURES,
                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                         0.1, 100000.0, now), 50}
        };
    }

    // TC4: Options Order
    static SignalSequence options_order() {
        auto now = now_ns();
        Signal sig = make_signal("BTC", Side::BUY, InstrumentType::OPTIONS,
                                 Exchange::BYBIT, SignalSource::BACKTEST_VALIDATED,
                                 1.0, 5000.0, now);
        sig.strike_price = 100000.0;
        sig.expiry = "20260501";
        sig.option_type = "CALL";
        return {{sig, 0}};
    }

    // TC5: Stale Signal Rejection
    static SignalSequence stale_signal() {
        auto now = now_ns();
        // DIRECT signal 300ms old — should be rejected (threshold 200ms)
        Signal stale_direct = make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                                          Exchange::BINANCE, SignalSource::DIRECT,
                                          0.1, 100000.0,
                                          now - 300 * 1000000LL);

        // BACKTEST signal 3s old — should pass (threshold 5000ms)
        Signal ok_backtest = make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                                         0.1, 100000.0,
                                         now - 3000 * 1000000LL);
        return {
            {stale_direct, 0},
            {ok_backtest, 0}
        };
    }

    // TC6: Insufficient Margin
    static SignalSequence insufficient_margin() {
        auto now = now_ns();
        // qty=10, price=100000, leverage=10 → margin=100000
        // With initial_cash=100000 this would work, but we need to
        // pre-reserve some margin to make it fail.
        // Actually per the spec: "needs 100k margin, only 100k cash but other reservations exist"
        // We'll use qty slightly above what causes it to exceed
        return {
            {make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                         10.0, 100000.0, now), 0}
        };
    }

    // TC7: CEX-DEX Arb — Both Legs Fill
    static SignalSequence arb_success() {
        auto now = now_ns();
        Signal cex = make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                                 Exchange::BINANCE, SignalSource::DIRECT,
                                 0.1, 100000.0, now);
        cex.group_id = "arb-001";
        cex.leg_index = 0;

        Signal dex = make_signal("BTC-PERP", Side::SELL, InstrumentType::FUTURES,
                                 Exchange::HYPERLIQUID, SignalSource::DIRECT,
                                 0.1, 100050.0, now);
        dex.group_id = "arb-001";
        dex.leg_index = 1;

        return {{cex, 0}, {dex, 0}};
    }

    // TC8: CEX-DEX Arb — DEX Leg Fails
    static SignalSequence arb_dex_failure() {
        auto now = now_ns();
        Signal cex = make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                                 Exchange::BINANCE, SignalSource::DIRECT,
                                 0.1, 100000.0, now);
        cex.group_id = "arb-002";
        cex.leg_index = 0;

        Signal dex = make_signal("BTC-PERP", Side::SELL, InstrumentType::FUTURES,
                                 Exchange::HYPERLIQUID, SignalSource::DIRECT,
                                 0.1, 100000.0, now);
        dex.group_id = "arb-002";
        dex.leg_index = 1;

        return {{cex, 0}, {dex, 0}};
    }

    // TC9: CEX-DEX Latency Asymmetry Stress Test
    static SignalSequence arb_latency_asymmetry() {
        auto now = now_ns();
        Signal cex = make_signal("ETH-PERP", Side::BUY, InstrumentType::FUTURES,
                                 Exchange::BYBIT, SignalSource::DIRECT,
                                 1.0, 3000.0, now);
        cex.group_id = "arb-003";
        cex.leg_index = 0;

        Signal dex = make_signal("ETH-PERP", Side::SELL, InstrumentType::FUTURES,
                                 Exchange::HYPERLIQUID, SignalSource::DIRECT,
                                 1.0, 3003.0, now);
        dex.group_id = "arb-003";
        dex.leg_index = 1;

        return {{cex, 0}, {dex, 0}};
    }

private:
    static Signal make_signal(const std::string& symbol, Side side,
                              InstrumentType itype, Exchange exchange,
                              SignalSource source, double qty, double price,
                              int64_t timestamp) {
        Signal sig;
        sig.signal_id = UuidGenerator::signal_id();
        sig.symbol = symbol;
        sig.side = side;
        sig.instrument_type = itype;
        sig.exchange = exchange;
        sig.signal_source = source;
        sig.quantity = qty;
        sig.price = price;
        sig.timestamp_ns = timestamp;
        return sig;
    }
};

} // namespace oem
