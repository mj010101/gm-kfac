# Claude Code Prompt — Latency Optimization Pass for Order Execution Module

## Context

We have a working C++20 Order Execution Module at:
`/Users/minjun/Desktop/MEV_Quant/gm-kfac/order-execution-module/`

It has 9 test cases, all passing (16 GoogleTest + 9 demo scenarios). The module handles single-leg futures/options orders and CEX-DEX arbitrage (Binance/Bybit ↔ Hyperliquid).

**The architecture is correct and must be preserved.** The component separation (SignalReceiver, OrderManager, ConflictResolver, PortfolioGuard, ArbCoordinator, Exchange Gateways) stays exactly as-is. The test cases stay as-is. The arb coordination logic, conflict resolution, and margin lifecycle (reserved → used → released) must not change.

**What we are doing:** Applying low-latency optimization techniques to the internals — replacing slow primitives with fast ones, eliminating heap allocations on the hot path, removing virtual dispatch overhead, and making the data structures cache-friendly. The goal is to get the hot path (signal received → gateway dispatch) under 1 microsecond of internal processing time, while keeping all existing functionality intact.

## IMPORTANT: Read First, Then Plan, Then Implement

1. First, read all existing source files in `src/` and `test/` to understand the current implementation
2. Create a migration plan listing every file that needs changes
3. Implement changes file by file, running tests after each major change to ensure nothing breaks
4. All 9 test cases must continue to pass after every change

---

## Optimization 1: Replace std::mutex / std::shared_mutex with SpinLock

### Why
`std::mutex` involves OS kernel calls and context switches (~1-10µs). For critical sections that are very short (< 100ns), a SpinLock using `std::atomic_flag` is faster because it avoids the kernel entirely.

### Current State
- `PortfolioGuard` uses `std::shared_mutex` (mutable shared_mutex mutex_)
- `ConflictResolver` uses `std::mutex` per symbol
- `ArbCoordinator` uses `std::mutex` for the arb pair map
- `OrderManager` uses `std::shared_mutex` for the order state map

### What to Do

Create `src/util/spinlock.h`:
```cpp
#pragma once
#include <atomic>

namespace oem {

class SpinLock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // Spin with pause hint to reduce power and improve performance
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

    // RAII guard
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
```

Replace all `std::mutex` and `std::shared_mutex` usages:
- `PortfolioGuard`: Replace `std::shared_mutex` + `std::shared_lock`/`std::unique_lock` with `SpinLock` + `SpinLock::Guard`. The critical section is just arithmetic and map lookups (< 100ns), so spinlock is appropriate. Note: we lose reader parallelism, but the critical sections are so short that contention is negligible.
- `ConflictResolver`: Replace per-symbol `std::mutex` with per-symbol `SpinLock`
- `ArbCoordinator`: Replace `std::mutex` with `SpinLock`
- `OrderManager`: Replace `std::shared_mutex` with `SpinLock`

**Design principle:** The SpinLock must be released BEFORE any gateway I/O call. Only the validation + margin reservation + state update should be inside the lock. Gateway dispatch happens outside the lock.

---

## Optimization 2: Replace std::unordered_map with FlatHashMap

### Why
`std::unordered_map` uses chaining (linked list per bucket), which causes pointer chasing and cache misses. A flat open-addressing hash map with linear probing keeps all data contiguous in memory, fitting in L1/L2 cache. Lookup goes from ~50-100ns to ~5-15ns.

### What to Do

