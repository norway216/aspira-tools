/*
 * AIC8800 USB WiFi Driver - Recovery and Self-Healing Engine
 *
 * 6-level recovery system with rate limiting. Handles URB errors,
 * TX timeout, firmware heartbeat loss, USB endpoint halt,
 * and firmware crashes. Recovery storms are prevented by
 * rate limiting: max 3/min, max 2 USB resets per 10 minutes.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_recovery.h"
#include "../include/aic_usb.h"
#include "../include/aic_rx.h"
#include "../include/aic_tx.h"
#include "../include/aic_fw.h"
#include "../include/aic_cmd.h"
#include "../include/aic_cfg80211.h"
#include "../include/aic_trace.h"

#include <linux/delay.h>

/* ================================================================== */
/* Recovery Init / Deinit                                               */
/* ================================================================== */

int aic_recovery_init(struct aic_recovery *rec)
{
	rec->level = AIC_RECOVERY_NONE;
	rec->last_reason = AIC_RECOVERY_REASON_NONE;
	rec->total_count = 0;
	rec->fail_count = 0;
	rec->last_recovery_jiffies = 0;
	rec->enabled = true;

	memset(&rec->rl, 0, sizeof(rec->rl));
	rec->rl.window_start_1min = jiffies;
	rec->rl.window_start_10min = jiffies;

	return 0;
}

void aic_recovery_deinit(struct aic_recovery *rec)
{
	/* No dynamic resources */
}

/* ================================================================== */
/* Name Helpers                                                         */
/* ================================================================== */

const char *aic_recovery_level_name(enum aic_recovery_level lvl)
{
	switch (lvl) {
	case AIC_RECOVERY_NONE:           return "NONE";
	case AIC_RECOVERY_RESTART_QUEUES:  return "RESTART_QUEUES";
	case AIC_RECOVERY_CLEAR_HALT:      return "CLEAR_HALT";
	case AIC_RECOVERY_FW_SOFT_RESET:   return "FW_SOFT_RESET";
	case AIC_RECOVERY_USB_RESET:       return "USB_RESET";
	case AIC_RECOVERY_REINIT:          return "REINIT";
	case AIC_RECOVERY_DEGRADED:        return "DEGRADED";
	default:                            return "UNKNOWN";
	}
}

const char *aic_recovery_reason_name(enum aic_recovery_reason rsn)
{
	switch (rsn) {
	case AIC_RECOVERY_REASON_URB_ERROR:     return "urb_error";
	case AIC_RECOVERY_REASON_TX_TIMEOUT:    return "tx_timeout";
	case AIC_RECOVERY_REASON_FW_HEARTBEAT:  return "fw_heartbeat";
	case AIC_RECOVERY_REASON_SCAN_STUCK:    return "scan_stuck";
	case AIC_RECOVERY_REASON_CONNECT_STUCK: return "connect_stuck";
	case AIC_RECOVERY_REASON_USB_EP_HALT:   return "usb_ep_halt";
	case AIC_RECOVERY_REASON_FW_ERROR:      return "fw_error";
	case AIC_RECOVERY_REASON_FW_CRASH:      return "fw_crash";
	case AIC_RECOVERY_REASON_STATE_MISMATCH: return "state_mismatch";
	default:                                 return "unknown";
	}
}

/* ================================================================== */
/* Rate Limiter                                                         */
/* ================================================================== */

bool aic_recovery_rate_limit_ok(struct aic_dev *adev,
				enum aic_recovery_level level)
{
	struct aic_recovery_rate_limit *rl = &adev->recovery.rl;
	unsigned long now = jiffies;
	bool ok = true;

	/* Reset 1-minute window if expired */
	if (time_after(now, rl->window_start_1min + AIC_RECOVERY_WINDOW_1MIN)) {
		rl->count_1min = 0;
		rl->window_start_1min = now;
	}

	/* Reset 10-minute window if expired */
	if (time_after(now, rl->window_start_10min + AIC_RECOVERY_WINDOW_10MIN)) {
		rl->usb_reset_count_10min = 0;
		rl->window_start_10min = now;
	}

