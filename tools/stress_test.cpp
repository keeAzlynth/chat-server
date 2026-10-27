#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    int connections = 100;
    int messages_per_client = 50;
    int message_size = 64;
    int ramp_up_ms = 2000;
};

struct Stats {
    std::atomic<int64_t> messages_sent{0};
    std::atomic<int64_t> messages_received{0};
    std::atomic<int64_t> errors{0};
    std::atomic<int64_t> connect_failures{0};
    std::mutex latency_mutex;
    std::vector<double> latencies_us;
};

Config parse_args(int argc, char* argv[]) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) c.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) c.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        else if (arg == "-c" && i + 1 < argc) c.connections = std::stoi(argv[++i]);
        else if (arg == "-n" && i + 1 < argc) c.messages_per_client = std::stoi(argv[++i]);
        else if (arg == "-s" && i + 1 < argc) c.message_size = std::stoi(argv[++i]);
        else if (arg == "--ramp" && i + 1 < argc) c.ramp_up_ms = std::stoi(argv[++i]);
        else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: stress_test [options]\n"
                      << "  --host <addr>    Server address (default: 127.0.0.1)\n"
                      << "  --port <port>    Server port (default: 8080)\n"
                      << "  -c <count>       Number of connections (default: 100)\n"
                      << "  -n <count>       Messages per client (default: 50)\n"
                      << "  -s <bytes>       Message payload size (default: 64)\n"
                      << "  --ramp <ms>      Ramp-up time in ms (default: 2000)\n";
            std::exit(0);
        }
    }
    return c;
}

std::string random_string(std::mt19937& rng, int len) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<int> dist(0, sizeof(chars) - 2);
    std::string s;
    s.reserve(len);
    for (int i = 0; i < len; ++i) s += chars[dist(rng)];
    return s;
}

class Client : public std::enable_shared_from_this<Client> {
public:
    Client(net::io_context& ioc, const Config& config, int id, Stats& stats)
        : ioc_(ioc)
        , strand_(net::make_strand(ioc))
        , ws_(std::make_shared<websocket::stream<tcp::socket>>(net::make_strand(ioc)))
        , config_(config)
        , id_(id)
        , stats_(stats)
        , rng_(id)
    {}

    void start(std::chrono::steady_clock::time_point start_time) {
        auto self = shared_from_this();
        auto my_start = start_time + std::chrono::milliseconds(
            id_ * config_.ramp_up_ms / config_.connections);

        net::post(ioc_, [this, self, my_start]() {
            std::this_thread::sleep_until(my_start);
            do_connect();
        });
    }

    void wait_done() {
        std::unique_lock<std::mutex> lk(done_mutex_);
        done_cv_.wait(lk, [this] { return done_; });
    }

private:
    void do_connect() {
        auto self = shared_from_this();
        auto resolver = std::make_shared<tcp::resolver>(ioc_);
        resolver->async_resolve(config_.host, std::to_string(config_.port),
            [this, self, resolver](beast::error_code ec, tcp::resolver::results_type results) {
                if (ec) {
                    stats_.connect_failures.fetch_add(1, std::memory_order_relaxed);
                    signal_done();
                    return;
                }
                net::async_connect(ws_->next_layer(), results,
                    [this, self](beast::error_code ec, tcp::endpoint) {
                        if (ec) {
                            stats_.connect_failures.fetch_add(1, std::memory_order_relaxed);
                            signal_done();
                            return;
                        }
                        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
                        ws_->async_handshake(config_.host + ":" + std::to_string(config_.port), "/",
                            [this, self](beast::error_code ec) {
                                if (ec) {
                                    stats_.errors.fetch_add(1, std::memory_order_relaxed);
                                    signal_done();
                                    return;
                                }
                                do_join();
                            });
                    });
            });
    }

    void do_join() {
        auto self = shared_from_this();
        std::string nickname = "stress_" + std::to_string(id_);
        nlohmann::json join_msg = {{"type", "join"}, {"nickname", nickname}};
        auto msg = std::make_shared<std::string>(join_msg.dump());

        ws_->async_write(net::buffer(*msg),
            [this, self, msg](beast::error_code ec, std::size_t) {
                if (ec) {
                    stats_.errors.fetch_add(1, std::memory_order_relaxed);
                    signal_done();
                    return;
                }
                // Read join response
                auto buf = std::make_shared<beast::flat_buffer>();
                ws_->async_read(*buf,
                    [this, self, buf](beast::error_code ec, std::size_t) {
                        if (ec) {
                            stats_.errors.fetch_add(1, std::memory_order_relaxed);
                            signal_done();
                            return;
                        }
                        stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                        // Start sending and reading concurrently
                        start_read_loop();
                        do_send_messages();
                    });
            });
    }

