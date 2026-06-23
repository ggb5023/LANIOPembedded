#include "lan_chat/transport/transport.h"

#include <string.h>

#include "lan_chat/platform/win32/platform_win32.h"

static void listener_reset(lan_chat_tcp_listener_t *listener)
{
    if (listener == 0) {
        return;
    }
    listener->socket = LAN_CHAT_SOCKET_INVALID;
    listener->port = 0;
    listener->is_open = 0;
}

static lan_chat_status_t connection_reset(lan_chat_tcp_connection_t *connection)
{
    lan_chat_status_t status;

    if (connection == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    memset(connection, 0, sizeof(*connection));
    connection->socket = LAN_CHAT_SOCKET_INVALID;
    status = lan_chat_framer_init(&connection->framer, connection->framer_buffer, sizeof(connection->framer_buffer));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }
    return LAN_CHAT_STATUS_OK;
}

static int connection_has_pending_send(const lan_chat_tcp_connection_t *connection)
{
    return connection != 0 && connection->send_offset < connection->send_len;
}

lan_chat_status_t lan_chat_transport_init(lan_chat_transport_t *transport)
{
    lan_chat_status_t status;

    if (transport == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    status = lan_chat_platform_win32_init();
    if (status != LAN_CHAT_STATUS_OK) {
        transport->initialized = 0;
        return status;
    }
    transport->initialized = 1;
    return LAN_CHAT_STATUS_OK;
}

void lan_chat_transport_shutdown(lan_chat_transport_t *transport)
{
    if (transport != 0) {
        transport->initialized = 0;
    }
    lan_chat_platform_win32_shutdown();
}

lan_chat_status_t lan_chat_tcp_listener_open(
    lan_chat_tcp_listener_t *listener,
    const char *host,
    uint16_t port,
    int backlog)
{
    lan_chat_socket_handle_t socket = LAN_CHAT_SOCKET_INVALID;
    lan_chat_status_t status;
    uint16_t local_port = 0;

    if (listener == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    listener_reset(listener);
    status = lan_chat_platform_tcp_listen(
        host,
        port,
        backlog > 0 ? backlog : LAN_CHAT_TRANSPORT_DEFAULT_BACKLOG,
        &socket);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    listener->socket = socket;
    listener->is_open = 1;
    status = lan_chat_platform_tcp_local_endpoint(socket, 0, 0, &local_port);
    listener->port = status == LAN_CHAT_STATUS_OK ? local_port : port;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_tcp_listener_accept(
    lan_chat_tcp_listener_t *listener,
    lan_chat_tcp_connection_t *out_connection)
{
    lan_chat_socket_handle_t socket = LAN_CHAT_SOCKET_INVALID;
    lan_chat_status_t status;

    if (listener == 0 || out_connection == 0 || !listener->is_open) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    status = lan_chat_platform_tcp_accept(listener->socket, &socket, 0, 0, 0);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    status = connection_reset(out_connection);
    if (status != LAN_CHAT_STATUS_OK) {
        lan_chat_platform_tcp_close(&socket);
        return status;
    }
    out_connection->socket = socket;
    out_connection->is_open = 1;
    return LAN_CHAT_STATUS_OK;
}

uint16_t lan_chat_tcp_listener_port(const lan_chat_tcp_listener_t *listener)
{
    return listener != 0 ? listener->port : 0;
}

void lan_chat_tcp_listener_close(lan_chat_tcp_listener_t *listener)
{
    if (listener == 0) {
        return;
    }
    lan_chat_platform_tcp_close(&listener->socket);
    listener_reset(listener);
}

lan_chat_status_t lan_chat_tcp_client_connect(
    lan_chat_tcp_connection_t *connection,
    const char *host,
    uint16_t port)
{
    lan_chat_socket_handle_t socket = LAN_CHAT_SOCKET_INVALID;
    lan_chat_status_t status;

    if (connection == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    status = connection_reset(connection);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    status = lan_chat_platform_tcp_connect(host, port, &socket);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    connection->socket = socket;
    connection->is_open = 1;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_tcp_connection_queue_packet(
    lan_chat_tcp_connection_t *connection,
    const lan_chat_header_t *header,
    const uint8_t *body,
    size_t body_len)
{
    lan_chat_status_t status;
    size_t packet_len = 0;

    if (connection == 0 || !connection->is_open) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (connection_has_pending_send(connection)) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }

    status = lan_chat_packet_pack(
        header,
        body,
        body_len,
        connection->send_queue,
        sizeof(connection->send_queue),
        &packet_len);
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    connection->send_len = packet_len;
    connection->send_offset = 0;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_tcp_connection_flush(lan_chat_tcp_connection_t *connection)
{
    lan_chat_status_t status;
    size_t sent = 0;

    if (connection == 0 || !connection->is_open) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    while (connection->send_offset < connection->send_len) {
        status = lan_chat_platform_tcp_send(
            connection->socket,
            connection->send_queue + connection->send_offset,
            connection->send_len - connection->send_offset,
            &sent);
        if (status == LAN_CHAT_STATUS_NEED_MORE_DATA) {
            return status;
        }
        if (status != LAN_CHAT_STATUS_OK) {
            lan_chat_tcp_connection_close(connection);
            return status;
        }
        if (sent == 0) {
            return LAN_CHAT_STATUS_NEED_MORE_DATA;
        }
        connection->send_offset += sent;
    }

    connection->send_len = 0;
    connection->send_offset = 0;
    return LAN_CHAT_STATUS_OK;
}

lan_chat_status_t lan_chat_tcp_connection_recv_packet(
    lan_chat_tcp_connection_t *connection,
    uint8_t *out_packet,
    size_t out_packet_capacity,
    size_t *out_packet_len)
{
    lan_chat_status_t status;
    size_t received = 0;

    if (out_packet_len != 0) {
        *out_packet_len = 0;
    }
    if (connection == 0 || !connection->is_open || out_packet == 0 || out_packet_len == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    status = lan_chat_framer_next(&connection->framer, out_packet, out_packet_capacity, out_packet_len);
    if (status == LAN_CHAT_STATUS_OK) {
        return LAN_CHAT_STATUS_OK;
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
        lan_chat_tcp_connection_close(connection);
        return status;
    }

    status = lan_chat_platform_tcp_recv(
        connection->socket,
        connection->recv_buffer,
        sizeof(connection->recv_buffer),
        &received);
    if (status == LAN_CHAT_STATUS_NEED_MORE_DATA) {
        return status;
    }
    if (status != LAN_CHAT_STATUS_OK) {
        lan_chat_tcp_connection_close(connection);
        return status;
    }

    status = lan_chat_framer_feed(&connection->framer, connection->recv_buffer, received);
    if (status != LAN_CHAT_STATUS_OK) {
        lan_chat_tcp_connection_close(connection);
        return status;
    }

    status = lan_chat_framer_next(&connection->framer, out_packet, out_packet_capacity, out_packet_len);
    if (status != LAN_CHAT_STATUS_OK && status != LAN_CHAT_STATUS_NEED_MORE_DATA) {
        lan_chat_tcp_connection_close(connection);
    }
    return status;
}

void lan_chat_tcp_connection_close(lan_chat_tcp_connection_t *connection)
{
    if (connection == 0) {
        return;
    }
    lan_chat_platform_tcp_close(&connection->socket);
    (void)connection_reset(connection);
}
