#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "lan_chat/core/endian.h"
#include "lan_chat/core/framer.h"
#include "lan_chat/core/protocol.h"
#include "lan_chat/core/ring_buffer.h"
#include "lan_chat/core/tlv.h"
#include "lan_chat/server/server.h"
#include "lan_chat/storage/memory_storage.h"
#include "lan_chat/transport/transport.h"

namespace {

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

std::vector<uint8_t> make_packet(const uint8_t *body, size_t body_len)
{
    lan_chat_header_t header{};
    lan_chat_header_init(
        &header,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        static_cast<uint32_t>(body_len));
    header.seq = 42;
    header.session_id = 100;
    header.sender_id = 1;
    header.receiver_id = 2;
    header.timestamp_ms = 123456789;

    std::vector<uint8_t> packet(LAN_CHAT_HEADER_LEN + body_len);
    size_t packet_len = 0;
    require_status(
        lan_chat_packet_pack(&header, body, body_len, packet.data(), packet.size(), &packet_len),
        LAN_CHAT_STATUS_OK,
        "packet pack");
    require_true(packet_len == packet.size(), "packet length mismatch");
    return packet;
}

void wait_for_status(lan_chat_status_t (*operation)(void *), void *context, const char *label)
{
    constexpr int max_attempts = 2000;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const lan_chat_status_t status = operation(context);
        if (status == LAN_CHAT_STATUS_OK) {
            return;
        }
        if (status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
            require_status(status, LAN_CHAT_STATUS_OK, label);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cerr << label << ": timed out\n";
    std::exit(EXIT_FAILURE);
}

void test_endian()
{
    uint8_t buffer[16]{};
    uint16_t v16 = 0;
    uint32_t v32 = 0;
    uint64_t v64 = 0;

    require_status(lan_chat_write_u16_be(buffer, sizeof(buffer), 0, 0x1234), LAN_CHAT_STATUS_OK, "write u16");
    require_status(lan_chat_write_u32_be(buffer, sizeof(buffer), 2, 0xA1B2C3D4), LAN_CHAT_STATUS_OK, "write u32");
    require_status(lan_chat_write_u64_be(buffer, sizeof(buffer), 6, 0x0102030405060708ULL), LAN_CHAT_STATUS_OK, "write u64");
    require_status(lan_chat_read_u16_be(buffer, sizeof(buffer), 0, &v16), LAN_CHAT_STATUS_OK, "read u16");
    require_status(lan_chat_read_u32_be(buffer, sizeof(buffer), 2, &v32), LAN_CHAT_STATUS_OK, "read u32");
    require_status(lan_chat_read_u64_be(buffer, sizeof(buffer), 6, &v64), LAN_CHAT_STATUS_OK, "read u64");
    require_true(v16 == 0x1234, "u16 mismatch");
    require_true(v32 == 0xA1B2C3D4, "u32 mismatch");
    require_true(v64 == 0x0102030405060708ULL, "u64 mismatch");
}

void test_header_packet()
{
    uint8_t body[] = {1, 2, 3, 4};
    auto packet = make_packet(body, sizeof(body));
    lan_chat_header_t decoded{};
    const uint8_t *body_ptr = nullptr;
    size_t body_len = 0;
    size_t packet_len = 0;

    require_true(packet[0] == 0x4C && packet[1] == 0x43, "magic bytes mismatch");
    require_true(packet[3] == LAN_CHAT_HEADER_LEN, "header_len byte mismatch");
    require_true(packet[20] == 0 && packet[27] == 100, "session_id offset mismatch");
    require_true(packet[28] == 0 && packet[35] == 1, "sender_id offset mismatch");
    require_true(packet[36] == 0 && packet[43] == 2, "receiver_id offset mismatch");

    require_status(lan_chat_packet_peek_size(packet.data(), packet.size(), &packet_len), LAN_CHAT_STATUS_OK, "peek packet size");
    require_true(packet_len == packet.size(), "peek packet size mismatch");
    require_status(
        lan_chat_packet_unpack(&decoded, packet.data(), packet.size(), &body_ptr, &body_len),
        LAN_CHAT_STATUS_OK,
        "packet unpack");
    require_true(decoded.message_group == LAN_CHAT_GROUP_CHAT, "decoded message group mismatch");
    require_true(decoded.sender_id == 1 && decoded.receiver_id == 2, "decoded ids mismatch");
    require_true(body_len == sizeof(body), "decoded body length mismatch");
    require_true(std::memcmp(body_ptr, body, sizeof(body)) == 0, "decoded body mismatch");

    packet[0] = 0;
    require_status(lan_chat_packet_peek_size(packet.data(), packet.size(), &packet_len), LAN_CHAT_STATUS_BAD_MAGIC, "bad magic");
    packet = make_packet(body, sizeof(body));
    packet[2] = 2;
    require_status(lan_chat_packet_peek_size(packet.data(), packet.size(), &packet_len), LAN_CHAT_STATUS_UNSUPPORTED_VERSION, "bad version");
    packet = make_packet(body, sizeof(body));
    packet[3] = 63;
    require_status(lan_chat_packet_peek_size(packet.data(), packet.size(), &packet_len), LAN_CHAT_STATUS_BAD_HEADER, "bad header len");
    packet = make_packet(body, sizeof(body));
    packet[52] = 1;
    require_status(lan_chat_packet_peek_size(packet.data(), packet.size(), &packet_len), LAN_CHAT_STATUS_BAD_HEADER, "bad reserved");

    lan_chat_header_t too_large{};
    lan_chat_header_init(
        &too_large,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        LAN_CHAT_MAX_BODY_LEN + 1u);
    require_status(lan_chat_header_validate(&too_large), LAN_CHAT_STATUS_BODY_TOO_LARGE, "body too large");
}

void test_tlv()
{
    uint8_t buffer[128]{};
    lan_chat_tlv_writer_t writer{};
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};

    require_status(lan_chat_tlv_writer_init(&writer, buffer, sizeof(buffer)), LAN_CHAT_STATUS_OK, "tlv writer init");
    require_status(lan_chat_tlv_write_u64(&writer, 1, 1, 0x0102030405060708ULL), LAN_CHAT_STATUS_OK, "tlv write u64");
    require_status(lan_chat_tlv_write_string(&writer, 2, 0, "alice", 5), LAN_CHAT_STATUS_OK, "tlv write string");

    require_status(lan_chat_tlv_reader_init(&reader, buffer, lan_chat_tlv_writer_size(&writer)), LAN_CHAT_STATUS_OK, "tlv reader init");
    require_status(lan_chat_tlv_next(&reader, &tlv), LAN_CHAT_STATUS_OK, "tlv next first");
    require_true(tlv.required && tlv.field_id == 1 && tlv.length == 8, "first tlv mismatch");
    require_status(lan_chat_tlv_next(&reader, &tlv), LAN_CHAT_STATUS_OK, "tlv next second");
    require_true(!tlv.required && tlv.field_id == 2 && tlv.length == 5, "second tlv mismatch");
    require_status(lan_chat_tlv_next(&reader, &tlv), LAN_CHAT_STATUS_NEED_MORE_DATA, "tlv reader end");

    require_status(lan_chat_tlv_writer_init(&writer, buffer, sizeof(buffer)), LAN_CHAT_STATUS_OK, "dup writer init");
    require_status(lan_chat_tlv_write_u16(&writer, 3, 0, 10), LAN_CHAT_STATUS_OK, "dup tlv first");
    require_status(lan_chat_tlv_write_u16(&writer, 3, 0, 11), LAN_CHAT_STATUS_OK, "dup tlv second");
    require_status(lan_chat_tlv_reader_init(&reader, buffer, lan_chat_tlv_writer_size(&writer)), LAN_CHAT_STATUS_OK, "dup reader init");
    require_status(lan_chat_tlv_next(&reader, &tlv), LAN_CHAT_STATUS_OK, "dup tlv read first");
    require_status(lan_chat_tlv_next(&reader, &tlv), LAN_CHAT_STATUS_DUPLICATE_FIELD, "dup tlv reject");

    require_status(lan_chat_tlv_writer_init(&writer, buffer, sizeof(buffer)), LAN_CHAT_STATUS_OK, "field zero writer init");
    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 0;
    buffer[3] = 0;
    require_status(lan_chat_tlv_reader_init(&reader, buffer, 4), LAN_CHAT_STATUS_OK, "field zero reader init");
    require_status(lan_chat_tlv_next(&reader, &tlv), LAN_CHAT_STATUS_BAD_HEADER, "field zero reject");

    require_true((lan_chat_tlv_type(5, 1) & LAN_CHAT_TLV_REQUIRED_MASK) != 0, "required tlv bit missing");
}

void test_ring_buffer()
{
    uint8_t backing[8]{};
    uint8_t out[6]{};
    lan_chat_ring_buffer_t ring{};
    const uint8_t first[] = {1, 2, 3, 4, 5, 6};
    const uint8_t second[] = {7, 8, 9, 10};

    require_status(lan_chat_ring_buffer_init(&ring, backing, sizeof(backing)), LAN_CHAT_STATUS_OK, "ring init");
    require_status(lan_chat_ring_buffer_write(&ring, first, sizeof(first)), LAN_CHAT_STATUS_OK, "ring write first");
    require_status(lan_chat_ring_buffer_discard(&ring, 4), LAN_CHAT_STATUS_OK, "ring discard");
    require_status(lan_chat_ring_buffer_write(&ring, second, sizeof(second)), LAN_CHAT_STATUS_OK, "ring write wrap");
    require_true(lan_chat_ring_buffer_size(&ring) == 6, "ring size mismatch");
    require_status(lan_chat_ring_buffer_peek(&ring, 0, out, sizeof(out)), LAN_CHAT_STATUS_OK, "ring peek wrap");
    const uint8_t expected[] = {5, 6, 7, 8, 9, 10};
    require_true(std::memcmp(out, expected, sizeof(expected)) == 0, "ring wrap content mismatch");
}

void test_framer()
{
    const uint8_t body_a[] = {10, 11, 12};
    const uint8_t body_b[] = {20, 21};
    auto packet_a = make_packet(body_a, sizeof(body_a));
    auto packet_b = make_packet(body_b, sizeof(body_b));
    std::vector<uint8_t> combined;
    uint8_t backing[256]{};
    uint8_t out[128]{};
    size_t out_len = 0;
    lan_chat_framer_t framer{};

    require_status(lan_chat_framer_init(&framer, backing, sizeof(backing)), LAN_CHAT_STATUS_OK, "framer init");
    require_status(lan_chat_framer_feed(&framer, packet_a.data(), 10), LAN_CHAT_STATUS_OK, "framer feed partial");
    require_status(lan_chat_framer_next(&framer, out, sizeof(out), &out_len), LAN_CHAT_STATUS_NEED_MORE_DATA, "framer partial next");
    require_status(lan_chat_framer_feed(&framer, packet_a.data() + 10, packet_a.size() - 10), LAN_CHAT_STATUS_OK, "framer feed rest");
    require_status(lan_chat_framer_next(&framer, out, sizeof(out), &out_len), LAN_CHAT_STATUS_OK, "framer complete next");
    require_true(out_len == packet_a.size() && std::memcmp(out, packet_a.data(), packet_a.size()) == 0, "framer packet mismatch");

    combined.insert(combined.end(), packet_a.begin(), packet_a.end());
    combined.insert(combined.end(), packet_b.begin(), packet_b.end());
    require_status(lan_chat_framer_feed(&framer, combined.data(), combined.size()), LAN_CHAT_STATUS_OK, "framer feed combined");
    require_status(lan_chat_framer_next(&framer, out, sizeof(out), &out_len), LAN_CHAT_STATUS_OK, "framer first combined");
    require_true(out_len == packet_a.size(), "framer first combined size");
    require_status(lan_chat_framer_next(&framer, out, sizeof(out), &out_len), LAN_CHAT_STATUS_OK, "framer second combined");
    require_true(out_len == packet_b.size(), "framer second combined size");
    require_status(lan_chat_framer_next(&framer, out, sizeof(out), &out_len), LAN_CHAT_STATUS_NEED_MORE_DATA, "framer empty");

    packet_a[0] = 0;
    require_status(lan_chat_framer_feed(&framer, packet_a.data(), packet_a.size()), LAN_CHAT_STATUS_OK, "framer feed bad magic");
    require_status(lan_chat_framer_next(&framer, out, sizeof(out), &out_len), LAN_CHAT_STATUS_BAD_MAGIC, "framer bad magic");
}

void test_server_skeleton()
{
    lan_chat_storage_t *storage = nullptr;
    require_status(lan_chat_storage_memory_open(&storage), LAN_CHAT_STATUS_OK, "memory storage open");
    require_true(storage != nullptr, "memory storage pointer is null");

    lan_chat_server_t server{};
    lan_chat_server_config_t config{};
    config.listen_port = 0;
    config.worker_thread_count = 1;
    config.storage = storage;

    require_status(lan_chat_server_init(&server, &config), LAN_CHAT_STATUS_OK, "server init");
    require_true(lan_chat_server_port(&server) != 0, "server port not assigned");

    lan_chat_server_shutdown(&server);
    lan_chat_storage_close(storage);
}

lan_chat_user_id_t create_test_account(
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
    require_status(
        lan_chat_storage_create_account(storage, &account, &user_id),
        LAN_CHAT_STATUS_OK,
        "storage create account");
    require_true(user_id != 0, "created user_id is zero");
    return user_id;
}

void test_storage_memory_accounts()
{
    lan_chat_storage_t *storage = nullptr;
    lan_chat_user_id_t alice_id = 0;
    lan_chat_user_id_t duplicate_id = 0;
    lan_chat_login_record_t login{};
    lan_chat_user_record_t users[2]{};
    size_t user_count = 0;
    lan_chat_account_create_t duplicate{};

    require_status(lan_chat_storage_memory_open(&storage), LAN_CHAT_STATUS_OK, "memory storage open accounts");
    require_status(
        lan_chat_storage_create_account(nullptr, nullptr, nullptr),
        LAN_CHAT_STATUS_INVALID_ARGUMENT,
        "storage null facade");
    alice_id = create_test_account(storage, "alice", "Alice", "hash-alice", "salt-alice");
    create_test_account(storage, "bob", nullptr, "hash-bob", "salt-bob");

    duplicate.username = "alice";
    duplicate.nickname = "Alice 2";
    duplicate.password_hash = "hash2";
    duplicate.password_salt = "salt2";
    require_status(
        lan_chat_storage_create_account(storage, &duplicate, &duplicate_id),
        LAN_CHAT_STATUS_ALREADY_EXISTS,
        "duplicate username");

    require_status(
        lan_chat_storage_get_login_record(storage, "alice", &login),
        LAN_CHAT_STATUS_OK,
        "get login alice");
    require_true(login.user_id == alice_id, "login user_id mismatch");
    require_true(std::strcmp(login.username, "alice") == 0, "login username mismatch");
    require_true(std::strcmp(login.nickname, "Alice") == 0, "login nickname mismatch");
    require_true(std::strcmp(login.password_hash, "hash-alice") == 0, "login hash mismatch");
    require_true(std::strcmp(login.password_salt, "salt-alice") == 0, "login salt mismatch");
    require_true(login.enabled == 1, "login enabled mismatch");

    require_status(
        lan_chat_storage_get_login_record(storage, "missing", &login),
        LAN_CHAT_STATUS_NOT_FOUND,
        "missing login");

    require_status(
        lan_chat_storage_list_users(storage, users, 1, &user_count),
        LAN_CHAT_STATUS_BUFFER_TOO_SMALL,
        "list users small capacity");
    require_true(user_count == 2, "list users required count mismatch");

    require_status(
        lan_chat_storage_list_users(storage, users, 2, &user_count),
        LAN_CHAT_STATUS_OK,
        "list users");
    require_true(user_count == 2, "list users count mismatch");
    require_true(users[0].user_id == 1 && users[1].user_id == 2, "list users order mismatch");
    require_true(std::strcmp(users[1].nickname, "bob") == 0, "default nickname mismatch");

    require_status(
        lan_chat_storage_update_last_login(storage, alice_id, 1234),
        LAN_CHAT_STATUS_OK,
        "update last login");
    require_status(
        lan_chat_storage_update_last_login(storage, 9999, 1234),
        LAN_CHAT_STATUS_NOT_FOUND,
        "update missing last login");

    lan_chat_storage_close(storage);
}

void test_storage_memory_messages()
{
    lan_chat_storage_t *storage = nullptr;
    lan_chat_user_id_t alice_id = 0;
    lan_chat_user_id_t bob_id = 0;
    lan_chat_message_record_t message{};
    lan_chat_message_id_t first_message_id = 0;
    lan_chat_message_id_t second_message_id = 0;
    lan_chat_message_id_t third_message_id = 0;
    lan_chat_delivery_id_t first_delivery_id = 0;
    lan_chat_delivery_id_t second_delivery_id = 0;
    lan_chat_delivery_id_t third_delivery_id = 0;
    lan_chat_message_record_t history[3]{};
    lan_chat_delivery_record_t deliveries[3]{};
    size_t count = 0;

    require_status(lan_chat_storage_memory_open(&storage), LAN_CHAT_STATUS_OK, "memory storage open messages");
    alice_id = create_test_account(storage, "alice", "Alice", "hash-alice", "salt-alice");
    bob_id = create_test_account(storage, "bob", "Bob", "hash-bob", "salt-bob");

    message.sender_id = alice_id;
    message.receiver_id = bob_id;
    message.content_type = 1;
    std::strcpy(message.content, "first");
    message.client_message_id = 101;
    message.send_time_ms = 1000;
    require_status(
        lan_chat_storage_store_private_message(storage, &message, &first_message_id, &first_delivery_id),
        LAN_CHAT_STATUS_OK,
        "store first message");
    require_true(first_message_id == 1 && first_delivery_id == 1, "first ids mismatch");

    message.conversation_id = 0;
    std::strcpy(message.content, "second");
    message.client_message_id = 102;
    message.send_time_ms = 2000;
    require_status(
        lan_chat_storage_store_private_message(storage, &message, &second_message_id, &second_delivery_id),
        LAN_CHAT_STATUS_OK,
        "store second message");

    message.sender_id = bob_id;
    message.receiver_id = alice_id;
    message.conversation_id = 0;
    std::strcpy(message.content, "third");
    message.client_message_id = 103;
    message.send_time_ms = 3000;
    require_status(
        lan_chat_storage_store_private_message(storage, &message, &third_message_id, &third_delivery_id),
        LAN_CHAT_STATUS_OK,
        "store third message");

    require_status(
        lan_chat_storage_list_history(storage, alice_id, bob_id, 0, 3, history, 2, &count),
        LAN_CHAT_STATUS_BUFFER_TOO_SMALL,
        "history small capacity");
    require_true(count == 3, "history required count mismatch");

    require_status(
        lan_chat_storage_list_history(storage, alice_id, bob_id, 0, 3, history, 3, &count),
        LAN_CHAT_STATUS_OK,
        "history all");
    require_true(count == 3, "history count mismatch");
    require_true(history[0].message_id == third_message_id, "history newest first mismatch");
    require_true(history[1].message_id == second_message_id, "history second mismatch");
    require_true(history[2].message_id == first_message_id, "history third mismatch");
    require_true(history[0].conversation_id == history[1].conversation_id, "conversation reuse mismatch");
    require_true(history[1].conversation_id == history[2].conversation_id, "reverse conversation reuse mismatch");

    require_status(
        lan_chat_storage_list_history(storage, alice_id, bob_id, third_message_id, 2, history, 2, &count),
        LAN_CHAT_STATUS_OK,
        "history before third");
    require_true(count == 2, "history before count mismatch");
    require_true(history[0].message_id == second_message_id && history[1].message_id == first_message_id, "history before order mismatch");

    require_status(
        lan_chat_storage_list_pending_deliveries(storage, bob_id, 10, deliveries, 3, &count),
        LAN_CHAT_STATUS_OK,
        "pending bob");
    require_true(count == 2, "pending bob count mismatch");
    require_true(deliveries[0].delivery_id == first_delivery_id, "pending first delivery mismatch");
    require_true(deliveries[1].delivery_id == second_delivery_id, "pending second delivery mismatch");
    require_true(deliveries[0].delivery_state == LAN_CHAT_DELIVERY_PENDING, "pending state mismatch");

    require_status(
        lan_chat_storage_mark_delivery_state(storage, first_delivery_id, LAN_CHAT_DELIVERY_DELIVERED, 4000),
        LAN_CHAT_STATUS_OK,
        "mark delivered");
    require_status(
        lan_chat_storage_mark_delivery_state(storage, first_delivery_id, LAN_CHAT_DELIVERY_DELIVERED, 4001),
        LAN_CHAT_STATUS_OK,
        "repeat delivered");
    require_status(
        lan_chat_storage_mark_delivery_state(storage, first_delivery_id, LAN_CHAT_DELIVERY_ACKED, 5000),
        LAN_CHAT_STATUS_OK,
        "mark acked");
    require_status(
        lan_chat_storage_mark_delivery_state(storage, first_delivery_id, LAN_CHAT_DELIVERY_DELIVERED, 6000),
        LAN_CHAT_STATUS_INVALID_STATE,
        "reject acked rollback");

    require_status(
        lan_chat_storage_mark_delivery_state(storage, second_delivery_id, LAN_CHAT_DELIVERY_ACKED, 5001),
        LAN_CHAT_STATUS_OK,
        "pending to acked");

    require_status(
        lan_chat_storage_list_pending_deliveries(storage, bob_id, 10, deliveries, 3, &count),
        LAN_CHAT_STATUS_OK,
        "pending bob after ack");
    require_true(count == 0, "pending bob after ack count mismatch");

    require_status(
        lan_chat_storage_list_pending_deliveries(storage, alice_id, 10, deliveries, 3, &count),
        LAN_CHAT_STATUS_OK,
        "pending alice");
    require_true(count == 1, "pending alice count mismatch");
    require_true(deliveries[0].delivery_id == third_delivery_id, "pending alice delivery mismatch");
    require_true(std::strcmp(deliveries[0].message.content, "third") == 0, "pending alice content mismatch");

    lan_chat_storage_close(storage);
}

struct AcceptContext {
    lan_chat_tcp_listener_t *listener;
    lan_chat_tcp_connection_t *connection;
};

lan_chat_status_t accept_operation(void *context)
{
    auto *ctx = static_cast<AcceptContext *>(context);
    return lan_chat_tcp_listener_accept(ctx->listener, ctx->connection);
}

struct FlushContext {
    lan_chat_tcp_connection_t *connection;
};

lan_chat_status_t flush_operation(void *context)
{
    auto *ctx = static_cast<FlushContext *>(context);
    return lan_chat_tcp_connection_flush(ctx->connection);
}

struct RecvContext {
    lan_chat_tcp_connection_t *connection;
    uint8_t *packet;
    size_t capacity;
    size_t *packet_len;
};

lan_chat_status_t recv_operation(void *context)
{
    auto *ctx = static_cast<RecvContext *>(context);
    return lan_chat_tcp_connection_recv_packet(ctx->connection, ctx->packet, ctx->capacity, ctx->packet_len);
}

enum {
    E2E_AUTH_FIELD_OPERATION = 1,
    E2E_AUTH_FIELD_USERNAME = 2,
    E2E_AUTH_FIELD_PASSWORD = 3,
    E2E_AUTH_FIELD_NICKNAME = 4,
    E2E_RESPONSE_FIELD_STATUS = 1,
    E2E_AUTH_RESPONSE_FIELD_USER_ID = 2,
    E2E_AUTH_RESPONSE_FIELD_SESSION_ID = 3,
    E2E_CHAT_FIELD_RECEIVER_ID = 1,
    E2E_CHAT_FIELD_CONTENT = 2,
    E2E_CHAT_FIELD_CLIENT_MESSAGE_ID = 3,
    E2E_CHAT_RESPONSE_FIELD_MESSAGE_ID = 2,
    E2E_CHAT_RESPONSE_FIELD_DELIVERY_ID = 3,
    E2E_CHAT_NOTIFY_FIELD_MESSAGE_ID = 1,
    E2E_CHAT_NOTIFY_FIELD_SENDER_ID = 2,
    E2E_CHAT_NOTIFY_FIELD_CONTENT = 3,
    E2E_CHAT_NOTIFY_FIELD_DELIVERY_ID = 4,
    E2E_USER_FIELD_OPERATION = 1,
    E2E_USER_FIELD_COUNT = 2,
    E2E_USER_FIELD_USERS_BLOB = 3,
    E2E_CALL_FIELD_OPERATION = 1,
    E2E_CALL_FIELD_CALL_ID = 2,
    E2E_CALL_FIELD_PEER_ID = 3,
    E2E_CALL_FIELD_MEDIA_MODE = 4,
    E2E_CALL_FIELD_SDP = 5,
    E2E_CALL_FIELD_ICE_CANDIDATE = 6,
    E2E_CALL_FIELD_ICE_MID = 7,
    E2E_CALL_FIELD_ICE_MLINE_INDEX = 8
};

struct AuthResult {
    lan_chat_status_t status = LAN_CHAT_STATUS_INTERNAL_ERROR;
    lan_chat_user_id_t user_id = 0;
    lan_chat_session_id_t session_id = 0;
};

struct ChatResult {
    lan_chat_status_t status = LAN_CHAT_STATUS_INTERNAL_ERROR;
    lan_chat_message_id_t message_id = 0;
    lan_chat_delivery_id_t delivery_id = 0;
};

struct ChatNotify {
    lan_chat_message_id_t message_id = 0;
    lan_chat_user_id_t sender_id = 0;
    lan_chat_delivery_id_t delivery_id = 0;
    std::string content;
};

struct UserListResult {
    lan_chat_status_t status = LAN_CHAT_STATUS_INTERNAL_ERROR;
    std::vector<lan_chat_user_id_t> user_ids;
};

struct CallResult {
    lan_chat_status_t status = LAN_CHAT_STATUS_INTERNAL_ERROR;
    uint64_t call_id = 0;
};

struct CallNotify {
    std::string operation;
    uint64_t call_id = 0;
    lan_chat_user_id_t peer_id = 0;
    std::string media_mode;
    std::string sdp;
    std::string ice_candidate;
    std::string ice_mid;
    uint32_t ice_mline_index = 0;
};

void flush_connection_once(lan_chat_tcp_connection_t *connection, const char *label)
{
    if (connection == nullptr || !connection->is_open) {
        return;
    }
    const lan_chat_status_t status = lan_chat_tcp_connection_flush(connection);
    if (status != LAN_CHAT_STATUS_OK && status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
        require_status(status, LAN_CHAT_STATUS_OK, label);
    }
}

void pump_server(lan_chat_server_t *server, int cycles)
{
    for (int i = 0; i < cycles; ++i) {
        require_status(lan_chat_server_run_once(server), LAN_CHAT_STATUS_OK, "server run once");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::vector<uint8_t> pump_until_packet(
    lan_chat_server_t *server,
    lan_chat_tcp_connection_t *alice,
    lan_chat_tcp_connection_t *bob,
    lan_chat_tcp_connection_t *receiver,
    const char *label)
{
    constexpr int max_attempts = 2000;
    std::vector<uint8_t> packet(LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE);

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        size_t packet_len = 0;

        require_status(lan_chat_server_run_once(server), LAN_CHAT_STATUS_OK, "server run once");
        flush_connection_once(alice, "alice flush");
        flush_connection_once(bob, "bob flush");
        flush_connection_once(receiver, "receiver flush");

        const lan_chat_status_t status = lan_chat_tcp_connection_recv_packet(
            receiver,
            packet.data(),
            packet.size(),
            &packet_len);
        if (status == LAN_CHAT_STATUS_OK) {
            packet.resize(packet_len);
            return packet;
        }
        if (status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
            require_status(status, LAN_CHAT_STATUS_OK, label);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cerr << label << ": timed out\n";
    std::exit(EXIT_FAILURE);
}

uint16_t read_tlv_u16_value(const lan_chat_tlv_t &tlv, const char *label)
{
    uint16_t value = 0;
    require_true(tlv.length == 2, label);
    require_status(lan_chat_read_u16_be(tlv.value, tlv.length, 0, &value), LAN_CHAT_STATUS_OK, label);
    return value;
}

uint32_t read_tlv_u32_value(const lan_chat_tlv_t &tlv, const char *label)
{
    uint32_t value = 0;
    require_true(tlv.length == 4, label);
    require_status(lan_chat_read_u32_be(tlv.value, tlv.length, 0, &value), LAN_CHAT_STATUS_OK, label);
    return value;
}

uint64_t read_tlv_u64_value(const lan_chat_tlv_t &tlv, const char *label)
{
    uint64_t value = 0;
    require_true(tlv.length == 8, label);
    require_status(lan_chat_read_u64_be(tlv.value, tlv.length, 0, &value), LAN_CHAT_STATUS_OK, label);
    return value;
}

std::string read_tlv_string_value(const lan_chat_tlv_t &tlv)
{
    return std::string(reinterpret_cast<const char *>(tlv.value), tlv.length);
}

void unpack_e2e_packet(
    const std::vector<uint8_t> &packet,
    lan_chat_header_t *header,
    const uint8_t **body,
    size_t *body_len,
    uint16_t group,
    uint16_t type,
    uint16_t flags,
    const char *label)
{
    require_status(
        lan_chat_packet_unpack(header, packet.data(), packet.size(), body, body_len),
        LAN_CHAT_STATUS_OK,
        label);
    require_true(header->message_group == group, "e2e packet group mismatch");
    require_true(header->message_type == type, "e2e packet type mismatch");
    require_true(header->flags == flags, "e2e packet flags mismatch");
}

void queue_auth_request(
    lan_chat_tcp_connection_t *connection,
    const char *operation,
    const char *username,
    const char *password,
    const char *nickname,
    uint32_t seq)
{
    uint8_t body[512]{};
    lan_chat_tlv_writer_t writer{};
    lan_chat_header_t header{};

    require_status(lan_chat_tlv_writer_init(&writer, body, sizeof(body)), LAN_CHAT_STATUS_OK, "auth tlv writer");
    require_status(
        lan_chat_tlv_write_string(&writer, E2E_AUTH_FIELD_OPERATION, 1, operation, std::strlen(operation)),
        LAN_CHAT_STATUS_OK,
        "auth operation tlv");
    require_status(
        lan_chat_tlv_write_string(&writer, E2E_AUTH_FIELD_USERNAME, 1, username, std::strlen(username)),
        LAN_CHAT_STATUS_OK,
        "auth username tlv");
    require_status(
        lan_chat_tlv_write_string(&writer, E2E_AUTH_FIELD_PASSWORD, 1, password, std::strlen(password)),
        LAN_CHAT_STATUS_OK,
        "auth password tlv");
    if (nickname != nullptr) {
        require_status(
            lan_chat_tlv_write_string(&writer, E2E_AUTH_FIELD_NICKNAME, 0, nickname, std::strlen(nickname)),
            LAN_CHAT_STATUS_OK,
            "auth nickname tlv");
    }

    lan_chat_header_init(
        &header,
        LAN_CHAT_GROUP_AUTH,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        static_cast<uint32_t>(lan_chat_tlv_writer_size(&writer)));
    header.seq = seq;
    require_status(
        lan_chat_tcp_connection_queue_packet(connection, &header, body, lan_chat_tlv_writer_size(&writer)),
        LAN_CHAT_STATUS_OK,
        "queue auth request");
}

void queue_chat_request(
    lan_chat_tcp_connection_t *connection,
    lan_chat_user_id_t sender_id,
    lan_chat_session_id_t session_id,
    lan_chat_user_id_t receiver_id,
    const char *content,
    uint64_t client_message_id,
    uint32_t seq)
{
    uint8_t body[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 64]{};
    lan_chat_tlv_writer_t writer{};
    lan_chat_header_t header{};

    require_status(lan_chat_tlv_writer_init(&writer, body, sizeof(body)), LAN_CHAT_STATUS_OK, "chat tlv writer");
    require_status(
        lan_chat_tlv_write_u64(&writer, E2E_CHAT_FIELD_RECEIVER_ID, 1, receiver_id),
        LAN_CHAT_STATUS_OK,
        "chat receiver tlv");
    require_status(
        lan_chat_tlv_write_string(&writer, E2E_CHAT_FIELD_CONTENT, 1, content, std::strlen(content)),
        LAN_CHAT_STATUS_OK,
        "chat content tlv");
    require_status(
        lan_chat_tlv_write_u64(&writer, E2E_CHAT_FIELD_CLIENT_MESSAGE_ID, 0, client_message_id),
        LAN_CHAT_STATUS_OK,
        "chat client message id tlv");

    lan_chat_header_init(
        &header,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        static_cast<uint32_t>(lan_chat_tlv_writer_size(&writer)));
    header.seq = seq;
    header.sender_id = sender_id;
    header.receiver_id = receiver_id;
    header.session_id = session_id;
    require_status(
        lan_chat_tcp_connection_queue_packet(connection, &header, body, lan_chat_tlv_writer_size(&writer)),
        LAN_CHAT_STATUS_OK,
        "queue chat request");
}

void queue_user_list_request(
    lan_chat_tcp_connection_t *connection,
    lan_chat_user_id_t sender_id,
    lan_chat_session_id_t session_id,
    uint32_t seq)
{
    uint8_t body[64]{};
    lan_chat_tlv_writer_t writer{};
    lan_chat_header_t header{};

    require_status(lan_chat_tlv_writer_init(&writer, body, sizeof(body)), LAN_CHAT_STATUS_OK, "user tlv writer");
    require_status(
        lan_chat_tlv_write_string(&writer, E2E_USER_FIELD_OPERATION, 1, "online-list", 11),
        LAN_CHAT_STATUS_OK,
        "user operation tlv");
    lan_chat_header_init(
        &header,
        LAN_CHAT_GROUP_USER,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        static_cast<uint32_t>(lan_chat_tlv_writer_size(&writer)));
    header.seq = seq;
    header.sender_id = sender_id;
    header.session_id = session_id;
    require_status(
        lan_chat_tcp_connection_queue_packet(connection, &header, body, lan_chat_tlv_writer_size(&writer)),
        LAN_CHAT_STATUS_OK,
        "queue user list request");
}

void queue_call_request(
    lan_chat_tcp_connection_t *connection,
    lan_chat_user_id_t sender_id,
    lan_chat_session_id_t session_id,
    const char *operation,
    uint64_t call_id,
    lan_chat_user_id_t peer_id,
    const char *media_mode,
    const char *sdp,
    const char *ice_candidate,
    uint32_t seq)
{
    uint8_t body[LAN_CHAT_MAX_SIGNALING_TEXT_LEN + 512]{};
    lan_chat_tlv_writer_t writer{};
    lan_chat_header_t header{};

    require_status(lan_chat_tlv_writer_init(&writer, body, sizeof(body)), LAN_CHAT_STATUS_OK, "call tlv writer");
    require_status(
        lan_chat_tlv_write_string(&writer, E2E_CALL_FIELD_OPERATION, 1, operation, std::strlen(operation)),
        LAN_CHAT_STATUS_OK,
        "call operation tlv");
    if (call_id != 0) {
        require_status(lan_chat_tlv_write_u64(&writer, E2E_CALL_FIELD_CALL_ID, 1, call_id), LAN_CHAT_STATUS_OK, "call id tlv");
    }
    if (peer_id != 0) {
        require_status(lan_chat_tlv_write_u64(&writer, E2E_CALL_FIELD_PEER_ID, 0, peer_id), LAN_CHAT_STATUS_OK, "call peer tlv");
    }
    if (media_mode != nullptr) {
        require_status(
            lan_chat_tlv_write_string(&writer, E2E_CALL_FIELD_MEDIA_MODE, 0, media_mode, std::strlen(media_mode)),
            LAN_CHAT_STATUS_OK,
            "call media tlv");
    }
    if (sdp != nullptr) {
        require_status(lan_chat_tlv_write_string(&writer, E2E_CALL_FIELD_SDP, 0, sdp, std::strlen(sdp)), LAN_CHAT_STATUS_OK, "call sdp tlv");
    }
    if (ice_candidate != nullptr) {
        require_status(
            lan_chat_tlv_write_string(&writer, E2E_CALL_FIELD_ICE_CANDIDATE, 0, ice_candidate, std::strlen(ice_candidate)),
            LAN_CHAT_STATUS_OK,
            "call ice tlv");
        require_status(lan_chat_tlv_write_string(&writer, E2E_CALL_FIELD_ICE_MID, 0, "0", 1), LAN_CHAT_STATUS_OK, "call ice mid tlv");
        require_status(lan_chat_tlv_write_u32(&writer, E2E_CALL_FIELD_ICE_MLINE_INDEX, 0, 0), LAN_CHAT_STATUS_OK, "call ice mline tlv");
    }
    lan_chat_header_init(
        &header,
        LAN_CHAT_GROUP_CALL,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        static_cast<uint32_t>(lan_chat_tlv_writer_size(&writer)));
    header.seq = seq;
    header.sender_id = sender_id;
    header.receiver_id = peer_id;
    header.session_id = session_id;
    require_status(
        lan_chat_tcp_connection_queue_packet(connection, &header, body, lan_chat_tlv_writer_size(&writer)),
        LAN_CHAT_STATUS_OK,
        "queue call request");
}

AuthResult parse_auth_response(const std::vector<uint8_t> &packet, uint32_t expected_seq)
{
    AuthResult result{};
    lan_chat_header_t header{};
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool has_status = false;

    unpack_e2e_packet(
        packet,
        &header,
        &body,
        &body_len,
        LAN_CHAT_GROUP_AUTH,
        LAN_CHAT_MSG_RSP,
        LAN_CHAT_FLAG_RESPONSE,
        "auth response unpack");
    require_true(header.seq == expected_seq, "auth response seq mismatch");

    require_status(lan_chat_tlv_reader_init(&reader, body, body_len), LAN_CHAT_STATUS_OK, "auth response reader");
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case E2E_RESPONSE_FIELD_STATUS:
            result.status = static_cast<lan_chat_status_t>(read_tlv_u16_value(tlv, "auth status tlv"));
            has_status = true;
            break;
        case E2E_AUTH_RESPONSE_FIELD_USER_ID:
            result.user_id = read_tlv_u64_value(tlv, "auth user_id tlv");
            break;
        case E2E_AUTH_RESPONSE_FIELD_SESSION_ID:
            result.session_id = read_tlv_u64_value(tlv, "auth session_id tlv");
            break;
        default:
            break;
        }
    }
    require_status(status, LAN_CHAT_STATUS_NEED_MORE_DATA, "auth response reader end");
    require_true(has_status, "auth response missing status");
    return result;
}

ChatResult parse_chat_response(const std::vector<uint8_t> &packet, uint32_t expected_seq)
{
    ChatResult result{};
    lan_chat_header_t header{};
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool has_status = false;

    unpack_e2e_packet(
        packet,
        &header,
        &body,
        &body_len,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_MSG_RSP,
        LAN_CHAT_FLAG_RESPONSE,
        "chat response unpack");
    require_true(header.seq == expected_seq, "chat response seq mismatch");

    require_status(lan_chat_tlv_reader_init(&reader, body, body_len), LAN_CHAT_STATUS_OK, "chat response reader");
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case E2E_RESPONSE_FIELD_STATUS:
            result.status = static_cast<lan_chat_status_t>(read_tlv_u16_value(tlv, "chat status tlv"));
            has_status = true;
            break;
        case E2E_CHAT_RESPONSE_FIELD_MESSAGE_ID:
            result.message_id = read_tlv_u64_value(tlv, "chat message_id tlv");
            break;
        case E2E_CHAT_RESPONSE_FIELD_DELIVERY_ID:
            result.delivery_id = read_tlv_u64_value(tlv, "chat delivery_id tlv");
            break;
        default:
            break;
        }
    }
    require_status(status, LAN_CHAT_STATUS_NEED_MORE_DATA, "chat response reader end");
    require_true(has_status, "chat response missing status");
    return result;
}

