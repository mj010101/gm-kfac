#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
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
    return ExchangeType::CEX;
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

struct alignas(64) Signal {
    char            signal_id[16] = {};
    char            symbol[16] = {};
    Side            side;
    InstrumentType  instrument_type = InstrumentType::FUTURES;
    Exchange        exchange;
    SignalSource    signal_source;
    double          quantity = 0.0;
    double          price = 0.0;
    int64_t         timestamp_ns = 0;

    // Arb grouping
    char            group_id[16] = {};
    bool            has_group_id = false;
    int             leg_index = -1;

    // Options
    double          strike_price = 0.0;
    bool            has_strike_price = false;
    char            expiry[16] = {};
    bool            has_expiry = false;
    char            option_type[8] = {};
    bool            has_option_type = false;

    // Helper methods for compatibility
    void set_signal_id(const std::string& id) {
        std::strncpy(signal_id, id.c_str(), 15);
        signal_id[15] = '\0';
    }
    void set_symbol(const std::string& s) {
        std::strncpy(symbol, s.c_str(), 15);
        symbol[15] = '\0';
    }
    void set_group_id(const std::string& g) {
        std::strncpy(group_id, g.c_str(), 15);
        group_id[15] = '\0';
        has_group_id = true;
    }
    void set_expiry(const std::string& e) {
        std::strncpy(expiry, e.c_str(), 15);
        expiry[15] = '\0';
        has_expiry = true;
    }
    void set_option_type(const std::string& o) {
        std::strncpy(option_type, o.c_str(), 7);
        option_type[7] = '\0';
        has_option_type = true;
    }
    void set_strike_price(double p) {
        strike_price = p;
        has_strike_price = true;
    }

    bool signal_id_empty() const { return signal_id[0] == '\0'; }
    bool symbol_empty() const { return symbol[0] == '\0'; }
};

} // namespace oem

namespace std {
template<> struct hash<oem::Exchange> {
    size_t operator()(oem::Exchange e) const noexcept {
        return hash<int>{}(static_cast<int>(e));
    }
};
} // namespace std
