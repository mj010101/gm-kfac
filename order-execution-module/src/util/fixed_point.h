#pragma once
#include <cstdint>

namespace oem {

constexpr int64_t SCALE = 100'000'000LL;

inline int64_t to_ticks(double value) {
    return static_cast<int64_t>(value * SCALE);
}

inline double to_double(int64_t ticks) {
    return static_cast<double>(ticks) / SCALE;
}

inline int64_t notional_ticks(int64_t price_ticks, int64_t qty_ticks) {
    return (price_ticks / SCALE) * qty_ticks;
}

inline int64_t margin_ticks(int64_t price_ticks, int64_t qty_ticks, int leverage) {
    return notional_ticks(price_ticks, qty_ticks) / leverage;
}

} // namespace oem
