#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <string>

namespace imchat {

class SessionManager;

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(boost::asio::ip::tcp::socket socket, SessionManager& session_manager);

    void run();
    void send(std::string text);
    const std::string& connection_id() const noexcept { return connection_id_; }
    bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }

private:
    void do_read();
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);
    void do_write();
    void shutdown();

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket_;
    boost::beast::flat_buffer buffer_;
    SessionManager& session_manager_;
    std::deque<std::string> outgoing_;
    std::string connection_id_;
    std::atomic<bool> closed_{false};
    std::chrono::steady_clock::time_point last_pong_{std::chrono::steady_clock::now()};
};

} // namespace imchat
