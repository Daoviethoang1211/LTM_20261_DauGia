#include "app.h"
#include "protocol.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3_stmt *prepare(App *app, const char *sql, const char *format, ...) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    va_list args;
    va_start(args, format);
    for (int i = 0; format[i]; i++) {
        int rc = format[i] == 's'
            ? sqlite3_bind_text(stmt, i + 1, va_arg(args, const char *), -1, SQLITE_TRANSIENT)
            : sqlite3_bind_int64(stmt, i + 1, va_arg(args, long long));
        if (rc != SQLITE_OK) { sqlite3_finalize(stmt); stmt = NULL; break; }
    }
    va_end(args);
    return stmt;
}
static int execute(sqlite3_stmt *stmt) {
    if (!stmt) return -1;
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}
static json_object *row(sqlite3_stmt *stmt) {
    json_object *value = json_object_new_object();
    for (int i = 0; i < sqlite3_column_count(stmt); i++) {
        json_object *field = NULL;
        if (sqlite3_column_type(stmt, i) == SQLITE_INTEGER)
            field = json_object_new_int64(sqlite3_column_int64(stmt, i));
        else if (sqlite3_column_type(stmt, i) != SQLITE_NULL)
            field = json_object_new_string((const char *)sqlite3_column_text(stmt, i));
        json_object_object_add(value, sqlite3_column_name(stmt, i), field);
    }
    return value;
}
static json_object *rows(sqlite3_stmt *stmt) {
    if (!stmt) return NULL;
    json_object *array = json_object_new_array();
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) json_object_array_add(array, row(stmt));
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { json_object_put(array); return NULL; }
    return array;
}
static json_object *get(json_object *object, const char *name) {
    json_object *value = NULL;
    json_object_object_get_ex(object, name, &value);
    return value;
}
static const char *text(json_object *object, const char *key, size_t minimum, size_t maximum) {
    json_object *value = get(object, key);
    if (!json_object_is_type(value, json_type_string)) return NULL;
    const char *s = json_object_get_string(value);
    size_t n = (size_t)json_object_get_string_len(value);
    if (n < minimum || n > maximum || strlen(s) != n) return NULL;
    /* Do not let control characters manipulate terminal output or logs. */
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || s[i] == 127) return NULL;
    return s;
}
static long long number(json_object *object, const char *key, long long minimum, long long maximum) {
    json_object *value = get(object, key);
    if (!json_object_is_type(value, json_type_int)) return -1;
    long long n = json_object_get_int64(value);
    return n >= minimum && n <= maximum ? n : -1;
}
static int username_valid(const char *s) {
    if (!s) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}
