#pragma once
#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <string>

namespace oem {

template<typename V, size_t CAP = 512>
class FlatHashMap {
    struct Slot {
        char key[32] = {};
        V value{};
        bool occupied = false;
    };

    std::array<Slot, CAP> slots_{};
    size_t size_ = 0;

    size_t hash_key(const char* key) const {
        size_t h = 14695981039346656037ULL;
        for (const char* p = key; *p; ++p) {
            h ^= static_cast<size_t>(*p);
            h *= 1099511628211ULL;
        }
        return h % CAP;
    }

public:
    V* find(const std::string& key) {
        return find(key.c_str());
    }

    V* find(const char* key) {
        size_t idx = hash_key(key);
        for (size_t i = 0; i < CAP; ++i) {
            size_t pos = (idx + i) % CAP;
            if (!slots_[pos].occupied) return nullptr;
            if (std::strcmp(slots_[pos].key, key) == 0)
                return &slots_[pos].value;
        }
        return nullptr;
    }

    const V* find(const std::string& key) const {
        return find(key.c_str());
    }

    const V* find(const char* key) const {
        size_t idx = hash_key(key);
        for (size_t i = 0; i < CAP; ++i) {
            size_t pos = (idx + i) % CAP;
            if (!slots_[pos].occupied) return nullptr;
            if (std::strcmp(slots_[pos].key, key) == 0)
                return &slots_[pos].value;
        }
        return nullptr;
    }

    V& operator[](const std::string& key) {
        return get_or_insert(key.c_str());
    }

    V& operator[](const char* key) {
        return get_or_insert(key);
    }

    V& get_or_insert(const char* key) {
        size_t idx = hash_key(key);
        for (size_t i = 0; i < CAP; ++i) {
            size_t pos = (idx + i) % CAP;
            if (!slots_[pos].occupied) {
                std::strncpy(slots_[pos].key, key, 31);
                slots_[pos].key[31] = '\0';
                slots_[pos].occupied = true;
                ++size_;
                return slots_[pos].value;
            }
            if (std::strcmp(slots_[pos].key, key) == 0)
                return slots_[pos].value;
        }
        static V dummy{};
        return dummy;
    }

    bool erase(const std::string& key) {
        return erase(key.c_str());
    }

    bool erase(const char* key) {
        size_t idx = hash_key(key);
        for (size_t i = 0; i < CAP; ++i) {
            size_t pos = (idx + i) % CAP;
            if (!slots_[pos].occupied) return false;
            if (std::strcmp(slots_[pos].key, key) == 0) {
                slots_[pos].occupied = false;
                slots_[pos].value = V{};
                std::memset(slots_[pos].key, 0, 32);
                --size_;
                // Rehash subsequent entries to fix probing chain
                size_t next = (pos + 1) % CAP;
                while (slots_[next].occupied) {
                    char tmp_key[32];
                    std::memcpy(tmp_key, slots_[next].key, 32);
                    V tmp_val = std::move(slots_[next].value);
                    slots_[next].occupied = false;
                    std::memset(slots_[next].key, 0, 32);
                    --size_;
                    // Re-insert
                    get_or_insert(tmp_key) = std::move(tmp_val);
                    next = (next + 1) % CAP;
                }
                return true;
            }
        }
        return false;
    }

    size_t size() const { return size_; }

    template<typename Fn>
    void for_each(Fn&& fn) {
        for (auto& slot : slots_) {
            if (slot.occupied) {
                fn(std::string(slot.key), slot.value);
            }
        }
    }

    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& slot : slots_) {
            if (slot.occupied) {
                fn(std::string(slot.key), slot.value);
            }
        }
    }

    void clear() {
        for (auto& slot : slots_) {
            slot.occupied = false;
            slot.value = V{};
            std::memset(slot.key, 0, 32);
        }
        size_ = 0;
    }
};

} // namespace oem
