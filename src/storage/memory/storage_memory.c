#include "lan_chat/storage/memory_storage.h"

#include <stdlib.h>
#include <string.h>

enum {
    LAN_CHAT_MEMORY_MAX_ACCOUNTS = 128,
    LAN_CHAT_MEMORY_MAX_CONVERSATIONS = 256,
    LAN_CHAT_MEMORY_MAX_MESSAGES = 1024,
    LAN_CHAT_MEMORY_MAX_DELIVERIES = 1024,
    LAN_CHAT_MEMORY_MAX_MEMBERS = 1024,
    LAN_CHAT_MEMORY_MAX_FILES = 128
};

typedef struct lan_chat_memory_account {
    lan_chat_user_id_t user_id;
    char username[LAN_CHAT_MAX_USERNAME_LEN + 1];
    char nickname[LAN_CHAT_MAX_NICKNAME_LEN + 1];
    char password_hash[LAN_CHAT_PASSWORD_HASH_LEN + 1];
    char password_salt[LAN_CHAT_PASSWORD_SALT_LEN + 1];
    uint8_t enabled;
    uint64_t last_login_ms;
} lan_chat_memory_account_t;

typedef struct lan_chat_memory_conversation {
    lan_chat_conversation_id_t conversation_id;
    uint8_t is_group;
    char name[LAN_CHAT_MAX_GROUP_NAME_LEN + 1];
    lan_chat_user_id_t user_a;
    lan_chat_user_id_t user_b;
} lan_chat_memory_conversation_t;

typedef struct lan_chat_memory_member {
    lan_chat_conversation_id_t conversation_id;
    lan_chat_user_id_t user_id;
    uint8_t active;
} lan_chat_memory_member_t;

typedef struct lan_chat_memory_delivery {
    lan_chat_delivery_id_t delivery_id;
    lan_chat_message_id_t message_id;
    lan_chat_user_id_t receiver_id;
    uint16_t delivery_state;
    uint64_t delivered_time_ms;
    uint64_t acked_time_ms;
} lan_chat_memory_delivery_t;

typedef struct lan_chat_memory_file {
    lan_chat_file_transfer_record_t record;
} lan_chat_memory_file_t;

typedef struct lan_chat_memory_storage {
    lan_chat_storage_t base;
    lan_chat_user_id_t next_user_id;
    lan_chat_conversation_id_t next_conversation_id;
    lan_chat_message_id_t next_message_id;
    lan_chat_delivery_id_t next_delivery_id;
    lan_chat_memory_account_t accounts[LAN_CHAT_MEMORY_MAX_ACCOUNTS];
    size_t account_count;
    lan_chat_memory_conversation_t conversations[LAN_CHAT_MEMORY_MAX_CONVERSATIONS];
    size_t conversation_count;
    lan_chat_memory_member_t members[LAN_CHAT_MEMORY_MAX_MEMBERS];
    size_t member_count;
    lan_chat_message_record_t messages[LAN_CHAT_MEMORY_MAX_MESSAGES];
    size_t message_count;
    lan_chat_memory_delivery_t deliveries[LAN_CHAT_MEMORY_MAX_DELIVERIES];
    size_t delivery_count;
    lan_chat_memory_file_t files[LAN_CHAT_MEMORY_MAX_FILES];
    size_t file_count;
    uint64_t next_file_transfer_id;
} lan_chat_memory_storage_t;

static size_t bounded_strlen(const char *value, size_t max_len)
{
    size_t len = 0;

    if (value == 0) {
        return 0;
    }
    while (len <= max_len && value[len] != '\0') {
        ++len;
    }
    return len;
}

static int string_is_valid(const char *value, size_t max_len, int allow_empty)
{
    size_t len;

    if (value == 0) {
        return 0;
    }
    len = bounded_strlen(value, max_len);
    if (len > max_len) {
        return 0;
    }
    if (!allow_empty && len == 0) {
        return 0;
    }
    return 1;
}

