#include "lan_chat/server.hpp"

#include <utility>

namespace lan_chat {

Server::Server(lan_chat_server_t server)
    : server_(server), initialized_(true)
{
}

Result<Server> Server::start(const ServerConfig &config)
{
    lan_chat_server_t server{};
    lan_chat_server_config_t c_config{};
    c_config.listen_port = config.listen_port;
    c_config.worker_thread_count = config.worker_thread_count;
    c_config.storage = config.storage;

    const lan_chat_status_t status = lan_chat_server_init(&server, &c_config);
    if (status != LAN_CHAT_STATUS_OK) {
        return Result<Server>::failure(status, lan_chat_status_name(status));
    }

    return Result<Server>::success(Server(server));
}

Server::Server(Server &&other) noexcept
    : server_(other.server_), initialized_(other.initialized_)
{
    other.server_ = {};
    other.initialized_ = false;
}

Server &Server::operator=(Server &&other) noexcept
{
    if (this != &other) {
        if (initialized_) {
            lan_chat_server_shutdown(&server_);
        }
        server_ = other.server_;
        initialized_ = other.initialized_;
        other.server_ = {};
        other.initialized_ = false;
    }
    return *this;
}

Server::~Server()
{
    if (initialized_) {
        lan_chat_server_shutdown(&server_);
    }
}

} // namespace lan_chat
