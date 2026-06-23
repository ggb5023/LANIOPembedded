# LAN 聊天新工程架构文档

## 1. 项目定位

第一阶段目标是先跑通一套可以真实工作的 LAN 聊天工程：

```text
C 底层协议与传输能力
  -> C++ 安全封装
    -> 中心聊天服务器
      -> 多个局域网聊天客户端
```

第一阶段开发环境：

- Windows 11
- MSVC
- CMake
- vcpkg 依赖管理
- MySQL 作为默认服务端持久化方案

第一阶段产品形态：

- 一个中心聊天服务器。
- 多个聊天客户端连接服务器。
- 客户端之间不直接传输聊天消息，所有文本消息经服务端中转。
- v1 采用全员可聊模型：登录用户可以看到用户列表，并向任意用户发起私聊。
- v1 优先 CLI 客户端，后续再扩展 Qt GUI 和 Launcher。

第一阶段明确不做：

- IoT 设备控制。
- 嵌入式、RTOS、lwIP 适配。
- 公网穿透。
- STUN/TURN。
- WebRTC 音视频。
- 端到端加密。
- 群聊、群音视频。
- 好友申请、联系人审批、黑名单。

语言分工：

- C 负责底层协议、packet、TCP framer、transport primitive、固定缓冲区、错误码、服务端基础状态机和 MySQL C API 存储适配。
- C++ 负责 RAII、类型安全、业务 API、配置加载、服务端/客户端组合和应用层工具。

## 2. 参考工程与借鉴边界

### 2.1 C++ WebRTC 工程

参考路径：

```text
E:\ComputerSys_Train\code\WebRTC
```

可借鉴内容：

- CMake/vcpkg 工程组织。
- MySQL 持久化经验。
- 登录、注册、会话、消息历史、离线消息设计。
- 服务端 session registry 思路。
- 运行时配置、profiles、local config 分层。
- Runtime Launcher 的开发期控制台思路。
- LAN-only 产品边界。

不直接继承内容：

- Qt UI 实现。
- WebSocket + JSON 作为唯一协议形态。
- 旧工程的 MySQL X DevAPI / Connector C++ 数据库接口，新工程不采用。
- WebRTC、H.264、Opus、音视频链路。
- 当前项目中的具体业务类和 UI 状态模型。
- 旧工程的历史兼容字段。

新工程借鉴它的服务端能力边界和 MySQL schema 经验，但协议底层改为新的 C binary protocol，数据库接口改为 MySQL C API/libmysql。

### 2.2 C winsocket LAN 工程

参考路径：

```text
E:\ComputerSys_Train\winsocket\codex
```

可借鉴内容：

- C 二进制协议头设计。
- 固定大小缓冲区。
- 大端序编码/解码。
- 错误码枚举。
- packet encode/decode 分层。
- dispatcher 表驱动分发。
- protocol smoke tests。
- 注册、登录、用户、聊天、离线消息的消息类型划分。
- MySQL C API/libmysql 的接入思路。

必须重构或避免继承的内容：

- 当前 TCP 处理没有正确支持半包、粘包和一次收到多个包。
- 当前 UDP listener 仍是占位实现。
- 当前服务端主循环偏同步阻塞，一个连接可能拖住整个服务端。
- 当前 online manager 保存连接裸指针，不适合后续线程化或异步化。
- 当前内存 fallback 容量偏固定，只适合测试和 smoke。
- 当前 WebRTC/GStreamer 路径是未完成规划，不作为新工程依据。

新工程可以参考它的 C 协议层和 MySQL C API 接入方式，但不以整个工程作为代码基座直接复制。

### 2.3 主流工程参照

新工程可以参考以下主流模型：

- IRC：中心服务器、客户端连接、私聊消息中转模型。
- XMPP：登录、presence、用户状态、即时消息边界。
- Matrix：服务端持久化、历史消息、事件同步模型。
- mDNS/DNS-SD：局域网服务发现模型。

这些工程只作为架构参照，不要求 v1 直接实现对应完整协议。

## 3. 总体架构

目标架构分为应用组合层和库依赖层。应用负责选择实现，库只依赖抽象边界。

