#pragma once
#include <chrono>
#include <cstdint>
#include <string>

namespace oem {

inline int64_t now_ns() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

struct LatencyMetrics {
    int64_t signal_to_validation_ns = 0;
    int64_t validation_to_send_ns = 0;
    int64_t send_to_ack_ns = 0;
    int64_t send_to_tx_confirm_ns = 0;
    int64_t ack_to_fill_ns = 0;
    int64_t total_ns = 0;
};

} // namespace oem
