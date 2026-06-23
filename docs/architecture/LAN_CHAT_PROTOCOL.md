# LAN 聊天协议架构文档

## 1. 范围

本文档定义 LAN Chat v1 的协议核心规格，直接指导 Phase 2 `lan_chat_core_c` 实现。

Phase 2 只实现：

- fixed binary header。
- TLV body。
- endian read/write。
- packet pack/unpack。
- ring buffer。
- TCP framer。
- C Core 单元测试。

Phase 2 不实现：

- WinSock。
- MySQL。
- vcpkg 依赖。
- 线程。
- 业务 handler。
- CLI 端到端流程。
- UDP discovery。

## 2. Transport

TCP：

- 承载所有核心聊天协议。
- 保证注册、登录、消息、ACK、待投递消息拉取和历史查询可靠。

UDP：

- 仅用于 LAN 服务发现。
- 客户端广播 discovery request。
- 服务端回复服务名、版本、TCP 地址和端口。
- 客户端仍必须支持手动输入服务器地址。
- Phase 1、Phase 2、Phase 3 和 CLI E2E 均不得依赖 UDP discovery。

## 3. Wire Format

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

## 4. Header

Phase 2 固定 TCP packet header 为 64 bytes：

```text
offset  size  field
0       2     magic
2       1     version
3       1     header_len
4       2     message_group
6       2     message_type
8       2     flags
10      2     status
12      4     body_len
16      4     seq
20      8     session_id
28      8     sender_id
36      8     receiver_id
44      8     timestamp_ms
52      12    reserved
```

Header 规则：

- `magic` 固定为 `0x4C43`，表示 ASCII `LC`。
- `version` 固定为 `1`。
- `header_len` 固定为 `64`。
- 所有多字节字段使用大端序。
- `body_len` 最大值固定为 `LAN_CHAT_MAX_BODY_LEN`，Phase 2 初始值为 `65535`。
- `reserved[12]` 必须全部为 0。
- `packet_size = header_len + body_len`，计算前必须检查整数溢出。

Header 标识规则：

- `session_id`、`sender_id`、`receiver_id` 均为 `uint64`。
- 未登录请求的 `sender_id` 为 0；登录成功后客户端必须使用服务端返回的 `user_id`。
- `username` 仅出现在注册、登录等 AUTH TLV body 中。
- 服务端路由私聊消息时只使用 `receiver_id`，不使用 username 字符串路由。

## 5. Message Groups And Types

Message group 使用 `uint16`：

```text
AUTH      = 0x0001
SESSION   = 0x0002
USER      = 0x0003
PRESENCE  = 0x0004
CHAT      = 0x0005
HISTORY   = 0x0006
HEARTBEAT = 0x0007
DISCOVERY = 0x0008
ERROR     = 0x00FF
```

Message type 使用组内 `uint16`：

```text
REQ    = 0x0001
RSP    = 0x0002
NOTIFY = 0x0003
ACK    = 0x0004
```

Phase 2 只要求 core 能编码、解码和校验数值；不实现具体业务语义。

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

v1 不包含 `CONTACT` 消息组。当前后续开发不规划联系人、好友申请或好友审批协议。

## 6. Flags And Status

Flags 使用 `uint16`：

```text
REQUEST  = 0x0001
RESPONSE = 0x0002
NOTIFY   = 0x0004
ACK      = 0x0008
ERROR    = 0x0010
```

Flags 校验规则：

- `REQUEST`、`RESPONSE`、`NOTIFY` 三者互斥。
- 未定义 bit 必须拒绝。
- `ERROR` 可以和 `RESPONSE` 同时出现。

Status 使用 `uint16`：

- 成功为 `0`。
- 非 0 值映射到 `lan_chat_status_t` 或协议错误码。
- Phase 2 不要求完整业务错误码，只要求 status 字段大端编解码和 packet 往返一致。

Phase 2 必须补齐的 core status：

- `LAN_CHAT_STATUS_NEED_MORE_DATA`。
- `LAN_CHAT_STATUS_BAD_MAGIC`。
- `LAN_CHAT_STATUS_UNSUPPORTED_VERSION`。
- `LAN_CHAT_STATUS_BAD_HEADER`。
- `LAN_CHAT_STATUS_BODY_TOO_LARGE`。
- `LAN_CHAT_STATUS_UNSUPPORTED_FIELD`。
- `LAN_CHAT_STATUS_DUPLICATE_FIELD`。

