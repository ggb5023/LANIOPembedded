# LAN Chat Phase 6 Text, Group, File Closure

## 1. Boundary

Phase 6 turns the current app smoke test into real command-line workflows backed by
MySQL. It does not change the overall architecture and does not introduce GUI,
friends, offline delivery, history query, ACK/read receipts, group ownership,
audio, or video.

Required closures:

- private text: online sender -> `chat_server` -> online receiver
- group text: persistent MySQL group conversation -> online fanout
- private file: online sender -> `chat_server` relay -> online receiver

## 2. CLI Contract

`chat_cli` exposes stable command-style operations:

- `register --server host:port --username name --password pass [--nickname name]`
- `login --server host:port --username name --password pass`
- `send --server host:port --username name --password pass --to-user-id id --message text`
- `listen --server host:port --username name --password pass --expect-count n --timeout-ms ms [--output-dir dir]`
- `group-create --server host:port --username name --password pass --name group --member-user-ids 2,3`
- `group-send --server host:port --username name --password pass --conversation-id id --message text`
- `send-file --server host:port --username name --password pass --to-user-id id --path file`

Machine-readable success markers:

- `REGISTER_OK`
- `LOGIN_OK`
- `PRIVATE_SEND_OK`
- `PRIVATE_TEXT`
- `GROUP_CREATE_OK`
- `GROUP_SEND_OK`
- `GROUP_TEXT`
- `FILE_SEND_OK`
- `FILE_RECEIVED`

## 3. Protocol Decisions

Private text keeps using `LAN_CHAT_GROUP_CHAT`.

Group text uses `LAN_CHAT_GROUP_CONVERSATION`:

- create request carries group name and a big-endian `uint64` member-id array.
- send request carries `conversation_id`, text content, and optional client message id.
- notify carries `conversation_id`, `message_id`, `sender_id`, and content.
- sender does not receive its own group notify.
- group member count is capped at 16, including creator.

File transfer uses `LAN_CHAT_GROUP_FILE`:

- `start` request carries receiver id, file name, file size, and CRC32.
- receiver must be online and listening; otherwise the request fails.
- chunks are relayed as notify packets and acknowledged to the sender per chunk.
- `complete` marks the metadata as completed and notifies the receiver.
- maximum file size is 1 MiB.
- file content is never stored in MySQL.

## 4. Storage Decisions

MySQL is the required Phase 6 acceptance backend. The memory backend is kept in
sync with the public storage interface so server code never depends on a MySQL
implementation detail.

Schema updates:

- `conversations` stores `conversation_type` and optional `conversation_name`.
- `conversation_members` stores active members.
- `messages.receiver_id` is nullable so group messages can be conversation-only.
- file metadata is stored in `file_transfers` with `started`/`completed` state.

## 5. Acceptance

Default lightweight tests remain available without MySQL. MySQL app E2E is
registered only when `LAN_CHAT_ENABLE_MYSQL_TESTS=ON`, and this phase must run it.

The MySQL app E2E:

1. recreates `lan_chat_e2e`;
2. starts `chat_server --storage mysql`;
3. registers Alice, Bob, and Carol;
4. verifies private text with Bob running `listen`;
5. verifies group text with Bob and Carol running `listen`;
6. verifies private file transfer to Bob, including file size and CRC32;
7. prints `APP_E2E_OK`.

The local default MySQL password for the E2E script is `123456`, with environment
variable or parameter override kept available.

## 6. Phase 6.1 Stabilization

Phase 6.1 keeps the Phase 6 scope unchanged and hardens the current closure:

- a new login for the same `user_id` closes the older server-side session;
- disconnecting a sender or receiver clears any active online file transfer;
- file chunks must be received in increasing chunk-index order;
- `chat_cli listen` rejects unsafe incoming file names and deletes partial files
  when a transfer fails validation;
- `chat_cli listen` reports a clear error when the expected event count is not
  reached before timeout.

These changes do not add offline delivery, ACK, history workflow, group file
transfer, GUI, friends, audio, or video.
