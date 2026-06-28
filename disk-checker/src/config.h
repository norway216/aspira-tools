#ifndef CONFIG_H
#define CONFIG_H

#include "disk_health.h"

void config_init(config_t *cfg);
int  config_parse_args(config_t *cfg, int argc, char *argv[]);
int  config_load_file(config_t *cfg, const char *path);

#endif /* CONFIG_H */
