#include "lan_chat/platform/win32/platform_win32.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifdef _WIN32
static int g_winsock_ref_count = 0;

static SOCKET to_socket(lan_chat_socket_handle_t socket)
{
    return (SOCKET)socket;
}

static lan_chat_socket_handle_t from_socket(SOCKET socket)
{
    return (lan_chat_socket_handle_t)socket;
}

static int socket_is_invalid(lan_chat_socket_handle_t socket)
{
    return socket == LAN_CHAT_SOCKET_INVALID;
}

static lan_chat_status_t map_wsa_error(int error_code)
{
    switch (error_code) {
    case WSAEWOULDBLOCK:
    case WSAEINPROGRESS:
    case WSAEALREADY:
        return LAN_CHAT_STATUS_NEED_MORE_DATA;
    default:
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
}

static lan_chat_status_t make_port_string(uint16_t port, char *buffer, size_t buffer_len)
{
    int written;

    if (buffer == 0 || buffer_len == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    written = snprintf(buffer, buffer_len, "%u", (unsigned int)port);
    if (written < 0 || (size_t)written >= buffer_len) {
        return LAN_CHAT_STATUS_BUFFER_TOO_SMALL;
    }
    return LAN_CHAT_STATUS_OK;
}

static lan_chat_status_t set_reuse_addr(SOCKET socket)
{
    int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&enabled, sizeof(enabled)) == SOCKET_ERROR) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
    return LAN_CHAT_STATUS_OK;
}
#endif

