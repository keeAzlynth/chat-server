#pragma once

#include "imchat/config.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>

namespace imchat {

class SessionManager;

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(boost::asio::ip::tcp::socket socket, SessionManager& session_manager,
                     const AppConfig& config);

    void run();

    // Thread-safe: hops onto the socket's executor before touching the queue.
    void send(std::string text);

    // Thread-safe and idempotent.
    void close();

    [[nodiscard]] const std::string& connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }

private:
    void do_read();
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);
    void do_write();
    void do_close();

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket_;
    boost::beast::flat_buffer buffer_;
    SessionManager&   session_manager_;
    std::deque<std::string> outgoing_;
    std::string       connection_id_;
    std::size_t       queued_bytes_{0};
    std::size_t       max_message_bytes_;
    std::size_t       max_queued_bytes_;
    std::atomic<bool> closed_{false};
};

} // namespace imchat