Create `src/util/flat_hash_map.h`:
```cpp
#pragma once
#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <string>

namespace oem {

// Fixed-capacity open-addressing hash map with linear probing.
// Key = std::string (symbol names), Value = template parameter.
// No heap allocation, no rehashing, cache-friendly.
template<typename V, size_t CAP = 512>
class FlatHashMap {
    struct Slot {
        char key[32] = {};      // Fixed-size key (symbols are short)
        V value{};
        bool occupied = false;
    };

    std::array<Slot, CAP> slots_{};
    size_t size_ = 0;

    size_t hash_key(const char* key) const {
        // FNV-1a hash
        size_t h = 14695981039346656037ULL;
        for (const char* p = key; *p; ++p) {
            h ^= static_cast<size_t>(*p);
            h *= 1099511628211ULL;
        }
        return h % CAP;
    }

public:
    V* find(const std::string& key) {
        size_t idx = hash_key(key.c_str());
        for (size_t i = 0; i < CAP; ++i) {
            size_t pos = (idx + i) % CAP;
            if (!slots_[pos].occupied) return nullptr;
            if (std::strcmp(slots_[pos].key, key.c_str()) == 0)
                return &slots_[pos].value;
        }
        return nullptr;
    }

    V& operator[](const std::string& key) {
        size_t idx = hash_key(key.c_str());
        for (size_t i = 0; i < CAP; ++i) {
            size_t pos = (idx + i) % CAP;
            if (!slots_[pos].occupied) {
                std::strncpy(slots_[pos].key, key.c_str(), 31);
                slots_[pos].occupied = true;
                ++size_;
                return slots_[pos].value;
            }
            if (std::strcmp(slots_[pos].key, key.c_str()) == 0)
                return slots_[pos].value;
        }
        // Should never reach here if CAP is sufficient
        static V dummy{};
        return dummy;
    }

    size_t size() const { return size_; }

    // Iterator support for range-for
    template<typename Fn>
    void for_each(Fn&& fn) {
        for (auto& slot : slots_) {
            if (slot.occupied) {
                fn(std::string(slot.key), slot.value);
            }
        }
    }

    void clear() {
        for (auto& slot : slots_) {
            slot.occupied = false;
            slot.value = V{};
        }
        size_ = 0;
    }
};

} // namespace oem
```

Replace these `std::unordered_map` usages:
- `PortfolioState::positions` (symbol → double)
- `PortfolioState::margin_used` (symbol → double)
- `PortfolioState::margin_reserved` (symbol → double)
- `ConflictResolver` internal tracking map (symbol → vector of in-flight orders)
- `ArbCoordinator` arb pair map (group_id → ArbPair)
- `OrderManager` order state map (order_id → Order)

