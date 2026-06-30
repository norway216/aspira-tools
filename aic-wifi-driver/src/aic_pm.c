/*
 * AIC8800 USB WiFi Driver - Power Management
 *
 * System suspend/resume, runtime PM, USB autosuspend policy.
 * Conservative defaults for industrial/medical equipment:
 * autosuspend disabled, power_save off by default.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_pm.h"
#include "../include/aic_usb.h"
#include "../include/aic_rx.h"
#include "../include/aic_tx.h"
#include "../include/aic_fw.h"
#include "../include/aic_recovery.h"

#include <linux/pm_runtime.h>

/* ================================================================== */
/* PM Init / Deinit                                                     */
/* ================================================================== */

int aic_pm_init(struct aic_pm *pm)
{
	pm->state = AIC_PM_ACTIVE;
	pm->wowlan_enabled = false;
	pm->autosuspend_disabled = false;
	pm->autosuspend_delay_ms = AIC_PM_AUTOSUSPEND_DELAY_MS;
	pm->last_active_jiffies = jiffies;
	return 0;
}

void aic_pm_deinit(struct aic_pm *pm)
{
	/* No dynamic resources to free */
}

/* ================================================================== */
/* System Suspend                                                       */
/* ================================================================== */

int aic_pm_suspend(struct aic_dev *adev)
{
	aic_dbg(adev, "PM suspend start\n");

	mutex_lock(&adev->op_mutex);

	if (adev->state == AIC_STATE_REMOVING ||
	    adev->state == AIC_STATE_DEAD) {
		mutex_unlock(&adev->op_mutex);
		return 0;
	}

	adev->pm.state = AIC_PM_SUSPENDING;
	aic_state_set(adev, AIC_STATE_SUSPENDING);

	/* Stop TX queue */
	if (adev->ndev) {
		netif_stop_queue(adev->ndev);
		netif_carrier_off(adev->ndev);
	}

	/* If connected, send sleep command to firmware */
	if (adev->state == AIC_STATE_CONNECTED) {
		/*
		 * In production: send AIC_CMD_SET_POWER_MGMT with
		 * sleep mode to preserve association during suspend
		 */
	}

	/* Flush any pending TX */
	aic_tx_flush_all(adev);

	/* Kill RX URBs to stop USB traffic */
	aic_usb_kill_all_urbs(adev);

	adev->pm.state = AIC_PM_SUSPENDED;
	aic_state_set(adev, AIC_STATE_SUSPENDED);

	aic_stats_inc(&adev->stats.suspend_count);

	mutex_unlock(&adev->op_mutex);

	aic_dbg(adev, "PM suspend complete\n");

	return 0;
}

/* ================================================================== */
/* System Resume                                                        */
/* ================================================================== */

int aic_pm_resume(struct aic_dev *adev)
{
	int ret;

	aic_dbg(adev, "PM resume start\n");

	mutex_lock(&adev->op_mutex);

	if (adev->state == AIC_STATE_REMOVING ||
	    adev->state == AIC_STATE_DEAD) {
		mutex_unlock(&adev->op_mutex);
		return 0;
	}

	adev->pm.state = AIC_PM_RESUMING;

	/* Resume USB interface */
	if (adev->udev) {
		ret = usb_autopm_get_interface(adev->intf);
		if (ret < 0) {
			aic_err(adev, "usb_autopm_get_interface failed: %d\n",
				ret);
		}
	}

	/* Check if firmware is still alive */
	if (!aic_fw_check_heartbeat(adev)) {
		aic_warn(adev, "firmware not responding after resume — "
			 "triggering recovery\n");
		/* Release autopm reference before scheduling recovery */
		if (adev->udev)
			usb_autopm_put_interface(adev->intf);
		aic_recovery_schedule(adev, AIC_RECOVERY_FW_SOFT_RESET,
				      AIC_RECOVERY_REASON_FW_HEARTBEAT);
		mutex_unlock(&adev->op_mutex);
		return 0;
	}

	/* Re-submit RX URBs */
	ret = aic_rx_submit_urbs(adev);
	if (ret) {
		aic_warn(adev, "RX URB re-submit after resume failed: %d\n",
			 ret);
	}

	/* Wake TX queue */
	if (adev->ndev)
		netif_wake_queue(adev->ndev);

	/* Release autopm reference acquired above */
	if (adev->udev)
		usb_autopm_put_interface(adev->intf);

	/* Mark PM active */
	adev->pm.state = AIC_PM_ACTIVE;
	adev->pm.last_active_jiffies = jiffies;

	if (adev->bssid[0]) {
		/* Was connected before suspend */
		aic_state_set(adev, AIC_STATE_CONNECTED);
		if (adev->ndev)
			netif_carrier_on(adev->ndev);
	} else {
		aic_state_set(adev, AIC_STATE_NETDEV_REGISTERED);
	}

	aic_stats_inc(&adev->stats.resume_count);

	mutex_unlock(&adev->op_mutex);

	aic_dbg(adev, "PM resume complete\n");

	return 0;
}

/* ================================================================== */
/* Runtime PM                                                           */
/* ================================================================== */

int aic_pm_runtime_suspend(struct aic_dev *adev)
{
	aic_dbg(adev, "runtime suspend\n");

	if (adev->disable_autosuspend)
		return -EBUSY;

	/* Similar to system suspend but lighter */
	aic_usb_kill_all_urbs(adev);
	adev->pm.state = AIC_PM_SUSPENDED;

	return 0;
}

int aic_pm_runtime_resume(struct aic_dev *adev)
{
	aic_dbg(adev, "runtime resume\n");

	adev->pm.state = AIC_PM_RESUMING;

	aic_rx_submit_urbs(adev);

	adev->pm.state = AIC_PM_ACTIVE;
	adev->pm.last_active_jiffies = jiffies;

	return 0;
}

void aic_pm_runtime_mark_active(struct aic_dev *adev)
{
	adev->pm.last_active_jiffies = jiffies;
}

/* ================================================================== */
/* WoWLAN                                                               */
/* ================================================================== */

int aic_pm_set_wowlan(struct aic_dev *adev, bool enable)
{
	aic_dbg(adev, "set_wowlan: %d\n", enable);
	adev->pm.wowlan_enabled = enable;
	return 0;
}

/* ================================================================== */
/* Autosuspend Policy                                                    */
/* ================================================================== */

void aic_pm_disable_autosuspend(struct aic_dev *adev)
{
	if (!adev->pm.autosuspend_disabled && adev->udev) {
		usb_disable_autosuspend(adev->udev);
		adev->pm.autosuspend_disabled = true;
		aic_info(adev, "USB autosuspend disabled\n");
	}
}

void aic_pm_enable_autosuspend(struct aic_dev *adev)
{
	if (adev->pm.autosuspend_disabled && adev->udev) {
		pm_runtime_set_autosuspend_delay(&adev->udev->dev,
						 adev->pm.autosuspend_delay_ms);
		pm_runtime_use_autosuspend(&adev->udev->dev);
		pm_runtime_put_noidle(&adev->udev->dev);
		adev->pm.autosuspend_disabled = false;
		aic_info(adev, "USB autosuspend enabled (delay=%dms)\n",
			 adev->pm.autosuspend_delay_ms);
	}
}
