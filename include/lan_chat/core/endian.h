#ifndef LAN_CHAT_CORE_ENDIAN_H
#define LAN_CHAT_CORE_ENDIAN_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

lan_chat_status_t lan_chat_write_u16_be(uint8_t *buffer, size_t buffer_len, size_t offset, uint16_t value);
lan_chat_status_t lan_chat_write_u32_be(uint8_t *buffer, size_t buffer_len, size_t offset, uint32_t value);
lan_chat_status_t lan_chat_write_u64_be(uint8_t *buffer, size_t buffer_len, size_t offset, uint64_t value);

lan_chat_status_t lan_chat_read_u16_be(const uint8_t *buffer, size_t buffer_len, size_t offset, uint16_t *out_value);
lan_chat_status_t lan_chat_read_u32_be(const uint8_t *buffer, size_t buffer_len, size_t offset, uint32_t *out_value);
lan_chat_status_t lan_chat_read_u64_be(const uint8_t *buffer, size_t buffer_len, size_t offset, uint64_t *out_value);

#ifdef __cplusplus
}
#endif

#endif
