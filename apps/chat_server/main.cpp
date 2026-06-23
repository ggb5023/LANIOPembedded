#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

extern "C" {
#include "lan_chat/core/status.h"
#include "lan_chat/server/server.h"
#include "lan_chat/storage/memory_storage.h"
#include "lan_chat/storage/mysql_storage.h"
}

namespace {

volatile std::sig_atomic_t g_running = 1;

struct Options {
    std::string storage = "memory";
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    std::string mysql_host = "127.0.0.1";
    std::uint16_t mysql_port = 3306;
    std::string mysql_database = "lan_chat";
    std::string mysql_user = "root";
    std::string mysql_password;
};

void handle_signal(int)
{
    g_running = 0;
}

const char *env_or_empty(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr ? value : "";
}

bool parse_u16(const std::string &text, std::uint16_t *out_value)
{
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 10);
        if (consumed != text.size() || value > 65535UL) {
            return false;
        }
        *out_value = static_cast<std::uint16_t>(value);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

void print_usage()
{
    std::cout
        << "Usage:\n"
        << "  chat_server [--storage memory|mysql] [--host 127.0.0.1] [--port 7777]\n"
        << "              [--mysql-host 127.0.0.1] [--mysql-port 3306]\n"
        << "              [--mysql-database lan_chat] [--mysql-user root]\n"
        << "              [--mysql-password <password>]\n";
}

bool parse_options(int argc, char **argv, Options *options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            ++i;
            return argv[i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return false;
        }
        if (arg == "--storage") {
            const char *value = require_value("--storage");
            if (value == nullptr) {
                return false;
            }
            options->storage = value;
        } else if (arg == "--host") {
            const char *value = require_value("--host");
            if (value == nullptr) {
                return false;
            }
            options->host = value;
        } else if (arg == "--port") {
            const char *value = require_value("--port");
            if (value == nullptr || !parse_u16(value, &options->port)) {
                std::cerr << "invalid --port\n";
                return false;
            }
        } else if (arg == "--mysql-host") {
            const char *value = require_value("--mysql-host");
            if (value == nullptr) {
                return false;
            }
            options->mysql_host = value;
        } else if (arg == "--mysql-port") {
            const char *value = require_value("--mysql-port");
            if (value == nullptr || !parse_u16(value, &options->mysql_port)) {
                std::cerr << "invalid --mysql-port\n";
                return false;
            }
        } else if (arg == "--mysql-database") {
            const char *value = require_value("--mysql-database");
            if (value == nullptr) {
                return false;
            }
            options->mysql_database = value;
        } else if (arg == "--mysql-user") {
            const char *value = require_value("--mysql-user");
            if (value == nullptr) {
                return false;
            }
            options->mysql_user = value;
        } else if (arg == "--mysql-password") {
            const char *value = require_value("--mysql-password");
            if (value == nullptr) {
                return false;
            }
            options->mysql_password = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return false;
        }
    }

    if (options->storage != "memory" && options->storage != "mysql") {
        std::cerr << "--storage must be memory or mysql\n";
        return false;
    }
    if (options->host.empty()) {
        std::cerr << "--host cannot be empty\n";
        return false;
    }
    if (options->storage == "mysql" && options->mysql_password.empty()) {
        options->mysql_password = env_or_empty("LAN_CHAT_MYSQL_PASSWORD");
    }
    return true;
}

lan_chat_status_t open_storage(const Options &options, lan_chat_storage_t **out_storage)
{
    if (options.storage == "memory") {
        return lan_chat_storage_memory_open(out_storage);
    }

    lan_chat_storage_config_t config{};
    config.kind = LAN_CHAT_STORAGE_KIND_MYSQL;
    config.host = options.mysql_host.c_str();
    config.port = options.mysql_port;
    config.database = options.mysql_database.c_str();
    config.user = options.mysql_user.c_str();
    config.password = options.mysql_password.c_str();
    config.connection_pool_size = 4;
    return lan_chat_storage_mysql_open(&config, out_storage);
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    lan_chat_storage_t *storage = nullptr;
    lan_chat_server_t server{};

    if (!parse_options(argc, argv, &options)) {
        return 1;
    }

    const lan_chat_status_t storage_status = open_storage(options, &storage);
    if (storage_status != LAN_CHAT_STATUS_OK) {
        std::cerr << "failed to initialize storage: " << lan_chat_status_name(storage_status) << '\n';
        return 1;
    }

    lan_chat_server_config_t server_config{};
    server_config.listen_host = options.host.c_str();
    server_config.listen_port = options.port;
    server_config.worker_thread_count = 1;
    server_config.storage = storage;

    const lan_chat_status_t server_status = lan_chat_server_init(&server, &server_config);
    if (server_status != LAN_CHAT_STATUS_OK) {
        std::cerr << "failed to initialize server: " << lan_chat_status_name(server_status) << '\n';
        lan_chat_storage_close(storage);
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "CHAT_SERVER_READY host=" << options.host
              << " port=" << lan_chat_server_port(&server)
              << " storage=" << options.storage << std::endl;

    while (g_running) {
        const lan_chat_status_t status = lan_chat_server_run_once(&server);
        if (status != LAN_CHAT_STATUS_OK) {
            std::cerr << "server loop failed: " << lan_chat_status_name(status) << '\n';
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    lan_chat_server_shutdown(&server);
    lan_chat_storage_close(storage);
    return 0;
}
