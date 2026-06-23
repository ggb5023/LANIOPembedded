#ifndef LAN_CHAT_CORE_FRAMER_H
#define LAN_CHAT_CORE_FRAMER_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/protocol.h"
#include "lan_chat/core/ring_buffer.h"
#include "lan_chat/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lan_chat_framer {
    lan_chat_ring_buffer_t ring;
} lan_chat_framer_t;

lan_chat_status_t lan_chat_framer_init(lan_chat_framer_t *framer, uint8_t *buffer, size_t capacity);
lan_chat_status_t lan_chat_framer_feed(lan_chat_framer_t *framer, const uint8_t *data, size_t data_len);
lan_chat_status_t lan_chat_framer_next(
    lan_chat_framer_t *framer,
    uint8_t *out_packet,
    size_t out_capacity,
    size_t *out_packet_len);

#ifdef __cplusplus
}
#endif

#endif
