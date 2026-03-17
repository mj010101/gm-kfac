#pragma once
#include "i_cex_gateway.h"
#include "../util/uuid.h"
#include "../util/async_logger.h"
#include "../util/latency_tracker.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace oem {

class MockCexGateway : public ICexGateway {
public:
    struct Config {
        Exchange exchange_id = Exchange::BINANCE;
        int fill_latency_ms = 50;
        double fill_probability = 1.0;
        double reject_probability = 0.0;
        double slippage_bps = 0.0;
    };

    explicit MockCexGateway(Config config)
        : config_(config)
        , rng_(std::random_device{}()) {}

    ~MockCexGateway() override {
        std::lock_guard lock(threads_mutex_);
        for (auto& t : pending_threads_) {
            if (t.joinable()) t.join();
        }
    }

    Exchange exchange() const override { return config_.exchange_id; }

    std::future<OrderResult> send_order(const Order& order) override {
        return std::async(std::launch::async, [this, order]() -> OrderResult {
            auto start = now_ns();

            std::this_thread::sleep_for(std::chrono::milliseconds(config_.fill_latency_ms));

            {
                std::lock_guard lock(rng_mutex_);
                std::uniform_real_distribution<> dist(0.0, 1.0);
                if (dist(rng_) < config_.reject_probability) {
                    global_logger().warn("cex_gateway", "Order %s REJECTED by %s",
                                         order.order_id, to_string(config_.exchange_id));
                    OrderResult r;
                    r.success = false;
                    r.status = OrderStatus::REJECTED;
                    r.set_error_message("mock_reject");
                    r.latency_ns = now_ns() - start;
                    return r;
                }
            }

            std::string exch_id = std::string("EX-") + order.order_id;

            global_logger().info("cex_gateway", "Order %s ACKNOWLEDGED by %s as %s",
                                 order.order_id, to_string(config_.exchange_id), exch_id.c_str());

            schedule_fill(order, exch_id);

            OrderResult r;
            r.success = true;
            r.set_exchange_order_id(exch_id);
            r.status = OrderStatus::ACKNOWLEDGED;
            r.latency_ns = now_ns() - start;
            return r;
        });
    }

    std::future<CancelResult> cancel_order(const std::string& exchange_order_id) override {
        return std::async(std::launch::async, [this, exchange_order_id]() -> CancelResult {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.fill_latency_ms / 2));

            {
                std::lock_guard lock(cancelled_mutex_);
                cancelled_orders_.insert(exchange_order_id);
            }

            global_logger().info("cex_gateway", "Order %s CANCELLED on %s",
                                 exchange_order_id.c_str(), to_string(config_.exchange_id));
            CancelResult r;
            r.success = true;
            return r;
        });
    }

    std::future<OrderStatusResult> query_order(const std::string& /*exchange_order_id*/) override {
        return std::async(std::launch::async, []() -> OrderStatusResult {
            OrderStatusResult r;
            r.status = OrderStatus::FILLED;
            return r;
        });
    }

    void subscribe_fills(std::function<void(const Fill&)> callback) override {
        std::lock_guard lock(callback_mutex_);
        fill_callback_ = callback;
    }

    bool is_connected() const override { return true; }

    void set_config(const Config& config) { config_ = config; }

private:
    void schedule_fill(const Order& order, const std::string& exch_id) {
        std::lock_guard lock(threads_mutex_);
        pending_threads_.emplace_back([this, order, exch_id]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            {
                std::lock_guard lock2(cancelled_mutex_);
                if (cancelled_orders_.count(exch_id)) {
                    global_logger().info("cex_gateway", "Order %s was cancelled, no fill", exch_id.c_str());
                    return;
                }
            }

            double fill_price = order.signal.price;
            if (fill_price > 0 && config_.slippage_bps > 0) {
                double slip = config_.slippage_bps / 10000.0;
                if (order.signal.side == Side::BUY) {
                    fill_price *= (1.0 + slip);
                } else {
                    fill_price *= (1.0 - slip);
                }
            }

            Fill fill;
            fill.set_exchange_order_id(exch_id);
            fill.filled_qty = order.signal.quantity;
            fill.fill_price = fill_price;
            fill.fill_timestamp_ns = now_ns();
            fill.is_final = true;

            global_logger().info("cex_gateway", "Order %s FILLED on %s at price %.2f",
                                 exch_id.c_str(), to_string(config_.exchange_id), fill_price);

            std::lock_guard lock2(callback_mutex_);
            if (fill_callback_) {
                fill_callback_(fill);
            }
        });
    }

    Config config_;
    std::mt19937 rng_;
    std::mutex rng_mutex_;
    std::function<void(const Fill&)> fill_callback_;
    std::mutex callback_mutex_;
    std::set<std::string> cancelled_orders_;
    std::mutex cancelled_mutex_;
    std::vector<std::thread> pending_threads_;
    std::mutex threads_mutex_;
};

} // namespace oem
