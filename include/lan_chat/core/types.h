#ifndef LAN_CHAT_CORE_TYPES_H
#define LAN_CHAT_CORE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LAN_CHAT_MAX_USERNAME_LEN = 64,
    LAN_CHAT_MAX_NICKNAME_LEN = 64,
    LAN_CHAT_MAX_MESSAGE_TEXT_LEN = 4096,
    LAN_CHAT_MAX_GROUP_NAME_LEN = 64,
    LAN_CHAT_MAX_GROUP_MEMBERS = 16,
    LAN_CHAT_MAX_FILE_NAME_LEN = 255,
    LAN_CHAT_MAX_FILE_SIZE = 1048576,
    LAN_CHAT_FILE_CHUNK_SIZE = 32768,
    LAN_CHAT_PASSWORD_HASH_LEN = 128,
    LAN_CHAT_PASSWORD_SALT_LEN = 64,
    LAN_CHAT_MAGIC = 0x4C43,
    LAN_CHAT_PROTOCOL_VERSION = 1,
    LAN_CHAT_HEADER_LEN = 64,
    LAN_CHAT_MAX_BODY_LEN = 65535,
    LAN_CHAT_HEADER_RESERVED_LEN = 12
};

typedef uint64_t lan_chat_user_id_t;
typedef uint64_t lan_chat_session_id_t;
typedef uint64_t lan_chat_conversation_id_t;
typedef uint64_t lan_chat_message_id_t;
typedef uint64_t lan_chat_delivery_id_t;

typedef enum lan_chat_message_group {
    LAN_CHAT_GROUP_AUTH = 0x0001,
    LAN_CHAT_GROUP_SESSION = 0x0002,
    LAN_CHAT_GROUP_USER = 0x0003,
    LAN_CHAT_GROUP_PRESENCE = 0x0004,
    LAN_CHAT_GROUP_CHAT = 0x0005,
    LAN_CHAT_GROUP_HISTORY = 0x0006,
    LAN_CHAT_GROUP_HEARTBEAT = 0x0007,
    LAN_CHAT_GROUP_DISCOVERY = 0x0008,
    LAN_CHAT_GROUP_CONVERSATION = 0x0009,
    LAN_CHAT_GROUP_FILE = 0x000A,
    LAN_CHAT_GROUP_ERROR = 0x00FF
} lan_chat_message_group_t;

typedef enum lan_chat_message_type {
    LAN_CHAT_MSG_REQ = 0x0001,
    LAN_CHAT_MSG_RSP = 0x0002,
    LAN_CHAT_MSG_NOTIFY = 0x0003,
    LAN_CHAT_MSG_ACK = 0x0004
} lan_chat_message_type_t;

typedef enum lan_chat_packet_flags {
    LAN_CHAT_FLAG_REQUEST = 0x0001,
    LAN_CHAT_FLAG_RESPONSE = 0x0002,
    LAN_CHAT_FLAG_NOTIFY = 0x0004,
    LAN_CHAT_FLAG_ACK = 0x0008,
    LAN_CHAT_FLAG_ERROR = 0x0010,
    LAN_CHAT_FLAG_KNOWN_MASK = 0x001F
} lan_chat_packet_flags_t;

#ifdef __cplusplus
}
#endif

#endif
