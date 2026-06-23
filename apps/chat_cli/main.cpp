#include <iostream>

#include "lan_chat/client.hpp"

int main()
{
    lan_chat::ClientConfig config{};
    auto client = lan_chat::Client::connect(config);
    if (!client.ok()) {
        std::cerr << "failed to initialize client: " << client.message() << '\n';
        return 1;
    }

    std::cout << "chat_cli skeleton initialized for " << config.server_host << ':' << config.server_port << '\n';
    return 0;
}