应用组合层：

```text
apps/chat_server
  -> lan_chat_cpp
  -> lan_chat_storage_mysql_c
     (注入到 lan_chat_server_c 的 storage interface)

apps/chat_cli
  -> lan_chat_cpp
```

库依赖层：

```text
lan_chat_cpp
  -> lan_chat_server_c
  -> lan_chat_transport_c
  -> lan_chat_storage_c
  -> lan_chat_core_c

lan_chat_server_c
  -> lan_chat_storage_c
  -> lan_chat_transport_c
  -> lan_chat_core_c

lan_chat_storage_mysql_c
  -> lan_chat_storage_c
  -> lan_chat_core_c

lan_chat_storage_memory_c
  -> lan_chat_storage_c
  -> lan_chat_core_c

lan_chat_storage_c
  -> lan_chat_core_c

lan_chat_transport_c
  -> lan_chat_platform_win32
  -> lan_chat_core_c

lan_chat_platform_win32
  -> lan_chat_core_c
```

核心原则：

- C Core 不依赖 MySQL、不依赖 vcpkg、不依赖 WinSock。
- WinSock 只允许出现在 Windows platform 层。
- MySQL C API/libmysql 只允许出现在 `lan_chat_storage_mysql_c`。
- `lan_chat_server_c` 只依赖 `lan_chat_storage_c` 接口，不链接 `lan_chat_storage_mysql_c`，不直接调用 `MYSQL*`。
- `chat_server` 应用层负责选择并注入 storage 实现，v1 默认注入 `lan_chat_storage_mysql_c`。
- `lan_chat_storage_memory_c` 只能用于测试和无数据库 smoke，不作为生产默认实现。
- C++ 层封装 C handle、错误和生命周期，但不改变底层 C API 的可用性。
- 第一阶段先保证 CLI 端到端聊天闭环，再扩展 GUI。

Phase 1 固定目录结构：

```text
WebRTC/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json

  docs/
    architecture/
      LAN_CHAT_ARCHITECTURE.md

  include/
    lan_chat/
      core/
      transport/
      server/
      storage/

  src/
    core/
    transport/
    platform/
      win32/
    server/
    storage/
      mysql/
      memory/

  cpp/
    include/
      lan_chat/
    src/

  apps/
    chat_server/
    chat_cli/

  db/
    mysql/

  config/
    defaults/
    local/
    profiles/

  tests/
    unit/
    integration/
```

Phase 1 固定 CMake target：

- `lan_chat_core_c`
- `lan_chat_platform_win32`
- `lan_chat_transport_c`
- `lan_chat_storage_c`
- `lan_chat_storage_mysql_c`
- `lan_chat_storage_memory_c`
- `lan_chat_server_c`
- `lan_chat_cpp`
- `chat_server`
- `chat_cli`

核心标识约定：

- 认证入口使用 `username` + password。
- `username` 只作为账号唯一登录名和注册输入，不作为协议路由、会话、消息或数据库外键。
- 登录成功后服务端返回 `user_id`，后续协议 header、服务端 session、消息和数据库外键统一使用服务端分配的数值 ID。
- `user_id`、`session_id`、`conversation_id`、`message_id`、`delivery_id` 均为 `uint64_t` / MySQL `BIGINT UNSIGNED`。

## 4. 模块职责

### 4.1 `lan_chat_core_c`

语言：

- C11

职责：

- 协议版本常量。
- 错误码。
- 消息组和消息类型。
- 固定二进制 header。
- TLV body 编码/解码。
- packet encode/decode。
- 大端序读写。
- ring buffer。
- TCP framer。
- 协议级状态机。

禁止依赖：

- WinSock。
- Windows headers。
- MySQL。
- SQLite。
- Qt。
- C++ 标准库。
- vcpkg 包。

设计要求：

- 所有 public API 使用 `lan_chat_status_t` 返回错误原因。
- 编解码函数使用调用方提供的 buffer。
- 不在协议编解码核心中隐藏动态内存分配。
- 所有最大长度集中定义。
- 所有 wire struct 尺寸必须有编译期断言。
- TCP framer 必须正确处理半包、粘包和多包连续输入。

