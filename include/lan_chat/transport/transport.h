#ifndef LAN_CHAT_TRANSPORT_TRANSPORT_H
#define LAN_CHAT_TRANSPORT_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "lan_chat/core/framer.h"
#include "lan_chat/core/protocol.h"
#include "lan_chat/core/status.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LAN_CHAT_TRANSPORT_DEFAULT_BACKLOG = 16,
    LAN_CHAT_TRANSPORT_RECV_BUFFER_SIZE = 8192,
    LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE = LAN_CHAT_HEADER_LEN + LAN_CHAT_MAX_BODY_LEN
};

typedef struct lan_chat_transport {
    int initialized;
} lan_chat_transport_t;

typedef struct lan_chat_tcp_listener {
    uintptr_t socket;
    uint16_t port;
    int is_open;
} lan_chat_tcp_listener_t;

typedef struct lan_chat_tcp_connection {
    uintptr_t socket;
    int is_open;
    uint8_t recv_buffer[LAN_CHAT_TRANSPORT_RECV_BUFFER_SIZE];
    uint8_t framer_buffer[LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE];
    uint8_t send_queue[LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE];
    size_t send_len;
    size_t send_offset;
    lan_chat_framer_t framer;
} lan_chat_tcp_connection_t;

lan_chat_status_t lan_chat_transport_init(lan_chat_transport_t *transport);
void lan_chat_transport_shutdown(lan_chat_transport_t *transport);

lan_chat_status_t lan_chat_tcp_listener_open(
    lan_chat_tcp_listener_t *listener,
    const char *host,
    uint16_t port,
    int backlog);

lan_chat_status_t lan_chat_tcp_listener_accept(
    lan_chat_tcp_listener_t *listener,
    lan_chat_tcp_connection_t *out_connection);

uint16_t lan_chat_tcp_listener_port(const lan_chat_tcp_listener_t *listener);

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

lan_chat_status_t lan_chat_tcp_connection_flush(lan_chat_tcp_connection_t *connection);

lan_chat_status_t lan_chat_tcp_connection_recv_packet(
    lan_chat_tcp_connection_t *connection,
    uint8_t *out_packet,
    size_t out_packet_capacity,
    size_t *out_packet_len);

void lan_chat_tcp_connection_close(lan_chat_tcp_connection_t *connection);

#ifdef __cplusplus
}
#endif

#endif
