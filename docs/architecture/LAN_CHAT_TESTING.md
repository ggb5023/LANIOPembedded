# LAN Chat Testing

## 1. Purpose

This document is the single testing entry point for the current project. Phase
documents may describe module acceptance intent, but test categories, commands,
and default/optional boundaries are defined here.

The testing policy stays lightweight and repeatable. Unimplemented capabilities
must stay in backlog instead of becoming default test blockers.

## 2. Default Tests

Default CTest includes only tests that do not require external services:

```text
lan_chat_skeleton_tests
lan_chat_app_e2e
```

`lan_chat_skeleton_tests` covers current library-level behavior:

- core endian/header/packet/TLV/ring buffer/framer
- memory storage account, message, history, and delivery basics
- Windows TCP transport heartbeat smoke
- server with memory storage register/login/private chat library E2E

`lan_chat_app_e2e` covers the lightweight real process smoke:

- starts `chat_server --storage memory`
- runs `chat_cli e2e --server 127.0.0.1:<port>`
- requires `APP_E2E_OK`
- stops `chat_server`

Default tests do not require:

- MySQL
- UDP discovery
- GUI
- friends/contacts
- presence
- offline delivery
- history query app workflow
- ACK/read receipts
- audio/video

## 3. Optional MySQL Tests

MySQL tests are explicit opt-in tests:

```text
LAN_CHAT_ENABLE_MYSQL_TESTS=ON
lan_chat_mysql_storage_tests
lan_chat_mysql_app_e2e
```

`lan_chat_mysql_storage_tests` verifies the storage contract against MySQL.

`lan_chat_mysql_app_e2e` is the Phase 6 app-level closure:

- recreates `lan_chat_e2e`
- starts `chat_server --storage mysql`
- registers Alice, Bob, and Carol
- verifies online private text
- verifies online group text fanout
- verifies online private file relay with size and CRC32 validation
- requires `APP_E2E_OK`

The local script default MySQL password is `123456`. It can be overridden with:

```text
LAN_CHAT_MYSQL_TEST_PASSWORD
LAN_CHAT_MYSQL_TEST_HOST
LAN_CHAT_MYSQL_TEST_PORT
LAN_CHAT_MYSQL_TEST_USER
MYSQL_EXE
```

Default CTest must not fail because MySQL is unavailable.

## 4. Phase 6.1 Stabilization Checks

Phase 6.1 keeps the same feature scope and strengthens engineering behavior:

- duplicate login closes the older session for that user
- file relay state is cleared when sender or receiver disconnects
- file chunks must arrive in order
- receiver rejects unsafe file names and removes partial files on failure
- CLI listen reports a clear error when expected events are not received

These checks are covered by the existing default and MySQL E2E paths unless a
future change adds focused unit tests.

## 5. Backlog

The following items are not default blockers yet:

- presence online/offline
- kick-old as a user-visible workflow
- user list and online user list app commands
- offline pending delivery
- delivery ACK
- pending delivery pull
- history query app workflow
- heartbeat timeout
- reconnect/session resume
- C++ RAII lifecycle coverage
- group admin/member management
- group file transfer
- audio/video calls

Each item must first get a small architecture note, an interface boundary, and a
minimal app closure before it can become a default or optional test requirement.

## 6. Commands

Default Windows/MSVC Debug verification:

```powershell
& 'E:\Qt\Tools\CMake_64\bin\cmake.exe' --build out\build\skeleton-verify --config Debug
& 'E:\Qt\Tools\CMake_64\bin\ctest.exe' --test-dir out\build\skeleton-verify -C Debug --output-on-failure
```

MySQL-enabled Phase 6 verification:

```powershell
& 'E:\Qt\Tools\CMake_64\bin\cmake.exe' -S . -B out\build\phase6-mysql -G "Visual Studio 17 2022" -A x64 -DLAN_CHAT_BUILD_TESTS=ON -DLAN_CHAT_BUILD_APPS=ON -DLAN_CHAT_USE_VCPKG_DEPS=OFF -DLAN_CHAT_CORE_NO_DEPS=ON -DLAN_CHAT_ENABLE_MYSQL_TESTS=ON -DLAN_CHAT_MYSQL_ROOT=D:/mysql
& 'E:\Qt\Tools\CMake_64\bin\cmake.exe' --build out\build\phase6-mysql --config Debug
$env:LAN_CHAT_MYSQL_TEST_PASSWORD='123456'
$env:MYSQL_EXE='D:\mysql\bin\mysql.exe'
& 'E:\Qt\Tools\CMake_64\bin\ctest.exe' --test-dir out\build\phase6-mysql -C Debug --output-on-failure
```
