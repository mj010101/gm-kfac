#pragma once
#include <atomic>

namespace oem {

class SpinLock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#endif
        }
    }

    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }

    class Guard {
        SpinLock& lock_;
    public:
        explicit Guard(SpinLock& l) : lock_(l) { lock_.lock(); }
        ~Guard() { lock_.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
};

} // namespace oem
