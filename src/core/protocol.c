#include "lan_chat/core/protocol.h"

#include <string.h>

#include "lan_chat/core/endian.h"

static int flags_are_valid(uint16_t flags)
{
    const uint16_t flow_flags = (uint16_t)(LAN_CHAT_FLAG_REQUEST | LAN_CHAT_FLAG_RESPONSE | LAN_CHAT_FLAG_NOTIFY);
    uint16_t flow_count = 0;

    if ((flags & (uint16_t)(~LAN_CHAT_FLAG_KNOWN_MASK)) != 0) {
        return 0;
    }

    if ((flags & LAN_CHAT_FLAG_REQUEST) != 0) {
        ++flow_count;
    }
    if ((flags & LAN_CHAT_FLAG_RESPONSE) != 0) {
        ++flow_count;
    }
    if ((flags & LAN_CHAT_FLAG_NOTIFY) != 0) {
        ++flow_count;
    }

    (void)flow_flags;
    return flow_count <= 1;
}

void lan_chat_header_init(
    lan_chat_header_t *header,
    uint16_t message_group,
    uint16_t message_type,
    uint16_t flags,
    uint32_t body_len)
{
    if (header == 0) {
        return;
    }

    memset(header, 0, sizeof(*header));
    header->magic = LAN_CHAT_MAGIC;
    header->version = LAN_CHAT_PROTOCOL_VERSION;
    header->header_len = LAN_CHAT_HEADER_LEN;
    header->message_group = message_group;
    header->message_type = message_type;
    header->flags = flags;
    header->body_len = body_len;
}

lan_chat_status_t lan_chat_header_validate(const lan_chat_header_t *header)
{
    size_t i;

    if (header == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (header->magic != LAN_CHAT_MAGIC) {
        return LAN_CHAT_STATUS_BAD_MAGIC;
    }
    if (header->version != LAN_CHAT_PROTOCOL_VERSION) {
        return LAN_CHAT_STATUS_UNSUPPORTED_VERSION;
    }
    if (header->header_len != LAN_CHAT_HEADER_LEN) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    if (header->body_len > LAN_CHAT_MAX_BODY_LEN) {
        return LAN_CHAT_STATUS_BODY_TOO_LARGE;
    }
    if (!flags_are_valid(header->flags)) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    for (i = 0; i < LAN_CHAT_HEADER_RESERVED_LEN; ++i) {
        if (header->reserved[i] != 0) {
            return LAN_CHAT_STATUS_BAD_HEADER;
        }
    }

    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_header_encode(const lan_chat_header_t *header, uint8_t *buffer, size_t buffer_len)
{
    lan_chat_status_t status;

    status = lan_chat_header_validate(header);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (buffer == 0 || buffer_len < LAN_CHAT_HEADER_LEN) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    status = lan_chat_write_u16_be(buffer, buffer_len, 0, header->magic);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    buffer[2] = header->version;
    buffer[3] = header->header_len;
    if ((status = lan_chat_write_u16_be(buffer, buffer_len, 4, header->message_group)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u16_be(buffer, buffer_len, 6, header->message_type)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u16_be(buffer, buffer_len, 8, header->flags)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u16_be(buffer, buffer_len, 10, header->status)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u32_be(buffer, buffer_len, 12, header->body_len)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u32_be(buffer, buffer_len, 16, header->seq)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u64_be(buffer, buffer_len, 20, header->session_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u64_be(buffer, buffer_len, 28, header->sender_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u64_be(buffer, buffer_len, 36, header->receiver_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_write_u64_be(buffer, buffer_len, 44, header->timestamp_ms)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    memcpy(buffer + 52, header->reserved, LAN_CHAT_HEADER_RESERVED_LEN);
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_header_decode(lan_chat_header_t *header, const uint8_t *buffer, size_t buffer_len)
{
    lan_chat_status_t status;

    if (header == 0 || buffer == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (buffer_len < LAN_CHAT_HEADER_LEN) {
        return LAN_CHAT_STATUS_NEED_MORE_DATA;
    }

    memset(header, 0, sizeof(*header));
    if ((status = lan_chat_read_u16_be(buffer, buffer_len, 0, &header->magic)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    header->version = buffer[2];
    header->header_len = buffer[3];
    if ((status = lan_chat_read_u16_be(buffer, buffer_len, 4, &header->message_group)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u16_be(buffer, buffer_len, 6, &header->message_type)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u16_be(buffer, buffer_len, 8, &header->flags)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u16_be(buffer, buffer_len, 10, &header->status)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u32_be(buffer, buffer_len, 12, &header->body_len)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u32_be(buffer, buffer_len, 16, &header->seq)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u64_be(buffer, buffer_len, 20, &header->session_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u64_be(buffer, buffer_len, 28, &header->sender_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u64_be(buffer, buffer_len, 36, &header->receiver_id)) != LAN_CHAT_STATUS_OK ||
        (status = lan_chat_read_u64_be(buffer, buffer_len, 44, &header->timestamp_ms)) != LAN_CHAT_STATUS_OK) {
        return status;
    }
    memcpy(header->reserved, buffer + 52, LAN_CHAT_HEADER_RESERVED_LEN);
    return lan_chat_header_validate(header);
}

lan_chat_status_t lan_chat_packet_pack(
    const lan_chat_header_t *header,
    const uint8_t *body,
    size_t body_len,
    uint8_t *buffer,
    size_t buffer_len,
    size_t *out_packet_len)
{
    lan_chat_status_t status;
    size_t packet_len;

    if (out_packet_len != 0) {
        *out_packet_len = 0;
    }
    if (header == 0 || buffer == 0 || (body == 0 && body_len > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (header->body_len != body_len) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    status = lan_chat_header_validate(header);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    packet_len = (size_t)LAN_CHAT_HEADER_LEN + body_len;
    if (packet_len < body_len || buffer_len < packet_len) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }
    status = lan_chat_header_encode(header, buffer, buffer_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (body_len > 0) {
        memcpy(buffer + LAN_CHAT_HEADER_LEN, body, body_len);
    }
    if (out_packet_len != 0) {
        *out_packet_len = packet_len;
    }
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_packet_peek_size(const uint8_t *buffer, size_t buffer_len, size_t *out_packet_len)
{
    lan_chat_header_t header;
    lan_chat_status_t status;
    size_t packet_len;

    if (out_packet_len == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_packet_len = 0;
    status = lan_chat_header_decode(&header, buffer, buffer_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    packet_len = (size_t)header.header_len + (size_t)header.body_len;
    if (packet_len < header.body_len) {
        return LAN_CHAT_STATUS_BODY_TOO_LARGE;
    }
    *out_packet_len = packet_len;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_packet_unpack(
    lan_chat_header_t *header,
    const uint8_t *buffer,
    size_t buffer_len,
    const uint8_t **out_body,
    size_t *out_body_len)
{
    lan_chat_status_t status;
    size_t packet_len = 0;

    if (out_body != 0) {
        *out_body = 0;
    }
    if (out_body_len != 0) {
        *out_body_len = 0;
    }
    if (header == 0 || buffer == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    status = lan_chat_packet_peek_size(buffer, buffer_len, &packet_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (buffer_len < packet_len) {
        return LAN_CHAT_STATUS_NEED_MORE_DATA;
    }
    status = lan_chat_header_decode(header, buffer, LAN_CHAT_HEADER_LEN);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (out_body != 0) {
        *out_body = buffer + LAN_CHAT_HEADER_LEN;
    }
    if (out_body_len != 0) {
        *out_body_len = header->body_len;
    }
    return LAN_CHAT_STATUS_OK;
}
