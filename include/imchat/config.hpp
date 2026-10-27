#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace imchat {

struct AppConfig {
    // ── Network ──────────────────────────────────────────
    std::string   host{"0.0.0.0"};
    std::uint16_t port{8080};              // WebSocket
    std::uint16_t http_port{8081};         // HTTP (web client + REST)
    bool          enable_http{true};
    std::string   web_root{"web"};         // static assets
    std::string   upload_dir{"uploads"};   // user files
    bool          enable_cors{true};

    // ── Limits ───────────────────────────────────────────
    std::size_t max_connections{5000};
    std::size_t message_max_length{500};
    std::size_t max_ws_message_bytes{64 * 1024};
    std::size_t max_upload_bytes{8 * 1024 * 1024};
    std::size_t max_history_limit{200};

    // ── Rate limiting ────────────────────────────────────
    double rate_limit_msgs_per_sec{10.0};
    double rate_limit_burst{20.0};
    double auth_attempts_per_sec{1.0};
    double auth_burst{5.0};

    // ── Logging / storage ────────────────────────────────
    std::string log_level{"info"};
    std::string log_file{"logs/im-chat.log"};
    std::string db_path{"imchat.db"};
    std::size_t user_cache_size{4096};
    std::size_t message_cache_size{256};

    // ── Threading (0 = auto) ─────────────────────────────
    std::size_t io_threads{0};
    std::size_t worker_threads{0};
};

[[nodiscard]] AppConfig load_config(std::string_view path);

} // namespace imchat