## 7. TLV

TLV header 固定 4 bytes：

```text
type:   uint16
length: uint16
value:  bytes[length]
```

TLV type 规则：

- bit 15 为 required 标记，`1` 表示 required，`0` 表示 optional。
- bit 0-14 是 field id。
- field id `0` 保留，必须拒绝。
- 未知 optional TLV 跳过。
- 未知 required TLV 返回 `LAN_CHAT_STATUS_UNSUPPORTED_FIELD` 或等价协议错误。
- 同一 field id 在同一个 body 中重复出现时，Phase 2 reader 必须返回 duplicate field 错误。

TLV value 规则：

- `uint16`、`uint32`、`uint64` 均为大端序。
- 字符串为 UTF-8 bytes，不带 null terminator。
- bytes 字段原样保存。
- reader 不校验 UTF-8 合法性，业务层需要时再校验。
- writer 和 reader 都必须在每次 offset 增长前检查溢出。

## 8. C Core Public API 草案

Phase 2 至少提供以下 public API 类别，具体命名可保持 `lan_chat_` 前缀并贴近已有骨架：

```c
lan_chat_status_t lan_chat_write_u16_be(...);
lan_chat_status_t lan_chat_write_u32_be(...);
lan_chat_status_t lan_chat_write_u64_be(...);
lan_chat_status_t lan_chat_read_u16_be(...);
lan_chat_status_t lan_chat_read_u32_be(...);
lan_chat_status_t lan_chat_read_u64_be(...);

lan_chat_status_t lan_chat_header_encode(...);
lan_chat_status_t lan_chat_header_decode(...);
lan_chat_status_t lan_chat_header_validate(...);

lan_chat_status_t lan_chat_tlv_writer_init(...);
lan_chat_status_t lan_chat_tlv_write_u64(...);
lan_chat_status_t lan_chat_tlv_write_string(...);
lan_chat_status_t lan_chat_tlv_write_bytes(...);
lan_chat_status_t lan_chat_tlv_reader_init(...);
lan_chat_status_t lan_chat_tlv_next(...);

lan_chat_status_t lan_chat_packet_pack(...);
lan_chat_status_t lan_chat_packet_unpack(...);
lan_chat_status_t lan_chat_packet_peek_size(...);

lan_chat_status_t lan_chat_ring_buffer_init(...);
lan_chat_status_t lan_chat_ring_buffer_write(...);
lan_chat_status_t lan_chat_ring_buffer_peek(...);
lan_chat_status_t lan_chat_ring_buffer_discard(...);

lan_chat_status_t lan_chat_framer_init(...);
lan_chat_status_t lan_chat_framer_feed(...);
lan_chat_status_t lan_chat_framer_next(...);
```

API 约束：

- 所有输出 buffer 由调用方提供。
- C Core 不隐藏动态内存分配。
- `lan_chat_framer_next` 返回 packet view 或拷贝到调用方 buffer，不返回内部长期有效裸指针。
- `LAN_CHAT_STATUS_NEED_MORE_DATA` 表示半包，不是错误。

## 9. TCP Framer

Framer 必须支持：

- `feed` 任意长度 bytes。
- 半包输入：保存状态并返回 `LAN_CHAT_STATUS_NEED_MORE_DATA`。
- 粘包输入：一次 `feed` 后允许连续 `next` 多个 packet。
- 多包连续输入：不得丢弃后续 packet。
- ring buffer wraparound 后仍能读取完整 header/body。

非法输入处理：

- 非法 `magic`、`version`、`header_len`、`body_len`、reserved 非 0，`next` 返回明确错误。
- Phase 2 framer 遇到非法 header 后不自动扫描下一个 magic，不做流恢复。
- 上层 transport 收到非法 packet 错误后应关闭连接。
- body 超过最大值必须拒绝，不能尝试分配或读取超限 body。

## 10. 消息投递语义

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

## 11. Phase 2 测试验收

C Core 单元测试覆盖 header/packet/TLV/endian/ring buffer/framer 的当前实现行为。具体默认测试入口和运行方式见 `docs/architecture/LAN_CHAT_TESTING.md`。

Phase 2 文档只保留协议验收边界：header 固定布局、TLV 必要校验、packet size 边界和 framer 半包/粘包/多包行为必须有测试覆盖。
