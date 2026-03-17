#pragma once
#include "../src/model/signal.h"
#include "../src/util/uuid.h"
#include "../src/util/latency_tracker.h"
#include <utility>
#include <vector>
#include <string>

namespace oem {

using SignalSequence = std::vector<std::pair<Signal, int64_t>>;

class DummySignalGenerator {
public:
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

    static SignalSequence options_order() {
        auto now = now_ns();
        Signal sig = make_signal("BTC", Side::BUY, InstrumentType::OPTIONS,
                                 Exchange::BYBIT, SignalSource::BACKTEST_VALIDATED,
                                 1.0, 5000.0, now);
        sig.set_strike_price(100000.0);
        sig.set_expiry("20260501");
        sig.set_option_type("CALL");
        return {{sig, 0}};
    }

    static SignalSequence stale_signal() {
        auto now = now_ns();
        Signal stale_direct = make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                                          Exchange::BINANCE, SignalSource::DIRECT,
                                          0.1, 100000.0,
                                          now - 300 * 1000000LL);

        Signal ok_backtest = make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                                         0.1, 100000.0,
                                         now - 3000 * 1000000LL);
        return {
            {stale_direct, 0},
            {ok_backtest, 0}
        };
    }

    static SignalSequence insufficient_margin() {
        auto now = now_ns();
        return {
            {make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                         Exchange::BINANCE, SignalSource::BACKTEST_VALIDATED,
                         10.0, 100000.0, now), 0}
        };
    }

    static SignalSequence arb_success() {
        auto now = now_ns();
        Signal cex = make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                                 Exchange::BINANCE, SignalSource::DIRECT,
                                 0.1, 100000.0, now);
        cex.set_group_id("arb-001");
        cex.leg_index = 0;

        Signal dex = make_signal("BTC-PERP", Side::SELL, InstrumentType::FUTURES,
                                 Exchange::HYPERLIQUID, SignalSource::DIRECT,
                                 0.1, 100050.0, now);
        dex.set_group_id("arb-001");
        dex.leg_index = 1;

        return {{cex, 0}, {dex, 0}};
    }

    static SignalSequence arb_dex_failure() {
        auto now = now_ns();
        Signal cex = make_signal("BTC-PERP", Side::BUY, InstrumentType::FUTURES,
                                 Exchange::BINANCE, SignalSource::DIRECT,
                                 0.1, 100000.0, now);
        cex.set_group_id("arb-002");
        cex.leg_index = 0;

        Signal dex = make_signal("BTC-PERP", Side::SELL, InstrumentType::FUTURES,
                                 Exchange::HYPERLIQUID, SignalSource::DIRECT,
                                 0.1, 100000.0, now);
        dex.set_group_id("arb-002");
        dex.leg_index = 1;

        return {{cex, 0}, {dex, 0}};
    }

    static SignalSequence arb_latency_asymmetry() {
        auto now = now_ns();
        Signal cex = make_signal("ETH-PERP", Side::BUY, InstrumentType::FUTURES,
                                 Exchange::BYBIT, SignalSource::DIRECT,
                                 1.0, 3000.0, now);
        cex.set_group_id("arb-003");
        cex.leg_index = 0;

        Signal dex = make_signal("ETH-PERP", Side::SELL, InstrumentType::FUTURES,
                                 Exchange::HYPERLIQUID, SignalSource::DIRECT,
                                 1.0, 3003.0, now);
        dex.set_group_id("arb-003");
        dex.leg_index = 1;

        return {{cex, 0}, {dex, 0}};
    }

private:
    static Signal make_signal(const char* symbol, Side side,
                              InstrumentType itype, Exchange exchange,
                              SignalSource source, double qty, double price,
                              int64_t timestamp) {
        Signal sig;
        sig.set_signal_id(UuidGenerator::signal_id());
        sig.set_symbol(symbol);
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
