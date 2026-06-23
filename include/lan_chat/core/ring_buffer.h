#ifndef LAN_CHAT_CORE_RING_BUFFER_H
#define LAN_CHAT_CORE_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lan_chat_ring_buffer {
    uint8_t *buffer;
    size_t capacity;
    size_t read_pos;
    size_t size;
} lan_chat_ring_buffer_t;

lan_chat_status_t lan_chat_ring_buffer_init(lan_chat_ring_buffer_t *ring, uint8_t *buffer, size_t capacity);
size_t lan_chat_ring_buffer_size(const lan_chat_ring_buffer_t *ring);
size_t lan_chat_ring_buffer_capacity(const lan_chat_ring_buffer_t *ring);
size_t lan_chat_ring_buffer_available(const lan_chat_ring_buffer_t *ring);
lan_chat_status_t lan_chat_ring_buffer_write(lan_chat_ring_buffer_t *ring, const uint8_t *data, size_t data_len);
lan_chat_status_t lan_chat_ring_buffer_peek(const lan_chat_ring_buffer_t *ring, size_t offset, uint8_t *out, size_t out_len);
lan_chat_status_t lan_chat_ring_buffer_read(lan_chat_ring_buffer_t *ring, uint8_t *out, size_t out_len);
lan_chat_status_t lan_chat_ring_buffer_discard(lan_chat_ring_buffer_t *ring, size_t len);

#ifdef __cplusplus
}
#endif

#endif
