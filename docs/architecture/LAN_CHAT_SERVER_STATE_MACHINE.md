# LAN 聊天服务端业务状态机架构文档

## 1. 文档范围

本文档固定最小 register/login/chat E2E 阶段的服务端业务状态机。目标是把已完成的 Phase 2 protocol、Phase 3 TCP transport、Phase 4 storage 串成一个可运行闭环：

```text
chat_cli / E2E client
  -> TCP transport
    -> lan_chat_server_c I/O poll loop
      -> packet dispatcher
        -> auth/session/chat handlers
          -> lan_chat_storage_c
```

本阶段不改变总体架构，不实现 GUI、UDP discovery、联系人/好友、群聊、端到端加密或复杂 worker pool。服务端业务只依赖 `lan_chat_storage_c`，不直接依赖 MySQL API。

## 2. 模块边界

### 2.1 `lan_chat_server_c`

职责：

- 持有 TCP listener。
- 持有固定容量 client connection/session registry。
- 从 transport 收取完整 packet。
- 根据 header group/type 分发到业务 handler。
- 通过 storage 完成账号、登录、消息写入和 pending delivery 状态更新。
- 将 response / notify packet 交回 transport 发送。

约束：

- 不包含 `mysql.h`。
- 不保存明文密码。
- 不跨线程共享 socket handle。
- Phase 5 最小实现可以是单线程 poll loop。

### 2.2 `apps/chat_server`

职责：

- 创建 storage backend。
- 初始化并启动 `lan_chat_server_c`。
- 进入阻塞运行循环，直到进程退出。

### 2.3 `apps/chat_cli`

职责：

- 手动配置服务器地址。
- 发起 register/login/chat。
- 打印最小 E2E 结果。

Phase 5 CLI 可以先支持命令模式，不要求交互式 TUI。

## 3. 运行模型

Phase 5 最小运行模型：

- 单线程 poll loop。
- listener non-blocking accept。
- 每个 client connection non-blocking recv/flush。
- 固定连接容量，默认 16。
- 每轮 loop：
  - accept 新连接。
  - flush 每个连接的 send queue。
  - recv packet。
  - dispatch packet。
  - 关闭错误连接。

后续 worker pool 引入后，socket 仍由 I/O 层独占，worker 只返回 response/fanout 任务。

## 4. Session Registry

每个连接槽保存：

- connection id，使用槽位 index + 1。
- `lan_chat_tcp_connection_t`。
- authenticated flag。
- `user_id`。
- `session_id`。
- username。
- last activity timestamp。

规则：

- 未登录连接只能发送 AUTH register/login 和 heartbeat。
- 登录成功后分配 `session_id`。
- `session_id` 使用服务端单调递增 `uint64_t`。
- 单账号重复登录时，最小实现允许新连接覆盖在线 registry；旧连接后续可关闭。
- disconnect 时清理 user online registry。

## 5. 协议消息

### 5.1 Header

- register/login 请求：
  - group = `LAN_CHAT_GROUP_AUTH`
  - type = `LAN_CHAT_MSG_REQ`
  - flags = `LAN_CHAT_FLAG_REQUEST`
- register/login 响应：
  - group = `LAN_CHAT_GROUP_AUTH`
  - type = `LAN_CHAT_MSG_RSP`
  - flags = `LAN_CHAT_FLAG_RESPONSE`
- chat send 请求：
  - group = `LAN_CHAT_GROUP_CHAT`
  - type = `LAN_CHAT_MSG_REQ`
  - flags = `LAN_CHAT_FLAG_REQUEST`
- chat send 响应：
  - group = `LAN_CHAT_GROUP_CHAT`
  - type = `LAN_CHAT_MSG_RSP`
  - flags = `LAN_CHAT_FLAG_RESPONSE`
- inbound chat notify：
  - group = `LAN_CHAT_GROUP_CHAT`
  - type = `LAN_CHAT_MSG_NOTIFY`
  - flags = `LAN_CHAT_FLAG_NOTIFY`

### 5.2 Auth TLV

Auth request body 字段：

- field 1 required string：operation，固定 `register` 或 `login`。
- field 2 required string：username。
- field 3 required string：password。
- field 4 optional string：nickname，仅 register 使用。

Auth response body 字段：

- field 1 required u16：status code，使用 `lan_chat_status_t` 数值。
- field 2 optional u64：user_id。
- field 3 optional u64：session_id。
- field 4 optional string：message。

### 5.3 Chat TLV

Chat request body 字段：

- field 1 required u64：receiver_id。
- field 2 required string：content。
- field 3 optional u64：client_message_id。

Chat response body 字段：

- field 1 required u16：status code。
- field 2 optional u64：message_id。
- field 3 optional u64：delivery_id。
- field 4 optional string：message。

Chat notify body 字段：

- field 1 required u64：message_id。
- field 2 required u64：sender_id。
- field 3 required string：content。
- field 4 optional u64：delivery_id。

## 6. 密码策略

Phase 5 最小实现不保存明文密码：

- register 时服务端生成 salt。
- password_hash 使用最小 deterministic hash 占位实现，格式为 `v1-dev:<salt>:<hash>`。
- storage 只保存 `password_hash` 和 `password_salt`。
- login 时服务端根据 salt 重新计算 hash 并比较。

该 hash 仅用于开发闭环；后续必须替换为 PBKDF2/bcrypt/Argon2 等正式密码 KDF。

## 7. Handler 行为

### 7.1 Register

流程：

1. 解析 username/password/nickname。
2. 生成 salt/hash。
3. 调用 `lan_chat_storage_create_account()`。
4. 成功后创建登录 session。
5. 返回 user_id/session_id。

重复 username 返回 `LAN_CHAT_STATUS_ALREADY_EXISTS`。

### 7.2 Login

流程：

1. 解析 username/password。
2. 调用 `lan_chat_storage_get_login_record()`。
3. 校验 enabled 和 password hash。
4. 调用 `lan_chat_storage_update_last_login()`。
5. 创建登录 session。
6. 返回 user_id/session_id。

密码错误返回 `LAN_CHAT_STATUS_INVALID_ARGUMENT`。

### 7.3 Send Chat

流程：

1. 校验 connection 已登录。
2. 解析 receiver_id/content/client_message_id。
3. 调用 `lan_chat_storage_store_private_message()`。
4. 返回 message_id/delivery_id。
5. 若 receiver 在线，发送 chat notify。
6. notify 发送成功后可标记 delivered；最小实现允许保留 pending，由后续 ACK 阶段处理。

## 8. E2E 验收

最小 E2E 验收边界：

- server 使用 memory storage 启动。
- client A register 成功，得到 user_id/session_id。
- client B register 成功，得到 user_id/session_id。
- client A login 成功。
- client B login 成功。
- client A 向 B 发送 chat。
- server 写入 storage 并返回 message_id/delivery_id。
- client B 收到 chat notify，内容与 sender_id 正确。
- server shutdown 后 socket 关闭干净。

MySQL storage 已由 Phase 4 集成测试覆盖；本阶段 E2E 默认使用 memory storage，避免数据库服务状态影响普通测试。

具体默认测试入口和运行方式见 `docs/architecture/LAN_CHAT_TESTING.md`。
