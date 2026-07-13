#include "imchat/config.hpp"
#include "imchat/database.hpp"
#include "imchat/server.hpp"
#include "imchat/session_manager.hpp"

#include <boost/asio.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

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

    auto logger = std::make_shared<spdlog::logger>("imchat", spdlog::sinks_init_list{file_sink, console_sink});
    logger->set_level(spdlog::level::from_str(config.log_level));
    spdlog::set_default_logger(logger);
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string config_path = argc > 1 ? argv[1] : "config/config.json";

    try {
        const auto config = imchat::load_config(config_path);
        setup_logging(config);

        auto db = std::make_shared<imchat::Database>();
        if (!db->open(config.db_path)) {
            std::cerr << "Failed to open database\n";
            return 1;
        }

        spdlog::info("Database initialized: {}", config.db_path);

        boost::asio::io_context io_context;

        // Keep io_context alive even when no work is pending
        auto work_guard = boost::asio::make_work_guard(io_context);

        // Async signal handling (C++20 modern approach)
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code& ec, int sig) {
            if (!ec) {
                spdlog::info("Received signal {}, shutting down...", sig);
                work_guard.reset(); // Allow io_context to stop naturally
                io_context.stop();
            }
        });

        imchat::SessionManager session_manager(config, db);
        imchat::Server server(io_context, config.host, config.port, session_manager);

        server.run();
        spdlog::info("IM chat server listening on {}:{}", config.host, config.port);

        const auto thread_count = config.worker_threads > 0
            ? config.worker_threads
            : std::max(1u, std::thread::hardware_concurrency());
        spdlog::info("Starting {} worker threads", thread_count);

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (unsigned i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }

        // Setup signal handling for graceful shutdown
        spdlog::info("Server ready. Press Ctrl+C to stop.");

        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        spdlog::info("Server stopped gracefully.");
    } catch (const std::exception& ex) {
        std::cerr << "启动失败: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
