#include "lan_chat/storage/storage.h"

static lan_chat_status_t storage_missing_method(void)
{
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
}

void lan_chat_storage_close(lan_chat_storage_t *storage)
{
    if (storage != 0 && storage->vtable != 0 && storage->vtable->close != 0) {
        storage->vtable->close(storage);
    }
}

lan_chat_status_t lan_chat_storage_create_account(
    lan_chat_storage_t *storage,
    const lan_chat_account_create_t *account,
    lan_chat_user_id_t *out_user_id)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->create_account == 0) {
        return storage_missing_method();
    }
    return storage->vtable->create_account(storage, account, out_user_id);
}

lan_chat_status_t lan_chat_storage_get_login_record(
    lan_chat_storage_t *storage,
    const char *username,
    lan_chat_login_record_t *out_record)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->get_login_record == 0) {
        return storage_missing_method();
    }
    return storage->vtable->get_login_record(storage, username, out_record);
}

lan_chat_status_t lan_chat_storage_update_last_login(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t user_id,
    uint64_t login_time_ms)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->update_last_login == 0) {
        return storage_missing_method();
    }
    return storage->vtable->update_last_login(storage, user_id, login_time_ms);
}

lan_chat_status_t lan_chat_storage_list_users(
    lan_chat_storage_t *storage,
    lan_chat_user_record_t *out_users,
    size_t users_capacity,
    size_t *out_user_count)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->list_users == 0) {
        return storage_missing_method();
    }
    return storage->vtable->list_users(storage, out_users, users_capacity, out_user_count);
}

lan_chat_status_t lan_chat_storage_store_private_message(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id,
    lan_chat_delivery_id_t *out_delivery_id)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->store_private_message == 0) {
        return storage_missing_method();
    }
    return storage->vtable->store_private_message(storage, message, out_message_id, out_delivery_id);
}

lan_chat_status_t lan_chat_storage_create_group(
    lan_chat_storage_t *storage,
    const lan_chat_group_create_t *group,
    lan_chat_conversation_id_t *out_conversation_id)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->create_group == 0) {
        return storage_missing_method();
    }
    return storage->vtable->create_group(storage, group, out_conversation_id);
}

lan_chat_status_t lan_chat_storage_list_group_members(
    lan_chat_storage_t *storage,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_group_member_record_t *out_members,
    size_t members_capacity,
    size_t *out_member_count)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->list_group_members == 0) {
        return storage_missing_method();
    }
    return storage->vtable->list_group_members(
        storage,
        conversation_id,
        out_members,
        members_capacity,
        out_member_count);
}

lan_chat_status_t lan_chat_storage_store_group_message(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->store_group_message == 0) {
        return storage_missing_method();
    }
    return storage->vtable->store_group_message(storage, message, out_message_id);
}

lan_chat_status_t lan_chat_storage_store_file_transfer(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    const char *file_name,
    uint64_t file_size,
    uint32_t crc32,
    lan_chat_message_id_t *out_message_id,
    lan_chat_delivery_id_t *out_delivery_id,
    uint64_t *out_file_transfer_id)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->store_file_transfer == 0) {
        return storage_missing_method();
    }
    return storage->vtable->store_file_transfer(
        storage,
        message,
        file_name,
        file_size,
        crc32,
        out_message_id,
        out_delivery_id,
        out_file_transfer_id);
}

lan_chat_status_t lan_chat_storage_complete_file_transfer(
    lan_chat_storage_t *storage,
    uint64_t file_transfer_id)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->complete_file_transfer == 0) {
        return storage_missing_method();
    }
    return storage->vtable->complete_file_transfer(storage, file_transfer_id);
}

lan_chat_status_t lan_chat_storage_list_history(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b,
    uint64_t before_message_id,
    size_t limit,
    lan_chat_message_record_t *out_messages,
    size_t messages_capacity,
    size_t *out_message_count)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->list_history == 0) {
        return storage_missing_method();
    }
    return storage->vtable->list_history(
        storage,
        user_a,
        user_b,
        before_message_id,
        limit,
        out_messages,
        messages_capacity,
        out_message_count);
}

lan_chat_status_t lan_chat_storage_list_pending_deliveries(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t receiver_id,
    size_t limit,
    lan_chat_delivery_record_t *out_deliveries,
    size_t deliveries_capacity,
    size_t *out_delivery_count)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->list_pending_deliveries == 0) {
        return storage_missing_method();
    }
    return storage->vtable->list_pending_deliveries(
        storage,
        receiver_id,
        limit,
        out_deliveries,
        deliveries_capacity,
        out_delivery_count);
}

lan_chat_status_t lan_chat_storage_mark_delivery_state(
    lan_chat_storage_t *storage,
    lan_chat_delivery_id_t delivery_id,
    uint16_t delivery_state,
    uint64_t state_time_ms)
{
    if (storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (storage->vtable == 0 || storage->vtable->mark_delivery_state == 0) {
        return storage_missing_method();
    }
    return storage->vtable->mark_delivery_state(storage, delivery_id, delivery_state, state_time_ms);
}
