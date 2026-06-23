#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lan_chat/result.hpp"

namespace lan_chat {

struct ClientConfig {
    std::string server_host = "127.0.0.1";
    std::uint16_t server_port = 7777;
};

struct Message {
    std::uint64_t message_id = 0;
    std::uint64_t sender_id = 0;
    std::uint64_t receiver_id = 0;
    std::string text;
};

class Client {
public:
    static Result<Client> connect(const ClientConfig &config);

    Result<void> sendText(std::uint64_t to_user_id, std::string_view text);
    Result<std::vector<Message>> pullPendingDeliveries();

private:
    explicit Client(ClientConfig config);

    ClientConfig config_;
};

} // namespace lan_chat
