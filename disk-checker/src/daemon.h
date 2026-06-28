#ifndef DAEMON_H
#define DAEMON_H

#include "disk_health.h"

void daemon_run(config_t *cfg);
void daemon_main_loop(config_t *cfg);
void daemon_signal_handler(int sig);
int  daemon_write_pidfile(void);
void daemon_remove_pidfile(void);
int  run_json_mode(config_t *cfg);

#endif
