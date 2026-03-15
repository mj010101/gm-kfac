#pragma once
#include "../model/portfolio.h"
#include "../model/signal.h"
#include "../model/order.h"
#include "../util/logger.h"
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <sstream>
#include <cmath>

namespace oem {

class PortfolioGuard {
public:
    struct Config {
        double max_leverage = 10.0;
        double initial_cash = 100000.0;
    };

    PortfolioGuard() : config_(), logger_(get_logger("portfolio_guard")) { state_.cash = config_.initial_cash; }
    explicit PortfolioGuard(Config config)
        : config_(config)
        , logger_(get_logger("portfolio_guard"))
    {
        state_.cash = config_.initial_cash;
    }

    struct MarginResult {
        bool approved = false;
        std::string reason;
        double required_margin = 0.0;
    };

    // Reference prices for market orders
    static double reference_price(const std::string& symbol) {
        if (symbol.find("BTC") != std::string::npos) return 100000.0;
        if (symbol.find("ETH") != std::string::npos) return 3000.0;
        if (symbol.find("SOL") != std::string::npos) return 150.0;
        return 1000.0;
    }

    double calc_required_margin(const Signal& signal) const {
        double price = signal.price;
        if (price <= 0) {
            price = reference_price(signal.symbol);
        }

        if (signal.instrument_type == InstrumentType::OPTIONS) {
            return signal.quantity * price; // premium
        }

        return (signal.quantity * price) / config_.max_leverage;
    }

    MarginResult check_and_reserve(const Signal& signal) {
        std::unique_lock lock(mutex_);

        double required = calc_required_margin(signal);
        double available = state_.available_margin();

        if (required > available) {
            std::ostringstream oss;
            oss << "insufficient_margin: required " << required
                << ", available " << available;
            logger_->warn("{} - {}", signal.signal_id, oss.str());
            return {false, oss.str(), required};
        }

        // Reserve margin
        std::string key = signal.symbol + ":" + signal.signal_id;
        state_.margin_reserved[key] += required;

        logger_->info("Margin reserved for {}: {:.2f} (available after: {:.2f})",
                     signal.signal_id, required, state_.available_margin());

        return {true, "", required};
    }

    // Atomic reservation for arb pair
    MarginResult check_and_reserve_pair(const Signal& cex_signal, const Signal& dex_signal) {
        std::unique_lock lock(mutex_);

        double cex_margin = calc_required_margin(cex_signal);
        double dex_margin = calc_required_margin(dex_signal);
        double total = cex_margin + dex_margin;
        double available = state_.available_margin();

        if (total > available) {
            std::ostringstream oss;
            oss << "insufficient_margin: required " << total
                << " (CEX=" << cex_margin << " + DEX=" << dex_margin
                << "), available " << available;
            logger_->warn("Arb pair {} - {}", cex_signal.group_id.value_or("?"), oss.str());
            return {false, oss.str(), total};
        }

        // Reserve both atomically
        std::string cex_key = cex_signal.symbol + ":" + cex_signal.signal_id;
        std::string dex_key = dex_signal.symbol + ":" + dex_signal.signal_id;
        state_.margin_reserved[cex_key] += cex_margin;
        state_.margin_reserved[dex_key] += dex_margin;

        logger_->info("Arb margin reserved: CEX={:.2f} DEX={:.2f} (available after: {:.2f})",
                     cex_margin, dex_margin, state_.available_margin());

        return {true, "", total};
    }

    void on_fill(const Signal& signal, double fill_price, double fill_qty) {
        std::unique_lock lock(mutex_);

        std::string key = signal.symbol + ":" + signal.signal_id;

        // Move from reserved to used
        double reserved = state_.margin_reserved[key];
        state_.margin_reserved.erase(key);

        double used_margin = (fill_qty * fill_price) / config_.max_leverage;
        if (signal.instrument_type == InstrumentType::OPTIONS) {
            used_margin = fill_qty * fill_price;
        }
        state_.margin_used[key] = used_margin;

        // Update position
        double delta = fill_qty * (signal.side == Side::BUY ? 1.0 : -1.0);
        state_.positions[signal.symbol] += delta;

        logger_->info("Fill applied: {} {} {:.4f} @ {:.2f}, margin reserved={:.2f} -> used={:.2f}",
                     signal.symbol, to_string(signal.side), fill_qty, fill_price,
                     reserved, used_margin);
    }

    void release_margin(const Signal& signal) {
        std::unique_lock lock(mutex_);

        std::string key = signal.symbol + ":" + signal.signal_id;
        auto it = state_.margin_reserved.find(key);
        if (it != state_.margin_reserved.end()) {
            logger_->info("Margin released for {}: {:.2f}", signal.signal_id, it->second);
            state_.margin_reserved.erase(it);
        }
    }

    PortfolioState get_state() const {
        std::shared_lock lock(mutex_);
        return state_;
    }

    void reset() {
        std::unique_lock lock(mutex_);
        state_.reset(config_.initial_cash);
    }

private:
    Config config_;
    PortfolioState state_;
    mutable std::shared_mutex mutex_;
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace oem