lan_chat_status_t lan_chat_platform_win32_init(void)
{
#ifdef _WIN32
    WSADATA wsa_data;
    if (g_winsock_ref_count > 0) {
        ++g_winsock_ref_count;
        return LAN_CHAT_STATUS_OK;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
    g_winsock_ref_count = 1;
#endif
    return LAN_CHAT_STATUS_OK;
}

void lan_chat_platform_win32_shutdown(void)
{
#ifdef _WIN32
    if (g_winsock_ref_count <= 0) {
        return;
    }
    --g_winsock_ref_count;
    if (g_winsock_ref_count > 0) {
        return;
    }
    WSACleanup();
#endif
}

lan_chat_status_t lan_chat_platform_tcp_set_nonblocking(
    lan_chat_socket_handle_t socket,
    int enabled)
{
#ifdef _WIN32
    u_long mode = enabled != 0 ? 1UL : 0UL;

    if (socket_is_invalid(socket)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (ioctlsocket(to_socket(socket), FIONBIO, &mode) == SOCKET_ERROR) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
    return LAN_CHAT_STATUS_OK;
#else
    (void)socket;
    (void)enabled;
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
#endif
}

lan_chat_status_t lan_chat_platform_tcp_listen(
    const char *host,
    uint16_t port,
    int backlog,
    lan_chat_socket_handle_t *out_listener)
{
#ifdef _WIN32
    char service[16];
    struct addrinfo hints;
    struct addrinfo *results = 0;
    struct addrinfo *it = 0;
    SOCKET listen_socket = INVALID_SOCKET;
    lan_chat_status_t status;
    const char *node = (host != 0 && host[0] != '\0') ? host : 0;

    if (out_listener == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_listener = LAN_CHAT_SOCKET_INVALID;

    status = make_port_string(port, service, sizeof(service));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(node, service, &hints, &results) != 0) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }

    for (it = results; it != 0; it = it->ai_next) {
        listen_socket = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (listen_socket == INVALID_SOCKET) {
            continue;
        }
        (void)set_reuse_addr(listen_socket);
        if (bind(listen_socket, it->ai_addr, (int)it->ai_addrlen) == 0) {
            break;
        }
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
    }

    freeaddrinfo(results);

    if (listen_socket == INVALID_SOCKET) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
    if (listen(listen_socket, backlog > 0 ? backlog : 16) == SOCKET_ERROR) {
        closesocket(listen_socket);
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }

    *out_listener = from_socket(listen_socket);
    status = lan_chat_platform_tcp_set_nonblocking(*out_listener, 1);
    if (status != LAN_CHAT_STATUS_OK) {
        lan_chat_platform_tcp_close(out_listener);
        return status;
    }
    return LAN_CHAT_STATUS_OK;
#else
    (void)host;
    (void)port;
    (void)backlog;
    if (out_listener != 0) {
        *out_listener = LAN_CHAT_SOCKET_INVALID;
    }
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
#endif
}

lan_chat_status_t lan_chat_platform_tcp_accept(
    lan_chat_socket_handle_t listener,
    lan_chat_socket_handle_t *out_socket,
    char *remote_ip,
    size_t remote_ip_capacity,
    uint16_t *out_remote_port)
{
#ifdef _WIN32
    struct sockaddr_storage remote_addr;
    int remote_len = (int)sizeof(remote_addr);
    SOCKET accepted;
    lan_chat_status_t status;

    if (out_socket == 0 || socket_is_invalid(listener)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_socket = LAN_CHAT_SOCKET_INVALID;
    if (remote_ip != 0 && remote_ip_capacity > 0) {
        remote_ip[0] = '\0';
    }
    if (out_remote_port != 0) {
        *out_remote_port = 0;
    }

    accepted = accept(to_socket(listener), (struct sockaddr *)&remote_addr, &remote_len);
    if (accepted == INVALID_SOCKET) {
        return map_wsa_error(WSAGetLastError());
    }

    if (remote_addr.ss_family == AF_INET) {
        struct sockaddr_in *addr = (struct sockaddr_in *)&remote_addr;
        if (remote_ip != 0 && remote_ip_capacity > 0) {
            (void)inet_ntop(AF_INET, &addr->sin_addr, remote_ip, (DWORD)remote_ip_capacity);
        }
        if (out_remote_port != 0) {
            *out_remote_port = ntohs(addr->sin_port);
        }
    }

    *out_socket = from_socket(accepted);
    status = lan_chat_platform_tcp_set_nonblocking(*out_socket, 1);
    if (status != LAN_CHAT_STATUS_OK) {
        lan_chat_platform_tcp_close(out_socket);
        return status;
    }
    return LAN_CHAT_STATUS_OK;
#else
    (void)listener;
    (void)remote_ip;
    (void)remote_ip_capacity;
    if (out_socket != 0) {
        *out_socket = LAN_CHAT_SOCKET_INVALID;
    }
    if (out_remote_port != 0) {
        *out_remote_port = 0;
    }
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
#endif
}

lan_chat_status_t lan_chat_platform_tcp_connect(
    const char *host,
    uint16_t port,
    lan_chat_socket_handle_t *out_socket)
{
#ifdef _WIN32
    char service[16];
    struct addrinfo hints;
    struct addrinfo *results = 0;
    struct addrinfo *it = 0;
    SOCKET connected_socket = INVALID_SOCKET;
    lan_chat_status_t status;

    if (host == 0 || host[0] == '\0' || out_socket == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    *out_socket = LAN_CHAT_SOCKET_INVALID;

    status = make_port_string(port, service, sizeof(service));
    if (status != LAN_CHAT_STATUS_OK) {
        return status;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, service, &hints, &results) != 0) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }

    for (it = results; it != 0; it = it->ai_next) {
        connected_socket = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (connected_socket == INVALID_SOCKET) {
            continue;
        }
        if (connect(connected_socket, it->ai_addr, (int)it->ai_addrlen) == 0) {
            break;
        }
        closesocket(connected_socket);
        connected_socket = INVALID_SOCKET;
    }

    freeaddrinfo(results);

    if (connected_socket == INVALID_SOCKET) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }

    *out_socket = from_socket(connected_socket);
    status = lan_chat_platform_tcp_set_nonblocking(*out_socket, 1);
    if (status != LAN_CHAT_STATUS_OK) {
        lan_chat_platform_tcp_close(out_socket);
        return status;
    }
    return LAN_CHAT_STATUS_OK;
#else
    (void)host;
    (void)port;
    if (out_socket != 0) {
        *out_socket = LAN_CHAT_SOCKET_INVALID;
    }
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
#endif
}

