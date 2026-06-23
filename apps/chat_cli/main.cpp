#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
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
    RESPONSE_FIELD_USER_ID = 2,
    RESPONSE_FIELD_SESSION_ID = 3,
    RESPONSE_FIELD_MESSAGE = 4,
    CHAT_FIELD_RECEIVER_ID = 1,
    CHAT_FIELD_CONTENT = 2,
    CHAT_FIELD_CLIENT_MESSAGE_ID = 3,
    CHAT_RESPONSE_FIELD_MESSAGE_ID = 2,
    CHAT_RESPONSE_FIELD_DELIVERY_ID = 3,
    CHAT_NOTIFY_FIELD_MESSAGE_ID = 1,
    CHAT_NOTIFY_FIELD_SENDER_ID = 2,
    CHAT_NOTIFY_FIELD_CONTENT = 3,
    CHAT_NOTIFY_FIELD_DELIVERY_ID = 4,
    CONV_FIELD_OPERATION = 1,
    CONV_FIELD_NAME = 2,
    CONV_FIELD_MEMBER_IDS = 3,
    CONV_FIELD_CONVERSATION_ID = 4,
    CONV_FIELD_CONTENT = 5,
    CONV_FIELD_CLIENT_MESSAGE_ID = 6,
    CONV_RESPONSE_FIELD_CONVERSATION_ID = 2,
    CONV_RESPONSE_FIELD_MESSAGE_ID = 3,
    CONV_NOTIFY_FIELD_CONVERSATION_ID = 1,
    CONV_NOTIFY_FIELD_MESSAGE_ID = 2,
    CONV_NOTIFY_FIELD_SENDER_ID = 3,
    CONV_NOTIFY_FIELD_CONTENT = 4,
    FILE_FIELD_OPERATION = 1,
    FILE_FIELD_RECEIVER_ID = 2,
    FILE_FIELD_FILE_NAME = 3,
    FILE_FIELD_FILE_SIZE = 4,
    FILE_FIELD_CRC32 = 5,
    FILE_FIELD_TRANSFER_ID = 6,
    FILE_FIELD_CHUNK_INDEX = 7,
    FILE_FIELD_CHUNK_DATA = 8,
    FILE_FIELD_MESSAGE_ID = 9,
    FILE_FIELD_DELIVERY_ID = 10
};

struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
};

struct CommonOptions {
    Endpoint server;
    std::string username;
    std::string password;
};

struct AuthResult {
    lan_chat_status_t status = LAN_CHAT_STATUS_INTERNAL_ERROR;
    std::uint64_t user_id = 0;
    std::uint64_t session_id = 0;
    std::string message;
};

struct StatusResult {
    lan_chat_status_t status = LAN_CHAT_STATUS_INTERNAL_ERROR;
    std::uint64_t id_a = 0;
    std::uint64_t id_b = 0;
    std::uint64_t id_c = 0;
    std::string message;
};

struct PrivateNotify {
    std::uint64_t message_id = 0;
    std::uint64_t sender_id = 0;
    std::uint64_t delivery_id = 0;
    std::string content;
};

struct GroupNotify {
    std::uint64_t conversation_id = 0;
    std::uint64_t message_id = 0;
    std::uint64_t sender_id = 0;
    std::string content;
};

struct FileNotify {
    std::string operation;
    std::uint64_t transfer_id = 0;
    std::uint64_t message_id = 0;
    std::uint64_t delivery_id = 0;
    std::uint64_t file_size = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t chunk_index = 0;
    std::string file_name;
    std::vector<std::uint8_t> chunk_data;
};

struct Client {
    lan_chat_transport_t transport{};
    lan_chat_tcp_connection_t connection{};
    bool transport_initialized = false;

    ~Client()
    {
        close();
    }

    bool connect(const Endpoint &endpoint)
    {
        if (!expect_status(lan_chat_transport_init(&transport), "transport init")) {
            return false;
        }
        transport_initialized = true;
        if (!expect_status(lan_chat_tcp_client_connect(&connection, endpoint.host.c_str(), endpoint.port), "connect")) {
            return false;
        }
        return true;
    }

    void close()
    {
        if (connection.is_open) {
            lan_chat_tcp_connection_close(&connection);
        }
        if (transport_initialized) {
            lan_chat_transport_shutdown(&transport);
            transport_initialized = false;
        }
    }

    static bool expect_status(lan_chat_status_t status, const char *label)
    {
        if (status == LAN_CHAT_STATUS_OK) {
            return true;
        }
        std::cerr << label << ": " << lan_chat_status_name(status) << '\n';
        return false;
    }
};

