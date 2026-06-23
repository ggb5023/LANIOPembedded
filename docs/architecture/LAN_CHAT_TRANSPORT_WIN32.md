# LAN 聊天 Phase 3 Windows TCP Transport 架构文档

## 1. 文档范围

本文档固定 Phase 3 的 Windows TCP Transport 架构。目标是把 `lan_chat_platform_win32` 和 `lan_chat_transport_c` 从骨架推进到可验证的 TCP 最小闭环：

```text
client transport
  -> TCP socket
    -> server listener accept
      -> server transport connection
        -> core framer
          -> packet echo / heartbeat smoke
```

Phase 3 不改变总体架构，不引入业务状态机，不接入 MySQL，不要求 UDP discovery 通过验收。协议 header、packet、ring buffer、framer 以 `docs/architecture/LAN_CHAT_PROTOCOL.md` 和 `lan_chat_core_c` 已实现接口为准。

## 2. 模块边界

### 2.1 `lan_chat_platform_win32`

职责：

- 负责 WinSock 生命周期管理。
- 封装 TCP listen、connect、accept、send、recv、close。
- 提供 socket non-blocking 配置。
- 提供 Windows socket error 到 `lan_chat_status_t` 的转换。

约束：

- 这是唯一允许接触 WinSock API 的实现层。
- 公共头文件不暴露 `SOCKET`、`WSA*`、`sockaddr*`、`windows.h` 或 `winsock2.h`。
- `MYSQL*`、storage、server session registry、业务消息处理均不得进入本模块。

### 2.2 `lan_chat_transport_c`

职责：

- 管理 TCP listener 和 TCP connection 状态。
- 管理接收缓冲、发送队列和 `lan_chat_framer_t`。
- 把 `lan_chat_header_t + body` 打包为 wire packet 后发送。
- 从 TCP 字节流中恢复完整 packet，并交给上层处理。
- 实现 Phase 3 heartbeat smoke 所需的 packet 收发能力。

约束：

- 不包含 WinSock/Windows networking headers。
- 不拥有业务用户、会话、数据库和消息投递语义。
- 不直接调用 C++ wrapper。
- 所有 socket I/O 必须经 `lan_chat_platform_win32`。

### 2.3 `lan_chat_core_c`

职责：

- 继续只负责 endian、protocol、TLV、ring buffer、framer。
- 不感知 socket、线程、server、client 或 Windows 平台。

## 3. 运行模型

Phase 3 固定为单线程可轮询 transport primitive：

- listener、client connection、accepted connection 均使用 non-blocking socket。
- `accept` 在没有新连接时返回 `LAN_CHAT_STATUS_NEED_MORE_DATA`。
- `recv` 在没有可读数据时返回 `LAN_CHAT_STATUS_NEED_MORE_DATA`。
- `send` 允许部分写入，未写完的数据保留在 connection send queue 中。
- 上层通过显式 poll/flush 调用驱动收发。

此模型服务于后续 I/O loop + worker queue + worker pool。Phase 3 不实现 worker pool，但必须保证 worker 未来不需要持有 socket 指针：transport 层只向上交付完整 packet，并只从上层接收待发送 packet/fanout 任务。

## 4. Socket 生命周期与所有权

### 4.1 WinSock 生命周期

- `lan_chat_platform_win32_init()` 负责调用 `WSAStartup`。
- `lan_chat_platform_win32_shutdown()` 负责调用 `WSACleanup`。
- 实现必须支持同一进程内多次 init/shutdown 配对调用，内部可使用引用计数。
- transport 初始化可以调用 platform init，但最终应用层仍应在进程启动和退出处显式控制生命周期。

### 4.2 Listener

- listener 拥有一个 listening socket。
- listener 创建流程固定为 socket -> bind -> listen -> set non-blocking。
- Phase 3 backlog 默认值为 16；后续可由 config 覆盖。
- listener close 后 socket handle 置为 invalid，重复 close 必须安全。
- listener 不保存 accepted connection 列表，只负责产生 connection 对象。

### 4.3 Connection

- connection 独占一个 connected socket。
- connection 不可复制。
- connection close 后 socket handle 置为 invalid，接收缓冲、发送队列和 framer 状态清空。
- client connect 成功后立即进入 non-blocking 模式。
- accepted socket 在 accept 成功后立即进入 non-blocking 模式。

## 5. Platform API 形态

Phase 3 的 platform public header 使用 opaque handle，不暴露 WinSock 类型：

```c
typedef uintptr_t lan_chat_socket_handle_t;

#define LAN_CHAT_SOCKET_INVALID ((lan_chat_socket_handle_t)(~(uintptr_t)0))
```

目标 API：

