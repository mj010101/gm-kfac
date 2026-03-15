#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace oem {

enum class Side { BUY, SELL };
enum class InstrumentType { FUTURES, OPTIONS };
enum class Exchange { BINANCE, BYBIT, HYPERLIQUID };
enum class ExchangeType { CEX, DEX };
enum class SignalSource { BACKTEST_VALIDATED, DIRECT };

inline ExchangeType get_exchange_type(Exchange ex) {
    switch (ex) {
        case Exchange::BINANCE:
        case Exchange::BYBIT:
            return ExchangeType::CEX;
        case Exchange::HYPERLIQUID:
            return ExchangeType::DEX;
    }
    return ExchangeType::CEX; // unreachable
}

inline const char* to_string(Side s) {
    return s == Side::BUY ? "BUY" : "SELL";
}

inline const char* to_string(Exchange ex) {
    switch (ex) {
        case Exchange::BINANCE: return "BINANCE";
        case Exchange::BYBIT: return "BYBIT";
        case Exchange::HYPERLIQUID: return "HYPERLIQUID";
    }
    return "UNKNOWN";
}

inline const char* to_string(ExchangeType et) {
    return et == ExchangeType::CEX ? "CEX" : "DEX";
}

inline const char* to_string(SignalSource ss) {
    return ss == SignalSource::BACKTEST_VALIDATED ? "BACKTEST_VALIDATED" : "DIRECT";
}

inline const char* to_string(InstrumentType it) {
    return it == InstrumentType::FUTURES ? "FUTURES" : "OPTIONS";
}

struct Signal {
    std::string     signal_id;
    std::string     symbol;
    Side            side;
    InstrumentType  instrument_type = InstrumentType::FUTURES;
    Exchange        exchange;
    SignalSource    signal_source;
    double          quantity = 0.0;
    double          price = 0.0;        // 0 = market order
    int64_t         timestamp_ns = 0;

    // Arb grouping
    std::optional<std::string> group_id;
    std::optional<int>         leg_index;  // 0 = CEX, 1 = DEX

    // Options
    std::optional<double>      strike_price;
    std::optional<std::string> expiry;
    std::optional<std::string> option_type;
};

} // namespace oem

// Hash for Exchange enum (needed for unordered_map<Exchange, ...>)
namespace std {
template<> struct hash<oem::Exchange> {
    size_t operator()(oem::Exchange e) const noexcept {
        return hash<int>{}(static_cast<int>(e));
    }
};
} // namespace std