ChatNotify parse_chat_notify(const std::vector<uint8_t> &packet)
{
    ChatNotify notify{};
    lan_chat_header_t header{};
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool has_message = false;
    bool has_sender = false;
    bool has_content = false;

    unpack_e2e_packet(
        packet,
        &header,
        &body,
        &body_len,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_MSG_NOTIFY,
        LAN_CHAT_FLAG_NOTIFY,
        "chat notify unpack");

    require_status(lan_chat_tlv_reader_init(&reader, body, body_len), LAN_CHAT_STATUS_OK, "chat notify reader");
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case E2E_CHAT_NOTIFY_FIELD_MESSAGE_ID:
            notify.message_id = read_tlv_u64_value(tlv, "notify message_id tlv");
            has_message = true;
            break;
        case E2E_CHAT_NOTIFY_FIELD_SENDER_ID:
            notify.sender_id = read_tlv_u64_value(tlv, "notify sender_id tlv");
            has_sender = true;
            break;
        case E2E_CHAT_NOTIFY_FIELD_CONTENT:
            notify.content = read_tlv_string_value(tlv);
            has_content = true;
            break;
        case E2E_CHAT_NOTIFY_FIELD_DELIVERY_ID:
            notify.delivery_id = read_tlv_u64_value(tlv, "notify delivery_id tlv");
            break;
        default:
            break;
        }
    }
    require_status(status, LAN_CHAT_STATUS_NEED_MORE_DATA, "chat notify reader end");
    require_true(has_message && has_sender && has_content, "chat notify missing required field");
    return notify;
}