void print_usage()
{
    std::cout
        << "Usage:\n"
        << "  chat_cli register --server host:port --username name --password pass [--nickname name]\n"
        << "  chat_cli login --server host:port --username name --password pass\n"
        << "  chat_cli send --server host:port --username name --password pass --to-user-id id --message text\n"
        << "  chat_cli listen --server host:port --username name --password pass --expect-count n --timeout-ms ms [--output-dir dir]\n"
        << "  chat_cli group-create --server host:port --username name --password pass --name group --member-user-ids 2,3\n"
        << "  chat_cli group-send --server host:port --username name --password pass --conversation-id id --message text\n"
        << "  chat_cli send-file --server host:port --username name --password pass --to-user-id id --path file\n"
        << "  chat_cli e2e [--server host:port] [--message text]\n";
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

bool parse_u64(const std::string &text, std::uint64_t *out_value)
{
    try {
        size_t consumed = 0;
        const auto value = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || value == 0) {
            return false;
        }
        *out_value = value;
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

bool parse_member_ids(const std::string &text, std::vector<std::uint64_t> *ids)
{
    std::stringstream ss(text);
    std::string item;
    ids->clear();
    while (std::getline(ss, item, ',')) {
        std::uint64_t id = 0;
        if (!parse_u64(item, &id)) {
            return false;
        }
        if (std::find(ids->begin(), ids->end(), id) == ids->end()) {
            ids->push_back(id);
        }
    }
    return !ids->empty() && ids->size() <= LAN_CHAT_MAX_GROUP_MEMBERS;
}

const char *require_value(int argc, char **argv, int *index, const char *name)
{
    if (*index + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
    }
    ++(*index);
    return argv[*index];
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
    lan_chat_tcp_connection_t *connection,
    std::vector<std::uint8_t> *out_packet,
    int timeout_ms,
    const char *label)
{
    out_packet->assign(LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        size_t packet_len = 0;
        if (!flush_once(connection)) {
            return false;
        }
        const lan_chat_status_t status = lan_chat_tcp_connection_recv_packet(
            connection,
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

std::uint32_t read_tlv_u32(const lan_chat_tlv_t &tlv, bool *ok)
{
    std::uint32_t value = 0;
    if (tlv.length != 4 || lan_chat_read_u32_be(tlv.value, tlv.length, 0, &value) != LAN_CHAT_STATUS_OK) {
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
    const lan_chat_status_t status = lan_chat_packet_unpack(header, packet.data(), packet.size(), body, body_len);
    if (status != LAN_CHAT_STATUS_OK) {
        std::cerr << label << ": " << lan_chat_status_name(status) << '\n';
        return false;
    }
    if (header->message_group != group || header->message_type != type || header->flags != flags) {
        std::cerr << label << ": unexpected packet header\n";
        return false;
    }
    return true;
}

bool queue_packet(
    lan_chat_tcp_connection_t *connection,
    std::uint16_t group,
    std::uint16_t type,
    std::uint16_t flags,
    std::uint32_t seq,
    std::uint64_t sender_id,
    std::uint64_t receiver_id,
    std::uint64_t session_id,
    const std::uint8_t *body,
    size_t body_len,
    const char *label)
{
    lan_chat_header_t header{};
    lan_chat_header_init(&header, group, type, flags, static_cast<std::uint32_t>(body_len));
    header.seq = seq;
    header.sender_id = sender_id;
    header.receiver_id = receiver_id;
    header.session_id = session_id;
    const lan_chat_status_t status = lan_chat_tcp_connection_queue_packet(connection, &header, body, body_len);
    if (status != LAN_CHAT_STATUS_OK) {
        std::cerr << label << ": " << lan_chat_status_name(status) << '\n';
        return false;
    }
    return true;
}

bool parse_auth_response(const std::vector<std::uint8_t> &packet, std::uint32_t expected_seq, AuthResult *result)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    bool has_status = false;
    lan_chat_status_t status;

    if (!unpack_packet(packet, &header, &body, &body_len, LAN_CHAT_GROUP_AUTH, LAN_CHAT_MSG_RSP, LAN_CHAT_FLAG_RESPONSE, "auth response")) {
        return false;
    }
    if (header.seq != expected_seq) {
        std::cerr << "auth response seq mismatch\n";
        return false;
    }
    if (lan_chat_tlv_reader_init(&reader, body, body_len) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case RESPONSE_FIELD_STATUS:
            result->status = static_cast<lan_chat_status_t>(read_tlv_u16(tlv, &ok));
            has_status = true;
            break;
        case RESPONSE_FIELD_USER_ID:
            result->user_id = read_tlv_u64(tlv, &ok);
            break;
        case RESPONSE_FIELD_SESSION_ID:
            result->session_id = read_tlv_u64(tlv, &ok);
            break;
        case RESPONSE_FIELD_MESSAGE:
            result->message = read_tlv_string(tlv);
            break;
        default:
            break;
        }
    }
    return status == LAN_CHAT_STATUS_NEED_MORE_DATA && ok && has_status;
}

bool parse_status_response(
    const std::vector<std::uint8_t> &packet,
    std::uint32_t expected_seq,
    std::uint16_t group,
    StatusResult *result)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    bool has_status = false;
    lan_chat_status_t status;

    if (!unpack_packet(packet, &header, &body, &body_len, group, LAN_CHAT_MSG_RSP, LAN_CHAT_FLAG_RESPONSE, "status response")) {
        return false;
    }
    if (header.seq != expected_seq) {
        std::cerr << "status response seq mismatch\n";
        return false;
    }
    if (lan_chat_tlv_reader_init(&reader, body, body_len) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        if (tlv.field_id == RESPONSE_FIELD_STATUS) {
            result->status = static_cast<lan_chat_status_t>(read_tlv_u16(tlv, &ok));
            has_status = true;
        } else if (tlv.field_id == RESPONSE_FIELD_MESSAGE) {
            result->message = read_tlv_string(tlv);
        } else if ((group == LAN_CHAT_GROUP_CHAT && tlv.field_id == CHAT_RESPONSE_FIELD_MESSAGE_ID) ||
                   (group == LAN_CHAT_GROUP_CONVERSATION && tlv.field_id == CONV_RESPONSE_FIELD_CONVERSATION_ID) ||
                   (group == LAN_CHAT_GROUP_FILE && tlv.field_id == FILE_FIELD_TRANSFER_ID)) {
            result->id_a = read_tlv_u64(tlv, &ok);
        } else if ((group == LAN_CHAT_GROUP_CHAT && tlv.field_id == CHAT_RESPONSE_FIELD_DELIVERY_ID) ||
                   (group == LAN_CHAT_GROUP_CONVERSATION && tlv.field_id == CONV_RESPONSE_FIELD_MESSAGE_ID) ||
                   (group == LAN_CHAT_GROUP_FILE && tlv.field_id == FILE_FIELD_MESSAGE_ID)) {
            result->id_b = read_tlv_u64(tlv, &ok);
        } else if (group == LAN_CHAT_GROUP_FILE && tlv.field_id == FILE_FIELD_DELIVERY_ID) {
            result->id_c = read_tlv_u64(tlv, &ok);
        }
    }
    return status == LAN_CHAT_STATUS_NEED_MORE_DATA && ok && has_status;
}

bool parse_private_notify(const std::vector<std::uint8_t> &packet, PrivateNotify *notify)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    bool has_message = false;
    bool has_sender = false;
    bool has_content = false;
    lan_chat_status_t status;

    if (!unpack_packet(packet, &header, &body, &body_len, LAN_CHAT_GROUP_CHAT, LAN_CHAT_MSG_NOTIFY, LAN_CHAT_FLAG_NOTIFY, "private notify")) {
        return false;
    }
    if (lan_chat_tlv_reader_init(&reader, body, body_len) != LAN_CHAT_STATUS_OK) {
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
    return status == LAN_CHAT_STATUS_NEED_MORE_DATA && ok && has_message && has_sender && has_content;
}

bool parse_group_notify(const std::vector<std::uint8_t> &packet, GroupNotify *notify)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    bool has_conversation = false;
    bool has_message = false;
    bool has_sender = false;
    bool has_content = false;
    lan_chat_status_t status;

    if (!unpack_packet(packet, &header, &body, &body_len, LAN_CHAT_GROUP_CONVERSATION, LAN_CHAT_MSG_NOTIFY, LAN_CHAT_FLAG_NOTIFY, "group notify")) {
        return false;
    }
    if (lan_chat_tlv_reader_init(&reader, body, body_len) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case CONV_NOTIFY_FIELD_CONVERSATION_ID:
            notify->conversation_id = read_tlv_u64(tlv, &ok);
            has_conversation = true;
            break;
        case CONV_NOTIFY_FIELD_MESSAGE_ID:
            notify->message_id = read_tlv_u64(tlv, &ok);
            has_message = true;
            break;
        case CONV_NOTIFY_FIELD_SENDER_ID:
            notify->sender_id = read_tlv_u64(tlv, &ok);
            has_sender = true;
            break;
        case CONV_NOTIFY_FIELD_CONTENT:
            notify->content = read_tlv_string(tlv);
            has_content = true;
            break;
        default:
            break;
        }
    }
    return status == LAN_CHAT_STATUS_NEED_MORE_DATA && ok && has_conversation && has_message && has_sender && has_content;
}

bool parse_file_notify(const std::vector<std::uint8_t> &packet, FileNotify *notify)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t body_len = 0;
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    lan_chat_status_t status;

    if (!unpack_packet(packet, &header, &body, &body_len, LAN_CHAT_GROUP_FILE, LAN_CHAT_MSG_NOTIFY, LAN_CHAT_FLAG_NOTIFY, "file notify")) {
        return false;
    }
    if (lan_chat_tlv_reader_init(&reader, body, body_len) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case FILE_FIELD_OPERATION:
            notify->operation = read_tlv_string(tlv);
            break;
        case FILE_FIELD_TRANSFER_ID:
            notify->transfer_id = read_tlv_u64(tlv, &ok);
            break;
        case FILE_FIELD_MESSAGE_ID:
            notify->message_id = read_tlv_u64(tlv, &ok);
            break;
        case FILE_FIELD_DELIVERY_ID:
            notify->delivery_id = read_tlv_u64(tlv, &ok);
            break;
        case FILE_FIELD_FILE_NAME:
            notify->file_name = read_tlv_string(tlv);
            break;
        case FILE_FIELD_FILE_SIZE:
            notify->file_size = read_tlv_u64(tlv, &ok);
            break;
        case FILE_FIELD_CRC32:
            notify->crc32 = read_tlv_u32(tlv, &ok);
            break;
        case FILE_FIELD_CHUNK_INDEX:
            notify->chunk_index = read_tlv_u64(tlv, &ok);
            break;
        case FILE_FIELD_CHUNK_DATA:
            notify->chunk_data.assign(tlv.value, tlv.value + tlv.length);
            break;
        default:
            break;
        }
    }
    return status == LAN_CHAT_STATUS_NEED_MORE_DATA && ok && !notify->operation.empty();
}

