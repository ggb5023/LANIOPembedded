#ifndef LAN_CHAT_APPS_CHAT_CLI_CLI_SUPPORT_HPP
#define LAN_CHAT_APPS_CHAT_CLI_CLI_SUPPORT_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "lan_chat/core/protocol.h"
#include "lan_chat/core/status.h"
#include "lan_chat/transport/transport.h"
}

namespace lan_chat_cli {

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

    ~Client();
    bool connect(const Endpoint &endpoint);
    void close();
    static bool expect_status(lan_chat_status_t status, const char *label);
};

struct ReceiveFileState {
    bool active = false;
    std::uint64_t transfer_id = 0;
    std::uint64_t expected_size = 0;
    std::uint32_t expected_crc32 = 0;
    std::uint64_t next_chunk_index = 0;
    std::filesystem::path path;
    std::uint64_t bytes_written = 0;
    std::uint32_t crc32 = 0;

    struct Impl;
    Impl *impl = nullptr;

    ReceiveFileState();
    ~ReceiveFileState();
    ReceiveFileState(const ReceiveFileState &) = delete;
    ReceiveFileState &operator=(const ReceiveFileState &) = delete;
    ReceiveFileState(ReceiveFileState &&) = delete;
    ReceiveFileState &operator=(ReceiveFileState &&) = delete;
};

bool parse_u16(const std::string &text, std::uint16_t *out_value);
bool parse_u64(const std::string &text, std::uint64_t *out_value);
bool parse_endpoint(const std::string &text, Endpoint *endpoint);
bool parse_member_ids(const std::string &text, std::vector<std::uint64_t> *ids);
const char *require_value(int argc, char **argv, int *index, const char *name);

bool flush_once(lan_chat_tcp_connection_t *connection);
bool wait_for_packet(lan_chat_tcp_connection_t *connection, std::vector<std::uint8_t> *out_packet, int timeout_ms, const char *label);
bool unpack_packet(
    const std::vector<std::uint8_t> &packet,
    lan_chat_header_t *header,
    const std::uint8_t **body,
    size_t *body_len,
    std::uint16_t group,
    std::uint16_t type,
    std::uint16_t flags,
    const char *label);
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
    const char *label);

bool parse_auth_response(const std::vector<std::uint8_t> &packet, std::uint32_t expected_seq, AuthResult *result);
bool parse_status_response(const std::vector<std::uint8_t> &packet, std::uint32_t expected_seq, std::uint16_t group, StatusResult *result);
bool parse_private_notify(const std::vector<std::uint8_t> &packet, PrivateNotify *notify);
bool parse_group_notify(const std::vector<std::uint8_t> &packet, GroupNotify *notify);
bool parse_file_notify(const std::vector<std::uint8_t> &packet, FileNotify *notify);

bool queue_auth_request(
    lan_chat_tcp_connection_t *connection,
    const char *operation,
    const std::string &username,
    const std::string &password,
    const std::string *nickname,
    std::uint32_t seq);
bool login_connected(Client *client, const CommonOptions &options, AuthResult *result);
bool queue_private_send(
    lan_chat_tcp_connection_t *connection,
    const AuthResult &auth,
    std::uint64_t receiver_id,
    const std::string &message,
    std::uint32_t seq);
bool queue_group_create(
    lan_chat_tcp_connection_t *connection,
    const AuthResult &auth,
    const std::string &name,
    const std::vector<std::uint64_t> &members,
    std::uint32_t seq);
bool queue_group_send(
    lan_chat_tcp_connection_t *connection,
    const AuthResult &auth,
    std::uint64_t conversation_id,
    const std::string &message,
    std::uint32_t seq);
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
    std::uint32_t seq);

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *data, size_t len);
std::uint32_t crc32_bytes(const std::vector<std::uint8_t> &bytes);
std::string base_name(const std::filesystem::path &path);
bool read_file(const std::filesystem::path &path, std::vector<std::uint8_t> *bytes);
bool handle_file_notify(const FileNotify &notify, const std::filesystem::path &output_dir, ReceiveFileState *state);
void discard_partial_file(ReceiveFileState *state);

} // namespace lan_chat_cli

#endif