UserListResult parse_user_list_response(const std::vector<uint8_t> &packet, uint32_t expected_seq)
{
    UserListResult result{};
    lan_chat_header_t header{};
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool has_status = false;

    unpack_e2e_packet(
        packet,
        &header,
        &body,
        &body_len,
        LAN_CHAT_GROUP_USER,
        LAN_CHAT_MSG_RSP,
        LAN_CHAT_FLAG_RESPONSE,
        "user response unpack");
    require_true(header.seq == expected_seq, "user response seq mismatch");
    require_status(lan_chat_tlv_reader_init(&reader, body, body_len), LAN_CHAT_STATUS_OK, "user response reader");
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case E2E_RESPONSE_FIELD_STATUS:
            result.status = static_cast<lan_chat_status_t>(read_tlv_u16_value(tlv, "user status tlv"));
            has_status = true;
            break;
        case E2E_USER_FIELD_USERS_BLOB: {
            size_t offset = 0;
            while (offset < tlv.length) {
                uint64_t user_id = 0;
                uint16_t username_len = 0;
                uint16_t nickname_len = 0;
                require_true(tlv.length - offset >= 13, "user blob short record");
                require_status(lan_chat_read_u64_be(tlv.value, tlv.length, offset, &user_id), LAN_CHAT_STATUS_OK, "user blob id");
                require_status(lan_chat_read_u16_be(tlv.value, tlv.length, offset + 8, &username_len), LAN_CHAT_STATUS_OK, "user blob username len");
                require_status(lan_chat_read_u16_be(tlv.value, tlv.length, offset + 10, &nickname_len), LAN_CHAT_STATUS_OK, "user blob nickname len");
                offset += 12;
                require_true(tlv.length - offset >= static_cast<size_t>(username_len) + nickname_len + 1, "user blob record len");
                offset += username_len + nickname_len;
                require_true(tlv.value[offset] == 1, "user blob online flag");
                ++offset;
                result.user_ids.push_back(user_id);
            }
            break;
        }
        default:
            break;
        }
    }
    require_status(status, LAN_CHAT_STATUS_NEED_MORE_DATA, "user response reader end");
    require_true(has_status, "user response missing status");
    return result;
}

