#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "lan_chat/core/endian.h"
#include "lan_chat/core/protocol.h"
#include "lan_chat/core/status.h"
#include "lan_chat/core/tlv.h"
#include "lan_chat/transport/transport.h"
}

namespace {

enum {
    AUTH_FIELD_OPERATION = 1,
    AUTH_FIELD_USERNAME = 2,
    AUTH_FIELD_PASSWORD = 3,
    AUTH_FIELD_NICKNAME = 4,
    RESPONSE_FIELD_STATUS = 1,
    AUTH_RESPONSE_FIELD_USER_ID = 2,
    AUTH_RESPONSE_FIELD_SESSION_ID = 3,
    CHAT_FIELD_RECEIVER_ID = 1,
    CHAT_FIELD_CONTENT = 2,
    CHAT_FIELD_CLIENT_MESSAGE_ID = 3,
    CHAT_RESPONSE_FIELD_MESSAGE_ID = 2,
    CHAT_RESPONSE_FIELD_DELIVERY_ID = 3,
    CHAT_NOTIFY_FIELD_MESSAGE_ID = 1,
    CHAT_NOTIFY_FIELD_SENDER_ID = 2,
    CHAT_NOTIFY_FIELD_CONTENT = 3,
    CHAT_NOTIFY_FIELD_DELIVERY_ID = 4
};

struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
};

struct E2EOptions {
    Endpoint server;
    std::string message = "hello from chat_cli e2e";
};

struct AuthResult {
    lan_chat_status_t status = LAN_CHAT_STATUS_INTERNAL_ERROR;
    std::uint64_t user_id = 0;
    std::uint64_t session_id = 0;
};

struct ChatResult {
    lan_chat_status_t status = LAN_CHAT_STATUS_INTERNAL_ERROR;
    std::uint64_t message_id = 0;
    std::uint64_t delivery_id = 0;
};

struct ChatNotify {
    std::uint64_t message_id = 0;
    std::uint64_t sender_id = 0;
    std::uint64_t delivery_id = 0;
    std::string content;
};

void print_usage()
{
    std::cout
        << "Usage:\n"
        << "  chat_cli e2e [--server 127.0.0.1:7777] [--message <text>]\n";
}

bool parse_u16(const std::string &text, std::uint16_t *out_value)
{
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 10);
        if (consumed != text.size() || value == 0 || value > 65535UL) {
            return false;
        }
        *out_value = static_cast<std::uint16_t>(value);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool parse_endpoint(const std::string &text, Endpoint *endpoint)
{
    const size_t pos = text.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= text.size()) {
        return false;
    }
    endpoint->host = text.substr(0, pos);
    return parse_u16(text.substr(pos + 1), &endpoint->port);
}

bool parse_e2e_options(int argc, char **argv, E2EOptions *options)
{
    for (int i = 2; i < argc; ++i) {
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
        if (arg == "--server") {
            const char *value = require_value("--server");
            if (value == nullptr || !parse_endpoint(value, &options->server)) {
                std::cerr << "invalid --server, expected host:port\n";
                return false;
            }
        } else if (arg == "--message") {
            const char *value = require_value("--message");
            if (value == nullptr || value[0] == '\0') {
                std::cerr << "invalid --message\n";
                return false;
            }
            options->message = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return false;
        }
    }
    return true;
}

bool expect_status(lan_chat_status_t status, const char *label)
{
    if (status == LAN_CHAT_STATUS_OK) {
        return true;
    }
    std::cerr << label << ": " << lan_chat_status_name(status) << '\n';
    return false;
}

bool flush_once(lan_chat_tcp_connection_t *connection)
{
    if (connection == nullptr || !connection->is_open) {
        return true;
    }
    const lan_chat_status_t status = lan_chat_tcp_connection_flush(connection);
    if (status == LAN_CHAT_STATUS_OK || status == LAN_CHAT_STATUS_NEED_MORE_DATA) {
        return true;
    }
    std::cerr << "flush failed: " << lan_chat_status_name(status) << '\n';
    return false;
}

