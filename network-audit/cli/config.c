/*
 * config.c — CLI Argument Parsing and Target List Expansion
 *
 * Supports:
 *   --target CIDR       (192.168.1.0/24, 10.0.0.1-10, 192.168.1.1)
 *   --ports RANGE       (22,80,443 or 1-1024)
 *   --concurrency N     (default 1000)
 *   --timeout MS        (default 3000)
 *   --output FORMAT     (text|json)
 *   --db PATH           (optional SQLite)
 *   --workers N         (default 0)
 *   --help
 */

#include "config.h"
#include <getopt.h>

/* ============================================================
 *  Defaults
 * ============================================================ */

void
config_init(na_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->target_arg, "127.0.0.1", sizeof(cfg->target_arg) - 1);
    cfg->port_start     = 22;
    cfg->port_end       = 443;
    cfg->concurrency    = NA_CONCURRENCY_DEF;
    cfg->timeout_ms     = NA_TIMEOUT_DEF_MS;
    cfg->worker_threads = 0;
    cfg->output_fmt     = OUTPUT_TEXT;
    cfg->use_db         = 0;
    cfg->db_path[0]     = '\0';
}

/* ============================================================
 *  Help text
 * ============================================================ */

void
config_print_help(const char *prog)
{
    printf("Lightweight Network Audit Framework V1\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --target CIDR       Target IP range\n");
    printf("                      Examples: 192.168.1.0/24, 10.0.0.1-10, 192.168.1.1\n");
    printf("  --ports RANGE       Port range (default: 22-443)\n");
    printf("                      Examples: 22, 22-80, 22,80,443, 1-1024\n");
    printf("  --concurrency N     Max concurrent connections (default: %d, max: %d)\n",
           NA_CONCURRENCY_DEF, NA_CONCURRENCY_MAX);
    printf("  --timeout MS        Connection timeout in ms (default: %d)\n",
           NA_TIMEOUT_DEF_MS);
    printf("  --output FORMAT     Output format: text (default) or json\n");
    printf("  --db PATH           Enable SQLite storage at PATH\n");
    printf("  --workers N         Worker threads for CPU tasks (default: 0)\n");
    printf("  --help              Show this help\n");
    printf("\nSecurity: Internal lab networks only. No brute force, exploitation,\n");
    printf("          credential attacks, or lateral movement.\n");
}

/* ============================================================
 *  Parse a port specification string
 *
 *  Formats: "22", "22-80", "22,80,443", "80,443,8000-9000"
 * ============================================================ */

static int
parse_ports(const char *str, int *start, int *end)
{
    char buf[256];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Try single port */
    char *dash = strchr(buf, '-');
    char *comma = strchr(buf, ',');

    if (!dash && !comma) {
        /* Single port: "22" */
        int p = atoi(buf);
        if (p < 1 || p > NA_PORT_MAX) return -1;
        *start = p;
        *end   = p;
        return 0;
    }

    if (dash && !comma) {
        /* Range: "22-80" */
        *dash = '\0';
        *start = atoi(buf);
        *end   = atoi(dash + 1);
        if (*start < 1 || *end > NA_PORT_MAX || *start > *end) return -1;
        return 0;
    }

    /*
     * Comma-separated or mixed: take the min and max as the range.
     * This simplifies scanning — we scan all ports in [min, max].
     */
    int min_port = NA_PORT_MAX;
    int max_port = 1;
    char *saveptr;
    char *token = strtok_r(buf, ",", &saveptr);

    while (token) {
        char *sub_dash = strchr(token, '-');
        int   p_start, p_end;

        if (sub_dash) {
            *sub_dash = '\0';
            p_start = atoi(token);
            p_end   = atoi(sub_dash + 1);
        } else {
            p_start = atoi(token);
            p_end   = p_start;
        }

        if (p_start < 1 || p_end > NA_PORT_MAX || p_start > p_end) return -1;

        if (p_start < min_port) min_port = p_start;
        if (p_end   > max_port) max_port = p_end;

        token = strtok_r(NULL, ",", &saveptr);
    }

    *start = min_port;
    *end   = max_port;
    return 0;
}