### 4.2 `lan_chat_platform_win32`

语言：

- C11

职责：

- WinSock 初始化和关闭。
- TCP listen/connect/accept/send/recv。
- UDP bind/sendto/recvfrom。
- 时间、sleep、线程、mutex、condition variable。
- Windows 错误码转 `lan_chat_status_t`。

约束：

- 这是第一阶段唯一允许包含 WinSock/Windows networking headers 的 C 层。
- 上层通过 platform abstraction 调用，不直接使用 WinSock。

Phase 3 Windows TCP Transport 详细规格独立存放在 `docs/architecture/LAN_CHAT_TRANSPORT_WIN32.md`。本节只保留总体职责和边界，平台 API 形态、socket 生命周期和错误映射以该文档为准。

### 4.3 `lan_chat_transport_c`

语言：

- C11

职责：

- TCP server/client transport。
- TCP 读写缓冲。
- 发送队列。
- 连接状态。
- 心跳和超时。
- UDP LAN discovery。

第一阶段要求：

- TCP 是聊天协议主通道。
- UDP 只用于发现服务端。
- 必须允许客户端手动配置服务器地址，不能强依赖 UDP discovery。

Phase 3 Windows TCP Transport 详细规格独立存放在 `docs/architecture/LAN_CHAT_TRANSPORT_WIN32.md`。本节只保留总体职责和边界，TCP listener/client、send queue、recv framer、heartbeat smoke 和多客户端 smoke 以该文档为准。

### 4.4 `lan_chat_storage_c`

语言：

- C11

职责：

- 定义服务端存储抽象接口。
- 隐藏 MySQL/memory 具体实现。
- 统一账号、用户列表、消息、历史、投递状态相关操作。

核心接口形态：

```c
typedef struct chat_storage chat_storage_t;
typedef struct chat_storage_config chat_storage_config_t;
typedef uint64_t lan_chat_user_id_t;
typedef uint64_t lan_chat_conversation_id_t;
typedef uint64_t lan_chat_message_id_t;
typedef uint64_t lan_chat_delivery_id_t;

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

lan_chat_status_t chat_storage_open(
    chat_storage_t **out,
    const chat_storage_config_t *config);

void chat_storage_close(chat_storage_t *storage);

lan_chat_status_t chat_storage_create_account(
    chat_storage_t *storage,
    const lan_chat_account_create_t *account,
    lan_chat_user_id_t *out_user_id);

lan_chat_status_t chat_storage_get_login_record(
    chat_storage_t *storage,
    const char *username,
    lan_chat_login_record_t *out_record);

lan_chat_status_t chat_storage_update_last_login(
    chat_storage_t *storage,
    lan_chat_user_id_t user_id,
    uint64_t login_time_ms);

lan_chat_status_t chat_storage_list_users(
    chat_storage_t *storage,
    lan_chat_user_record_t *out_users,
    size_t users_capacity,
    size_t *out_user_count);

lan_chat_status_t chat_storage_store_private_message(
    chat_storage_t *storage,
    const lan_chat_message_record_t *message,
    lan_chat_message_id_t *out_message_id,
    lan_chat_delivery_id_t *out_delivery_id);

lan_chat_status_t chat_storage_list_history(
    chat_storage_t *storage,
    lan_chat_user_id_t user_a,
    lan_chat_user_id_t user_b,
    uint64_t before_message_id,
    size_t limit,
    lan_chat_message_record_t *out_messages,
    size_t messages_capacity,
    size_t *out_message_count);

lan_chat_status_t chat_storage_list_pending_deliveries(
    chat_storage_t *storage,
    lan_chat_user_id_t receiver_id,
    size_t limit,
    lan_chat_delivery_record_t *out_deliveries,
    size_t deliveries_capacity,
    size_t *out_delivery_count);

lan_chat_status_t chat_storage_mark_delivery_state(
    chat_storage_t *storage,
    lan_chat_delivery_id_t delivery_id,
    uint16_t delivery_state,
    uint64_t state_time_ms);
```

接口要求：

