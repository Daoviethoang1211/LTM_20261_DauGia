#include "client_net.h"
#include "protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static long long milliseconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int client_connect(ClientConnection *connection, const char *host, int port) {
    memset(connection, 0, sizeof *connection); connection->fd = -1;
    if (port < 1 || port > 65535) return -1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 5};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) < 0 ||
        connect(fd, (struct sockaddr *)&address, sizeof address) < 0) { close(fd); return -1; }
    connection->fd = fd;
    return 0;
}
void client_close(ClientConnection *connection) {
    if (connection->fd >= 0) close(connection->fd);
    connection->fd = -1; connection->received = 0;
}
int client_send(ClientConnection *connection, json_object *request) {
    const char *body = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
    size_t length = strlen(body);
    if (!length || length > FRAME_MAX) return -1;
    unsigned char buffer[FRAME_MAX + 4];
    frame_header(buffer, (uint32_t)length); memcpy(buffer + 4, body, length);
    size_t sent = 0;
    while (sent < length + 4) {
        ssize_t n = send(connection->fd, buffer + sent, length + 4 - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}
int client_receive(ClientConnection *connection, int timeout_ms, json_object **message) {
    long long deadline = milliseconds() + timeout_ms;
    *message = NULL;
    for (;;) {
        if (connection->received >= 4) {
            uint32_t length = frame_length(connection->input);
            if (!length || length > FRAME_MAX) return -1;
            if (connection->received >= length + 4u) {
                *message = protocol_parse(connection->input + 4, length);
                connection->received -= length + 4u;
                memmove(connection->input, connection->input + length + 4, connection->received);
                return *message ? 1 : -1;
            }
        }
        long long remaining = deadline - milliseconds();
        struct pollfd fd = {.fd = connection->fd, .events = POLLIN};
        int ready = poll(&fd, 1, remaining > 0 ? (int)remaining : 0);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return ready;
        ssize_t n = recv(connection->fd, connection->input + connection->received,
                         sizeof connection->input - connection->received, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (milliseconds() >= deadline) return 0;
            continue;
        }
        if (n <= 0) return -1;
        connection->received += (size_t)n;
    }
}
json_object *client_request(ClientConnection *connection, const char *command, json_object *payload,
                            ClientEvent callback, void *context) {
    char id[32]; snprintf(id, sizeof id, "%lu", ++connection->sequence);
    json_object *request = json_object_new_object();
    json_object_object_add(request, "type", json_object_new_string(command));
    json_object_object_add(request, "request_id", json_object_new_string(id));
    json_object_object_add(request, "payload", json_object_get(payload));
    int sent = client_send(connection, request);
    json_object_put(request);
    if (sent < 0) return NULL;
    long long deadline = milliseconds() + 30000;
    while (milliseconds() < deadline) {
        json_object *reply = NULL, *event = NULL, *rid = NULL;
        long long remaining = deadline - milliseconds();
        if (remaining <= 0) return NULL;
        int rc = client_receive(connection, (int)remaining, &reply);
        if (rc != 1) return NULL;
        if (json_object_object_get_ex(reply, "event", &event)) {
            if (callback) callback(reply, context);
            json_object_put(reply); continue;
        }
        json_object_object_get_ex(reply, "request_id", &rid);
        if (!json_object_is_type(rid, json_type_string) || strcmp(json_object_get_string(rid), id)) {
            json_object_put(reply); return NULL;
        }
        return reply;
    }
    return NULL;
}
