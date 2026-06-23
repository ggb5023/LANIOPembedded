# LAN Chat Development Rules

## 1. Product Boundary

Phase 7 may start building a lightweight Qt GUI for LAN chat and one-to-one
audio calls. GUI work must stay tied to the current `chat_server` protocol and
must not introduce a separate product line.

The project still does not build a friends/contact social system. Users remain
in the all-users-can-communicate model unless a later phase explicitly changes
that boundary.

Out of scope for the current route:

- public internet traversal
- STUN/TURN deployment
- group calls
- multi-server federation
- end-to-end encryption
- mobile or web clients
- friends/contact approval workflow

## 2. Feature Order

Development order is fixed:

```text
private text
  -> group text
    -> private file transfer
      -> GUI one-to-one audio call
        -> GUI one-to-one video call
```

Each phase must finish a runnable closure before the next phase begins. Phase 7
is audio-only. Video UI, camera capture, H.264, and remote video rendering are
reserved for Phase 8.

## 3. Engineering Rules

- Define requirements, boundaries, protocol, and acceptance before broad coding.
- Keep default build and default CTest lightweight and independent from Qt,
  libdatachannel, real audio devices, and MySQL.
- Put heavy GUI/AV dependencies behind explicit CMake options.
- Keep C ABI free of Qt, C++ exceptions, MySQL private types, and media-library
  private types.
- Keep business code behind current storage/transport/protocol abstractions.
- Do not use reference-project absolute paths as production dependencies.
- Do not turn temporary local passwords, local machine paths, or one-off scripts
  into stable public interfaces.

## 4. Design Principles

The design principle remains: lightweight boundary, stable reliability.

Lightweight boundary:

- expose only the minimum interface required by the current phase;
- prefer small protocol groups and explicit runtime state over broad framework
  rewrites;
- avoid introducing a second signaling server while the current server can route
  LAN call signaling.

Stable reliability:

- every socket, storage, server, client, GUI, and media lifecycle must have
  explicit init/shutdown behavior;
- online runtime state must be cleared on disconnect;
- tests must cover protocol/state-machine behavior before manual media testing;
- each phase must be rebuildable from a clean build directory.
