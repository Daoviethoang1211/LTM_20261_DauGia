#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <json-c/json.h>
#include <stddef.h>
json_object *protocol_error(json_object *id, const char *code);
json_object *protocol_handle(const unsigned char *body, size_t length, long long starts);
#endif
