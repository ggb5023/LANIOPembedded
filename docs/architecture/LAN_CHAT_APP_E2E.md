# LAN 聊天 Phase 5 App-Level E2E 架构文档

## 1. 范围

本文档固定 Phase 5 App-Level E2E 的最小闭环。目标是把库级 register/login/chat E2E 推进为真实可运行的两个应用：

```text
chat_server
  -> lan_chat_server_c
    -> TCP transport
    -> memory storage

chat_cli e2e
  -> TCP transport
    -> register/login/chat protocol
```

本阶段不改变总体架构，不实现 GUI、交互式 TUI、UDP discovery、好友系统、群聊或生产级密码 KDF。

## 2. 运行命令

Phase 5 固定两个可执行入口：

```text
chat_server --storage memory --host 127.0.0.1 --port 7777
chat_cli e2e --server 127.0.0.1:7777
```

`chat_server` 默认使用 memory storage，便于本地 E2E 和 CI smoke test 稳定运行。MySQL backend 保留为可选启动模式，不作为 App-Level E2E 的默认依赖。

## 3. `chat_server`

职责：

- 解析最小命令行参数。
- 创建 storage backend。
- 初始化 `lan_chat_server_c`。
- 进入 `lan_chat_server_run_once()` 循环。
- 支持 Ctrl+C / SIGTERM 触发优雅退出。

固定参数：

- `--storage memory|mysql`，默认 `memory`。
- `--host <ip>`，默认 `127.0.0.1`。
- `--port <port>`，默认 `7777`。

MySQL 参数优先从命令行读取，其次使用环境变量；不得把本地数据库密码写入仓库默认配置。

## 4. `chat_cli e2e`

职责：

- 连接外部 `chat_server`，而不是在进程内启动 server。
- 在一个 CLI 进程内创建 Alice/Bob 两个 TCP client。
- 依次执行：
  1. Alice register。
  2. Bob register。
  3. Alice login。
  4. Bob login。
  5. Alice 向 Bob 发送文本消息。
  6. Alice 收到 chat response。
  7. Bob 收到 chat notify。

验收输出必须包含：

- Alice/Bob 的 `user_id` 与 `session_id`。
- `message_id` 与 `delivery_id`。
- Bob 收到的 `sender_id` 与文本内容。
- 成功结束标记：`APP_E2E_OK`。

## 5. 测试

新增 app-level smoke test：

```text
CTest
  -> 启动 chat_server --storage memory
  -> 等待 TCP port 可连接
  -> 执行 chat_cli e2e --server 127.0.0.1:<port>
  -> 校验 APP_E2E_OK
  -> 停止 chat_server
```

该测试只验证应用层闭环，不替代 storage MySQL 集成测试。
