#pragma once
#include "../model/order.h"
#include "../gateway/i_exchange_gateway.h"
#include "../util/logger.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace oem {

enum class ConflictStrategy {
    CANCEL_AND_REPLACE,
    REJECT_NEW,
    QUEUE
};

struct ConflictResult {
    bool can_proceed = true;
    std::string reason;
    std::vector<std::string> cancelled_order_ids;
};

class ConflictResolver {
public:
    ConflictResolver() : strategy_(ConflictStrategy::CANCEL_AND_REPLACE), logger_(get_logger("conflict_resolver")) {}
    explicit ConflictResolver(ConflictStrategy strategy)
        : strategy_(strategy)
        , logger_(get_logger("conflict_resolver")) {}

    // Register an in-flight order
    void register_order(const Order& order) {
        std::lock_guard lock(mutex_);
        in_flight_[order.signal.symbol].push_back({
            .order_id = order.order_id,
            .exchange_order_id = order.exchange_order_id,
            .side = order.signal.side,
            .exchange = order.signal.exchange,
            .status = order.status
        });
        logger_->info("Registered in-flight order {} for {}", order.order_id, order.signal.symbol);
    }

    // Check for conflicts before sending a new signal
    ConflictResult check(const Signal& signal,
                         std::unordered_map<Exchange, IExchangeGateway*>& gateways) {
        std::lock_guard lock(mutex_);

        auto it = in_flight_.find(signal.symbol);
        if (it == in_flight_.end() || it->second.empty()) {
            return {true, ""};
        }

        // Find opposite-side in-flight orders
        std::vector<InFlightEntry*> conflicts;
        for (auto& entry : it->second) {
            if (entry.side != signal.side && is_active(entry.status)) {
                conflicts.push_back(&entry);
            }
        }

        if (conflicts.empty()) {
            return {true, ""};
        }

        logger_->warn("Conflict detected: {} has {} opposite-side in-flight order(s), new signal {}",
                      signal.symbol, conflicts.size(), signal.signal_id);

        if (strategy_ == ConflictStrategy::REJECT_NEW) {
            return {false, "conflict: opposite-side order in-flight for " + signal.symbol};
        }

        // CANCEL_AND_REPLACE
        ConflictResult result{true, ""};
        for (auto* conflict : conflicts) {
            if (conflict->exchange_order_id.empty()) continue;

            auto gw_it = gateways.find(conflict->exchange);
            if (gw_it == gateways.end()) continue;

            logger_->info("Cancelling conflicting order {} on {}",
                         conflict->order_id, to_string(conflict->exchange));

            auto cancel_future = gw_it->second->cancel_order(conflict->exchange_order_id);

            // Wait with timeout (500ms)
            auto status = cancel_future.wait_for(std::chrono::milliseconds(500));
            if (status == std::future_status::ready) {
                auto cancel_result = cancel_future.get();
                if (cancel_result.success) {
                    conflict->status = OrderStatus::CANCELLED;
                    result.cancelled_order_ids.push_back(conflict->order_id);
                    logger_->info("Cancel confirmed for {}", conflict->order_id);
                } else {
                    result.can_proceed = false;
                    result.reason = "conflict: cancel of " + conflict->order_id
                        + " failed, rejecting new signal";
                    logger_->warn("{}", result.reason);
                    return result;
                }
            } else {
                result.can_proceed = false;
                result.reason = "conflict: cancel of " + conflict->order_id
                    + " timed out, rejecting new signal";
                logger_->warn("{}", result.reason);
                return result;
            }
        }

        return result;
    }

    // Update order status (call on terminal states)
    void update_status(const std::string& order_id, OrderStatus status) {
        std::lock_guard lock(mutex_);
        for (auto& [symbol, entries] : in_flight_) {
            for (auto& entry : entries) {
                if (entry.order_id == order_id) {
                    entry.status = status;
                    if (is_terminal(status)) {
                        logger_->info("Removing terminal order {} from tracking", order_id);
                    }
                    return;
                }
            }
        }
    }

    // Clean up terminal orders
    void cleanup() {
        std::lock_guard lock(mutex_);
        for (auto& [symbol, entries] : in_flight_) {
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                    [](const InFlightEntry& e) { return is_terminal(e.status); }),
                entries.end()
            );
        }
    }

    void reset() {
        std::lock_guard lock(mutex_);
        in_flight_.clear();
    }

private:
    struct InFlightEntry {
        std::string order_id;
        std::string exchange_order_id;
        Side side;
        Exchange exchange;
        OrderStatus status;
    };

    static bool is_active(OrderStatus s) {
        return s == OrderStatus::SENT || s == OrderStatus::TX_PENDING ||
               s == OrderStatus::ACKNOWLEDGED || s == OrderStatus::TX_CONFIRMED;
    }

    ConflictStrategy strategy_;
    std::unordered_map<std::string, std::vector<InFlightEntry>> in_flight_;
    std::mutex mutex_;
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace oem
