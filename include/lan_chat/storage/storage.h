#ifndef LAN_CHAT_STORAGE_STORAGE_H
#define LAN_CHAT_STORAGE_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/status.h"
#include "lan_chat/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lan_chat_storage lan_chat_storage_t;

typedef enum lan_chat_storage_kind {
    LAN_CHAT_STORAGE_KIND_MEMORY = 1,
    LAN_CHAT_STORAGE_KIND_MYSQL = 2
} lan_chat_storage_kind_t;

typedef enum lan_chat_delivery_state {
    LAN_CHAT_DELIVERY_PENDING = 1,
    LAN_CHAT_DELIVERY_DELIVERED = 2,
    LAN_CHAT_DELIVERY_ACKED = 3
} lan_chat_delivery_state_t;

typedef struct lan_chat_storage_config {
    lan_chat_storage_kind_t kind;
    const char *host;
    uint16_t port;
    const char *database;
    const char *user;
    const char *password;
    uint32_t connection_pool_size;
} lan_chat_storage_config_t;

typedef struct lan_chat_account_create {
    const char *username;
    const char *nickname;
    const char *password_hash;
    const char *password_salt;
} lan_chat_account_create_t;

typedef struct lan_chat_login_record {
    lan_chat_user_id_t user_id;
    char username[LAN_CHAT_MAX_USERNAME_LEN + 1];
    char nickname[LAN_CHAT_MAX_NICKNAME_LEN + 1];
    char password_hash[LAN_CHAT_PASSWORD_HASH_LEN + 1];
    char password_salt[LAN_CHAT_PASSWORD_SALT_LEN + 1];
    uint8_t enabled;
} lan_chat_login_record_t;

typedef struct lan_chat_user_record {
    lan_chat_user_id_t user_id;
    char username[LAN_CHAT_MAX_USERNAME_LEN + 1];
    char nickname[LAN_CHAT_MAX_NICKNAME_LEN + 1];
    uint8_t online;
} lan_chat_user_record_t;

typedef struct lan_chat_message_record {
    lan_chat_message_id_t message_id;
    lan_chat_conversation_id_t conversation_id;
    lan_chat_user_id_t sender_id;
    lan_chat_user_id_t receiver_id;
    uint16_t content_type;
    char content[LAN_CHAT_MAX_MESSAGE_TEXT_LEN + 1];
    uint64_t client_message_id;
    uint64_t send_time_ms;
    uint64_t store_time_ms;
} lan_chat_message_record_t;

typedef struct lan_chat_delivery_record {
    lan_chat_delivery_id_t delivery_id;
    lan_chat_message_record_t message;
    uint16_t delivery_state;
} lan_chat_delivery_record_t;

typedef struct lan_chat_storage_vtable {
    void (*close)(lan_chat_storage_t *storage);
    lan_chat_status_t (*create_account)(
        lan_chat_storage_t *storage,
        const lan_chat_account_create_t *account,
        lan_chat_user_id_t *out_user_id);
    lan_chat_status_t (*get_login_record)(
        lan_chat_storage_t *storage,
        const char *username,
        lan_chat_login_record_t *out_record);
    lan_chat_status_t (*update_last_login)(
        lan_chat_storage_t *storage,
        lan_chat_user_id_t user_id,
        uint64_t login_time_ms);
    lan_chat_status_t (*list_users)(
        lan_chat_storage_t *storage,
        lan_chat_user_record_t *out_users,
        size_t users_capacity,
        size_t *out_user_count);
    lan_chat_status_t (*store_private_message)(
        lan_chat_storage_t *storage,
        const lan_chat_message_record_t *message,
        lan_chat_message_id_t *out_message_id,
        lan_chat_delivery_id_t *out_delivery_id);
    lan_chat_status_t (*list_history)(
        lan_chat_storage_t *storage,
        lan_chat_user_id_t user_a,
        lan_chat_user_id_t user_b,
        uint64_t before_message_id,
        size_t limit,
        lan_chat_message_record_t *out_messages,
        size_t messages_capacity,
        size_t *out_message_count);
    lan_chat_status_t (*list_pending_deliveries)(
        lan_chat_storage_t *storage,
        lan_chat_user_id_t receiver_id,
        size_t limit,
        lan_chat_delivery_record_t *out_deliveries,
        size_t deliveries_capacity,
        size_t *out_delivery_count);
    lan_chat_status_t (*mark_delivery_state)(
        lan_chat_storage_t *storage,
        lan_chat_delivery_id_t delivery_id,
        uint16_t delivery_state,
        uint64_t state_time_ms);
} lan_chat_storage_vtable_t;

struct lan_chat_storage {
    const lan_chat_storage_vtable_t *vtable;
};

void lan_chat_storage_close(lan_chat_storage_t *storage);

lan_chat_status_t lan_chat_storage_create_account(
    lan_chat_storage_t *storage,
    const lan_chat_account_create_t *account,
    lan_chat_user_id_t *out_user_id);

lan_chat_status_t lan_chat_storage_get_login_record(
    lan_chat_storage_t *storage,
    const char *username,
    lan_chat_login_record_t *out_record);

lan_chat_status_t lan_chat_storage_update_last_login(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t user_id,
    uint64_t login_time_ms);

lan_chat_status_t lan_chat_storage_list_users(
    lan_chat_storage_t *storage,
    lan_chat_user_record_t *out_users,
    size_t users_capacity,
    size_t *out_user_count);

lan_chat_status_t lan_chat_storage_store_private_message(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id,
    lan_chat_delivery_id_t *out_delivery_id);

lan_chat_status_t lan_chat_storage_list_history(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b,
    uint64_t before_message_id,
    size_t limit,
    lan_chat_message_record_t *out_messages,
    size_t messages_capacity,
    size_t *out_message_count);

lan_chat_status_t lan_chat_storage_list_pending_deliveries(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t receiver_id,
    size_t limit,
    lan_chat_delivery_record_t *out_deliveries,
    size_t deliveries_capacity,
    size_t *out_delivery_count);

lan_chat_status_t lan_chat_storage_mark_delivery_state(
    lan_chat_storage_t *storage,
    lan_chat_delivery_id_t delivery_id,
    uint16_t delivery_state,
    uint64_t state_time_ms);

#ifdef __cplusplus
}
#endif

#endif
