#include "lan_chat/core/tlv.h"

#include <string.h>

#include "lan_chat/core/endian.h"

static int field_id_is_seen(const lan_chat_tlv_reader_t *reader, uint16_t field_id)
{
    size_t i;
    for (i = 0; i < reader->seen_count; ++i) {
        if (reader->seen_fields[i] == field_id) {
            return 1;
        }
    }
    return 0;
}

static lan_chat_status_t remember_field(lan_chat_tlv_reader_t *reader, uint16_t field_id)
{
    if (field_id_is_seen(reader, field_id)) {
        return LAN_CHAT_STATUS_DUPLICATE_FIELD;
    }
    if (reader->seen_count >= LAN_CHAT_TLV_MAX_TRACKED_FIELDS) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }
    reader->seen_fields[reader->seen_count++] = field_id;
    return LAN_CHAT_STATUS_OK;
}

uint16_t lan_chat_tlv_type(uint16_t field_id, int required)
{
    uint16_t raw = (uint16_t)(field_id & LAN_CHAT_TLV_FIELD_MASK);
    if (required) {
        raw = (uint16_t)(raw | LAN_CHAT_TLV_REQUIRED_MASK);
    }
    return raw;
}

lan_chat_status_t lan_chat_tlv_writer_init(lan_chat_tlv_writer_t *writer, uint8_t *buffer, size_t capacity)
{
    if (writer == 0 || (buffer == 0 && capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->offset = 0;
    return LAN_CHAT_STATUS_OK;
}

size_t lan_chat_tlv_writer_size(const lan_chat_tlv_writer_t *writer)
{
    return writer != 0 ? writer->offset : 0;
}

lan_chat_status_t lan_chat_tlv_write_bytes(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, const uint8_t *value, size_t value_len)
{
    uint16_t raw_type;
    lan_chat_status_t status;

    if (writer == 0 || field_id == 0 || field_id > LAN_CHAT_TLV_FIELD_MASK || (value == 0 && value_len > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (value_len > UINT16_MAX) {
        return LAN_CHAT_STATUS_BODY_TOO_LARGE;
    }
    if (writer->offset > writer->capacity ||
        LAN_CHAT_TLV_HEADER_LEN > (writer->capacity - writer->offset) ||
        value_len > (writer->capacity - writer->offset - LAN_CHAT_TLV_HEADER_LEN)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    raw_type = lan_chat_tlv_type(field_id, required);
    status = lan_chat_write_u16_be(writer->buffer, writer->capacity, writer->offset, raw_type);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = lan_chat_write_u16_be(writer->buffer, writer->capacity, writer->offset + 2, (uint16_t)value_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (value_len > 0) {
        memcpy(writer->buffer + writer->offset + LAN_CHAT_TLV_HEADER_LEN, value, value_len);
    }
    writer->offset += LAN_CHAT_TLV_HEADER_LEN + value_len;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_tlv_write_u16(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, uint16_t value)
{
    uint8_t bytes[2];
    lan_chat_status_t status = lan_chat_write_u16_be(bytes, sizeof(bytes), 0, value);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return lan_chat_tlv_write_bytes(writer, field_id, required, bytes, sizeof(bytes));
}

lan_chat_status_t lan_chat_tlv_write_u32(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, uint32_t value)
{
    uint8_t bytes[4];
    lan_chat_status_t status = lan_chat_write_u32_be(bytes, sizeof(bytes), 0, value);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return lan_chat_tlv_write_bytes(writer, field_id, required, bytes, sizeof(bytes));
}

lan_chat_status_t lan_chat_tlv_write_u64(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, uint64_t value)
{
    uint8_t bytes[8];
    lan_chat_status_t status = lan_chat_write_u64_be(bytes, sizeof(bytes), 0, value);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return lan_chat_tlv_write_bytes(writer, field_id, required, bytes, sizeof(bytes));
}

lan_chat_status_t lan_chat_tlv_write_string(lan_chat_tlv_writer_t *writer, uint16_t field_id, int required, const char *value, size_t value_len)
{
    return lan_chat_tlv_write_bytes(writer, field_id, required, (const uint8_t *)value, value_len);
}

lan_chat_status_t lan_chat_tlv_reader_init(lan_chat_tlv_reader_t *reader, const uint8_t *buffer, size_t length)
{
    if (reader == 0 || (buffer == 0 && length > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    memset(reader, 0, sizeof(*reader));
    reader->buffer = buffer;
    reader->length = length;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_tlv_next(lan_chat_tlv_reader_t *reader, lan_chat_tlv_t *out_tlv)
{
    lan_chat_status_t status;
    uint16_t raw_type = 0;
    uint16_t value_len = 0;
    uint16_t field_id;

    if (reader == 0 || out_tlv == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    memset(out_tlv, 0, sizeof(*out_tlv));
    if (reader->offset == reader->length) {
        return LAN_CHAT_STATUS_NEED_MORE_DATA;
    }
    if (reader->offset > reader->length || LAN_CHAT_TLV_HEADER_LEN > (reader->length - reader->offset)) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }

    status = lan_chat_read_u16_be(reader->buffer, reader->length, reader->offset, &raw_type);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = lan_chat_read_u16_be(reader->buffer, reader->length, reader->offset + 2, &value_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    field_id = (uint16_t)(raw_type & LAN_CHAT_TLV_FIELD_MASK);
    if (field_id == 0) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    if ((size_t)value_len > (reader->length - reader->offset - LAN_CHAT_TLV_HEADER_LEN)) {
        return LAN_CHAT_STATUS_BAD_HEADER;
    }
    status = remember_field(reader, field_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    out_tlv->raw_type = raw_type;
    out_tlv->field_id = field_id;
    out_tlv->required = (uint8_t)((raw_type & LAN_CHAT_TLV_REQUIRED_MASK) != 0);
    out_tlv->value = reader->buffer + reader->offset + LAN_CHAT_TLV_HEADER_LEN;
    out_tlv->length = value_len;
    reader->offset += LAN_CHAT_TLV_HEADER_LEN + value_len;
    return LAN_CHAT_STATUS_OK;
}
