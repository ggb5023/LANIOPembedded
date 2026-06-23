#include "cli_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <thread>

extern "C" {
#include "lan_chat/core/endian.h"
#include "lan_chat/core/tlv.h"
}

namespace lan_chat_cli {

struct ReceiveFileState::Impl {
    std::ofstream out;
};

ReceiveFileState::ReceiveFileState() : impl(new Impl) {}

ReceiveFileState::~ReceiveFileState()
{
    discard_partial_file(this);
    delete impl;
    impl = nullptr;
}

Client::~Client()
{
    close();
}

bool Client::connect(const Endpoint &endpoint)
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

void Client::close()
{
    if (connection.is_open) {
        lan_chat_tcp_connection_close(&connection);
    }
    if (transport_initialized) {
        lan_chat_transport_shutdown(&transport);
        transport_initialized = false;
    }
}

bool Client::expect_status(lan_chat_status_t status, const char *label)
{
    if (status == LAN_CHAT_STATUS_OK) {
        return true;
    }
    std::cerr << label << ": " << lan_chat_status_name(status) << '\n';
    return false;
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


bool safe_file_name(const std::string &file_name)
{
    return !file_name.empty() &&
        file_name.find('/') == std::string::npos &&
        file_name.find('\\') == std::string::npos &&
        file_name != "." &&
        file_name != "..";
}

void reset_file_state(ReceiveFileState *state)
{
    state->active = false;
    state->transfer_id = 0;
    state->expected_size = 0;
    state->expected_crc32 = 0;
    state->next_chunk_index = 0;
    state->path.clear();
    state->bytes_written = 0;
    state->crc32 = 0;
}

void discard_partial_file(ReceiveFileState *state)
{
    if (state == nullptr) {
        return;
    }
    const bool remove_partial = state->active && !state->path.empty();
    if (state->impl != nullptr && state->impl->out.is_open()) {
        state->impl->out.close();
    }
    if (remove_partial) {
        std::error_code ec;
        std::filesystem::remove(state->path, ec);
    }
    reset_file_state(state);
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
        state->impl->out.open(state->path, std::ios::binary);
        if (!state->impl->out) {
            std::cerr << "cannot open output file\n";
            return false;
        }
        return true;
    }
    if (notify.operation == "chunk") {
        if (!state->active || state->transfer_id != notify.transfer_id || !state->impl->out) {
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
            state->impl->out.write(reinterpret_cast<const char *>(notify.chunk_data.data()), static_cast<std::streamsize>(notify.chunk_data.size()));
            state->bytes_written += notify.chunk_data.size();
            state->crc32 = crc32_update(state->crc32, notify.chunk_data.data(), notify.chunk_data.size());
        }
        ++state->next_chunk_index;
        return static_cast<bool>(state->impl->out);
    }
    if (notify.operation == "complete") {
        if (!state->active || state->transfer_id != notify.transfer_id) {
            std::cerr << "unexpected file complete\n";
            return false;
        }
        state->impl->out.close();
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



} // namespace lan_chat_cli
