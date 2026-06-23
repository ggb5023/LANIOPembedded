#ifndef LAN_CHAT_CORE_TLV_H
#define LAN_CHAT_CORE_TLV_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LAN_CHAT_TLV_REQUIRED_MASK = 0x8000,
    LAN_CHAT_TLV_FIELD_MASK = 0x7FFF,
    LAN_CHAT_TLV_HEADER_LEN = 4,
    LAN_CHAT_TLV_MAX_TRACKED_FIELDS = 128
};

typedef struct lan_chat_tlv_writer {
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
} lan_chat_tlv_writer_t;

typedef struct lan_chat_tlv {
    uint16_t raw_type;
    uint16_t field_id;
    uint8_t required;
    const uint8_t *value;
    size_t length;
} lan_chat_tlv_t;

typedef struct lan_chat_tlv_reader {
    const uint8_t *buffer;
    size_t length;
    size_t offset;
    uint16_t seen_fields[LAN_CHAT_TLV_MAX_TRACKED_FIELDS];
    size_t seen_count;
} lan_chat_tlv_reader_t;

uint16_t lan_chat_tlv_type(uint16_t field_id, int required);

lan_chat_status_t lan_chat_tlv_writer_init(lan_chat_tlv_writer_t *writer, uint8_t *buffer, size_t capacity);
size_t lan_chat_tlv_writer_size(const lan_chat_tlv_writer_t *writer);
lan_chat_status_t lan_chat_tlv_write_u16(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, uint16_t value);
lan_chat_status_t lan_chat_tlv_write_u32(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, uint32_t value);
lan_chat_status_t lan_chat_tlv_write_u64(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, uint64_t value);
lan_chat_status_t lan_chat_tlv_write_string(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, const char *value, size_t value_len);
lan_chat_status_t lan_chat_tlv_write_bytes(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, const uint8_t *value, size_t value_len);

lan_chat_status_t lan_chat_tlv_reader_init(lan_chat_tlv_reader_t *reader, const uint8_t *buffer, size_t length);
lan_chat_status_t lan_chat_tlv_next(lan_chat_tlv_reader_t *reader, lan_chat_tlv_t *out_tlv);

#ifdef __cplusplus
}
#endif

#endif
