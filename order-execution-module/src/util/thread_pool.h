#pragma once
#include <atomic>
#include <array>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>
#include <new>

namespace oem {

struct alignas(64) Task {
    static constexpr size_t STORAGE_SIZE = 120;
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
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.data = std::move(task);
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
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
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    task = std::move(cell.data);
                    cell.sequence.store(pos + CAP, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }
};

class ThreadPool {
    MPMCQueue<64> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{true};

public:
    explicit ThreadPool(size_t n_workers = 4) {
        workers_.reserve(n_workers);
        for (size_t i = 0; i < n_workers; ++i) {
            workers_.emplace_back([this] {
                while (running_.load(std::memory_order_relaxed)) {
                    Task task;
                    if (queue_.dequeue(task)) {
                        task.run();
                    } else {
#if defined(__x86_64__)
                        for (int j = 0; j < 64; ++j) __builtin_ia32_pause();
#elif defined(__aarch64__)
                        for (int j = 0; j < 64; ++j) asm volatile("yield");
#else
                        std::this_thread::yield();
#endif
                    }
                }
                Task task;
                while (queue_.dequeue(task)) task.run();
            });
        }
    }

    ~ThreadPool() {
        running_.store(false, std::memory_order_release);
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F>
    bool submit(F&& fn) {
        return queue_.enqueue(Task::from(std::forward<F>(fn)));
    }
};

inline ThreadPool& global_pool() {
    static ThreadPool instance(4);
    return instance;
}

} // namespace oem
