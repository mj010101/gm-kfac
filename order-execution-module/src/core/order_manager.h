#pragma once
#include "../model/signal.h"
#include "../model/order.h"
#include "../transport/signal_receiver.h"
#include "../gateway/i_exchange_gateway.h"
#include "portfolio_guard.h"
#include "conflict_resolver.h"
#include "arb_coordinator.h"
#include "../util/uuid.h"
#include "../util/async_logger.h"
#include "../util/latency_tracker.h"
#include "../util/spinlock.h"
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace oem {

class OrderManager {
public:
    using GatewayMap = std::unordered_map<Exchange, IExchangeGateway*>;

    struct Config {
        SignalReceiver::Config signal_config;
        PortfolioGuard::Config portfolio_config;
        ArbCoordinator::Config arb_config;
    };

    struct ProcessResult {
        bool success = false;
        char order_id[16] = {};
        char reason[128] = {};

        void set_order_id(const std::string& id) {
            std::strncpy(order_id, id.c_str(), 15);
            order_id[15] = '\0';
        }
        void set_reason(const char* r) {
            std::strncpy(reason, r, 127);
            reason[127] = '\0';
        }
    };

    OrderManager(Config config, GatewayMap gateways)
        : gateways_(std::move(gateways))
        , signal_receiver_(config.signal_config)
        , portfolio_(config.portfolio_config)
        , conflict_resolver_()
        , arb_coordinator_(config.arb_config, portfolio_, gateways_)
    {
        for (auto& [exch, gw] : gateways_) {
            gw->subscribe_fills([this](const Fill& fill) {
                on_fill(fill);
            });
        }
    }

    ~OrderManager() {
        std::lock_guard lock(threads_mutex_);
        for (auto& t : dispatch_threads_) {
            if (t.joinable()) t.join();
        }
    }

    ProcessResult process_signal(const Signal& signal) {
        auto validation = signal_receiver_.validate(signal);
        if (!validation.valid) {
            global_logger().warn("order_manager", "Signal %s REJECTED: %s",
                                 signal.signal_id, validation.reason);
            ProcessResult r;
            r.set_reason(validation.reason);
            return r;
        }

        if (signal.has_group_id) {
            auto order = arb_coordinator_.add_leg(signal);
            ProcessResult r;
            r.success = true;
            std::strncpy(r.order_id, order.order_id, 15);
            return r;
        }

        return process_single_leg(signal);
    }

    void wait_for_completion(int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);

        {
            std::lock_guard lock(threads_mutex_);
            for (auto& t : dispatch_threads_) {
                if (t.joinable()) t.join();
            }
            dispatch_threads_.clear();
        }

        while (std::chrono::steady_clock::now() < deadline) {
            bool all_done = true;
            {
                SpinLock::Guard lock(orders_mutex_);
                for (auto& [id, order] : orders_) {
                    if (!is_terminal(order.status) && order.status != OrderStatus::UNWINDING) {
                        all_done = false;
                        break;
                    }
                }
            }
            if (all_done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    Order get_order(const std::string& order_id) const {
        // SpinLock doesn't support shared lock, but sections are short
        return orders_.count(order_id) ? orders_.at(order_id) : Order{};
    }

    std::vector<Order> get_all_orders() const {
        std::vector<Order> result;
        for (auto& [_, o] : orders_) result.push_back(o);
        return result;
    }

    PortfolioGuard& portfolio() { return portfolio_; }
    ArbCoordinator& arb_coordinator() { return arb_coordinator_; }
    ConflictResolver& conflict_resolver() { return conflict_resolver_; }

    void reset() {
        {
            std::lock_guard lock(threads_mutex_);
            for (auto& t : dispatch_threads_) {
                if (t.joinable()) t.join();
            }
            dispatch_threads_.clear();
        }
        arb_coordinator_.reset();
        conflict_resolver_.reset();
        portfolio_.reset();
        {
            SpinLock::Guard lock(orders_mutex_);
            orders_.clear();
        }

        for (auto& [exch, gw] : gateways_) {
            gw->subscribe_fills([this](const Fill& fill) {
                on_fill(fill);
            });
        }
    }

private:
    ProcessResult process_single_leg(const Signal& signal) {
        Order order;
        order.set_order_id(UuidGenerator::order_id());
        order.signal = signal;
        order.status = OrderStatus::CREATED;
        order.created_at_ns = now_ns();

        auto conflict_result = conflict_resolver_.check(signal, gateways_);
        if (!conflict_result.can_proceed) {
            order.status = OrderStatus::REJECTED;
            order.set_reject_reason(conflict_result.reason);
            store_order(order);
            global_logger().warn("order_manager", "Order %s REJECTED: %s",
                                 order.order_id, conflict_result.reason);
            ProcessResult r;
            std::strncpy(r.order_id, order.order_id, 15);
            r.set_reason(conflict_result.reason);
            return r;
        }

        auto margin_result = portfolio_.check_and_reserve(signal);
        if (!margin_result.approved) {
            order.status = OrderStatus::REJECTED;
            order.set_reject_reason(margin_result.reason);
            store_order(order);
            global_logger().warn("order_manager", "Order %s REJECTED: %s",
                                 order.order_id, margin_result.reason);
            ProcessResult r;
            std::strncpy(r.order_id, order.order_id, 15);
            r.set_reason(margin_result.reason);
            return r;
        }

        order.status = OrderStatus::SENT;
        order.sent_at_ns = now_ns();
        store_order(order);

        conflict_resolver_.register_order(order);

        auto exch = signal.exchange;
        auto gw_it = gateways_.find(exch);
        if (gw_it == gateways_.end()) {
            order.status = OrderStatus::REJECTED;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "no gateway for exchange %s", to_string(exch));
            order.set_reject_reason(buf);
            store_order(order);
            portfolio_.release_margin(signal);
            ProcessResult r;
            std::strncpy(r.order_id, order.order_id, 15);
            r.set_reason(buf);
            return r;
        }

        global_logger().info("order_manager", "Order %s SENT to %s for %s %s %.4f",
                             order.order_id, to_string(exch), signal.symbol,
                             to_string(signal.side), signal.quantity);

        std::string order_id_str(order.order_id);
        auto* gw = gw_it->second;
        {
            std::lock_guard lock(threads_mutex_);
            dispatch_threads_.emplace_back([this, gw, order, order_id_str]() {
                auto result = gw->send_order(order).get();

                SpinLock::Guard lock(orders_mutex_);
                auto it = orders_.find(order_id_str);
                if (it != orders_.end()) {
                    it->second.set_exchange_order_id(result.exchange_order_id);
                    it->second.latency.send_to_ack_ns = result.latency_ns;

                    if (result.success) {
                        auto new_status = (get_exchange_type(order.signal.exchange) == ExchangeType::DEX)
                            ? OrderStatus::TX_PENDING : OrderStatus::ACKNOWLEDGED;
                        it->second.status = new_status;
                        if (result.has_tx_hash) {
                            it->second.set_tx_hash(result.tx_hash);
                        }
                        global_logger().info("order_manager", "Order %s status -> %s",
                                             order_id_str.c_str(), to_string(new_status));
                    } else {
                        it->second.status = OrderStatus::REJECTED;
                        it->second.set_reject_reason(result.error_message);
                        portfolio_.release_margin(order.signal);
                        conflict_resolver_.update_status(order_id_str, OrderStatus::REJECTED);
                    }
                }
            });
        }

        ProcessResult r;
        r.success = true;
        std::strncpy(r.order_id, order.order_id, 15);
        return r;
    }

    void on_fill(const Fill& fill) {
        bool is_reject = (fill.filled_qty == 0.0 && fill.fill_price == 0.0);

        arb_coordinator_.on_fill(fill.exchange_order_id, fill.fill_price,
                                fill.filled_qty, is_reject);

        SpinLock::Guard lock(orders_mutex_);
        for (auto& [id, order] : orders_) {
            if (std::strcmp(order.exchange_order_id, fill.exchange_order_id) == 0) {
                if (is_reject) {
                    order.status = OrderStatus::REJECTED;
                    portfolio_.release_margin(order.signal);
                } else {
                    order.status = OrderStatus::FILLED;
                    order.filled_quantity = fill.filled_qty;
                    order.avg_fill_price = fill.fill_price;
                    order.last_update_ns = fill.fill_timestamp_ns;
                    order.latency.total_ns = fill.fill_timestamp_ns - order.created_at_ns;

                    if (fill.has_tx_hash) {
                        order.set_tx_hash(fill.tx_hash);
                        order.tx_confirmed_ns = fill.fill_timestamp_ns;
                        order.has_tx_confirmed_ns = true;
                    }

                    portfolio_.on_fill(order.signal, fill.fill_price, fill.filled_qty);
                    conflict_resolver_.update_status(id, OrderStatus::FILLED);

                    global_logger().info("order_manager", "Order %s FILLED: %.4f @ %.2f, latency=%.2fms",
                                         id.c_str(), fill.filled_qty, fill.fill_price,
                                         order.latency.total_ns / 1e6);
                }
                break;
            }
        }
    }

    void store_order(const Order& order) {
        SpinLock::Guard lock(orders_mutex_);
        orders_[order.order_id] = order;
    }

    GatewayMap gateways_;
    SignalReceiver signal_receiver_;
    PortfolioGuard portfolio_;
    ConflictResolver conflict_resolver_;
    ArbCoordinator arb_coordinator_;

    std::unordered_map<std::string, Order> orders_;
    mutable SpinLock orders_mutex_;

    std::vector<std::thread> dispatch_threads_;
    std::mutex threads_mutex_;
};

} // namespace oem
