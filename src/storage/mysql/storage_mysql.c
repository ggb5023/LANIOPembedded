#include "lan_chat/storage/mysql_storage.h"

#include <stdlib.h>
#include <string.h>

#if LAN_CHAT_HAS_MYSQL
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <mysql.h>
#include <mysqld_error.h>
#endif

typedef struct lan_chat_mysql_storage {
    lan_chat_storage_t base;
    lan_chat_storage_config_t config;
#if LAN_CHAT_HAS_MYSQL
    MYSQL *mysql;
#endif
} lan_chat_mysql_storage_t;

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

static void copy_string(char *dest, size_t dest_capacity, const char *src, unsigned long src_len)
{
    size_t len;

    if (dest == 0 || dest_capacity == 0) {
        return;
    }
    dest[0] = '\0';
    if (src == 0) {
        return;
    }
    len = src_len;
    if (len >= dest_capacity) {
        len = dest_capacity - 1;
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
}

#if LAN_CHAT_HAS_MYSQL
static lan_chat_status_t map_mysql_errno(unsigned int error_code)
{
    if (error_code == ER_DUP_ENTRY) {
        return LAN_CHAT_STATUS_ALREADY_EXISTS;
    }
    return LAN_CHAT_STATUS_STORAGE_ERROR;
}

static lan_chat_status_t stmt_status(MYSQL_STMT *stmt)
{
    return map_mysql_errno(mysql_stmt_errno(stmt));
}

static void bind_u64(MYSQL_BIND *bind, uint64_t *value)
{
    memset(bind, 0, sizeof(*bind));
    bind->buffer_type = MYSQL_TYPE_LONGLONG;
    bind->buffer = value;
    bind->is_unsigned = true;
}

static void bind_u16(MYSQL_BIND *bind, uint16_t *value)
{
    memset(bind, 0, sizeof(*bind));
    bind->buffer_type = MYSQL_TYPE_SHORT;
    bind->buffer = value;
    bind->is_unsigned = true;
}

static void bind_u32(MYSQL_BIND *bind, uint32_t *value)
{
    memset(bind, 0, sizeof(*bind));
    bind->buffer_type = MYSQL_TYPE_LONG;
    bind->buffer = value;
    bind->is_unsigned = true;
}

static void bind_string(MYSQL_BIND *bind, const char *value, unsigned long *length)
{
    memset(bind, 0, sizeof(*bind));
    bind->buffer_type = MYSQL_TYPE_STRING;
    bind->buffer = (void *)value;
    bind->buffer_length = *length;
    bind->length = length;
}

static void bind_string_result(MYSQL_BIND *bind, char *buffer, unsigned long capacity, unsigned long *length, bool *is_null)
{
    memset(bind, 0, sizeof(*bind));
    bind->buffer_type = MYSQL_TYPE_STRING;
    bind->buffer = buffer;
    bind->buffer_length = capacity;
    bind->length = length;
    bind->is_null = is_null;
}

static lan_chat_status_t exec_query(lan_chat_mysql_storage_t *storage, const char *query)
{
    if (mysql_query(storage->mysql, query) != 0) {
        return map_mysql_errno(mysql_errno(storage->mysql));
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t begin_transaction(lan_chat_mysql_storage_t *storage)
{
    return exec_query(storage, "START TRANSACTION");
}

static lan_chat_status_t commit_transaction(lan_chat_mysql_storage_t *storage)
{
    return exec_query(storage, "COMMIT");
}

static void rollback_transaction(lan_chat_mysql_storage_t *storage)
{
    (void)exec_query(storage, "ROLLBACK");
}

static lan_chat_status_t execute_statement(MYSQL *mysql, const char *sql, MYSQL_BIND *params)
{
    MYSQL_STMT *stmt = mysql_stmt_init(mysql);
    lan_chat_status_t status = LAN_CHAT_STATUS_OK;

    if (stmt == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }
    if (params != 0 && mysql_stmt_bind_param(stmt, params) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }
    if (mysql_stmt_execute(stmt) != 0) {
        status = stmt_status(stmt);
    }
    mysql_stmt_close(stmt);
    return status;
}

static lan_chat_status_t mysql_scalar_u64(
    MYSQL *mysql,
    const char *sql,
    MYSQL_BIND *params,
    uint64_t *out_value,
    int *out_found)
{
    MYSQL_STMT *stmt = mysql_stmt_init(mysql);
    MYSQL_BIND result[1];
    uint64_t value = 0;
    unsigned long value_len = 0;
    bool is_null = false;
    int fetch_result;
    lan_chat_status_t status = LAN_CHAT_STATUS_OK;

    if (out_value != 0) {
        *out_value = 0;
    }
    if (out_found != 0) {
        *out_found = 0;
    }
    if (stmt == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }
    if (params != 0 && mysql_stmt_bind_param(stmt, params) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }
    bind_u64(&result[0], &value);
    result[0].length = &value_len;
    result[0].is_null = &is_null;
    if (mysql_stmt_bind_result(stmt, result) != 0 || mysql_stmt_execute(stmt) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }
    fetch_result = mysql_stmt_fetch(stmt);
    if (fetch_result == 0 || fetch_result == MYSQL_DATA_TRUNCATED) {
        if (!is_null && out_value != 0) {
            *out_value = value;
        }
        if (out_found != 0) {
            *out_found = 1;
        }
    } else if (fetch_result == MYSQL_NO_DATA) {
        status = LAN_CHAT_STATUS_OK;
    } else {
        status = stmt_status(stmt);
    }
    mysql_stmt_close(stmt);
    return status;
}

static lan_chat_status_t find_private_conversation(
    MYSQL *mysql,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b,
    lan_chat_conversation_id_t *out_conversation_id,
    int *out_found)
{
    MYSQL_BIND params[2];
    uint64_t a = user_a < user_b ? user_a : user_b;
    uint64_t b = user_a < user_b ? user_b : user_a;
    const char *sql =
        "SELECT cm1.conversation_id "
        "FROM conversation_members cm1 "
        "JOIN conversation_members cm2 ON cm1.conversation_id = cm2.conversation_id "
        "JOIN conversations c ON c.conversation_id = cm1.conversation_id "
        "WHERE c.conversation_type = 'private' "
        "AND cm1.user_id = ? AND cm2.user_id = ? "
        "AND cm1.active = 1 AND cm2.active = 1 "
        "ORDER BY cm1.conversation_id ASC LIMIT 1";

    bind_u64(&params[0], &a);
    bind_u64(&params[1], &b);
    return mysql_scalar_u64(mysql, sql, params, out_conversation_id, out_found);
}

static lan_chat_status_t ensure_user_enabled(MYSQL *mysql, lan_chat_user_id_t user_id)
{
    MYSQL_BIND params[1];
    uint64_t id = user_id;
    uint64_t found_id = 0;
    int found = 0;
    lan_chat_status_t status;
    const char *sql = "SELECT user_id FROM accounts WHERE user_id = ? AND enabled = 1";

    bind_u64(&params[0], &id);
    status = mysql_scalar_u64(mysql, sql, params, &found_id, &found);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return found ? LAN_CHAT_STATUS_OK : LAN_CHAT_STATUS_NOT_FOUND;
}

static lan_chat_status_t insert_conversation(MYSQL *mysql, lan_chat_conversation_id_t *out_conversation_id)
{
    lan_chat_status_t status = execute_statement(
        mysql,
        "INSERT INTO conversations (conversation_type) VALUES ('private')",
        0);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    *out_conversation_id = mysql_insert_id(mysql);
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t insert_group_conversation(
    MYSQL *mysql,
    const char *name,
    lan_chat_conversation_id_t *out_conversation_id)
{
    MYSQL_BIND params[1];
    unsigned long name_len = (unsigned long)bounded_strlen(name, LAN_CHAT_MAX_GROUP_NAME_LEN);
    lan_chat_status_t status;
    const char *sql =
        "INSERT INTO conversations (conversation_type, conversation_name) VALUES ('group', ?)";

    bind_string(&params[0], name, &name_len);
    status = execute_statement(mysql, sql, params);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    *out_conversation_id = mysql_insert_id(mysql);
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t ensure_conversation_member(
    MYSQL *mysql,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_user_id_t user_id)
{
    MYSQL_BIND params[2];
    uint64_t cid = conversation_id;
    uint64_t uid = user_id;
    const char *sql =
        "INSERT INTO conversation_members (conversation_id, user_id, active) "
        "VALUES (?, ?, 1) "
        "ON DUPLICATE KEY UPDATE active = VALUES(active)";

    bind_u64(&params[0], &cid);
    bind_u64(&params[1], &uid);
    return execute_statement(mysql, sql, params);
}

static lan_chat_status_t validate_conversation_members(
    MYSQL *mysql,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b)
{
    MYSQL_BIND params[3];
    uint64_t cid = conversation_id;
    uint64_t a = user_a;
    uint64_t b = user_b;
    uint64_t member_count = 0;
    int found = 0;
    lan_chat_status_t status;
    const char *sql =
        "SELECT COUNT(*) FROM conversation_members "
        "WHERE conversation_id = ? AND active = 1 AND user_id IN (?, ?)";

    bind_u64(&params[0], &cid);
    bind_u64(&params[1], &a);
    bind_u64(&params[2], &b);
    status = mysql_scalar_u64(mysql, sql, params, &member_count, &found);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return found && member_count == 2 ? LAN_CHAT_STATUS_OK : LAN_CHAT_STATUS_NOT_FOUND;
}

static lan_chat_status_t validate_group_member(
    MYSQL *mysql,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_user_id_t user_id)
{
    MYSQL_BIND params[2];
    uint64_t cid = conversation_id;
    uint64_t uid = user_id;
    uint64_t found_id = 0;
    int found = 0;
    lan_chat_status_t status;
    const char *sql =
        "SELECT cm.user_id "
        "FROM conversation_members cm "
        "JOIN conversations c ON c.conversation_id = cm.conversation_id "
        "WHERE cm.conversation_id = ? AND cm.user_id = ? "
        "AND cm.active = 1 AND c.conversation_type = 'group' "
        "LIMIT 1";

    bind_u64(&params[0], &cid);
    bind_u64(&params[1], &uid);
    status = mysql_scalar_u64(mysql, sql, params, &found_id, &found);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return found ? LAN_CHAT_STATUS_OK : LAN_CHAT_STATUS_NOT_FOUND;
}

static lan_chat_status_t get_or_create_conversation(
    lan_chat_mysql_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_conversation_id_t *out_conversation_id)
{
    lan_chat_status_t status;
    int found = 0;

    if (message->conversation_id != 0) {
        status = validate_conversation_members(storage->mysql, message->conversation_id, message->sender_id, message->receiver_id);
        if (status != LAN_CHAT_STATUS_OK) {
            return status;
        }
        *out_conversation_id = message->conversation_id;
        return LAN_CHAT_STATUS_OK;
    }

    status = find_private_conversation(
        storage->mysql,
        message->sender_id,
        message->receiver_id,
        out_conversation_id,
        &found);
    if (status != LAN_CHAT_STATUS_OK || found) {
        return status;
    }

    status = insert_conversation(storage->mysql, out_conversation_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = ensure_conversation_member(storage->mysql, *out_conversation_id, message->sender_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return ensure_conversation_member(storage->mysql, *out_conversation_id, message->receiver_id);
}

static lan_chat_status_t insert_message(
    lan_chat_mysql_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_message_id_t *out_message_id)
{
    MYSQL_BIND params[7];
    uint64_t cid = conversation_id;
    uint64_t sender_id = message->sender_id;
    uint64_t receiver_id = message->receiver_id;
    uint16_t content_type = message->content_type;
    unsigned long content_len = (unsigned long)bounded_strlen(message->content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN);
    uint64_t client_message_id = message->client_message_id;
    uint64_t send_time_ms = message->send_time_ms;
    lan_chat_status_t status;
    const char *sql =
        "INSERT INTO messages "
        "(conversation_id, sender_id, receiver_id, content_type, content, client_message_id, send_time) "
        "VALUES (?, ?, ?, ?, ?, ?, FROM_UNIXTIME(? / 1000.0))";

    bind_u64(&params[0], &cid);
    bind_u64(&params[1], &sender_id);
    bind_u64(&params[2], &receiver_id);
    bind_u16(&params[3], &content_type);
    bind_string(&params[4], message->content, &content_len);
    bind_u64(&params[5], &client_message_id);
    bind_u64(&params[6], &send_time_ms);

    status = execute_statement(storage->mysql, sql, params);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    *out_message_id = mysql_insert_id(storage->mysql);
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t insert_group_message(
    lan_chat_mysql_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id)
{
    MYSQL_BIND params[6];
    uint64_t cid = message->conversation_id;
    uint64_t sender_id = message->sender_id;
    uint16_t content_type = message->content_type;
    unsigned long content_len = (unsigned long)bounded_strlen(message->content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN);
    uint64_t client_message_id = message->client_message_id;
    uint64_t send_time_ms = message->send_time_ms;
    lan_chat_status_t status;
    const char *sql =
        "INSERT INTO messages "
        "(conversation_id, sender_id, receiver_id, content_type, content, client_message_id, send_time) "
        "VALUES (?, ?, NULL, ?, ?, ?, FROM_UNIXTIME(? / 1000.0))";

    bind_u64(&params[0], &cid);
    bind_u64(&params[1], &sender_id);
    bind_u16(&params[2], &content_type);
    bind_string(&params[3], message->content, &content_len);
    bind_u64(&params[4], &client_message_id);
    bind_u64(&params[5], &send_time_ms);
    status = execute_statement(storage->mysql, sql, params);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    *out_message_id = mysql_insert_id(storage->mysql);
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t insert_delivery(
    lan_chat_mysql_storage_t *storage,
    lan_chat_message_id_t message_id,
    lan_chat_user_id_t receiver_id,
    lan_chat_delivery_id_t *out_delivery_id)
{
    MYSQL_BIND params[2];
    uint64_t mid = message_id;
    uint64_t rid = receiver_id;
    lan_chat_status_t status;
    const char *sql =
        "INSERT INTO message_deliveries (message_id, receiver_id, delivery_state) "
        "VALUES (?, ?, 'pending')";

    bind_u64(&params[0], &mid);
    bind_u64(&params[1], &rid);
    status = execute_statement(storage->mysql, sql, params);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    *out_delivery_id = mysql_insert_id(storage->mysql);
    return LAN_CHAT_STATUS_OK;
}
#endif

static void mysql_storage_close(lan_chat_storage_t *storage)
{
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
#if LAN_CHAT_HAS_MYSQL
    if (mysql_storage != 0 && mysql_storage->mysql != 0) {
        mysql_close(mysql_storage->mysql);
        mysql_storage->mysql = 0;
    }
#endif
    free(mysql_storage);
}

#if !LAN_CHAT_HAS_MYSQL
static lan_chat_status_t mysql_not_implemented(void)
{
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
}
#endif

static lan_chat_status_t mysql_create_account(
    lan_chat_storage_t *storage,
    const lan_chat_account_create_t *account,
    lan_chat_user_id_t *out_user_id)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_BIND params[4];
    const char *nickname;
    unsigned long username_len;
    unsigned long nickname_len;
    unsigned long hash_len;
    unsigned long salt_len;
    lan_chat_status_t status;
    const char *sql =
        "INSERT INTO accounts (username, nickname, password_hash, password_salt, enabled) "
        "VALUES (?, ?, ?, ?, 1)";

    if (mysql_storage == 0 || account == 0 || out_user_id == 0) {
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

    nickname = (account->nickname != 0 && account->nickname[0] != '\0') ? account->nickname : account->username;
    username_len = (unsigned long)bounded_strlen(account->username, LAN_CHAT_MAX_USERNAME_LEN);
    nickname_len = (unsigned long)bounded_strlen(nickname, LAN_CHAT_MAX_NICKNAME_LEN);
    hash_len = (unsigned long)bounded_strlen(account->password_hash, LAN_CHAT_PASSWORD_HASH_LEN);
    salt_len = (unsigned long)bounded_strlen(account->password_salt, LAN_CHAT_PASSWORD_SALT_LEN);

    bind_string(&params[0], account->username, &username_len);
    bind_string(&params[1], nickname, &nickname_len);
    bind_string(&params[2], account->password_hash, &hash_len);
    bind_string(&params[3], account->password_salt, &salt_len);
    status = execute_statement(mysql_storage->mysql, sql, params);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    *out_user_id = mysql_insert_id(mysql_storage->mysql);
    return LAN_CHAT_STATUS_OK;
#else
    (void)storage;
    (void)account;
    (void)out_user_id;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_get_login_record(
    lan_chat_storage_t *storage,
    const char *username,
    lan_chat_login_record_t *out_record)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_STMT *stmt;
    MYSQL_BIND params[1];
    MYSQL_BIND result[6];
    unsigned long username_param_len;
    unsigned long username_len = 0;
    unsigned long nickname_len = 0;
    unsigned long hash_len = 0;
    unsigned long salt_len = 0;
    uint8_t enabled = 0;
    bool is_null[6] = {false, false, false, false, false, false};
    int fetch_result;
    lan_chat_status_t status = LAN_CHAT_STATUS_OK;
    const char *sql =
        "SELECT user_id, username, nickname, password_hash, password_salt, enabled "
        "FROM accounts WHERE username = ?";

    if (mysql_storage == 0 || out_record == 0 || !string_is_valid(username, LAN_CHAT_MAX_USERNAME_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    memset(out_record, 0, sizeof(*out_record));

    stmt = mysql_stmt_init(mysql_storage->mysql);
    if (stmt == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }
    username_param_len = (unsigned long)bounded_strlen(username, LAN_CHAT_MAX_USERNAME_LEN);
    bind_string(&params[0], username, &username_param_len);
    bind_u64(&result[0], &out_record->user_id);
    result[0].is_null = &is_null[0];
    bind_string_result(&result[1], out_record->username, sizeof(out_record->username), &username_len, &is_null[1]);
    bind_string_result(&result[2], out_record->nickname, sizeof(out_record->nickname), &nickname_len, &is_null[2]);
    bind_string_result(&result[3], out_record->password_hash, sizeof(out_record->password_hash), &hash_len, &is_null[3]);
    bind_string_result(&result[4], out_record->password_salt, sizeof(out_record->password_salt), &salt_len, &is_null[4]);
    memset(&result[5], 0, sizeof(result[5]));
    result[5].buffer_type = MYSQL_TYPE_TINY;
    result[5].buffer = &enabled;
    result[5].is_unsigned = true;
    result[5].is_null = &is_null[5];

    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_bind_result(stmt, result) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }

    fetch_result = mysql_stmt_fetch(stmt);
    if (fetch_result == MYSQL_NO_DATA) {
        status = LAN_CHAT_STATUS_NOT_FOUND;
    } else if (fetch_result != 0 && fetch_result != MYSQL_DATA_TRUNCATED) {
        status = stmt_status(stmt);
    } else {
        out_record->username[sizeof(out_record->username) - 1] = '\0';
        out_record->nickname[sizeof(out_record->nickname) - 1] = '\0';
        out_record->password_hash[sizeof(out_record->password_hash) - 1] = '\0';
        out_record->password_salt[sizeof(out_record->password_salt) - 1] = '\0';
        out_record->enabled = enabled;
    }
    mysql_stmt_close(stmt);
    return status;
#else
    (void)storage;
    (void)username;
    (void)out_record;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_update_last_login(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t user_id,
    uint64_t login_time_ms)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_BIND params[2];
    uint64_t time_ms = login_time_ms;
    uint64_t uid = user_id;
    lan_chat_status_t status;
    const char *sql = "UPDATE accounts SET last_login_at = FROM_UNIXTIME(? / 1000.0) WHERE user_id = ?";

    if (mysql_storage == 0 || user_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    status = ensure_user_enabled(mysql_storage->mysql, user_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    bind_u64(&params[0], &time_ms);
    bind_u64(&params[1], &uid);
    return execute_statement(mysql_storage->mysql, sql, params);
#else
    (void)storage;
    (void)user_id;
    (void)login_time_ms;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_list_users(
    lan_chat_storage_t *storage,
    lan_chat_user_record_t *out_users,
    size_t users_capacity,
    size_t *out_user_count)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_STMT *stmt;
    MYSQL_BIND result[3];
    lan_chat_user_record_t row;
    unsigned long username_len = 0;
    unsigned long nickname_len = 0;
    bool is_null[3] = {false, false, false};
    int fetch_result;
    size_t count = 0;
    lan_chat_status_t status = LAN_CHAT_STATUS_OK;
    const char *sql = "SELECT user_id, username, nickname FROM accounts WHERE enabled = 1 ORDER BY user_id ASC";

    if (mysql_storage == 0 || out_user_count == 0 || (out_users == 0 && users_capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_user_count = 0;
    stmt = mysql_stmt_init(mysql_storage->mysql);
    if (stmt == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }
    memset(&row, 0, sizeof(row));
    bind_u64(&result[0], &row.user_id);
    result[0].is_null = &is_null[0];
    bind_string_result(&result[1], row.username, sizeof(row.username), &username_len, &is_null[1]);
    bind_string_result(&result[2], row.nickname, sizeof(row.nickname), &nickname_len, &is_null[2]);
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0 ||
        mysql_stmt_bind_result(stmt, result) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }

    while ((fetch_result = mysql_stmt_fetch(stmt)) == 0 || fetch_result == MYSQL_DATA_TRUNCATED) {
        if (count < users_capacity) {
            row.username[sizeof(row.username) - 1] = '\0';
            row.nickname[sizeof(row.nickname) - 1] = '\0';
            row.online = 0;
            out_users[count] = row;
        }
        ++count;
        memset(&row, 0, sizeof(row));
        username_len = 0;
        nickname_len = 0;
    }
    if (fetch_result != MYSQL_NO_DATA) {
        status = stmt_status(stmt);
    }
    mysql_stmt_close(stmt);
    *out_user_count = count;
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return count > users_capacity ? LAN_CHAT_STATUS_BUFFER_TOO_SMALL : LAN_CHAT_STATUS_OK;
#else
    (void)storage;
    (void)out_users;
    (void)users_capacity;
    (void)out_user_count;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_store_private_message(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id,
    lan_chat_delivery_id_t *out_delivery_id)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    lan_chat_conversation_id_t conversation_id = 0;
    lan_chat_status_t status;

    if (mysql_storage == 0 || message == 0 || out_message_id == 0 || out_delivery_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_message_id = 0;
    *out_delivery_id = 0;
    if (message->sender_id == 0 || message->receiver_id == 0 || message->sender_id == message->receiver_id ||
        message->content_type == 0 ||
        !string_is_valid(message->content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    status = ensure_user_enabled(mysql_storage->mysql, message->sender_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = ensure_user_enabled(mysql_storage->mysql, message->receiver_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    status = begin_transaction(mysql_storage);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = get_or_create_conversation(mysql_storage, message, &conversation_id);
    if (status == LAN_CHAT_STATUS_OK) {
        status = insert_message(mysql_storage, message, conversation_id, out_message_id);
    }
    if (status == LAN_CHAT_STATUS_OK) {
        status = insert_delivery(mysql_storage, *out_message_id, message->receiver_id, out_delivery_id);
    }
    if (status != LAN_CHAT_STATUS_OK) {
        rollback_transaction(mysql_storage);
        *out_message_id = 0;
        *out_delivery_id = 0;
        return status;
    }
    status = commit_transaction(mysql_storage);
    if (status != LAN_CHAT_STATUS_OK) {
        rollback_transaction(mysql_storage);
        *out_message_id = 0;
        *out_delivery_id = 0;
    }
    return status;
#else
    (void)storage;
    (void)message;
    (void)out_message_id;
    (void)out_delivery_id;
    return mysql_not_implemented();
#endif
}

static int mysql_member_seen(const lan_chat_user_id_t *ids, size_t count, lan_chat_user_id_t user_id)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        if (ids[i] == user_id) {
            return 1;
        }
    }
    return 0;
}

static lan_chat_status_t mysql_create_group(
    lan_chat_storage_t *storage,
    const lan_chat_group_create_t *group,
    lan_chat_conversation_id_t *out_conversation_id)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    lan_chat_user_id_t unique_members[LAN_CHAT_MAX_GROUP_MEMBERS];
    size_t unique_count = 0;
    size_t i;
    lan_chat_status_t status;

    if (mysql_storage == 0 || group == 0 || out_conversation_id == 0 ||
        group->creator_id == 0 || group->member_ids == 0 || group->member_count == 0 ||
        group->member_count > LAN_CHAT_MAX_GROUP_MEMBERS ||
        !string_is_valid(group->name, LAN_CHAT_MAX_GROUP_NAME_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_conversation_id = 0;
    status = ensure_user_enabled(mysql_storage->mysql, group->creator_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    unique_members[unique_count++] = group->creator_id;
    for (i = 0; i < group->member_count; ++i) {
        if (group->member_ids[i] == 0) {
            return LAN_CHAT_STATUS_INVALID_ARGUMENT;
        }
        status = ensure_user_enabled(mysql_storage->mysql, group->member_ids[i]);
        if (status != LAN_CHAT_STATUS_OK) {
            return status;
        }
        if (!mysql_member_seen(unique_members, unique_count, group->member_ids[i])) {
            if (unique_count >= LAN_CHAT_MAX_GROUP_MEMBERS) {
                return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
            }
            unique_members[unique_count++] = group->member_ids[i];
        }
    }
    if (unique_count < 2) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    status = begin_transaction(mysql_storage);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = insert_group_conversation(mysql_storage->mysql, group->name, out_conversation_id);
    for (i = 0; status == LAN_CHAT_STATUS_OK && i < unique_count; ++i) {
        status = ensure_conversation_member(mysql_storage->mysql, *out_conversation_id, unique_members[i]);
    }
    if (status != LAN_CHAT_STATUS_OK) {
        rollback_transaction(mysql_storage);
        *out_conversation_id = 0;
        return status;
    }
    status = commit_transaction(mysql_storage);
    if (status != LAN_CHAT_STATUS_OK) {
        rollback_transaction(mysql_storage);
        *out_conversation_id = 0;
    }
    return status;
#else
    (void)storage;
    (void)group;
    (void)out_conversation_id;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_list_group_members(
    lan_chat_storage_t *storage,
    lan_chat_conversation_id_t conversation_id,
    lan_chat_group_member_record_t *out_members,
    size_t members_capacity,
    size_t *out_member_count)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_STMT *stmt;
    MYSQL_BIND params[1];
    MYSQL_BIND result[1];
    uint64_t cid = conversation_id;
    lan_chat_group_member_record_t row;
    bool is_null = false;
    int fetch_result;
    size_t count = 0;
    lan_chat_status_t status = LAN_CHAT_STATUS_OK;
    const char *sql =
        "SELECT cm.user_id "
        "FROM conversation_members cm "
        "JOIN conversations c ON c.conversation_id = cm.conversation_id "
        "WHERE cm.conversation_id = ? AND cm.active = 1 AND c.conversation_type = 'group' "
        "ORDER BY cm.user_id ASC";

    if (mysql_storage == 0 || conversation_id == 0 || out_member_count == 0 ||
        (out_members == 0 && members_capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_member_count = 0;
    stmt = mysql_stmt_init(mysql_storage->mysql);
    if (stmt == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }
    bind_u64(&params[0], &cid);
    memset(&row, 0, sizeof(row));
    bind_u64(&result[0], &row.user_id);
    result[0].is_null = &is_null;
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_bind_result(stmt, result) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }
    while ((fetch_result = mysql_stmt_fetch(stmt)) == 0 || fetch_result == MYSQL_DATA_TRUNCATED) {
        if (count < members_capacity) {
            out_members[count] = row;
        }
        ++count;
        memset(&row, 0, sizeof(row));
    }
    status = fetch_result == MYSQL_NO_DATA ? LAN_CHAT_STATUS_OK : stmt_status(stmt);
    mysql_stmt_close(stmt);
    *out_member_count = count;
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (count == 0) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    return count > members_capacity ? LAN_CHAT_STATUS_BUFFER_TOO_SMALL : LAN_CHAT_STATUS_OK;
#else
    (void)storage;
    (void)conversation_id;
    (void)out_members;
    (void)members_capacity;
    (void)out_member_count;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_store_group_message(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    lan_chat_status_t status;

    if (mysql_storage == 0 || message == 0 || out_message_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_message_id = 0;
    if (message->conversation_id == 0 || message->sender_id == 0 || message->content_type == 0 ||
        !string_is_valid(message->content, LAN_CHAT_MAX_MESSAGE_TEXT_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    status = validate_group_member(mysql_storage->mysql, message->conversation_id, message->sender_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return insert_group_message(mysql_storage, message, out_message_id);
#else
    (void)storage;
    (void)message;
    (void)out_message_id;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_store_file_transfer(
    lan_chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    const char *file_name,
    uint64_t file_size,
    uint32_t crc32,
    lan_chat_message_id_t *out_message_id,
    lan_chat_delivery_id_t *out_delivery_id,
    uint64_t *out_file_transfer_id)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    lan_chat_message_record_t file_message;
    MYSQL_BIND params[6];
    uint64_t mid;
    uint64_t sid;
    uint64_t rid;
    unsigned long file_name_len;
    uint64_t fsize = file_size;
    uint32_t fcrc = crc32;
    lan_chat_status_t status;
    const char *sql =
        "INSERT INTO file_transfers "
        "(message_id, sender_id, receiver_id, file_name, file_size, crc32, transfer_state) "
        "VALUES (?, ?, ?, ?, ?, ?, 'started')";

    if (mysql_storage == 0 || message == 0 || file_name == 0 || out_message_id == 0 ||
        out_delivery_id == 0 || out_file_transfer_id == 0 ||
        file_size == 0 || file_size > LAN_CHAT_MAX_FILE_SIZE ||
        !string_is_valid(file_name, LAN_CHAT_MAX_FILE_NAME_LEN, 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_message_id = 0;
    *out_delivery_id = 0;
    *out_file_transfer_id = 0;
    file_message = *message;
    file_message.content_type = LAN_CHAT_CONTENT_FILE;

    status = begin_transaction(mysql_storage);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    status = get_or_create_conversation(mysql_storage, &file_message, &file_message.conversation_id);
    if (status == LAN_CHAT_STATUS_OK) {
        status = insert_message(mysql_storage, &file_message, file_message.conversation_id, out_message_id);
    }
    if (status == LAN_CHAT_STATUS_OK) {
        status = insert_delivery(mysql_storage, *out_message_id, file_message.receiver_id, out_delivery_id);
    }
    if (status == LAN_CHAT_STATUS_OK) {
        mid = *out_message_id;
        sid = file_message.sender_id;
        rid = file_message.receiver_id;
        file_name_len = (unsigned long)bounded_strlen(file_name, LAN_CHAT_MAX_FILE_NAME_LEN);
        bind_u64(&params[0], &mid);
        bind_u64(&params[1], &sid);
        bind_u64(&params[2], &rid);
        bind_string(&params[3], file_name, &file_name_len);
        bind_u64(&params[4], &fsize);
        bind_u32(&params[5], &fcrc);
        status = execute_statement(mysql_storage->mysql, sql, params);
    }
    if (status == LAN_CHAT_STATUS_OK) {
        *out_file_transfer_id = mysql_insert_id(mysql_storage->mysql);
        status = commit_transaction(mysql_storage);
    }
    if (status != LAN_CHAT_STATUS_OK) {
        rollback_transaction(mysql_storage);
        *out_message_id = 0;
        *out_delivery_id = 0;
        *out_file_transfer_id = 0;
    }
    return status;
#else
    (void)storage;
    (void)message;
    (void)file_name;
    (void)file_size;
    (void)crc32;
    (void)out_message_id;
    (void)out_delivery_id;
    (void)out_file_transfer_id;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_complete_file_transfer(
    lan_chat_storage_t *storage,
    uint64_t file_transfer_id)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_BIND params[1];
    uint64_t fid = file_transfer_id;
    const char *sql =
        "UPDATE file_transfers "
        "SET transfer_state = 'completed', completed_at = CURRENT_TIMESTAMP "
        "WHERE file_transfer_id = ?";

    if (mysql_storage == 0 || file_transfer_id == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    bind_u64(&params[0], &fid);
    return execute_statement(mysql_storage->mysql, sql, params);
#else
    (void)storage;
    (void)file_transfer_id;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_list_history(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b,
    uint64_t before_message_id,
    size_t limit,
    lan_chat_message_record_t *out_messages,
    size_t messages_capacity,
    size_t *out_message_count)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_STMT *stmt;
    MYSQL_BIND params[4];
    MYSQL_BIND result[8];
    lan_chat_conversation_id_t conversation_id = 0;
    int found = 0;
    uint64_t before_id = before_message_id == 0 ? UINT64_MAX : before_message_id;
    uint64_t query_limit = limit;
    lan_chat_message_record_t row;
    unsigned long content_len = 0;
    bool is_null[8] = {false, false, false, false, false, false, false, false};
    int fetch_result;
    size_t count = 0;
    lan_chat_status_t status;
    const char *sql =
        "SELECT message_id, conversation_id, sender_id, receiver_id, content_type, content, client_message_id, "
        "CAST(UNIX_TIMESTAMP(store_time) * 1000 AS UNSIGNED) "
        "FROM messages "
        "WHERE conversation_id = ? AND message_id < ? "
        "ORDER BY message_id DESC LIMIT ?";

    if (mysql_storage == 0 || out_message_count == 0 || user_a == 0 || user_b == 0 || user_a == user_b ||
        (out_messages == 0 && messages_capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_message_count = 0;
    if (limit == 0) {
        return LAN_CHAT_STATUS_OK;
    }
    status = find_private_conversation(mysql_storage->mysql, user_a, user_b, &conversation_id, &found);
    if (status != LAN_CHAT_STATUS_OK || !found) {
        return status;
    }

    stmt = mysql_stmt_init(mysql_storage->mysql);
    if (stmt == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }
    bind_u64(&params[0], &conversation_id);
    bind_u64(&params[1], &before_id);
    bind_u64(&params[2], &query_limit);
    memset(&params[3], 0, sizeof(params[3]));
    memset(&row, 0, sizeof(row));
    bind_u64(&result[0], &row.message_id);
    result[0].is_null = &is_null[0];
    bind_u64(&result[1], &row.conversation_id);
    result[1].is_null = &is_null[1];
    bind_u64(&result[2], &row.sender_id);
    result[2].is_null = &is_null[2];
    bind_u64(&result[3], &row.receiver_id);
    result[3].is_null = &is_null[3];
    bind_u16(&result[4], &row.content_type);
    result[4].is_null = &is_null[4];
    bind_string_result(&result[5], row.content, sizeof(row.content), &content_len, &is_null[5]);
    bind_u64(&result[6], &row.client_message_id);
    result[6].is_null = &is_null[6];
    bind_u64(&result[7], &row.store_time_ms);
    result[7].is_null = &is_null[7];
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_bind_result(stmt, result) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }

    while ((fetch_result = mysql_stmt_fetch(stmt)) == 0 || fetch_result == MYSQL_DATA_TRUNCATED) {
        if (count < messages_capacity) {
            row.content[sizeof(row.content) - 1] = '\0';
            out_messages[count] = row;
        }
        ++count;
        memset(&row, 0, sizeof(row));
        content_len = 0;
    }
    status = fetch_result == MYSQL_NO_DATA ? LAN_CHAT_STATUS_OK : stmt_status(stmt);
    mysql_stmt_close(stmt);
    *out_message_count = count;
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return count > messages_capacity ? LAN_CHAT_STATUS_BUFFER_TOO_SMALL : LAN_CHAT_STATUS_OK;
#else
    (void)storage;
    (void)user_a;
    (void)user_b;
    (void)before_message_id;
    (void)limit;
    (void)out_messages;
    (void)messages_capacity;
    (void)out_message_count;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_list_pending_deliveries(
    lan_chat_storage_t *storage,
    lan_chat_user_id_t receiver_id,
    size_t limit,
    lan_chat_delivery_record_t *out_deliveries,
    size_t deliveries_capacity,
    size_t *out_delivery_count)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_STMT *stmt;
    MYSQL_BIND params[2];
    MYSQL_BIND result[9];
    uint64_t rid = receiver_id;
    uint64_t query_limit = limit;
    lan_chat_delivery_record_t row;
    unsigned long content_len = 0;
    bool is_null[9] = {false, false, false, false, false, false, false, false, false};
    int fetch_result;
    size_t count = 0;
    lan_chat_status_t status;
    const char *sql =
        "SELECT d.delivery_id, m.message_id, m.conversation_id, m.sender_id, m.receiver_id, "
        "m.content_type, m.content, m.client_message_id, CAST(UNIX_TIMESTAMP(m.store_time) * 1000 AS UNSIGNED) "
        "FROM message_deliveries d "
        "JOIN messages m ON m.message_id = d.message_id "
        "WHERE d.receiver_id = ? AND d.delivery_state = 'pending' "
        "ORDER BY d.delivery_id ASC LIMIT ?";

    if (mysql_storage == 0 || out_delivery_count == 0 || receiver_id == 0 ||
        (out_deliveries == 0 && deliveries_capacity > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_delivery_count = 0;
    if (limit == 0) {
        return LAN_CHAT_STATUS_OK;
    }
    status = ensure_user_enabled(mysql_storage->mysql, receiver_id);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    stmt = mysql_stmt_init(mysql_storage->mysql);
    if (stmt == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }
    bind_u64(&params[0], &rid);
    bind_u64(&params[1], &query_limit);
    memset(&row, 0, sizeof(row));
    bind_u64(&result[0], &row.delivery_id);
    result[0].is_null = &is_null[0];
    bind_u64(&result[1], &row.message.message_id);
    result[1].is_null = &is_null[1];
    bind_u64(&result[2], &row.message.conversation_id);
    result[2].is_null = &is_null[2];
    bind_u64(&result[3], &row.message.sender_id);
    result[3].is_null = &is_null[3];
    bind_u64(&result[4], &row.message.receiver_id);
    result[4].is_null = &is_null[4];
    bind_u16(&result[5], &row.message.content_type);
    result[5].is_null = &is_null[5];
    bind_string_result(&result[6], row.message.content, sizeof(row.message.content), &content_len, &is_null[6]);
    bind_u64(&result[7], &row.message.client_message_id);
    result[7].is_null = &is_null[7];
    bind_u64(&result[8], &row.message.store_time_ms);
    result[8].is_null = &is_null[8];
    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0 ||
        mysql_stmt_bind_param(stmt, params) != 0 ||
        mysql_stmt_bind_result(stmt, result) != 0 ||
        mysql_stmt_execute(stmt) != 0) {
        status = stmt_status(stmt);
        mysql_stmt_close(stmt);
        return status;
    }

    while ((fetch_result = mysql_stmt_fetch(stmt)) == 0 || fetch_result == MYSQL_DATA_TRUNCATED) {
        if (count < deliveries_capacity) {
            row.message.content[sizeof(row.message.content) - 1] = '\0';
            row.delivery_state = LAN_CHAT_DELIVERY_PENDING;
            out_deliveries[count] = row;
        }
        ++count;
        memset(&row, 0, sizeof(row));
        content_len = 0;
    }
    status = fetch_result == MYSQL_NO_DATA ? LAN_CHAT_STATUS_OK : stmt_status(stmt);
    mysql_stmt_close(stmt);
    *out_delivery_count = count;
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return count > deliveries_capacity ? LAN_CHAT_STATUS_BUFFER_TOO_SMALL : LAN_CHAT_STATUS_OK;
#else
    (void)storage;
    (void)receiver_id;
    (void)limit;
    (void)out_deliveries;
    (void)deliveries_capacity;
    (void)out_delivery_count;
    return mysql_not_implemented();
#endif
}

static lan_chat_status_t mysql_mark_delivery_state(
    lan_chat_storage_t *storage,
    lan_chat_delivery_id_t delivery_id,
    uint16_t delivery_state,
    uint64_t state_time_ms)
{
#if LAN_CHAT_HAS_MYSQL
    lan_chat_mysql_storage_t *mysql_storage = (lan_chat_mysql_storage_t *)storage;
    MYSQL_BIND params[2];
    uint64_t did = delivery_id;
    uint64_t time_ms = state_time_ms;
    uint64_t current_state_num = 0;
    int found = 0;
    lan_chat_status_t status;
    const char *select_sql =
        "SELECT CASE delivery_state "
        "WHEN 'pending' THEN 1 WHEN 'delivered' THEN 2 WHEN 'acked' THEN 3 ELSE 0 END "
        "FROM message_deliveries WHERE delivery_id = ?";
    const char *to_delivered_sql =
        "UPDATE message_deliveries SET delivery_state = 'delivered', delivered_at = FROM_UNIXTIME(? / 1000.0) "
        "WHERE delivery_id = ?";
    const char *to_acked_sql =
        "UPDATE message_deliveries SET delivery_state = 'acked', "
        "delivered_at = COALESCE(delivered_at, FROM_UNIXTIME(? / 1000.0)), "
        "acked_at = FROM_UNIXTIME(? / 1000.0) WHERE delivery_id = ?";
    MYSQL_BIND ack_params[3];

    if (mysql_storage == 0 || delivery_id == 0 ||
        (delivery_state != LAN_CHAT_DELIVERY_PENDING &&
         delivery_state != LAN_CHAT_DELIVERY_DELIVERED &&
         delivery_state != LAN_CHAT_DELIVERY_ACKED)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    bind_u64(&params[0], &did);
    status = mysql_scalar_u64(mysql_storage->mysql, select_sql, params, &current_state_num, &found);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    if (!found) {
        return LAN_CHAT_STATUS_NOT_FOUND;
    }
    if (current_state_num == delivery_state) {
        return LAN_CHAT_STATUS_OK;
    }
    if (!((current_state_num == LAN_CHAT_DELIVERY_PENDING &&
           (delivery_state == LAN_CHAT_DELIVERY_DELIVERED || delivery_state == LAN_CHAT_DELIVERY_ACKED)) ||
          (current_state_num == LAN_CHAT_DELIVERY_DELIVERED && delivery_state == LAN_CHAT_DELIVERY_ACKED))) {
        return LAN_CHAT_STATUS_INVALID_STATE;
    }

    if (delivery_state == LAN_CHAT_DELIVERY_DELIVERED) {
        bind_u64(&params[0], &time_ms);
        bind_u64(&params[1], &did);
        return execute_statement(mysql_storage->mysql, to_delivered_sql, params);
    }

    bind_u64(&ack_params[0], &time_ms);
    bind_u64(&ack_params[1], &time_ms);
    bind_u64(&ack_params[2], &did);
    return execute_statement(mysql_storage->mysql, to_acked_sql, ack_params);
#else
    (void)storage;
    (void)delivery_id;
    (void)delivery_state;
    (void)state_time_ms;
    return mysql_not_implemented();
#endif
}

static const lan_chat_storage_vtable_t k_mysql_vtable = {
    mysql_storage_close,
    mysql_create_account,
    mysql_get_login_record,
    mysql_update_last_login,
    mysql_list_users,
    mysql_store_private_message,
    mysql_create_group,
    mysql_list_group_members,
    mysql_store_group_message,
    mysql_store_file_transfer,
    mysql_complete_file_transfer,
    mysql_list_history,
    mysql_list_pending_deliveries,
    mysql_mark_delivery_state
};

lan_chat_status_t lan_chat_storage_mysql_open(
    const lan_chat_storage_config_t *config,
    lan_chat_storage_t **out_storage)
{
    lan_chat_mysql_storage_t *mysql_storage;
#if LAN_CHAT_HAS_MYSQL
    const char *host;
    unsigned int port;
    const char *password;
#endif

    if (config == 0 || out_storage == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_storage = 0;
    if (config->database == 0 || config->database[0] == '\0' ||
        config->user == 0 || config->user[0] == '\0') {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

#if !LAN_CHAT_HAS_MYSQL
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
#else
    mysql_storage = (lan_chat_mysql_storage_t *)calloc(1, sizeof(*mysql_storage));
    if (mysql_storage == 0) {
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }

    mysql_storage->base.vtable = &k_mysql_vtable;
    mysql_storage->config = *config;
    mysql_storage->mysql = mysql_init(0);
    if (mysql_storage->mysql == 0) {
        free(mysql_storage);
        return LAN_CHAT_STATUS_OUT_OF_MEMORY;
    }

    host = config->host != 0 && config->host[0] != '\0' ? config->host : "127.0.0.1";
    port = config->port != 0 ? config->port : 3306;
    password = config->password != 0 ? config->password : "";
    if (mysql_real_connect(
            mysql_storage->mysql,
            host,
            config->user,
            password,
            config->database,
            port,
            0,
            0) == 0) {
        mysql_close(mysql_storage->mysql);
        free(mysql_storage);
        return LAN_CHAT_STATUS_STORAGE_ERROR;
    }
    (void)mysql_set_character_set(mysql_storage->mysql, "utf8mb4");
    *out_storage = &mysql_storage->base;
    return LAN_CHAT_STATUS_OK;
#endif
}