bool queue_auth_request(
    lan_chat_tcp_connection_t *connection,
    const char *operation,
    const std::string &username,
    const std::string &password,
    const std::string *nickname,
    std::uint32_t seq)
{
    std::uint8_t body[512]{};
    lan_chat_tlv_writer_t writer{};
    if (lan_chat_tlv_writer_init(&writer, body, sizeof(body)) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (lan_chat_tlv_write_string(&writer, AUTH_FIELD_OPERATION, 1, operation, std::strlen(operation)) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_string(&writer, AUTH_FIELD_USERNAME, 1, username.c_str(), username.size()) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_string(&writer, AUTH_FIELD_PASSWORD, 1, password.c_str(), password.size()) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (nickname != nullptr &&
        lan_chat_tlv_write_string(&writer, AUTH_FIELD_NICKNAME, 0, nickname->c_str(), nickname->size()) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    return queue_packet(
        connection,
        LAN_CHAT_GROUP_AUTH,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        seq,
        0,
        0,
        0,
        body,
        lan_chat_tlv_writer_size(&writer),
        "queue auth");
}

bool login_connected(Client *client, const CommonOptions &options, AuthResult *result)
{
    std::vector<std::uint8_t> packet;
    if (!client->connect(options.server)) {
        return false;
    }
    if (!queue_auth_request(&client->connection, "login", options.username, options.password, nullptr, 1) ||
        !wait_for_packet(&client->connection, &packet, 5000, "login response") ||
        !parse_auth_response(packet, 1, result)) {
        return false;
    }
    if (result->status != LAN_CHAT_STATUS_OK) {
        std::cerr << "login failed: " << lan_chat_status_name(result->status) << '\n';
        return false;
    }
    return true;
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

std::uint32_t crc32_bytes(const std::vector<std::uint8_t> &bytes)
{
    return crc32_update(0, bytes.data(), bytes.size());
}

std::string base_name(const std::filesystem::path &path)
{
    return path.filename().string();
}

std::filesystem::path unique_output_path(const std::filesystem::path &dir, const std::string &file_name)
{
    std::filesystem::path base = dir / std::filesystem::path(file_name).filename();
    if (!std::filesystem::exists(base)) {
        return base;
    }
    const std::string stem = base.stem().string();
    const std::string ext = base.extension().string();
    for (int i = 1; i < 10000; ++i) {
        std::filesystem::path candidate = dir / (stem + "_" + std::to_string(i) + ext);
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return base;
}

bool read_file(const std::filesystem::path &path, std::vector<std::uint8_t> *bytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open file: " << path.string() << '\n';
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > LAN_CHAT_MAX_FILE_SIZE) {
        std::cerr << "file exceeds limit\n";
        return false;
    }
    in.seekg(0, std::ios::beg);
    bytes->assign(static_cast<size_t>(size), 0);
    if (!bytes->empty()) {
        in.read(reinterpret_cast<char *>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    }
    return static_cast<bool>(in) || in.eof();
}

int command_register(int argc, char **argv)
{
    CommonOptions options;
    std::optional<std::string> nickname;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--nickname") {
            const char *value = require_value(argc, argv, &i, "--nickname");
            if (value == nullptr) {
                return 1;
            }
            nickname = std::string(value);
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }
    if (options.username.empty() || options.password.empty()) {
        return 1;
    }

    Client client;
    std::vector<std::uint8_t> packet;
    AuthResult result;
    if (!client.connect(options.server) ||
        !queue_auth_request(&client.connection, "register", options.username, options.password, nickname ? &*nickname : nullptr, 1) ||
        !wait_for_packet(&client.connection, &packet, 5000, "register response") ||
        !parse_auth_response(packet, 1, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "register failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    std::cout << "REGISTER_OK user_id=" << result.user_id << " session_id=" << result.session_id << '\n';
    return 0;
}

int command_login(int argc, char **argv)
{
    CommonOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }
    Client client;
    AuthResult result;
    if (!login_connected(&client, options, &result)) {
        return 1;
    }
    std::cout << "LOGIN_OK user_id=" << result.user_id << " session_id=" << result.session_id << '\n';
    return 0;
}

bool queue_private_send(
    lan_chat_tcp_connection_t *connection,
    const AuthResult &auth,
    std::uint64_t receiver_id,
    const std::string &message,
    std::uint32_t seq)
{
    std::uint8_t body[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 64]{};
    lan_chat_tlv_writer_t writer{};
    if (lan_chat_tlv_writer_init(&writer, body, sizeof(body)) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (lan_chat_tlv_write_u64(&writer, CHAT_FIELD_RECEIVER_ID, 1, receiver_id) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_string(&writer, CHAT_FIELD_CONTENT, 1, message.c_str(), message.size()) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_u64(&writer, CHAT_FIELD_CLIENT_MESSAGE_ID, 0, seq) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    return queue_packet(
        connection,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        seq,
        auth.user_id,
        receiver_id,
        auth.session_id,
        body,
        lan_chat_tlv_writer_size(&writer),
        "queue private send");
}

int command_send(int argc, char **argv)
{
    CommonOptions options;
    std::uint64_t receiver_id = 0;
    std::string message;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--to-user-id") {
            const char *value = require_value(argc, argv, &i, "--to-user-id");
            if (value == nullptr || !parse_u64(value, &receiver_id)) {
                return 1;
            }
        } else if (arg == "--message") {
            const char *value = require_value(argc, argv, &i, "--message");
            if (value == nullptr) {
                return 1;
            }
            message = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }
    if (message.empty() || receiver_id == 0) {
        return 1;
    }

    Client client;
    AuthResult auth;
    std::vector<std::uint8_t> packet;
    StatusResult result;
    if (!login_connected(&client, options, &auth) ||
        !queue_private_send(&client.connection, auth, receiver_id, message, 2) ||
        !wait_for_packet(&client.connection, &packet, 5000, "private send response") ||
        !parse_status_response(packet, 2, LAN_CHAT_GROUP_CHAT, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "send failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    std::cout << "PRIVATE_SEND_OK message_id=" << result.id_a << " delivery_id=" << result.id_b << '\n';
    return 0;
}

struct ReceiveFileState {
    bool active = false;
    std::uint64_t transfer_id = 0;
    std::uint64_t expected_size = 0;
    std::uint32_t expected_crc32 = 0;
    std::uint64_t next_chunk_index = 0;
    std::filesystem::path path;
    std::ofstream out;
    std::uint64_t bytes_written = 0;
    std::uint32_t crc32 = 0;
};

bool safe_file_name(const std::string &file_name)
{
    return !file_name.empty() &&
        file_name.find('/') == std::string::npos &&
        file_name.find('\\') == std::string::npos &&
        file_name != "." &&
        file_name != "..";
}

void discard_partial_file(ReceiveFileState *state)
{
    if (state == nullptr) {
        return;
    }
    if (state->out.is_open()) {
        state->out.close();
    }
    if (!state->path.empty()) {
        std::error_code ec;
        std::filesystem::remove(state->path, ec);
    }
    *state = ReceiveFileState{};
}

bool handle_file_notify(const FileNotify &notify, const std::filesystem::path &output_dir, ReceiveFileState *state)
{
    if (notify.operation == "start") {
        if (output_dir.empty()) {
            std::cerr << "file received but --output-dir was not provided\n";
            return false;
        }
        if (!safe_file_name(notify.file_name) || notify.file_size == 0 || notify.file_size > LAN_CHAT_MAX_FILE_SIZE) {
            std::cerr << "invalid incoming file metadata\n";
            return false;
        }
        if (state->active) {
            discard_partial_file(state);
        }
        std::filesystem::create_directories(output_dir);
        state->active = true;
        state->transfer_id = notify.transfer_id;
        state->expected_size = notify.file_size;
        state->expected_crc32 = notify.crc32;
        state->next_chunk_index = 0;
        state->bytes_written = 0;
        state->crc32 = 0;
        state->path = unique_output_path(output_dir, notify.file_name);
        state->out.open(state->path, std::ios::binary);
        if (!state->out) {
            std::cerr << "cannot open output file\n";
            return false;
        }
        return true;
    }
    if (notify.operation == "chunk") {
        if (!state->active || state->transfer_id != notify.transfer_id || !state->out) {
            std::cerr << "unexpected file chunk\n";
            return false;
        }
        if (notify.chunk_index != state->next_chunk_index) {
            std::cerr << "file chunk out of order\n";
            discard_partial_file(state);
            return false;
        }
        if (state->bytes_written + notify.chunk_data.size() > state->expected_size) {
            std::cerr << "file chunk exceeds expected size\n";
            discard_partial_file(state);
            return false;
        }
        if (!notify.chunk_data.empty()) {
            state->out.write(reinterpret_cast<const char *>(notify.chunk_data.data()), static_cast<std::streamsize>(notify.chunk_data.size()));
            state->bytes_written += notify.chunk_data.size();
            state->crc32 = crc32_update(state->crc32, notify.chunk_data.data(), notify.chunk_data.size());
        }
        ++state->next_chunk_index;
        return static_cast<bool>(state->out);
    }
    if (notify.operation == "complete") {
        if (!state->active || state->transfer_id != notify.transfer_id) {
            std::cerr << "unexpected file complete\n";
            return false;
        }
        state->out.close();
        if (state->bytes_written != state->expected_size || state->crc32 != state->expected_crc32) {
            std::cerr << "file validation failed\n";
            discard_partial_file(state);
            return false;
        }
        std::cout << "FILE_RECEIVED transfer_id=" << state->transfer_id
                  << " path=\"" << state->path.string() << "\""
                  << " size=" << state->bytes_written
                  << " crc32=" << state->crc32 << '\n';
        state->active = false;
        return true;
    }
    return true;
}

int command_listen(int argc, char **argv)
{
    CommonOptions options;
    int expect_count = 1;
    int timeout_ms = 10000;
    std::filesystem::path output_dir;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--expect-count") {
            const char *value = require_value(argc, argv, &i, "--expect-count");
            if (value == nullptr) {
                return 1;
            }
            expect_count = std::max(0, std::atoi(value));
        } else if (arg == "--timeout-ms") {
            const char *value = require_value(argc, argv, &i, "--timeout-ms");
            if (value == nullptr) {
                return 1;
            }
            timeout_ms = std::max(1, std::atoi(value));
        } else if (arg == "--output-dir") {
            const char *value = require_value(argc, argv, &i, "--output-dir");
            if (value == nullptr) {
                return 1;
            }
            output_dir = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }

    Client client;
    AuthResult auth;
    if (!login_connected(&client, options, &auth)) {
        return 1;
    }
    std::cout << "LOGIN_OK user_id=" << auth.user_id << " session_id=" << auth.session_id << '\n';

    int received = 0;
    ReceiveFileState file_state;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (received < expect_count && std::chrono::steady_clock::now() < deadline) {
        std::vector<std::uint8_t> packet;
        const int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (!wait_for_packet(&client.connection, &packet, std::max(1, remaining_ms), "listen packet")) {
            discard_partial_file(&file_state);
            return 1;
        }
        lan_chat_header_t header{};
        const std::uint8_t *body = nullptr;
        size_t body_len = 0;
        if (lan_chat_packet_unpack(&header, packet.data(), packet.size(), &body, &body_len) != LAN_CHAT_STATUS_OK) {
            return 1;
        }
        if (header.message_group == LAN_CHAT_GROUP_CHAT) {
            PrivateNotify notify;
            if (!parse_private_notify(packet, &notify)) {
                discard_partial_file(&file_state);
                return 1;
            }
            std::cout << "PRIVATE_TEXT sender_id=" << notify.sender_id
                      << " message_id=" << notify.message_id
                      << " delivery_id=" << notify.delivery_id
                      << " content=\"" << notify.content << "\"\n";
            ++received;
        } else if (header.message_group == LAN_CHAT_GROUP_CONVERSATION) {
            GroupNotify notify;
            if (!parse_group_notify(packet, &notify)) {
                discard_partial_file(&file_state);
                return 1;
            }
            std::cout << "GROUP_TEXT conversation_id=" << notify.conversation_id
                      << " sender_id=" << notify.sender_id
                      << " message_id=" << notify.message_id
                      << " content=\"" << notify.content << "\"\n";
            ++received;
        } else if (header.message_group == LAN_CHAT_GROUP_FILE) {
            FileNotify notify;
            if (!parse_file_notify(packet, &notify) || !handle_file_notify(notify, output_dir, &file_state)) {
                discard_partial_file(&file_state);
                return 1;
            }
            if (notify.operation == "complete") {
                ++received;
            }
        }
    }
    if (received != expect_count) {
        std::cerr << "listen expected " << expect_count << " event(s), received " << received << '\n';
        discard_partial_file(&file_state);
        return 1;
    }
    return 0;
}

bool queue_group_create(
    lan_chat_tcp_connection_t *connection,
    const AuthResult &auth,
    const std::string &name,
    const std::vector<std::uint64_t> &members,
    std::uint32_t seq)
{
    std::uint8_t body[512]{};
    std::uint8_t member_bytes[LAN_CHAT_MAX_GROUP_MEMBERS * 8]{};
    lan_chat_tlv_writer_t writer{};
    for (size_t i = 0; i < members.size(); ++i) {
        if (lan_chat_write_u64_be(member_bytes, sizeof(member_bytes), i * 8, members[i]) != LAN_CHAT_STATUS_OK) {
            return false;
        }
    }
    if (lan_chat_tlv_writer_init(&writer, body, sizeof(body)) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (lan_chat_tlv_write_string(&writer, CONV_FIELD_OPERATION, 1, "create", 6) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_string(&writer, CONV_FIELD_NAME, 1, name.c_str(), name.size()) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_bytes(&writer, CONV_FIELD_MEMBER_IDS, 1, member_bytes, members.size() * 8) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    return queue_packet(
        connection,
        LAN_CHAT_GROUP_CONVERSATION,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        seq,
        auth.user_id,
        0,
        auth.session_id,
        body,
        lan_chat_tlv_writer_size(&writer),
        "queue group create");
}

int command_group_create(int argc, char **argv)
{
    CommonOptions options;
    std::string name;
    std::vector<std::uint64_t> members;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--name") {
            const char *value = require_value(argc, argv, &i, "--name");
            if (value == nullptr) {
                return 1;
            }
            name = value;
        } else if (arg == "--member-user-ids") {
            const char *value = require_value(argc, argv, &i, "--member-user-ids");
            if (value == nullptr || !parse_member_ids(value, &members)) {
                return 1;
            }
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }

    Client client;
    AuthResult auth;
    StatusResult result;
    std::vector<std::uint8_t> packet;
    if (!login_connected(&client, options, &auth) ||
        !queue_group_create(&client.connection, auth, name, members, 2) ||
        !wait_for_packet(&client.connection, &packet, 5000, "group create response") ||
        !parse_status_response(packet, 2, LAN_CHAT_GROUP_CONVERSATION, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "group create failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    std::cout << "GROUP_CREATE_OK conversation_id=" << result.id_a << '\n';
    return 0;
}

bool queue_group_send(
    lan_chat_tcp_connection_t *connection,
    const AuthResult &auth,
    std::uint64_t conversation_id,
    const std::string &message,
    std::uint32_t seq)
{
    std::uint8_t body[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 128]{};
    lan_chat_tlv_writer_t writer{};
    if (lan_chat_tlv_writer_init(&writer, body, sizeof(body)) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (lan_chat_tlv_write_string(&writer, CONV_FIELD_OPERATION, 1, "send", 4) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_u64(&writer, CONV_FIELD_CONVERSATION_ID, 1, conversation_id) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_string(&writer, CONV_FIELD_CONTENT, 1, message.c_str(), message.size()) != LAN_CHAT_STATUS_OK ||
        lan_chat_tlv_write_u64(&writer, CONV_FIELD_CLIENT_MESSAGE_ID, 0, seq) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    return queue_packet(
        connection,
        LAN_CHAT_GROUP_CONVERSATION,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        seq,
        auth.user_id,
        0,
        auth.session_id,
        body,
        lan_chat_tlv_writer_size(&writer),
        "queue group send");
}

int command_group_send(int argc, char **argv)
{
    CommonOptions options;
    std::uint64_t conversation_id = 0;
    std::string message;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--conversation-id") {
            const char *value = require_value(argc, argv, &i, "--conversation-id");
            if (value == nullptr || !parse_u64(value, &conversation_id)) {
                return 1;
            }
        } else if (arg == "--message") {
            const char *value = require_value(argc, argv, &i, "--message");
            if (value == nullptr) {
                return 1;
            }
            message = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }

    Client client;
    AuthResult auth;
    StatusResult result;
    std::vector<std::uint8_t> packet;
    if (!login_connected(&client, options, &auth) ||
        !queue_group_send(&client.connection, auth, conversation_id, message, 2) ||
        !wait_for_packet(&client.connection, &packet, 5000, "group send response") ||
        !parse_status_response(packet, 2, LAN_CHAT_GROUP_CONVERSATION, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "group send failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    std::cout << "GROUP_SEND_OK conversation_id=" << result.id_a << " message_id=" << result.id_b << '\n';
    return 0;
}

bool queue_file_operation(
    lan_chat_tcp_connection_t *connection,
    const AuthResult &auth,
    std::uint64_t receiver_id,
    const std::string &operation,
    std::uint64_t transfer_id,
    std::uint64_t chunk_index,
    const std::string *file_name,
    std::uint64_t file_size,
    std::uint32_t crc32,
    const std::uint8_t *chunk,
    size_t chunk_len,
    std::uint32_t seq)
{
    std::vector<std::uint8_t> body(LAN_CHAT_FILE_CHUNK_SIZE + 512);
    lan_chat_tlv_writer_t writer{};
    if (lan_chat_tlv_writer_init(&writer, body.data(), body.size()) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (lan_chat_tlv_write_string(&writer, FILE_FIELD_OPERATION, 1, operation.c_str(), operation.size()) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (operation == "start") {
        if (lan_chat_tlv_write_u64(&writer, FILE_FIELD_RECEIVER_ID, 1, receiver_id) != LAN_CHAT_STATUS_OK ||
            lan_chat_tlv_write_string(&writer, FILE_FIELD_FILE_NAME, 1, file_name->c_str(), file_name->size()) != LAN_CHAT_STATUS_OK ||
            lan_chat_tlv_write_u64(&writer, FILE_FIELD_FILE_SIZE, 1, file_size) != LAN_CHAT_STATUS_OK ||
            lan_chat_tlv_write_u32(&writer, FILE_FIELD_CRC32, 1, crc32) != LAN_CHAT_STATUS_OK) {
            return false;
        }
    } else {
        if (lan_chat_tlv_write_u64(&writer, FILE_FIELD_TRANSFER_ID, 1, transfer_id) != LAN_CHAT_STATUS_OK) {
            return false;
        }
        if (operation == "chunk" &&
            (lan_chat_tlv_write_u64(&writer, FILE_FIELD_CHUNK_INDEX, 1, chunk_index) != LAN_CHAT_STATUS_OK ||
             lan_chat_tlv_write_bytes(&writer, FILE_FIELD_CHUNK_DATA, 1, chunk, chunk_len) != LAN_CHAT_STATUS_OK)) {
            return false;
        }
    }
    return queue_packet(
        connection,
        LAN_CHAT_GROUP_FILE,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        seq,
        auth.user_id,
        receiver_id,
        auth.session_id,
        body.data(),
        lan_chat_tlv_writer_size(&writer),
        "queue file operation");
}

int command_send_file(int argc, char **argv)
{
    CommonOptions options;
    std::uint64_t receiver_id = 0;
    std::filesystem::path path;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--to-user-id") {
            const char *value = require_value(argc, argv, &i, "--to-user-id");
            if (value == nullptr || !parse_u64(value, &receiver_id)) {
                return 1;
            }
        } else if (arg == "--path") {
            const char *value = require_value(argc, argv, &i, "--path");
            if (value == nullptr) {
                return 1;
            }
            path = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }

    std::vector<std::uint8_t> bytes;
    if (receiver_id == 0 || path.empty() || !read_file(path, &bytes)) {
        return 1;
    }
    const std::string name = base_name(path);
    const std::uint32_t crc = crc32_bytes(bytes);
    Client client;
    AuthResult auth;
    StatusResult result;
    std::vector<std::uint8_t> packet;
    if (!login_connected(&client, options, &auth) ||
        !queue_file_operation(&client.connection, auth, receiver_id, "start", 0, 0, &name, bytes.size(), crc, nullptr, 0, 2) ||
        !wait_for_packet(&client.connection, &packet, 5000, "file start response") ||
        !parse_status_response(packet, 2, LAN_CHAT_GROUP_FILE, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "file start failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    const std::uint64_t transfer_id = result.id_a;
    std::uint32_t seq = 3;
    for (size_t offset = 0, chunk_index = 0; offset < bytes.size(); offset += LAN_CHAT_FILE_CHUNK_SIZE, ++chunk_index, ++seq) {
        const size_t chunk_len = std::min<size_t>(LAN_CHAT_FILE_CHUNK_SIZE, bytes.size() - offset);
        StatusResult chunk_result;
        if (!queue_file_operation(&client.connection, auth, receiver_id, "chunk", transfer_id, chunk_index, nullptr, 0, 0, bytes.data() + offset, chunk_len, seq) ||
            !wait_for_packet(&client.connection, &packet, 5000, "file chunk response") ||
            !parse_status_response(packet, seq, LAN_CHAT_GROUP_FILE, &chunk_result) ||
            chunk_result.status != LAN_CHAT_STATUS_OK) {
            std::cerr << "file chunk failed\n";
            return 1;
        }
    }
    StatusResult complete_result;
    if (!queue_file_operation(&client.connection, auth, receiver_id, "complete", transfer_id, 0, nullptr, 0, 0, nullptr, 0, seq) ||
        !wait_for_packet(&client.connection, &packet, 5000, "file complete response") ||
        !parse_status_response(packet, seq, LAN_CHAT_GROUP_FILE, &complete_result) ||
        complete_result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "file complete failed\n";
        return 1;
    }
    std::cout << "FILE_SEND_OK transfer_id=" << transfer_id
              << " message_id=" << result.id_b
              << " delivery_id=" << result.id_c
              << " size=" << bytes.size()
              << " crc32=" << crc << '\n';
    return 0;
}

std::string make_unique_suffix()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool run_e2e(const Endpoint &server, const std::string &message)
{
    const std::string suffix = make_unique_suffix();
    const std::string alice_name = "alice_app_" + suffix;
    const std::string bob_name = "bob_app_" + suffix;
    const std::string alice_password = "alice-password-" + suffix;
    const std::string bob_password = "bob-password-" + suffix;

    {
        const char *alice_argv[] = {"chat_cli", "register", "--server", "", "--username", "", "--password", "", "--nickname", "Alice App"};
        (void)alice_argv;
    }

    Client alice;
    Client bob;
    std::vector<std::uint8_t> packet;
    AuthResult alice_register;
    AuthResult bob_register;
    AuthResult alice_login;
    AuthResult bob_login;
    StatusResult send_result;
    PrivateNotify notify;

    if (!alice.connect(server) || !bob.connect(server)) {
        return false;
    }
    if (!queue_auth_request(&alice.connection, "register", alice_name, alice_password, nullptr, 1) ||
        !wait_for_packet(&alice.connection, &packet, 5000, "alice register") ||
        !parse_auth_response(packet, 1, &alice_register) ||
        alice_register.status != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!queue_auth_request(&bob.connection, "register", bob_name, bob_password, nullptr, 2) ||
        !wait_for_packet(&bob.connection, &packet, 5000, "bob register") ||
        !parse_auth_response(packet, 2, &bob_register) ||
        bob_register.status != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!queue_auth_request(&alice.connection, "login", alice_name, alice_password, nullptr, 3) ||
        !wait_for_packet(&alice.connection, &packet, 5000, "alice login") ||
        !parse_auth_response(packet, 3, &alice_login) ||
        alice_login.status != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!queue_auth_request(&bob.connection, "login", bob_name, bob_password, nullptr, 4) ||
        !wait_for_packet(&bob.connection, &packet, 5000, "bob login") ||
        !parse_auth_response(packet, 4, &bob_login) ||
        bob_login.status != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!queue_private_send(&alice.connection, alice_login, bob_login.user_id, message, 5) ||
        !wait_for_packet(&alice.connection, &packet, 5000, "private e2e send") ||
        !parse_status_response(packet, 5, LAN_CHAT_GROUP_CHAT, &send_result) ||
        send_result.status != LAN_CHAT_STATUS_OK ||
        !wait_for_packet(&bob.connection, &packet, 5000, "private e2e notify") ||
        !parse_private_notify(packet, &notify) ||
        notify.sender_id != alice_login.user_id ||
        notify.content != message) {
        return false;
    }
    std::cout << "alice user_id=" << alice_login.user_id << " session_id=" << alice_login.session_id << '\n';
    std::cout << "bob user_id=" << bob_login.user_id << " session_id=" << bob_login.session_id << '\n';
    std::cout << "chat message_id=" << send_result.id_a << " delivery_id=" << send_result.id_b << '\n';
    std::cout << "notify sender_id=" << notify.sender_id << " content=\"" << notify.content << "\"\n";
    std::cout << "APP_E2E_OK\n";
    return true;
}

int command_e2e(int argc, char **argv)
{
    Endpoint server;
    std::string message = "hello from chat_cli e2e";
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &server)) {
                return 1;
            }
        } else if (arg == "--message") {
            const char *value = require_value(argc, argv, &i, "--message");
            if (value == nullptr) {
                return 1;
            }
            message = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }
    return run_e2e(server, message) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        print_usage();
        return argc < 2 ? 1 : 0;
    }

    const std::string command = argv[1];
    if (command == "register") {
        return command_register(argc, argv);
    }
    if (command == "login") {
        return command_login(argc, argv);
    }
    if (command == "send") {
        return command_send(argc, argv);
    }
    if (command == "listen") {
        return command_listen(argc, argv);
    }
    if (command == "group-create") {
        return command_group_create(argc, argv);
    }
    if (command == "group-send") {
        return command_group_send(argc, argv);
    }
    if (command == "send-file") {
        return command_send_file(argc, argv);
    }
    if (command == "e2e") {
        return command_e2e(argc, argv);
    }

    std::cerr << "unknown command: " << command << '\n';
    print_usage();
    return 1;
}