bool wait_for_packet(
    lan_chat_tcp_connection_t *alice,
    lan_chat_tcp_connection_t *bob,
    lan_chat_tcp_connection_t *receiver,
    std::vector<std::uint8_t> *out_packet,
    const char *label)
{
    constexpr int max_attempts = 5000;
    out_packet->assign(LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE, 0);

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        size_t packet_len = 0;
        if (!flush_once(alice) || !flush_once(bob)) {
            return false;
        }
        const lan_chat_status_t status = lan_chat_tcp_connection_recv_packet(
            receiver,
            out_packet->data(),
            out_packet->size(),
            &packet_len);
        if (status == LAN_CHAT_STATUS_OK) {
            out_packet->resize(packet_len);
            return true;
        }
        if (status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
            std::cerr << label << ": " << lan_chat_status_name(status) << '\n';
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cerr << label << ": timed out\n";
    return false;
}

std::uint16_t read_tlv_u16(const lan_chat_tlv_t &tlv, bool *ok)
{
    std::uint16_t value = 0;
    if (tlv.length != 2 || lan_chat_read_u16_be(tlv.value, tlv.length, 0, &value) != LAN_CHAT_STATUS_OK) {
        *ok = false;
    }
    return value;
}

std::uint64_t read_tlv_u64(const lan_chat_tlv_t &tlv, bool *ok)
{
    std::uint64_t value = 0;
    if (tlv.length != 8 || lan_chat_read_u64_be(tlv.value, tlv.length, 0, &value) != LAN_CHAT_STATUS_OK) {
        *ok = false;
    }
    return value;
}

std::string read_tlv_string(const lan_chat_tlv_t &tlv)
{
    return std::string(reinterpret_cast<const char *>(tlv.value), tlv.length);
}

bool unpack_packet(
    const std::vector<std::uint8_t> &packet,
    lan_chat_header_t *header,
    const std::uint8_t **body,
    size_t *body_len,
    std::uint16_t group,
    std::uint16_t type,
    std::uint16_t flags,
    const char *label)
{
    const lan_chat_status_t status = lan_chat_packet_unpack(
        header,
        packet.data(),
        packet.size(),
        body,
        body_len);
    if (!expect_status(status, label)) {
        return false;
    }
    if (header->message_group != group || header->message_type != type || header->flags != flags) {
        std::cerr << label << ": unexpected packet header\n";
        return false;
    }
    return true;
}

bool queue_auth_request(
    lan_chat_tcp_connection_t *connection,
    const char *operation,
    const std::string &username,
    const std::string &password,
    const char *nickname,
    std::uint32_t seq)
{
    std::uint8_t body[512]{};
    lan_chat_tlv_writer_t writer{};
    lan_chat_header_t header{};

    if (!expect_status(lan_chat_tlv_writer_init(&writer, body, sizeof(body)), "auth writer init")) {
        return false;
    }
    if (!expect_status(lan_chat_tlv_write_string(&writer, AUTH_FIELD_OPERATION, 1, operation, std::strlen(operation)), "auth operation") ||
        !expect_status(lan_chat_tlv_write_string(&writer, AUTH_FIELD_USERNAME, 1, username.c_str(), username.size()), "auth username") ||
        !expect_status(lan_chat_tlv_write_string(&writer, AUTH_FIELD_PASSWORD, 1, password.c_str(), password.size()), "auth password")) {
        return false;
    }
    if (nickname != nullptr &&
        !expect_status(lan_chat_tlv_write_string(&writer, AUTH_FIELD_NICKNAME, 0, nickname, std::strlen(nickname)), "auth nickname")) {
        return false;
    }

    lan_chat_header_init(
        &header,
        LAN_CHAT_GROUP_AUTH,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        static_cast<std::uint32_t>(lan_chat_tlv_writer_size(&writer)));
    header.seq = seq;
    return expect_status(
        lan_chat_tcp_connection_queue_packet(connection, &header, body, lan_chat_tlv_writer_size(&writer)),
        "queue auth request");
}

bool queue_chat_request(
    lan_chat_tcp_connection_t *connection,
    std::uint64_t sender_id,
    std::uint64_t session_id,
    std::uint64_t receiver_id,
    const std::string &content,
    std::uint64_t client_message_id,
    std::uint32_t seq)
{
    std::uint8_t body[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 64]{};
    lan_chat_tlv_writer_t writer{};
    lan_chat_header_t header{};

    if (!expect_status(lan_chat_tlv_writer_init(&writer, body, sizeof(body)), "chat writer init")) {
        return false;
    }
    if (!expect_status(lan_chat_tlv_write_u64(&writer, CHAT_FIELD_RECEIVER_ID, 1, receiver_id), "chat receiver") ||
        !expect_status(lan_chat_tlv_write_string(&writer, CHAT_FIELD_CONTENT, 1, content.c_str(), content.size()), "chat content") ||
        !expect_status(lan_chat_tlv_write_u64(&writer, CHAT_FIELD_CLIENT_MESSAGE_ID, 0, client_message_id), "chat client_message_id")) {
        return false;
    }

    lan_chat_header_init(
        &header,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        static_cast<std::uint32_t>(lan_chat_tlv_writer_size(&writer)));
    header.seq = seq;
    header.sender_id = sender_id;
    header.receiver_id = receiver_id;
    header.session_id = session_id;
    return expect_status(
        lan_chat_tcp_connection_queue_packet(connection, &header, body, lan_chat_tlv_writer_size(&writer)),
        "queue chat request");
}

bool parse_auth_response(const std::vector<std::uint8_t> &packet, std::uint32_t expected_seq, AuthResult *result)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool ok = true;
    bool has_status = false;

    if (!unpack_packet(packet, &header, &body, &body_len, LAN_CHAT_GROUP_AUTH, LAN_CHAT_MSG_RSP, LAN_CHAT_FLAG_RESPONSE, "auth response")) {
        return false;
    }
    if (header.seq != expected_seq) {
        std::cerr << "auth response seq mismatch\n";
        return false;
    }
    if (!expect_status(lan_chat_tlv_reader_init(&reader, body, body_len), "auth response reader")) {
        return false;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case RESPONSE_FIELD_STATUS:
            result->status = static_cast<lan_chat_status_t>(read_tlv_u16(tlv, &ok));
            has_status = true;
            break;
        case AUTH_RESPONSE_FIELD_USER_ID:
            result->user_id = read_tlv_u64(tlv, &ok);
            break;
        case AUTH_RESPONSE_FIELD_SESSION_ID:
            result->session_id = read_tlv_u64(tlv, &ok);
            break;
        default:
            break;
        }
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA || !ok || !has_status) {
        std::cerr << "invalid auth response body\n";
        return false;
    }
    return true;
}

