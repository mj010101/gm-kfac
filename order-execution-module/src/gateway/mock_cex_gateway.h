#pragma once
#include "i_cex_gateway.h"
#include "../util/uuid.h"
#include "../util/logger.h"
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
        , logger_(get_logger("cex_gateway"))
        , rng_(std::random_device{}()) {}

    ~MockCexGateway() override {
        // Wait for any pending fill threads
        std::lock_guard lock(threads_mutex_);
        for (auto& t : pending_threads_) {
            if (t.joinable()) t.join();
        }
    }

    Exchange exchange() const override { return config_.exchange_id; }

    std::future<OrderResult> send_order(const Order& order) override {
        return std::async(std::launch::async, [this, order]() -> OrderResult {
            auto start = now_ns();

            // Simulate network + matching latency
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.fill_latency_ms));

            // Check reject
            {
                std::lock_guard lock(rng_mutex_);
                std::uniform_real_distribution<> dist(0.0, 1.0);
                if (dist(rng_) < config_.reject_probability) {
                    logger_->warn("Order {} REJECTED by {}", order.order_id,
                                  to_string(config_.exchange_id));
                    return OrderResult{
                        .success = false,
                        .exchange_order_id = "",
                        .status = OrderStatus::REJECTED,
                        .error_message = "mock_reject",
                        .latency_ns = now_ns() - start
                    };
                }
            }

            auto exch_id = "EX-" + order.order_id;

            logger_->info("Order {} ACKNOWLEDGED by {} as {}",
                         order.order_id, to_string(config_.exchange_id), exch_id);

            // Schedule fill callback
            schedule_fill(order, exch_id);

            return OrderResult{
                .success = true,
                .exchange_order_id = exch_id,
                .status = OrderStatus::ACKNOWLEDGED,
                .error_message = "",
                .latency_ns = now_ns() - start
            };
        });
    }

    std::future<CancelResult> cancel_order(const std::string& exchange_order_id) override {
        return std::async(std::launch::async, [this, exchange_order_id]() -> CancelResult {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.fill_latency_ms / 2));

            {
                std::lock_guard lock(cancelled_mutex_);
                cancelled_orders_.insert(exchange_order_id);
            }

            logger_->info("Order {} CANCELLED on {}", exchange_order_id,
                         to_string(config_.exchange_id));
            return CancelResult{.success = true};
        });
    }

    std::future<OrderStatusResult> query_order(const std::string& /*exchange_order_id*/) override {
        return std::async(std::launch::async, []() -> OrderStatusResult {
            return OrderStatusResult{.status = OrderStatus::FILLED};
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
            // Small delay to simulate WS fill feed
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            // Check if cancelled before fill
            {
                std::lock_guard lock2(cancelled_mutex_);
                if (cancelled_orders_.count(exch_id)) {
                    logger_->info("Order {} was cancelled, no fill", exch_id);
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

            Fill fill{
                .exchange_order_id = exch_id,
                .filled_qty = order.signal.quantity,
                .fill_price = fill_price,
                .fill_timestamp_ns = now_ns(),
                .is_final = true
            };

            logger_->info("Order {} FILLED on {} at price {:.2f}",
                         exch_id, to_string(config_.exchange_id), fill_price);

            std::lock_guard lock2(callback_mutex_);
            if (fill_callback_) {
                fill_callback_(fill);
            }
        });
    }

    Config config_;
    std::shared_ptr<spdlog::logger> logger_;
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
