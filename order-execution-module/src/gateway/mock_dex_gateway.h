#pragma once
#include "i_dex_gateway.h"
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

class MockDexGateway : public IDexGateway {
public:
    struct Config {
        int block_time_ms = 1000;
        int signing_latency_ms = 5;
        double fill_probability = 1.0;
        double reject_probability = 0.0;
        double slippage_bps = 0.0;
    };

    MockDexGateway() : config_(), rng_(std::random_device{}()) {}
    explicit MockDexGateway(Config config)
        : config_(config)
        , rng_(std::random_device{}()) {}

    ~MockDexGateway() override {
        std::lock_guard lock(threads_mutex_);
        for (auto& t : pending_threads_) {
            if (t.joinable()) t.join();
        }
    }

    Exchange exchange() const override { return Exchange::HYPERLIQUID; }

    int64_t estimated_block_time_ms() const override { return config_.block_time_ms; }

    uint64_t next_nonce() override { return nonce_.fetch_add(1); }

    int64_t signing_latency_ns() const override {
        return config_.signing_latency_ms * 1000000LL;
    }

    std::future<OrderResult> send_order(const Order& order) override {
        return std::async(std::launch::async, [this, order]() -> OrderResult {
            auto start = now_ns();
            auto nonce = next_nonce();

            std::this_thread::sleep_for(std::chrono::milliseconds(config_.signing_latency_ms));

            auto tx_hash_str = UuidGenerator::tx_hash();

            global_logger().info("dex_gateway", "Order %s TX_PENDING on HYPERLIQUID, tx_hash=%s, nonce=%llu",
                                 order.order_id, tx_hash_str.c_str(), (unsigned long long)nonce);

            schedule_block_confirm(order, tx_hash_str);

            OrderResult r;
            r.success = true;
            std::string hl_id = std::string("HL-") + order.order_id;
            r.set_exchange_order_id(hl_id);
            r.status = OrderStatus::TX_PENDING;
            r.latency_ns = now_ns() - start;
            r.set_tx_hash(tx_hash_str);
            return r;
        });
    }

    std::future<CancelResult> cancel_order(const std::string& exchange_order_id) override {
        return std::async(std::launch::async, [this, exchange_order_id]() -> CancelResult {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                config_.signing_latency_ms + config_.block_time_ms / 2));

            {
                std::lock_guard lock(cancelled_mutex_);
                cancelled_orders_.insert(exchange_order_id);
            }

            global_logger().info("dex_gateway", "Order %s CANCELLED on HYPERLIQUID",
                                 exchange_order_id.c_str());
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
    void schedule_block_confirm(const Order& order, const std::string& tx_hash_str) {
        std::lock_guard lock(threads_mutex_);
        pending_threads_.emplace_back([this, order, tx_hash_str]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.block_time_ms));

            std::string exch_id = std::string("HL-") + order.order_id;

            {
                std::lock_guard lock2(cancelled_mutex_);
                if (cancelled_orders_.count(exch_id)) {
                    global_logger().info("dex_gateway", "Order %s was cancelled, no confirm", exch_id.c_str());
                    return;
                }
            }

            {
                std::lock_guard lock2(rng_mutex_);
                std::uniform_real_distribution<> dist(0.0, 1.0);
                if (dist(rng_) < config_.reject_probability) {
                    global_logger().warn("dex_gateway", "Order %s REJECTED during block confirm on HYPERLIQUID",
                                         order.order_id);
                    Fill reject_fill;
                    reject_fill.set_exchange_order_id(exch_id);
                    reject_fill.filled_qty = 0.0;
                    reject_fill.fill_price = 0.0;
                    reject_fill.fill_timestamp_ns = now_ns();
                    reject_fill.is_final = true;
                    reject_fill.set_tx_hash(tx_hash_str);
                    std::lock_guard lock3(callback_mutex_);
                    if (fill_callback_) {
                        fill_callback_(reject_fill);
                    }
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

            global_logger().info("dex_gateway", "Order %s TX_CONFIRMED and FILLED on HYPERLIQUID at price %.2f, tx_hash=%s",
                                 order.order_id, fill_price, tx_hash_str.c_str());

            Fill fill;
            fill.set_exchange_order_id(exch_id);
            fill.filled_qty = order.signal.quantity;
            fill.fill_price = fill_price;
            fill.fill_timestamp_ns = now_ns();
            fill.is_final = true;
            fill.set_tx_hash(tx_hash_str);

            std::lock_guard lock2(callback_mutex_);
            if (fill_callback_) {
                fill_callback_(fill);
            }
        });
    }

    Config config_;
    std::mt19937 rng_;
    std::mutex rng_mutex_;
    std::atomic<uint64_t> nonce_{0};
    std::function<void(const Fill&)> fill_callback_;
    std::mutex callback_mutex_;
    std::set<std::string> cancelled_orders_;
    std::mutex cancelled_mutex_;
    std::vector<std::thread> pending_threads_;
    std::mutex threads_mutex_;
};

} // namespace oem
