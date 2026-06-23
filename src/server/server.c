#include "lan_chat/server/server.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lan_chat/core/endian.h"
#include "lan_chat/core/tlv.h"

enum {
    LAN_CHAT_SERVER_MAX_CLIENTS = 16,
    LAN_CHAT_SERVER_PACKET_BUFFER_SIZE = LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE,
    LAN_CHAT_AUTH_FIELD_OPERATION = 1,
    LAN_CHAT_AUTH_FIELD_USERNAME = 2,
    LAN_CHAT_AUTH_FIELD_PASSWORD = 3,
    LAN_CHAT_AUTH_FIELD_NICKNAME = 4,
    LAN_CHAT_RESPONSE_FIELD_STATUS = 1,
    LAN_CHAT_RESPONSE_FIELD_USER_ID = 2,
    LAN_CHAT_RESPONSE_FIELD_SESSION_ID = 3,
    LAN_CHAT_RESPONSE_FIELD_MESSAGE = 4,
    LAN_CHAT_CHAT_FIELD_RECEIVER_ID = 1,
    LAN_CHAT_CHAT_FIELD_CONTENT = 2,
    LAN_CHAT_CHAT_FIELD_CLIENT_MESSAGE_ID = 3,
    LAN_CHAT_CHAT_RESPONSE_FIELD_MESSAGE_ID = 2,
    LAN_CHAT_CHAT_RESPONSE_FIELD_DELIVERY_ID = 3,
    LAN_CHAT_CHAT_NOTIFY_FIELD_MESSAGE_ID = 1,
    LAN_CHAT_CHAT_NOTIFY_FIELD_SENDER_ID = 2,
    LAN_CHAT_CHAT_NOTIFY_FIELD_CONTENT = 3,
    LAN_CHAT_CHAT_NOTIFY_FIELD_DELIVERY_ID = 4
};

typedef struct parsed_auth_request {
    char operation[16];
    char username[LAN_CHAT_MAX_USERNAME_LEN + 1];
    char password[LAN_CHAT_PASSWORD_HASH_LEN + 1];
    char nickname[LAN_CHAT_MAX_NICKNAME_LEN + 1];
    int has_nickname;
} parsed_auth_request_t;

typedef struct parsed_chat_request {
    lan_chat_user_id_t receiver_id;
    char content[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 1];
    uint64_t client_message_id;
} parsed_chat_request_t;

struct lan_chat_server_client {
    uint64_t connection_id;
    lan_chat_tcp_connection_t connection;
    uint8_t authenticated;
    lan_chat_user_id_t user_id;
    lan_chat_session_id_t session_id;
    char username[LAN_CHAT_MAX_USERNAME_LEN + 1];
    uint64_t last_activity_ms;
};

static size_t bounded_strlen(const char *value, size_t max_len)
{
    size_t len = 0;

    if (value == 0) {
        return 0;
    }
    while (len <= max_len && value[len] != '\0') {
        ++len;
    }
    return len;
}

static int string_is_valid(const char *value, size_t max_len, int allow_empty)
{
    size_t len;

    if (value == 0) {
        return 0;
    }
    len = bounded_strlen(value, max_len);
    if (len > max_len) {
        return 0;
    }
    if (!allow_empty && len == 0) {
        return 0;
    }
    return 1;
}

