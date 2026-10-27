#pragma once

#include "imchat/string_hash.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>

namespace imchat {

// Token bucket keyed by an arbitrary string (connection id / user id).
//
// The previous implementation used wall-clock seconds and reset a counter
// every whole second, which both allowed 2x bursts across the boundary and
// never freed the per-connection state (one leak per connection ever seen).
// This version refills continuously against a monotonic clock and supports
// explicit eviction when a connection goes away.
class RateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    RateLimiter() : RateLimiter(10.0, 20.0) {}

    RateLimiter(double rate_per_sec, double burst)
        : rate_(rate_per_sec > 0.0 ? rate_per_sec : 1.0)
        , burst_(burst > 0.0 ? burst : (rate_per_sec > 0.0 ? rate_per_sec : 1.0))
    {
    }

    [[nodiscard]] bool allow(std::string_view key) {
        const auto now = Clock::now();
        std::lock_guard lock(mutex_);

        auto it = buckets_.find(key);
        if (it == buckets_.end()) {
            if (buckets_.size() >= kMaxBuckets) sweep_locked(now);
            it = buckets_.emplace(std::string(key), Bucket{burst_, now}).first;
        } else {
            const double elapsed = std::chrono::duration<double>(now - it->second.last).count();
            if (elapsed > 0.0) {
                it->second.tokens = std::min(burst_, it->second.tokens + elapsed * rate_);
            }
        }

        it->second.last = now;
        if (it->second.tokens < 1.0) return false;
        it->second.tokens -= 1.0;
        return true;
    }

    void forget(std::string_view key) {
        std::lock_guard lock(mutex_);
        buckets_.erase(std::string(key));
    }

    // Drop buckets that have been idle long enough to be full again.
    void sweep_idle() {
        std::lock_guard lock(mutex_);
        sweep_locked(Clock::now());
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return buckets_.size();
    }

private:
    static constexpr std::size_t kMaxBuckets = 100'000;
    static constexpr auto        kIdleAfter  = std::chrono::minutes(10);

    struct Bucket {
        double          tokens{0.0};
        Clock::time_point last{};
    };

    void sweep_locked(Clock::time_point now) {
        std::erase_if(buckets_, [now](const auto& entry) {
            return now - entry.second.last > kIdleAfter;
        });
    }

    double   rate_;
    double   burst_;
    mutable std::mutex mutex_;
    StringMap<Bucket>  buckets_;
};

} // namespace imchat
