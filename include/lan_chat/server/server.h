#ifndef LAN_CHAT_SERVER_SERVER_H
#define LAN_CHAT_SERVER_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/status.h"
#include "lan_chat/storage/storage.h"
#include "lan_chat/transport/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lan_chat_server {
    lan_chat_storage_t *storage;
    uint16_t listen_port;
    uint32_t worker_thread_count;
    lan_chat_transport_t transport;
    lan_chat_tcp_listener_t listener;
    uint64_t next_session_id;
    int running;
    struct lan_chat_server_client *clients;
    size_t client_capacity;
    struct lan_chat_server_file_transfer *file_transfers;
    size_t file_transfer_capacity;
    uint64_t next_runtime_file_transfer_id;
    struct lan_chat_server_call *calls;
    size_t call_capacity;
    uint64_t next_call_id;
} lan_chat_server_t;

typedef struct lan_chat_server_config {
    const char *listen_host;
    uint16_t listen_port;
    uint32_t worker_thread_count;
    lan_chat_storage_t *storage;
} lan_chat_server_config_t;

lan_chat_status_t lan_chat_server_init(
    lan_chat_server_t *server,
    const lan_chat_server_config_t *config);

void lan_chat_server_shutdown(lan_chat_server_t *server);

lan_chat_status_t lan_chat_server_run_once(lan_chat_server_t *server);
uint16_t lan_chat_server_port(const lan_chat_server_t *server);

#ifdef __cplusplus
}
#endif

#endif