- server handler 只依赖该接口。
- 接口返回 `lan_chat_status_t`。
- 输出记录使用调用方提供的数组和容量。
- 大批量历史查询必须分页。
- auth 服务负责 password salt/hash 的生成与校验，storage 只保存和返回 `password_hash`、`password_salt`、账号状态。
- 禁止在 storage schema、配置、测试种子数据中存储明文密码。

Phase 4 Storage 详细规格独立存放在 `docs/architecture/LAN_CHAT_STORAGE.md`。本节只保留总体职责和接口方向，账号语义、消息写入、历史分页、pending delivery 和 delivery state 规则以该文档为准。

### 4.5 `lan_chat_storage_mysql_c`

语言：

- C11

职责：

- 使用 MySQL C API/libmysql 实现 `lan_chat_storage_c`。
- MySQL 连接管理。
- schema 初始化和迁移。
- 账号读写。
- 密码 hash/salt 存储。
- 用户列表查询。
- 会话记录。
- 消息存储。
- 待投递消息查询。
- delivered/acked 状态更新。

边界：

- MySQL 是 v1 默认持久化实现。
- MySQL 不进入 C Core。
- MySQL 不直接暴露给聊天业务 handler。
- `MYSQL*` 不能跨线程共享。
- 每个 worker 使用独立连接，或从连接池 checkout 独占连接。
- `lan_chat_storage_memory_c` 只用于测试和无数据库 smoke。
- SQLite 不作为 v1 默认方向，仅保留为未来轻量部署可选项。
- schema、迁移和测试数据禁止引入 `password_plain` 或其他明文密码字段。

### 4.6 `lan_chat_server_c`

语言：

- C11

职责：

- session registry。
- 登录态管理。
- 单账号在线会话管理。
- presence 广播。
- 用户列表查询。
- 私聊消息路由。
- 待投递消息队列。
- 历史消息查询。
- 心跳超时下线。
- dispatcher。

设计要求：

- 服务端业务只依赖 storage interface，不直接依赖 MySQL API。
- 会话对象使用稳定 handle/id 管理，避免保存临时连接裸指针。
- 所有业务 handler 经 dispatcher 注册。
- v1 全员可聊，不检查好友关系。

服务端业务状态机详细规格独立存放在 `docs/architecture/LAN_CHAT_SERVER_STATE_MACHINE.md`。Phase 5 应用层 E2E 规格独立存放在 `docs/architecture/LAN_CHAT_APP_E2E.md`。本节只保留总体职责，register/login/chat E2E 的 session registry、TLV 字段、handler 行为和验收以对应文档为准。

### 4.7 `lan_chat_cpp`

语言：

- C++20

职责：

- RAII 封装 C handle。
- `Result<T>` 错误处理。
- `Client`、`Server`、`Session`、`Message`、`Conversation` 等类型。
- 配置加载。
- 日志适配。
- 应用层生命周期组合。
- 封装 storage、client、server 的资源释放。

C++ API 形态：

```cpp
lan_chat::Result<lan_chat::Server> Server::start(const ServerConfig&);
lan_chat::Result<lan_chat::Client> Client::connect(const ClientConfig&);
lan_chat::Result<void> Client::sendText(uint64_t toUserId, std::string_view text);
lan_chat::Result<std::vector<Message>> Client::pullPendingDeliveries();
```

设计要求：

- 不让异常跨 C ABI 边界。
- C++ 析构必须释放 C 资源。
- C++ API 对调用者隐藏 C buffer 细节。
- 保留直接测试 C Core 的能力。

### 4.8 应用层

第一阶段应用：

- `apps/chat_server`
  - 启动聊天服务器。
  - 加载配置。
  - 初始化 MySQL storage。
  - 监听 TCP。
  - Phase 1 不依赖 UDP discovery，v1 后置实现时可开启 UDP discovery。
  - 启动 I/O loop 和 worker pool。

- `apps/chat_cli`
  - 注册。
  - 登录。
  - 查看用户列表。
  - 查看在线用户。
  - 发私聊消息。
  - 拉取待投递消息。
  - 查询历史消息。
  - 心跳保持在线。

后续应用：

- `apps/chat_launcher`
  - 借鉴旧 C++ 工程 Runtime Launcher。
  - 管理 server/client 启停、日志收集、LAN 检查。

