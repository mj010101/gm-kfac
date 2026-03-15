#pragma once
#include "order.h"
#include <cstdint>
#include <optional>
#include <string>

namespace oem {

enum class ArbPairStatus {
    ASSEMBLING,
    DISPATCHED,
    CEX_FILLED,
    DEX_FILLED,
    ALL_FILLED,
    UNWINDING,
    FAILED,
    COMPLETED
};

inline const char* to_string(ArbPairStatus s) {
    switch (s) {
        case ArbPairStatus::ASSEMBLING:  return "ASSEMBLING";
        case ArbPairStatus::DISPATCHED:  return "DISPATCHED";
        case ArbPairStatus::CEX_FILLED:  return "CEX_FILLED";
        case ArbPairStatus::DEX_FILLED:  return "DEX_FILLED";
        case ArbPairStatus::ALL_FILLED:  return "ALL_FILLED";
        case ArbPairStatus::UNWINDING:   return "UNWINDING";
        case ArbPairStatus::FAILED:      return "FAILED";
        case ArbPairStatus::COMPLETED:   return "COMPLETED";
    }
    return "UNKNOWN";
}

struct ArbPair {
    std::string     group_id;
    std::optional<Order> cex_leg;
    std::optional<Order> dex_leg;
    ArbPairStatus   status = ArbPairStatus::ASSEMBLING;
    double          expected_spread_bps = 0.0;
    double          realized_spread_bps = 0.0;
    int64_t         created_at_ns = 0;
    int64_t         cex_fill_ns = 0;
    int64_t         dex_confirm_ns = 0;
    int64_t         risk_window_ns = 0;
    int64_t         dispatch_deadline_ns = 0;

    bool both_legs_present() const {
        return cex_leg.has_value() && dex_leg.has_value();
    }
};

} // namespace oem
