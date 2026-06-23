#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

#include "lan_chat/storage/mysql_storage.h"

namespace {

const char *env_or_default(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : fallback;
}

uint16_t env_port_or_default(const char *name, uint16_t fallback)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    const int parsed = std::atoi(value);
    return parsed > 0 && parsed <= 65535 ? static_cast<uint16_t>(parsed) : fallback;
}

void require_status(lan_chat_status_t actual, lan_chat_status_t expected, const char *label)
{
    if (actual != expected) {
        std::cerr << label << ": expected " << lan_chat_status_name(expected)
                  << ", got " << lan_chat_status_name(actual) << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void require_true(bool condition, const char *label)
{
    if (!condition) {
        std::cerr << label << '\n';
        std::exit(EXIT_FAILURE);
    }
}

lan_chat_user_id_t create_account(
    lan_chat_storage_t *storage,
    const char *username,
    const char *nickname,
    const char *hash,
    const char *salt)
{
    lan_chat_account_create_t account{};
    lan_chat_user_id_t user_id = 0;
    account.username = username;
    account.nickname = nickname;
    account.password_hash = hash;
    account.password_salt = salt;
    require_status(lan_chat_storage_create_account(storage, &account, &user_id), LAN_CHAT_STATUS_OK, "create account");
    require_true(user_id != 0, "created user_id is zero");
    return user_id;
}

void run_storage_contract(lan_chat_storage_t *storage)
{
    const std::string suffix = std::to_string(static_cast<unsigned long long>(std::time(nullptr))) +
        "_" + std::to_string(static_cast<unsigned long long>(std::rand()));
    const std::string alice = "it_alice_" + suffix;
    const std::string bob = "it_bob_" + suffix;
    lan_chat_user_id_t alice_id = create_account(storage, alice.c_str(), "Alice", "hash-alice", "salt-alice");
    lan_chat_user_id_t bob_id = create_account(storage, bob.c_str(), nullptr, "hash-bob", "salt-bob");
    lan_chat_user_id_t duplicate_id = 0;
    lan_chat_account_create_t duplicate{};
    lan_chat_login_record_t login{};
    lan_chat_user_record_t users[8]{};
    lan_chat_message_record_t message{};
    lan_chat_message_record_t history[3]{};
    lan_chat_delivery_record_t deliveries[3]{};
    lan_chat_message_id_t first_message_id = 0;
    lan_chat_message_id_t second_message_id = 0;
    lan_chat_delivery_id_t first_delivery_id = 0;
    lan_chat_delivery_id_t second_delivery_id = 0;
    size_t count = 0;

    duplicate.username = alice.c_str();
    duplicate.nickname = "Alice 2";
    duplicate.password_hash = "hash2";
    duplicate.password_salt = "salt2";
    require_status(
        lan_chat_storage_create_account(storage, &duplicate, &duplicate_id),
        LAN_CHAT_STATUS_ALREADY_EXISTS,
        "duplicate account");

    require_status(lan_chat_storage_get_login_record(storage, alice.c_str(), &login), LAN_CHAT_STATUS_OK, "login record");
    require_true(login.user_id == alice_id, "login user_id mismatch");
    require_true(std::strcmp(login.password_hash, "hash-alice") == 0, "login hash mismatch");
    require_status(lan_chat_storage_list_users(storage, users, 8, &count), LAN_CHAT_STATUS_OK, "list users");
    require_true(count >= 2, "list users count too small");

    message.sender_id = alice_id;
    message.receiver_id = bob_id;
    message.content_type = 1;
    std::strcpy(message.content, "hello mysql");
    message.client_message_id = 1;
    message.send_time_ms = 1000;
    require_status(
        lan_chat_storage_store_private_message(storage, &message, &first_message_id, &first_delivery_id),
        LAN_CHAT_STATUS_OK,
        "store first message");

    message.conversation_id = 0;
    std::strcpy(message.content, "second mysql");
    message.client_message_id = 2;
    message.send_time_ms = 2000;
    require_status(
        lan_chat_storage_store_private_message(storage, &message, &second_message_id, &second_delivery_id),
        LAN_CHAT_STATUS_OK,
        "store second message");

    require_status(
        lan_chat_storage_list_history(storage, alice_id, bob_id, 0, 2, history, 2, &count),
        LAN_CHAT_STATUS_OK,
        "list history");
    require_true(count == 2, "history count mismatch");
    require_true(history[0].message_id == second_message_id, "history order mismatch");
    require_true(history[1].message_id == first_message_id, "history second mismatch");
    require_true(history[0].conversation_id == history[1].conversation_id, "conversation reuse mismatch");

    require_status(
        lan_chat_storage_list_pending_deliveries(storage, bob_id, 10, deliveries, 3, &count),
        LAN_CHAT_STATUS_OK,
        "pending bob");
    require_true(count == 2, "pending count mismatch");
    require_true(deliveries[0].delivery_id == first_delivery_id, "pending order mismatch");

    require_status(
        lan_chat_storage_mark_delivery_state(storage, first_delivery_id, LAN_CHAT_DELIVERY_DELIVERED, 3000),
        LAN_CHAT_STATUS_OK,
        "mark delivered");
    require_status(
        lan_chat_storage_mark_delivery_state(storage, first_delivery_id, LAN_CHAT_DELIVERY_ACKED, 4000),
        LAN_CHAT_STATUS_OK,
        "mark acked");
    require_status(
        lan_chat_storage_mark_delivery_state(storage, first_delivery_id, LAN_CHAT_DELIVERY_DELIVERED, 5000),
        LAN_CHAT_STATUS_INVALID_STATE,
        "reject rollback");
}

} // namespace

int main()
{
    lan_chat_storage_config_t config{};
    lan_chat_storage_t *storage = nullptr;

    config.kind = LAN_CHAT_STORAGE_KIND_MYSQL;
    config.host = env_or_default("LAN_CHAT_MYSQL_TEST_HOST", "127.0.0.1");
    config.port = env_port_or_default("LAN_CHAT_MYSQL_TEST_PORT", 3306);
    config.database = env_or_default("LAN_CHAT_MYSQL_TEST_DATABASE", "lan_chat_test");
    config.user = env_or_default("LAN_CHAT_MYSQL_TEST_USER", "root");
    config.password = env_or_default("LAN_CHAT_MYSQL_TEST_PASSWORD", "");

    const lan_chat_status_t open_status = lan_chat_storage_mysql_open(&config, &storage);
    if (open_status == LAN_CHAT_STATUS_NOT_IMPLEMENTED) {
        std::cerr << "MySQL backend is not compiled in\n";
        return EXIT_FAILURE;
    }
    require_status(open_status, LAN_CHAT_STATUS_OK, "mysql open");
    run_storage_contract(storage);
    lan_chat_storage_close(storage);
    return EXIT_SUCCESS;
}
