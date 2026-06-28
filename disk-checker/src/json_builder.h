#ifndef JSON_BUILDER_H
#define JSON_BUILDER_H

#include "disk_health.h"

void        json_init(json_builder_t *jb);
int         json_open_object(json_builder_t *jb);
int         json_close_object(json_builder_t *jb);
int         json_open_array(json_builder_t *jb, const char *key);
int         json_close_array(json_builder_t *jb);
int         json_add_int(json_builder_t *jb, const char *key, int value);
int         json_add_uint64(json_builder_t *jb, const char *key, uint64_t value);
int         json_add_string(json_builder_t *jb, const char *key, const char *value);
int         json_add_bool(json_builder_t *jb, const char *key, int value);
int         json_add_double(json_builder_t *jb, const char *key, double value);
const char *json_str(const json_builder_t *jb);
int         json_len(const json_builder_t *jb);

#endif
