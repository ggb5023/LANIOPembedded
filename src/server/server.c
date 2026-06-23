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
    LAN_CHAT_CHAT_NOTIFY_FIELD_DELIVERY_ID = 4,
    LAN_CHAT_CONV_FIELD_OPERATION = 1,
    LAN_CHAT_CONV_FIELD_NAME = 2,
    LAN_CHAT_CONV_FIELD_MEMBER_IDS = 3,
    LAN_CHAT_CONV_FIELD_CONVERSATION_ID = 4,
    LAN_CHAT_CONV_FIELD_CONTENT = 5,
    LAN_CHAT_CONV_FIELD_CLIENT_MESSAGE_ID = 6,
    LAN_CHAT_CONV_RESPONSE_FIELD_CONVERSATION_ID = 2,
    LAN_CHAT_CONV_RESPONSE_FIELD_MESSAGE_ID = 3,
    LAN_CHAT_CONV_NOTIFY_FIELD_CONVERSATION_ID = 1,
    LAN_CHAT_CONV_NOTIFY_FIELD_MESSAGE_ID = 2,
    LAN_CHAT_CONV_NOTIFY_FIELD_SENDER_ID = 3,
    LAN_CHAT_CONV_NOTIFY_FIELD_CONTENT = 4,
    LAN_CHAT_FILE_FIELD_OPERATION = 1,
    LAN_CHAT_FILE_FIELD_RECEIVER_ID = 2,
    LAN_CHAT_FILE_FIELD_FILE_NAME = 3,
    LAN_CHAT_FILE_FIELD_FILE_SIZE = 4,
    LAN_CHAT_FILE_FIELD_CRC32 = 5,
    LAN_CHAT_FILE_FIELD_TRANSFER_ID = 6,
    LAN_CHAT_FILE_FIELD_CHUNK_INDEX = 7,
    LAN_CHAT_FILE_FIELD_CHUNK_DATA = 8,
    LAN_CHAT_FILE_FIELD_MESSAGE_ID = 9,
    LAN_CHAT_FILE_FIELD_DELIVERY_ID = 10
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

typedef struct parsed_conversation_request {
    char operation[16];
    char name[LAN_CHAT_MAX_GROUP_NAME_LEN + 1];
    lan_chat_user_id_t member_ids[LAN_CHAT_MAX_GROUP_MEMBERS];
    size_t member_count;
    lan_chat_conversation_id_t conversation_id;
    char content[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 1];
    uint64_t client_message_id;
} parsed_conversation_request_t;

typedef struct parsed_file_request {
    char operation[16];
    lan_chat_user_id_t receiver_id;
    char file_name[LAN_CHAT_MAX_FILE_NAME_LEN + 1];
    uint64_t file_size;
    uint32_t crc32;
    uint64_t transfer_id;
    uint64_t chunk_index;
    const uint8_t *chunk_data;
    size_t chunk_len;
} parsed_file_request_t;

struct lan_chat_server_file_transfer {
    uint8_t active;
    uint64_t runtime_transfer_id;
    uint64_t storage_file_transfer_id;
    lan_chat_message_id_t message_id;
    lan_chat_delivery_id_t delivery_id;
    lan_chat_user_id_t sender_id;
    lan_chat_user_id_t receiver_id;
    char file_name[LAN_CHAT_MAX_FILE_NAME_LEN + 1];
    uint64_t file_size;
    uint64_t bytes_seen;
    uint64_t next_chunk_index;
    uint32_t crc32;
};

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

static lan_chat_status_t read_tlv_u32(const lan_chat_tlv_t *tlv, uint32_t *out_value)
{
    if (tlv == 0 || out_value == 0 || tlv->length != 4) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    return lan_chat_read_u32_be(tlv->value, tlv->length, 0, out_value);
}

static lan_chat_status_t parse_member_ids(
    const lan_chat_tlv_t *tlv,
    lan_chat_user_id_t *out_member_ids,
    size_t *out_member_count)
{
    size_t count;
    size_t i;
    lan_chat_status_t status;

    if (tlv == 0 || out_member_ids == 0 || out_member_count == 0 ||
        tlv->length == 0 || (tlv->length % 8) != 0) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    count = tlv->length / 8;
    if (count > LAN_CHAT_MAX_GROUP_MEMBERS) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }
    for (i = 0; i < count; ++i) {
        status = lan_chat_read_u64_be(tlv->value, tlv->length, i * 8, &out_member_ids[i]);
        if (status != LAN_CHAT_STATUS_OK) {
            return status;
        }
        if (out_member_ids[i] == 0) {
            return LAN_CHAT_STATUS_INVALID_ARGUMENT;
        }
    }
    *out_member_count = count;
    return LAN_CHAT_STATUS_OK;
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