- `apps/chat_qt`
  - Qt 桌面客户端。
  - 在 CLI 和底层稳定后再实现。

## 5. v1 功能范围

v1 必须实现：

- 注册。
- 登录。
- 登出。
- 在线状态同步。
- 用户列表。
- 在线用户列表。
- 全员可聊私聊文本消息。
- 服务端存储 ACK。
- 待投递消息拉取。
- 历史消息查询。
- 心跳。
- 超时下线。
- 断线重连后重新登录或恢复 session。
- 手动指定服务器地址。
- UDP LAN 服务发现作为 v1 后置可选能力，不作为 Phase 1 或 CLI E2E 验收阻塞项。

v1 不实现：

- 好友申请。
- 联系人添加/删除。
- 黑名单。
- 群聊。
- 文件传输。
- 头像。
- 音视频。
- 表情/贴纸。
- 消息撤回。
- 已读回执。
- 端到端加密。
- 多服务端联邦。

这些能力等 v1 私聊闭环稳定后再规划。好友/联系人关系作为 v2 扩展，不影响 v1 全员可聊模型。

## 6. 协议设计

完整协议规格独立存放在 `docs/architecture/LAN_CHAT_PROTOCOL.md`。本节只保留总体边界，Phase 2 实现 `lan_chat_core_c` 时以协议文档为准。

### 6.1 Transport

TCP：

- 承载所有核心聊天协议。
- 保证注册、登录、消息、ACK、待投递消息拉取和历史查询可靠。

UDP：

- 仅用于 LAN 服务发现。
- 客户端广播 discovery request。
- 服务端回复服务名、版本、TCP 地址和端口。
- 客户端仍必须支持手动输入服务器地址。
- Phase 1、Phase 2、Phase 3 和 CLI E2E 均不得依赖 UDP discovery。

### 6.2 Wire Format

协议采用：

```text
fixed binary header + TLV body
```

统一规则：

- 多字节整数字段使用大端序。
- header 定长。
- body 使用 TLV。
- 请求携带 `seq`。
- 服务端响应复用请求 `seq`。
- 所有错误使用 `ERROR` 响应。
- 协议字段保留 reserved 位，未协商时必须为 0。

header 标识规则：

- `session_id`、`sender_id`、`receiver_id` 均为 `uint64`。
- 未登录请求的 `sender_id` 为 0；登录成功后客户端必须使用服务端返回的 `user_id`。
- `username` 仅出现在注册、登录等 AUTH TLV body 中。
- 服务端路由私聊消息时只使用 `receiver_id`，不使用 username 字符串路由。

### 6.3 Message Groups

v1 消息组：

- `AUTH`
  - register req/rsp
  - login req/rsp
  - logout req/rsp

- `SESSION`
  - session resume req/rsp
  - session close notify

- `USER`
  - user list req/rsp
  - online user list req/rsp
  - user profile req/rsp

- `PRESENCE`
  - user online notify
  - user offline notify

- `CHAT`
  - chat send req/rsp
  - chat deliver notify
  - chat ack req/rsp

- `HISTORY`
  - history req/rsp
  - pending delivery pull req/rsp

- `HEARTBEAT`
  - heartbeat req/rsp

- `DISCOVERY`
  - discovery req/rsp

- `ERROR`
  - error rsp

v1 不包含 `CONTACT` 消息组。联系人、好友申请、好友审批放入 v2。

### 6.4 消息投递语义

在线私聊：

```text
A -> server: chat_send_req
server -> MySQL: store message + delivery row for B
server -> A: chat_send_rsp(state=stored)
server -> B: chat_deliver_notify
B -> server: chat_ack_req
server -> MySQL: mark delivery acked
server -> B: chat_ack_rsp
```

离线私聊：

```text
A -> server: chat_send_req
server -> MySQL: store message + pending delivery row for B
server -> A: chat_send_rsp(state=stored)
B login/resume
B -> server: pending_delivery_pull_req
server -> B: pending_delivery_pull_rsp(messages)
B -> server: chat_ack_req
server -> MySQL: mark delivery acked
```

