#pragma once
#include "signal.h"
#include "../util/latency_tracker.h"
#include <cstdint>
#include <cstring>
#include <string>

namespace oem {

enum class OrderStatus {
    CREATED,
    VALIDATING,
    PENDING,
    SENT,
    TX_PENDING,
    TX_CONFIRMED,
    ACKNOWLEDGED,
    PARTIAL_FILL,
    FILLED,
    CANCELLED,
    REJECTED,
    TIMEOUT,
    UNWINDING
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
    char exchange_order_id[32] = {};
    OrderStatus status = OrderStatus::REJECTED;
    char error_message[64] = {};
    int64_t latency_ns = 0;
    char tx_hash[32] = {};
    bool has_tx_hash = false;

    void set_exchange_order_id(const std::string& id) {
        std::strncpy(exchange_order_id, id.c_str(), 31);
        exchange_order_id[31] = '\0';
    }
    void set_error_message(const std::string& msg) {
        std::strncpy(error_message, msg.c_str(), 63);
        error_message[63] = '\0';
    }
    void set_tx_hash(const std::string& h) {
        std::strncpy(tx_hash, h.c_str(), 31);
        tx_hash[31] = '\0';
        has_tx_hash = true;
    }
};

struct CancelResult {
    bool success = false;
    char error_message[64] = {};
};

struct OrderStatusResult {
    OrderStatus status;
    double filled_quantity = 0.0;
    double avg_fill_price = 0.0;
};

struct Fill {
    char exchange_order_id[32] = {};
    double filled_qty = 0.0;
    double fill_price = 0.0;
    int64_t fill_timestamp_ns = 0;
    bool is_final = false;
    char tx_hash[32] = {};
    bool has_tx_hash = false;

    void set_exchange_order_id(const std::string& id) {
        std::strncpy(exchange_order_id, id.c_str(), 31);
        exchange_order_id[31] = '\0';
    }
    void set_tx_hash(const std::string& h) {
        std::strncpy(tx_hash, h.c_str(), 31);
        tx_hash[31] = '\0';
        has_tx_hash = true;
    }
};

struct alignas(64) Order {
    char            order_id[16] = {};
    char            exchange_order_id[32] = {};
    Signal          signal;
    OrderStatus     status = OrderStatus::CREATED;
    double          filled_quantity = 0.0;
    double          avg_fill_price = 0.0;
    int64_t         created_at_ns = 0;
    int64_t         sent_at_ns = 0;
    int64_t         last_update_ns = 0;
    char            reject_reason[64] = {};

    // DEX-specific
    char            tx_hash[32] = {};
    bool            has_tx_hash = false;
    int64_t         tx_confirmed_ns = 0;
    bool            has_tx_confirmed_ns = false;

    LatencyMetrics  latency;

    void set_order_id(const std::string& id) {
        std::strncpy(order_id, id.c_str(), 15);
        order_id[15] = '\0';
    }
    void set_exchange_order_id(const std::string& id) {
        std::strncpy(exchange_order_id, id.c_str(), 31);
        exchange_order_id[31] = '\0';
    }
    void set_reject_reason(const std::string& r) {
        std::strncpy(reject_reason, r.c_str(), 63);
        reject_reason[63] = '\0';
    }
    void set_tx_hash(const std::string& h) {
        std::strncpy(tx_hash, h.c_str(), 31);
        tx_hash[31] = '\0';
        has_tx_hash = true;
    }
};

} // namespace oem