    void start_read_loop() {
        auto self = shared_from_this();
        auto buf = std::make_shared<beast::flat_buffer>();
        ws_->async_read(*buf,
            [this, self, buf](beast::error_code ec, std::size_t) {
                if (ec) {
                    // Timeout or disconnect is expected
                    return;
                }
                stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                start_read_loop(); // Continue reading
            });
    }

    void do_send_messages() {
        auto self = shared_from_this();
        std::string payload = random_string(rng_, config_.message_size);
        sent_count_ = 0;
        send_next(payload);
    }

    void send_next(const std::string& payload) {
        if (sent_count_ >= config_.messages_per_client) {
            // All messages sent, wait a bit then close
            auto self = shared_from_this();
            auto timer = std::make_shared<net::steady_timer>(ioc_);
            timer->expires_after(std::chrono::milliseconds(500));
            timer->async_wait([this, self](beast::error_code) {
                do_close();
            });
            return;
        }

        auto self = shared_from_this();
        nlohmann::json msg = {{"type", "send"}, {"text", payload + "_" + std::to_string(sent_count_)}};
        auto serialized = std::make_shared<std::string>(msg.dump());

        ws_->async_write(net::buffer(*serialized),
            [this, self, payload, serialized](beast::error_code ec, std::size_t) {
                if (ec) {
                    stats_.errors.fetch_add(1, std::memory_order_relaxed);
                    do_close();
                    return;
                }
                stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
                sent_count_++;
                send_next(payload);
            });
    }

    void do_close() {
        auto self = shared_from_this();
        ws_->async_close(websocket::close_code::normal,
            [this, self](beast::error_code) {
                signal_done();
            });
    }

    void signal_done() {
        {
            std::lock_guard<std::mutex> lk(done_mutex_);
            done_ = true;
        }
        done_cv_.notify_all();
    }

    net::io_context& ioc_;
    net::strand<net::io_context::executor_type> strand_;
    std::shared_ptr<websocket::stream<tcp::socket>> ws_;
    const Config& config_;
    int id_;
    Stats& stats_;
    std::mt19937 rng_;
    int sent_count_ = 0;
    bool done_ = false;
    std::mutex done_mutex_;
    std::condition_variable done_cv_;
};

void print_stats(const Config& config, Stats& stats, double elapsed_sec) {
    int64_t sent = stats.messages_sent.load();
    int64_t received = stats.messages_received.load();
    int64_t errs = stats.errors.load();
    int64_t conn_fails = stats.connect_failures.load();

    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "         STRESS TEST RESULTS\n";
    std::cout << "========================================\n";
    std::cout << " Config\n";
    std::cout << "   Connections:     " << config.connections << "\n";
    std::cout << "   Msgs/client:     " << config.messages_per_client << "\n";
    std::cout << "   Payload size:    " << config.message_size << " bytes\n";
    std::cout << "----------------------------------------\n";
    std::cout << " Throughput\n";
    std::cout << "   Duration:        " << std::fixed << std::setprecision(2) << elapsed_sec << " sec\n";
    std::cout << "   Messages sent:   " << sent << "\n";
    std::cout << "   Messages recv:   " << received << "\n";
    if (elapsed_sec > 0) {
        std::cout << "   Send rate:       " << static_cast<int64_t>(sent / elapsed_sec) << " msg/sec\n";
        std::cout << "   Broadcast rate:  " << static_cast<int64_t>(received / elapsed_sec) << " msg/sec\n";
    }
    std::cout << "   Errors:          " << errs << "\n";
    std::cout << "   Conn failures:   " << conn_fails << "\n";
    std::cout << "========================================\n";
}

int main(int argc, char* argv[]) {
    auto config = parse_args(argc, argv);

    std::cout << "Stress test: " << config.connections << " clients x "
              << config.messages_per_client << " msgs, payload "
              << config.message_size << " bytes\n";
    std::cout << "Connecting to " << config.host << ":" << config.port << "\n";

    Stats stats;
    auto start_time = std::chrono::steady_clock::now();

    net::io_context ioc;

    std::vector<std::shared_ptr<Client>> clients;
    clients.reserve(config.connections);
    for (int i = 0; i < config.connections; ++i) {
        auto client = std::make_shared<Client>(ioc, config, i, stats);
        client->start(start_time);
        clients.push_back(client);
    }

    // Run io_context in a thread pool
    const unsigned thread_count = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < thread_count; ++i) {
        threads.emplace_back([&ioc]() { ioc.run(); });
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    print_stats(config, stats, elapsed);

    return 0;
}