CallResult parse_call_response(const std::vector<uint8_t> &packet, uint32_t expected_seq)
{
    CallResult result{};
    lan_chat_header_t header{};
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool has_status = false;

    unpack_e2e_packet(
        packet,
        &header,
        &body,
        &body_len,
        LAN_CHAT_GROUP_CALL,
        LAN_CHAT_MSG_RSP,
        LAN_CHAT_FLAG_RESPONSE,
        "call response unpack");
    require_true(header.seq == expected_seq, "call response seq mismatch");
    require_status(lan_chat_tlv_reader_init(&reader, body, body_len), LAN_CHAT_STATUS_OK, "call response reader");
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case E2E_RESPONSE_FIELD_STATUS:
            result.status = static_cast<lan_chat_status_t>(read_tlv_u16_value(tlv, "call status tlv"));
            has_status = true;
            break;
        case E2E_CALL_FIELD_CALL_ID:
            result.call_id = read_tlv_u64_value(tlv, "call id tlv");
            break;
        default:
            break;
        }
    }
    require_status(status, LAN_CHAT_STATUS_NEED_MORE_DATA, "call response reader end");
    require_true(has_status, "call response missing status");
    return result;
}

CallNotify parse_call_notify(const std::vector<uint8_t> &packet)
{
    CallNotify notify{};
    lan_chat_header_t header{};
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool has_operation = false;
    bool has_call = false;
    bool has_peer = false;

    unpack_e2e_packet(
        packet,
        &header,
        &body,
        &body_len,
        LAN_CHAT_GROUP_CALL,
        LAN_CHAT_MSG_NOTIFY,
        LAN_CHAT_FLAG_NOTIFY,
        "call notify unpack");
    require_status(lan_chat_tlv_reader_init(&reader, body, body_len), LAN_CHAT_STATUS_OK, "call notify reader");
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case E2E_CALL_FIELD_OPERATION:
            notify.operation = read_tlv_string_value(tlv);
            has_operation = true;
            break;
        case E2E_CALL_FIELD_CALL_ID:
            notify.call_id = read_tlv_u64_value(tlv, "call notify id");
            has_call = true;
            break;
        case E2E_CALL_FIELD_PEER_ID:
            notify.peer_id = read_tlv_u64_value(tlv, "call notify peer");
            has_peer = true;
            break;
        case E2E_CALL_FIELD_MEDIA_MODE:
            notify.media_mode = read_tlv_string_value(tlv);
            break;
        case E2E_CALL_FIELD_SDP:
            notify.sdp = read_tlv_string_value(tlv);
            break;
        case E2E_CALL_FIELD_ICE_CANDIDATE:
            notify.ice_candidate = read_tlv_string_value(tlv);
            break;
        case E2E_CALL_FIELD_ICE_MID:
            notify.ice_mid = read_tlv_string_value(tlv);
            break;
        case E2E_CALL_FIELD_ICE_MLINE_INDEX:
            notify.ice_mline_index = read_tlv_u32_value(tlv, "call notify ice mline");
            break;
        default:
            break;
        }
    }
    require_status(status, LAN_CHAT_STATUS_NEED_MORE_DATA, "call notify reader end");
    require_true(has_operation && has_call && has_peer, "call notify missing required field");
    return notify;
}

