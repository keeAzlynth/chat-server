#include "imchat/config.hpp"
#include "imchat/database.hpp"
#include "imchat/http_server.hpp"
#include "imchat/server.hpp"
#include "imchat/session_manager.hpp"

#include <boost/asio.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void setup_logging(const imchat::AppConfig& config)
{
    const auto log_path = std::filesystem::path(config.log_file);
    if (log_path.has_parent_path()) {
        std::filesystem::create_directories(log_path.parent_path());
    }

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        config.log_file, 1024 * 1024 * 5, 3);
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);

    auto logger = std::make_shared<spdlog::logger>(
        "imchat", spdlog::sinks_init_list{file_sink, console_sink});
    logger->set_level(spdlog::level::from_str(config.log_level));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string config_path = argc > 1 ? argv[1] : "config/config.json";

    try {
        const auto config = imchat::load_config(config_path);
        setup_logging(config);

        auto db = std::make_shared<imchat::Database>(config.user_cache_size, config.message_cache_size);
        if (!db->open(config.db_path)) {
            std::cerr << "Failed to open database: " << config.db_path << '\n';
            return 1;
        }
        spdlog::info("database ready: {}", config.db_path);

        boost::asio::io_context io_context;
        auto work_guard = boost::asio::make_work_guard(io_context);

        imchat::SessionManager session_manager(config, db);
        imchat::Server server(io_context, config, session_manager);

        std::unique_ptr<imchat::HttpServer> http_server;
        if (config.enable_http && config.http_port != config.port) {
            http_server = std::make_unique<imchat::HttpServer>(io_context, config, session_manager);
        } else if (config.http_port == config.port) {
            spdlog::warn("http_port 与 port 相同，HTTP 服务已禁用");
        }

        server.run();
        if (http_server) http_server->run();

        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code& ec, int sig) {
            if (ec) return;
            spdlog::info("received signal {}, shutting down", sig);

            // Stop accepting first, then let queued work and in-flight writes
            // finish before tearing the event loop down.
            server.stop();
            if (http_server) http_server->stop();
            session_manager.stop();

            auto timer = std::make_shared<boost::asio::steady_timer>(
                io_context, std::chrono::milliseconds(200));
            timer->async_wait([&, timer](const boost::system::error_code&) {
                work_guard.reset();
                io_context.stop();
            });
        });

        const auto hardware = std::max(2u, std::thread::hardware_concurrency());
        const auto io_threads = config.io_threads > 0
            ? static_cast<unsigned>(config.io_threads)
            : std::min(hardware, 8u);   // 再多也只是空转，瓶颈在数据库

        spdlog::info("WebSocket : ws://{}:{}", config.host, config.port);
        if (http_server) {
            spdlog::info("Web client: http://{}:{}/  (静态目录: {})",
                         config.host, http_server->port(), config.web_root);
        }
        spdlog::info("I/O threads: {}", io_threads);

        std::vector<std::thread> threads;
        threads.reserve(io_threads);
        for (unsigned i = 0; i < io_threads; ++i) {
            threads.emplace_back([&io_context] { io_context.run(); });
        }

        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }

        session_manager.stop();
        db->close();
        spdlog::info("server stopped");
    } catch (const std::exception& ex) {
        std::cerr << "启动失败: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
