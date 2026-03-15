#pragma once
#include "signal.h"
#include "../util/latency_tracker.h"
#include <cstdint>
#include <optional>
#include <string>

namespace oem {

enum class OrderStatus {
    CREATED,
    VALIDATING,
    PENDING,
    SENT,
    TX_PENDING,      // DEX: tx broadcast, waiting for block
    TX_CONFIRMED,    // DEX: included in block
    ACKNOWLEDGED,    // CEX: exchange accepted
    PARTIAL_FILL,
    FILLED,
    CANCELLED,
    REJECTED,
    TIMEOUT,
    UNWINDING        // arb leg failure, reversing position
};

inline const char* to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::CREATED:      return "CREATED";
        case OrderStatus::VALIDATING:   return "VALIDATING";
        case OrderStatus::PENDING:      return "PENDING";
        case OrderStatus::SENT:         return "SENT";
        case OrderStatus::TX_PENDING:   return "TX_PENDING";
        case OrderStatus::TX_CONFIRMED: return "TX_CONFIRMED";
        case OrderStatus::ACKNOWLEDGED: return "ACKNOWLEDGED";
        case OrderStatus::PARTIAL_FILL: return "PARTIAL_FILL";
        case OrderStatus::FILLED:       return "FILLED";
        case OrderStatus::CANCELLED:    return "CANCELLED";
        case OrderStatus::REJECTED:     return "REJECTED";
        case OrderStatus::TIMEOUT:      return "TIMEOUT";
        case OrderStatus::UNWINDING:    return "UNWINDING";
    }
    return "UNKNOWN";
}

inline bool is_terminal(OrderStatus s) {
    return s == OrderStatus::FILLED || s == OrderStatus::CANCELLED ||
           s == OrderStatus::REJECTED || s == OrderStatus::TIMEOUT;
}

inline bool is_in_flight(OrderStatus s) {
    return s == OrderStatus::SENT || s == OrderStatus::TX_PENDING ||
           s == OrderStatus::ACKNOWLEDGED || s == OrderStatus::TX_CONFIRMED;
}

struct OrderResult {
    bool success = false;
    std::string exchange_order_id;
    OrderStatus status = OrderStatus::REJECTED;
    std::string error_message;
    int64_t latency_ns = 0;
    std::optional<std::string> tx_hash;
};

struct CancelResult {
    bool success = false;
    std::string error_message;
};

struct OrderStatusResult {
    OrderStatus status;
    double filled_quantity = 0.0;
    double avg_fill_price = 0.0;
};

struct Fill {
    std::string exchange_order_id;
    double filled_qty = 0.0;
    double fill_price = 0.0;
    int64_t fill_timestamp_ns = 0;
    bool is_final = false;
    std::optional<std::string> tx_hash;
};

struct Order {
    std::string     order_id;
    std::string     exchange_order_id;
    Signal          signal;
    OrderStatus     status = OrderStatus::CREATED;
    double          filled_quantity = 0.0;
    double          avg_fill_price = 0.0;
    int64_t         created_at_ns = 0;
    int64_t         sent_at_ns = 0;
    int64_t         last_update_ns = 0;
    std::string     reject_reason;

    // DEX-specific
    std::optional<std::string> tx_hash;
    std::optional<int64_t>     tx_confirmed_ns;

    LatencyMetrics latency;
};

} // namespace oem
