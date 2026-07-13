#pragma once

#include <chrono>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace imchat {

// Approximate-LRU cache optimized for read-heavy workloads.
// Reads take a shared_lock (concurrent) and only bump an atomic counter.
// Eviction scans for the entry with the lowest counter — O(n) but n ≤ 1000 so
// it's negligible compared to eliminating the exclusive lock on every read.
template<typename Key, typename Value>
class LRUCache {
public:
    explicit LRUCache(std::size_t capacity, std::chrono::seconds ttl = std::chrono::seconds{0})
        : capacity_(capacity), ttl_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(ttl).count()) {}

    void put(const Key& key, Value value) {
        std::unique_lock lock(mutex_);

        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.value = std::move(value);
            it->second.access_count.store(0, std::memory_order_relaxed);
            it->second.created_ns = now_ns();
            return;
        }

        if (map_.size() >= capacity_) {
            evict();
        }

        map_.emplace(key, Entry{std::move(value), 0, now_ns()});
    }

    [[nodiscard]] std::optional<Value> get(const Key& key) {
        std::shared_lock lock(mutex_);

        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;

        if (ttl_ns_ > 0 && (now_ns() - it->second.created_ns) > ttl_ns_) {
            return std::nullopt;            // expired — lazy removal on next put()
        }

        it->second.access_count.fetch_add(1, std::memory_order_relaxed);
        return it->second.value;
    }

    [[nodiscard]] bool contains(const Key& key) const {
        std::shared_lock lock(mutex_);
        return map_.count(key) > 0;
    }

    void remove(const Key& key) {
        std::unique_lock lock(mutex_);
        map_.erase(key);
    }

    void clear() {
        std::unique_lock lock(mutex_);
        map_.clear();
    }

    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock(mutex_);
        return map_.size();
    }

private:
    struct Entry {
        Value value;
        std::atomic<uint64_t> access_count{0};
        int64_t created_ns{0};

        Entry() = default;
        Entry(Value v, uint64_t ac, int64_t cn)
            : value(std::move(v)), access_count(ac), created_ns(cn) {}

        Entry(Entry&& o) noexcept
            : value(std::move(o.value))
            , access_count(o.access_count.load(std::memory_order_relaxed))
            , created_ns(o.created_ns) {}

        Entry& operator=(Entry&& o) noexcept {
            value = std::move(o.value);
            access_count.store(o.access_count.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
            created_ns = o.created_ns;
            return *this;
        }

        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
    };

    static int64_t now_ns() noexcept {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }

    void evict() noexcept {
        auto victim = map_.begin();
        auto lowest = victim->second.access_count.load(std::memory_order_relaxed);
        for (auto it = map_.begin(); it != map_.end(); ++it) {
            auto cnt = it->second.access_count.load(std::memory_order_relaxed);
            if (cnt < lowest) {
                lowest = cnt;
                victim = it;
            }
        }
        map_.erase(victim);
    }

    std::size_t capacity_;
    int64_t ttl_ns_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<Key, Entry> map_;
};

} // namespace imchat