服务端必须遵守：

- 消息先写入 MySQL，再向发送方返回 stored ACK。
- 在线接收方投递失败时，`message_deliveries` 保持 pending 状态。
- 客户端重连后通过 pending delivery pull 和 history query 恢复状态。
- 客户端 ACK 只确认接收方已收到，不代表已读。

## 7. MySQL 存储设计

MySQL 是 v1 默认服务端存储。schema 借鉴旧 C++ 工程经验，但不兼容旧表，避免继承历史兼容字段。

v1 核心表：

- `accounts`
  - user id，`BIGINT UNSIGNED`。
  - 用户账号。
  - 昵称。
  - 密码 hash。
  - 密码 salt。
  - enabled。
  - 创建时间。
  - 最后登录时间。

- `conversations`
  - conversation id，`BIGINT UNSIGNED`。
  - 会话类型，v1 固定为 private。
  - 创建时间。
  - 更新时间。

- `conversation_members`
  - conversation id。
  - user id，`BIGINT UNSIGNED`。
  - active。
  - joined time。
  - last_seen_message_id，可作为后续已读能力预留。

- `messages`
  - server message id，`BIGINT UNSIGNED`。
  - conversation id。
  - sender id。
  - receiver id。
  - content type。
  - content。
  - client message id。
  - send time。
  - store time。

- `message_deliveries`
  - delivery id，`BIGINT UNSIGNED`。
  - message id。
  - receiver id。
  - delivery state: pending, delivered, acked。
  - created time。
  - delivered time。
  - acked time。

设计说明：

- `message_deliveries` 统一表达离线待投递、已投递、客户端 ACK。
- 数据库外键统一使用 `BIGINT UNSIGNED` 数值 ID，不使用 username 字符串作消息、会话或投递关系外键。
- `accounts.username` 是唯一登录名，auth 根据 username 查询 hash/salt 后完成密码校验。
- v1 不创建 `contacts` 表。
- v1 不创建独立 `offline_messages` 表。
- v1 不创建独立 `client_acks` 表。
- schema 初始化脚本放在 `db/mysql/`。
- schema 迁移策略可以借鉴旧 C++ 工程，但新工程不承诺兼容旧库。

## 8. 服务端运行模型

v1 服务端采用：

```text
network I/O loop
  -> decoded packets
    -> worker queue
      -> protocol dispatcher
        -> server services
          -> storage interface
```

线程边界：

- TCP accept/read/write、framer、连接状态在 I/O 层。
- 协议 dispatch、登录、消息存储、历史查询在 worker 层。
- worker 不能直接持有短生命周期 socket 指针。
- worker 返回 response/fanout 任务，由 I/O 层发送。

MySQL 线程规则：

- 禁止多个线程共享同一个 `MYSQL*`。
- 每个 worker 使用独立 MySQL 连接，或从连接池 checkout 独占连接。
- MySQL 失败必须转换为 `lan_chat_status_t` 和协议 `ERROR` 响应。

单账号登录策略：

- v1 采用 kick-old 策略。
- 同一账号新登录成功后，旧 session 收到 session close notify 并下线。

## 9. vcpkg 策略

vcpkg 可以作为依赖管理工具，但不能污染 C Core。

允许 vcpkg 进入：

- C++ wrapper。
- 测试。
- CLI 工具。
- MySQL C API/libmysql。
- 日志。
- JSON 配置。
- 后续 Qt/GUI 辅助库。

禁止 vcpkg 进入：

- `lan_chat_core_c`。
- 协议编解码。
- packet。
- ring buffer。
- TCP framer。
- C 状态机核心。

Phase 1 固定 vcpkg manifest：

```json
{
  "name": "lan-chat",
  "version-string": "0.1.0",
  "dependencies": [
    "libmysql",
    "nlohmann-json",
    "fmt",
    "spdlog",
    "gtest"
  ]
}
```

Phase 1 固定 CMake 选项：

```cmake
option(LAN_CHAT_BUILD_TESTS "Build LAN chat tests" ON)
option(LAN_CHAT_BUILD_APPS "Build LAN chat applications" ON)
option(LAN_CHAT_USE_VCPKG_DEPS "Use optional vcpkg dependencies" ON)
option(LAN_CHAT_CORE_NO_DEPS "Keep C core free of third-party dependencies" ON)
```