static void copy_string(char *dest, size_t dest_capacity, const char *src, size_t src_len)
{
    size_t len;

    if (dest == 0 || dest_capacity == 0) {
        return;
    }
    dest[0] = '\0';
    if (src == 0) {
        return;
    }
    len = src_len;
    if (len >= dest_capacity) {
        len = dest_capacity - 1;
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
}

static uint64_t fnv1a64(const char *value)
{
    uint64_t hash = 1469598103934665603ULL;

    if (value == 0) {
        return hash;
    }
    while (*value != '\0') {
        hash ^= (unsigned char)*value;
        hash *= 1099511628211ULL;
        ++value;
    }
    return hash;
}

static void make_dev_salt(const char *username, char *out_salt, size_t out_capacity)
{
    (void)snprintf(out_salt, out_capacity, "dev-salt-%016llx", (unsigned long long)fnv1a64(username));
}

static void make_dev_hash(const char *password, const char *salt, char *out_hash, size_t out_capacity)
{
    char combined[LAN_CHAT_PASSWORD_HASH_LEN + LAN_CHAT_PASSWORD_SALT_LEN + 2];
    (void)snprintf(combined, sizeof(combined), "%s:%s", salt != 0 ? salt : "", password != 0 ? password : "");
    (void)snprintf(out_hash, out_capacity, "v1-dev:%s:%016llx", salt, (unsigned long long)fnv1a64(combined));
}

static lan_chat_status_t read_tlv_u64(const lan_chat_tlv_t *tlv, uint64_t *out_value)
{
    if (tlv == 0 || out_value == 0 || tlv->length != 8) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    return lan_chat_read_u64_be(tlv->value, tlv->length, 0, out_value);
}

static lan_chat_status_t parse_auth_request(const uint8_t *body, size_t body_len, parsed_auth_request_t *out)
{
    lan_chat_tlv_reader_t reader;
    lan_chat_tlv_t tlv;
    lan_chat_status_t status;
    int has_operation = 0;
    int has_username = 0;
    int has_password = 0;

    if (out == 0 || (body == 0 && body_len > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    status = lan_chat_tlv_reader_init(&reader, body, body_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case LAN_CHAT_AUTH_FIELD_OPERATION:
            copy_string(out->operation, sizeof(out->operation), (const char *)tlv.value, tlv.length);
            has_operation = 1;
            break;
        case LAN_CHAT_AUTH_FIELD_USERNAME:
            copy_string(out->username, sizeof(out->username), (const char *)tlv.value, tlv.length);
            has_username = 1;
            break;
        case LAN_CHAT_AUTH_FIELD_PASSWORD:
            copy_string(out->password, sizeof(out->password), (const char *)tlv.value, tlv.length);
            has_password = 1;
            break;
        case LAN_CHAT_AUTH_FIELD_NICKNAME:
            copy_string(out->nickname, sizeof(out->nickname), (const char *)tlv.value, tlv.length);
            out->has_nickname = 1;
            break;
        default:
            if (tlv.required) {
                return LAN_CHAT_STATUS_UNSUPPORTED_FIELD;
            }
            break;
        }
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
        return status;
    }
    if (!has_operation || !has_username || !has_password) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    if (!string_is_valid(out->username, LAN_CHAT_MAX_USERNAME_LEN, 0) ||
        !string_is_valid(out->password, LAN_CHAT_PASSWORD_HASH_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t parse_chat_request(const uint8_t *body, size_t body_len, parsed_chat_request_t *out)
{
    lan_chat_tlv_reader_t reader;
    lan_chat_tlv_t tlv;
    lan_chat_status_t status;
    int has_receiver = 0;
    int has_content = 0;

    if (out == 0 || (body == 0 && body_len > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    status = lan_chat_tlv_reader_init(&reader, body, body_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case LAN_CHAT_CHAT_FIELD_RECEIVER_ID:
            status = read_tlv_u64(&tlv, &out->receiver_id);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            has_receiver = 1;
            break;
        case LAN_CHAT_CHAT_FIELD_CONTENT:
            copy_string(out->content, sizeof(out->content), (const char *)tlv.value, tlv.length);
            has_content = 1;
            break;
        case LAN_CHAT_CHAT_FIELD_CLIENT_MESSAGE_ID:
            status = read_tlv_u64(&tlv, &out->client_message_id);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            break;
        default:
            if (tlv.required) {
                return LAN_CHAT_STATUS_UNSUPPORTED_FIELD;
            }
            break;
        }
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
        return status;
    }
    if (!has_receiver || !has_content || out->receiver_id == 0 ||
        !string_is_valid(out->content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t queue_packet(
    lan_chat_tcp_connection_t *connection,
    const lan_chat_header_t *request_header,
    uint16_t group,
    uint16_t type,
    uint16_t flags,
    const uint8_t *body,
    size_t body_len,
    lan_chat_user_id_t sender_id,
    lan_chat_user_id_t receiver_id,
    lan_chat_session_id_t session_id)
{
    lan_chat_header_t response;

    lan_chat_header_init(&response, group, type, flags, (uint32_t)body_len);
    if (request_header != 0) {
        response.seq = request_header->seq;
    }
    response.sender_id = sender_id;
    response.receiver_id = receiver_id;
    response.session_id = session_id;
    return lan_chat_tcp_connection_queue_packet(connection, &response, body, body_len);
}

static lan_chat_status_t queue_status_response(
    lan_chat_tcp_connection_t *connection,
    const lan_chat_header_t *request_header,
    uint16_t group,
    lan_chat_status_t status_code,
    lan_chat_user_id_t user_id,
    lan_chat_session_id_t session_id,
    lan_chat_message_id_t message_id,
    lan_chat_delivery_id_t delivery_id,
    const char *message)
{
    uint8_t body[512];
    lan_chat_tlv_writer_t writer;
    lan_chat_status_t status;

    status = lan_chat_tlv_writer_init(&writer, body, sizeof(body));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = lan_chat_tlv_write_u16(&writer, LAN_CHAT_RESPONSE_FIELD_STATUS, 1, (uint16_t)status_code);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (group == LAN_CHAT_GROUP_AUTH && status_code == LAN_CHAT_STATUS_OK) {
        if ((status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_RESPONSE_FIELD_USER_ID, 0, user_id)) != LAN_CHAT_STATUS_OK ||
            (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_RESPONSE_FIELD_SESSION_ID, 0, session_id)) != LAN_CHAT_STATUS_OK) {
            return status;
        }
    }
    if (group == LAN_CHAT_GROUP_CHAT && status_code == LAN_CHAT_STATUS_OK) {
        if ((status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CHAT_RESPONSE_FIELD_MESSAGE_ID, 0, message_id)) != LAN_CHAT_STATUS_OK ||
            (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CHAT_RESPONSE_FIELD_DELIVERY_ID, 0, delivery_id)) != LAN_CHAT_STATUS_OK) {
            return status;
        }
    }
    if (message != 0 && message[0] != '\0') {
        status = lan_chat_tlv_write_string(&writer, LAN_CHAT_RESPONSE_FIELD_MESSAGE, 0, message, bounded_strlen(message, 255));
        if (status != LAN_CHAT_STATUS_OK) {
            return status;
        }
    }
    return queue_packet(
        connection,
        request_header,
        group,
        LAN_CHAT_MSG_RSP,
        LAN_CHAT_FLAG_RESPONSE,
        body,
        lan_chat_tlv_writer_size(&writer),
        0,
        request_header != 0 ? request_header->sender_id : 0,
        session_id);
}

static lan_chat_status_t queue_chat_notify(
    lan_chat_tcp_connection_t *connection,
    lan_chat_message_id_t message_id,
    lan_chat_delivery_id_t delivery_id,
    lan_chat_user_id_t sender_id,
    lan_chat_user_id_t receiver_id,
    lan_chat_session_id_t session_id,
    const char *content)
{
    uint8_t body[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 64];
    lan_chat_tlv_writer_t writer;
    lan_chat_status_t status;

    status = lan_chat_tlv_writer_init(&writer, body, sizeof(body));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if ((status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CHAT_NOTIFY_FIELD_MESSAGE_ID, 1, message_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CHAT_NOTIFY_FIELD_SENDER_ID, 1, sender_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_string(&writer, LAN_CHAT_CHAT_NOTIFY_FIELD_CONTENT, 1, content, bounded_strlen(content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN))) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CHAT_NOTIFY_FIELD_DELIVERY_ID, 0, delivery_id)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return queue_packet(
        connection,
        0,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_MSG_NOTIFY,
        LAN_CHAT_FLAG_NOTIFY,
        body,
        lan_chat_tlv_writer_size(&writer),
        sender_id,
        receiver_id,
        session_id);
}

static void close_client(lan_chat_server_t *server, size_t index)
{
    if (server == 0 || server->clients == 0 || index >= server->client_capacity) {
        return;
    }
    if (server->clients[index].connection.is_open) {
        lan_chat_tcp_connection_close(&server->clients[index].connection);
    }
    memset(&server->clients[index], 0, sizeof(server->clients[index]));
}

static size_t find_free_client(lan_chat_server_t *server)
{
    size_t i;

    if (server == 0 || server->clients == 0) {
        return LAN_CHAT_SERVER_MAX_CLIENTS;
    }
    for (i = 0; i < server->client_capacity; ++i) {
        if (!server->clients[i].connection.is_open) {
            return i;
        }
    }
    return LAN_CHAT_SERVER_MAX_CLIENTS;
}

static size_t find_client_by_user_id(lan_chat_server_t *server, lan_chat_user_id_t user_id)
{
    size_t i;

    if (server == 0 || server->clients == 0) {
        return LAN_CHAT_SERVER_MAX_CLIENTS;
    }
    for (i = 0; i < server->client_capacity; ++i) {
        if (server->clients[i].connection.is_open &&
            server->clients[i].authenticated &&
            server->clients[i].user_id == user_id) {
            return i;
        }
    }
    return LAN_CHAT_SERVER_MAX_CLIENTS;
}

static void authenticate_client(
    lan_chat_server_t *server,
    size_t client_index,
    lan_chat_user_id_t user_id,
    const char *username)
{
    server->clients[client_index].authenticated = 1;
    server->clients[client_index].user_id = user_id;
    server->clients[client_index].session_id = server->next_session_id++;
    copy_string(server->clients[client_index].username, sizeof(server->clients[client_index].username), username, bounded_strlen(username, LAN_CHAT_MAX_USERNAME_LEN));
}

static lan_chat_status_t handle_register(
    lan_chat_server_t *server,
    size_t client_index,
    const lan_chat_header_t *header,
    const parsed_auth_request_t *request)
{
    lan_chat_account_create_t account;
    lan_chat_user_id_t user_id = 0;
    char salt[LAN_CHAT_PASSWORD_SALT_LEN + 1];
    char hash[LAN_CHAT_PASSWORD_HASH_LEN + 1];
    lan_chat_status_t status;

    make_dev_salt(request->username, salt, sizeof(salt));
    make_dev_hash(request->password, salt, hash, sizeof(hash));
    memset(&account, 0, sizeof(account));
    account.username = request->username;
    account.nickname = request->has_nickname ? request->nickname : request->username;
    account.password_hash = hash;
    account.password_salt = salt;
    status = lan_chat_storage_create_account(server->storage, &account, &user_id);
    if (status == LAN_CHAT_STATUS_OK) {
        authenticate_client(server, client_index, user_id, request->username);
        return queue_status_response(
            &server->clients[client_index].connection,
            header,
            LAN_CHAT_GROUP_AUTH,
            LAN_CHAT_STATUS_OK,
            user_id,
            server->clients[client_index].session_id,
            0,
            0,
            "registered");
    }
    return queue_status_response(
        &server->clients[client_index].connection,
        header,
        LAN_CHAT_GROUP_AUTH,
        status,
        0,
        0,
        0,
        0,
        "register failed");
}

static lan_chat_status_t handle_login(
    lan_chat_server_t *server,
    size_t client_index,
    const lan_chat_header_t *header,
    const parsed_auth_request_t *request)
{
    lan_chat_login_record_t record;
    char hash[LAN_CHAT_PASSWORD_HASH_LEN + 1];
    lan_chat_status_t status;

    status = lan_chat_storage_get_login_record(server->storage, request->username, &record);
    if (status != LAN_CHAT_STATUS_OK) {
        return queue_status_response(
            &server->clients[client_index].connection,
            header,
            LAN_CHAT_GROUP_AUTH,
            status,
            0,
            0,
            0,
            0,
            "login failed");
    }
    if (!record.enabled) {
        status = LAN_CHAT_STATUS_INVALID_STATE;
    } else {
        make_dev_hash(request->password, record.password_salt, hash, sizeof(hash));
        status = strcmp(hash, record.password_hash) == 0 ? LAN_CHAT_STATUS_OK : LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (status == LAN_CHAT_STATUS_OK) {
        (void)lan_chat_storage_update_last_login(server->storage, record.user_id, 0);
        authenticate_client(server, client_index, record.user_id, request->username);
        return queue_status_response(
            &server->clients[client_index].connection,
            header,
            LAN_CHAT_GROUP_AUTH,
            LAN_CHAT_STATUS_OK,
            record.user_id,
            server->clients[client_index].session_id,
            0,
            0,
            "logged in");
    }
    return queue_status_response(
        &server->clients[client_index].connection,
        header,
        LAN_CHAT_GROUP_AUTH,
        status,
        0,
        0,
        0,
        0,
        "login failed");
}

static lan_chat_status_t handle_auth(
    lan_chat_server_t *server,
    size_t client_index,
    const lan_chat_header_t *header,
    const uint8_t *body,
    size_t body_len)
{
    parsed_auth_request_t request;
    lan_chat_status_t status = parse_auth_request(body, body_len, &request);

    if (status != LAN_CHAT_STATUS_OK) {
        return queue_status_response(&server->clients[client_index].connection, header, LAN_CHAT_GROUP_AUTH, status, 0, 0, 0, 0, "bad auth request");
    }
    if (strcmp(request.operation, "register") == 0) {
        return handle_register(server, client_index, header, &request);
    }
    if (strcmp(request.operation, "login") == 0) {
        return handle_login(server, client_index, header, &request);
    }
    return queue_status_response(&server->clients[client_index].connection, header, LAN_CHAT_GROUP_AUTH, LAN_CHAT_STATUS_INVALID_ARGUMENT, 0, 0, 0, 0, "unknown auth operation");
}

static lan_chat_status_t handle_chat(
    lan_chat_server_t *server,
    size_t client_index,
    const lan_chat_header_t *header,
    const uint8_t *body,
    size_t body_len)
{
    parsed_chat_request_t request;
    lan_chat_message_record_t message;
    lan_chat_message_id_t message_id = 0;
    lan_chat_delivery_id_t delivery_id = 0;
    lan_chat_status_t status;
    size_t receiver_index;

    if (!server->clients[client_index].authenticated) {
        return queue_status_response(&server->clients[client_index].connection, header, LAN_CHAT_GROUP_CHAT, LAN_CHAT_STATUS_INVALID_STATE, 0, 0, 0, 0, "login required");
    }
    status = parse_chat_request(body, body_len, &request);
    if (status != LAN_CHAT_STATUS_OK) {
        return queue_status_response(&server->clients[client_index].connection, header, LAN_CHAT_GROUP_CHAT, status, 0, 0, 0, 0, "bad chat request");
    }

    memset(&message, 0, sizeof(message));
    message.sender_id = server->clients[client_index].user_id;
    message.receiver_id = request.receiver_id;
    message.content_type = 1;
    copy_string(message.content, sizeof(message.content), request.content, bounded_strlen(request.content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN));
    message.client_message_id = request.client_message_id;
    status = lan_chat_storage_store_private_message(server->storage, &message, &message_id, &delivery_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return queue_status_response(&server->clients[client_index].connection, header, LAN_CHAT_GROUP_CHAT, status, 0, 0, 0, 0, "store failed");
    }

    status = queue_status_response(
        &server->clients[client_index].connection,
        header,
        LAN_CHAT_GROUP_CHAT,
        LAN_CHAT_STATUS_OK,
        0,
        0,
        message_id,
        delivery_id,
        "sent");
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    receiver_index = find_client_by_user_id(server, request.receiver_id);
    if (receiver_index < server->client_capacity) {
        (void)queue_chat_notify(
            &server->clients[receiver_index].connection,
            message_id,
            delivery_id,
            server->clients[client_index].user_id,
            request.receiver_id,
            server->clients[receiver_index].session_id,
            request.content);
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t dispatch_packet(
    lan_chat_server_t *server,
    size_t client_index,
    const uint8_t *packet,
    size_t packet_len)
{
    lan_chat_header_t header;
    const uint8_t *body = 0;
    size_t body_len = 0;
    lan_chat_status_t status;

    status = lan_chat_packet_unpack(&header, packet, packet_len, &body, &body_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (header.message_group == LAN_CHAT_GROUP_AUTH && header.message_type == LAN_CHAT_MSG_REQ) {
        return handle_auth(server, client_index, &header, body, body_len);
    }
    if (header.message_group == LAN_CHAT_GROUP_CHAT && header.message_type == LAN_CHAT_MSG_REQ) {
        return handle_chat(server, client_index, &header, body, body_len);
    }
    if (header.message_group == LAN_CHAT_GROUP_HEARTBEAT) {
        return queue_packet(
            &server->clients[client_index].connection,
            &header,
            LAN_CHAT_GROUP_HEARTBEAT,
            LAN_CHAT_MSG_RSP,
            LAN_CHAT_FLAG_RESPONSE,
            0,
            0,
            0,
            header.sender_id,
            server->clients[client_index].session_id);
    }
    return queue_status_response(&server->clients[client_index].connection, &header, LAN_CHAT_GROUP_ERROR, LAN_CHAT_STATUS_UNSUPPORTED_FIELD, 0, 0, 0, 0, "unsupported packet");
}

static void accept_clients(lan_chat_server_t *server)
{
    for (;;) {
        size_t index = find_free_client(server);
        lan_chat_status_t status;

        if (index >= server->client_capacity) {
            return;
        }
        memset(&server->clients[index], 0, sizeof(server->clients[index]));
        status = lan_chat_tcp_listener_accept(&server->listener, &server->clients[index].connection);
        if (status == LAN_CHAT_STATUS_NEED_MORE_DATA) {
            return;
        }
        if (status != LAN_CHAT_STATUS_OK) {
            memset(&server->clients[index], 0, sizeof(server->clients[index]));
            return;
        }
        server->clients[index].connection_id = index + 1;
    }
}

lan_chat_status_t lan_chat_server_init(
    lan_chat_server_t *server,
    const lan_chat_server_config_t *config)
{
    lan_chat_status_t status;

    if (server == 0 || config == 0 || config->storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    memset(server, 0, sizeof(*server));
    server->storage = config->storage;
    server->listen_port = config->listen_port;
    server->worker_thread_count = config->worker_thread_count;
    server->next_session_id = 1;
    server->client_capacity = LAN_CHAT_SERVER_MAX_CLIENTS;
    server->clients = (struct lan_chat_server_client *)calloc(server->client_capacity, sizeof(*server->clients));
    if (server->clients == 0) {
        memset(server, 0, sizeof(*server));
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }

    status = lan_chat_transport_init(&server->transport);
    if (status != LAN_CHAT_STATUS_OK) {
        free(server->clients);
        memset(server, 0, sizeof(*server));
        return status;
    }
    status = lan_chat_tcp_listener_open(
        &server->listener,
        config->listen_host != 0 && config->listen_host[0] != '\0' ? config->listen_host : "127.0.0.1",
        config->listen_port,
        LAN_CHAT_TRANSPORT_DEFAULT_BACKLOG);
    if (status != LAN_CHAT_STATUS_OK) {
        lan_chat_transport_shutdown(&server->transport);
        free(server->clients);
        memset(server, 0, sizeof(*server));
        return status;
    }
    server->listen_port = lan_chat_tcp_listener_port(&server->listener);
    server->running = 1;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_server_run_once(lan_chat_server_t *server)
{
    size_t i;
    uint8_t packet[LAN_CHAT_SERVER_PACKET_BUFFER_SIZE];

    if (server == 0 || !server->running) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    accept_clients(server);
    for (i = 0; i < server->client_capacity; ++i) {
        lan_chat_status_t status;
        size_t packet_len = 0;

        if (!server->clients[i].connection.is_open) {
            continue;
        }
        status = lan_chat_tcp_connection_flush(&server->clients[i].connection);
        if (status != LAN_CHAT_STATUS_OK && status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
            close_client(server, i);
            continue;
        }
        status = lan_chat_tcp_connection_recv_packet(&server->clients[i].connection, packet, sizeof(packet), &packet_len);
        if (status == LAN_CHAT_STATUS_NEED_MORE_DATA) {
            continue;
        }
        if (status != LAN_CHAT_STATUS_OK) {
            close_client(server, i);
            continue;
        }
        status = dispatch_packet(server, i, packet, packet_len);
        if (status != LAN_CHAT_STATUS_OK && status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
            close_client(server, i);
        }
    }
    return LAN_CHAT_STATUS_OK;
}

uint16_t lan_chat_server_port(const lan_chat_server_t *server)
{
    return server != 0 ? server->listen_port : 0;
}

void lan_chat_server_shutdown(lan_chat_server_t *server)
{
    size_t i;

    if (server == 0) {
        return;
    }
    for (i = 0; i < server->client_capacity; ++i) {
        close_client(server, i);
    }
    if (server->listener.is_open) {
        lan_chat_tcp_listener_close(&server->listener);
    }
    if (server->transport.initialized) {
        lan_chat_transport_shutdown(&server->transport);
    }
    free(server->clients);
    memset(server, 0, sizeof(*server));
}
