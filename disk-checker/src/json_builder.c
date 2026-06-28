#include "json_builder.h"
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Internal helpers
 * ============================================================ */

static int json_append(json_builder_t *jb, const char *s) {
    int n = (int)strlen(s);
    if (jb->pos + n >= JSON_BUF_SIZE - 2) return -1;
    memcpy(jb->buf + jb->pos, s, (size_t)n);
    jb->pos += n;
    jb->buf[jb->pos] = '\0';
    return 0;
}

static int json_append_char(json_builder_t *jb, char c) {
    if (jb->pos >= JSON_BUF_SIZE - 2) return -1;
    jb->buf[jb->pos++] = c;
    jb->buf[jb->pos] = '\0';
    return 0;
}

static int json_append_raw(json_builder_t *jb, const char *s, int len) {
    if (jb->pos + len >= JSON_BUF_SIZE - 2) return -1;
    memcpy(jb->buf + jb->pos, s, (size_t)len);
    jb->pos += len;
    jb->buf[jb->pos] = '\0';
    return 0;
}

/* Insert a comma if this is not the first item at the current depth */
static int json_comma(json_builder_t *jb) {
    if (jb->depth > 0 && jb->item_count[jb->depth - 1] > 0) {
        return json_append_char(jb, ',');
    }
    return 0;
}

/* ============================================================
 *  Public API
 * ============================================================ */

void json_init(json_builder_t *jb) {
    memset(jb, 0, sizeof(*jb));
}

const char *json_str(const json_builder_t *jb) {
    return jb->buf;
}

int json_len(const json_builder_t *jb) {
    return jb->pos;
}

/* --- Object --- */

int json_open_object(json_builder_t *jb) {
    if (jb->depth >= JSON_MAX_DEPTH) return -1;
    if (json_append_char(jb, '{')) return -1;
    jb->depth++;
    jb->item_count[jb->depth - 1] = 0;
    return 0;
}

int json_close_object(json_builder_t *jb) {
    if (jb->depth <= 0) return -1;
    jb->depth--;
    if (json_append_char(jb, '}')) return -1;
    /* mark that we added an item to the parent */
    if (jb->depth > 0) jb->item_count[jb->depth - 1]++;
    return 0;
}

/* --- Array --- */

int json_open_array(json_builder_t *jb, const char *key) {
    if (json_comma(jb)) return -1;
    if (json_append_char(jb, '"')) return -1;
    if (json_append(jb, key)) return -1;
    if (json_append(jb, "\":")) return -1;
    if (json_append_char(jb, '[')) return -1;
    if (jb->depth > 0) jb->item_count[jb->depth - 1]++;
    jb->depth++;
    jb->item_count[jb->depth - 1] = 0;
    return 0;
}

int json_close_array(json_builder_t *jb) {
    if (jb->depth <= 0) return -1;
    jb->depth--;
    if (json_append_char(jb, ']')) return -1;
    if (jb->depth > 0) jb->item_count[jb->depth - 1]++;
    return 0;
}

/* --- Typed value helpers --- */

int json_add_int(json_builder_t *jb, const char *key, int value) {
    if (json_comma(jb)) return -1;
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "\"%s\":%d", key, value);
    if (n < 0 || n >= (int)sizeof(tmp)) return -1;
    if (json_append_raw(jb, tmp, n)) return -1;
    if (jb->depth > 0) jb->item_count[jb->depth - 1]++;
    return 0;
}

int json_add_uint64(json_builder_t *jb, const char *key, uint64_t value) {
    if (json_comma(jb)) return -1;
    char tmp[80];
    int n = snprintf(tmp, sizeof(tmp), "\"%s\":%lu", key, (unsigned long)value);
    if (n < 0 || n >= (int)sizeof(tmp)) return -1;
    if (json_append_raw(jb, tmp, n)) return -1;
    if (jb->depth > 0) jb->item_count[jb->depth - 1]++;
    return 0;
}

int json_add_double(json_builder_t *jb, const char *key, double value) {
    if (json_comma(jb)) return -1;
    char tmp[80];
    int n = snprintf(tmp, sizeof(tmp), "\"%s\":%.2f", key, value);
    if (n < 0 || n >= (int)sizeof(tmp)) return -1;
    if (json_append_raw(jb, tmp, n)) return -1;
    if (jb->depth > 0) jb->item_count[jb->depth - 1]++;
    return 0;
}

int json_add_bool(json_builder_t *jb, const char *key, int value) {
    if (json_comma(jb)) return -1;
    char tmp[80];
    int n = snprintf(tmp, sizeof(tmp), "\"%s\":%s", key, value ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(tmp)) return -1;
    if (json_append_raw(jb, tmp, n)) return -1;
    if (jb->depth > 0) jb->item_count[jb->depth - 1]++;
    return 0;
}

int json_add_string(json_builder_t *jb, const char *key, const char *value) {
    if (json_comma(jb)) return -1;
    /* Write key */
    if (json_append_char(jb, '"')) return -1;
    if (json_append(jb, key)) return -1;
    if (json_append(jb, "\":\"")) return -1;
    /* Write escaped value */
    const char *p = value;
    while (*p) {
        if (*p == '"' || *p == '\\') {
            if (json_append_char(jb, '\\')) return -1;
        }
        if (json_append_char(jb, *p)) return -1;
        p++;
    }
    if (json_append_char(jb, '"')) return -1;
    if (jb->depth > 0) jb->item_count[jb->depth - 1]++;
    return 0;
}
