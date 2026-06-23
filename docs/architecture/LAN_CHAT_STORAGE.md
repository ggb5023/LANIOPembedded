# LAN 聊天 Phase 4 Storage 架构文档

## 1. 文档范围

本文档固定 Phase 4 Storage 架构。Phase 4 的目标是把 `lan_chat_storage_c` 从接口骨架推进到可验证的账号、消息、历史和投递状态闭环：

```text
server handler / future auth
  -> lan_chat_storage_c
    -> lan_chat_storage_memory_c  (unit test / smoke)
    -> lan_chat_storage_mysql_c   (v1 production persistence)
```

Phase 4 不改变总体架构，不引入联系人/好友系统，不创建 `offline_messages` 或 `client_acks` 独立表。v1 schema 固定为：

- `accounts`
- `conversations`
- `conversation_members`
- `messages`
- `message_deliveries`

`user_id`、`conversation_id`、`message_id`、`delivery_id` 均使用 `uint64_t` / MySQL `BIGINT UNSIGNED`。

## 2. 模块边界

### 2.1 `lan_chat_storage_c`

职责：

- 定义 storage 抽象接口和公共数据结构。
- 通过 vtable 分发到 memory 或 MySQL backend。
- 不包含 MySQL header。
- 不生成或校验密码 hash。

约束：

- server handler 只依赖 `lan_chat_storage_c`。
- 接口返回 `lan_chat_status_t`。
- 查询输出使用调用方提供的数组和容量。
- 查询容量不足时返回 `LAN_CHAT_STATUS_BUFFER_TOO_SMALL`，并通过 `out_*_count` 返回所需记录数。

### 2.2 `lan_chat_storage_memory_c`

职责：

- 提供完整 Phase 4 storage 行为，用于单元测试、server smoke 和无数据库 CLI E2E。
- 使用进程内内存保存 accounts、private conversations、messages、message_deliveries。
- 行为必须与 MySQL backend 的外部语义一致。

约束：

- 不作为生产默认实现。
- 不做持久化。
- 不保存明文密码。
- 默认单线程使用；并发访问由调用方加锁或后续 server I/O loop 串行化。

### 2.3 `lan_chat_storage_mysql_c`

职责：

- 使用 MySQL C API / libmysql 实现 `lan_chat_storage_c`。
- 只在该模块内包含 `mysql.h`。
- 管理 `MYSQL*` 连接生命周期。
- 使用事务保存 message + delivery。

约束：

- `MYSQL*` 不跨线程共享。
- C++ wrapper 不包含 `mysql.h`。
- `lan_chat_server_c` 不链接 `lan_chat_storage_mysql_c`。
- `chat_server` 应用层负责选择并注入 MySQL storage。

## 3. 密码与账号边界

storage 只保存和返回：

- `password_hash`
- `password_salt`
- `enabled`

禁止保存：

- 明文密码
- 可逆加密密码
- 测试种子中的 `password_plain`

auth 层负责：

- salt 生成
- password hash 生成
- 登录密码校验

storage 层负责：

- 根据 `username` 创建账号。
- 根据 `username` 返回登录校验所需记录。
- 维护 `last_login_at` / `last_login_ms`。

## 4. 固定 C API

Phase 4 以 `include/lan_chat/storage/storage.h` 中的实际 API 为准：

```c
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
```

## 5. API 语义

### 5.1 Account

- `username` 必须非空，长度不超过 `LAN_CHAT_MAX_USERNAME_LEN`。
- `nickname` 可为空；为空时 storage 使用 `username` 作为默认 nickname。
- `password_hash` 必须非空，长度不超过 `LAN_CHAT_PASSWORD_HASH_LEN`。
- `password_salt` 必须非空，长度不超过 `LAN_CHAT_PASSWORD_SALT_LEN`。
- 重复 `username` 返回 `LAN_CHAT_STATUS_ALREADY_EXISTS`。
- 未找到账号返回 `LAN_CHAT_STATUS_NOT_FOUND`。
- 新账号默认 `enabled = 1`。

### 5.2 User List

- `lan_chat_storage_list_users()` 返回 enabled account 列表。
- 返回顺序按 `user_id` 升序。
- `online` 字段由 server session registry 在业务层覆盖；storage backend 默认返回 0。

### 5.3 Private Message

- `sender_id` 和 `receiver_id` 必须是已存在且 enabled 的账号。
- `sender_id` 和 `receiver_id` 不允许相同。
- `content_type` 必须非 0。
- `content` 必须是以 `\0` 结尾的文本，长度不超过 `LAN_CHAT_MAX_MESSAGE_TEXT_LEN`。
- `conversation_id == 0` 时，storage 查找或创建这两个用户之间的 private conversation。
- `conversation_id != 0` 时，storage 必须校验该 conversation 存在且成员匹配。
- 写入消息时必须同时创建一条 `message_deliveries` 记录，初始状态为 `LAN_CHAT_DELIVERY_PENDING`。

### 5.4 History

- `before_message_id == 0` 表示从最新消息开始分页。
- `before_message_id != 0` 表示只返回 `message_id < before_message_id` 的消息。
- 返回顺序固定为 `message_id` 降序，即最新消息在前。
- `limit == 0` 返回 0 条。

