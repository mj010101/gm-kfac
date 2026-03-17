#pragma once
#include "../model/portfolio.h"
#include "../model/signal.h"
#include "../model/order.h"
#include "../util/async_logger.h"
#include "../util/spinlock.h"
#include <cstring>
#include <string>
#include <cmath>

namespace oem {

class PortfolioGuard {
public:
    struct Config {
        double max_leverage = 10.0;
        double initial_cash = 100000.0;
    };

    PortfolioGuard() : config_() { state_.cash = config_.initial_cash; }
    explicit PortfolioGuard(Config config) : config_(config) {
        state_.cash = config_.initial_cash;
    }

    struct MarginResult {
        bool approved = false;
        char reason[128] = {};
        double required_margin = 0.0;

        void set_reason(const std::string& r) {
            std::strncpy(reason, r.c_str(), 127);
            reason[127] = '\0';
        }
    };

    static double reference_price(const char* symbol) {
        if (std::strstr(symbol, "BTC") != nullptr) return 100000.0;
        if (std::strstr(symbol, "ETH") != nullptr) return 3000.0;
        if (std::strstr(symbol, "SOL") != nullptr) return 150.0;
        return 1000.0;
    }

    double calc_required_margin(const Signal& signal) const {
        double price = signal.price;
        if (price <= 0) {
            price = reference_price(signal.symbol);
        }

        if (signal.instrument_type == InstrumentType::OPTIONS) {
            return signal.quantity * price;
        }

        return (signal.quantity * price) / config_.max_leverage;
    }

    MarginResult check_and_reserve(const Signal& signal) {
        SpinLock::Guard lock(mutex_);

        double required = calc_required_margin(signal);
        double available = state_.available_margin();

        if (required > available) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "insufficient_margin: required %.0f, available %.0f",
                         required, available);
            global_logger().warn("portfolio_guard", "%s - %s", signal.signal_id, buf);
            MarginResult r;
            r.set_reason(buf);
            r.required_margin = required;
            return r;
        }

        // Reserve margin
        char key[48];
        std::snprintf(key, sizeof(key), "%s:%s", signal.symbol, signal.signal_id);
        state_.margin_reserved[key] += required;

        global_logger().info("portfolio_guard", "Margin reserved for %s: %.2f (available after: %.2f)",
                             signal.signal_id, required, state_.available_margin());

        MarginResult r;
        r.approved = true;
        r.required_margin = required;
        return r;
    }

    MarginResult check_and_reserve_pair(const Signal& cex_signal, const Signal& dex_signal) {
        SpinLock::Guard lock(mutex_);

        double cex_margin = calc_required_margin(cex_signal);
        double dex_margin = calc_required_margin(dex_signal);
        double total = cex_margin + dex_margin;
        double available = state_.available_margin();

        if (total > available) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                         "insufficient_margin: required %.0f (CEX=%.0f + DEX=%.0f), available %.0f",
                         total, cex_margin, dex_margin, available);
            global_logger().warn("portfolio_guard", "Arb pair %s - %s",
                                 cex_signal.has_group_id ? cex_signal.group_id : "?", buf);
            MarginResult r;
            r.set_reason(buf);
            r.required_margin = total;
            return r;
        }

        char cex_key[48], dex_key[48];
        std::snprintf(cex_key, sizeof(cex_key), "%s:%s", cex_signal.symbol, cex_signal.signal_id);
        std::snprintf(dex_key, sizeof(dex_key), "%s:%s", dex_signal.symbol, dex_signal.signal_id);
        state_.margin_reserved[cex_key] += cex_margin;
        state_.margin_reserved[dex_key] += dex_margin;

        global_logger().info("portfolio_guard", "Arb margin reserved: CEX=%.2f DEX=%.2f (available after: %.2f)",
                             cex_margin, dex_margin, state_.available_margin());

        MarginResult r;
        r.approved = true;
        r.required_margin = total;
        return r;
    }

    void on_fill(const Signal& signal, double fill_price, double fill_qty) {
        SpinLock::Guard lock(mutex_);

        char key[48];
        std::snprintf(key, sizeof(key), "%s:%s", signal.symbol, signal.signal_id);

        auto* reserved_ptr = state_.margin_reserved.find(key);
        double reserved = reserved_ptr ? *reserved_ptr : 0.0;
        state_.margin_reserved.erase(key);

        double used_margin = (fill_qty * fill_price) / config_.max_leverage;
        if (signal.instrument_type == InstrumentType::OPTIONS) {
            used_margin = fill_qty * fill_price;
        }
        state_.margin_used[key] = used_margin;

        double delta = fill_qty * (signal.side == Side::BUY ? 1.0 : -1.0);
        state_.positions[signal.symbol] += delta;

        global_logger().info("portfolio_guard", "Fill applied: %s %s %.4f @ %.2f, margin reserved=%.2f -> used=%.2f",
                             signal.symbol, to_string(signal.side), fill_qty, fill_price,
                             reserved, used_margin);
    }

    void release_margin(const Signal& signal) {
        SpinLock::Guard lock(mutex_);

        char key[48];
        std::snprintf(key, sizeof(key), "%s:%s", signal.symbol, signal.signal_id);
        auto* val = state_.margin_reserved.find(key);
        if (val) {
            global_logger().info("portfolio_guard", "Margin released for %s: %.2f", signal.signal_id, *val);
            state_.margin_reserved.erase(key);
        }
    }

    PortfolioState get_state() const {
        // Note: SpinLock doesn't support shared locking, but critical sections are short
        return state_;
    }

    void reset() {
        SpinLock::Guard lock(mutex_);
        state_.reset(config_.initial_cash);
    }

private:
    Config config_;
    PortfolioState state_;
    mutable SpinLock mutex_;
};

} // namespace oem
