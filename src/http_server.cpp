#include "imchat/http_server.hpp"
#include "imchat/ids.hpp"
#include "imchat/session_manager.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace imchat {
namespace {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;
namespace fs    = std::filesystem;

const auto kStartedAt = std::chrono::steady_clock::now();

std::string_view mime_type(std::string_view path)
{
    struct Entry { std::string_view ext; std::string_view mime; };
    static constexpr std::array<Entry, 14> kTypes{{
        {".html", "text/html; charset=utf-8"},
        {".htm",  "text/html; charset=utf-8"},
        {".js",   "application/javascript; charset=utf-8"},
        {".mjs",  "application/javascript; charset=utf-8"},
        {".css",  "text/css; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".svg",  "image/svg+xml"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".webp", "image/webp"},
        {".ico",  "image/x-icon"},
        {".woff2", "font/woff2"},
    }};

    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return "application/octet-stream";

    const auto ext = path.substr(dot);
    for (const auto& entry : kTypes) {
        if (entry.ext == ext) return entry.mime;
    }
    return "application/octet-stream";
}

// Rejects traversal attempts and absolute paths before touching the filesystem.
std::optional<std::string> safe_relative_target(std::string_view target)
{
    const auto query = target.find('?');
    if (query != std::string_view::npos) target = target.substr(0, query);

    if (target.empty() || target.front() != '/') return std::nullopt;
    if (target.find("..") != std::string_view::npos) return std::nullopt;
    if (target.find('\\') != std::string_view::npos) return std::nullopt;
    if (target.size() > 512) return std::nullopt;

    auto relative = std::string(target.substr(1));
    if (relative.empty()) relative = "index.html";
    return relative;
}

[[nodiscard]] int hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool write_file(const fs::path& path, std::string_view data){
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

std::string sanitize_extension(std::string_view name){
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return {};

    auto ext = std::string(name.substr(dot));
    if (ext.size() < 2 || ext.size() > 8) return {};
    for (char c : ext) {
        const bool ok = (c == '.') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9');
        if (!ok) return {};
    }
    return ext;
}

// ── One connection, keep-alive, one request at a time ────

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket&& socket, const AppConfig& config, SessionManager& sessions)
        : stream_(std::move(socket))
        , config_(config)
        , sessions_(sessions)
    {
        beast::error_code ec;
        stream_.socket().set_option(tcp::no_delay(true), ec);
    }
    void run()
    {
        net::dispatch(stream_.get_executor(),
                      beast::bind_front_handler(&HttpSession::do_read, shared_from_this()));
    }

private:
    void do_read()
    {
        parser_.emplace();
        parser_->body_limit(static_cast<std::uint64_t>(config_.max_upload_bytes) + 64 * 1024);
        http::async_read(stream_, buffer_, *parser_,
                         beast::bind_front_handler(&HttpSession::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t)
    {
        if (ec == http::error::end_of_stream) return do_close();

        if (ec == http::error::body_limit) {
            return send_json(http::status::payload_too_large, {{"error", "文件过大"}}, false);
        }
        if (ec) {
            return send_json(http::status::bad_request, {{"error", ec.message()}}, false);
        }

        handle(parser_->release());
    }

    // ── Routing ──────────────────────────────────────────

    void handle(http::request<http::string_body>&& req)
    {
        if (req.method() == http::verb::options) {
            return send_json(http::status::ok, nlohmann::json::object(), req.keep_alive());
        }

        std::string target(req.target());
        const auto path = target.substr(0, target.find('?'));

        if (path == "/api/health")  return handle_health(req);
        if (path == "/api/stats")   return handle_stats(req);
        if (path == "/api/upload")  return handle_upload(std::move(req));
        if (path == "/config.js")   return handle_config_js(req);

        if (path.starts_with("/uploads/")) {
            return serve_file(req, config_.upload_dir, path.substr(std::string("/uploads/").size()),
                              false);
        }

        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            return send_json(http::status::method_not_allowed, {{"error", "方法不支持"}},
                             req.keep_alive());
        }

        serve_file(req, config_.web_root, path, true);
    }

    void handle_health(const http::request<http::string_body>& req)
    {
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - kStartedAt).count();

        send_json(http::status::ok, {
            {"status", "ok"},
            {"version", "3.0.0"},
            {"uptimeSeconds", uptime},
        }, req.keep_alive());
    }

    void handle_stats(const http::request<http::string_body>& req)
    {
        const auto metrics = sessions_.metrics();
        const auto room    = sessions_.room_stats();
        const auto uptime  = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - kStartedAt).count();

        send_json(http::status::ok, {
            {"uptimeSeconds", uptime},
            {"connections", metrics.connections},
            {"onlineUsers", room.online_users},
            {"knownUsers", room.users},
            {"groups", room.groups},
            {"commands", metrics.commands},
            {"messagesIn", metrics.messages_in},
            {"messagesOut", metrics.messages_out},
            {"bytesIn", metrics.bytes_in},
            {"bytesOut", metrics.bytes_out},
            {"rejected", metrics.rejected},
            {"queued", metrics.queued},
        }, req.keep_alive());
    }

