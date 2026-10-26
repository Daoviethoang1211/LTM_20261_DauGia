#include "frame.h"
#include "protocol.h"
#include "storage.h"
#include "app.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CLIENT_MAX APP_CLIENT_MAX
#define OUTPUT_MAX (4 * (FRAME_MAX + 4))
typedef struct {
    int fd, draining, failed, slot;
    App *app;
    size_t received, queued;
    unsigned char input[FRAME_MAX + 4], output[OUTPUT_MAX];
    double progress;
} Client;
static volatile sig_atomic_t stopped;
static void stop_server(int sig) { (void)sig; stopped = 1; }
static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void log_event(const char *event, int fd) {
    time_t t = time(NULL);
    struct tm utc;
    char stamp[32];
    gmtime_r(&t, &utc);
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    fprintf(stderr, "%s event=%s fd=%d\n", stamp, event, fd);
}
static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static void disconnect(Client *c) {
    log_event("disconnect", c->fd);
    close(c->fd); c->fd = -1;
    app_disconnect(c->app, c->slot);
}
static int enqueue(Client *c, json_object *reply) {
    const char *body = json_object_to_json_string_ext(reply, JSON_C_TO_STRING_PLAIN);
    size_t length = strlen(body);
    if (length > FRAME_MAX) {
        json_object *id = NULL;
        json_object_object_get_ex(reply, "request_id", &id);
        if (id) json_object_get(id);
        json_object_put(reply);
        /* Oversized responses cannot be queued, but still correlate the error. */
        reply = protocol_error(id, "RESPONSE_TOO_LARGE");
        json_object_put(id);
        body = json_object_to_json_string_ext(reply, JSON_C_TO_STRING_PLAIN);
        length = strlen(body);
    }
    if (c->queued + length + 4 > OUTPUT_MAX) {
        json_object_put(reply); log_event("slow_client", c->fd); return -1;
    }
    if (!c->queued) c->progress = now();
    frame_header(c->output + c->queued, (uint32_t)length);
    memcpy(c->output + c->queued + 4, body, length);
    c->queued += length + 4;
    json_object_put(reply);
    return 0;
}
static void emit(void *context, int slot, json_object *message) {
    Client *clients = context;
    if (clients[slot].fd < 0) { json_object_put(message); return; }
    if (enqueue(&clients[slot], message) < 0) clients[slot].failed = 1;
}
static int process_frames(Client *c, long long starts) {
    size_t consumed = 0;
    while (!c->app->sessions[c->slot].pending && c->received - consumed >= 4) {
        uint32_t length = frame_length(c->input + consumed);
        if (!length || length > FRAME_MAX) {
            log_event("invalid_frame_size", c->fd);
            c->draining = 1; c->received = 0;
            return enqueue(c, protocol_error(NULL, "INVALID_FRAME_SIZE"));
        }
        if (c->received - consumed < length + 4u) break;
        json_object *reply = app_handle(c->app, c->slot, c->input + consumed + 4, length, starts);
        if (reply) {
            json_object *ok = NULL;
            json_object_object_get_ex(reply, "ok", &ok);
            log_event(json_object_get_boolean(ok) ? "request_ok" : "request_rejected", c->fd);
            if (enqueue(c, reply) < 0) return -1;
        }
        consumed += length + 4u;
    }
    c->received -= consumed;
    memmove(c->input, c->input + consumed, c->received);
    return 0;
}
static int receive_frames(Client *c, long long starts) {
    ssize_t n = recv(c->fd, c->input + c->received, sizeof c->input - c->received, 0);
    if (n == 0) {
        log_event(c->received ? "truncated_frame" : "read_eof", c->fd);
        c->draining = 1;
        return c->queued ? 0 : -1;
    }
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    c->received += (size_t)n; c->progress = now();
    return process_frames(c, starts);
}
static int flush(Client *c) {
    ssize_t n = send(c->fd, c->output, c->queued, MSG_NOSIGNAL);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    if (n == 0) return -1;
    c->queued -= (size_t)n;
    memmove(c->output, c->output + n, c->queued);
    c->progress = now();
    return c->draining && !c->queued ? -1 : 0;
}
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s BIND_IPV4 PORT DB_PATH (port 0 chooses a free port)\n", argv[0]); return 1; }
    char *end;
    errno = 0;
    long port = strtol(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || port < 0 || port > 65535) return 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) return 1;
    signal(SIGINT, stop_server); signal(SIGTERM, stop_server); signal(SIGPIPE, SIG_IGN);
    sqlite3 *db = NULL;
    long long starts;
    if (storage_open(&db, argv[3], &starts) < 0) return 1;
    int listener = socket(AF_INET, SOCK_STREAM, 0), reuse = 1, result = 1;
    Client *clients = calloc(CLIENT_MAX, sizeof *clients);
    if (!clients) { sqlite3_close(db); if (listener >= 0) close(listener); return 1; }
    for (int i = 0; i < CLIENT_MAX; i++) clients[i].fd = -1;
    App app;
    if (app_start(&app, db, emit, clients) < 0) {
        fprintf(stderr, "auth worker initialization failed\n");
        free(clients); if (listener >= 0) close(listener); sqlite3_close(db); return 1;
    }
    if (listener < 0) goto cleanup;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    if (nonblocking(listener) < 0 || bind(listener, (struct sockaddr *)&address, sizeof address) < 0 || listen(listener, 64) < 0) {
        perror("listen"); goto cleanup;
    }
    socklen_t size = sizeof address;
    if (getsockname(listener, (struct sockaddr *)&address, &size) < 0) goto cleanup;
    printf("READY %u\n", ntohs(address.sin_port)); fflush(stdout);
    log_event("server_started", listener);
    result = 0;
    while (!stopped) {
        app_collect(&app);
        for (int i = 0; i < CLIENT_MAX; i++) {
            Client *c = &clients[i];
            if (c->fd < 0) continue;
            if (c->failed || (!c->draining && process_frames(c, starts) < 0)) disconnect(c);
        }
        struct pollfd fds[CLIENT_MAX + 1] = {{.fd = listener, .events = POLLIN}};
        for (int i = 0; i < CLIENT_MAX; i++) {
            fds[i+1].fd = clients[i].fd;
            fds[i+1].events = (clients[i].draining || app.sessions[i].pending ? 0 : POLLIN) | (clients[i].queued ? POLLOUT : 0);
        }
        int ready = poll(fds, CLIENT_MAX + 1, 250);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); result = 1; break; }
        /* Process existing clients before accepting into vacant slots. */
        for (int i = 0; i < CLIENT_MAX; i++) {
            Client *c = &clients[i]; short events = fds[i+1].revents;
            if (c->fd < 0) continue;
            int failed = c->failed || (events & (POLLERR | POLLNVAL));
            if (!failed && !c->draining && !app.sessions[i].pending && (events & (POLLIN | POLLHUP))) failed = receive_frames(c, starts) < 0;
            if (!failed && app.sessions[i].pending && (events & POLLHUP)) failed = 1;
            if (!failed && (events & POLLOUT) && c->queued) failed = flush(c) < 0;
            if (!failed && (c->received || c->queued || c->draining) && now() - c->progress > 10) {
                log_event("io_timeout", c->fd); failed = 1;
            }
            if (failed) disconnect(c);
        }
        if (fds[0].revents & POLLIN) {
            /* Bound accepts per iteration so existing clients keep making progress. */
            for (int count = 0; count < CLIENT_MAX; count++) {
                int fd = accept(listener, NULL, NULL);
                if (fd < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) log_event("accept_error", listener); break; }
                int slot;
                for (slot = 0; slot < CLIENT_MAX && clients[slot].fd >= 0; slot++) {}
                if (slot == CLIENT_MAX || nonblocking(fd) < 0) { log_event("connection_rejected", fd); close(fd); continue; }
                Client *c = &clients[slot];
                c->fd = fd; c->received = c->queued = 0; c->draining = c->failed = 0; c->progress = now();
                c->slot = slot; c->app = &app; app_connect(&app, slot);
                log_event("connect", fd);
            }
        }
    }
cleanup:
    for (int i = 0; i < CLIENT_MAX; i++) if (clients[i].fd >= 0) disconnect(&clients[i]);
    free(clients);
    app_stop(&app);
    if (listener >= 0) close(listener);
    sqlite3_close(db);
    return result;
}