static void copy_string(char *dest, size_t dest_capacity, const char *src)
{
    size_t len;

    if (dest == 0 || dest_capacity == 0) {
        return;
    }
    dest[0] = '\0';
    if (src == 0) {
        return;
    }
    len = bounded_strlen(src, dest_capacity - 1);
    if (len >= dest_capacity) {
        len = dest_capacity - 1;
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
}

static void normalize_pair(lan_chat_user_id_t a, lan_chat_user_id_t b, lan_chat_user_id_t *out_low, lan_chat_user_id_t *out_high)
{
    if (a < b) {
        *out_low = a;
        *out_high = b;
    } else {
        *out_low = b;
        *out_high = a;
    }
}

static lan_chat_memory_account_t *find_account_by_id(lan_chat_memory_storage_t *memory, lan_chat_user_id_t user_id)
{
    size_t i;

    for (i = 0; i < memory->account_count; ++i) {
        if (memory->accounts[i].user_id == user_id) {
            return &memory->accounts[i];
        }
    }
    return 0;
}

static lan_chat_memory_account_t *find_account_by_username(lan_chat_memory_storage_t *memory, const char *username)
{
    size_t i;

    for (i = 0; i < memory->account_count; ++i) {
        if (strcmp(memory->accounts[i].username, username) == 0) {
            return &memory->accounts[i];
        }
    }
    return 0;
}

static int account_is_enabled(lan_chat_memory_storage_t *memory, lan_chat_user_id_t user_id)
{
    lan_chat_memory_account_t *account = find_account_by_id(memory, user_id);
    return account != 0 && account->enabled != 0;
}

static lan_chat_memory_conversation_t *find_conversation_by_id(
    lan_chat_memory_storage_t *memory,
    lan_chat_conversation_id_t conversation_id)
{
    size_t i;

    for (i = 0; i < memory->conversation_count; ++i) {
        if (memory->conversations[i].conversation_id == conversation_id) {
            return &memory->conversations[i];
        }
    }
    return 0;
}

static lan_chat_memory_conversation_t *find_private_conversation(
    lan_chat_memory_storage_t *memory,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b)
{
    lan_chat_user_id_t low;
    lan_chat_user_id_t high;
    size_t i;

    normalize_pair(user_a, user_b, &low, &high);
    for (i = 0; i < memory->conversation_count; ++i) {
        if (memory->conversations[i].user_a == low && memory->conversations[i].user_b == high) {
            return &memory->conversations[i];
        }
    }
    return 0;
}

static lan_chat_status_t get_or_create_private_conversation(
    lan_chat_memory_storage_t *memory,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b,
    lan_chat_conversation_id_t *out_conversation_id)
{
    lan_chat_memory_conversation_t *conversation;
    lan_chat_user_id_t low;
    lan_chat_user_id_t high;

    conversation = find_private_conversation(memory, user_a, user_b);
    if (conversation != 0) {
        *out_conversation_id = conversation->conversation_id;
        return LAN_CHAT_STATUS_OK;
    }
    if (memory->conversation_count >= LAN_CHAT_MEMORY_MAX_CONVERSATIONS) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    normalize_pair(user_a, user_b, &low, &high);
    conversation = &memory->conversations[memory->conversation_count++];
    conversation->conversation_id = memory->next_conversation_id++;
    conversation->is_group = 0;
    conversation->user_a = low;
    conversation->user_b = high;
    *out_conversation_id = conversation->conversation_id;
    return LAN_CHAT_STATUS_OK;
}

static int conversation_has_members(
    const lan_chat_memory_conversation_t *conversation,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b)
{
    lan_chat_user_id_t low;
    lan_chat_user_id_t high;

    normalize_pair(user_a, user_b, &low, &high);
    return conversation != 0 && conversation->user_a == low && conversation->user_b == high;
}

static int group_has_member(
    lan_chat_memory_storage_t *memory,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_user_id_t user_id)
{
    size_t i;

    for (i = 0; i < memory->member_count; ++i) {
        if (memory->members[i].conversation_id == conversation_id &&
            memory->members[i].user_id == user_id &&
            memory->members[i].active != 0) {
            return 1;
        }
    }
    return 0;
}

static int member_seen(const lan_chat_user_id_t *ids, size_t count, lan_chat_user_id_t user_id)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        if (ids[i] == user_id) {
            return 1;
        }
    }
    return 0;
}