/* ============================================================
 *  long-option parsing table
 * ============================================================ */

enum {
    OPT_TARGET      = 256,
    OPT_PORTS       = 257,
    OPT_CONCURRENCY = 258,
    OPT_TIMEOUT     = 259,
    OPT_OUTPUT      = 260,
    OPT_DB          = 261,
    OPT_WORKERS     = 262,
    OPT_HELP        = 263
};

/* ============================================================
 *  Main parser
 * ============================================================ */

int
config_parse_args(na_config_t *cfg, int argc, char *argv[])
{
    static struct option long_opts[] = {
        { "target",      required_argument, NULL, OPT_TARGET      },
        { "ports",       required_argument, NULL, OPT_PORTS       },
        { "concurrency", required_argument, NULL, OPT_CONCURRENCY },
        { "timeout",     required_argument, NULL, OPT_TIMEOUT     },
        { "output",      required_argument, NULL, OPT_OUTPUT      },
        { "db",          required_argument, NULL, OPT_DB          },
        { "workers",     required_argument, NULL, OPT_WORKERS     },
        { "help",        no_argument,       NULL, OPT_HELP        },
        { NULL,          0,                 NULL, 0               }
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "", long_opts, &option_index)) != -1) {
        switch (opt) {
        case OPT_TARGET:
            strncpy(cfg->target_arg, optarg, sizeof(cfg->target_arg) - 1);
            cfg->target_arg[sizeof(cfg->target_arg) - 1] = '\0';
            break;

        case OPT_PORTS:
            if (parse_ports(optarg, &cfg->port_start, &cfg->port_end) < 0) {
                fprintf(stderr, "Error: invalid port specification '%s'\n", optarg);
                return -1;
            }
            break;

        case OPT_CONCURRENCY: {
            int c = atoi(optarg);
            if (c < 1 || c > NA_CONCURRENCY_MAX) {
                fprintf(stderr, "Error: concurrency must be 1-%d\n", NA_CONCURRENCY_MAX);
                return -1;
            }
            cfg->concurrency = c;
            break;
        }

        case OPT_TIMEOUT: {
            int t = atoi(optarg);
            if (t < 10 || t > 600000) {
                fprintf(stderr, "Error: timeout must be 10-600000 ms\n");
                return -1;
            }
            cfg->timeout_ms = t;
            break;
        }

        case OPT_OUTPUT:
            if (strcmp(optarg, "json") == 0) {
                cfg->output_fmt = OUTPUT_JSON;
            } else if (strcmp(optarg, "text") == 0) {
                cfg->output_fmt = OUTPUT_TEXT;
            } else {
                fprintf(stderr, "Error: output format must be 'text' or 'json'\n");
                return -1;
            }
            break;

        case OPT_DB:
            cfg->use_db = 1;
            strncpy(cfg->db_path, optarg, sizeof(cfg->db_path) - 1);
            cfg->db_path[sizeof(cfg->db_path) - 1] = '\0';
            break;

        case OPT_WORKERS: {
            int w = atoi(optarg);
            if (w < 0 || w > NA_WORKER_MAX) {
                fprintf(stderr, "Error: workers must be 0-%d\n", NA_WORKER_MAX);
                return -1;
            }
            cfg->worker_threads = w;
            break;
        }

        case OPT_HELP:
            config_print_help(argv[0]);
            return 1;                  /* signal caller to exit(0) */

        case '?':
        default:
            return -1;
        }
    }

    /* Validate: if --db specified without SQLite support, warn */
#ifndef HAVE_SQLITE3
    if (cfg->use_db) {
        fprintf(stderr,
                "Warning: SQLite support not compiled in. "
                "Rebuild with HAVE_SQLITE=1 to enable --db.\n");
        cfg->use_db = 0;
    }
#endif

    return 0;
}

