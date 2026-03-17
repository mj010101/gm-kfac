#pragma once
#include "../model/arb_pair.h"
#include "../model/order.h"
#include "../gateway/i_exchange_gateway.h"
#include "portfolio_guard.h"
#include "../util/async_logger.h"
#include "../util/uuid.h"
#include "../util/latency_tracker.h"
#include "../util/spinlock.h"
#include <chrono>
#include <cmath>
#include <cstring>
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
        , gateways_(gateways) {}

    ~ArbCoordinator() {
        std::lock_guard lock(threads_mutex_);
        for (auto& t : worker_threads_) {
            if (t.joinable()) t.join();
        }
    }

    void set_order_update_callback(OrderUpdateCallback cb) {
        order_update_cb_ = cb;
    }

    Order add_leg(const Signal& signal) {
        std::unique_lock lock(mutex_);

        std::string group_id_str(signal.group_id);
        auto exch_type = get_exchange_type(signal.exchange);

        Order order;
        order.set_order_id(UuidGenerator::order_id());
        order.signal = signal;
        order.status = OrderStatus::CREATED;
        order.created_at_ns = now_ns();

        auto it = pairs_.find(group_id_str);
        if (it == pairs_.end()) {
            ArbPair pair;
            pair.set_group_id(group_id_str);
            pair.created_at_ns = now_ns();
            pair.dispatch_deadline_ns = now_ns() + config_.assembly_deadline_ms * 1000000LL;
            pairs_[group_id_str] = pair;
            it = pairs_.find(group_id_str);

            global_logger().info("arb_coordinator", "ArbPair %s created, deadline in %lldms",
                                 group_id_str.c_str(), (long long)config_.assembly_deadline_ms);

            start_deadline_timer(group_id_str);
        }

        auto& pair = it->second;

        if (exch_type == ExchangeType::CEX) {
            pair.cex_leg = order;
            pair.has_cex_leg = true;
        } else {
            pair.dex_leg = order;
            pair.has_dex_leg = true;
        }

        global_logger().info("arb_coordinator", "ArbPair %s leg added: %s %s on %s (%s)",
                             group_id_str.c_str(), to_string(signal.side), signal.symbol,
                             to_string(signal.exchange), to_string(exch_type));

        if (pair.both_legs_present()) {
            lock.unlock();
            dispatch_pair(group_id_str);
        }

        return order;
    }

    void on_fill(const std::string& exchange_order_id, double fill_price,
                 double fill_qty, bool is_reject) {
        std::unique_lock lock(mutex_);

        for (auto& [group_id, pair] : pairs_) {
            std::string cex_exch_id = pair.has_cex_leg ?
                (std::string("EX-") + pair.cex_leg.order_id) : "";
            std::string dex_exch_id = pair.has_dex_leg ?
                (std::string("HL-") + pair.dex_leg.order_id) : "";

            bool is_cex = pair.has_cex_leg && cex_exch_id == exchange_order_id;
            bool is_dex = pair.has_dex_leg && dex_exch_id == exchange_order_id;

            if (!is_cex && !is_dex) continue;

            if (is_reject) {
                handle_leg_reject(pair, is_cex, lock);
                return;
            }

            if (is_cex) {
                pair.cex_leg.status = OrderStatus::FILLED;
                pair.cex_leg.filled_quantity = fill_qty;
                pair.cex_leg.avg_fill_price = fill_price;
                pair.cex_fill_ns = now_ns();

                if (order_update_cb_) {
                    order_update_cb_(pair.cex_leg.order_id, OrderStatus::FILLED,
                                    fill_price, fill_qty);
                }

                if (pair.status == ArbPairStatus::DISPATCHED) {
                    pair.status = ArbPairStatus::CEX_FILLED;
                    global_logger().info("arb_coordinator", "ArbPair %s: CEX leg FILLED at %.2f",
                                         group_id.c_str(), fill_price);
                } else if (pair.status == ArbPairStatus::DEX_FILLED) {
                    pair.status = ArbPairStatus::ALL_FILLED;
                    finalize_pair(pair);
                }
            } else {
                pair.dex_leg.status = OrderStatus::FILLED;
                pair.dex_leg.filled_quantity = fill_qty;
                pair.dex_leg.avg_fill_price = fill_price;
                pair.dex_confirm_ns = now_ns();

                if (order_update_cb_) {
                    order_update_cb_(pair.dex_leg.order_id, OrderStatus::FILLED,
                                    fill_price, fill_qty);
                }

                if (pair.status == ArbPairStatus::DISPATCHED) {
                    pair.status = ArbPairStatus::DEX_FILLED;
                    global_logger().info("arb_coordinator", "ArbPair %s: DEX leg FILLED at %.2f",
                                         group_id.c_str(), fill_price);
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
                global_logger().warn("arb_coordinator",
                    "ArbPair %s: incomplete_pair: deadline %lldms exceeded, only %d/2 legs received",
                    group_id.c_str(), (long long)config_.assembly_deadline_ms,
                    (pair.has_cex_leg ? 1 : 0) + (pair.has_dex_leg ? 1 : 0));

                if (pair.has_cex_leg) {
                    pair.cex_leg.status = OrderStatus::REJECTED;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "incomplete_pair: deadline %lldms exceeded",
                                 (long long)config_.assembly_deadline_ms);
                    pair.cex_leg.set_reject_reason(buf);
                    portfolio_.release_margin(pair.cex_leg.signal);
                }
                if (pair.has_dex_leg) {
                    pair.dex_leg.status = OrderStatus::REJECTED;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "incomplete_pair: deadline %lldms exceeded",
                                 (long long)config_.assembly_deadline_ms);
                    pair.dex_leg.set_reject_reason(buf);
                    portfolio_.release_margin(pair.dex_leg.signal);
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

        auto& cex_sig = pair.cex_leg.signal;
        auto& dex_sig = pair.dex_leg.signal;

        pair.expected_spread_bps = calc_expected_spread(cex_sig, dex_sig);

        auto margin_result = portfolio_.check_and_reserve_pair(cex_sig, dex_sig);
        if (!margin_result.approved) {
            pair.status = ArbPairStatus::FAILED;
            pair.cex_leg.status = OrderStatus::REJECTED;
            pair.cex_leg.set_reject_reason(margin_result.reason);
            pair.dex_leg.status = OrderStatus::REJECTED;
            pair.dex_leg.set_reject_reason(margin_result.reason);
            global_logger().warn("arb_coordinator", "ArbPair %s: margin rejected - %s",
                                 group_id.c_str(), margin_result.reason);
            return;
        }

        pair.status = ArbPairStatus::DISPATCHED;
        pair.cex_leg.status = OrderStatus::SENT;
        pair.cex_leg.sent_at_ns = now_ns();
        pair.dex_leg.status = OrderStatus::SENT;
        pair.dex_leg.sent_at_ns = now_ns();

        auto cex_order = pair.cex_leg;
        auto dex_order = pair.dex_leg;

        global_logger().info("arb_coordinator", "ArbPair %s: DISPATCHED - CEX %s on %s, DEX %s on %s",
                             group_id.c_str(), cex_order.order_id, to_string(cex_sig.exchange),
                             dex_order.order_id, to_string(dex_sig.exchange));

        lock.unlock();

        auto cex_gw = gateways_.find(cex_sig.exchange);
        auto dex_gw = gateways_.find(dex_sig.exchange);

        if (cex_gw != gateways_.end()) {
            auto cex_future = cex_gw->second->send_order(cex_order);
            std::lock_guard tlock(threads_mutex_);
            worker_threads_.emplace_back([this, group_id, fut = std::move(cex_future)]() mutable {
                auto result = fut.get();
                std::unique_lock lock(mutex_);
                auto it = pairs_.find(group_id);
                if (it != pairs_.end() && it->second.has_cex_leg) {
                    it->second.cex_leg.set_exchange_order_id(result.exchange_order_id);
                    if (result.success) {
                        it->second.cex_leg.status = OrderStatus::ACKNOWLEDGED;
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
                if (it != pairs_.end() && it->second.has_dex_leg) {
                    it->second.dex_leg.set_exchange_order_id(result.exchange_order_id);
                    if (result.success) {
                        it->second.dex_leg.status = OrderStatus::TX_PENDING;
                        if (result.has_tx_hash) {
                            it->second.dex_leg.set_tx_hash(result.tx_hash);
                        }
                    } else {
                        handle_leg_reject(it->second, false, lock);
                    }
                }
            });
        }

        start_risk_window_monitor(group_id);
    }

    void handle_leg_reject(ArbPair& pair, bool is_cex_reject,
                           std::unique_lock<std::mutex>& /*lock*/) {
        if (is_cex_reject) {
            global_logger().warn("arb_coordinator", "ArbPair %s: CEX leg REJECTED", pair.group_id);

            if (pair.has_dex_leg && pair.dex_leg.status == OrderStatus::FILLED) {
                pair.status = ArbPairStatus::UNWINDING;
                pair.status = ArbPairStatus::FAILED;
            } else {
                pair.status = ArbPairStatus::FAILED;
            }

            pair.cex_leg.status = OrderStatus::REJECTED;
            portfolio_.release_margin(pair.cex_leg.signal);
            if (pair.has_dex_leg && !is_terminal(pair.dex_leg.status)) {
                pair.dex_leg.status = OrderStatus::CANCELLED;
                portfolio_.release_margin(pair.dex_leg.signal);
            }
        } else {
            global_logger().warn("arb_coordinator", "ArbPair %s: DEX leg REJECTED", pair.group_id);

            if (pair.has_cex_leg && pair.cex_leg.status == OrderStatus::FILLED) {
                pair.status = ArbPairStatus::UNWINDING;
                global_logger().warn("arb_coordinator", "ArbPair %s: DEX leg rejected, unwinding CEX leg %s",
                                     pair.group_id, pair.cex_leg.order_id);
                start_unwind(pair);
            } else {
                pair.status = ArbPairStatus::FAILED;
                if (pair.has_cex_leg) {
                    pair.cex_leg.status = OrderStatus::CANCELLED;
                    portfolio_.release_margin(pair.cex_leg.signal);
                }
            }

            pair.dex_leg.status = OrderStatus::REJECTED;
            portfolio_.release_margin(pair.dex_leg.signal);
        }
    }

    void start_unwind(ArbPair& pair) {
        auto& cex_leg = pair.cex_leg;
        std::string group_id_str(pair.group_id);

        Signal unwind_sig = cex_leg.signal;
        unwind_sig.set_signal_id(UuidGenerator::signal_id());
        unwind_sig.side = (cex_leg.signal.side == Side::BUY) ? Side::SELL : Side::BUY;
        unwind_sig.price = 0;

        Order unwind_order;
        unwind_order.set_order_id(UuidGenerator::order_id());
        unwind_order.signal = unwind_sig;
        unwind_order.status = OrderStatus::SENT;
        unwind_order.sent_at_ns = now_ns();

        auto exch = cex_leg.signal.exchange;

        global_logger().info("arb_coordinator", "ArbPair %s: Sending unwind %s %s on %s",
                             group_id_str.c_str(), to_string(unwind_sig.side), unwind_sig.symbol,
                             to_string(exch));

        auto gw_it = gateways_.find(exch);
        if (gw_it != gateways_.end()) {
            auto* gw = gw_it->second;
            std::lock_guard tlock(threads_mutex_);
            worker_threads_.emplace_back([this, gw, unwind_order, group_id_str]() {
                auto result = gw->send_order(unwind_order).get();

                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                std::lock_guard lock(mutex_);
                auto it = pairs_.find(group_id_str);
                if (it != pairs_.end()) {
                    if (result.success) {
                        it->second.status = ArbPairStatus::COMPLETED;
                        global_logger().info("arb_coordinator", "ArbPair %s: unwind complete",
                                             group_id_str.c_str());
                    } else {
                        it->second.status = ArbPairStatus::FAILED;
                        global_logger().error("arb_coordinator", "ArbPair %s: unwind FAILED",
                                              group_id_str.c_str());
                    }
                    portfolio_.release_margin(it->second.cex_leg.signal);
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
            if (pair.status == ArbPairStatus::CEX_FILLED) {
                global_logger().warn("arb_coordinator", "ArbPair %s: max risk window %lldms exceeded, DEX still pending",
                                     group_id.c_str(), (long long)config_.max_risk_window_ms);
                pair.status = ArbPairStatus::UNWINDING;
                pair.dex_leg.status = OrderStatus::TIMEOUT;
                portfolio_.release_margin(pair.dex_leg.signal);
                start_unwind(pair);
            } else if (pair.status == ArbPairStatus::DEX_FILLED) {
                global_logger().warn("arb_coordinator", "ArbPair %s: max risk window %lldms exceeded, CEX still pending",
                                     group_id.c_str(), (long long)config_.max_risk_window_ms);
                pair.status = ArbPairStatus::FAILED;
                pair.cex_leg.status = OrderStatus::TIMEOUT;
                portfolio_.release_margin(pair.cex_leg.signal);
                portfolio_.release_margin(pair.dex_leg.signal);
            }
        });
    }

    void finalize_pair(ArbPair& pair) {
        if (pair.cex_fill_ns > 0 && pair.dex_confirm_ns > 0) {
            pair.risk_window_ns = std::abs(pair.dex_confirm_ns - pair.cex_fill_ns);
        }

        pair.realized_spread_bps = calc_realized_spread(pair);

        double slippage = pair.expected_spread_bps - pair.realized_spread_bps;

        pair.status = ArbPairStatus::COMPLETED;

        global_logger().info("arb_coordinator",
            "ArbPair %s: COMPLETED - risk_window=%.0fms, expected=%.1fbps, realized=%.1fbps, slippage=%.1fbps",
            pair.group_id,
            pair.risk_window_ns / 1e6,
            pair.expected_spread_bps,
            pair.realized_spread_bps,
            slippage);

        portfolio_.on_fill(pair.cex_leg.signal, pair.cex_leg.avg_fill_price,
                          pair.cex_leg.filled_quantity);
        portfolio_.on_fill(pair.dex_leg.signal, pair.dex_leg.avg_fill_price,
                          pair.dex_leg.filled_quantity);
    }

    double calc_expected_spread(const Signal& cex_sig, const Signal& dex_sig) const {
        double cex_price = cex_sig.price > 0 ? cex_sig.price
            : PortfolioGuard::reference_price(cex_sig.symbol);
        double dex_price = dex_sig.price > 0 ? dex_sig.price
            : PortfolioGuard::reference_price(dex_sig.symbol);

        if (cex_sig.side == Side::BUY) {
            return (dex_price - cex_price) / cex_price * 10000.0;
        } else {
            return (cex_price - dex_price) / dex_price * 10000.0;
        }
    }

    double calc_realized_spread(const ArbPair& pair) const {
        double cex_fill = pair.cex_leg.avg_fill_price;
        double dex_fill = pair.dex_leg.avg_fill_price;

        if (pair.cex_leg.signal.side == Side::BUY) {
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
    OrderUpdateCallback order_update_cb_;
};

} // namespace oem
