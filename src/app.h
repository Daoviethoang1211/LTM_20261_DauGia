#ifndef APP_H
#define APP_H
#include "auth_worker.h"
#include <json-c/json.h>
#include <sqlite3.h>
#define APP_CLIENT_MAX 64
#define APP_CATALOG_MAX 30
typedef struct {
    uint64_t generation;
    long long user_id, room_id;
    int connected, pending;
    char username[33];
} Session;
/* emit takes ownership of the JSON value. Called only by the event-loop thread. */
typedef void (*AppEmit)(void *context, int slot, json_object *value);
typedef struct {
    sqlite3 *db;
    AuthWorker auth;
    Session sessions[APP_CLIENT_MAX];
    AppEmit emit;
    void *context;
} App;
int app_start(App *app, sqlite3 *db, AppEmit emit, void *context);
void app_stop(App *app);
void app_connect(App *app, int slot);
void app_disconnect(App *app, int slot);
void app_collect(App *app);
json_object *app_handle(App *app, int slot, const unsigned char *body, size_t length, long long starts);
#endif
