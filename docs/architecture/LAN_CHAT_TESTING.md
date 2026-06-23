# LAN 聊天测试文档

## 1. 文档范围

本文档是当前工程测试入口。其他架构文档只保留模块验收边界，具体测试分类、运行方式和默认/可选范围以本文档为准。

本次测试体系目标是轻量、稳定、可重复，避免把未实现能力写成当前默认测试阻塞项。

## 2. 默认测试

默认 CTest 只包含不依赖外部服务的测试：

```text
lan_chat_skeleton_tests
lan_chat_app_e2e
```

`lan_chat_skeleton_tests` 覆盖当前库级能力：

- core endian/header/packet/TLV/ring buffer/framer。
- memory storage 账号、消息、历史和 delivery 状态基础行为。
- Windows TCP transport heartbeat smoke。
- server memory storage register/login/chat 库级 E2E。

`lan_chat_app_e2e` 覆盖当前真实进程闭环：

- 启动 `chat_server --storage memory`。
- 执行 `chat_cli e2e --server 127.0.0.1:<port>`。
- 校验输出包含 `APP_E2E_OK`。
- 停止 `chat_server`。

默认测试不要求：

- MySQL 服务可用。
- UDP discovery。
- GUI。
- 好友/联系人系统。
- presence、kick-old、离线投递、历史查询、ACK 或 RAII 生命周期完整验收。

## 3. 可选测试

MySQL 集成测试为显式可选项：

```text
LAN_CHAT_ENABLE_MYSQL_TESTS=ON
lan_chat_mysql_storage_tests
```

该测试需要本机或 CI 提供 MySQL，并通过环境变量或本地配置注入连接信息。普通默认测试不得因为 MySQL 未启动、密码缺失或本机数据库状态而失败。

## 4. 后续 Backlog

以下能力尚未作为当前默认测试阻塞项：

- presence online/offline。
- 单账号 kick-old。
- 用户列表和在线用户列表。
- receiver offline -> pending delivery。
- delivery ACK。
- pending delivery pull。
- 历史消息查询。
- heartbeat timeout。
- 断线重连和 session 恢复。
- C++ RAII client/server/storage/session 生命周期完整测试。

这些能力进入对应 Phase 实现后，必须先补文档、接口和最小闭环，再升级为默认测试或可选测试。

## 5. 运行命令

当前 Windows/MSVC Debug 验证命令：

```powershell
cmake --build out\build\skeleton-verify --config Debug
ctest --test-dir out\build\skeleton-verify -C Debug --output-on-failure
```

如果当前 shell 没有 `cmake`，可使用 Visual Studio bundled CMake：

```powershell
& 'D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build out\build\skeleton-verify --config Debug
& 'D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir out\build\skeleton-verify -C Debug --output-on-failure
```

## 6. 文档规则

- “必须覆盖”只用于当前已实现能力或当前 Phase 验收。
- 未实现能力必须放入 backlog，不写成默认测试要求。
- 阶段文档可以说明模块验收意图，但测试入口统一引用本文档。
- 默认测试应保持轻量、可重复、无外部服务依赖。