约束：

- `LAN_CHAT_CORE_NO_DEPS` 永远保持开启。
- `lan_chat_core_c` 不得调用 `find_package` 得到的目标。
- `libmysql` 只链接到 `lan_chat_storage_mysql_c`。
- `lan_chat_server_c` 只链接 `lan_chat_storage_c`，不得链接 `lan_chat_storage_mysql_c`。
- `chat_server` 同时链接 `lan_chat_server_c` 和 `lan_chat_storage_mysql_c`，并在启动时完成 storage 实现注入。
- `lan_chat_transport_c` 只通过 `lan_chat_platform_win32` 访问 WinSock。
- C++ wrapper 不直接包含 `mysql.h`。
- `nlohmann-json` 用于 C++ 配置加载。
- `fmt`、`spdlog` 用于 C++ wrapper、apps 和运行时日志。
- `gtest` 用于 C/C++ 单元测试和集成测试，不链接到 C Core。

## 10. 配置与运行时

Phase 1 固定配置结构：

```text
config/
  defaults/
    server.json
    client.json
  local/
    server.json
    client.json
  profiles/
    server.test.json
    client.lan.json
```

服务端配置：

- listen host。
- listen port。
- discovery UDP port。
- worker thread count。
- MySQL host。
- MySQL port。
- MySQL user。
- MySQL password。
- MySQL database。
- MySQL connection pool size。
- log level。
- heartbeat timeout。

客户端配置：

- server host。
- server port。
- discovery enable。
- username default，只作为 CLI 登录输入默认值。
- log level。
- local runtime directory。

配置原则：

- 默认配置可提交。
- 本地密码放 `config/local/server.json`，不提交。
- 测试配置独立。
- CLI 参数可以覆盖配置文件。
- 环境变量只用于本机路径、测试注入和 CI。

## 11. 测试与验收

### 11.1 文档校验

必须确认：

- 文档不再把 IoT、嵌入式、gateway、telemetry 作为主线。
- 文档不再推荐 SQLite 作为 v1 默认存储。
- 文档不再把 `mysql-connector-cpp` 作为优先 MySQL 依赖。
- 文档明确 MySQL 默认实现使用 MySQL C API/libmysql。
- 文档明确 v1 全员可聊，好友/联系人申请进入 v2。
- 文档明确 `uint64 user_id` 是协议、会话、消息和数据库外键的统一用户标识。
- 文档明确禁止明文密码存储，auth 使用 salt/hash。
- 文档明确 UDP discovery 不作为 Phase 1 或 CLI E2E 阻塞项。

### 11.2 C Core 单元测试

详细验收见 `docs/architecture/LAN_CHAT_PROTOCOL.md`。本节保留主架构验收摘要，必须覆盖：

- header encode/decode。
- header 64 bytes 固定布局和字段 offset。
- TLV encode/decode。
- TLV required/optional bit。
- TLV unknown optional skip。
- TLV unknown required reject。
- TLV duplicate field reject。
- 非法 magic。
- 非法 version。
- 非法 header_len。
- 非法 body_len。
- reserved bits 非 0。
- 大端序读写。
- packet size 边界。
- ring buffer wraparound。
- TCP 半包。
- TCP 粘包。
- TCP 一次输入多个完整包。
- framer 半包返回 `LAN_CHAT_STATUS_NEED_MORE_DATA`。
- framer 非法 header 返回错误且不自动流恢复。
- body 超过 `LAN_CHAT_MAX_BODY_LEN` 必须拒绝。

### 11.3 服务端单元测试

必须覆盖：

- session registry。
- 登录态绑定。
- 单账号 kick-old。
- presence online/offline。
- user list。
- chat send -> store ack。
- receiver offline -> pending delivery。
- heartbeat timeout。

### 11.4 MySQL 集成测试

必须覆盖：

- schema 初始化。
- 注册用户。
- 登录验证。
- 禁止明文密码字段和明文测试种子数据。
- 用户列表查询。
- 消息写入。
- `message_deliveries` pending 查询。
- `message_deliveries` delivered/acked 状态更新。
- 历史消息查询。

