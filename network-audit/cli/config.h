/*
 * config.h — CLI Argument Parsing
 */
#ifndef CLI_CONFIG_H
#define CLI_CONFIG_H

#include "../include/net_audit.h"

/* ---- Lifecycle ---- */
void config_init(na_config_t *cfg);

/* ---- Parse ---- */
int  config_parse_args(na_config_t *cfg, int argc, char *argv[]);

/* ---- Help ---- */
void config_print_help(const char *prog);

/* ---- Target list expansion ---- */
int  config_expand_targets(const na_config_t *cfg,
                           scan_target_t *targets, int max_targets);

#endif /* CLI_CONFIG_H */
