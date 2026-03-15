#pragma once
#include <string>
#include <unordered_map>

namespace oem {

struct PortfolioState {
    double cash = 100000.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;

    std::unordered_map<std::string, double> positions;
    std::unordered_map<std::string, double> margin_used;
    std::unordered_map<std::string, double> margin_reserved;

    double total_value() const {
        return cash + unrealized_pnl + realized_pnl;
    }

    double available_margin() const {
        double reserved = 0.0, used = 0.0;
        for (auto& [_, v] : margin_reserved) reserved += v;
        for (auto& [_, v] : margin_used) used += v;
        return cash - reserved - used;
    }

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
