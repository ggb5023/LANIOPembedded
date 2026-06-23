#ifndef LAN_CHAT_PLATFORM_WIN32_H
#define LAN_CHAT_PLATFORM_WIN32_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t lan_chat_socket_handle_t;

#define LAN_CHAT_SOCKET_INVALID ((lan_chat_socket_handle_t)(~(uintptr_t)0))

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

#ifdef __cplusplus
}
#endif

#endif