### 11.5 端到端测试

必须覆盖：

- 启动 `chat_server`。
- 启动两个 `chat_cli`。
- A/B 登录。
- A 给 B 发送在线私聊。
- B 收到消息并 ACK。
- B 下线。
- A 给 B 发送离线消息。
- B 重连后从 `message_deliveries` 拉取待投递消息。
- B 查询历史消息。
- 服务端停止后客户端感知断线。
- 服务端恢复后客户端可以重新登录或恢复 session。
- CLI E2E 使用手动配置服务器地址，不依赖 UDP discovery。
- C++ RAII 测试覆盖 client/server/storage/session 释放。

## 12. 分阶段开发路线

### Phase 1: 工程骨架

交付：

- CMake 根工程。
- vcpkg manifest。
- CMakePresets。
- C Core target。
- `lan_chat_platform_win32` target。
- `lan_chat_transport_c` target。
- C storage interface target。
- `lan_chat_storage_mysql_c` target。
- `lan_chat_storage_memory_c` target。
- `lan_chat_server_c` target。
- C++ wrapper target。
- `chat_server` 空壳。
- `chat_cli` 空壳。
- MySQL schema 初版。
- 默认配置和本地配置模板。
- 本架构文档。

### Phase 2: C 协议核心

交付：

- header encode/decode。
- TLV encode/decode。
- packet pack/unpack。
- ring buffer。
- TCP framer。
- 协议单元测试。

### Phase 3: Windows TCP Transport

详细验收见 `docs/architecture/LAN_CHAT_TRANSPORT_WIN32.md`。本节保留主架构验收摘要，必须覆盖：

交付：

- WinSock platform adapter。
- TCP listener。
- TCP client。
- send/recv loop。
- 心跳。
- 多客户端连接 smoke。
- 不要求 UDP discovery 通过验收。

### Phase 4: C Storage Core + MySQL Storage

详细验收见 `docs/architecture/LAN_CHAT_STORAGE.md`。本节保留主架构验收摘要，必须覆盖：

交付：

- `lan_chat_storage_c`。
- `lan_chat_storage_mysql_c`。
- `lan_chat_storage_memory_c`。
- libmysql 连接。
- schema 初始化。
- accounts/messages/message_deliveries 基础操作。
- MySQL 集成测试。

### Phase 5: C++ Wrapper

交付：

- `Result<T>`。
- `Server` RAII。
- `Client` RAII。
- `Storage` RAII。
- 配置加载。
- 日志适配。
- 生命周期测试。

### Phase 6: CLI Client E2E

交付：

- CLI 注册/登录。
- 用户列表/在线用户显示。
- 私聊发送和接收。
- 待投递消息拉取。
- 历史消息查询。
- 端到端测试。

### Phase 7: Runtime Tooling

交付：

- 本地启动脚本。
- 测试配置。
- 日志收集。
- LAN 检查。
- 后续 Launcher 设计。

## 13. v2 规划边界

v2 可规划：

- 好友申请。
- 联系人关系。
- 黑名单。
- 群聊。
- 文件传输。
- Qt GUI。
- Runtime Launcher。
- 消息已读回执。
- SQLite 轻量部署适配。

v2 不应反向污染 v1 的核心目标。v1 的验收标准始终是全员可聊私聊闭环。

## 14. 旧错误方向排除

本文档明确排除之前错误方向：

- 不使用 `LAN-IoT` 作为产品定位。
- 不以 `gatewayd` 为主线命名。
- 不实现 `device_sim`。
- 不实现 telemetry。
- 不实现 command routing。
- 不把嵌入式、RTOS、lwIP 作为当前阶段目标。
- 不推荐 SQLite 作为 v1 默认存储。
- 不把 GitHub 仓库名中的 `embedded` 解释为当前阶段产品方向。

当前阶段唯一主线是：

```text
LAN 聊天系统
  -> C 底层
  -> C++ 封装
  -> MySQL C API/libmysql 服务端持久化
  -> CLI 客户端端到端跑通
```
