#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace imchat {

struct AppConfig {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    std::size_t max_connections{5000};
    std::size_t message_max_length{500};
    std::string log_level{"info"};
    std::string log_file{"logs/im-chat.log"};
    std::string db_path{"imchat.db"};
    std::size_t user_cache_size{1000};
    std::size_t message_cache_size{100};
    std::size_t worker_threads{0};  // 0 = auto-detect hardware concurrency
};

[[nodiscard]] AppConfig load_config(std::string_view path);

} // namespace imchat
