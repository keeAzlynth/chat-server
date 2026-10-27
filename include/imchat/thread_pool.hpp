#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace imchat {

// Fixed-size pool where work is routed to a shard by key.
//
// Two properties matter for a chat server:
//   * commands from one connection must be executed in submission order,
//     otherwise "send" could overtake "recall";
//   * different connections must run in parallel, because every command
//     touches SQLite and would otherwise serialise on the reactor thread.
//
// Routing by connection id gives both: one worker owns a key at a time,
// while the other shards keep running.
class ShardedExecutor {
public:
    explicit ShardedExecutor(std::size_t shards);
    ~ShardedExecutor();

    ShardedExecutor(const ShardedExecutor&) = delete;
    ShardedExecutor& operator=(const ShardedExecutor&) = delete;

    void post(std::string_view key, std::function<void()> fn);

    // drain=true finishes queued work before returning.
    void stop(bool drain = true);

    [[nodiscard]] std::size_t size() const noexcept { return shards_.size(); }
    [[nodiscard]] std::size_t pending() const;

private:
    struct Shard {
        std::mutex                        mutex;
        std::condition_variable           cv;
        std::deque<std::function<void()>> queue;
        std::thread                       thread;
        bool                              stopping{false};
    };

    static void run(Shard* shard);

    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic<bool>                   stopped_{false};
};

} // namespace imchat