```c
lan_chat_status_t lan_chat_platform_win32_init(void);
void lan_chat_platform_win32_shutdown(void);

lan_chat_status_t lan_chat_platform_tcp_listen(
    const char *host,
    uint16_t port,
    int backlog,
    lan_chat_socket_handle_t *out_listener);

lan_chat_status_t lan_chat_platform_tcp_accept(
    lan_chat_socket_handle_t listener,
    lan_chat_socket_handle_t *out_socket,
    char *remote_ip,
    size_t remote_ip_capacity,
    uint16_t *out_remote_port);

lan_chat_status_t lan_chat_platform_tcp_connect(
    const char *host,
    uint16_t port,
    lan_chat_socket_handle_t *out_socket);

lan_chat_status_t lan_chat_platform_tcp_set_nonblocking(
    lan_chat_socket_handle_t socket,
    int enabled);

lan_chat_status_t lan_chat_platform_tcp_send(
    lan_chat_socket_handle_t socket,
    const uint8_t *data,
    size_t data_len,
    size_t *out_sent);

lan_chat_status_t lan_chat_platform_tcp_recv(
    lan_chat_socket_handle_t socket,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *out_received);

lan_chat_status_t lan_chat_platform_tcp_local_endpoint(
    lan_chat_socket_handle_t socket,
    char *local_ip,
    size_t local_ip_capacity,
    uint16_t *out_local_port);

void lan_chat_platform_tcp_close(lan_chat_socket_handle_t *socket);
```

API 约定：

- `host == NULL` 或空字符串表示 bind/connect 默认地址由调用方显式决定；server 默认建议使用 `0.0.0.0`，smoke test 使用 `127.0.0.1`。
- `port == 0` 允许系统分配端口；Phase 3 使用 `lan_chat_platform_tcp_local_endpoint()` 读取实际监听端口。
- `send` 返回成功时 `out_sent` 可以小于 `data_len`。
- `recv` 返回成功时 `out_received > 0`。
- `recv` 返回 0 字节或连接被重置时映射为 `LAN_CHAT_STATUS_NETWORK_ERROR`，transport 将其解释为连接关闭。
- `WSAEWOULDBLOCK` 映射为 `LAN_CHAT_STATUS_NEED_MORE_DATA`。

## 6. Transport API 形态

Phase 3 固定引入 listener 和 connection 两类对象：

```c
typedef struct lan_chat_tcp_listener lan_chat_tcp_listener_t;
typedef struct lan_chat_tcp_connection lan_chat_tcp_connection_t;

lan_chat_status_t lan_chat_tcp_listener_open(
    lan_chat_tcp_listener_t *listener,
    const char *host,
    uint16_t port,
    int backlog);

lan_chat_status_t lan_chat_tcp_listener_accept(
    lan_chat_tcp_listener_t *listener,
    lan_chat_tcp_connection_t *out_connection);

void lan_chat_tcp_listener_close(lan_chat_tcp_listener_t *listener);

lan_chat_status_t lan_chat_tcp_client_connect(
    lan_chat_tcp_connection_t *connection,
    const char *host,
    uint16_t port);

lan_chat_status_t lan_chat_tcp_connection_queue_packet(
    lan_chat_tcp_connection_t *connection,
    const lan_chat_header_t *header,
    const uint8_t *body,
    size_t body_len);

lan_chat_status_t lan_chat_tcp_connection_flush(
    lan_chat_tcp_connection_t *connection);

lan_chat_status_t lan_chat_tcp_connection_recv_packet(
    lan_chat_tcp_connection_t *connection,
    uint8_t *out_packet,
    size_t out_packet_capacity,
    size_t *out_packet_len);

void lan_chat_tcp_connection_close(lan_chat_tcp_connection_t *connection);
```

对象约定：

- `lan_chat_tcp_connection_t` 内部持有 socket handle、receive scratch buffer、send queue、send offset、framer buffer 和连接状态。
- receive scratch buffer 建议 8 KiB。
- framer buffer 容量不得小于 `LAN_CHAT_HEADER_LEN + LAN_CHAT_MAX_BODY_LEN`。
- send queue 容量不得小于 `LAN_CHAT_HEADER_LEN + LAN_CHAT_MAX_BODY_LEN`，Phase 3 可固定单包队列。
- `queue_packet` 使用 `lan_chat_packet_pack()`，不允许调用方传入已拼接 wire bytes 绕过协议校验。
- `recv_packet` 内部执行 platform recv -> framer feed -> framer next；未形成完整包时返回 `LAN_CHAT_STATUS_NEED_MORE_DATA`。

## 7. 收发数据流

发送路径：

```text
upper layer packet intent
  -> lan_chat_header_t + body
  -> lan_chat_packet_pack()
  -> connection send queue
  -> lan_chat_platform_tcp_send()
  -> partial write offset update
```

接收路径：

