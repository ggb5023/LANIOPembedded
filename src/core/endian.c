#include "lan_chat/core/endian.h"

static int can_access(size_t buffer_len, size_t offset, size_t value_size)
{
    return offset <= buffer_len && value_size <= (buffer_len - offset);
}

lan_chat_status_t lan_chat_write_u16_be(uint8_t *buffer, size_t buffer_len, size_t offset, uint16_t value)
{
    if (buffer == 0 || !can_access(buffer_len, offset, 2)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    buffer[offset] = (uint8_t)((value >> 8) & 0xFFu);
    buffer[offset + 1] = (uint8_t)(value & 0xFFu);
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_write_u32_be(uint8_t *buffer, size_t buffer_len, size_t offset, uint32_t value)
{
    if (buffer == 0 || !can_access(buffer_len, offset, 4)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    buffer[offset] = (uint8_t)((value >> 24) & 0xFFu);
    buffer[offset + 1] = (uint8_t)((value >> 16) & 0xFFu);
    buffer[offset + 2] = (uint8_t)((value >> 8) & 0xFFu);
    buffer[offset + 3] = (uint8_t)(value & 0xFFu);
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_write_u64_be(uint8_t *buffer, size_t buffer_len, size_t offset, uint64_t value)
{
    if (buffer == 0 || !can_access(buffer_len, offset, 8)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    buffer[offset] = (uint8_t)((value >> 56) & 0xFFu);
    buffer[offset + 1] = (uint8_t)((value >> 48) & 0xFFu);
    buffer[offset + 2] = (uint8_t)((value >> 40) & 0xFFu);
    buffer[offset + 3] = (uint8_t)((value >> 32) & 0xFFu);
    buffer[offset + 4] = (uint8_t)((value >> 24) & 0xFFu);
    buffer[offset + 5] = (uint8_t)((value >> 16) & 0xFFu);
    buffer[offset + 6] = (uint8_t)((value >> 8) & 0xFFu);
    buffer[offset + 7] = (uint8_t)(value & 0xFFu);
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_read_u16_be(const uint8_t *buffer, size_t buffer_len, size_t offset, uint16_t *out_value)
{
    if (buffer == 0 || out_value == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (!can_access(buffer_len, offset, 2)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    *out_value = (uint16_t)(((uint16_t)buffer[offset] << 8) | (uint16_t)buffer[offset + 1]);
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_read_u32_be(const uint8_t *buffer, size_t buffer_len, size_t offset, uint32_t *out_value)
{
    if (buffer == 0 || out_value == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (!can_access(buffer_len, offset, 4)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    *out_value = ((uint32_t)buffer[offset] << 24) |
                 ((uint32_t)buffer[offset + 1] << 16) |
                 ((uint32_t)buffer[offset + 2] << 8) |
                 (uint32_t)buffer[offset + 3];
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_read_u64_be(const uint8_t *buffer, size_t buffer_len, size_t offset, uint64_t *out_value)
{
    if (buffer == 0 || out_value == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (!can_access(buffer_len, offset, 8)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    *out_value = ((uint64_t)buffer[offset] << 56) |
                 ((uint64_t)buffer[offset + 1] << 48) |
                 ((uint64_t)buffer[offset + 2] << 40) |
                 ((uint64_t)buffer[offset + 3] << 32) |
                 ((uint64_t)buffer[offset + 4] << 24) |
                 ((uint64_t)buffer[offset + 5] << 16) |
                 ((uint64_t)buffer[offset + 6] << 8) |
                 (uint64_t)buffer[offset + 7];
    return LAN_CHAT_STATUS_OK;
}
