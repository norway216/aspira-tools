/*
 * AIC8800 USB WiFi Driver - Recovery and Self-Healing
 *
 * 6-level recovery system with rate limiting to prevent recovery storms.
 * Handles: URB errors, TX timeout, firmware heartbeat loss, scan/connect
 * stalls, USB endpoint halt, and FW crashes.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_RECOVERY_H__
#define __AIC_RECOVERY_H__

#include <linux/types.h>
#include <linux/workqueue.h>

/* ================================================================== */
/* Recovery Levels                                                     */
/* ================================================================== */

enum aic_recovery_level {
	AIC_RECOVERY_NONE         = 0,  /* ignore single error, count */
	AIC_RECOVERY_RESTART_QUEUES = 1, /* restart TX/RX queues */
	AIC_RECOVERY_CLEAR_HALT   = 2,  /* clear USB halt + re-submit URBs */
	AIC_RECOVERY_FW_SOFT_RESET = 3, /* firmware soft reset */
	AIC_RECOVERY_USB_RESET    = 4,  /* USB device reset */
	AIC_RECOVERY_REINIT       = 5,  /* remove/probe level re-init */
	AIC_RECOVERY_DEGRADED     = 6,  /* stop auto-recovery, notify user */
};

/* ================================================================== */
/* Recovery Reasons                                                    */
/* ================================================================== */

enum aic_recovery_reason {
	AIC_RECOVERY_REASON_NONE          = 0,
	AIC_RECOVERY_REASON_URB_ERROR     = 1,
	AIC_RECOVERY_REASON_TX_TIMEOUT    = 2,
	AIC_RECOVERY_REASON_FW_HEARTBEAT  = 3,
	AIC_RECOVERY_REASON_SCAN_STUCK    = 4,
	AIC_RECOVERY_REASON_CONNECT_STUCK = 5,
	AIC_RECOVERY_REASON_USB_EP_HALT   = 6,
	AIC_RECOVERY_REASON_FW_ERROR      = 7,
	AIC_RECOVERY_REASON_FW_CRASH      = 8,
	AIC_RECOVERY_REASON_STATE_MISMATCH = 9,
};

/* ================================================================== */
/* Rate Limiter                                                        */
/* ================================================================== */

#define AIC_RECOVERY_WINDOW_1MIN    (60 * HZ)
#define AIC_RECOVERY_WINDOW_10MIN   (600 * HZ)
#define AIC_RECOVERY_MAX_1MIN       3
#define AIC_RECOVERY_MAX_USB_10MIN  2

struct aic_recovery_rate_limit {
	int             count_1min;
	unsigned long   window_start_1min;
	int             usb_reset_count_10min;
	unsigned long   window_start_10min;
};

/* ================================================================== */
/* Recovery Subsystem                                                  */
/* ================================================================== */

struct aic_recovery {
	enum aic_recovery_level     level;
	enum aic_recovery_reason    last_reason;
	int                        total_count;
	int                        fail_count;
	unsigned long               last_recovery_jiffies;

	struct aic_recovery_rate_limit rl;
	bool                       enabled;
};

/* ================================================================== */
/* Recovery API                                                        */
/* ================================================================== */

int  aic_recovery_init(struct aic_recovery *rec);
void aic_recovery_deinit(struct aic_recovery *rec);

/* Schedule recovery work */
int  aic_recovery_schedule(struct aic_dev *adev,
			   enum aic_recovery_level level,
			   enum aic_recovery_reason reason);

/* Recovery work callback */
void aic_recovery_work(struct work_struct *work);

/* Execute recovery procedure */
int  aic_recovery_execute(struct aic_dev *adev);

/* Periodic health check and link watch (defined in aic_recovery.c) */
void aic_health_check_work(struct work_struct *work);
void aic_link_watch_work(struct work_struct *work);

/* Rate limit check */
bool aic_recovery_rate_limit_ok(struct aic_dev *adev,
				enum aic_recovery_level level);

/* Level-to-string helpers */
const char *aic_recovery_level_name(enum aic_recovery_level lvl);
const char *aic_recovery_reason_name(enum aic_recovery_reason rsn);

#endif /* __AIC_RECOVERY_H__ */
