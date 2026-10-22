#include "protocol.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
/* Validate complete UTF-8 frames independently of json-c's optional validator. */
static int valid_utf8(const unsigned char *s, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned int c = s[i++], value, minimum;
        size_t extra;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { value = c & 0x1f; extra = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { value = c & 0x0f; extra = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { value = c & 0x07; extra = 3; minimum = 0x10000; }
        else return 0;
        if (length - i < extra) return 0;
        while (extra--) {
            c = s[i++];
            if ((c & 0xc0) != 0x80) return 0;
            value = (value << 6) | (c & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}
/* json-c 0.18 strict mode on this platform rejects raw non-ASCII strings.
 * Preserve strict parsing by representing validated Unicode as JSON escapes. */
static char *ascii_json(const unsigned char *s, size_t length, size_t *out_length) {
    char *out = malloc(length * 6 + 1);
    if (!out) return NULL;
    size_t i = 0, used = 0;
    int in_string = 0, escaped = 0;
    while (i < length) {
        unsigned int c = s[i++], code;
        size_t extra;
        if (c < 0x80) {
            out[used++] = (char)c;
            if (in_string) {
                if (escaped) escaped = 0;
                else if (c == '\\') escaped = 1;
                else if (c == '"') in_string = 0;
            } else if (c == '"') in_string = 1;
            continue;
        }
        if (!in_string || escaped) { free(out); *out_length = (size_t)-1; return NULL; }
        if (c <= 0xdf) { code = c & 0x1f; extra = 1; }
        else if (c <= 0xef) { code = c & 0x0f; extra = 2; }
        else { code = c & 0x07; extra = 3; }
        while (extra--) code = (code << 6) | (s[i++] & 0x3f);
        if (code <= 0xffff) used += (size_t)sprintf(out + used, "\\u%04x", code);
        else {
            code -= 0x10000;
            used += (size_t)sprintf(out + used, "\\u%04x\\u%04x", 0xd800 + (code >> 10), 0xdc00 + (code & 0x3ff));
        }
    }
    out[used] = '\0'; *out_length = used;
    return out;
}
static int finite_numbers(json_object *value) {
    if (json_object_is_type(value, json_type_double)) return isfinite(json_object_get_double(value));
    if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t i = 0; i < count; i++) if (!finite_numbers(json_object_array_get_idx(value, i))) return 0;
    } else if (json_object_is_type(value, json_type_object)) {
        json_object_object_foreach(value, key, member) {
            (void)key;
            if (!finite_numbers(member)) return 0;
        }
    }
    return 1;
}
json_object *protocol_error(json_object *id, const char *code) {
    json_object *reply = json_object_new_object();
    json_object_object_add(reply, "request_id", id ? json_object_get(id) : NULL);
    json_object_object_add(reply, "ok", json_object_new_boolean(0));
    json_object_object_add(reply, "error_code", json_object_new_string(code));
    return reply;
}
json_object *protocol_handle(const unsigned char *body, size_t length, long long starts) {
    if (!valid_utf8(body, length)) return protocol_error(NULL, "INVALID_JSON");
    size_t parse_length = 0;
    char *parse_body = ascii_json(body, length, &parse_length);
    if (!parse_body) return protocol_error(NULL, parse_length == (size_t)-1 ? "INVALID_JSON" : "INTERNAL_ERROR");
    json_tokener *tok = json_tokener_new();
    if (!tok) { free(parse_body); return protocol_error(NULL, "INTERNAL_ERROR"); }
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    json_object *request = json_tokener_parse_ex(tok, parse_body, (int)parse_length);
    size_t end = json_tokener_get_parse_end(tok);
    int valid = json_tokener_get_error(tok) == json_tokener_success;
    while (end < parse_length && (parse_body[end] == ' ' || parse_body[end] == '\n' || parse_body[end] == '\r' || parse_body[end] == '\t')) end++;
    json_tokener_free(tok);
    free(parse_body);
    if (!valid || end != parse_length || !json_object_is_type(request, json_type_object) || !finite_numbers(request)) {
        json_object_put(request);
        return protocol_error(NULL, "INVALID_JSON");
    }
    json_object *id = NULL, *type = NULL, *payload = NULL, *reply;
    json_object_object_get_ex(request, "request_id", &id);
    json_object_object_get_ex(request, "type", &type);
    json_object_object_get_ex(request, "payload", &payload);
    if (!json_object_is_type(id, json_type_string) || json_object_get_string_len(id) < 1 ||
        json_object_get_string_len(id) > 128 ||
        strlen(json_object_get_string(id)) != (size_t)json_object_get_string_len(id)) {
        reply = protocol_error(NULL, "INVALID_REQUEST");
    } else if (!json_object_is_type(type, json_type_string) ||
               strlen(json_object_get_string(type)) != (size_t)json_object_get_string_len(type) ||
               !json_object_is_type(payload, json_type_object)) {
        reply = protocol_error(id, "INVALID_REQUEST");
    } else if (strcmp(json_object_get_string(type), "PING") != 0) {
        reply = protocol_error(id, "UNKNOWN_COMMAND");
    } else {
        reply = json_object_new_object();
        json_object *data = json_object_new_object();
        json_object_object_add(reply, "request_id", json_object_get(id));
        json_object_object_add(reply, "ok", json_object_new_boolean(1));
        json_object_object_add(data, "message", json_object_new_string("PONG"));
        json_object_object_add(data, "echo", json_object_get(payload));
        json_object_object_add(data, "server_start_count", json_object_new_int64(starts));
        json_object_object_add(reply, "data", data);
    }
    json_object_put(request);
    return reply;
}
