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
    config.host = json.value("host", config.host);
    config.port = json.value("port", config.port);
    config.max_connections = json.value("max_connections", config.max_connections);
    config.message_max_length = json.value("message_max_length", config.message_max_length);
    config.log_level = json.value("log_level", config.log_level);
    config.log_file = json.value("log_file", config.log_file);
    config.db_path = json.value("db_path", config.db_path);
    config.user_cache_size = json.value("user_cache_size", config.user_cache_size);
    config.message_cache_size = json.value("message_cache_size", config.message_cache_size);
    config.worker_threads = json.value("worker_threads", config.worker_threads);
    return config;
}

} // namespace imchat