static json_object *error(json_object *id, const char *code) { return protocol_error(id, code); }
static json_object *empty_success(json_object *id) { return protocol_success(id, json_object_new_object()); }
static json_object *database_error(App *app, json_object *id) {
    (void)app;
    fprintf(stderr, "event=database_error\n");
    return error(id, "DATABASE_ERROR");
}
static json_object *room_get(App *app, long long id) {
    sqlite3_stmt *stmt = prepare(app, "SELECT id,owner_user_id,name,description,status,version,published_at FROM rooms WHERE id=?", "i", id);
    if (!stmt) return NULL;
    json_object *room = sqlite3_step(stmt) == SQLITE_ROW ? row(stmt) : NULL;
    sqlite3_finalize(stmt);
    return room;
}
static int visible(Session *session, json_object *room) {
    return room && (json_object_get_int64(get(room, "owner_user_id")) == session->user_id ||
        strcmp(json_object_get_string(get(room, "status")), "DRAFT") != 0);
}
static json_object *items_get(App *app, long long room_id) {
    return rows(prepare(app, "SELECT id,room_id,title,description,starting_price_vnd,buy_now_price_vnd,planned_start_at,duration_sec,queue_position,status FROM items WHERE room_id=? ORDER BY queue_position,id", "i", room_id));
}
static json_object *members_get(App *app, long long room_id) {
    json_object *members = json_object_new_array();
    for (int i = 0; i < APP_CLIENT_MAX; i++) {
        Session *s = &app->sessions[i];
        if (s->connected && s->room_id == room_id && s->user_id) {
            json_object *member = json_object_new_object();
            json_object_object_add(member, "user_id", json_object_new_int64(s->user_id));
            json_object_object_add(member, "username", json_object_new_string(s->username));
            json_object_array_add(members, member);
        }
    }
    return members;
}
static json_object *snapshot(App *app, long long room_id) {
    json_object *room = room_get(app, room_id), *items = items_get(app, room_id);
    if (!room || !items) { json_object_put(room); json_object_put(items); return NULL; }
    json_object *data = json_object_new_object();
    json_object_object_add(data, "room", room);
    json_object_object_add(data, "items", items);
    json_object_object_add(data, "members", members_get(app, room_id));
    return data;
}
static int bump(App *app, long long room_id) {
    return execute(prepare(app, "UPDATE rooms SET version=version+1 WHERE id=?", "i", room_id));
}
static int mutate_item(App *app, sqlite3_stmt *stmt, long long room_id) {
    if (!stmt) return -1;
    if (sqlite3_exec(app->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) { sqlite3_finalize(stmt); return -1; }
    int failed = execute(stmt) < 0;
    if (!failed) failed = bump(app, room_id) < 0;
    if (!failed) failed = sqlite3_exec(app->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK;
    if (failed) sqlite3_exec(app->db, "ROLLBACK", NULL, NULL, NULL);
    return failed ? -1 : 0;
}
static void membership_event(App *app, int slot, long long room_id, const char *event) {
    Session *s = &app->sessions[slot];
    json_object *room = room_get(app, room_id);
    if (!room) return;
    json_object *message = json_object_new_object();
    json_object_object_add(message, "event", json_object_new_string(event));
    json_object_object_add(message, "room_id", json_object_new_int64(room_id));
    json_object_object_add(message, "version", json_object_get(get(room, "version")));
    json_object_object_add(message, "user_id", json_object_new_int64(s->user_id));
    json_object_object_add(message, "username", json_object_new_string(s->username));
    for (int i = 0; i < APP_CLIENT_MAX; i++)
        if (i != slot && app->sessions[i].connected && app->sessions[i].room_id == room_id)
            app->emit(app->context, i, json_object_get(message));
    json_object_put(message); json_object_put(room);
}
static void leave(App *app, int slot) {
    Session *s = &app->sessions[slot];
    if (s->room_id) {
        long long room_id = s->room_id;
        s->room_id = 0;
        if (bump(app, room_id) == 0) membership_event(app, slot, room_id, "MEMBER_LEFT");
    }
}
int app_start(App *app, sqlite3 *db, AppEmit emit, void *context) {
    memset(app, 0, sizeof *app); app->db = db; app->emit = emit; app->context = context;
    return auth_worker_start(&app->auth);
}
void app_stop(App *app) { auth_worker_stop(&app->auth); }
void app_connect(App *app, int slot) {
    uint64_t generation = app->sessions[slot].generation + 1;
    memset(&app->sessions[slot], 0, sizeof(Session));
    app->sessions[slot].generation = generation; app->sessions[slot].connected = 1;
}
void app_disconnect(App *app, int slot) {
    leave(app, slot);
    app->sessions[slot].connected = 0;
    app->sessions[slot].user_id = 0;
}
static json_object *auth_begin(App *app, int slot, json_object *id, const char *command, json_object *payload) {
    Session *s = &app->sessions[slot];
    int change = strcmp(command, "PASSWORD_CHANGE") == 0;
    int registration = strcmp(command, "REGISTER") == 0;
    if (change && !s->user_id) return error(id, "AUTH_REQUIRED");
    if (!change && s->user_id) return error(id, "ALREADY_AUTHENTICATED");
    const char *username = change ? s->username : text(payload, "username", 3, 32);
    const char *password = text(payload, change ? "old_password" : "password", 8, 128);
    const char *new_password = change ? text(payload, "new_password", 8, 128) : NULL;
    if (!username_valid(username) || !password || (change && !new_password)) return error(id, "INVALID_REQUEST");
    AuthJob *job = calloc(1, sizeof *job);
    if (!job) return error(id, "INTERNAL_ERROR");
    job->slot = slot; job->generation = s->generation;
    job->operation = registration ? AUTH_REGISTER : change ? AUTH_PASSWORD : AUTH_LOGIN;
    strcpy(job->request_id, json_object_get_string(id)); strcpy(job->username, username);
    strcpy(job->password, password);
    if (change) strcpy(job->new_password, new_password);
    sqlite3_stmt *stmt = prepare(app, "SELECT id,password_hash FROM users WHERE username=?", "s", username);
    if (!stmt) { auth_job_free(job); return database_error(app, id); }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        job->user_id = sqlite3_column_int64(stmt, 0);
        const char *hash = (const char *)sqlite3_column_text(stmt, 1);
        if (strlen(hash) >= sizeof job->hash) { sqlite3_finalize(stmt); auth_job_free(job); return database_error(app, id); }
        strcpy(job->hash, hash);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) { auth_job_free(job); return database_error(app, id); }
    if ((registration && job->user_id) || (!registration && !job->user_id)) {
        auth_job_free(job); return error(id, registration ? "USERNAME_TAKEN" : "INVALID_CREDENTIALS");
    }
    if (auth_worker_submit(&app->auth, job) < 0) { auth_job_free(job); return error(id, "SERVER_BUSY"); }
    s->pending = 1;
    return NULL;
}
void app_collect(App *app) {
    AuthJob *job;
    while ((job = auth_worker_collect(&app->auth))) {
        Session *s = &app->sessions[job->slot];
        if (!s->connected || s->generation != job->generation) { auth_job_free(job); continue; }
        s->pending = 0;
        json_object *id = json_object_new_string(job->request_id), *reply = NULL;
        if (!job->ok) reply = error(id, job->operation == AUTH_REGISTER ? "INTERNAL_ERROR" : "INVALID_CREDENTIALS");
        else if (job->operation == AUTH_REGISTER) {
            sqlite3_stmt *stmt = prepare(app, "INSERT INTO users(username,password_hash) VALUES(?,?)", "ss", job->username, job->hash);
            if (execute(stmt) < 0) reply = error(id, sqlite3_errcode(app->db) == SQLITE_CONSTRAINT ? "USERNAME_TAKEN" : "DATABASE_ERROR");
            else {
                json_object *data = json_object_new_object();
                json_object_object_add(data, "user_id", json_object_new_int64(sqlite3_last_insert_rowid(app->db)));
                reply = protocol_success(id, data);
            }
        } else if (job->operation == AUTH_PASSWORD) {
            if (execute(prepare(app, "UPDATE users SET password_hash=? WHERE id=?", "si", job->hash, s->user_id)) < 0)
                reply = database_error(app, id);
            else reply = empty_success(id);
        } else {
            int active = 0;
            for (int i = 0; i < APP_CLIENT_MAX; i++)
                if (app->sessions[i].connected && app->sessions[i].user_id == job->user_id) active = 1;
            if (active) reply = error(id, "ALREADY_LOGGED_IN");
            else {
                /* A password may have changed while this verification was queued. */
                sqlite3_stmt *stmt = prepare(app, "SELECT password_hash FROM users WHERE id=?", "i", job->user_id);
                int current = stmt && sqlite3_step(stmt) == SQLITE_ROW && strcmp((const char *)sqlite3_column_text(stmt, 0), job->hash) == 0;
                sqlite3_finalize(stmt);
                if (!current) reply = error(id, "INVALID_CREDENTIALS");
                else {
                    s->user_id = job->user_id; strcpy(s->username, job->username);
                    json_object *data = json_object_new_object();
                    json_object_object_add(data, "user_id", json_object_new_int64(s->user_id));
                    json_object_object_add(data, "username", json_object_new_string(s->username));
                    reply = protocol_success(id, data);
                }
            }
        }
        fprintf(stderr, "event=auth_completed slot=%d user_id=%lld ok=%d\n", job->slot, s->user_id, json_object_get_boolean(get(reply, "ok")));
        app->emit(app->context, job->slot, reply);
        json_object_put(id); auth_job_free(job);
    }
}

static json_object *room_command(App *app, int slot, json_object *id, const char *command, json_object *payload) {
    Session *s = &app->sessions[slot];
    if (!strcmp(command, "ROOM_CREATE")) {
        const char *name = text(payload, "name", 1, 120), *description = text(payload, "description", 0, 512);
        if (!name || !description) return error(id, "INVALID_REQUEST");
        if (execute(prepare(app, "INSERT INTO rooms(owner_user_id,name,description) VALUES(?,?,?)", "iss", s->user_id, name, description)) < 0)
            return database_error(app, id);
        json_object *data = room_get(app, sqlite3_last_insert_rowid(app->db));
        return data ? protocol_success(id, data) : database_error(app, id);
    }
    if (!strcmp(command, "ROOM_LIST")) {
        long long offset = get(payload, "offset") ? number(payload, "offset", 0, 1000000) : 0;
        if (offset < 0) return error(id, "INVALID_REQUEST");
        json_object *array = rows(prepare(app, "SELECT id,owner_user_id,name,description,status,version,published_at FROM rooms WHERE owner_user_id=? OR status<>'DRAFT' ORDER BY id LIMIT 20 OFFSET ?", "ii", s->user_id, offset));
        if (!array) return database_error(app, id);
        json_object *data = json_object_new_object();
        json_object_object_add(data, "next_offset", json_object_array_length(array) == 20 ? json_object_new_int64(offset + 20) : NULL);
        json_object_object_add(data, "rooms", array);
        return protocol_success(id, data);
    }
    if (!strcmp(command, "ROOM_LEAVE")) { leave(app, slot); return empty_success(id); }
    long long room_id = number(payload, "room_id", 1, INT64_MAX);
    if (room_id < 0) return error(id, "INVALID_REQUEST");
    json_object *room = room_get(app, room_id);
    if (!visible(s, room)) { json_object_put(room); return error(id, "NOT_FOUND"); }
    int owner = json_object_get_int64(get(room, "owner_user_id")) == s->user_id;
    int draft = !strcmp(json_object_get_string(get(room, "status")), "DRAFT");
    json_object_put(room);
    if (!strcmp(command, "ROOM_PUBLISH")) {
        if (!owner) return error(id, "FORBIDDEN");
        if (!draft) return error(id, "ROOM_LOCKED");
        json_object *items = items_get(app, room_id);
        if (!items) return database_error(app, id);
        size_t count = json_object_array_length(items);
        long long previous = 0, now = (long long)time(NULL);
        int valid = count > 0;
        for (size_t i = 0; i < count; i++) {
            json_object *item = json_object_array_get_idx(items, i);
            long long start = json_object_get_int64(get(item, "planned_start_at"));
            if (start <= now || start < previous) valid = 0;
            previous = start;
        }
        json_object_put(items);
        if (!valid) return error(id, "INVALID_CATALOG");
        if (execute(prepare(app, "UPDATE rooms SET status='PUBLISHED',published_at=?,version=version+1 WHERE id=? AND status='DRAFT'", "ii", now, room_id)) < 0)
            return database_error(app, id);
        json_object *data = snapshot(app, room_id);
        return data ? protocol_success(id, data) : database_error(app, id);
    }
    if (!strcmp(command, "ROOM_JOIN")) {
        if (s->room_id && s->room_id != room_id) return error(id, "ALREADY_IN_ROOM");
        if (s->room_id != room_id) {
            if (bump(app, room_id) < 0) return database_error(app, id);
            s->room_id = room_id;
            membership_event(app, slot, room_id, "MEMBER_JOINED");
        }
    }
    json_object *data = snapshot(app, room_id);
    return data ? protocol_success(id, data) : database_error(app, id);
}

static json_object *item_command(App *app, int slot, json_object *id, const char *command, json_object *payload) {
    Session *s = &app->sessions[slot];
    long long room_id = number(payload, "room_id", 1, INT64_MAX);
    if (room_id < 0) return error(id, "INVALID_REQUEST");
    json_object *room = room_get(app, room_id);
    if (!visible(s, room)) { json_object_put(room); return error(id, "NOT_FOUND"); }
    int owner = json_object_get_int64(get(room, "owner_user_id")) == s->user_id;
    int draft = !strcmp(json_object_get_string(get(room, "status")), "DRAFT");
    json_object_put(room);
    json_object *items = items_get(app, room_id);
    if (!items) return database_error(app, id);
    if (!strcmp(command, "ITEM_LIST")) {
        json_object *data = json_object_new_object(); json_object_object_add(data, "items", items);
        return protocol_success(id, data);
    }
    size_t count = json_object_array_length(items);
    if (!owner || !draft) { json_object_put(items); return error(id, !owner ? "FORBIDDEN" : "ROOM_LOCKED"); }
    if (!strcmp(command, "ITEM_REORDER")) {
        json_object *order = get(payload, "item_ids");
        int valid = json_object_is_type(order, json_type_array) && json_object_array_length(order) == count;
        for (size_t i = 0; valid && i < count; i++) {
            json_object *entry = json_object_array_get_idx(order, i);
            if (!json_object_is_type(entry, json_type_int)) { valid = 0; break; }
            long long item_id = json_object_get_int64(entry);
            int found = 0;
            for (size_t j = 0; j < count; j++) if (json_object_get_int64(get(json_object_array_get_idx(items, j), "id")) == item_id) found = 1;
            for (size_t j = 0; j < i; j++) if (json_object_get_int64(json_object_array_get_idx(order, j)) == item_id) found = 0;
            if (!found) valid = 0;
        }
        json_object_put(items);
        if (!valid) return error(id, "INVALID_REQUEST");
        if (sqlite3_exec(app->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return database_error(app, id);
        int failed = 0;
        for (size_t i = 0; i < count && !failed; i++)
            failed = execute(prepare(app, "UPDATE items SET queue_position=? WHERE id=? AND room_id=?", "iii", (long long)i + 1, (long long)json_object_get_int64(json_object_array_get_idx(order, i)), room_id)) < 0;
        if (!failed) failed = bump(app, room_id) < 0;
        if (!failed) failed = sqlite3_exec(app->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK;
        if (failed) { sqlite3_exec(app->db, "ROLLBACK", NULL, NULL, NULL); return database_error(app, id); }
        return empty_success(id);
    }
    long long item_id = 0, next_position = 1;
    if (!strcmp(command, "ITEM_CREATE")) {
        if (count >= APP_CATALOG_MAX) { json_object_put(items); return error(id, "CATALOG_FULL"); }
        if (count) next_position = json_object_get_int64(get(json_object_array_get_idx(items, count - 1), "queue_position")) + 1;
    } else {
        item_id = number(payload, "item_id", 1, INT64_MAX);
        int found = 0;
        for (size_t i = 0; i < count; i++) if (json_object_get_int64(get(json_object_array_get_idx(items, i), "id")) == item_id) found = 1;
        if (!found) { json_object_put(items); return error(id, item_id < 0 ? "INVALID_REQUEST" : "NOT_FOUND"); }
    }
    json_object_put(items);
    if (!strcmp(command, "ITEM_DELETE")) {
        if (mutate_item(app, prepare(app, "DELETE FROM items WHERE id=? AND room_id=?", "ii", item_id, room_id), room_id) < 0)
            return database_error(app, id);
        return empty_success(id);
    }
    const char *title = text(payload, "title", 1, 120), *description = text(payload, "description", 0, 512);
    long long start = number(payload, "starting_price_vnd", 0, 1000000000000000LL);
    long long buy = number(payload, "buy_now_price_vnd", 1, 1000000000000000LL);
    long long planned = number(payload, "planned_start_at", 1, 253402300799LL);
    long long duration = number(payload, "duration_sec", 30, 86400);
    if (!title || !description || start < 0 || buy <= start || planned < 0 || duration < 0) return error(id, "INVALID_REQUEST");
    sqlite3_stmt *stmt;
    if (!item_id) stmt = prepare(app, "INSERT INTO items(room_id,title,description,starting_price_vnd,buy_now_price_vnd,planned_start_at,duration_sec,queue_position) VALUES(?,?,?,?,?,?,?,?)",
        "issiiiii", room_id, title, description, start, buy, planned, duration, next_position);
    else stmt = prepare(app, "UPDATE items SET title=?,description=?,starting_price_vnd=?,buy_now_price_vnd=?,planned_start_at=?,duration_sec=? WHERE id=? AND room_id=?",
        "ssiiiiii", title, description, start, buy, planned, duration, item_id, room_id);
    if (mutate_item(app, stmt, room_id) < 0) return database_error(app, id);
    if (!item_id) item_id = sqlite3_last_insert_rowid(app->db);
    json_object *data = json_object_new_object();
    json_object_object_add(data, "item_id", json_object_new_int64(item_id));
    return protocol_success(id, data);
}

json_object *app_handle(App *app, int slot, const unsigned char *body, size_t length, long long starts) {
    /* Keep phase-1 envelope validation and PING semantics in one place. */
    json_object *base = protocol_handle(body, length, starts);
    const char *code = json_object_get_string(get(base, "error_code"));
    if (!code || strcmp(code, "UNKNOWN_COMMAND")) return base;
    json_object *request = protocol_parse(body, length);
    if (!request) { json_object_put(base); return error(NULL, "INVALID_JSON"); }
    json_object *id = get(request, "request_id"), *payload = get(request, "payload");
    const char *command = json_object_get_string(get(request, "type"));
    const char *known[] = {"REGISTER","LOGIN","PASSWORD_CHANGE","LOGOUT","ROOM_CREATE","ROOM_LIST","ROOM_DETAIL","ROOM_JOIN","ROOM_LEAVE","ROOM_SNAPSHOT","ROOM_PUBLISH","ITEM_CREATE","ITEM_UPDATE","ITEM_DELETE","ITEM_REORDER","ITEM_LIST",NULL};
    int found = 0;
    for (int i = 0; known[i]; i++) if (!strcmp(command, known[i])) found = 1;
    if (!found) { json_object_put(request); return base; }
    json_object_put(base);
    Session *s = &app->sessions[slot];
    json_object *reply;
    if (!strcmp(command, "REGISTER") || !strcmp(command, "LOGIN") || !strcmp(command, "PASSWORD_CHANGE"))
        reply = auth_begin(app, slot, id, command, payload);
    else if (!s->user_id) reply = error(id, "AUTH_REQUIRED");
    else if (!strcmp(command, "LOGOUT")) { leave(app, slot); s->user_id = 0; s->username[0] = '\0'; reply = empty_success(id); }
    else if (!strncmp(command, "ROOM_", 5)) reply = room_command(app, slot, id, command, payload);
    else reply = item_command(app, slot, id, command, payload);
    fprintf(stderr, "event=command command=%s slot=%d user_id=%lld room_id=%lld ok=%d\n", command, slot, s->user_id, s->room_id, reply ? json_object_get_boolean(get(reply, "ok")) : -1);
    json_object_put(request);
    return reply;
}