static lan_chat_message_record_t *find_message_by_id(
    lan_chat_memory_storage_t *memory,
    lan_chat_message_id_t message_id)
{
    size_t i;

    for (i = 0; i < memory->message_count; ++i) {
        if (memory->messages[i].message_id == message_id) {
            return &memory->messages[i];
        }
    }
    return 0;
}

static lan_chat_memory_delivery_t *find_delivery_by_id(
    lan_chat_memory_storage_t *memory,
    lan_chat_delivery_id_t delivery_id)
{
    size_t i;

    for (i = 0; i < memory->delivery_count; ++i) {
        if (memory->deliveries[i].delivery_id == delivery_id) {
            return &memory->deliveries[i];
        }
    }
    return 0;
}

static int delivery_state_is_valid(uint16_t delivery_state)
{
    return delivery_state == LAN_CHAT_DELIVERY_PENDING ||
           delivery_state == LAN_CHAT_DELIVERY_DELIVERED ||
           delivery_state == LAN_CHAT_DELIVERY_ACKED;
}

static int delivery_transition_is_valid(uint16_t current_state, uint16_t next_state)
{
    if (current_state == next_state) {
        return 1;
    }
    if (current_state == LAN_CHAT_DELIVERY_PENDING &&
        (next_state == LAN_CHAT_DELIVERY_DELIVERED || next_state == LAN_CHAT_DELIVERY_ACKED)) {
        return 1;
    }
    if (current_state == LAN_CHAT_DELIVERY_DELIVERED && next_state == LAN_CHAT_DELIVERY_ACKED) {
        return 1;
    }
    return 0;
}

static void memory_close(lan_chat_storage_t *storage)
{
    free(storage);
}

