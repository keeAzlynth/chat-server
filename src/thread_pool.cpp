#include "imchat/thread_pool.hpp"

#include <algorithm>
#include <functional>

namespace imchat {

namespace {
// More shards than this only adds contention: every command eventually
// serialises on the single SQLite writer.
constexpr std::size_t kMinShards = 4;
constexpr std::size_t kMaxShards = 8;
} // namespace

ShardedExecutor::ShardedExecutor(std::size_t shards)
{
    if (shards == 0) {
        shards = std::thread::hardware_concurrency();
    }
    shards = std::clamp<std::size_t>(shards, kMinShards, kMaxShards);

    shards_.reserve(shards);
    for (std::size_t i = 0; i < shards; ++i) {
        auto shard = std::make_unique<Shard>();
        shard->thread = std::thread(&ShardedExecutor::run, shard.get());
        shards_.push_back(std::move(shard));
    }
}

ShardedExecutor::~ShardedExecutor()
{
    stop(true);
}

void ShardedExecutor::post(std::string_view key, std::function<void()> fn)
{
    if (!fn || stopped_.load(std::memory_order_acquire)) return;

    const auto shard_index = std::hash<std::string_view>{}(key) % shards_.size();
    auto& shard = *shards_[shard_index];

    {
        std::lock_guard lock(shard.mutex);
        if (shard.stopping) return;
        shard.queue.push_back(std::move(fn));
    }
    shard.cv.notify_one();
}

void ShardedExecutor::stop(bool drain)
{
    if (stopped_.exchange(true, std::memory_order_acq_rel)) return;

    for (auto& shard : shards_) {
        {
            std::lock_guard lock(shard->mutex);
            shard->stopping = true;
            if (!drain) shard->queue.clear();
        }
        shard->cv.notify_all();
    }

    for (auto& shard : shards_) {
        if (shard->thread.joinable()) shard->thread.join();
    }
    shards_.clear();
}

std::size_t ShardedExecutor::pending() const
{
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        std::lock_guard lock(shard->mutex);
        total += shard->queue.size();
    }
    return total;
}

void ShardedExecutor::run(Shard* shard)
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(shard->mutex);
            shard->cv.wait(lock, [shard] { return shard->stopping || !shard->queue.empty(); });

            if (shard->queue.empty()) {
                if (shard->stopping) return;
                continue;
            }

            task = std::move(shard->queue.front());
            shard->queue.pop_front();
        }

        try {
            task();
        } catch (...) {
            // A single failing command must never take a worker down.
        }
    }
}

} // namespace imchat