bool parse_chat_response(const std::vector<std::uint8_t> &packet, std::uint32_t expected_seq, ChatResult *result)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool ok = true;
    bool has_status = false;

    if (!unpack_packet(packet, &header, &body, &body_len, LAN_CHAT_GROUP_CHAT, LAN_CHAT_MSG_RSP, LAN_CHAT_FLAG_RESPONSE, "chat response")) {
        return false;
    }
    if (header.seq != expected_seq) {
        std::cerr << "chat response seq mismatch\n";
        return false;
    }
    if (!expect_status(lan_chat_tlv_reader_init(&reader, body, body_len), "chat response reader")) {
        return false;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case RESPONSE_FIELD_STATUS:
            result->status = static_cast<lan_chat_status_t>(read_tlv_u16(tlv, &ok));
            has_status = true;
            break;
        case CHAT_RESPONSE_FIELD_MESSAGE_ID:
            result->message_id = read_tlv_u64(tlv, &ok);
            break;
        case CHAT_RESPONSE_FIELD_DELIVERY_ID:
            result->delivery_id = read_tlv_u64(tlv, &ok);
            break;
        default:
            break;
        }
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA || !ok || !has_status) {
        std::cerr << "invalid chat response body\n";
        return false;
    }
    return true;
}

bool parse_chat_notify(const std::vector<std::uint8_t> &packet, ChatNotify *notify)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    lan_chat_status_t status;
    bool ok = true;
    bool has_message = false;
    bool has_sender = false;
    bool has_content = false;

    if (!unpack_packet(packet, &header, &body, &body_len, LAN_CHAT_GROUP_CHAT, LAN_CHAT_MSG_NOTIFY, LAN_CHAT_FLAG_NOTIFY, "chat notify")) {
        return false;
    }
    if (!expect_status(lan_chat_tlv_reader_init(&reader, body, body_len), "chat notify reader")) {
        return false;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case CHAT_NOTIFY_FIELD_MESSAGE_ID:
            notify->message_id = read_tlv_u64(tlv, &ok);
            has_message = true;
            break;
        case CHAT_NOTIFY_FIELD_SENDER_ID:
            notify->sender_id = read_tlv_u64(tlv, &ok);
            has_sender = true;
            break;
        case CHAT_NOTIFY_FIELD_CONTENT:
            notify->content = read_tlv_string(tlv);
            has_content = true;
            break;
        case CHAT_NOTIFY_FIELD_DELIVERY_ID:
            notify->delivery_id = read_tlv_u64(tlv, &ok);
            break;
        default:
            break;
        }
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA || !ok || !has_message || !has_sender || !has_content) {
        std::cerr << "invalid chat notify body\n";
        return false;
    }
    return true;
}

void close_if_open(lan_chat_tcp_connection_t *connection)
{
    if (connection != nullptr && connection->is_open) {
        lan_chat_tcp_connection_close(connection);
    }
}

