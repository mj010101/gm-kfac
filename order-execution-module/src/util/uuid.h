#pragma once
#include <atomic>
#include <iomanip>
#include <sstream>
#include <string>

namespace oem {

class UuidGenerator {
public:
    static std::string order_id() {
        auto n = order_counter_.fetch_add(1);
        std::ostringstream oss;
        oss << "ORD-" << std::setw(6) << std::setfill('0') << n;
        return oss.str();
    }

    static std::string signal_id() {
        auto n = signal_counter_.fetch_add(1);
        std::ostringstream oss;
        oss << "SIG-" << std::setw(6) << std::setfill('0') << n;
        return oss.str();
    }

    static std::string tx_hash() {
        auto n = tx_counter_.fetch_add(1);
        std::ostringstream oss;
        oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << n;
        return oss.str();
    }

    static void reset() {
        order_counter_.store(1);
        signal_counter_.store(1);
        tx_counter_.store(1);
    }

private:
    static inline std::atomic<uint64_t> order_counter_{1};
    static inline std::atomic<uint64_t> signal_counter_{1};
    static inline std::atomic<uint64_t> tx_counter_{1};
};

} // namespace oem
