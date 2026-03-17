#pragma once
#include "../util/flat_hash_map.h"
#include <string>

namespace oem {

struct PortfolioState {
    double cash = 100000.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;

    FlatHashMap<double, 128> positions;
    FlatHashMap<double, 128> margin_used;
    FlatHashMap<double, 128> margin_reserved;

    double total_value() const {
        return cash + unrealized_pnl + realized_pnl;
    }

    double available_margin() const {
        double reserved = 0.0, used = 0.0;
        margin_reserved.for_each([&](const std::string&, double v) { reserved += v; });
        margin_used.for_each([&](const std::string&, double v) { used += v; });
        return cash - reserved - used;
    }

    size_t margin_used_size() const { return margin_used.size(); }

    void reset(double initial_cash = 100000.0) {
        cash = initial_cash;
        unrealized_pnl = 0.0;
        realized_pnl = 0.0;
        positions.clear();
        margin_used.clear();
        margin_reserved.clear();
    }
};

} // namespace oem