void test_server_register_login_chat_e2e()
{
    lan_chat_storage_t *storage = nullptr;
    lan_chat_server_t server{};
    lan_chat_server_config_t config{};
    lan_chat_tcp_connection_t alice{};
    lan_chat_tcp_connection_t bob{};
    const char *content = "hello from alice";

    require_status(lan_chat_storage_memory_open(&storage), LAN_CHAT_STATUS_OK, "e2e memory storage open");
    config.listen_port = 0;
    config.worker_thread_count = 1;
    config.storage = storage;
    require_status(lan_chat_server_init(&server, &config), LAN_CHAT_STATUS_OK, "e2e server init");
    require_true(lan_chat_server_port(&server) != 0, "e2e server port not assigned");

    require_status(
        lan_chat_tcp_client_connect(&alice, "127.0.0.1", lan_chat_server_port(&server)),
        LAN_CHAT_STATUS_OK,
        "alice connect");
    require_status(
        lan_chat_tcp_client_connect(&bob, "127.0.0.1", lan_chat_server_port(&server)),
        LAN_CHAT_STATUS_OK,
        "bob connect");
    pump_server(&server, 8);

    queue_auth_request(&alice, "register", "alice_e2e", "alice-password", "Alice", 1);
    AuthResult alice_register = parse_auth_response(
        pump_until_packet(&server, &alice, &bob, &alice, "alice register response"),
        1);
    require_status(alice_register.status, LAN_CHAT_STATUS_OK, "alice register status");
    require_true(alice_register.user_id != 0, "alice register user_id");
    require_true(alice_register.session_id != 0, "alice register session_id");

    queue_auth_request(&bob, "register", "bob_e2e", "bob-password", "Bob", 2);
    AuthResult bob_register = parse_auth_response(
        pump_until_packet(&server, &alice, &bob, &bob, "bob register response"),
        2);
    require_status(bob_register.status, LAN_CHAT_STATUS_OK, "bob register status");
    require_true(bob_register.user_id != 0, "bob register user_id");
    require_true(bob_register.user_id != alice_register.user_id, "bob user_id should differ");

    queue_auth_request(&alice, "login", "alice_e2e", "alice-password", nullptr, 3);
    AuthResult alice_login = parse_auth_response(
        pump_until_packet(&server, &alice, &bob, &alice, "alice login response"),
        3);
    require_status(alice_login.status, LAN_CHAT_STATUS_OK, "alice login status");
    require_true(alice_login.user_id == alice_register.user_id, "alice login user_id mismatch");
    require_true(alice_login.session_id != 0, "alice login session_id");

    queue_auth_request(&bob, "login", "bob_e2e", "bob-password", nullptr, 4);
    AuthResult bob_login = parse_auth_response(
        pump_until_packet(&server, &alice, &bob, &bob, "bob login response"),
        4);
    require_status(bob_login.status, LAN_CHAT_STATUS_OK, "bob login status");
    require_true(bob_login.user_id == bob_register.user_id, "bob login user_id mismatch");
    require_true(bob_login.session_id != 0, "bob login session_id");

    queue_chat_request(
        &alice,
        alice_login.user_id,
        alice_login.session_id,
        bob_login.user_id,
        content,
        9001,
        5);
    ChatResult chat_result = parse_chat_response(
        pump_until_packet(&server, &alice, &bob, &alice, "alice chat response"),
        5);
    require_status(chat_result.status, LAN_CHAT_STATUS_OK, "chat response status");
    require_true(chat_result.message_id != 0, "chat response message_id");
    require_true(chat_result.delivery_id != 0, "chat response delivery_id");

    ChatNotify notify = parse_chat_notify(
        pump_until_packet(&server, &alice, &bob, &bob, "bob chat notify"));
    require_true(notify.message_id == chat_result.message_id, "notify message_id mismatch");
    require_true(notify.delivery_id == chat_result.delivery_id, "notify delivery_id mismatch");
    require_true(notify.sender_id == alice_login.user_id, "notify sender_id mismatch");
    require_true(notify.content == content, "notify content mismatch");

    lan_chat_tcp_connection_close(&alice);
    lan_chat_tcp_connection_close(&bob);
    lan_chat_server_shutdown(&server);
    lan_chat_storage_close(storage);
}

