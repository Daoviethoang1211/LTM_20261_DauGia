#ifndef CLIENT_NET_H
#define CLIENT_NET_H
#include "frame.h"
#include <json-c/json.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int fd;
    unsigned long sequence;
    size_t received;
    unsigned char input[FRAME_MAX + 4];
} ClientConnection;
typedef void (*ClientEvent)(json_object *event, void *context);
int client_connect(ClientConnection *connection, const char *host, int port);
void client_close(ClientConnection *connection);
int client_send(ClientConnection *connection, json_object *request);
/* Returns 1 for a message (caller owns it), 0 on timeout, -1 on disconnection/error. */
int client_receive(ClientConnection *connection, int timeout_ms, json_object **message);
/* Payload is borrowed; response owned by caller. Events are borrowed during callback. */
json_object *client_request(ClientConnection *connection, const char *command, json_object *payload,
                            ClientEvent callback, void *context);
#ifdef __cplusplus
}
#endif
#endif
