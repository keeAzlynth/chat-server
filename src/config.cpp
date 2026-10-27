#include "imchat/config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace imchat {

AppConfig load_config(std::string_view path)
{
    std::string path_str(path);
    std::ifstream input(path_str);
    if (!input) {
        throw std::runtime_error("无法打开配置文件: " + path_str);
    }

    const auto json = nlohmann::json::parse(input);
    AppConfig config;

    // Network
    config.host           = json.value("host", config.host);
    config.port           = json.value("port", config.port);
    config.http_port      = json.value("http_port", config.http_port);
    config.enable_http    = json.value("enable_http", config.enable_http);
    config.web_root       = json.value("web_root", config.web_root);
    config.upload_dir     = json.value("upload_dir", config.upload_dir);
    config.enable_cors    = json.value("enable_cors", config.enable_cors);

    // Limits
    config.max_connections       = json.value("max_connections", config.max_connections);
    config.message_max_length    = json.value("message_max_length", config.message_max_length);
    config.max_ws_message_bytes  = json.value("max_ws_message_bytes", config.max_ws_message_bytes);
    config.max_upload_bytes      = json.value("max_upload_bytes", config.max_upload_bytes);
    config.max_history_limit     = json.value("max_history_limit", config.max_history_limit);

    // Rate limiting
    config.rate_limit_msgs_per_sec = json.value("rate_limit_msgs_per_sec", config.rate_limit_msgs_per_sec);
    config.rate_limit_burst        = json.value("rate_limit_burst", config.rate_limit_burst);
    config.auth_attempts_per_sec   = json.value("auth_attempts_per_sec", config.auth_attempts_per_sec);
    config.auth_burst              = json.value("auth_burst", config.auth_burst);

    // Logging / storage
    config.log_level          = json.value("log_level", config.log_level);
    config.log_file           = json.value("log_file", config.log_file);
    config.db_path            = json.value("db_path", config.db_path);
    config.user_cache_size    = json.value("user_cache_size", config.user_cache_size);
    config.message_cache_size = json.value("message_cache_size", config.message_cache_size);

    // Threading
    config.io_threads     = json.value("io_threads", config.io_threads);
    config.worker_threads = json.value("worker_threads", config.worker_threads);

    // Sanity clamp — a 0-length message limit or absurd upload size is always a config bug.
    if (config.message_max_length < 16) config.message_max_length = 16;
    if (config.max_ws_message_bytes < 1024) config.max_ws_message_bytes = 1024;
    if (config.max_history_limit == 0) config.max_history_limit = 1;

    return config;
}

} // namespace imchat
