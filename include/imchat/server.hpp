#pragma once

#include "imchat/session_manager.hpp"

#include <boost/asio.hpp>

#include <cstdint>
#include <string_view>

namespace imchat {

class Server {
public:
    Server(boost::asio::io_context& io_context,
           std::string_view host,
           std::uint16_t port,
           SessionManager& session_manager);

    void run();

private:
    void do_accept();

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    SessionManager& session_manager_;
};

} // namespace imchat
