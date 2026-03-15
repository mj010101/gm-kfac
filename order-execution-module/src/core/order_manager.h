#pragma once
#include "../model/signal.h"
#include "../model/order.h"
#include "../transport/signal_receiver.h"
#include "../gateway/i_exchange_gateway.h"
#include "portfolio_guard.h"
#include "conflict_resolver.h"
#include "arb_coordinator.h"
#include "../util/uuid.h"
#include "../util/logger.h"
#include "../util/latency_tracker.h"
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <shared_mutex>
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

    OrderManager(Config config, GatewayMap gateways)
        : gateways_(std::move(gateways))
        , signal_receiver_(config.signal_config)
        , portfolio_(config.portfolio_config)
        , conflict_resolver_()
        , arb_coordinator_(config.arb_config, portfolio_, gateways_)
        , logger_(get_logger("order_manager"))
    {
        // Subscribe to fill callbacks on all gateways
        for (auto& [exch, gw] : gateways_) {
            gw->subscribe_fills([this](const Fill& fill) {
                on_fill(fill);
            });
        }
    }

    ~OrderManager() {
        // Wait for pending dispatch threads
        std::lock_guard lock(threads_mutex_);
        for (auto& t : dispatch_threads_) {
            if (t.joinable()) t.join();
        }
    }

    struct ProcessResult {
        bool success = false;
        std::string order_id;
        std::string reason;
    };

    ProcessResult process_signal(const Signal& signal) {
        // 1. Validate
        auto validation = signal_receiver_.validate(signal);
        if (!validation.valid) {
            logger_->warn("Signal {} REJECTED: {}", signal.signal_id, validation.reason);
            return {false, "", validation.reason};
        }

        // 2. Route
        if (signal.group_id.has_value()) {
            // Arb path
            auto order = arb_coordinator_.add_leg(signal);
            return {true, order.order_id, ""};
        }

        // 3. Single-leg pipeline
        return process_single_leg(signal);
    }

    // Wait for all orders to reach terminal state (with timeout)
    void wait_for_completion(int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);

        // Wait for dispatch threads
        {
            std::lock_guard lock(threads_mutex_);
            for (auto& t : dispatch_threads_) {
                if (t.joinable()) t.join();
            }
            dispatch_threads_.clear();
        }

        // Wait for orders to complete
        while (std::chrono::steady_clock::now() < deadline) {
            bool all_done = true;
            {
                std::shared_lock lock(orders_mutex_);
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
        std::shared_lock lock(orders_mutex_);
        auto it = orders_.find(order_id);
        if (it != orders_.end()) return it->second;
        return {};
    }

    std::vector<Order> get_all_orders() const {
        std::shared_lock lock(orders_mutex_);
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
            std::unique_lock lock(orders_mutex_);
            orders_.clear();
        }

        // Re-subscribe fills
        for (auto& [exch, gw] : gateways_) {
            gw->subscribe_fills([this](const Fill& fill) {
                on_fill(fill);
            });
        }
    }

