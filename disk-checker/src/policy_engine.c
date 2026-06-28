#include "policy_engine.h"
#include <string.h>

/* ============================================================
 *  Policy Decision Matrix (V2 Section 7)
 *
 *  Maps conditions → healing actions with configurable level cap.
 *  Decision order is important: check most severe conditions first.
 *  After selecting an action, the level cap is applied.
 * ============================================================ */

/* Apply the heal_max_level cap: downgrade action if it exceeds the cap */
static void apply_level_cap(policy_decision_t *decision, int heal_max_level) {
    if ((int)decision->level <= heal_max_level) return;

    /* Downgrade to WARN at the max allowed level */
    decision->action            = ACTION_WARN;
    decision->level             = (healing_level_t)heal_max_level;
    decision->description       = "Action capped by heal-level limit. "
                                  "Would have taken stronger action.";
    decision->require_confirmation = 0;
}

void policy_evaluate(const health_score_t *score,
                     const fault_result_t *fault,
                     int heal_max_level,
                     policy_decision_t    *decision) {
    memset(decision, 0, sizeof(*decision));
    decision->action = ACTION_NONE;
    decision->level  = HEAL_LEVEL_SOFT;
    decision->description = "Continue monitoring";
    decision->require_confirmation = 0;

    /* Emergency — Level 4 */
    if (fault->level == FAULT_FATAL && score->score == 0) {
        decision->action = ACTION_EMERGENCY;
        decision->level  = HEAL_LEVEL_EMERGENCY;
        decision->description = "FATAL: freeze non-critical services, "
                                "force system safe state, alert external system";
        decision->require_confirmation = 1;
        apply_level_cap(decision, heal_max_level);
        return;
    }

    /* Failover — Level 3 */
    if (score->score < 20) {
        decision->action = ACTION_FAILOVER;
        decision->level  = HEAL_LEVEL_ISOLATION;
        decision->description = "Health score < 20: redirect IO to backup device, "
                                "trigger failover";
        decision->require_confirmation = 1;
        apply_level_cap(decision, heal_max_level);
        return;
    }

    /* Isolate — Level 3 */
    if (fault->level >= FAULT_CRITICAL && score->score < 40) {
        decision->action = ACTION_ISOLATE;
        decision->level  = HEAL_LEVEL_ISOLATION;
        decision->description = "SMART failure + critical score: "
                                "remove disk from active pool, stop IO routing";
        decision->require_confirmation = 1;
        apply_level_cap(decision, heal_max_level);
        return;
    }

    /* Read-only — Level 2 */
    if (fault->latency_spike && score->score < 50) {
        decision->action = ACTION_READONLY;
        decision->level  = HEAL_LEVEL_CONTROLLED;
        decision->description = "IO latency spike + degraded score: "
                                "enable read-only mode, flush caches, disable writes";
        decision->require_confirmation = 1;
        apply_level_cap(decision, heal_max_level);
        return;
    }

    /* Reduce IO — Level 1 */
    if (fault->temp_spike) {
        decision->action = ACTION_REDUCE_IO;
        decision->level  = HEAL_LEVEL_SOFT;
        decision->description = "Temperature > 60°C: reduce IO load, "
                                "increase monitoring frequency";
        decision->require_confirmation = 0;
        apply_level_cap(decision, heal_max_level);
        return;
    }

    /* Throttle — Level 1 */
    if (fault->realloc_increasing && score->score < 70) {
        decision->action = ACTION_THROTTLE;
        decision->level  = HEAL_LEVEL_SOFT;
        decision->description = "Reallocated sectors increasing + score < 70: "
                                "throttle IO priority";
        decision->require_confirmation = 0;
        apply_level_cap(decision, heal_max_level);
        return;
    }

    /* Warn — Level 1 */
    if (fault->pending_increasing) {
        decision->action = ACTION_WARN;
        decision->level  = HEAL_LEVEL_SOFT;
        decision->description = "Pending sectors increasing: "
                                "increase monitoring, log warnings";
        decision->require_confirmation = 0;
        apply_level_cap(decision, heal_max_level);
        return;
    }

    if (fault->level >= FAULT_WARN) {
        decision->action = ACTION_WARN;
        decision->level  = HEAL_LEVEL_SOFT;
        decision->description = "Early degradation detected: log and monitor closely";
        decision->require_confirmation = 0;
        apply_level_cap(decision, heal_max_level);
        return;
    }

    /* No action needed */
    apply_level_cap(decision, heal_max_level);
}