void test_server_user_list_and_call_signaling_e2e()
{
    lan_chat_storage_t *storage = nullptr;
    lan_chat_server_t server{};
    lan_chat_server_config_t config{};
    lan_chat_tcp_connection_t alice{};
    lan_chat_tcp_connection_t bob{};
    lan_chat_tcp_connection_t carol{};

    require_status(lan_chat_storage_memory_open(&storage), LAN_CHAT_STATUS_OK, "call memory storage open");
    config.listen_port = 0;
    config.worker_thread_count = 1;
    config.storage = storage;
    require_status(lan_chat_server_init(&server, &config), LAN_CHAT_STATUS_OK, "call server init");

    require_status(lan_chat_tcp_client_connect(&alice, "127.0.0.1", lan_chat_server_port(&server)), LAN_CHAT_STATUS_OK, "alice call connect");
    require_status(lan_chat_tcp_client_connect(&bob, "127.0.0.1", lan_chat_server_port(&server)), LAN_CHAT_STATUS_OK, "bob call connect");
    require_status(lan_chat_tcp_client_connect(&carol, "127.0.0.1", lan_chat_server_port(&server)), LAN_CHAT_STATUS_OK, "carol call connect");
    pump_server(&server, 8);

    queue_auth_request(&alice, "register", "alice_call", "alice-password", "Alice", 1);
    AuthResult alice_auth = parse_auth_response(pump_until_packet(&server, &alice, &bob, &alice, "alice call register"), 1);
    require_status(alice_auth.status, LAN_CHAT_STATUS_OK, "alice call register status");

    queue_auth_request(&bob, "register", "bob_call", "bob-password", "Bob", 2);
    AuthResult bob_auth = parse_auth_response(pump_until_packet(&server, &alice, &bob, &bob, "bob call register"), 2);
    require_status(bob_auth.status, LAN_CHAT_STATUS_OK, "bob call register status");

    queue_auth_request(&carol, "register", "carol_call", "carol-password", "Carol", 3);
    AuthResult carol_auth = parse_auth_response(pump_until_packet(&server, &alice, &bob, &carol, "carol call register"), 3);
    require_status(carol_auth.status, LAN_CHAT_STATUS_OK, "carol call register status");

    queue_user_list_request(&alice, alice_auth.user_id, alice_auth.session_id, 4);
    UserListResult users = parse_user_list_response(pump_until_packet(&server, &alice, &bob, &alice, "alice user list"), 4);
    require_status(users.status, LAN_CHAT_STATUS_OK, "user list status");
    require_true(std::find(users.user_ids.begin(), users.user_ids.end(), bob_auth.user_id) != users.user_ids.end(), "bob missing from user list");
    require_true(std::find(users.user_ids.begin(), users.user_ids.end(), alice_auth.user_id) == users.user_ids.end(), "self should not be in user list");

    queue_call_request(&alice, alice_auth.user_id, alice_auth.session_id, "invite", 0, bob_auth.user_id, "audio", nullptr, nullptr, 5);
    CallResult invite = parse_call_response(pump_until_packet(&server, &alice, &bob, &alice, "alice invite response"), 5);
    require_status(invite.status, LAN_CHAT_STATUS_OK, "invite status");
    require_true(invite.call_id != 0, "invite call id");
    CallNotify bob_invite = parse_call_notify(pump_until_packet(&server, &alice, &bob, &bob, "bob invite notify"));
    require_true(bob_invite.operation == "invite", "bob invite operation");
    require_true(bob_invite.call_id == invite.call_id, "bob invite call id");
    require_true(bob_invite.peer_id == alice_auth.user_id, "bob invite peer");
    require_true(bob_invite.media_mode == "audio", "bob invite media mode");

    queue_call_request(&carol, carol_auth.user_id, carol_auth.session_id, "invite", 0, bob_auth.user_id, "audio", nullptr, nullptr, 6);
    CallResult busy = parse_call_response(pump_until_packet(&server, &alice, &bob, &carol, "carol busy response"), 6);
    require_status(busy.status, LAN_CHAT_STATUS_INVALID_STATE, "busy call status");

    queue_call_request(&alice, alice_auth.user_id, alice_auth.session_id, "invite", 0, alice_auth.user_id, "audio", nullptr, nullptr, 7);
    CallResult self_call = parse_call_response(pump_until_packet(&server, &alice, &bob, &alice, "alice self call response"), 7);
    require_status(self_call.status, LAN_CHAT_STATUS_INVALID_ARGUMENT, "self call status");

    queue_call_request(&bob, bob_auth.user_id, bob_auth.session_id, "accept", invite.call_id, 0, nullptr, nullptr, nullptr, 8);
    CallResult accept = parse_call_response(pump_until_packet(&server, &alice, &bob, &bob, "bob accept response"), 8);
    require_status(accept.status, LAN_CHAT_STATUS_OK, "accept status");
    CallNotify alice_accept = parse_call_notify(pump_until_packet(&server, &alice, &bob, &alice, "alice accept notify"));
    require_true(alice_accept.operation == "accept", "alice accept operation");
    require_true(alice_accept.peer_id == bob_auth.user_id, "alice accept peer");

    std::string oversized_sdp(LAN_CHAT_MAX_SIGNALING_TEXT_LEN + 1, 'x');
    queue_call_request(&alice, alice_auth.user_id, alice_auth.session_id, "offer", invite.call_id, 0, nullptr, oversized_sdp.c_str(), nullptr, 9);
    CallResult oversized_offer = parse_call_response(pump_until_packet(&server, &alice, &bob, &alice, "alice oversized offer response"), 9);
    require_status(oversized_offer.status, LAN_CHAT_STATUS_BODY_TOO_LARGE, "oversized offer status");

    queue_call_request(&alice, alice_auth.user_id, alice_auth.session_id, "offer", invite.call_id, 0, nullptr, "offer-sdp", nullptr, 10);
    CallResult offer = parse_call_response(pump_until_packet(&server, &alice, &bob, &alice, "alice offer response"), 10);
    require_status(offer.status, LAN_CHAT_STATUS_OK, "offer status");
    CallNotify bob_offer = parse_call_notify(pump_until_packet(&server, &alice, &bob, &bob, "bob offer notify"));
    require_true(bob_offer.operation == "offer" && bob_offer.sdp == "offer-sdp", "bob offer payload");

    queue_call_request(&bob, bob_auth.user_id, bob_auth.session_id, "answer", invite.call_id, 0, nullptr, "answer-sdp", nullptr, 11);
    CallResult answer = parse_call_response(pump_until_packet(&server, &alice, &bob, &bob, "bob answer response"), 11);
    require_status(answer.status, LAN_CHAT_STATUS_OK, "answer status");
    CallNotify alice_answer = parse_call_notify(pump_until_packet(&server, &alice, &bob, &alice, "alice answer notify"));
    require_true(alice_answer.operation == "answer" && alice_answer.sdp == "answer-sdp", "alice answer payload");

    queue_call_request(&alice, alice_auth.user_id, alice_auth.session_id, "ice", invite.call_id, 0, nullptr, nullptr, "candidate-a", 12);
    CallResult ice = parse_call_response(pump_until_packet(&server, &alice, &bob, &alice, "alice ice response"), 12);
    require_status(ice.status, LAN_CHAT_STATUS_OK, "ice status");
    CallNotify bob_ice = parse_call_notify(pump_until_packet(&server, &alice, &bob, &bob, "bob ice notify"));
    require_true(bob_ice.operation == "ice" && bob_ice.ice_candidate == "candidate-a", "bob ice payload");

    queue_call_request(&bob, bob_auth.user_id, bob_auth.session_id, "hangup", invite.call_id, 0, nullptr, nullptr, nullptr, 13);
    CallResult hangup = parse_call_response(pump_until_packet(&server, &alice, &bob, &bob, "bob hangup response"), 13);
    require_status(hangup.status, LAN_CHAT_STATUS_OK, "hangup status");
    CallNotify alice_hangup = parse_call_notify(pump_until_packet(&server, &alice, &bob, &alice, "alice hangup notify"));
    require_true(alice_hangup.operation == "hangup", "alice hangup operation");

    queue_call_request(&alice, alice_auth.user_id, alice_auth.session_id, "invite", 0, 999999, "audio", nullptr, nullptr, 14);
    CallResult offline = parse_call_response(pump_until_packet(&server, &alice, &bob, &alice, "offline invite response"), 14);
    require_status(offline.status, LAN_CHAT_STATUS_NOT_FOUND, "offline invite status");

    queue_call_request(&alice, alice_auth.user_id, alice_auth.session_id, "invite", 0, bob_auth.user_id, "audio", nullptr, nullptr, 15);
    CallResult reject_invite = parse_call_response(pump_until_packet(&server, &alice, &bob, &alice, "reject invite response"), 15);
    require_status(reject_invite.status, LAN_CHAT_STATUS_OK, "reject invite status");
    (void)parse_call_notify(pump_until_packet(&server, &alice, &bob, &bob, "reject bob invite notify"));
    queue_call_request(&bob, bob_auth.user_id, bob_auth.session_id, "reject", reject_invite.call_id, 0, nullptr, nullptr, nullptr, 16);
    CallResult reject = parse_call_response(pump_until_packet(&server, &alice, &bob, &bob, "bob reject response"), 16);
    require_status(reject.status, LAN_CHAT_STATUS_OK, "reject status");
    CallNotify alice_reject = parse_call_notify(pump_until_packet(&server, &alice, &bob, &alice, "alice reject notify"));
    require_true(alice_reject.operation == "reject", "alice reject operation");

    queue_call_request(&alice, alice_auth.user_id, alice_auth.session_id, "invite", 0, bob_auth.user_id, "audio", nullptr, nullptr, 17);
    CallResult disconnect_invite = parse_call_response(pump_until_packet(&server, &alice, &bob, &alice, "disconnect invite response"), 17);
    require_status(disconnect_invite.status, LAN_CHAT_STATUS_OK, "disconnect invite status");
    (void)parse_call_notify(pump_until_packet(&server, &alice, &bob, &bob, "disconnect bob invite notify"));
    lan_chat_tcp_connection_close(&bob);
    CallNotify disconnect_hangup = parse_call_notify(pump_until_packet(&server, &alice, &bob, &alice, "disconnect hangup notify"));
    require_true(disconnect_hangup.operation == "hangup", "disconnect hangup operation");

    lan_chat_tcp_connection_close(&alice);
    lan_chat_tcp_connection_close(&carol);
    lan_chat_server_shutdown(&server);
    lan_chat_storage_close(storage);
}

