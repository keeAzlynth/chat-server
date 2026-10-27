#include "imchat/websocket_session.hpp"
#include "imchat/session_manager.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <utility>

namespace imchat {
namespace {

std::atomic_uint64_t next_connection_number{1};

std::string make_connection_id()
{
    return "conn_" + std::to_string(next_connection_number.fetch_add(1, std::memory_order_relaxed));
}

// A client that stops reading must not be able to grow our heap without bound.
constexpr std::size_t kMaxQueuedBytes = 8 * 1024 * 1024;

} // namespace

WebSocketSession::WebSocketSession(boost::asio::ip::tcp::socket socket,
                                   SessionManager& session_manager,
                                   const AppConfig& config)
    : websocket_(std::move(socket))
    , session_manager_(session_manager)
    , connection_id_(make_connection_id())
    , max_message_bytes_(config.max_ws_message_bytes)
    , max_queued_bytes_(kMaxQueuedBytes)
{
    // Nagle delays small frames by up to 40 ms — fatal for a chat UI.
    boost::beast::error_code ec;
    websocket_.next_layer().set_option(boost::asio::ip::tcp::no_delay(true), ec);
}

void WebSocketSession::run()
{
    boost::beast::websocket::stream_base::timeout timeout_opt;
    timeout_opt.handshake_timeout = std::chrono::seconds(30);
    timeout_opt.idle_timeout      = std::chrono::seconds(120);
    timeout_opt.keep_alive_pings  = true;
    websocket_.set_option(timeout_opt);

    // Cap a single frame. Beast would otherwise accept 16 MB by default.
    websocket_.read_message_max(max_message_bytes_);

    websocket_.set_option(boost::beast::websocket::stream_base::decorator(
        [](boost::beast::websocket::response_type& response) {
            response.set(boost::beast::http::field::server, "im-chat/3.0");
        }));

    websocket_.async_accept(
        boost::beast::bind_front_handler([self = shared_from_this()](boost::beast::error_code ec) {
            if (ec) {
                spdlog::debug("websocket accept failed: {}", ec.message());
                self->close();
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
        [self = shared_from_this(), text = std::move(text)]() mutable {
            if (self->is_closed()) return;

            const bool idle = self->outgoing_.empty();
            self->queued_bytes_ += text.size();
            self->outgoing_.push_back(std::move(text));

            if (self->queued_bytes_ > self->max_queued_bytes_) {
                spdlog::warn("closing slow consumer {} ({} bytes queued)",
                             self->connection_id_, self->queued_bytes_);
                self->close();
                return;
            }

            if (idle) self->do_write();
        });
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
            spdlog::debug("read failed on {}: {}", connection_id_, ec.message());
        }
        close();
        return;
    }

    if (buffer_.size() > max_message_bytes_) {
        spdlog::warn("{} sent an oversized frame ({} bytes)", connection_id_, buffer_.size());
        close();
        return;
    }

    std::string text(static_cast<const char*>(buffer_.data().data()), buffer_.size());
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
        [self = shared_from_this()](boost::beast::error_code ec, std::size_t) {
            if (ec) {
                self->close();
                return;
            }

            self->queued_bytes_ -= self->outgoing_.front().size();
            self->outgoing_.pop_front();

            if (!self->outgoing_.empty()) self->do_write();
        });
}

void WebSocketSession::close()
{
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    boost::asio::dispatch(websocket_.get_executor(), [self = shared_from_this()]() {
        self->do_close();
    });
}

void WebSocketSession::do_close()
{
    boost::beast::error_code ec;
    if (websocket_.next_layer().is_open()) {
        websocket_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        websocket_.next_layer().close(ec);
    }
    outgoing_.clear();
    queued_bytes_ = 0;
    session_manager_.remove(connection_id_);
}

} // namespace imchat