	/* Check 1-minute limit */
	if (rl->count_1min >= AIC_RECOVERY_MAX_1MIN) {
		aic_warn(adev, "recovery rate limit: %d/min exceeded\n",
			 rl->count_1min);
		return false;
	}

	/* Check USB reset limit (level >= 4) */
	if (level >= AIC_RECOVERY_USB_RESET &&
	    rl->usb_reset_count_10min >= AIC_RECOVERY_MAX_USB_10MIN) {
		aic_warn(adev, "USB reset rate limit: %d/10min exceeded\n",
			 rl->usb_reset_count_10min);
		return false;
	}

	rl->count_1min++;
	if (level >= AIC_RECOVERY_USB_RESET)
		rl->usb_reset_count_10min++;

	return ok;
}

/* ================================================================== */
/* Schedule Recovery                                                    */
/* ================================================================== */

int aic_recovery_schedule(struct aic_dev *adev,
			  enum aic_recovery_level level,
			  enum aic_recovery_reason reason)
{
	if (!adev->recovery_enabled) {
		aic_dbg(adev, "recovery disabled, ignoring trigger\n");
		return 0;
	}

	if (!aic_state_can_recover(adev->state)) {
		aic_dbg(adev, "cannot recover in state %s\n",
			aic_state_name(adev->state));
		return -EINVAL;
	}

	if (atomic_read(&adev->reset_pending)) {
		aic_dbg(adev, "recovery already pending\n");
		return 0;
	}

	/* Rate limit check */
	if (!aic_recovery_rate_limit_ok(adev, level)) {
		aic_warn(adev, "recovery rate limited, entering degraded mode\n");
		adev->recovery.level = AIC_RECOVERY_DEGRADED;
		adev->recovery.last_reason = reason;
		/* Notify userspace that driver needs manual intervention */
		return -EBUSY;
	}

	adev->recovery.level = level;
	adev->recovery.last_reason = reason;
	adev->recovery.total_count++;

	atomic_set(&adev->reset_pending, 1);

	trace_aic_recovery(netdev_name(adev->ndev), level,
			   aic_recovery_reason_name(reason));

	aic_log_raw(adev, "RECOVERY", "level=%d (%s) reason=%s count=%d\n",
		    level, aic_recovery_level_name(level),
		    aic_recovery_reason_name(reason),
		    adev->recovery.total_count);

	if (!adev->removing && adev->wq)
		queue_work(adev->wq, &adev->recovery_work);

	return 0;
}

/* ================================================================== */
/* Recovery Work Callback                                                */
/* ================================================================== */

void aic_recovery_work(struct work_struct *work)
{
	struct aic_dev *adev = container_of(work, struct aic_dev,
					    recovery_work);

	mutex_lock(&adev->op_mutex);

	if (adev->removing || adev->surprise_removed) {
		mutex_unlock(&adev->op_mutex);
		return;
	}

	aic_state_set(adev, AIC_STATE_RECOVERING);

	aic_recovery_execute(adev);

	atomic_set(&adev->reset_pending, 0);

	mutex_unlock(&adev->op_mutex);
}

/* ================================================================== */
/* Recovery Execution                                                   */
/* ================================================================== */

