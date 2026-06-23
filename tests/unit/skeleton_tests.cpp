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
    E2E_CHAT_NOTIFY_FIELD_DELIVERY_ID = 4
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
    return EXIT_SUCCESS;
}
