#include "imchat/server.hpp"
#include "imchat/websocket_session.hpp"

#include <boost/beast/core.hpp>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace imchat {

Server::Server(boost::asio::io_context& io_context,
               std::string_view host,
               std::uint16_t port,
               SessionManager& session_manager)
    : io_context_(io_context)
    , acceptor_(io_context)
    , session_manager_(session_manager)
{
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(std::string(host), ec);
    if (ec) {
        throw boost::system::system_error(ec);
    }

    const boost::asio::ip::tcp::endpoint endpoint{address, port};
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

void Server::do_accept()
{
    acceptor_.async_accept(
        boost::asio::make_strand(io_context_),
        [this](boost::beast::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (ec) {
                spdlog::warn("accept failed: {}", ec.message());
            } else {
                const auto session = std::make_shared<WebSocketSession>(std::move(socket), session_manager_);
                if (session_manager_.add(session)) {
                    session->run();
                }
            }

            do_accept();
        });
}

} // namespace imchat

