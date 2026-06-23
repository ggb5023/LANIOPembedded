#include "lan_chat/client.hpp"

#include <utility>

namespace lan_chat {

Client::Client(ClientConfig config)
    : config_(std::move(config))
{
}

Result<Client> Client::connect(const ClientConfig &config)
{
    if (config.server_host.empty() || config.server_port == 0) {
        return Result<Client>::failure(LAN_CHAT_STATUS_INVALID_ARGUMENT, "invalid client config");
    }

    return Result<Client>::success(Client(config));
}

Result<void> Client::sendText(std::uint64_t to_user_id, std::string_view text)
{
    if (to_user_id == 0 || text.empty()) {
        return Result<void>::failure(LAN_CHAT_STATUS_INVALID_ARGUMENT, "invalid message");
    }

    return Result<void>::failure(LAN_CHAT_STATUS_NOT_IMPLEMENTED, "sendText is not implemented in Phase 1 skeleton");
}

Result<std::vector<Message>> Client::pullPendingDeliveries()
{
    return Result<std::vector<Message>>::success({});
}

} // namespace lan_chat