int aic_recovery_execute(struct aic_dev *adev)
{
	int ret = 0;

	aic_info(adev, "executing recovery level=%d (%s)\n",
		 adev->recovery.level,
		 aic_recovery_level_name(adev->recovery.level));

	switch (adev->recovery.level) {
	case AIC_RECOVERY_NONE:
		/* Nothing to do — just count the error */
		break;

	case AIC_RECOVERY_RESTART_QUEUES:
		/* Flush and restart TX/RX queues */
		aic_tx_flush_all(adev);
		aic_rx_submit_urbs(adev);
		if (adev->ndev)
			netif_wake_queue(adev->ndev);
		break;

	case AIC_RECOVERY_CLEAR_HALT:
		/* Clear USB endpoint halt */
		aic_usb_kill_all_urbs(adev);
		if (adev->udev) {
			ret = usb_clear_halt(adev->udev,
					     adev->usb.bulk_in_pipe);
			if (ret)
				aic_err(adev, "clear halt IN failed: %d\n", ret);
			ret = usb_clear_halt(adev->udev,
					     adev->usb.bulk_out_pipe);
			if (ret)
				aic_err(adev, "clear halt OUT failed: %d\n",
					ret);
		}
		msleep(100);
		aic_rx_submit_urbs(adev);
		if (adev->ndev)
			netif_wake_queue(adev->ndev);
		break;

	case AIC_RECOVERY_FW_SOFT_RESET:
		/* Firmware soft reset */
		aic_usb_kill_all_urbs(adev);
		aic_cmd_flush_all(adev);
		aic_tx_flush_all(adev);

		if (adev->ndev) {
			netif_carrier_off(adev->ndev);
			netif_stop_queue(adev->ndev);
		}

		ret = aic_fw_soft_reset(adev);
		if (ret) {
			aic_err(adev, "FW soft reset failed: %d\n", ret);
			adev->recovery.fail_count++;
			aic_stats_inc(&adev->stats.recovery_failures);
			/*
			 * Escalate to USB reset directly (not via schedule
			 * which checks reset_pending and would silently
			 * drop the escalation).
			 */
			adev->recovery.level = AIC_RECOVERY_USB_RESET;
			adev->recovery.last_reason =
				AIC_RECOVERY_REASON_FW_CRASH;
			/* Fall through to USB_RESET case below */
		} else {
			/* Wait for firmware ready */
			ret = aic_fw_wait_ready(adev, 5000);
			if (ret) {
				aic_err(adev, "FW ready wait failed after "
					"reset\n");
				adev->recovery.fail_count++;
				aic_stats_inc(&adev->stats.recovery_failures);
				break;
			}

			aic_rx_submit_urbs(adev);

			if (adev->bssid[0] && adev->ndev) {
				/*
				 * Was connected — firmware lost association.
				 * Notify cfg80211 to trigger reconnection.
				 */
				aic_cfg80211_notify_disconnect(adev,
					WLAN_REASON_PREV_AUTH_NOT_VALID);
			}
			aic_state_set(adev, AIC_STATE_NETDEV_REGISTERED);
			aic_stats_inc(&adev->stats.recovery_count);
			break;
		}
		/* If soft reset failed, fall through to USB_RESET */
		if (adev->recovery.level != AIC_RECOVERY_USB_RESET)
			break;
		/* fallthrough */
	case AIC_RECOVERY_USB_RESET:

	case AIC_RECOVERY_USB_RESET:
		/* USB device reset */
		aic_usb_kill_all_urbs(adev);
		aic_cmd_flush_all(adev);
		aic_tx_flush_all(adev);

		if (adev->ndev) {
			netif_carrier_off(adev->ndev);
			netif_stop_queue(adev->ndev);
		}

		if (adev->udev) {
			ret = usb_lock_device_for_reset(adev->udev,
							adev->intf);
			if (ret >= 0) {
				ret = usb_reset_device(adev->udev);
				if (ret)
					aic_err(adev, "USB reset failed: %d\n",
						ret);
			}
		}

		aic_stats_inc(&adev->stats.usb_reset_count);

		/* Re-download firmware */
		adev->fw.loaded = false;
		adev->fw_ready = false;
		ret = aic_fw_download_and_boot(adev);
		if (ret) {
			aic_err(adev, "FW re-download after USB reset "
				"failed: %d\n", ret);
			adev->recovery.fail_count++;
			aic_stats_inc(&adev->stats.recovery_failures);
			break;
		}

		ret = aic_fw_wait_ready(adev, 10000);
		if (ret) {
			aic_err(adev, "FW not ready after USB reset\n");
			break;
		}

		aic_rx_submit_urbs(adev);
		aic_state_set(adev, AIC_STATE_NETDEV_REGISTERED);
		aic_stats_inc(&adev->stats.recovery_count);
		break;

	case AIC_RECOVERY_REINIT:
		/* Full driver-level re-initialization.
		 * In practice this would unregister/register wiphy,
		 * but is very disruptive. Usually delegated to
		 * userspace (unload/reload module).
		 */
		aic_err(adev, "full reinit recovery not implemented — "
			"module reload required\n");
		aic_usb_kill_all_urbs(adev);
		aic_cmd_flush_all(adev);
		aic_tx_flush_all(adev);

		if (adev->ndev) {
			netif_carrier_off(adev->ndev);
			netif_stop_queue(adev->ndev);
		}

		aic_state_set(adev, AIC_STATE_DEAD);
		break;

	case AIC_RECOVERY_DEGRADED:
		aic_err(adev, "driver in DEGRADED state — requires "
			"manual intervention (module reload)\n");
		break;
	}

	adev->recovery.last_recovery_jiffies = jiffies;

	return ret;
}

