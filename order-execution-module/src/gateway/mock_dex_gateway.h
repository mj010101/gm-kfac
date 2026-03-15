#pragma once
#include "i_dex_gateway.h"
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

class MockDexGateway : public IDexGateway {
public:
    struct Config {
        int block_time_ms = 1000;
        int signing_latency_ms = 5;
        double fill_probability = 1.0;
        double reject_probability = 0.0;
        double slippage_bps = 0.0;
    };

    MockDexGateway() : config_(), logger_(get_logger("dex_gateway")), rng_(std::random_device{}()) {}
    explicit MockDexGateway(Config config)
        : config_(config)
        , logger_(get_logger("dex_gateway"))
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

            // Simulate EIP-712 signing
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.signing_latency_ms));

            auto tx_hash = UuidGenerator::tx_hash();

            logger_->info("Order {} TX_PENDING on HYPERLIQUID, tx_hash={}, nonce={}",
                         order.order_id, tx_hash, nonce);

            // Schedule block confirmation
            schedule_block_confirm(order, tx_hash);

            return OrderResult{
                .success = true,
                .exchange_order_id = "HL-" + order.order_id,
                .status = OrderStatus::TX_PENDING,
                .error_message = "",
                .latency_ns = now_ns() - start,
                .tx_hash = tx_hash
            };
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

            logger_->info("Order {} CANCELLED on HYPERLIQUID", exchange_order_id);
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
    void schedule_block_confirm(const Order& order, const std::string& tx_hash) {
        std::lock_guard lock(threads_mutex_);
        pending_threads_.emplace_back([this, order, tx_hash]() {
            // Simulate block confirmation delay
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.block_time_ms));

            auto exch_id = "HL-" + order.order_id;

            // Check if cancelled
            {
                std::lock_guard lock2(cancelled_mutex_);
                if (cancelled_orders_.count(exch_id)) {
                    logger_->info("Order {} was cancelled, no confirm", exch_id);
                    return;
                }
            }

            // Check reject
            {
                std::lock_guard lock2(rng_mutex_);
                std::uniform_real_distribution<> dist(0.0, 1.0);
                if (dist(rng_) < config_.reject_probability) {
                    logger_->warn("Order {} REJECTED during block confirm on HYPERLIQUID",
                                 order.order_id);
                    // Send a reject fill
                    Fill reject_fill{
                        .exchange_order_id = exch_id,
                        .filled_qty = 0.0,
                        .fill_price = 0.0,
                        .fill_timestamp_ns = now_ns(),
                        .is_final = true,
                        .tx_hash = tx_hash
                    };
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

            logger_->info("Order {} TX_CONFIRMED and FILLED on HYPERLIQUID at price {:.2f}, tx_hash={}",
                         order.order_id, fill_price, tx_hash);

            Fill fill{
                .exchange_order_id = exch_id,
                .filled_qty = order.signal.quantity,
                .fill_price = fill_price,
                .fill_timestamp_ns = now_ns(),
                .is_final = true,
                .tx_hash = tx_hash
            };

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
    std::atomic<uint64_t> nonce_{0};
    std::function<void(const Fill&)> fill_callback_;
    std::mutex callback_mutex_;
    std::set<std::string> cancelled_orders_;
    std::mutex cancelled_mutex_;
    std::vector<std::thread> pending_threads_;
    std::mutex threads_mutex_;
};

} // namespace oem