    void handle_config_js(const http::request<http::string_body>& req)
    {
        nlohmann::json config{
            {"wsPort", config_.port},
            {"wsPath", "/"},
            {"httpPort", config_.http_port},
            {"maxMessageLength", config_.message_max_length},
            {"maxUploadBytes", config_.max_upload_bytes},
        };

        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "application/javascript; charset=utf-8");
        res.set(http::field::cache_control, "no-store");
        res.body() = "window.IM_CONFIG = " + config.dump() + ";\n";
        res.content_length(res.body().size());
        res.keep_alive(req.keep_alive());
        send(std::move(res));
    }

    void handle_upload(http::request<http::string_body>&& req)
    {
        if (req.method() != http::verb::post && req.method() != http::verb::put) {
            return send_json(http::status::method_not_allowed, {{"error", "方法不支持"}},
                             req.keep_alive());
        }

        if (req.body().empty()) {
            return send_json(http::status::bad_request, {{"error", "空文件"}}, req.keep_alive());
        }

        std::string original_name(req["X-Filename"]);
        if (original_name.empty()) {
            const auto target = std::string(req.target());
            const auto pos    = target.find("name=");
            if (pos != std::string::npos) {
                original_name = target.substr(pos + 5);
                // Percent-decode the minimal set browsers produce.
                std::string decoded;
                for (std::size_t i = 0; i < original_name.size(); ++i) {
                    const char c = original_name[i];
                    if (c == '%' && i + 2 < original_name.size()) {
                        const int hi = hex_digit(original_name[i + 1]);
                        const int lo = hex_digit(original_name[i + 2]);
                        if (hi >= 0 && lo >= 0) {
                            decoded.push_back(static_cast<char>(hi * 16 + lo));
                            i += 2;
                            continue;
                        }
                    }
                    decoded.push_back(c == '+' ? ' ' : c);
                }
                original_name = std::move(decoded);
            }
        }
        if (original_name.empty()) original_name = "file";

        const auto stored_name = ids::prefixed("f_", 8) + sanitize_extension(original_name);
        const auto path        = fs::path(config_.upload_dir) / stored_name;

        if (!write_file(path, req.body())) {
            return send_json(http::status::internal_server_error, {{"error", "保存失败"}},
                             req.keep_alive());
        }

        send_json(http::status::ok, {
            {"url", "/uploads/" + stored_name},
            {"name", original_name},
            {"size", req.body().size()},
        }, req.keep_alive());
    }

    // ── Static files ─────────────────────────────────────

    void serve_file(const http::request<http::string_body>& req, const std::string& root,
                    std::string_view relative, bool from_web_root)
    {
        // `relative` may already start with '/', which would turn the joined
        // path into an absolute one and escape the root.
        std::string trimmed(relative);
        while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(trimmed.begin());

        const auto safe = safe_relative_target("/" + trimmed);
        if (!safe) {
            return send_json(http::status::bad_request, {{"error", "非法路径"}}, req.keep_alive());
        }

        std::error_code ec;
        const auto base = fs::weakly_canonical(fs::path(root), ec);
        if (ec) {
            return send_json(http::status::not_found, {{"error", "目录不存在"}}, req.keep_alive());
        }

        auto full = fs::weakly_canonical(base / *safe, ec);
        if (ec || !full.string().starts_with(base.string())) {
            return send_json(http::status::forbidden, {{"error", "非法路径"}}, req.keep_alive());
        }

        if (!fs::is_regular_file(full, ec)) {
            // Unknown asset: fall back to index.html so the SPA can route.
            if (!from_web_root) {
                return send_json(http::status::not_found, {{"error", "文件不存在"}},
                                 req.keep_alive());
            }
            full = base / "index.html";
            if (!fs::is_regular_file(full, ec)) {
                return send_json(http::status::not_found,
                                 {{"error", "前端资源未找到，请确认 web_root 配置"}},
                                 req.keep_alive());
            }
        }

        std::ifstream in(full, std::ios::binary);
        if (!in) {
            return send_json(http::status::not_found, {{"error", "无法读取文件"}}, req.keep_alive());
        }

        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "im-chat/3.0");
        res.set(http::field::content_type, mime_type(full.string()));
        res.set(http::field::cache_control, "no-cache");
        res.body() = std::move(body);
        res.content_length(res.body().size());
        res.keep_alive(req.keep_alive());

        send(std::move(res));
    }

    // ── Responses ────────────────────────────────────────

    void send_json(http::status status, const nlohmann::json& payload, bool keep_alive)
    {
        http::response<http::string_body> res{status, 11};
        res.set(http::field::server, "im-chat/3.0");
        res.set(http::field::content_type, "application/json; charset=utf-8");
        res.set(http::field::cache_control, "no-store");
        if (config_.enable_cors) {
            res.set(http::field::access_control_allow_origin, "*");
            res.set(http::field::access_control_allow_headers, "*");
            res.set(http::field::access_control_allow_methods, "GET, POST, PUT, OPTIONS");
        }
        res.body() = payload.dump();
        res.content_length(res.body().size());
        res.keep_alive(keep_alive);
        send(std::move(res));
    }

    void send(http::response<http::string_body>&& res)
    {
        const bool keep_alive = res.keep_alive();
        auto message = std::make_shared<http::response<http::string_body>>(std::move(res));

        http::async_write(stream_, *message,
            [self = shared_from_this(), message, keep_alive](beast::error_code ec, std::size_t) {
                if (ec) return self->do_close();
                if (!keep_alive) return self->do_close();
                self->do_read();
            });
    }

    void do_close()
    {
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
        stream_.socket().close(ec);
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> parser_;
    AppConfig        config_;
    SessionManager&  sessions_;
};

} // namespace

