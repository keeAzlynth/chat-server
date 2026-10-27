#pragma once

#include "imchat/config.hpp"
#include "imchat/session_manager.hpp"

#include <boost/asio.hpp>

#include <cstdint>
#include <string_view>

namespace imchat {

class Server {
public:
    Server(boost::asio::io_context& io_context,
           const AppConfig& config,
           SessionManager& session_manager);

    void run();

    // Stops accepting new connections; existing sessions are closed by the
    // session manager during shutdown.
    void stop();

private:
    void do_accept();

    boost::asio::io_context&       io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    SessionManager&                session_manager_;
    AppConfig                      config_;
};

} // namespace imchat