void queue_heartbeat(lan_chat_tcp_connection_t *connection, uint16_t type, uint16_t flags, uint32_t seq)
{
    lan_chat_header_t header{};
    lan_chat_header_init(&header, LAN_CHAT_GROUP_HEARTBEAT, type, flags, 0);
    header.seq = seq;
    require_status(
        lan_chat_tcp_connection_queue_packet(connection, &header, nullptr, 0),
        LAN_CHAT_STATUS_OK,
        "queue heartbeat");
}

void verify_heartbeat_packet(
    const uint8_t *packet,
    size_t packet_len,
    uint16_t type,
    uint16_t flags,
    uint32_t seq,
    const char *label)
{
    lan_chat_header_t header{};
    const uint8_t *body = nullptr;
    size_t body_len = 0;

    require_status(
        lan_chat_packet_unpack(&header, packet, packet_len, &body, &body_len),
        LAN_CHAT_STATUS_OK,
        label);
    require_true(header.message_group == LAN_CHAT_GROUP_HEARTBEAT, "heartbeat group mismatch");
    require_true(header.message_type == type, "heartbeat type mismatch");
    require_true(header.flags == flags, "heartbeat flags mismatch");
    require_true(header.seq == seq, "heartbeat seq mismatch");
    require_true(body_len == 0 && body == packet + LAN_CHAT_HEADER_LEN, "heartbeat body mismatch");
}

