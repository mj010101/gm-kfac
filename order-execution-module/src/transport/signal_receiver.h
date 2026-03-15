#pragma once
#include "../model/signal.h"
#include "../util/logger.h"
#include "../util/latency_tracker.h"
#include <string>
#include <utility>

namespace oem {

struct ValidationResult {
    bool valid = false;
    std::string reason;
};

class SignalReceiver {
public:
    struct Config {
        int64_t backtest_freshness_ms = 5000;
        int64_t direct_freshness_ms = 200;
    };

    SignalReceiver() : config_(), logger_(get_logger("signal_receiver")) {}
    explicit SignalReceiver(Config config)
        : config_(config)
        , logger_(get_logger("signal_receiver")) {}

    ValidationResult validate(const Signal& signal) {
        auto receive_ns = now_ns();

        // Required fields
        if (signal.signal_id.empty()) {
            return {false, "missing_field: signal_id"};
        }
        if (signal.symbol.empty()) {
            return {false, "missing_field: symbol"};
        }
        if (signal.quantity <= 0) {
            return {false, "invalid_field: quantity must be > 0"};
        }

        // Options validation
        if (signal.instrument_type == InstrumentType::OPTIONS) {
            if (!signal.strike_price.has_value()) {
                return {false, "missing_field: strike_price required for OPTIONS"};
            }
            if (!signal.expiry.has_value()) {
                return {false, "missing_field: expiry required for OPTIONS"};
            }
            if (!signal.option_type.has_value()) {
                return {false, "missing_field: option_type required for OPTIONS"};
            }
        }

        // Freshness check
        int64_t age_ms = (receive_ns - signal.timestamp_ns) / 1000000;
        int64_t threshold_ms = (signal.signal_source == SignalSource::DIRECT)
            ? config_.direct_freshness_ms
            : config_.backtest_freshness_ms;

        if (age_ms > threshold_ms) {
            std::string reason = "stale_signal: age " + std::to_string(age_ms)
                + "ms exceeds " + to_string(signal.signal_source)
                + " threshold " + std::to_string(threshold_ms) + "ms";
            logger_->warn("{} - {}", signal.signal_id, reason);
            return {false, reason};
        }

        logger_->info("Signal {} validated: {} {} {} qty={:.4f} on {}",
                      signal.signal_id, signal.symbol,
                      to_string(signal.side),
                      to_string(signal.instrument_type),
                      signal.quantity,
                      to_string(signal.exchange));

        return {true, ""};
    }

private:
    Config config_;
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace oem
