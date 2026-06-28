#ifndef SELF_HEALING_H
#define SELF_HEALING_H

#include "disk_health.h"

/* Execute a self-healing action.
   If auto_mode = 0, only logs what it would do (dry-run).
   If auto_mode = 1, actually executes the action (requires root).
   Returns 0 on success, -1 on failure. */
int self_healing_execute(const policy_decision_t *decision,
                         const device_info_t     *dev,
                         int                      auto_mode);

#endif
