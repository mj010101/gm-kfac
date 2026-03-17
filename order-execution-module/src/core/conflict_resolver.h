#pragma once
#include "../model/order.h"
#include "../gateway/i_exchange_gateway.h"
#include "../util/async_logger.h"
#include "../util/spinlock.h"
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
    char reason[128] = {};
    std::vector<std::string> cancelled_order_ids;

    void set_reason(const std::string& r) {
        std::strncpy(reason, r.c_str(), 127);
        reason[127] = '\0';
    }
};

class ConflictResolver {
public:
    ConflictResolver() : strategy_(ConflictStrategy::CANCEL_AND_REPLACE) {}
    explicit ConflictResolver(ConflictStrategy strategy) : strategy_(strategy) {}

    void register_order(const Order& order) {
        SpinLock::Guard lock(mutex_);
        InFlightEntry entry;
        std::strncpy(entry.order_id, order.order_id, 15);
        entry.order_id[15] = '\0';
        std::strncpy(entry.exchange_order_id, order.exchange_order_id, 31);
        entry.exchange_order_id[31] = '\0';
        entry.side = order.signal.side;
        entry.exchange = order.signal.exchange;
        entry.status = order.status;
        in_flight_[order.signal.symbol].push_back(entry);
        global_logger().info("conflict_resolver", "Registered in-flight order %s for %s",
                             order.order_id, order.signal.symbol);
    }

    ConflictResult check(const Signal& signal,
                         std::unordered_map<Exchange, IExchangeGateway*>& gateways) {
        SpinLock::Guard lock(mutex_);

        auto it = in_flight_.find(signal.symbol);
        if (it == in_flight_.end() || it->second.empty()) {
            return {};
        }

        std::vector<InFlightEntry*> conflicts;
        for (auto& entry : it->second) {
            if (entry.side != signal.side && is_active(entry.status)) {
                conflicts.push_back(&entry);
            }
        }

        if (conflicts.empty()) {
            return {};
        }

        global_logger().warn("conflict_resolver", "Conflict detected: %s has %zu opposite-side in-flight order(s), new signal %s",
                             signal.symbol, conflicts.size(), signal.signal_id);

        if (strategy_ == ConflictStrategy::REJECT_NEW) {
            ConflictResult r;
            r.can_proceed = false;
            std::string reason = "conflict: opposite-side order in-flight for " + std::string(signal.symbol);
            r.set_reason(reason);
            return r;
        }

        // CANCEL_AND_REPLACE
        ConflictResult result;
        result.can_proceed = true;
        for (auto* conflict : conflicts) {
            if (conflict->exchange_order_id[0] == '\0') continue;

            auto gw_it = gateways.find(conflict->exchange);
            if (gw_it == gateways.end()) continue;

            global_logger().info("conflict_resolver", "Cancelling conflicting order %s on %s",
                                 conflict->order_id, to_string(conflict->exchange));

            auto cancel_future = gw_it->second->cancel_order(conflict->exchange_order_id);

            auto status = cancel_future.wait_for(std::chrono::milliseconds(500));
            if (status == std::future_status::ready) {
                auto cancel_result = cancel_future.get();
                if (cancel_result.success) {
                    conflict->status = OrderStatus::CANCELLED;
                    result.cancelled_order_ids.push_back(conflict->order_id);
                    global_logger().info("conflict_resolver", "Cancel confirmed for %s", conflict->order_id);
                } else {
                    result.can_proceed = false;
                    std::string reason = "conflict: cancel of " + std::string(conflict->order_id)
                        + " failed, rejecting new signal";
                    result.set_reason(reason);
                    global_logger().warn("conflict_resolver", "%s", result.reason);
                    return result;
                }
            } else {
                result.can_proceed = false;
                std::string reason = "conflict: cancel of " + std::string(conflict->order_id)
                    + " timed out, rejecting new signal";
                result.set_reason(reason);
                global_logger().warn("conflict_resolver", "%s", result.reason);
                return result;
            }
        }

        return result;
    }

    void update_status(const std::string& order_id, OrderStatus status) {
        SpinLock::Guard lock(mutex_);
        for (auto& [symbol, entries] : in_flight_) {
            for (auto& entry : entries) {
                if (std::strcmp(entry.order_id, order_id.c_str()) == 0) {
                    entry.status = status;
                    if (is_terminal(status)) {
                        global_logger().info("conflict_resolver", "Removing terminal order %s from tracking",
                                             order_id.c_str());
                    }
                    return;
                }
            }
        }
    }

    void cleanup() {
        SpinLock::Guard lock(mutex_);
        for (auto& [symbol, entries] : in_flight_) {
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                    [](const InFlightEntry& e) { return is_terminal(e.status); }),
                entries.end()
            );
        }
    }

    void reset() {
        SpinLock::Guard lock(mutex_);
        in_flight_.clear();
    }

private:
    struct InFlightEntry {
        char order_id[16] = {};
        char exchange_order_id[32] = {};
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
    SpinLock mutex_;
};

} // namespace oem