```text
lan_chat_platform_tcp_recv()
  -> receive scratch buffer
  -> lan_chat_framer_feed()
  -> lan_chat_framer_next()
  -> complete wire packet
  -> upper layer decode / dispatch
```

规则：

- transport 不解释 auth/chat/history 等业务字段。
- transport 可以读取 header 做基础校验、日志字段和 heartbeat smoke。
- malformed packet 或 body too large 视为协议错误，connection 必须关闭。
- 同一连接内 packet 顺序由 TCP 和 send queue 保证，transport 不重排。

## 8. 错误处理

Phase 3 先复用现有 `lan_chat_status_t`：

- invalid argument -> `LAN_CHAT_STATUS_INVALID_ARGUMENT`
- out of memory -> `LAN_CHAT_STATUS_OUT_OF_MEMORY`
- would block / no pending accept / no complete packet -> `LAN_CHAT_STATUS_NEED_MORE_DATA`
- send queue capacity 不足 -> `LAN_CHAT_STATUS_BUFFER_TOO_SMALL`
- WinSock connect/send/recv/accept/bind/listen 失败 -> `LAN_CHAT_STATUS_NETWORK_ERROR`
- protocol header/body/framer 校验失败 -> 对应 core status

实现要求：

- platform 层保存或日志化原始 WSA error code，但不向公共 API 暴露 Windows 类型。
- transport 收到 `LAN_CHAT_STATUS_NETWORK_ERROR` 后可以把 connection 标记为 closed。
- close 操作不返回错误；close 后重复调用安全。

Phase 3 如确实需要更精细状态，只允许小范围补充：

- `LAN_CHAT_STATUS_CONNECTION_CLOSED`
- `LAN_CHAT_STATUS_ADDRESS_IN_USE`
- `LAN_CHAT_STATUS_CONNECT_FAILED`

补充状态必须同步更新 `lan_chat_status_name()` 和单元测试。

## 9. Heartbeat 边界

Phase 3 heartbeat 只验证 transport 能发送和接收 heartbeat packet：

- group 使用 `LAN_CHAT_GROUP_HEARTBEAT`。
- request 使用 `LAN_CHAT_MSG_REQ` + `LAN_CHAT_FLAG_REQUEST`。
- response 使用 `LAN_CHAT_MSG_RSP` + `LAN_CHAT_FLAG_RESPONSE`。
- body 可以为空。
- transport 不维护登录 session，不做 user online/offline 判定。
- 超时策略只在文档中预留，不作为 Phase 3 smoke 阻塞项。

后续服务端阶段再把 heartbeat 接入 session registry、last_seen、presence notify 和断线清理。

## 10. 并发约束

- Phase 3 transport 对象默认单线程访问。
- 同一个 listener 或 connection 不允许多个线程并发调用，除非调用方自行加锁。
- platform socket handle 不跨线程共享所有权。
- worker 线程不得直接持有 socket handle 或 connection 指针。
- 后续 I/O loop 可以在单独线程中独占 listener/connection，并通过 queue 接收 worker 产生的 response/fanout 任务。

## 11. 配置边界

Phase 3 必须支持手动配置服务器地址：

- `host`
- `port`
- `backlog`

UDP discovery 不参与 Phase 3 验收，不得成为 CLI E2E 的阻塞项。

## 12. Smoke 验收

Phase 3 最小闭环测试固定覆盖：

- WinSock init/shutdown 可重复配对调用。
- listener 在 `127.0.0.1` 启动。
- client connect 到 listener。
- server accept 得到 accepted connection。
- client queue + flush 一个 heartbeat request packet。
- server recv 得到完整 packet，并校验 group/type/flags/body_len。
- server queue + flush 一个 heartbeat response packet。
- client recv 得到完整 response packet。
- 半包输入通过 framer 恢复完整 packet。
- 粘包输入可以连续取出多个完整 packet。
- 两个 client 顺序连接和收发，不串包。
- close listener/connection 后重复 close 安全。

非目标：

- 不要求 UDP discovery。
- 不要求 MySQL。
- 不要求登录、注册、用户列表、聊天转发。
- 不要求 worker pool。
- 不要求 GUI。

## 13. 验收检查

- `winsock2.h`、`ws2tcpip.h`、`windows.h` 不出现在 `lan_chat_core_c`、`lan_chat_transport_c`、`lan_chat_server_c`、`lan_chat_cpp` 的 public headers 中。
- `lan_chat_transport_c` 只链接 `lan_chat_core_c` 和 `lan_chat_platform_win32`。
- packet 收发必须经过 `lan_chat_packet_pack()` 和 `lan_chat_framer_t`。
- `LAN_CHAT_PROTOCOL.md` 中的 header、flags、message group 约定保持一致。
- UDP discovery 仍为后置能力。
- 现有 protocol/core 单元测试继续通过。
- 新增 transport smoke test 在 Windows/MSVC Debug 配置下通过。
