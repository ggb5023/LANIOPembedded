#include "lan_chat/core/framer.h"

lan_chat_status_t lan_chat_framer_init(lan_chat_framer_t *framer, uint8_t *buffer, size_t capacity)
{
    if (framer == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    return lan_chat_ring_buffer_init(&framer->ring, buffer, capacity);
}

lan_chat_status_t lan_chat_framer_feed(lan_chat_framer_t *framer, const uint8_t *data, size_t data_len)
{
    if (framer == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    return lan_chat_ring_buffer_write(&framer->ring, data, data_len);
}

lan_chat_status_t lan_chat_framer_next(
    lan_chat_framer_t *framer,
    uint8_t *out_packet,
    size_t out_capacity,
    size_t *out_packet_len)
{
    uint8_t header_bytes[LAN_CHAT_HEADER_LEN];
    lan_chat_header_t header;
    lan_chat_status_t status;
    size_t packet_len;

    if (out_packet_len != 0) {
        *out_packet_len = 0;
    }
    if (framer == 0 || out_packet == 0 || out_packet_len == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (lan_chat_ring_buffer_size(&framer->ring) < LAN_CHAT_HEADER_LEN) {
        return LAN_CHAT_STATUS_NEED_MORE_DATA;
    }

    status = lan_chat_ring_buffer_peek(&framer->ring, 0, header_bytes, sizeof(header_bytes));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = lan_chat_header_decode(&header, header_bytes, sizeof(header_bytes));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    packet_len = (size_t)header.header_len + (size_t)header.body_len;
    if (packet_len < header.body_len) {
        return LAN_CHAT_STATUS_BODY_TOO_LARGE;
    }
    if (lan_chat_ring_buffer_size(&framer->ring) < packet_len) {
        return LAN_CHAT_STATUS_NEED_MORE_DATA;
    }
    if (out_capacity < packet_len) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    status = lan_chat_ring_buffer_read(&framer->ring, out_packet, packet_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    *out_packet_len = packet_len;
    return LAN_CHAT_STATUS_OK;
}
