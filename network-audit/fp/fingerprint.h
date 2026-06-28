/*
 * fingerprint.h — Lightweight Banner Fingerprint Engine
 */
#ifndef FP_FINGERPRINT_H
#define FP_FINGERPRINT_H

#include "../include/net_audit.h"

/* ---- Lifecycle ---- */
void fp_init_default(fp_db_t *db);

/* ---- Matching ---- */
fp_service_t fp_match(const char *banner, int banner_len);
const char  *fp_service_name(fp_service_t s);

/* ---- Custom patterns ---- */
int fp_register_pattern(fp_db_t *db, const char *pattern,
                        fp_service_t service, const char *service_name);

/*
 * Global fingerprint DB (singleton).
 * Initialized once at startup, read-only during scan.
 */
extern fp_db_t g_fp_db;

#endif /* FP_FINGERPRINT_H */