static lan_chat_status_t parse_conversation_request(const uint8_t *body, size_t body_len, parsed_conversation_request_t *out)
{
    lan_chat_tlv_reader_t reader;
    lan_chat_tlv_t tlv;
    lan_chat_status_t status;
    int has_operation = 0;

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
        case LAN_CHAT_CONV_FIELD_OPERATION:
            copy_string(out->operation, sizeof(out->operation), (const char *)tlv.value, tlv.length);
            has_operation = 1;
            break;
        case LAN_CHAT_CONV_FIELD_NAME:
            copy_string(out->name, sizeof(out->name), (const char *)tlv.value, tlv.length);
            break;
        case LAN_CHAT_CONV_FIELD_MEMBER_IDS:
            status = parse_member_ids(&tlv, out->member_ids, &out->member_count);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            break;
        case LAN_CHAT_CONV_FIELD_CONVERSATION_ID:
            status = read_tlv_u64(&tlv, &out->conversation_id);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            break;
        case LAN_CHAT_CONV_FIELD_CONTENT:
            copy_string(out->content, sizeof(out->content), (const char *)tlv.value, tlv.length);
            break;
        case LAN_CHAT_CONV_FIELD_CLIENT_MESSAGE_ID:
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
    if (!has_operation || !string_is_valid(out->operation, sizeof(out->operation) - 1, 0)) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t parse_file_request(const uint8_t *body, size_t body_len, parsed_file_request_t *out)
{
    lan_chat_tlv_reader_t reader;
    lan_chat_tlv_t tlv;
    lan_chat_status_t status;
    int has_operation = 0;

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
        case LAN_CHAT_FILE_FIELD_OPERATION:
            copy_string(out->operation, sizeof(out->operation), (const char *)tlv.value, tlv.length);
            has_operation = 1;
            break;
        case LAN_CHAT_FILE_FIELD_RECEIVER_ID:
            status = read_tlv_u64(&tlv, &out->receiver_id);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            break;
        case LAN_CHAT_FILE_FIELD_FILE_NAME:
            copy_string(out->file_name, sizeof(out->file_name), (const char *)tlv.value, tlv.length);
            break;
        case LAN_CHAT_FILE_FIELD_FILE_SIZE:
            status = read_tlv_u64(&tlv, &out->file_size);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            break;
        case LAN_CHAT_FILE_FIELD_CRC32:
            status = read_tlv_u32(&tlv, &out->crc32);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            break;
        case LAN_CHAT_FILE_FIELD_TRANSFER_ID:
            status = read_tlv_u64(&tlv, &out->transfer_id);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            break;
        case LAN_CHAT_FILE_FIELD_CHUNK_INDEX:
            status = read_tlv_u64(&tlv, &out->chunk_index);
            if (status != LAN_CHAT_STATUS_OK) {
                return status;
            }
            break;
        case LAN_CHAT_FILE_FIELD_CHUNK_DATA:
            out->chunk_data = tlv.value;
            out->chunk_len = tlv.length;
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
    if (!has_operation || !string_is_valid(out->operation, sizeof(out->operation) - 1, 0)) {
        return LAN_CHAT_STATUS_BAD_HEADER;
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

static lan_chat_status_t queue_conversation_status_response(
    lan_chat_tcp_connection_t *connection,
    const lan_chat_header_t *request_header,
    lan_chat_status_t status_code,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_message_id_t message_id,
    const char *message)
{
    uint8_t body[512];
    lan_chat_tlv_writer_t writer;
    lan_chat_status_t status;

    status = lan_chat_tlv_writer_init(&writer, body, sizeof(body));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if ((status = lan_chat_tlv_write_u16(&writer, LAN_CHAT_RESPONSE_FIELD_STATUS, 1, (uint16_t)status_code)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (status_code == LAN_CHAT_STATUS_OK) {
        if ((status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CONV_RESPONSE_FIELD_CONVERSATION_ID, 0, conversation_id)) != LAN_CHAT_STATUS_OK ||
            (message_id != 0 && (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CONV_RESPONSE_FIELD_MESSAGE_ID, 0, message_id)) != LAN_CHAT_STATUS_OK)) {
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
        LAN_CHAT_GROUP_CONVERSATION,
        LAN_CHAT_MSG_RSP,
        LAN_CHAT_FLAG_RESPONSE,
        body,
        lan_chat_tlv_writer_size(&writer),
        0,
        request_header != 0 ? request_header->sender_id : 0,
        0);
}

static lan_chat_status_t queue_group_notify(
    lan_chat_tcp_connection_t *connection,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_message_id_t message_id,
    lan_chat_user_id_t sender_id,
    lan_chat_user_id_t receiver_id,
    lan_chat_session_id_t session_id,
    const char *content)
{
    uint8_t body[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 128];
    lan_chat_tlv_writer_t writer;
    lan_chat_status_t status;

    status = lan_chat_tlv_writer_init(&writer, body, sizeof(body));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if ((status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CONV_NOTIFY_FIELD_CONVERSATION_ID, 1, conversation_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CONV_NOTIFY_FIELD_MESSAGE_ID, 1, message_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_CONV_NOTIFY_FIELD_SENDER_ID, 1, sender_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_string(&writer, LAN_CHAT_CONV_NOTIFY_FIELD_CONTENT, 1, content, bounded_strlen(content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN))) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return queue_packet(
        connection,
        0,
        LAN_CHAT_GROUP_CONVERSATION,
        LAN_CHAT_MSG_NOTIFY,
        LAN_CHAT_FLAG_NOTIFY,
        body,
        lan_chat_tlv_writer_size(&writer),
        sender_id,
        receiver_id,
        session_id);
}

static lan_chat_status_t queue_file_status_response(
    lan_chat_tcp_connection_t *connection,
    const lan_chat_header_t *request_header,
    lan_chat_status_t status_code,
    uint64_t transfer_id,
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
    if ((status = lan_chat_tlv_write_u16(&writer, LAN_CHAT_RESPONSE_FIELD_STATUS, 1, (uint16_t)status_code)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (status_code == LAN_CHAT_STATUS_OK) {
        if ((status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_TRANSFER_ID, 0, transfer_id)) != LAN_CHAT_STATUS_OK ||
            (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_MESSAGE_ID, 0, message_id)) != LAN_CHAT_STATUS_OK ||
            (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_DELIVERY_ID, 0, delivery_id)) != LAN_CHAT_STATUS_OK) {
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
        LAN_CHAT_GROUP_FILE,
        LAN_CHAT_MSG_RSP,
        LAN_CHAT_FLAG_RESPONSE,
        body,
        lan_chat_tlv_writer_size(&writer),
        0,
        request_header != 0 ? request_header->sender_id : 0,
        0);
}

static lan_chat_status_t queue_file_start_notify(
    lan_chat_tcp_connection_t *connection,
    uint64_t transfer_id,
    lan_chat_message_id_t message_id,
    lan_chat_delivery_id_t delivery_id,
    lan_chat_user_id_t sender_id,
    lan_chat_user_id_t receiver_id,
    lan_chat_session_id_t session_id,
    const char *file_name,
    uint64_t file_size,
    uint32_t crc32)
{
    uint8_t body[512];
    lan_chat_tlv_writer_t writer;
    lan_chat_status_t status;

    status = lan_chat_tlv_writer_init(&writer, body, sizeof(body));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if ((status = lan_chat_tlv_write_string(&writer, LAN_CHAT_FILE_FIELD_OPERATION, 1, "start", 5)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_TRANSFER_ID, 1, transfer_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_MESSAGE_ID, 0, message_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_DELIVERY_ID, 0, delivery_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_string(&writer, LAN_CHAT_FILE_FIELD_FILE_NAME, 1, file_name, bounded_strlen(file_name, LAN_CHAT_MAX_FILE_NAME_LEN))) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_FILE_SIZE, 1, file_size)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u32(&writer, LAN_CHAT_FILE_FIELD_CRC32, 1, crc32)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return queue_packet(
        connection,
        0,
        LAN_CHAT_GROUP_FILE,
        LAN_CHAT_MSG_NOTIFY,
        LAN_CHAT_FLAG_NOTIFY,
        body,
        lan_chat_tlv_writer_size(&writer),
        sender_id,
        receiver_id,
        session_id);
}

static lan_chat_status_t queue_file_chunk_notify(
    lan_chat_tcp_connection_t *connection,
    uint64_t transfer_id,
    uint64_t chunk_index,
    lan_chat_user_id_t sender_id,
    lan_chat_user_id_t receiver_id,
    lan_chat_session_id_t session_id,
    const uint8_t *chunk_data,
    size_t chunk_len)
{
    uint8_t body[LAN_CHAT_FILE_CHUNK_SIZE + 128];
    lan_chat_tlv_writer_t writer;
    lan_chat_status_t status;

    status = lan_chat_tlv_writer_init(&writer, body, sizeof(body));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if ((status = lan_chat_tlv_write_string(&writer, LAN_CHAT_FILE_FIELD_OPERATION, 1, "chunk", 5)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_TRANSFER_ID, 1, transfer_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_CHUNK_INDEX, 1, chunk_index)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_bytes(&writer, LAN_CHAT_FILE_FIELD_CHUNK_DATA, 1, chunk_data, chunk_len)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return queue_packet(
        connection,
        0,
        LAN_CHAT_GROUP_FILE,
        LAN_CHAT_MSG_NOTIFY,
        LAN_CHAT_FLAG_NOTIFY,
        body,
        lan_chat_tlv_writer_size(&writer),
        sender_id,
        receiver_id,
        session_id);
}

static lan_chat_status_t queue_file_complete_notify(
    lan_chat_tcp_connection_t *connection,
    uint64_t transfer_id,
    lan_chat_user_id_t sender_id,
    lan_chat_user_id_t receiver_id,
    lan_chat_session_id_t session_id)
{
    uint8_t body[128];
    lan_chat_tlv_writer_t writer;
    lan_chat_status_t status;

    status = lan_chat_tlv_writer_init(&writer, body, sizeof(body));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if ((status = lan_chat_tlv_write_string(&writer, LAN_CHAT_FILE_FIELD_OPERATION, 1, "complete", 8)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_tlv_write_u64(&writer, LAN_CHAT_FILE_FIELD_TRANSFER_ID, 1, transfer_id)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return queue_packet(
        connection,
        0,
        LAN_CHAT_GROUP_FILE,
        LAN_CHAT_MSG_NOTIFY,
        LAN_CHAT_FLAG_NOTIFY,
        body,
        lan_chat_tlv_writer_size(&writer),
        sender_id,
        receiver_id,
        session_id);
}

static void clear_file_transfers_for_user(lan_chat_server_t *server, lan_chat_user_id_t user_id)
{
    size_t i;

    if (server == 0 || server->file_transfers == 0 || user_id == 0) {
        return;
    }
    for (i = 0; i < server->file_transfer_capacity; ++i) {
        if (server->file_transfers[i].active &&
            (server->file_transfers[i].sender_id == user_id ||
             server->file_transfers[i].receiver_id == user_id)) {
            memset(&server->file_transfers[i], 0, sizeof(server->file_transfers[i]));
        }
    }
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

static struct lan_chat_server_file_transfer *find_free_file_transfer(lan_chat_server_t *server)
{
    size_t i;

    if (server == 0 || server->file_transfers == 0) {
        return 0;
    }
    for (i = 0; i < server->file_transfer_capacity; ++i) {
        if (!server->file_transfers[i].active) {
            return &server->file_transfers[i];
        }
    }
    return 0;
}

static struct lan_chat_server_file_transfer *find_file_transfer(lan_chat_server_t *server, uint64_t runtime_transfer_id)
{
    size_t i;

    if (server == 0 || server->file_transfers == 0 || runtime_transfer_id == 0) {
        return 0;
    }
    for (i = 0; i < server->file_transfer_capacity; ++i) {
        if (server->file_transfers[i].active &&
            server->file_transfers[i].runtime_transfer_id == runtime_transfer_id) {
            return &server->file_transfers[i];
        }
    }
    return 0;
}

static int server_user_is_online(lan_chat_server_t *server, lan_chat_user_id_t user_id)
{
    if (server == 0 || user_id == 0) {
        return 0;
    }
    return find_client_by_user_id(server, user_id) < server->client_capacity;
}

static void clear_file_transfers_with_offline_peer(lan_chat_server_t *server)
{
    size_t i;

    if (server == 0 || server->file_transfers == 0) {
        return;
    }
    for (i = 0; i < server->file_transfer_capacity; ++i) {
        if (server->file_transfers[i].active &&
            (!server_user_is_online(server, server->file_transfers[i].sender_id) ||
             !server_user_is_online(server, server->file_transfers[i].receiver_id))) {
            memset(&server->file_transfers[i], 0, sizeof(server->file_transfers[i]));
        }
    }
}

static void close_client(lan_chat_server_t *server, size_t index)
{
    lan_chat_user_id_t user_id = 0;

    if (server == 0 || server->clients == 0 || index >= server->client_capacity) {
        return;
    }
    if (server->clients[index].authenticated) {
        user_id = server->clients[index].user_id;
    }
    if (server->clients[index].connection.is_open) {
        lan_chat_tcp_connection_close(&server->clients[index].connection);
    }
    clear_file_transfers_for_user(server, user_id);
    memset(&server->clients[index], 0, sizeof(server->clients[index]));
}

static void authenticate_client(
    lan_chat_server_t *server,
    size_t client_index,
    lan_chat_user_id_t user_id,
    const char *username)
{
    size_t i;

    for (i = 0; i < server->client_capacity; ++i) {
        if (i != client_index &&
            server->clients[i].connection.is_open &&
            server->clients[i].authenticated &&
            server->clients[i].user_id == user_id) {
            close_client(server, i);
        }
    }
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

static lan_chat_status_t handle_conversation(
    lan_chat_server_t *server,
    size_t client_index,
    const lan_chat_header_t *header,
    const uint8_t *body,
    size_t body_len)
{
    parsed_conversation_request_t request;
    lan_chat_status_t status;
    lan_chat_conversation_id_t conversation_id = 0;
    lan_chat_message_id_t message_id = 0;
    lan_chat_group_create_t group;
    lan_chat_message_record_t message;
    lan_chat_group_member_record_t members[LAN_CHAT_MAX_GROUP_MEMBERS];
    size_t member_count = 0;
    size_t i;

    if (!server->clients[client_index].authenticated) {
        return queue_conversation_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_STATE, 0, 0, "login required");
    }
    status = parse_conversation_request(body, body_len, &request);
    if (status != LAN_CHAT_STATUS_OK) {
        return queue_conversation_status_response(&server->clients[client_index].connection, header, status, 0, 0, "bad conversation request");
    }

    if (strcmp(request.operation, "create") == 0) {
        memset(&group, 0, sizeof(group));
        group.name = request.name;
        group.creator_id = server->clients[client_index].user_id;
        group.member_ids = request.member_ids;
        group.member_count = request.member_count;
        status = lan_chat_storage_create_group(server->storage, &group, &conversation_id);
        return queue_conversation_status_response(
            &server->clients[client_index].connection,
            header,
            status,
            conversation_id,
            0,
            status == LAN_CHAT_STATUS_OK ? "group created" : "group create failed");
    }

    if (strcmp(request.operation, "send") != 0) {
        return queue_conversation_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_ARGUMENT, 0, 0, "unknown conversation operation");
    }
    if (request.conversation_id == 0 || !string_is_valid(request.content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN, 0)) {
        return queue_conversation_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_ARGUMENT, 0, 0, "bad group message");
    }

    memset(&message, 0, sizeof(message));
    message.conversation_id = request.conversation_id;
    message.sender_id = server->clients[client_index].user_id;
    message.content_type = LAN_CHAT_CONTENT_TEXT;
    message.client_message_id = request.client_message_id;
    copy_string(message.content, sizeof(message.content), request.content, bounded_strlen(request.content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN));
    status = lan_chat_storage_store_group_message(server->storage, &message, &message_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return queue_conversation_status_response(&server->clients[client_index].connection, header, status, 0, 0, "store group message failed");
    }

    status = queue_conversation_status_response(
        &server->clients[client_index].connection,
        header,
        LAN_CHAT_STATUS_OK,
        request.conversation_id,
        message_id,
        "group sent");
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    status = lan_chat_storage_list_group_members(server->storage, request.conversation_id, members, LAN_CHAT_MAX_GROUP_MEMBERS, &member_count);
    if (status != LAN_CHAT_STATUS_OK) {
        return LAN_CHAT_STATUS_OK;
    }
    for (i = 0; i < member_count; ++i) {
        size_t receiver_index;
        if (members[i].user_id == server->clients[client_index].user_id) {
            continue;
        }
        receiver_index = find_client_by_user_id(server, members[i].user_id);
        if (receiver_index < server->client_capacity) {
            (void)queue_group_notify(
                &server->clients[receiver_index].connection,
                request.conversation_id,
                message_id,
                server->clients[client_index].user_id,
                members[i].user_id,
                server->clients[receiver_index].session_id,
                request.content);
        }
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t handle_file(
    lan_chat_server_t *server,
    size_t client_index,
    const lan_chat_header_t *header,
    const uint8_t *body,
    size_t body_len)
{
    parsed_file_request_t request;
    lan_chat_status_t status;
    struct lan_chat_server_file_transfer *transfer;
    size_t receiver_index;

    if (!server->clients[client_index].authenticated) {
        return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_STATE, 0, 0, 0, "login required");
    }
    status = parse_file_request(body, body_len, &request);
    if (status != LAN_CHAT_STATUS_OK) {
        return queue_file_status_response(&server->clients[client_index].connection, header, status, 0, 0, 0, "bad file request");
    }

    if (strcmp(request.operation, "start") == 0) {
        lan_chat_message_record_t message;
        lan_chat_message_id_t message_id = 0;
        lan_chat_delivery_id_t delivery_id = 0;
        uint64_t storage_file_transfer_id = 0;

        if (request.receiver_id == 0 || request.receiver_id == server->clients[client_index].user_id ||
            request.file_size == 0 || request.file_size > LAN_CHAT_MAX_FILE_SIZE ||
            !string_is_valid(request.file_name, LAN_CHAT_MAX_FILE_NAME_LEN, 0)) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_ARGUMENT, 0, 0, 0, "bad file start");
        }
        receiver_index = find_client_by_user_id(server, request.receiver_id);
        if (receiver_index >= server->client_capacity) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_NOT_FOUND, 0, 0, 0, "receiver offline");
        }
        transfer = find_free_file_transfer(server);
        if (transfer == 0) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_BUFFER_TOO_SMALL, 0, 0, 0, "too many file transfers");
        }

        memset(&message, 0, sizeof(message));
        message.sender_id = server->clients[client_index].user_id;
        message.receiver_id = request.receiver_id;
        message.content_type = LAN_CHAT_CONTENT_FILE;
        (void)snprintf(message.content, sizeof(message.content), "file:%s:%llu:%u", request.file_name, (unsigned long long)request.file_size, (unsigned)request.crc32);
        status = lan_chat_storage_store_file_transfer(
            server->storage,
            &message,
            request.file_name,
            request.file_size,
            request.crc32,
            &message_id,
            &delivery_id,
            &storage_file_transfer_id);
        if (status != LAN_CHAT_STATUS_OK) {
            return queue_file_status_response(&server->clients[client_index].connection, header, status, 0, 0, 0, "store file transfer failed");
        }

        memset(transfer, 0, sizeof(*transfer));
        transfer->active = 1;
        transfer->runtime_transfer_id = server->next_runtime_file_transfer_id++;
        transfer->storage_file_transfer_id = storage_file_transfer_id;
        transfer->message_id = message_id;
        transfer->delivery_id = delivery_id;
        transfer->sender_id = server->clients[client_index].user_id;
        transfer->receiver_id = request.receiver_id;
        transfer->file_size = request.file_size;
        transfer->crc32 = request.crc32;
        copy_string(transfer->file_name, sizeof(transfer->file_name), request.file_name, bounded_strlen(request.file_name, LAN_CHAT_MAX_FILE_NAME_LEN));

        status = queue_file_start_notify(
            &server->clients[receiver_index].connection,
            transfer->runtime_transfer_id,
            transfer->message_id,
            transfer->delivery_id,
            transfer->sender_id,
            transfer->receiver_id,
            server->clients[receiver_index].session_id,
            transfer->file_name,
            transfer->file_size,
            transfer->crc32);
        if (status != LAN_CHAT_STATUS_OK) {
            memset(transfer, 0, sizeof(*transfer));
            return status;
        }
        return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_OK, transfer->runtime_transfer_id, message_id, delivery_id, "file started");
    }

    if (strcmp(request.operation, "chunk") == 0) {
        if (request.transfer_id == 0 || request.chunk_data == 0 || request.chunk_len == 0 || request.chunk_len > LAN_CHAT_FILE_CHUNK_SIZE) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_ARGUMENT, request.transfer_id, 0, 0, "bad file chunk");
        }
        transfer = find_file_transfer(server, request.transfer_id);
        if (transfer == 0 || transfer->sender_id != server->clients[client_index].user_id) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_NOT_FOUND, request.transfer_id, 0, 0, "file transfer not found");
        }
        if (transfer->bytes_seen + request.chunk_len > transfer->file_size) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_ARGUMENT, request.transfer_id, 0, 0, "file chunk exceeds size");
        }
        if (request.chunk_index != transfer->next_chunk_index) {
            memset(transfer, 0, sizeof(*transfer));
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_ARGUMENT, request.transfer_id, 0, 0, "file chunk out of order");
        }
        receiver_index = find_client_by_user_id(server, transfer->receiver_id);
        if (receiver_index >= server->client_capacity) {
            memset(transfer, 0, sizeof(*transfer));
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_NOT_FOUND, request.transfer_id, 0, 0, "receiver offline");
        }
        status = queue_file_chunk_notify(
            &server->clients[receiver_index].connection,
            transfer->runtime_transfer_id,
            request.chunk_index,
            transfer->sender_id,
            transfer->receiver_id,
            server->clients[receiver_index].session_id,
            request.chunk_data,
            request.chunk_len);
        if (status != LAN_CHAT_STATUS_OK) {
            return status;
        }
        transfer->bytes_seen += request.chunk_len;
        ++transfer->next_chunk_index;
        return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_OK, transfer->runtime_transfer_id, transfer->message_id, transfer->delivery_id, "file chunk relayed");
    }

    if (strcmp(request.operation, "complete") == 0) {
        if (request.transfer_id == 0) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_ARGUMENT, 0, 0, 0, "bad file complete");
        }
        transfer = find_file_transfer(server, request.transfer_id);
        if (transfer == 0 || transfer->sender_id != server->clients[client_index].user_id) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_NOT_FOUND, request.transfer_id, 0, 0, "file transfer not found");
        }
        if (transfer->bytes_seen != transfer->file_size) {
            return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_STATE, request.transfer_id, 0, 0, "file incomplete");
        }
        receiver_index = find_client_by_user_id(server, transfer->receiver_id);
        if (receiver_index < server->client_capacity) {
            (void)queue_file_complete_notify(
                &server->clients[receiver_index].connection,
                transfer->runtime_transfer_id,
                transfer->sender_id,
                transfer->receiver_id,
                server->clients[receiver_index].session_id);
        }
        (void)lan_chat_storage_complete_file_transfer(server->storage, transfer->storage_file_transfer_id);
        status = queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_OK, transfer->runtime_transfer_id, transfer->message_id, transfer->delivery_id, "file completed");
        memset(transfer, 0, sizeof(*transfer));
        return status;
    }

    return queue_file_status_response(&server->clients[client_index].connection, header, LAN_CHAT_STATUS_INVALID_ARGUMENT, 0, 0, 0, "unknown file operation");
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
    if (header.message_group == LAN_CHAT_GROUP_CONVERSATION && header.message_type == LAN_CHAT_MSG_REQ) {
        return handle_conversation(server, client_index, &header, body, body_len);
    }
    if (header.message_group == LAN_CHAT_GROUP_FILE && header.message_type == LAN_CHAT_MSG_REQ) {
        return handle_file(server, client_index, &header, body, body_len);
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
    server->next_runtime_file_transfer_id = 1;
    server->client_capacity = LAN_CHAT_SERVER_MAX_CLIENTS;
    server->clients = (struct lan_chat_server_client *)calloc(server->client_capacity, sizeof(*server->clients));
    if (server->clients == 0) {
        memset(server, 0, sizeof(*server));
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }
    server->file_transfer_capacity = LAN_CHAT_SERVER_MAX_CLIENTS;
    server->file_transfers = (struct lan_chat_server_file_transfer *)calloc(
        server->file_transfer_capacity,
        sizeof(*server->file_transfers));
    if (server->file_transfers == 0) {
        free(server->clients);
        memset(server, 0, sizeof(*server));
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }

    status = lan_chat_transport_init(&server->transport);
    if (status != LAN_CHAT_STATUS_OK) {
        free(server->file_transfers);
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
        free(server->file_transfers);
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
    clear_file_transfers_with_offline_peer(server);
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
    free(server->file_transfers);
    free(server->clients);
    memset(server, 0, sizeof(*server));
}