private:
    ProcessResult process_single_leg(const Signal& signal) {
        // Create order
        Order order;
        order.order_id = UuidGenerator::order_id();
        order.signal = signal;
        order.status = OrderStatus::CREATED;
        order.created_at_ns = now_ns();

        // 3a. Conflict check
        auto conflict_result = conflict_resolver_.check(signal, gateways_);
        if (!conflict_result.can_proceed) {
            order.status = OrderStatus::REJECTED;
            order.reject_reason = conflict_result.reason;
            store_order(order);
            logger_->warn("Order {} REJECTED: {}", order.order_id, conflict_result.reason);
            return {false, order.order_id, conflict_result.reason};
        }

        // 3b. Portfolio check
        auto margin_result = portfolio_.check_and_reserve(signal);
        if (!margin_result.approved) {
            order.status = OrderStatus::REJECTED;
            order.reject_reason = margin_result.reason;
            store_order(order);
            logger_->warn("Order {} REJECTED: {}", order.order_id, margin_result.reason);
            return {false, order.order_id, margin_result.reason};
        }

        // 3c. Dispatch
        order.status = OrderStatus::SENT;
        order.sent_at_ns = now_ns();
        store_order(order);

        conflict_resolver_.register_order(order);

        auto exch = signal.exchange;
        auto gw_it = gateways_.find(exch);
        if (gw_it == gateways_.end()) {
            order.status = OrderStatus::REJECTED;
            order.reject_reason = "no gateway for exchange " + std::string(to_string(exch));
            store_order(order);
            portfolio_.release_margin(signal);
            return {false, order.order_id, order.reject_reason};
        }

        logger_->info("Order {} SENT to {} for {} {} {:.4f}",
                     order.order_id, to_string(exch), signal.symbol,
                     to_string(signal.side), signal.quantity);

        // Async dispatch
        auto order_id = order.order_id;
        auto* gw = gw_it->second;
        {
            std::lock_guard lock(threads_mutex_);
            dispatch_threads_.emplace_back([this, gw, order, order_id]() {
                auto result = gw->send_order(order).get();

                std::unique_lock lock(orders_mutex_);
                auto it = orders_.find(order_id);
                if (it != orders_.end()) {
                    it->second.exchange_order_id = result.exchange_order_id;
                    it->second.latency.send_to_ack_ns = result.latency_ns;

                    if (result.success) {
                        auto new_status = (get_exchange_type(order.signal.exchange) == ExchangeType::DEX)
                            ? OrderStatus::TX_PENDING : OrderStatus::ACKNOWLEDGED;
                        it->second.status = new_status;
                        if (result.tx_hash) {
                            it->second.tx_hash = result.tx_hash;
                        }
                        logger_->info("Order {} status -> {}", order_id, to_string(new_status));
                    } else {
                        it->second.status = OrderStatus::REJECTED;
                        it->second.reject_reason = result.error_message;
                        portfolio_.release_margin(order.signal);
                        conflict_resolver_.update_status(order_id, OrderStatus::REJECTED);
                    }
                }
            });
        }

        return {true, order_id, ""};
    }

    void on_fill(const Fill& fill) {
        // Check if this fill belongs to an arb pair

        bool is_reject = (fill.filled_qty == 0.0 && fill.fill_price == 0.0);

        // Try arb coordinator
        arb_coordinator_.on_fill(fill.exchange_order_id, fill.fill_price,
                                fill.filled_qty, is_reject);

        // Also update single-leg orders
        std::unique_lock lock(orders_mutex_);
        for (auto& [id, order] : orders_) {
            if (order.exchange_order_id == fill.exchange_order_id) {
                if (is_reject) {
                    order.status = OrderStatus::REJECTED;
                    portfolio_.release_margin(order.signal);
                } else {
                    order.status = OrderStatus::FILLED;
                    order.filled_quantity = fill.filled_qty;
                    order.avg_fill_price = fill.fill_price;
                    order.last_update_ns = fill.fill_timestamp_ns;
                    order.latency.total_ns = fill.fill_timestamp_ns - order.created_at_ns;

                    if (fill.tx_hash) {
                        order.tx_hash = fill.tx_hash;
                        order.tx_confirmed_ns = fill.fill_timestamp_ns;
                    }

                    portfolio_.on_fill(order.signal, fill.fill_price, fill.filled_qty);
                    conflict_resolver_.update_status(id, OrderStatus::FILLED);

                    logger_->info("Order {} FILLED: {:.4f} @ {:.2f}, latency={:.2f}ms",
                                 id, fill.filled_qty, fill.fill_price,
                                 order.latency.total_ns / 1e6);
                }
                break;
            }
        }
    }

    void store_order(const Order& order) {
        std::unique_lock lock(orders_mutex_);
        orders_[order.order_id] = order;
    }

    GatewayMap gateways_;
    SignalReceiver signal_receiver_;
    PortfolioGuard portfolio_;
    ConflictResolver conflict_resolver_;
    ArbCoordinator arb_coordinator_;
    std::shared_ptr<spdlog::logger> logger_;

    std::unordered_map<std::string, Order> orders_;
    mutable std::shared_mutex orders_mutex_;

    std::vector<std::thread> dispatch_threads_;
    std::mutex threads_mutex_;
};

} // namespace oem
