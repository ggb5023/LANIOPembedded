#ifndef LAN_CHAT_CORE_PROTOCOL_H
#define LAN_CHAT_CORE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/status.h"
#include "lan_chat/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lan_chat_header {
    uint16_t magic;
    uint8_t version;
    uint8_t header_len;
    uint16_t message_group;
    uint16_t message_type;
    uint16_t flags;
    uint16_t status;
    uint32_t body_len;
    uint32_t seq;
    uint64_t session_id;
    uint64_t sender_id;
    uint64_t receiver_id;
    uint64_t timestamp_ms;
    uint8_t reserved[LAN_CHAT_HEADER_RESERVED_LEN];
} lan_chat_header_t;

void lan_chat_header_init(
    lan_chat_header_t *header,
    uint16_t message_group,
    uint16_t message_type,
    uint16_t flags,
    uint32_t body_len);

lan_chat_status_t lan_chat_header_validate(const lan_chat_header_t *header);
lan_chat_status_t lan_chat_header_encode(const lan_chat_header_t *header, uint8_t *buffer, size_t buffer_len);
lan_chat_status_t lan_chat_header_decode(lan_chat_header_t *header, const uint8_t *buffer, size_t buffer_len);

lan_chat_status_t lan_chat_packet_pack(
    const lan_chat_header_t *header,
    const uint8_t *body,
    size_t body_len,
    uint8_t *buffer,
    size_t buffer_len,
    size_t *out_packet_len);

lan_chat_status_t lan_chat_packet_unpack(
    lan_chat_header_t *header,
    const uint8_t *buffer,
    size_t buffer_len,
    const uint8_t **out_body,
    size_t *out_body_len);

lan_chat_status_t lan_chat_packet_peek_size(const uint8_t *buffer, size_t buffer_len, size_t *out_packet_len);

#ifdef __cplusplus
}
#endif

#endif
