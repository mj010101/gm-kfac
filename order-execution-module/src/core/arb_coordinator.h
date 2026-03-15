#pragma once
#include "../model/arb_pair.h"
#include "../model/order.h"
#include "../gateway/i_exchange_gateway.h"
#include "portfolio_guard.h"
#include "../util/logger.h"
#include "../util/uuid.h"
#include "../util/latency_tracker.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace oem {

class ArbCoordinator {
public:
    struct Config {
        int64_t assembly_deadline_ms = 100;
        int64_t unwind_timeout_ms = 3000;
        int64_t max_risk_window_ms = 2000;
    };

    using GatewayMap = std::unordered_map<Exchange, IExchangeGateway*>;
    using OrderUpdateCallback = std::function<void(const std::string& order_id,
                                                    OrderStatus status,
                                                    double fill_price,
                                                    double fill_qty)>;

    ArbCoordinator(Config config, PortfolioGuard& portfolio, GatewayMap& gateways)
        : config_(config)
        , portfolio_(portfolio)
        , gateways_(gateways)
        , logger_(get_logger("arb_coordinator")) {}

    ~ArbCoordinator() {
        std::lock_guard lock(threads_mutex_);
        for (auto& t : worker_threads_) {
            if (t.joinable()) t.join();
        }
    }

    void set_order_update_callback(OrderUpdateCallback cb) {
        order_update_cb_ = cb;
    }

    // Returns the order created for the signal
    Order add_leg(const Signal& signal) {
        std::unique_lock lock(mutex_);

        auto group_id = signal.group_id.value();
        auto exch_type = get_exchange_type(signal.exchange);

        // Create order
        Order order;
        order.order_id = UuidGenerator::order_id();
        order.signal = signal;
        order.status = OrderStatus::CREATED;
        order.created_at_ns = now_ns();

        // Find or create ArbPair
        auto it = pairs_.find(group_id);
        if (it == pairs_.end()) {
            ArbPair pair;
            pair.group_id = group_id;
            pair.created_at_ns = now_ns();
            pair.dispatch_deadline_ns = now_ns() + config_.assembly_deadline_ms * 1000000LL;
            pairs_[group_id] = pair;
            it = pairs_.find(group_id);

            logger_->info("ArbPair {} created, deadline in {}ms",
                         group_id, config_.assembly_deadline_ms);

            // Start deadline timer
            start_deadline_timer(group_id);
        }

        auto& pair = it->second;

        if (exch_type == ExchangeType::CEX) {
            pair.cex_leg = order;
        } else {
            pair.dex_leg = order;
        }

        logger_->info("ArbPair {} leg added: {} {} on {} ({})",
                     group_id, to_string(signal.side), signal.symbol,
                     to_string(signal.exchange), to_string(exch_type));

        // Check if both legs present
        if (pair.both_legs_present()) {
            lock.unlock();
            dispatch_pair(group_id);
        }

        return order;
    }

    // Called by fill callbacks
    void on_fill(const std::string& exchange_order_id, double fill_price,
                 double fill_qty, bool is_reject) {
        std::unique_lock lock(mutex_);

        // Find which pair this fill belongs to
        for (auto& [group_id, pair] : pairs_) {
            bool is_cex = pair.cex_leg && ("EX-" + pair.cex_leg->order_id) == exchange_order_id;
            bool is_dex = pair.dex_leg && ("HL-" + pair.dex_leg->order_id) == exchange_order_id;

            if (!is_cex && !is_dex) continue;

            if (is_reject) {
                handle_leg_reject(pair, is_cex, lock);
                return;
            }

            if (is_cex) {
                pair.cex_leg->status = OrderStatus::FILLED;
                pair.cex_leg->filled_quantity = fill_qty;
                pair.cex_leg->avg_fill_price = fill_price;
                pair.cex_fill_ns = now_ns();

                if (order_update_cb_) {
                    order_update_cb_(pair.cex_leg->order_id, OrderStatus::FILLED,
                                    fill_price, fill_qty);
                }

                if (pair.status == ArbPairStatus::DISPATCHED) {
                    pair.status = ArbPairStatus::CEX_FILLED;
                    logger_->info("ArbPair {}: CEX leg FILLED at {:.2f}", group_id, fill_price);
                } else if (pair.status == ArbPairStatus::DEX_FILLED) {
                    pair.status = ArbPairStatus::ALL_FILLED;
                    finalize_pair(pair);
                }
            } else { // is_dex
                pair.dex_leg->status = OrderStatus::FILLED;
                pair.dex_leg->filled_quantity = fill_qty;
                pair.dex_leg->avg_fill_price = fill_price;
                pair.dex_confirm_ns = now_ns();

                if (order_update_cb_) {
                    order_update_cb_(pair.dex_leg->order_id, OrderStatus::FILLED,
                                    fill_price, fill_qty);
                }

                if (pair.status == ArbPairStatus::DISPATCHED) {
                    pair.status = ArbPairStatus::DEX_FILLED;
                    logger_->info("ArbPair {}: DEX leg FILLED at {:.2f}", group_id, fill_price);
                } else if (pair.status == ArbPairStatus::CEX_FILLED) {
                    pair.status = ArbPairStatus::ALL_FILLED;
                    finalize_pair(pair);
                }
            }
            return;
        }
    }

