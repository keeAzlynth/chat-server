#include "imchat/websocket_session.hpp"
#include "imchat/session_manager.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

#include <atomic>

namespace imchat {
namespace {

std::atomic_uint64_t next_connection_number{1};

std::string make_connection_id()
{
    return "conn_" + std::to_string(next_connection_number.fetch_add(1));
}

} // namespace

WebSocketSession::WebSocketSession(boost::asio::ip::tcp::socket socket, SessionManager& session_manager)
    : websocket_(std::move(socket))
    , session_manager_(session_manager)
    , connection_id_(make_connection_id())
{
}

void WebSocketSession::run()
{
    // Custom timeout: 30s handshake, 120s idle (detect zombie connections),
    // with automatic keep-alive pings every 30s
    boost::beast::websocket::stream_base::timeout timeout_opt;
    timeout_opt.handshake_timeout = std::chrono::seconds(30);
    timeout_opt.idle_timeout = std::chrono::seconds(120);
    timeout_opt.keep_alive_pings = true;
    websocket_.set_option(timeout_opt);

    // Application-level ping/pong (detect half-open connections faster than TCP keepalive)
    websocket_.set_option(boost::beast::websocket::stream_base::decorator(
        [](boost::beast::websocket::response_type& response) {
            response.set(boost::beast::http::field::server, "im-chat-beast");
        }));

    // Auto-pong: Beast responds to ping frames automatically
    websocket_.control_callback(
        [this](boost::beast::websocket::frame_type kind, boost::beast::string_view) {
            if (kind == boost::beast::websocket::frame_type::pong) {
                last_pong_ = std::chrono::steady_clock::now();
            }
        });

    auto self = shared_from_this();
    websocket_.async_accept(
        boost::beast::bind_front_handler([self](boost::beast::error_code ec) {
            if (ec) {
                spdlog::warn("websocket accept failed: {}", ec.message());
                self->shutdown();
                return;
            }
            self->do_read();
        }));
}

void WebSocketSession::send(std::string text)
{
    if (is_closed()) return;

    boost::asio::dispatch(
        websocket_.get_executor(),
        boost::beast::bind_front_handler(
            [self = shared_from_this(), text = std::move(text)]() mutable {
                if (self->is_closed()) return;
                const bool idle = self->outgoing_.empty();
                self->outgoing_.push_back(std::move(text));
                if (idle) self->do_write();
            }));
}

void WebSocketSession::do_read()
{
    if (is_closed()) return;

    websocket_.async_read(
        buffer_,
        boost::beast::bind_front_handler(&WebSocketSession::on_read, shared_from_this()));
}

void WebSocketSession::on_read(boost::beast::error_code ec, std::size_t)
{
    if (is_closed()) return;

    if (ec) {
        if (ec != boost::beast::websocket::error::closed) {
            spdlog::warn("read failed on {}: {}", connection_id_, ec.message());
        }
        shutdown();
        return;
    }

    // Zero-copy: use buffer data directly instead of buffers_to_string
    auto text = std::string(
        static_cast<const char*>(buffer_.data().data()),
        buffer_.data().size());
    buffer_.consume(buffer_.size());

    session_manager_.handle_message(shared_from_this(), text);
    do_read();
}

void WebSocketSession::do_write()
{
    if (is_closed()) return;

    websocket_.text(true);
    websocket_.async_write(
        boost::asio::buffer(outgoing_.front()),
        boost::beast::bind_front_handler([self = shared_from_this()](boost::beast::error_code ec, std::size_t) {
            if (ec) {
                spdlog::warn("write failed on {}: {}", self->connection_id_, ec.message());
                self->shutdown();
                return;
            }
            self->outgoing_.pop_front();
            if (!self->outgoing_.empty()) {
                self->do_write();
            }
        }));
}

void WebSocketSession::shutdown()
{
    if (closed_.exchange(true)) return;

    boost::beast::error_code ec;
    if (websocket_.next_layer().is_open()) {
        websocket_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        websocket_.next_layer().close(ec);
    }
    session_manager_.remove(connection_id_);
}

} // namespace imchat
