#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>

namespace oem {

inline std::shared_ptr<spdlog::logger> get_logger(const std::string& name) {
    auto logger = spdlog::get(name);
    if (!logger) {
        logger = spdlog::stdout_color_mt(name);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
    }
    return logger;
}

} // namespace oem
