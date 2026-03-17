#pragma once
#include <cstdio>
#include <string>

namespace oem {

struct PosKey {
    char buf[32] = {};

    static PosKey make(const char* exchange, const char* symbol) {
        PosKey k;
        std::snprintf(k.buf, sizeof(k.buf), "%s_%s", exchange, symbol);
        return k;
    }

    const char* c_str() const { return buf; }
    std::string to_string() const { return std::string(buf); }
};

} // namespace oem
