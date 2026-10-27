#pragma once

#include "imchat/config.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace imchat {

class SessionManager;

// Serves the web client, a health/metrics API and file uploads.
//
// The WebSocket endpoint stays on `port` so existing clients (including the
// Android app) keep working; the browser talks to this one.
class HttpServer {
public:
    HttpServer(boost::asio::io_context& io_context, const AppConfig& config, SessionManager& sessions);
    ~HttpServer();

    void run();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void do_accept();

    boost::asio::io_context&       io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    AppConfig                      config_;
    SessionManager&                sessions_;
    std::uint16_t                  port_{0};
    std::atomic<bool>              stopped_{false};
};

} // namespace imchat