void run_heartbeat_roundtrip(lan_chat_tcp_listener_t *listener, uint32_t seq)
{
    lan_chat_tcp_connection_t client{};
    lan_chat_tcp_connection_t server{};
    AcceptContext accept_ctx{listener, &server};
    FlushContext client_flush{&client};
    FlushContext server_flush{&server};
    uint8_t packet[LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE]{};
    size_t packet_len = 0;
    RecvContext server_recv{&server, packet, sizeof(packet), &packet_len};
    RecvContext client_recv{&client, packet, sizeof(packet), &packet_len};

    require_status(
        lan_chat_tcp_client_connect(&client, "127.0.0.1", lan_chat_tcp_listener_port(listener)),
        LAN_CHAT_STATUS_OK,
        "client connect");
    wait_for_status(accept_operation, &accept_ctx, "server accept");

    queue_heartbeat(&client, LAN_CHAT_MSG_REQ, LAN_CHAT_FLAG_REQUEST, seq);
    wait_for_status(flush_operation, &client_flush, "client flush heartbeat");
    wait_for_status(recv_operation, &server_recv, "server recv heartbeat");
    verify_heartbeat_packet(packet, packet_len, LAN_CHAT_MSG_REQ, LAN_CHAT_FLAG_REQUEST, seq, "server unpack heartbeat");

    queue_heartbeat(&server, LAN_CHAT_MSG_RSP, LAN_CHAT_FLAG_RESPONSE, seq);
    wait_for_status(flush_operation, &server_flush, "server flush heartbeat");
    wait_for_status(recv_operation, &client_recv, "client recv heartbeat");
    verify_heartbeat_packet(packet, packet_len, LAN_CHAT_MSG_RSP, LAN_CHAT_FLAG_RESPONSE, seq, "client unpack heartbeat");

    lan_chat_tcp_connection_close(&server);
    lan_chat_tcp_connection_close(&server);
    lan_chat_tcp_connection_close(&client);
    lan_chat_tcp_connection_close(&client);
}

void test_transport_tcp_smoke()
{
    lan_chat_transport_t transport{};
    lan_chat_tcp_listener_t listener{};

    require_status(lan_chat_transport_init(&transport), LAN_CHAT_STATUS_OK, "transport init");
    require_status(lan_chat_transport_init(&transport), LAN_CHAT_STATUS_OK, "transport init second");
    lan_chat_transport_shutdown(&transport);

    require_status(
        lan_chat_tcp_listener_open(&listener, "127.0.0.1", 0, LAN_CHAT_TRANSPORT_DEFAULT_BACKLOG),
        LAN_CHAT_STATUS_OK,
        "listener open");
    require_true(lan_chat_tcp_listener_port(&listener) != 0, "listener port not assigned");

    run_heartbeat_roundtrip(&listener, 1001);
    run_heartbeat_roundtrip(&listener, 1002);

    lan_chat_tcp_listener_close(&listener);
    lan_chat_tcp_listener_close(&listener);
    lan_chat_transport_shutdown(&transport);
}

} // namespace

int main()
{
    test_endian();
    test_header_packet();
    test_tlv();
    test_ring_buffer();
    test_framer();
    test_server_skeleton();
    test_storage_memory_accounts();
    test_storage_memory_messages();
    test_transport_tcp_smoke();
    test_server_register_login_chat_e2e();
    test_server_user_list_and_call_signaling_e2e();
    return EXIT_SUCCESS;
}