static lan_chat_status_t memory_create_account(
    lan_chat_storage_t *storage,
    const lan_chat_account_create_t *account,
    lan_chat_user_id_t *out_user_id)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_memory_account_t *new_account;
    const char *nickname;

    if (memory == 0 || account == 0 || out_user_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_user_id = 0;
    if (!string_is_valid(account->username, LAN_CHAT_MAX_USERNAME_LEN, 0) ||
        !string_is_valid(account->password_hash, LAN_CHAT_PASSWORD_HASH_LEN, 0) ||
        !string_is_valid(account->password_salt, LAN_CHAT_PASSWORD_SALT_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (account->nickname != 0 && !string_is_valid(account->nickname, LAN_CHAT_MAX_NICKNAME_LEN, 1)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (find_account_by_username(memory, account->username) != 0) {
        return LAN_CHAT_STATUS_ALREADY_EXISTS;
    }
    if (memory->account_count >= LAN_CHAT_MEMORY_MAX_ACCOUNTS) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    new_account = &memory->accounts[memory->account_count++];
    new_account->user_id = memory->next_user_id++;
    copy_string(new_account->username, sizeof(new_account->username), account->username);
    nickname = (account->nickname != 0 && account->nickname[0] != '\0') ? account->nickname : account->username;
    copy_string(new_account->nickname, sizeof(new_account->nickname), nickname);
    copy_string(new_account->password_hash, sizeof(new_account->password_hash), account->password_hash);
    copy_string(new_account->password_salt, sizeof(new_account->password_salt), account->password_salt);
    new_account->enabled = 1;
    new_account->last_login_ms = 0;
    *out_user_id = new_account->user_id;
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_get_login_record(
    lan_chat_storage_t *storage,
    const char *username,
    lan_chat_login_record_t *out_record)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_memory_account_t *account;

    if (memory == 0 || out_record == 0 || !string_is_valid(username, LAN_CHAT_MAX_USERNAME_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    memset(out_record, 0, sizeof(*out_record));
    account = find_account_by_username(memory, username);
    if (account == 0) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }

    out_record->user_id = account->user_id;
    copy_string(out_record->username, sizeof(out_record->username), account->username);
    copy_string(out_record->nickname, sizeof(out_record->nickname), account->nickname);
    copy_string(out_record->password_hash, sizeof(out_record->password_hash), account->password_hash);
    copy_string(out_record->password_salt, sizeof(out_record->password_salt), account->password_salt);
    out_record->enabled = account->enabled;
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_update_last_login(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t user_id,
    uint64_t login_time_ms)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_memory_account_t *account;

    if (memory == 0 || user_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    account = find_account_by_id(memory, user_id);
    if (account == 0) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    account->last_login_ms = login_time_ms;
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_list_users(
    lan_chat_storage_t *storage,
    lan_chat_user_record_t *out_users,
    size_t users_capacity,
    size_t *out_user_count)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    size_t i;
    size_t count = 0;

    if (memory == 0 || out_user_count == 0 || (out_users == 0 && users_capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    for (i = 0; i < memory->account_count; ++i) {
        if (memory->accounts[i].enabled != 0) {
            ++count;
        }
    }
    *out_user_count = count;
    if (count > users_capacity) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    count = 0;
    for (i = 0; i < memory->account_count; ++i) {
        if (memory->accounts[i].enabled != 0) {
            out_users[count].user_id = memory->accounts[i].user_id;
            copy_string(out_users[count].username, sizeof(out_users[count].username), memory->accounts[i].username);
            copy_string(out_users[count].nickname, sizeof(out_users[count].nickname), memory->accounts[i].nickname);
            out_users[count].online = 0;
            ++count;
        }
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_store_private_message(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id,
    lan_chat_delivery_id_t *out_delivery_id)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_conversation_id_t conversation_id;
    lan_chat_memory_conversation_t *conversation;
    lan_chat_message_record_t *stored_message;
    lan_chat_memory_delivery_t *delivery;
    lan_chat_status_t status;

    if (memory == 0 || message == 0 || out_message_id == 0 || out_delivery_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_message_id = 0;
    *out_delivery_id = 0;
    if (message->sender_id == 0 || message->receiver_id == 0 || message->sender_id == message->receiver_id ||
        message->content_type == 0 ||
        !string_is_valid(message->content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (!account_is_enabled(memory, message->sender_id) || !account_is_enabled(memory, message->receiver_id)) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    if (memory->message_count >= LAN_CHAT_MEMORY_MAX_MESSAGES ||
        memory->delivery_count >= LAN_CHAT_MEMORY_MAX_DELIVERIES) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    if (message->conversation_id == 0) {
        status = get_or_create_private_conversation(memory, message->sender_id, message->receiver_id, &conversation_id);
        if (status != LAN_CHAT_STATUS_OK) {
            return status;
        }
    } else {
        conversation = find_conversation_by_id(memory, message->conversation_id);
        if (!conversation_has_members(conversation, message->sender_id, message->receiver_id)) {
            return LAN_CHAT_STATUS_NOT_FOUND;
        }
        conversation_id = message->conversation_id;
    }

    stored_message = &memory->messages[memory->message_count++];
    *stored_message = *message;
    stored_message->message_id = memory->next_message_id++;
    stored_message->conversation_id = conversation_id;
    if (stored_message->store_time_ms == 0) {
        stored_message->store_time_ms = stored_message->send_time_ms;
    }

    delivery = &memory->deliveries[memory->delivery_count++];
    delivery->delivery_id = memory->next_delivery_id++;
    delivery->message_id = stored_message->message_id;
    delivery->receiver_id = stored_message->receiver_id;
    delivery->delivery_state = LAN_CHAT_DELIVERY_PENDING;
    delivery->delivered_time_ms = 0;
    delivery->acked_time_ms = 0;

    *out_message_id = stored_message->message_id;
    *out_delivery_id = delivery->delivery_id;
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_create_group(
    lan_chat_storage_t *storage,
    const lan_chat_group_create_t *group,
    lan_chat_conversation_id_t *out_conversation_id)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_memory_conversation_t *conversation;
    lan_chat_user_id_t unique_members[LAN_CHAT_MAX_GROUP_MEMBERS];
    size_t unique_count = 0;
    size_t i;

    if (memory == 0 || group == 0 || out_conversation_id == 0 ||
        group->creator_id == 0 || group->member_ids == 0 || group->member_count == 0 ||
        group->member_count > LAN_CHAT_MAX_GROUP_MEMBERS ||
        !string_is_valid(group->name, LAN_CHAT_MAX_GROUP_NAME_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_conversation_id = 0;
    if (!account_is_enabled(memory, group->creator_id)) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    unique_members[unique_count++] = group->creator_id;
    for (i = 0; i < group->member_count; ++i) {
        if (group->member_ids[i] == 0 || !account_is_enabled(memory, group->member_ids[i])) {
            return LAN_CHAT_STATUS_NOT_FOUND;
        }
        if (!member_seen(unique_members, unique_count, group->member_ids[i])) {
            if (unique_count >= LAN_CHAT_MAX_GROUP_MEMBERS) {
                return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
            }
            unique_members[unique_count++] = group->member_ids[i];
        }
    }
    if (unique_count < 2) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (memory->conversation_count >= LAN_CHAT_MEMORY_MAX_CONVERSATIONS ||
        memory->member_count + unique_count > LAN_CHAT_MEMORY_MAX_MEMBERS) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    conversation = &memory->conversations[memory->conversation_count++];
    memset(conversation, 0, sizeof(*conversation));
    conversation->conversation_id = memory->next_conversation_id++;
    conversation->is_group = 1;
    copy_string(conversation->name, sizeof(conversation->name), group->name);
    for (i = 0; i < unique_count; ++i) {
        memory->members[memory->member_count].conversation_id = conversation->conversation_id;
        memory->members[memory->member_count].user_id = unique_members[i];
        memory->members[memory->member_count].active = 1;
        ++memory->member_count;
    }
    *out_conversation_id = conversation->conversation_id;
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_list_group_members(
    lan_chat_storage_t *storage,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_group_member_record_t *out_members,
    size_t members_capacity,
    size_t *out_member_count)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_memory_conversation_t *conversation;
    size_t i;
    size_t count = 0;

    if (memory == 0 || conversation_id == 0 || out_member_count == 0 ||
        (out_members == 0 && members_capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    conversation = find_conversation_by_id(memory, conversation_id);
    if (conversation == 0 || !conversation->is_group) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    for (i = 0; i < memory->member_count; ++i) {
        if (memory->members[i].conversation_id == conversation_id && memory->members[i].active != 0) {
            if (count < members_capacity) {
                out_members[count].user_id = memory->members[i].user_id;
            }
            ++count;
        }
    }
    *out_member_count = count;
    return count > members_capacity ? LAN_CHAT_STATUS_BUFFER_TOO_SMALL : LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_store_group_message(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_memory_conversation_t *conversation;
    lan_chat_message_record_t *stored_message;

    if (memory == 0 || message == 0 || out_message_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_message_id = 0;
    if (message->conversation_id == 0 || message->sender_id == 0 || message->content_type == 0 ||
        !string_is_valid(message->content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    conversation = find_conversation_by_id(memory, message->conversation_id);
    if (conversation == 0 || !conversation->is_group || !group_has_member(memory, message->conversation_id, message->sender_id)) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    if (memory->message_count >= LAN_CHAT_MEMORY_MAX_MESSAGES) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    stored_message = &memory->messages[memory->message_count++];
    *stored_message = *message;
    stored_message->message_id = memory->next_message_id++;
    stored_message->receiver_id = 0;
    if (stored_message->store_time_ms == 0) {
        stored_message->store_time_ms = stored_message->send_time_ms;
    }
    *out_message_id = stored_message->message_id;
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_store_file_transfer(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    const char *file_name,
    uint64_t file_size,
    uint32_t crc32,
    lan_chat_message_id_t *out_message_id,
    lan_chat_delivery_id_t *out_delivery_id,
    uint64_t *out_file_transfer_id)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_message_record_t file_message;
    lan_chat_status_t status;

    if (memory == 0 || message == 0 || file_name == 0 || out_message_id == 0 ||
        out_delivery_id == 0 || out_file_transfer_id == 0 ||
        file_size == 0 || file_size > LAN_CHAT_MAX_FILE_SIZE ||
        !string_is_valid(file_name, LAN_CHAT_MAX_FILE_NAME_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (memory->file_count >= LAN_CHAT_MEMORY_MAX_FILES) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    file_message = *message;
    file_message.content_type = LAN_CHAT_CONTENT_FILE;
    status = memory_store_private_message(storage, &file_message, out_message_id, out_delivery_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    memory->files[memory->file_count].record.file_transfer_id = memory->next_file_transfer_id++;
    memory->files[memory->file_count].record.message_id = *out_message_id;
    memory->files[memory->file_count].record.sender_id = message->sender_id;
    memory->files[memory->file_count].record.receiver_id = message->receiver_id;
    copy_string(memory->files[memory->file_count].record.file_name, sizeof(memory->files[memory->file_count].record.file_name), file_name);
    memory->files[memory->file_count].record.file_size = file_size;
    memory->files[memory->file_count].record.crc32 = crc32;
    memory->files[memory->file_count].record.transfer_state = LAN_CHAT_FILE_TRANSFER_STARTED;
    *out_file_transfer_id = memory->files[memory->file_count].record.file_transfer_id;
    ++memory->file_count;
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_complete_file_transfer(
    lan_chat_storage_t *storage,
    uint64_t file_transfer_id)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    size_t i;

    if (memory == 0 || file_transfer_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    for (i = 0; i < memory->file_count; ++i) {
        if (memory->files[i].record.file_transfer_id == file_transfer_id) {
            memory->files[i].record.transfer_state = LAN_CHAT_FILE_TRANSFER_COMPLETED;
            return LAN_CHAT_STATUS_OK;
        }
    }
    return LAN_CHAT_STATUS_NOT_FOUND;
}

static lan_chat_status_t memory_list_history(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b,
    uint64_t before_message_id,
    size_t limit,
    lan_chat_message_record_t *out_messages,
    size_t messages_capacity,
    size_t *out_message_count)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_memory_conversation_t *conversation;
    size_t i;
    size_t count = 0;
    size_t written = 0;

    if (memory == 0 || out_message_count == 0 || user_a == 0 || user_b == 0 || user_a == user_b ||
        (out_messages == 0 && messages_capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    conversation = find_private_conversation(memory, user_a, user_b);
    if (conversation == 0 || limit == 0) {
        *out_message_count = 0;
        return LAN_CHAT_STATUS_OK;
    }

    for (i = memory->message_count; i > 0; --i) {
        const lan_chat_message_record_t *message = &memory->messages[i - 1];
        if (message->conversation_id == conversation->conversation_id &&
            (before_message_id == 0 || message->message_id < before_message_id)) {
            ++count;
            if (count == limit) {
                break;
            }
        }
    }
    if (count > limit) {
        count = limit;
    }
    *out_message_count = count;
    if (count > messages_capacity) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    for (i = memory->message_count; i > 0 && written < count; --i) {
        const lan_chat_message_record_t *message = &memory->messages[i - 1];
        if (message->conversation_id == conversation->conversation_id &&
            (before_message_id == 0 || message->message_id < before_message_id)) {
            out_messages[written++] = *message;
        }
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_list_pending_deliveries(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t receiver_id,
    size_t limit,
    lan_chat_delivery_record_t *out_deliveries,
    size_t deliveries_capacity,
    size_t *out_delivery_count)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    size_t i;
    size_t count = 0;
    size_t written = 0;

    if (memory == 0 || out_delivery_count == 0 || receiver_id == 0 ||
        (out_deliveries == 0 && deliveries_capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (!account_is_enabled(memory, receiver_id)) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    if (limit == 0) {
        *out_delivery_count = 0;
        return LAN_CHAT_STATUS_OK;
    }

    for (i = 0; i < memory->delivery_count; ++i) {
        if (memory->deliveries[i].receiver_id == receiver_id &&
            memory->deliveries[i].delivery_state == LAN_CHAT_DELIVERY_PENDING) {
            ++count;
            if (count == limit) {
                break;
            }
        }
    }
    if (count > limit) {
        count = limit;
    }
    *out_delivery_count = count;
    if (count > deliveries_capacity) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < memory->delivery_count && written < count; ++i) {
        lan_chat_message_record_t *message;
        if (memory->deliveries[i].receiver_id != receiver_id ||
            memory->deliveries[i].delivery_state != LAN_CHAT_DELIVERY_PENDING) {
            continue;
        }
        message = find_message_by_id(memory, memory->deliveries[i].message_id);
        if (message == 0) {
            return LAN_CHAT_STATUS_INTERNAL_ERROR;
        }
        out_deliveries[written].delivery_id = memory->deliveries[i].delivery_id;
        out_deliveries[written].message = *message;
        out_deliveries[written].delivery_state = memory->deliveries[i].delivery_state;
        ++written;
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t memory_mark_delivery_state(
    lan_chat_storage_t *storage,
    lan_chat_delivery_id_t delivery_id,
    uint16_t delivery_state,
    uint64_t state_time_ms)
{
    lan_chat_memory_storage_t *memory = (lan_chat_memory_storage_t *)storage;
    lan_chat_memory_delivery_t *delivery;

    if (memory == 0 || delivery_id == 0 || !delivery_state_is_valid(delivery_state)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    delivery = find_delivery_by_id(memory, delivery_id);
    if (delivery == 0) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    if (!delivery_transition_is_valid(delivery->delivery_state, delivery_state)) {
        return LAN_CHAT_STATUS_INVALID_STATE;
    }
    if (delivery->delivery_state == delivery_state) {
        return LAN_CHAT_STATUS_OK;
    }

    delivery->delivery_state = delivery_state;
    if (delivery_state == LAN_CHAT_DELIVERY_DELIVERED) {
        delivery->delivered_time_ms = state_time_ms;
    }
    if (delivery_state == LAN_CHAT_DELIVERY_ACKED) {
        if (delivery->delivered_time_ms == 0) {
            delivery->delivered_time_ms = state_time_ms;
        }
        delivery->acked_time_ms = state_time_ms;
    }
    return LAN_CHAT_STATUS_OK;
}

static const lan_chat_storage_vtable_t k_memory_vtable = {
    memory_close,
    memory_create_account,
    memory_get_login_record,
    memory_update_last_login,
    memory_list_users,
    memory_store_private_message,
    memory_create_group,
    memory_list_group_members,
    memory_store_group_message,
    memory_store_file_transfer,
    memory_complete_file_transfer,
    memory_list_history,
    memory_list_pending_deliveries,
    memory_mark_delivery_state
};

lan_chat_status_t lan_chat_storage_memory_open(lan_chat_storage_t **out_storage)
{
    lan_chat_memory_storage_t *memory;

    if (out_storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    *out_storage = 0;
    memory = (lan_chat_memory_storage_t *)calloc(1, sizeof(*memory));
    if (memory == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }

    memory->base.vtable = &k_memory_vtable;
    memory->next_user_id = 1;
    memory->next_conversation_id = 1;
    memory->next_message_id = 1;
    memory->next_delivery_id = 1;
    memory->next_file_transfer_id = 1;
    *out_storage = &memory->base;
    return LAN_CHAT_STATUS_OK;
}
