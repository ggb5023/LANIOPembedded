#include <iostream>

#include "lan_chat/server.hpp"

extern "C" {
#include "lan_chat/storage/mysql_storage.h"
}

int main()
{
    lan_chat_storage_config_t storage_config{};
    storage_config.kind = LAN_CHAT_STORAGE_KIND_MYSQL;
    storage_config.host = "127.0.0.1";
    storage_config.port = 3306;
    storage_config.database = "lan_chat";
    storage_config.user = "root";
    storage_config.password = "";
    storage_config.connection_pool_size = 4;

    lan_chat_storage_t *storage = nullptr;
    const lan_chat_status_t storage_status = lan_chat_storage_mysql_open(&storage_config, &storage);
    if (storage_status != LAN_CHAT_STATUS_OK) {
        std::cerr << "failed to initialize MySQL storage: " << lan_chat_status_name(storage_status) << '\n';
        return 1;
    }

    lan_chat::ServerConfig server_config{};
    server_config.listen_port = 7777;
    server_config.worker_thread_count = 4;
    server_config.storage = storage;

    auto server = lan_chat::Server::start(server_config);
    if (!server.ok()) {
        std::cerr << "failed to initialize server: " << server.message() << '\n';
        lan_chat_storage_close(storage);
        return 1;
    }

    std::cout << "chat_server skeleton initialized on port " << server_config.listen_port << '\n';
    lan_chat_storage_close(storage);
    return 0;
}
