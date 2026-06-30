/*
 * fingerprint.c — Lightweight Banner Fingerprint Engine
 *
 * Uses strstr() for substring matching against a pre-defined pattern
 * database. No regex, no heavy parsing — O(N×M) with small constants
 * (N ≈ banner_len ≤ 128, M = pattern count ≤ 64).
 *
 * Matching strategy:
 *   - Patterns are checked in registration order (first match wins).
 *   - TLS record headers (0x16 0x03) are matched as raw bytes.
 *   - TCP port context for TLS vs HTTPS is left to the caller.
 */

#include "fingerprint.h"

/* ---- Global singleton ---- */
fp_db_t g_fp_db;

/* ============================================================
 *  Initialize with default patterns
 * ============================================================ */

void
fp_init_default(fp_db_t *db)
{
    memset(db, 0, sizeof(*db));

    /* Registration order: more specific patterns first */

    fp_register_pattern(db, "SSH-",            FP_SSH,    "SSH");
    fp_register_pattern(db, "HTTP/",           FP_HTTP,   "HTTP");
    fp_register_pattern(db, "\x16\x03",        FP_TLS,    "TLS");
    fp_register_pattern(db, "220",             FP_SMTP,   "SMTP");
    fp_register_pattern(db, "FTP",             FP_FTP,    "FTP");
    fp_register_pattern(db, "mysql",           FP_MYSQL,  "MySQL");
    fp_register_pattern(db, "\x03\x00",        FP_RDP,    "RDP");
    fp_register_pattern(db, "login:",          FP_TELNET, "Telnet");
    fp_register_pattern(db, "RTSP/",           FP_RTSP,   "RTSP");
    fp_register_pattern(db, "* OK",            FP_IMAP,   "IMAP");
    fp_register_pattern(db, "+OK",             FP_POP3,   "POP3");
}

/* ============================================================
 *  Register a custom pattern
 * ============================================================ */

int
fp_register_pattern(fp_db_t *db, const char *pattern,
                    fp_service_t service, const char *service_name)
{
    if (db->count >= NA_FP_PATTERNS_MAX) {
        return -1;
    }

    fp_entry_t *entry = &db->entries[db->count];
    entry->pattern       = pattern;
    entry->service       = service;
    entry->service_name  = service_name;
    entry->min_match_len = (int)strlen(pattern);

    db->count++;
    return 0;
}

/* ============================================================
 *  Match a banner against the fingerprint database (Fix #16:
 *  takes explicit db parameter instead of using global)
 * ============================================================ */

fp_service_t
fp_match(const fp_db_t *db, const char *banner, int banner_len)
{
    if (!db || !banner || banner_len <= 0) {
        return FP_UNKNOWN;
    }

    for (int i = 0; i < db->count; i++) {
        const fp_entry_t *entry = &db->entries[i];

        if (banner_len < entry->min_match_len) {
            continue;
        }

        if (strstr(banner, entry->pattern)) {
            return entry->service;
        }
    }

    return FP_UNKNOWN;
}

/* ============================================================
 *  Human-readable service name (Fix #15: single switch, no
 *  redundant DB scan)
 * ============================================================ */

const char *
fp_service_name(fp_service_t s)
{
    switch (s) {
    case FP_UNKNOWN: return "unknown";
    case FP_SSH:     return "SSH";
    case FP_HTTP:    return "HTTP";
    case FP_HTTPS:   return "HTTPS";
    case FP_TLS:     return "TLS";
    case FP_SMTP:    return "SMTP";
    case FP_FTP:     return "FTP";
    case FP_MYSQL:   return "MySQL";
    case FP_RDP:     return "RDP";
    case FP_DNS:     return "DNS";
    case FP_TELNET:  return "Telnet";
    case FP_RTSP:    return "RTSP";
    case FP_IMAP:    return "IMAP";
    case FP_POP3:    return "POP3";
    default:         return "unknown";
    }
}
