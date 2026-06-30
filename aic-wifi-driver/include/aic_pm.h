/*
 * AIC8800 USB WiFi Driver - Power Management
 *
 * System suspend/resume, runtime PM, and USB autosuspend policy.
 * Conservative defaults for industrial/medical use.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_PM_H__
#define __AIC_PM_H__

#include <linux/types.h>
#include <linux/pm.h>

/* ================================================================== */
/* Power Management State                                              */
/* ================================================================== */

enum aic_pm_state {
	AIC_PM_ACTIVE    = 0,
	AIC_PM_SUSPENDING,
	AIC_PM_SUSPENDED,
	AIC_PM_RESUMING,
};

/* ================================================================== */
/* PM Subsystem Structure                                              */
/* ================================================================== */

struct aic_pm {
	enum aic_pm_state     state;
	bool                  wowlan_enabled;
	bool                  autosuspend_disabled;
	int                   autosuspend_delay_ms;
	unsigned long         last_active_jiffies;
};

/* Default autosuspend delay (negative = disabled) */
#define AIC_PM_AUTOSUSPEND_DELAY_MS   -1

/* ================================================================== */
/* PM API                                                              */
/* ================================================================== */

int  aic_pm_init(struct aic_pm *pm);
void aic_pm_deinit(struct aic_pm *pm);

/* System suspend/resume */
int  aic_pm_suspend(struct aic_dev *adev);
int  aic_pm_resume(struct aic_dev *adev);

/* Runtime PM */
int  aic_pm_runtime_suspend(struct aic_dev *adev);
int  aic_pm_runtime_resume(struct aic_dev *adev);
void aic_pm_runtime_mark_active(struct aic_dev *adev);

/* WoWLAN */
int  aic_pm_set_wowlan(struct aic_dev *adev, bool enable);

/* Autosuspend policy */
void aic_pm_disable_autosuspend(struct aic_dev *adev);
void aic_pm_enable_autosuspend(struct aic_dev *adev);

#endif /* __AIC_PM_H__ */