### 5.5 Pending Delivery

- `lan_chat_storage_list_pending_deliveries()` 只返回指定 receiver 的 pending delivery。
- 返回顺序固定为 `delivery_id` 升序，即最早待投递消息在前。
- delivered 或 acked delivery 不再出现在 pending 查询中。

### 5.6 Delivery State

固定状态：

```c
LAN_CHAT_DELIVERY_PENDING   = 1
LAN_CHAT_DELIVERY_DELIVERED = 2
LAN_CHAT_DELIVERY_ACKED     = 3
```

状态更新必须单调：

- pending -> delivered
- delivered -> acked
- pending -> acked
- 重复设置当前状态视为成功
- acked 不允许回退到 delivered 或 pending

## 6. MySQL 实现要求

MySQL backend Phase 4 必须满足：

### 6.1 构建与依赖

- 优先使用 vcpkg `unofficial::libmysql::libmysql`。
- 如果未启用 vcpkg，CMake 可通过 `LAN_CHAT_MYSQL_ROOT`、`MYSQL_ROOT` 或本机默认安装路径探测 `mysql.h` 与 `libmysql.lib`。
- 找不到 libmysql 时，`lan_chat_storage_mysql_c` 仍保持可编译，但 `lan_chat_storage_mysql_open()` 返回 `LAN_CHAT_STATUS_NOT_IMPLEMENTED`。
- 找到 libmysql 时，`LAN_CHAT_HAS_MYSQL=1`，`lan_chat_storage_mysql_c` 必须编译真实 MySQL C API 实现。

### 6.2 连接配置

`lan_chat_storage_config_t` 字段固定含义：

- `host`：MySQL host，空值默认 `127.0.0.1`。
- `port`：MySQL port，0 默认 `3306`。
- `database`：数据库名，必须非空。
- `user`：用户名，必须非空。
- `password`：MySQL 登录密码，可为空字符串。
- `connection_pool_size`：Phase 4 不实现连接池，保留给后续 worker pool；当前每个 storage 实例持有一个 `MYSQL*`。

### 6.3 SQL 行为

- `create_account`
  - `INSERT accounts (...)`
  - duplicate username 映射为 `LAN_CHAT_STATUS_ALREADY_EXISTS`
- `get_login_record`
  - `SELECT user_id, username, nickname, password_hash, password_salt, enabled FROM accounts WHERE username = ?`
- `store_private_message`
  - 事务内完成 conversation 查找/创建、members upsert、message insert、delivery insert
- `list_history`
  - `WHERE conversation_id = ? AND message_id < ? ORDER BY message_id DESC LIMIT ?`
- `list_pending_deliveries`
  - join `message_deliveries` + `messages`
  - `WHERE receiver_id = ? AND delivery_state = 'pending' ORDER BY delivery_id ASC LIMIT ?`
- `mark_delivery_state`
  - 更新 `delivery_state`、`delivered_at`、`acked_at`
  - 不允许状态回退

### 6.4 Prepared Statement 规则

- 账号、消息、历史和投递查询必须使用 MySQL prepared statement。
- 事务控制语句可以使用 `mysql_query()`。
- 输入字符串长度必须来自 bounded length，不依赖未受控的 `strlen()`。
- `MYSQL_BIND` 不跨函数持久保存，statement 执行结束后必须 close。

### 6.5 事务规则

`store_private_message` 必须在单个事务内完成：

1. 查找 private conversation。
2. 未找到则插入 `conversations`。
3. 确保两个 `conversation_members` 存在。
4. 插入 `messages`。
5. 插入 `message_deliveries`。
6. 成功 commit，失败 rollback。

### 6.6 集成测试入口

- 普通单元测试只依赖 `lan_chat_storage_memory_c`，不连接 MySQL。
- MySQL 集成测试必须通过显式环境变量启用，避免本机服务未启动时阻塞常规构建。
- 建议环境变量：
  - `LAN_CHAT_MYSQL_TEST_HOST`
  - `LAN_CHAT_MYSQL_TEST_PORT`
  - `LAN_CHAT_MYSQL_TEST_DATABASE`
  - `LAN_CHAT_MYSQL_TEST_USER`
  - `LAN_CHAT_MYSQL_TEST_PASSWORD`
- 集成测试使用与 memory backend 相同的行为断言。

Phase 4 开发顺序固定为：

1. `lan_chat_storage_c` 接口和状态码定稿。
2. `lan_chat_storage_memory_c` 完整行为和单元测试。
3. `lan_chat_storage_mysql_c` 连接、prepared statement、事务和集成测试。

## 7. 验收检查

- memory storage 默认测试覆盖账号、私聊消息、历史分页、pending delivery 和 delivery 状态基础行为。
- MySQL storage 集成测试是显式可选项，不进入默认 CTest。
- memory backend 行为与本文档一致。
- MySQL backend 不向 public header 暴露 `MYSQL*`。
- schema 不包含 `contacts`、`offline_messages`、`client_acks`。
- schema 和测试数据不包含明文密码字段。
- 具体测试入口和运行方式见 `docs/architecture/LAN_CHAT_TESTING.md`。