lan_chat_status_t lan_chat_platform_tcp_send(
    lan_chat_socket_handle_t socket,
    const uint8_t *data,
    size_t data_len,
    size_t *out_sent)
{
#ifdef _WIN32
    int chunk_len;
    int sent;

    if (out_sent != 0) {
        *out_sent = 0;
    }
    if (socket_is_invalid(socket) || out_sent == 0 || (data == 0 && data_len > 0)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (data_len == 0) {
        return LAN_CHAT_STATUS_OK;
    }

    chunk_len = data_len > (size_t)INT_MAX ? INT_MAX : (int)data_len;
    sent = send(to_socket(socket), (const char *)data, chunk_len, 0);
    if (sent == SOCKET_ERROR) {
        return map_wsa_error(WSAGetLastError());
    }
    if (sent < 0) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
    *out_sent = (size_t)sent;
    return LAN_CHAT_STATUS_OK;
#else
    (void)socket;
    (void)data;
    (void)data_len;
    if (out_sent != 0) {
        *out_sent = 0;
    }
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
#endif
}

lan_chat_status_t lan_chat_platform_tcp_recv(
    lan_chat_socket_handle_t socket,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *out_received)
{
#ifdef _WIN32
    int chunk_len;
    int received;

    if (out_received != 0) {
        *out_received = 0;
    }
    if (socket_is_invalid(socket) || buffer == 0 || buffer_capacity == 0 || out_received == 0) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }

    chunk_len = buffer_capacity > (size_t)INT_MAX ? INT_MAX : (int)buffer_capacity;
    received = recv(to_socket(socket), (char *)buffer, chunk_len, 0);
    if (received == SOCKET_ERROR) {
        return map_wsa_error(WSAGetLastError());
    }
    if (received == 0) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
    if (received < 0) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
    *out_received = (size_t)received;
    return LAN_CHAT_STATUS_OK;
#else
    (void)socket;
    (void)buffer;
    (void)buffer_capacity;
    if (out_received != 0) {
        *out_received = 0;
    }
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
#endif
}

lan_chat_status_t lan_chat_platform_tcp_local_endpoint(
    lan_chat_socket_handle_t socket,
    char *local_ip,
    size_t local_ip_capacity,
    uint16_t *out_local_port)
{
#ifdef _WIN32
    struct sockaddr_storage local_addr;
    int local_len = (int)sizeof(local_addr);

    if (socket_is_invalid(socket)) {
        return LAN_CHAT_STATUS_INVALID_ARGUMENT;
    }
    if (local_ip != 0 && local_ip_capacity > 0) {
        local_ip[0] = '\0';
    }
    if (out_local_port != 0) {
        *out_local_port = 0;
    }

    if (getsockname(to_socket(socket), (struct sockaddr *)&local_addr, &local_len) == SOCKET_ERROR) {
        return LAN_CHAT_STATUS_NETWORK_ERROR;
    }
    if (local_addr.ss_family == AF_INET) {
        struct sockaddr_in *addr = (struct sockaddr_in *)&local_addr;
        if (local_ip != 0 && local_ip_capacity > 0) {
            (void)inet_ntop(AF_INET, &addr->sin_addr, local_ip, (DWORD)local_ip_capacity);
        }
        if (out_local_port != 0) {
            *out_local_port = ntohs(addr->sin_port);
        }
    }
    return LAN_CHAT_STATUS_OK;
#else
    (void)socket;
    if (local_ip != 0 && local_ip_capacity > 0) {
        local_ip[0] = '\0';
    }
    if (out_local_port != 0) {
        *out_local_port = 0;
    }
    return LAN_CHAT_STATUS_NOT_IMPLEMENTED;
#endif
}

void lan_chat_platform_tcp_close(lan_chat_socket_handle_t *socket)
{
#ifdef _WIN32
    if (socket == 0 || socket_is_invalid(*socket)) {
        return;
    }
    closesocket(to_socket(*socket));
    *socket = LAN_CHAT_SOCKET_INVALID;
#else
    if (socket != 0) {
        *socket = LAN_CHAT_SOCKET_INVALID;
    }
#endif
}
