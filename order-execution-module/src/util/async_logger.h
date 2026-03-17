#pragma once
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <thread>

namespace oem {

class AsyncLogger {
    static constexpr size_t SLOT_SIZE = 256;
    static constexpr size_t RING_CAP = 4096;

    struct Slot {
        char buf[SLOT_SIZE];
        std::atomic<bool> ready{false};
    };

    std::array<Slot, RING_CAP> ring_{};
    alignas(64) std::atomic<size_t> write_head_{0};
    alignas(64) std::atomic<size_t> read_head_{0};
    std::atomic<bool> running_{true};
    std::thread io_thread_;

    void io_loop() {
        while (running_.load(std::memory_order_relaxed) ||
               read_head_.load(std::memory_order_acquire) !=
               write_head_.load(std::memory_order_acquire)) {
            size_t r = read_head_.load(std::memory_order_acquire);
            size_t w = write_head_.load(std::memory_order_acquire);
            if (r == w) {
#if defined(__x86_64__)
                for (int i = 0; i < 32; ++i) __builtin_ia32_pause();
#elif defined(__aarch64__)
                asm volatile("yield");
#else
                std::this_thread::yield();
#endif
                continue;
            }
            auto& slot = ring_[r % RING_CAP];
            if (slot.ready.load(std::memory_order_acquire)) {
                std::fwrite(slot.buf, 1, std::strlen(slot.buf), stdout);
                slot.ready.store(false, std::memory_order_release);
                read_head_.fetch_add(1, std::memory_order_release);
            }
        }
        std::fflush(stdout);
    }

    void push_impl(const char* level, const char* component, const char* fmt, va_list args) {
        size_t idx = write_head_.fetch_add(1, std::memory_order_acq_rel);
        auto& slot = ring_[idx % RING_CAP];
        while (slot.ready.load(std::memory_order_acquire)) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#else
            std::this_thread::yield();
#endif
        }
        int offset = std::snprintf(slot.buf, 48, "[%s] [%s] ", level, component);
        if (offset < 0) offset = 0;
        std::vsnprintf(slot.buf + offset, SLOT_SIZE - offset, fmt, args);
        size_t len = std::strlen(slot.buf);
        if (len < SLOT_SIZE - 1) { slot.buf[len] = '\n'; slot.buf[len+1] = '\0'; }
        slot.ready.store(true, std::memory_order_release);
    }

public:
    AsyncLogger() : io_thread_([this]{ io_loop(); }) {}

    ~AsyncLogger() {
        running_.store(false, std::memory_order_release);
        if (io_thread_.joinable()) io_thread_.join();
    }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void info(const char* component, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        push_impl("info", component, fmt, args);
        va_end(args);
    }

    void warn(const char* component, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        push_impl("warn", component, fmt, args);
        va_end(args);
    }

    void error(const char* component, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        push_impl("error", component, fmt, args);
        va_end(args);
    }
};

inline AsyncLogger& global_logger() {
    static AsyncLogger instance;
    return instance;
}

} // namespace oem
