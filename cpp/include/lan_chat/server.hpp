#pragma once

#include <cstdint>

#include "lan_chat/result.hpp"

extern "C" {
#include "lan_chat/server/server.h"
}

namespace lan_chat {

struct ServerConfig {
    std::uint16_t listen_port = 7777;
    std::uint32_t worker_thread_count = 4;
    lan_chat_storage_t *storage = nullptr;
};

class Server {
public:
    static Result<Server> start(const ServerConfig &config);

    Server(Server &&other) noexcept;
    Server &operator=(Server &&other) noexcept;
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    ~Server();

private:
    explicit Server(lan_chat_server_t server);

    lan_chat_server_t server_{};
    bool initialized_ = false;
};

} // namespace lan_chat
