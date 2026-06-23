#include "lan_chat/core/ring_buffer.h"

#include <string.h>

static size_t write_pos(const lan_chat_ring_buffer_t *ring)
{
    return (ring->read_pos + ring->size) % ring->capacity;
}

lan_chat_status_t lan_chat_ring_buffer_init(lan_chat_ring_buffer_t *ring, uint8_t *buffer, size_t capacity)
{
    if (ring == 0 || buffer == 0 || capacity == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    ring->buffer = buffer;
    ring->capacity = capacity;
    ring->read_pos = 0;
    ring->size = 0;
    return LAN_CHAT_STATUS_OK;
}

size_t lan_chat_ring_buffer_size(const lan_chat_ring_buffer_t *ring)
{
    return ring != 0 ? ring->size : 0;
}

size_t lan_chat_ring_buffer_capacity(const lan_chat_ring_buffer_t *ring)
{
    return ring != 0 ? ring->capacity : 0;
}

size_t lan_chat_ring_buffer_available(const lan_chat_ring_buffer_t *ring)
{
    return ring != 0 ? ring->capacity - ring->size : 0;
}

lan_chat_status_t lan_chat_ring_buffer_write(lan_chat_ring_buffer_t *ring, const uint8_t *data, size_t data_len)
{
    size_t pos;
    size_t first;
    size_t second;

    if (ring == 0 || (data == 0 && data_len > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (data_len > lan_chat_ring_buffer_available(ring)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }
    if (data_len == 0) {
        return LAN_CHAT_STATUS_OK;
    }

    pos = write_pos(ring);
    first = ring->capacity - pos;
    if (first > data_len) {
        first = data_len;
    }
    second = data_len - first;
    memcpy(ring->buffer + pos, data, first);
    if (second > 0) {
        memcpy(ring->buffer, data + first, second);
    }
    ring->size += data_len;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_ring_buffer_peek(const lan_chat_ring_buffer_t *ring, size_t offset, uint8_t *out, size_t out_len)
{
    size_t pos;
    size_t first;
    size_t second;

    if (ring == 0 || (out == 0 && out_len > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (offset > ring->size || out_len > ring->size - offset) {
        return LAN_CHAT_STATUS_NEED_MORE_DATA;
    }
    if (out_len == 0) {
        return LAN_CHAT_STATUS_OK;
    }

    pos = (ring->read_pos + offset) % ring->capacity;
    first = ring->capacity - pos;
    if (first > out_len) {
        first = out_len;
    }
    second = out_len - first;
    memcpy(out, ring->buffer + pos, first);
    if (second > 0) {
        memcpy(out + first, ring->buffer, second);
    }
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_ring_buffer_discard(lan_chat_ring_buffer_t *ring, size_t len)
{
    if (ring == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (len > ring->size) {
        return LAN_CHAT_STATUS_NEED_MORE_DATA;
    }
    ring->read_pos = (ring->read_pos + len) % ring->capacity;
    ring->size -= len;
    if (ring->size == 0) {
        ring->read_pos = 0;
    }
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_ring_buffer_read(lan_chat_ring_buffer_t *ring, uint8_t *out, size_t out_len)
{
    lan_chat_status_t status = lan_chat_ring_buffer_peek(ring, 0, out, out_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return lan_chat_ring_buffer_discard(ring, out_len);
}
