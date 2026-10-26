#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <json-c/json.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
json_object *protocol_error(json_object *id, const char *code);
json_object *protocol_parse(const unsigned char *body, size_t length);
json_object *protocol_success(json_object *id, json_object *data);
json_object *protocol_handle(const unsigned char *body, size_t length, long long starts);
#ifdef __cplusplus
}
#endif
#endif
