#pragma once
#include "../model/signal.h"
#include "../util/async_logger.h"
#include "../util/latency_tracker.h"
#include <string>
#include <cstring>

namespace oem {

struct ValidationResult {
    bool valid = false;
    char reason[128] = {};

    void set_reason(const std::string& r) {
        std::strncpy(reason, r.c_str(), 127);
        reason[127] = '\0';
    }
};

class SignalReceiver {
public:
    struct Config {
        int64_t backtest_freshness_ms = 5000;
        int64_t direct_freshness_ms = 200;
    };

    SignalReceiver() : config_() {}
    explicit SignalReceiver(Config config) : config_(config) {}

    ValidationResult validate(const Signal& signal) {
        auto receive_ns = now_ns();

        if (signal.signal_id_empty()) {
            ValidationResult r;
            r.set_reason("missing_field: signal_id");
            return r;
        }
        if (signal.symbol_empty()) {
            ValidationResult r;
            r.set_reason("missing_field: symbol");
            return r;
        }
        if (signal.quantity <= 0) {
            ValidationResult r;
            r.set_reason("invalid_field: quantity must be > 0");
            return r;
        }

        if (signal.instrument_type == InstrumentType::OPTIONS) {
            if (!signal.has_strike_price) {
                ValidationResult r;
                r.set_reason("missing_field: strike_price required for OPTIONS");
                return r;
            }
            if (!signal.has_expiry) {
                ValidationResult r;
                r.set_reason("missing_field: expiry required for OPTIONS");
                return r;
            }
            if (!signal.has_option_type) {
                ValidationResult r;
                r.set_reason("missing_field: option_type required for OPTIONS");
                return r;
            }
        }

        int64_t age_ms = (receive_ns - signal.timestamp_ns) / 1000000;
        int64_t threshold_ms = (signal.signal_source == SignalSource::DIRECT)
            ? config_.direct_freshness_ms
            : config_.backtest_freshness_ms;

        if (age_ms > threshold_ms) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "stale_signal: age %lldms exceeds %s threshold %lldms",
                         (long long)age_ms, to_string(signal.signal_source), (long long)threshold_ms);
            global_logger().warn("signal_receiver", "%s - %s", signal.signal_id, buf);
            ValidationResult r;
            r.set_reason(buf);
            return r;
        }

        global_logger().info("signal_receiver", "Signal %s validated: %s %s %s qty=%.4f on %s",
                             signal.signal_id, signal.symbol,
                             to_string(signal.side),
                             to_string(signal.instrument_type),
                             signal.quantity,
                             to_string(signal.exchange));

        ValidationResult r;
        r.valid = true;
        return r;
    }

private:
    Config config_;
};

} // namespace oem