// ── HttpServer ───────────────────────────────────────────

HttpServer::HttpServer(boost::asio::io_context& io_context, const AppConfig& config,
                       SessionManager& sessions)
    : io_context_(io_context)
    , acceptor_(io_context)
    , config_(config)
    , sessions_(sessions)
{
    namespace fs = std::filesystem;
    std::error_code fs_ec;
    fs::create_directories(config_.upload_dir, fs_ec);
    if (!fs::exists(config_.web_root)) {
        spdlog::warn("web_root '{}' 不存在，HTTP 将只提供 API", config_.web_root);
    }

    boost::system::error_code ec;
    const auto address = net::ip::make_address(config_.host, ec);
    if (ec) throw boost::system::system_error(ec);

    const tcp::endpoint endpoint{address, config_.http_port};
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) throw boost::system::system_error(ec);

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec) throw boost::system::system_error(ec);

    acceptor_.bind(endpoint, ec);
    if (ec) throw boost::system::system_error(ec);

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) throw boost::system::system_error(ec);

    port_ = acceptor_.local_endpoint(ec).port();
}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::run()
{
    do_accept();
}

void HttpServer::stop()
{
    if (stopped_.exchange(true)) return;
    beast::error_code ec;
    acceptor_.close(ec);
}

void HttpServer::do_accept()
{
    acceptor_.async_accept(
        net::make_strand(io_context_),
        [this](beast::error_code ec, tcp::socket socket) {
            if (ec == net::error::operation_aborted) return;

            if (ec) {
                spdlog::warn("http accept failed: {}", ec.message());
            } else {
                std::make_shared<HttpSession>(std::move(socket), config_, sessions_)->run();
            }

            if (acceptor_.is_open()) do_accept();
        });
}

} // namespace imchat