**Note:** For maps where the value is `std::vector<Order>` (like ConflictResolver's in-flight tracking), use a FlatHashMap with a small fixed-size array as value instead of vector, e.g., `FlatHashMap<std::array<Order, 8>>` with a count field, to avoid any heap allocation.

---

## Optimization 3: Replace double with int64_t Ticks for Prices and Quantities

### Why
Floating point arithmetic has precision issues (e.g., 0.1 + 0.2 ≠ 0.3). In financial systems, this causes margin calculation drift over time. Using fixed-point integer arithmetic with a scale factor eliminates this entirely. It also enables integer comparison instead of floating-point epsilon comparison.

### What to Do

Add to `src/util/fixed_point.h`:
```cpp
#pragma once
#include <cstdint>

namespace oem {

// Scale factor: 1.0 = 100,000,000 ticks (10^8)
// Supports 8 decimal places of precision
// Max representable value: ~92,233,720,368 (int64_t max / SCALE)
constexpr int64_t SCALE = 100'000'000LL;

// Conversion helpers
inline int64_t to_ticks(double value) {
    return static_cast<int64_t>(value * SCALE);
}

inline double to_double(int64_t ticks) {
    return static_cast<double>(ticks) / SCALE;
}

// Safe notional calculation: (price_ticks / SCALE) * qty_ticks
// Division first to prevent int64_t overflow
inline int64_t notional_ticks(int64_t price_ticks, int64_t qty_ticks) {
    return (price_ticks / SCALE) * qty_ticks;
}

// Margin calculation: notional / leverage
inline int64_t margin_ticks(int64_t price_ticks, int64_t qty_ticks, int leverage) {
    return notional_ticks(price_ticks, qty_ticks) / leverage;
}

} // namespace oem
```

Then update these structures to use `int64_t` instead of `double`:
- `Signal`: `quantity` → `qty_ticks`, `price` → `price_ticks`, `strike_price` → `strike_price_ticks`
- `Order`: `filled_quantity` → `filled_qty_ticks`, `avg_fill_price` → `avg_fill_price_ticks`
- `ArbPair`: `expected_spread_bps` → `expected_spread_ticks`, `realized_spread_bps` → `realized_spread_ticks` (or keep bps as int)
- `PortfolioState`: `cash` → `cash_ticks`, `unrealized_pnl` → `unrealized_pnl_ticks`, `realized_pnl` → `realized_pnl_ticks`. All map values (`positions`, `margin_used`, `margin_reserved`) become `int64_t`.
- `Fill` struct in gateway: `filled_qty` → `filled_qty_ticks`, `fill_price` → `fill_price_ticks`

Update all arithmetic in PortfolioGuard, ArbCoordinator, etc. to use integer operations.

**For logging and test output**, convert back to double with `to_double()` only at the display boundary.

**Update DummySignalGenerator** to produce ticks values:
```cpp
// Before: price: 100000.0, quantity: 0.1
// After:  price_ticks: 100000LL * SCALE, qty_ticks: to_ticks(0.1)
```

**Update config.json parsing** to convert config doubles to ticks at load time.

---

## Optimization 4: Replace spdlog with Lock-Free AsyncLogger

### Why
spdlog, while fast, still uses internal mutexes and `std::string` formatting which causes heap allocations. On the hot path (signal → dispatch), even a single log call can add 1-5µs. A custom async logger with a lock-free ring buffer and a dedicated I/O thread removes logging from the critical path entirely.

### What to Do

Create `src/util/async_logger.h`:
```cpp
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
    static constexpr size_t RING_CAP = 4096; // Must be power of 2

    struct Slot {
        char buf[SLOT_SIZE];
        std::atomic<bool> ready{false};
    };

    std::array<Slot, RING_CAP> ring_{};
    alignas(64) std::atomic<size_t> write_head_{0};
    alignas(64) std::atomic<size_t> read_head_{0};
    std::atomic<bool> running_{true};
    std::jthread io_thread_;

    void io_loop() {
        while (running_.load(std::memory_order_relaxed) ||
               read_head_.load(std::memory_order_acquire) !=
               write_head_.load(std::memory_order_acquire)) {
            size_t r = read_head_.load(std::memory_order_acquire);
            size_t w = write_head_.load(std::memory_order_acquire);
            if (r == w) {
                // Use pause instead of sleep to avoid context switch
                #if defined(__x86_64__)
                    for (int i = 0; i < 32; ++i) __builtin_ia32_pause();
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

public:
    AsyncLogger() : io_thread_([this]{ io_loop(); }) {}

    ~AsyncLogger() {
        running_.store(false, std::memory_order_release);
        if (io_thread_.joinable()) io_thread_.join();
    }

    // Push formatted message - lock-free, no heap allocation
    void pushf(const char* fmt, ...) {
        size_t idx = write_head_.fetch_add(1, std::memory_order_acq_rel);
        auto& slot = ring_[idx % RING_CAP];
        // Spin-wait if slot is still being read (extremely rare in practice)
        while (slot.ready.load(std::memory_order_acquire)) {
            #if defined(__x86_64__)
                __builtin_ia32_pause();
            #else
                std::this_thread::yield();
            #endif
        }
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(slot.buf, SLOT_SIZE, fmt, args);
        va_end(args);
        slot.ready.store(true, std::memory_order_release);
    }

    // Convenience: info/warn/error with level prefix
    void info(const char* component, const char* fmt, ...) {
        size_t idx = write_head_.fetch_add(1, std::memory_order_acq_rel);
        auto& slot = ring_[idx % RING_CAP];
        while (slot.ready.load(std::memory_order_acquire)) {
            #if defined(__x86_64__)
                __builtin_ia32_pause();
            #else
                std::this_thread::yield();
            #endif
        }
        int offset = std::snprintf(slot.buf, 32, "[info] [%s] ", component);
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(slot.buf + offset, SLOT_SIZE - offset, fmt, args);
        va_end(args);
        // Append newline
        size_t len = std::strlen(slot.buf);
        if (len < SLOT_SIZE - 1) { slot.buf[len] = '\n'; slot.buf[len+1] = '\0'; }
        slot.ready.store(true, std::memory_order_release);
    }

    void warn(const char* component, const char* fmt, ...) {
        size_t idx = write_head_.fetch_add(1, std::memory_order_acq_rel);
        auto& slot = ring_[idx % RING_CAP];
        while (slot.ready.load(std::memory_order_acquire)) {
            #if defined(__x86_64__)
                __builtin_ia32_pause();
            #else
                std::this_thread::yield();
            #endif
        }
        int offset = std::snprintf(slot.buf, 32, "[warn] [%s] ", component);
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(slot.buf + offset, SLOT_SIZE - offset, fmt, args);
        va_end(args);
        size_t len = std::strlen(slot.buf);
        if (len < SLOT_SIZE - 1) { slot.buf[len] = '\n'; slot.buf[len+1] = '\0'; }
        slot.ready.store(true, std::memory_order_release);
    }
};

// Global logger instance
inline AsyncLogger& global_logger() {
    static AsyncLogger instance;
    return instance;
}

} // namespace oem
```

Replace all `spdlog` calls across the codebase:
- `logger_->info("...")` → `global_logger().info("component", "...")`
- `logger_->warn("...")` → `global_logger().warn("component", "...")`
- Remove spdlog from CMakeLists.txt FetchContent (keep nlohmann/json and GoogleTest)
- Remove all `#include <spdlog/...>` headers
- Remove the `get_logger()` utility function

**Keep the same log message content** so that test assertions based on behavior (not log output) still pass.

---

## Optimization 5: Replace Virtual Dispatch with std::variant + std::visit

### Why
Virtual function calls (through `IExchangeGateway*`) involve an indirect branch via vtable lookup. This can cause a branch misprediction penalty (~10-20ns). `std::variant` + `std::visit` resolves the dispatch at compile time, allowing the compiler to generate a direct jump table.

### What to Do

Replace the gateway pointer map:
```cpp
// Before (virtual dispatch):
std::unordered_map<Exchange, IExchangeGateway*> gateways_;

// After (static dispatch):
using AnyGateway = std::variant<MockCexGateway*, MockDexGateway*>;
// When Phase 2 adds real gateways:
// using AnyGateway = std::variant<MockCexGateway*, MockDexGateway*,
//                                  BinanceGateway*, HyperliquidGateway*>;

FlatHashMap<AnyGateway, 8> gateways_;  // exchange_id → variant
```

Update OrderManager and ArbCoordinator dispatch:
```cpp
// Before:
auto* gw = gateways_[order.signal.exchange];
auto future = gw->send_order(order);

// After:
auto* gw_var = gateways_.find(exchange_to_string(order.signal.exchange));
if (gw_var) {
    std::visit([&](auto* gw) {
        gw->send_order(order, [this](const Fill& fill) { on_fill(fill); });
    }, *gw_var);
}
```

**Note:** Keep the `IExchangeGateway`, `ICexGateway`, `IDexGateway` interfaces for documentation and type safety, but the actual dispatch path uses the variant. The interfaces still serve as contracts for Phase 2 when real gateways are implemented.

---

## Optimization 6: Pre-Spawned ThreadPool with Lock-Free Task Queue

### Why
Currently, gateway calls may use `std::async` or spawn threads. Creating a thread costs ~10-50µs. A pre-spawned thread pool with spinning workers and a lock-free queue reduces task submission to ~10ns.

### What to Do

Create `src/util/thread_pool.h`:
```cpp
#pragma once
#include <atomic>
#include <array>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>
#include <new>

namespace oem {

// Fixed-size task that fits in cache line — no heap allocation
struct alignas(64) Task {
    static constexpr size_t STORAGE_SIZE = 120; // 128 - 8 bytes for vtable-like ptr
    using InvokeFn = void(*)(void*);

    alignas(8) char storage[STORAGE_SIZE];
    InvokeFn invoke = nullptr;

    Task() = default;

    template<typename F>
    static Task from(F&& fn) {
        static_assert(sizeof(F) <= STORAGE_SIZE, "Callable too large for Task inline storage");
        Task t;
        new (t.storage) F(std::forward<F>(fn));
        t.invoke = [](void* ptr) {
            auto* f = reinterpret_cast<F*>(ptr);
            (*f)();
            f->~F();
        };
        return t;
    }

    void run() {
        if (invoke) {
            invoke(storage);
            invoke = nullptr;
        }
    }
};

// Lock-free MPMC ring buffer for tasks
template<size_t CAP = 64>
class MPMCQueue {
    static_assert((CAP & (CAP - 1)) == 0, "CAP must be power of 2");

    struct Cell {
        std::atomic<size_t> sequence;
        Task data;
    };

    std::array<Cell, CAP> buffer_;
    alignas(64) std::atomic<size_t> enqueue_pos_{0};
    alignas(64) std::atomic<size_t> dequeue_pos_{0};

public:
    MPMCQueue() {
        for (size_t i = 0; i < CAP; ++i)
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool enqueue(Task&& task) {
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            auto& cell = buffer_[pos & (CAP - 1)];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.data = std::move(task);
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    bool dequeue(Task& task) {
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            auto& cell = buffer_[pos & (CAP - 1)];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    task = std::move(cell.data);
                    cell.sequence.store(pos + CAP, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }
};

class ThreadPool {
    MPMCQueue<64> queue_;
    std::vector<std::jthread> workers_;
    std::atomic<bool> running_{true};

public:
    explicit ThreadPool(size_t n_workers = 10) {
        workers_.reserve(n_workers);
        for (size_t i = 0; i < n_workers; ++i) {
            workers_.emplace_back([this] {
                while (running_.load(std::memory_order_relaxed)) {
                    Task task;
                    if (queue_.dequeue(task)) {
                        task.run();
                    } else {
                        // Spin with pause to avoid burning CPU
                        #if defined(__x86_64__)
                            for (int j = 0; j < 64; ++j) __builtin_ia32_pause();
                        #else
                            std::this_thread::yield();
                        #endif
                    }
                }
                // Drain remaining
                Task task;
                while (queue_.dequeue(task)) task.run();
            });
        }
    }

    ~ThreadPool() {
        running_.store(false, std::memory_order_release);
    }

    template<typename F>
    bool submit(F&& fn) {
        return queue_.enqueue(Task::from(std::forward<F>(fn)));
    }
};

// Global thread pool
inline ThreadPool& global_pool() {
    static ThreadPool instance(10);
    return instance;
}

} // namespace oem
```

Update gateway dispatch in OrderManager and ArbCoordinator to use `global_pool().submit(...)` instead of creating threads or using `std::async`.

---

## Optimization 7: Cache-Line Aligned Structures

### Why
When two frequently-accessed fields share a cache line (64 bytes) and are written by different threads, "false sharing" occurs — one thread's write invalidates the other thread's cache. Aligning hot structures to cache line boundaries prevents this.

### What to Do

Add `alignas(64)` to these structures:
- `Signal` struct
- `Order` struct
- `PortfolioState::cash_ticks` field (separate cache line from positions map)
- `SpinLock` internal flag
- `ThreadPool` queue head/tail (already done in MPMCQueue above)

Make `Signal` struct fit in a cache-friendly size:
- Replace `std::string signal_id` with `char signal_id[16]`
- Replace `std::string symbol` with `char symbol[16]`
- Replace `std::optional<std::string> group_id` with `char group_id[16]` + `bool has_group_id`
- Replace `std::optional<std::string> expiry` with `char expiry[16]` + `bool has_expiry`
- Replace `std::optional<std::string> option_type` with `char option_type[8]` + `bool has_option_type`

This eliminates all `std::string` heap allocations from the Signal struct and makes it a fixed-size POD type.

---

## Optimization 8: Stack-Allocated Position Keys

### Why
Creating `std::string` keys for hash map lookups causes `malloc` on every call. Using a fixed-size `char[32]` stack buffer eliminates this.

### What to Do

Create a `PosKey` utility:
```cpp
// In src/util/pos_key.h
#pragma once
#include <cstdio>

namespace oem {

struct PosKey {
    char buf[32] = {};

    // Format: "EXCHANGE_SYMBOL" e.g., "BINANCE_BTC-PERP"
    static PosKey make(const char* exchange, const char* symbol) {
        PosKey k;
        std::snprintf(k.buf, sizeof(k.buf), "%s_%s", exchange, symbol);
        return k;
    }

    const char* c_str() const { return buf; }
    std::string to_string() const { return std::string(buf); }
};

} // namespace oem
```

Use `PosKey` wherever symbol-based map lookups happen in PortfolioGuard and ConflictResolver.

---

## Migration Order

Apply optimizations in this order to minimize breakage:

1. **SpinLock** (Opt 1) — Drop-in replacement for mutexes. Tests should pass immediately.
2. **Fixed-point ticks** (Opt 3) — Requires updating Signal, Order, PortfolioState, all tests, and DummySignalGenerator. This is the largest change. Run tests after.
3. **FlatHashMap** (Opt 2) — Replace unordered_maps one at a time. Run tests after each replacement.
4. **Cache-aligned structs + stack keys** (Opt 7, 8) — Replace std::string in structs with char arrays. Update all code that reads these fields.
5. **AsyncLogger** (Opt 4) — Replace spdlog. Remove from CMakeLists. Tests should still pass since tests check behavior, not log format.
6. **ThreadPool** (Opt 6) — Replace std::async/thread creation with pool submission.
7. **Variant dispatch** (Opt 5) — Replace virtual gateway pointers with std::variant. This requires careful handling of the gateway map type.

---

## What NOT to Change

- **Component architecture**: SignalReceiver, OrderManager, ConflictResolver, PortfolioGuard, ArbCoordinator, Gateways — these stay as separate components in separate files
- **ArbCoordinator logic**: Risk window tracking, spread calculation, unwind mechanism
- **ConflictResolver logic**: Cancel-and-replace flow
- **Margin lifecycle**: reserved → used → released
- **Test cases**: All 9 scenarios must produce the same results
- **Config loading**: Keep config.json based configuration
- **Gateway interfaces**: Keep IExchangeGateway/ICexGateway/IDexGateway as documentation contracts

---

## Verification

After all optimizations are applied:

1. All 16 GoogleTest cases must pass: `./order_execution_tests`
2. All 9 demo scenarios must pass: `./order_execution`
3. Add a latency benchmark in main.cpp that measures:
   - Time from `receiveOrder()` entry to gateway dispatch (internal hot path)
   - Report p50, p99, and max latency over 1000 dummy signals
   - Target: p99 < 1µs for single-leg, p99 < 2µs for arb pair dispatch
4. No heap allocations on the hot path — verify by running with `-fsanitize=address` and checking for no `malloc` calls in the hot path (or add a simple allocation counter)

---

## Build

After changes, the project should still build with:
```bash
cd order-execution-module
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./order_execution_tests  # All 16 pass
./order_execution         # All 9 pass
```

spdlog should be removed from CMakeLists.txt FetchContent. The remaining dependencies are nlohmann/json and GoogleTest only.
