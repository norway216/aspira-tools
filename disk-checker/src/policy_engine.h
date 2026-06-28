#ifndef POLICY_ENGINE_H
#define POLICY_ENGINE_H

#include "disk_health.h"

/* Evaluate fault + score → healing action decision */
void policy_evaluate(const health_score_t *score,
                     const fault_result_t *fault,
                     int heal_max_level,
                     policy_decision_t    *decision);

#endif