/* ================================================================== */
/* Health Check Work (periodic)                                         */
/* ================================================================== */

void aic_health_check_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct aic_dev *adev = container_of(dwork, struct aic_dev,
					    health_check_work);

	if (adev->removing)
		return;

	/* Check firmware heartbeat */
	if (aic_state_is_online(adev->state)) {
		if (aic_fw_check_heartbeat(adev)) {
			adev->recovery.heartbeat_miss_count = 0;
		} else {
			adev->recovery.heartbeat_miss_count++;
			aic_warn(adev, "heartbeat missed %d/%d\n",
				 adev->recovery.heartbeat_miss_count,
				 AIC_HEARTBEAT_MAX_MISS);

			if (adev->recovery.heartbeat_miss_count >=
			    AIC_HEARTBEAT_MAX_MISS) {
				aic_err(adev, "firmware heartbeat lost — "
					"scheduling recovery\n");
				aic_stats_inc(&adev->stats.fw_heartbeat_timeout);
				aic_recovery_schedule(adev,
					AIC_RECOVERY_FW_SOFT_RESET,
					AIC_RECOVERY_REASON_FW_HEARTBEAT);
				adev->recovery.heartbeat_miss_count = 0;
			}
		}
	}

	/* Check for cfg80211 vs firmware state mismatch */
	if (adev->state == AIC_STATE_CONNECTED && adev->ndev) {
		if (!netif_carrier_ok(adev->ndev)) {
			aic_warn(adev, "state=CONNECTED but carrier off — "
				 "triggering recovery\n");
			aic_recovery_schedule(adev,
				AIC_RECOVERY_RESTART_QUEUES,
				AIC_RECOVERY_REASON_STATE_MISMATCH);
		}
	}

	/* Re-arm health check */
	queue_delayed_work(adev->wq, &adev->health_check_work,
			   msecs_to_jiffies(AIC_HEARTBEAT_INTERVAL_MS));
}

/* ================================================================== */
/* Link Watch Work (periodic, only when connected)                       */
/* ================================================================== */

void aic_link_watch_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct aic_dev *adev = container_of(dwork, struct aic_dev,
					    link_watch_work);

	if (adev->removing)
		return;

	if (adev->state != AIC_STATE_CONNECTED)
		return;

	/* Check that carrier is on and TX/RX counters are moving */
	if (adev->ndev && netif_carrier_ok(adev->ndev)) {
		s64 tx = atomic64_read(&adev->stats.tx_packets);
		s64 rx = atomic64_read(&adev->stats.rx_packets);

		/*
		 * If TX counter hasn't changed and we have
		 * pending TX frames, something might be stuck
		 */
		if (tx == adev->stats.last_tx_jiffies &&
		    atomic_read(&adev->tx_pending) > 0) {
			aic_warn(adev, "TX appears stuck — potential issue\n");
		}

		adev->stats.last_tx_jiffies = tx;
		adev->stats.last_rx_jiffies = rx;
	}

	/* Re-arm link watch (every 3 seconds when connected) */
	queue_delayed_work(adev->wq, &adev->link_watch_work,
			   msecs_to_jiffies(3000));
}