    ArbPair get_pair(const std::string& group_id) const {
        std::lock_guard lock(mutex_);
        auto it = pairs_.find(group_id);
        if (it != pairs_.end()) return it->second;
        return {};
    }

    void reset() {
        // Wait for worker threads
        {
            std::lock_guard lock(threads_mutex_);
            for (auto& t : worker_threads_) {
                if (t.joinable()) t.join();
            }
            worker_threads_.clear();
        }
        std::lock_guard lock(mutex_);
        pairs_.clear();
    }

private:
    void start_deadline_timer(const std::string& group_id) {
        std::lock_guard lock(threads_mutex_);
        worker_threads_.emplace_back([this, group_id]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.assembly_deadline_ms));

            std::unique_lock lock(mutex_);
            auto it = pairs_.find(group_id);
            if (it == pairs_.end()) return;

            auto& pair = it->second;
            if (pair.status == ArbPairStatus::ASSEMBLING && !pair.both_legs_present()) {
                pair.status = ArbPairStatus::FAILED;
                logger_->warn("ArbPair {}: incomplete_pair: deadline {}ms exceeded, only {}/2 legs received",
                             group_id, config_.assembly_deadline_ms,
                             (pair.cex_leg.has_value() ? 1 : 0) + (pair.dex_leg.has_value() ? 1 : 0));

                // Release any reserved margins
                if (pair.cex_leg) {
                    pair.cex_leg->status = OrderStatus::REJECTED;
                    pair.cex_leg->reject_reason = "incomplete_pair: deadline " +
                        std::to_string(config_.assembly_deadline_ms) + "ms exceeded";
                    portfolio_.release_margin(pair.cex_leg->signal);
                }
                if (pair.dex_leg) {
                    pair.dex_leg->status = OrderStatus::REJECTED;
                    pair.dex_leg->reject_reason = "incomplete_pair: deadline " +
                        std::to_string(config_.assembly_deadline_ms) + "ms exceeded";
                    portfolio_.release_margin(pair.dex_leg->signal);
                }
            }
        });
    }

    void dispatch_pair(const std::string& group_id) {
        std::unique_lock lock(mutex_);
        auto it = pairs_.find(group_id);
        if (it == pairs_.end()) return;

        auto& pair = it->second;
        if (pair.status != ArbPairStatus::ASSEMBLING) return;

        auto& cex_sig = pair.cex_leg->signal;
        auto& dex_sig = pair.dex_leg->signal;

        // Calculate expected spread
        pair.expected_spread_bps = calc_expected_spread(cex_sig, dex_sig);

        // Atomic margin reservation
        auto margin_result = portfolio_.check_and_reserve_pair(cex_sig, dex_sig);
        if (!margin_result.approved) {
            pair.status = ArbPairStatus::FAILED;
            pair.cex_leg->status = OrderStatus::REJECTED;
            pair.cex_leg->reject_reason = margin_result.reason;
            pair.dex_leg->status = OrderStatus::REJECTED;
            pair.dex_leg->reject_reason = margin_result.reason;
            logger_->warn("ArbPair {}: margin rejected - {}", group_id, margin_result.reason);
            return;
        }

        pair.status = ArbPairStatus::DISPATCHED;
        pair.cex_leg->status = OrderStatus::SENT;
        pair.cex_leg->sent_at_ns = now_ns();
        pair.dex_leg->status = OrderStatus::SENT;
        pair.dex_leg->sent_at_ns = now_ns();

        auto cex_order = *pair.cex_leg;
        auto dex_order = *pair.dex_leg;

        logger_->info("ArbPair {}: DISPATCHED - CEX {} on {}, DEX {} on {}",
                     group_id, cex_order.order_id, to_string(cex_sig.exchange),
                     dex_order.order_id, to_string(dex_sig.exchange));

        lock.unlock();

        // Dispatch both legs simultaneously
        auto cex_gw = gateways_.find(cex_sig.exchange);
        auto dex_gw = gateways_.find(dex_sig.exchange);

        if (cex_gw != gateways_.end()) {
            auto cex_future = cex_gw->second->send_order(cex_order);
            std::lock_guard tlock(threads_mutex_);
            worker_threads_.emplace_back([this, group_id, fut = std::move(cex_future)]() mutable {
                auto result = fut.get();
                std::unique_lock lock(mutex_);
                auto it = pairs_.find(group_id);
                if (it != pairs_.end() && it->second.cex_leg) {
                    it->second.cex_leg->exchange_order_id = result.exchange_order_id;
                    if (result.success) {
                        it->second.cex_leg->status = OrderStatus::ACKNOWLEDGED;
                    } else {
                        handle_leg_reject(it->second, true, lock);
                    }
                }
            });
        }

        if (dex_gw != gateways_.end()) {
            auto dex_future = dex_gw->second->send_order(dex_order);
            std::lock_guard tlock(threads_mutex_);
            worker_threads_.emplace_back([this, group_id, fut = std::move(dex_future)]() mutable {
                auto result = fut.get();
                std::unique_lock lock(mutex_);
                auto it = pairs_.find(group_id);
                if (it != pairs_.end() && it->second.dex_leg) {
                    it->second.dex_leg->exchange_order_id = result.exchange_order_id;
                    if (result.success) {
                        it->second.dex_leg->status = OrderStatus::TX_PENDING;
                        it->second.dex_leg->tx_hash = result.tx_hash;
                    } else {
                        handle_leg_reject(it->second, false, lock);
                    }
                }
            });
        }

        // Start risk window monitor
        start_risk_window_monitor(group_id);
    }

    void handle_leg_reject(ArbPair& pair, bool is_cex_reject,
                           std::unique_lock<std::mutex>& /*lock*/) {
        auto& group_id = pair.group_id;

        if (is_cex_reject) {
            logger_->warn("ArbPair {}: CEX leg REJECTED", group_id);

            if (pair.dex_leg && pair.dex_leg->status == OrderStatus::FILLED) {
                // DEX filled but CEX rejected — unwind DEX
                pair.status = ArbPairStatus::UNWINDING;
                // For mock, just mark as failed
                pair.status = ArbPairStatus::FAILED;
            } else {
                // Cancel DEX leg if pending
                pair.status = ArbPairStatus::FAILED;
            }

            pair.cex_leg->status = OrderStatus::REJECTED;
            portfolio_.release_margin(pair.cex_leg->signal);
            if (pair.dex_leg && !is_terminal(pair.dex_leg->status)) {
                pair.dex_leg->status = OrderStatus::CANCELLED;
                portfolio_.release_margin(pair.dex_leg->signal);
            }
        } else {
            // DEX rejected
            logger_->warn("ArbPair {}: DEX leg REJECTED", group_id);

            if (pair.cex_leg && pair.cex_leg->status == OrderStatus::FILLED) {
                // CEX filled but DEX rejected — need to unwind CEX
                pair.status = ArbPairStatus::UNWINDING;
                logger_->warn("ArbPair {}: DEX leg rejected, unwinding CEX leg {}",
                             group_id, pair.cex_leg->order_id);

                // Send reverse order on CEX
                start_unwind(pair);
            } else {
                pair.status = ArbPairStatus::FAILED;
                if (pair.cex_leg) {
                    pair.cex_leg->status = OrderStatus::CANCELLED;
                    portfolio_.release_margin(pair.cex_leg->signal);
                }
            }

            pair.dex_leg->status = OrderStatus::REJECTED;
            portfolio_.release_margin(pair.dex_leg->signal);
        }
    }

    void start_unwind(ArbPair& pair) {
        auto& cex_leg = *pair.cex_leg;
        auto group_id = pair.group_id;

        // Create reverse order
        Signal unwind_sig = cex_leg.signal;
        unwind_sig.signal_id = UuidGenerator::signal_id();
        unwind_sig.side = (cex_leg.signal.side == Side::BUY) ? Side::SELL : Side::BUY;
        unwind_sig.price = 0; // market order

        Order unwind_order;
        unwind_order.order_id = UuidGenerator::order_id();
        unwind_order.signal = unwind_sig;
        unwind_order.status = OrderStatus::SENT;
        unwind_order.sent_at_ns = now_ns();

        auto exch = cex_leg.signal.exchange;

        logger_->info("ArbPair {}: Sending unwind {} {} on {}",
                     group_id, to_string(unwind_sig.side), unwind_sig.symbol,
                     to_string(exch));

        auto gw_it = gateways_.find(exch);
        if (gw_it != gateways_.end()) {
            // Need to handle the unwind fill - subscribe with a special callback
            auto* gw = gw_it->second;
            std::lock_guard tlock(threads_mutex_);
            worker_threads_.emplace_back([this, gw, unwind_order, group_id]() {
                auto result = gw->send_order(unwind_order).get();

                // Wait a bit for fill
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                std::lock_guard lock(mutex_);
                auto it = pairs_.find(group_id);
                if (it != pairs_.end()) {
                    if (result.success) {
                        it->second.status = ArbPairStatus::COMPLETED;
                        logger_->info("ArbPair {}: unwind complete", group_id);
                    } else {
                        it->second.status = ArbPairStatus::FAILED;
                        logger_->error("ArbPair {}: unwind FAILED", group_id);
                    }
                    // Release CEX margin since we unwound
                    portfolio_.release_margin(it->second.cex_leg->signal);
                }
            });
        }
    }

    void start_risk_window_monitor(const std::string& group_id) {
        std::lock_guard tlock(threads_mutex_);
        worker_threads_.emplace_back([this, group_id]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.max_risk_window_ms));

            std::unique_lock lock(mutex_);
            auto it = pairs_.find(group_id);
            if (it == pairs_.end()) return;

            auto& pair = it->second;
            // If one leg filled and the other hasn't after max risk window, trigger unwind
            if (pair.status == ArbPairStatus::CEX_FILLED) {
                logger_->warn("ArbPair {}: max risk window {}ms exceeded, DEX still pending",
                             group_id, config_.max_risk_window_ms);
                pair.status = ArbPairStatus::UNWINDING;
                pair.dex_leg->status = OrderStatus::TIMEOUT;
                portfolio_.release_margin(pair.dex_leg->signal);
                start_unwind(pair);
            } else if (pair.status == ArbPairStatus::DEX_FILLED) {
                logger_->warn("ArbPair {}: max risk window {}ms exceeded, CEX still pending",
                             group_id, config_.max_risk_window_ms);
                pair.status = ArbPairStatus::FAILED;
                pair.cex_leg->status = OrderStatus::TIMEOUT;
                portfolio_.release_margin(pair.cex_leg->signal);
                portfolio_.release_margin(pair.dex_leg->signal);
            }
        });
    }

    void finalize_pair(ArbPair& pair) {
        auto& group_id = pair.group_id;

        // Calculate risk window
        if (pair.cex_fill_ns > 0 && pair.dex_confirm_ns > 0) {
            pair.risk_window_ns = std::abs(pair.dex_confirm_ns - pair.cex_fill_ns);
        }

        // Calculate realized spread
        pair.realized_spread_bps = calc_realized_spread(pair);

        double slippage = pair.expected_spread_bps - pair.realized_spread_bps;

        pair.status = ArbPairStatus::COMPLETED;

        logger_->info("ArbPair {}: COMPLETED - risk_window={:.0f}ms, expected={:.1f}bps, "
                     "realized={:.1f}bps, slippage={:.1f}bps",
                     group_id,
                     pair.risk_window_ns / 1e6,
                     pair.expected_spread_bps,
                     pair.realized_spread_bps,
                     slippage);

        // Apply fills to portfolio
        portfolio_.on_fill(pair.cex_leg->signal, pair.cex_leg->avg_fill_price,
                          pair.cex_leg->filled_quantity);
        portfolio_.on_fill(pair.dex_leg->signal, pair.dex_leg->avg_fill_price,
                          pair.dex_leg->filled_quantity);
    }

    double calc_expected_spread(const Signal& cex_sig, const Signal& dex_sig) const {
        double cex_price = cex_sig.price > 0 ? cex_sig.price
            : PortfolioGuard::reference_price(cex_sig.symbol);
        double dex_price = dex_sig.price > 0 ? dex_sig.price
            : PortfolioGuard::reference_price(dex_sig.symbol);

        if (cex_sig.side == Side::BUY) {
            // CEX BUY, DEX SELL
            return (dex_price - cex_price) / cex_price * 10000.0;
        } else {
            // CEX SELL, DEX BUY
            return (cex_price - dex_price) / dex_price * 10000.0;
        }
    }

    double calc_realized_spread(const ArbPair& pair) const {
        double cex_fill = pair.cex_leg->avg_fill_price;
        double dex_fill = pair.dex_leg->avg_fill_price;

        if (pair.cex_leg->signal.side == Side::BUY) {
            return (dex_fill - cex_fill) / cex_fill * 10000.0;
        } else {
            return (cex_fill - dex_fill) / dex_fill * 10000.0;
        }
    }

    Config config_;
    PortfolioGuard& portfolio_;
    GatewayMap& gateways_;
    std::unordered_map<std::string, ArbPair> pairs_;
    mutable std::mutex mutex_;
    std::vector<std::thread> worker_threads_;
    std::mutex threads_mutex_;
    std::shared_ptr<spdlog::logger> logger_;
    OrderUpdateCallback order_update_cb_;
};

} // namespace oem