/* ============================================================
 *  Expand targets from --target argument
 *
 *  Supported formats:
 *    "192.168.1.1"             → single IP
 *    "192.168.1.0/24"          → CIDR range (256 IPs, .0 and .255 skipped)
 *    "10.0.0.1-10.0.0.10"      → IP range
 *    "10.0.0.1-10"             → shorthand: same /24, last octet range
 *
 *  For each IP, all ports in [port_start, port_end] are generated.
 * ============================================================ */

int
config_expand_targets(const na_config_t *cfg,
                      scan_target_t *targets, int max_targets)
{
    int count = 0;

    const char *arg = cfg->target_arg;
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint32_t ip_start = 0;
    uint32_t ip_end   = 0;

    /* Check for CIDR notation: x.x.x.x/NN */
    char *slash = strchr(buf, '/');
    if (slash) {
        *slash = '\0';
        int prefix = atoi(slash + 1);
        if (prefix < 0 || prefix > 32) {
            fprintf(stderr, "Error: invalid CIDR prefix /%d\n", prefix);
            return -1;
        }

        struct in_addr in;
        if (inet_pton(AF_INET, buf, &in) != 1) {
            fprintf(stderr, "Error: invalid IP address '%s'\n", buf);
            return -1;
        }

        uint32_t base = ntohl(in.s_addr);
        uint32_t mask = (prefix == 0) ? 0 : (~0U << (32 - (unsigned int)prefix));
        ip_start = base & mask;
        ip_end   = ip_start | (~mask);

        /* Skip network and broadcast for /24 and smaller */
        if (prefix < 31) {
            ip_start++;               /* skip network address */
            ip_end--;                 /* skip broadcast address */
        }

        if (ip_start > ip_end) {
            fprintf(stderr, "Error: no usable hosts in /%d range\n", prefix);
            return 0;
        }
    }
    /* Check for range: "x.x.x.x-y.y.y.y" or "x.x.x.x-YY" */
    else {
        char *dash = strchr(buf, '-');
        if (dash) {
            *dash = '\0';
            char *end_str = dash + 1;

            struct in_addr in_start;
            if (inet_pton(AF_INET, buf, &in_start) != 1) {
                fprintf(stderr, "Error: invalid start IP '%s'\n", buf);
                return -1;
            }
            ip_start = ntohl(in_start.s_addr);

            /* Check if end is a full IP or just last octet */
            if (strchr(end_str, '.')) {
                struct in_addr in_end;
                if (inet_pton(AF_INET, end_str, &in_end) != 1) {
                    fprintf(stderr, "Error: invalid end IP '%s'\n", end_str);
                    return -1;
                }
                ip_end = ntohl(in_end.s_addr);
            } else {
                /* Shorthand: last octet only */
                int last = atoi(end_str);
                if (last < 0 || last > 255) {
                    fprintf(stderr, "Error: invalid last octet '%s'\n", end_str);
                    return -1;
                }
                ip_end = (ip_start & 0xFFFFFF00U) | (uint32_t)last;
            }
        }
        /* Single IP */
        else {
            struct in_addr in;
            if (inet_pton(AF_INET, buf, &in) != 1) {
                fprintf(stderr, "Error: invalid IP address '%s'\n", buf);
                return -1;
            }
            ip_start = ntohl(in.s_addr);
            ip_end   = ip_start;
        }
    }

    /* Generate all IP:port combinations */
    int n_ports = cfg->port_end - cfg->port_start + 1;

    for (uint32_t ip = ip_start; ip <= ip_end && count < max_targets; ip++) {
        for (int port = cfg->port_start; port <= cfg->port_end && count < max_targets; port++) {
            targets[count].ip   = htonl(ip);
            targets[count].port = htons((uint16_t)port);
            count++;
        }
    }

    fprintf(stderr, "Generated %d targets (%d IPs × %d ports)\n",
            count, (int)(ip_end - ip_start + 1), n_ports);

    return count;
}
