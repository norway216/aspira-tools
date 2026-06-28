#include "self_healing.h"
#include "logger.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* ============================================================
 *  Self-Healing Executor (V2 Section 4.5)
 *
 *  Actions:
 *    ACTION_WARN      — log only
 *    ACTION_THROTTLE  — ionice -c3 (best-effort IO class)
 *    ACTION_REDUCE_IO — log recommendation
 *    ACTION_READONLY  — mount -o remount,ro
 *    ACTION_ISOLATE   — echo 1 > /sys/block/<dev>/device/delete
 *    ACTION_FAILOVER  — log failover event
 *    ACTION_EMERGENCY — log critical alert
 *
 *  Safety: destructive actions require auto_mode = 1.
 *  Default is dry-run (log only).
 * ============================================================ */

static int execute_command(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) {
        logger_write(LOG_WARN, "self_healing: system() failed: %s", strerror(errno));
        return -1;
    }
    if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) {
        logger_write(LOG_WARN, "self_healing: command exited %d: %s",
                     WEXITSTATUS(rc), cmd);
        return -1;
    }
    return 0;
}

/* ============================================================
 *  Attempt to find a mount point for the given device
 * ============================================================ */

static int find_mountpoint(const char *dev_path, char *mp, size_t mp_sz) {
    /* Read /proc/mounts to find the mount point */
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return -1;

    char line[512];
    int found = 0;
    while (fgets(line, (int)sizeof(line), f)) {
        char dev[256], mnt[256];
        if (sscanf(line, "%255s %255s", dev, mnt) == 2) {
            if (strcmp(dev, dev_path) == 0) {
                snprintf(mp, mp_sz, "%s", mnt);
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/* ============================================================
 *  IO Throttling via ionice
 * ============================================================ */

static int heal_throttle_io(const device_info_t *dev, int auto_mode) {
    logger_write(LOG_INFO, "self_healing: throttling IO on %s (ionice)", dev->path);

    if (!auto_mode) {
        logger_write(LOG_INFO, "self_healing: [DRY-RUN] would run: ionice -c3 -p <daemon_pid>");
        return 0;
    }

    /* Set our own IO scheduling class to idle (best-effort, lowest priority) */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ionice -c2 -n7 -p %d 2>/dev/null", getpid());
    return execute_command(cmd);
}

/* ============================================================
 *  Read-only remount
 * ============================================================ */

static int heal_readonly(const device_info_t *dev, int auto_mode) {
    char mountpoint[256] = {0};

    if (find_mountpoint(dev->path, mountpoint, sizeof(mountpoint)) != 0) {
        logger_write(LOG_WARN,
                     "self_healing: no mount point found for %s, "
                     "cannot remount read-only", dev->path);
        return -1;
    }

    logger_write(LOG_WARN, "self_healing: remounting %s as read-only", mountpoint);

    if (!auto_mode) {
        logger_write(LOG_INFO, "self_healing: [DRY-RUN] would run: "
                     "mount -o remount,ro %s", mountpoint);
        return 0;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mount -o remount,ro %s 2>/dev/null", mountpoint);
    int rc = execute_command(cmd);
    if (rc == 0) {
        logger_write(LOG_INFO, "self_healing: %s remounted read-only", mountpoint);
    }
    return rc;
}

/* ============================================================
 *  Disk isolation (remove from kernel's device list)
 * ============================================================ */

static int heal_isolate(const device_info_t *dev, int auto_mode) {
    char sys_path[256];
    snprintf(sys_path, sizeof(sys_path),
             "/sys/block/%s/device/delete", dev->name);

    logger_write(LOG_CRITICAL, "self_healing: isolating disk %s", dev->path);

    if (!auto_mode) {
        logger_write(LOG_INFO, "self_healing: [DRY-RUN] would write '1' to %s",
                     sys_path);
        return 0;
    }

    /* Write "1" to the sysfs delete file */
    int fd = open(sys_path, O_WRONLY);
    if (fd < 0) {
        logger_write(LOG_WARN, "self_healing: cannot open %s: %s",
                     sys_path, strerror(errno));
        return -1;
    }

    if (write(fd, "1", 1) != 1) {
        logger_write(LOG_WARN, "self_healing: write to %s failed: %s",
                     sys_path, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    logger_write(LOG_CRITICAL, "self_healing: disk %s isolated successfully",
                 dev->path);
    return 0;
}

/* ============================================================
 *  Failover (log event — actual failover is system-specific)
 * ============================================================ */

static int heal_failover(const device_info_t *dev, int auto_mode) {
    (void)auto_mode;
    logger_write(LOG_CRITICAL,
                 "self_healing: FAILOVER triggered for %s — "
                 "redirecting IO to backup device (system-specific)",
                 dev->path);
    /* In a real medical device, this would trigger a hardware failover
     * or redirect IO to a redundant storage path. Here we log it. */
    return 0;
}

/* ============================================================
 *  Emergency mode
 * ============================================================ */

static int heal_emergency(const device_info_t *dev, int auto_mode) {
    (void)auto_mode;
    logger_write(LOG_CRITICAL,
                 "self_healing: EMERGENCY mode for %s — "
                 "freeze non-critical services, force safe state, "
                 "alert external monitoring system", dev->path);
    /* In a real medical device, this would signal the system supervisor
     * to enter a safe state. Here we log it at CRITICAL level. */
    return 0;
}

/* ============================================================
 *  Public dispatcher
 * ============================================================ */

int self_healing_execute(const policy_decision_t *decision,
                         const device_info_t     *dev,
                         int                      auto_mode) {
    if (!decision || !dev) return -1;

    const char *mode_str = auto_mode ? "EXECUTE" : "DRY-RUN";
    logger_write(LOG_INFO, "self_healing: [%s] action=%s level=%d desc=\"%s\"",
                 mode_str,
                 healing_action_label(decision->action),
                 (int)decision->level,
                 decision->description);

    if (decision->require_confirmation && !auto_mode) {
        logger_write(LOG_WARN,
                     "self_healing: action '%s' requires confirmation — "
                     "use --auto-heal to execute",
                     healing_action_label(decision->action));
    }

    switch (decision->action) {
    case ACTION_NONE:
    case ACTION_WARN:
        /* Log only — already logged above */
        return 0;

    case ACTION_THROTTLE:
        return heal_throttle_io(dev, auto_mode);

    case ACTION_REDUCE_IO:
        logger_write(LOG_INFO,
                     "self_healing: [%s] would increase monitoring interval "
                     "to reduce IO load on %s", mode_str, dev->path);
        return 0;

    case ACTION_READONLY:
        return heal_readonly(dev, auto_mode);

    case ACTION_ISOLATE:
        return heal_isolate(dev, auto_mode);

    case ACTION_FAILOVER:
        return heal_failover(dev, auto_mode);

    case ACTION_EMERGENCY:
        return heal_emergency(dev, auto_mode);

    default:
        logger_write(LOG_WARN, "self_healing: unknown action %d",
                     (int)decision->action);
        return -1;
    }
}
