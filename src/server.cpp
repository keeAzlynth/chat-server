#include "imchat/server.hpp"
#include "imchat/websocket_session.hpp"

#include <boost/beast/core.hpp>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace imchat {

Server::Server(boost::asio::io_context& io_context,
               const AppConfig& config,
               SessionManager& session_manager)
    : io_context_(io_context)
    , acceptor_(io_context)
    , session_manager_(session_manager)
    , config_(config)
{
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(config_.host, ec);
    if (ec) {
        throw boost::system::system_error(ec);
    }

    const boost::asio::ip::tcp::endpoint endpoint{address, config_.port};
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        throw boost::system::system_error(ec);
    }

    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw boost::system::system_error(ec);
    }

    acceptor_.bind(endpoint, ec);
    if (ec) {
        throw boost::system::system_error(ec);
    }

    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw boost::system::system_error(ec);
    }
}

void Server::run()
{
    do_accept();
}

void Server::stop()
{
    boost::system::error_code ec;
    acceptor_.close(ec);
}

void Server::do_accept()
{
    // Each connection gets its own strand, so session state is only ever
    // touched by one I/O thread at a time.
    acceptor_.async_accept(
        boost::asio::make_strand(io_context_),
        [this](boost::beast::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (ec == boost::asio::error::operation_aborted) return;

            if (ec) {
                // EMFILE and friends must not kill the accept loop.
                spdlog::warn("accept failed: {}", ec.message());
            } else {
                const auto session =
                    std::make_shared<WebSocketSession>(std::move(socket), session_manager_, config_);
                if (session_manager_.add(session)) {
                    session->run();
                }
            }

            if (acceptor_.is_open()) do_accept();
        });
}

} // namespace imchat