std::string make_unique_suffix()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool run_e2e(const E2EOptions &options)
{
    lan_chat_transport_t transport{};
    lan_chat_tcp_connection_t alice{};
    lan_chat_tcp_connection_t bob{};
    std::vector<std::uint8_t> packet;
    const std::string suffix = make_unique_suffix();
    const std::string alice_name = "alice_app_" + suffix;
    const std::string bob_name = "bob_app_" + suffix;
    const std::string alice_password = "alice-password-" + suffix;
    const std::string bob_password = "bob-password-" + suffix;

    if (!expect_status(lan_chat_transport_init(&transport), "transport init")) {
        return false;
    }
    if (!expect_status(lan_chat_tcp_client_connect(&alice, options.server.host.c_str(), options.server.port), "alice connect") ||
        !expect_status(lan_chat_tcp_client_connect(&bob, options.server.host.c_str(), options.server.port), "bob connect")) {
        close_if_open(&alice);
        close_if_open(&bob);
        lan_chat_transport_shutdown(&transport);
        return false;
    }

    AuthResult alice_register{};
    if (!queue_auth_request(&alice, "register", alice_name, alice_password, "Alice App", 1) ||
        !wait_for_packet(&alice, &bob, &alice, &packet, "alice register response") ||
        !parse_auth_response(packet, 1, &alice_register) ||
        alice_register.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "alice register failed\n";
        close_if_open(&alice);
        close_if_open(&bob);
        lan_chat_transport_shutdown(&transport);
        return false;
    }

    AuthResult bob_register{};
    if (!queue_auth_request(&bob, "register", bob_name, bob_password, "Bob App", 2) ||
        !wait_for_packet(&alice, &bob, &bob, &packet, "bob register response") ||
        !parse_auth_response(packet, 2, &bob_register) ||
        bob_register.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "bob register failed\n";
        close_if_open(&alice);
        close_if_open(&bob);
        lan_chat_transport_shutdown(&transport);
        return false;
    }

    AuthResult alice_login{};
    if (!queue_auth_request(&alice, "login", alice_name, alice_password, nullptr, 3) ||
        !wait_for_packet(&alice, &bob, &alice, &packet, "alice login response") ||
        !parse_auth_response(packet, 3, &alice_login) ||
        alice_login.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "alice login failed\n";
        close_if_open(&alice);
        close_if_open(&bob);
        lan_chat_transport_shutdown(&transport);
        return false;
    }

    AuthResult bob_login{};
    if (!queue_auth_request(&bob, "login", bob_name, bob_password, nullptr, 4) ||
        !wait_for_packet(&alice, &bob, &bob, &packet, "bob login response") ||
        !parse_auth_response(packet, 4, &bob_login) ||
        bob_login.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "bob login failed\n";
        close_if_open(&alice);
        close_if_open(&bob);
        lan_chat_transport_shutdown(&transport);
        return false;
    }

    ChatResult chat_result{};
    if (!queue_chat_request(&alice, alice_login.user_id, alice_login.session_id, bob_login.user_id, options.message, 5001, 5) ||
        !wait_for_packet(&alice, &bob, &alice, &packet, "alice chat response") ||
        !parse_chat_response(packet, 5, &chat_result) ||
        chat_result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "chat send failed\n";
        close_if_open(&alice);
        close_if_open(&bob);
        lan_chat_transport_shutdown(&transport);
        return false;
    }

    ChatNotify notify{};
    if (!wait_for_packet(&alice, &bob, &bob, &packet, "bob chat notify") ||
        !parse_chat_notify(packet, &notify)) {
        std::cerr << "chat notify failed\n";
        close_if_open(&alice);
        close_if_open(&bob);
        lan_chat_transport_shutdown(&transport);
        return false;
    }

    const bool ids_match = notify.message_id == chat_result.message_id &&
        notify.delivery_id == chat_result.delivery_id &&
        notify.sender_id == alice_login.user_id &&
        notify.content == options.message;
    if (!ids_match) {
        std::cerr << "chat notify payload mismatch\n";
        close_if_open(&alice);
        close_if_open(&bob);
        lan_chat_transport_shutdown(&transport);
        return false;
    }

    std::cout << "alice user_id=" << alice_login.user_id << " session_id=" << alice_login.session_id << '\n';
    std::cout << "bob user_id=" << bob_login.user_id << " session_id=" << bob_login.session_id << '\n';
    std::cout << "chat message_id=" << chat_result.message_id << " delivery_id=" << chat_result.delivery_id << '\n';
    std::cout << "notify sender_id=" << notify.sender_id << " content=\"" << notify.content << "\"\n";
    std::cout << "APP_E2E_OK\n";

    close_if_open(&alice);
    close_if_open(&bob);
    lan_chat_transport_shutdown(&transport);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        print_usage();
        return argc < 2 ? 1 : 0;
    }

    if (std::strcmp(argv[1], "e2e") != 0) {
        std::cerr << "unknown command: " << argv[1] << '\n';
        print_usage();
        return 1;
    }

    E2EOptions options;
    if (!parse_e2e_options(argc, argv, &options)) {
        return 1;
    }
    return run_e2e(options) ? 0 : 1;
}
