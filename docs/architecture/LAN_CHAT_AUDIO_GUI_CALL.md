# LAN Chat Phase 7 GUI Audio Call

## 1. Boundary

Phase 7 adds a lightweight Qt GUI and one-to-one online audio calls. It reuses
the current `chat_server`, TCP/TLV protocol, `uint64 user_id` identity model, and
existing storage backends.

Phase 7 does not implement video, group calls, friends/contacts, offline call
invites, STUN/TURN, public internet traversal, call history, or call persistence.

## 2. GUI Contract

`chat_gui` is optional and built only when `LAN_CHAT_BUILD_GUI=ON`.

The first GUI must provide:

- server host/port input;
- register/login with username/password;
- online user list refresh;
- one-to-one audio call controls: invite, accept, reject, hangup, mute;
- status text for login, call state, and media state.

The GUI may reference the previous WebRTC prototype for Qt widget layout and
media implementation ideas, but it must not depend on absolute paths under the
reference project.

## 3. Protocol Additions

`LAN_CHAT_GROUP_USER` gains `online-list`:

- request: `operation = "online-list"`;
- response: `status`, `user_count`, `users_blob`;
- `users_blob` is a single packed byte field because TLV duplicate field ids are
  rejected by the core reader;
- each record stores `user_id`, username length, nickname length, username,
  nickname, and an online flag;
- the response excludes the requesting user.

`LAN_CHAT_GROUP_CALL = 0x000B` handles realtime call signaling:

- operations: `invite`, `accept`, `reject`, `hangup`, `offer`, `answer`, `ice`;
- server allocates `uint64 call_id` on invite;
- `peer_id` is always a `uint64 user_id`;
- `media_mode` is fixed to `audio` in Phase 7;
- SDP and ICE strings are relayed only between online call participants;
- the server does not parse SDP/ICE content and does not store it.

## 4. Runtime State

The server owns an in-memory call registry:

- one active call per user;
- invite fails if caller equals callee, callee is offline, or either user is busy;
- only call participants may accept/reject/hangup/forward SDP or ICE;
- disconnect of either participant clears the call and notifies the peer with a
  hangup notify when possible.

## 5. Dependencies

Default builds do not require GUI/AV dependencies.

Optional Phase 7 dependencies:

- Qt 6 Widgets/Multimedia for `chat_gui`;
- libdatachannel for WebRTC peer connection and RTP transport;
- Opus for audio encoding/decoding.

Qt is discovered from `Qt6_DIR`, then `WEBRTC_QT_ROOT`, then common local Qt
install paths. libdatachannel must be vendored or submoduled under this
repository's `third_party/` and pinned; current reference commit is
`9a64f8889d1823a6d0a2295b5bbc68e46d8d9fa9`.

## 6. Acceptance

Default CTest must keep passing without Qt, libdatachannel, real audio devices,
or MySQL.

Default tests cover:

- online user list after multiple users login;
- invite/accept/offer/answer/ice/hangup signaling;
- busy, offline, self-call, and disconnect cleanup paths.

Manual Phase 7 audio acceptance:

1. Start `chat_server`.
2. Start two `chat_gui` instances.
3. Register or login as two different users.
4. Refresh online users and start an audio call.
5. Accept the call on the callee.
6. Verify microphone audio is heard both ways.
7. Hang up and verify both UIs return to idle.
8. Verify a third user sees busy while one participant is in a call.
