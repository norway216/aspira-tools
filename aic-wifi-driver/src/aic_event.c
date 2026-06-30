/*
 * AIC8800 USB WiFi Driver - Firmware Event Handling
 *
 * Event queue with demux dispatcher, work scheduling, and
 * handler table for all firmware event types.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_event.h"
#include "../include/aic_cmd.h"
#include "../include/aic_cfg80211.h"
#include "../include/aic_recovery.h"
#include "../include/aic_trace.h"

#include <linux/slab.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

/* ================================================================== */
/* Event Manager Init / Deinit                                         */
/* ================================================================== */

int aic_event_mgr_init(struct aic_event_mgr *mgr)
{
	spin_lock_init(&mgr->lock);
	mgr->head = 0;
	mgr->tail = 0;
	atomic_set(&mgr->count, 0);
	atomic_set(&mgr->dropped, 0);
	mgr->last_event_idx = 0;
	memset(mgr->last_events, 0, sizeof(mgr->last_events));
	return 0;
}

void aic_event_mgr_deinit(struct aic_event_mgr *mgr)
{
	/* Nothing to free — all data is static */
}

/* ================================================================== */
/* Event Enqueue (called from RX path)                                 */
/* ================================================================== */

int aic_event_enqueue(struct aic_dev *adev, u16 event_id,
		      const u8 *payload, u16 len)
{
	struct aic_event_mgr *mgr = &adev->event;
	unsigned long flags;
	struct aic_event_entry *entry;

	spin_lock_irqsave(&mgr->lock, flags);

	/* Check queue full */
	if (((mgr->tail + 1) % AIC_EVENT_QUEUE_DEPTH) == mgr->head) {
		atomic64_inc(&mgr->dropped);
		spin_unlock_irqrestore(&mgr->lock, flags);
		aic_warn(adev, "event queue full, dropping event 0x%04x\n",
			 event_id);
		return -ENOSPC;
	}

	entry = &mgr->queue[mgr->tail];
	entry->event_id = event_id;
	entry->timestamp_ms = aic_ktime_get_ms();

	if (len > AIC_EVENT_MAX_PAYLOAD)
		len = AIC_EVENT_MAX_PAYLOAD;

	entry->payload_len = len;
	if (payload && len > 0)
		memcpy(entry->payload, payload, len);

	mgr->tail = (mgr->tail + 1) % AIC_EVENT_QUEUE_DEPTH;
	atomic_inc(&mgr->count);

	/* Save to last_events ring buffer */
	mgr->last_events[mgr->last_event_idx] = *entry;
	mgr->last_event_idx = (mgr->last_event_idx + 1) % 16;

	spin_unlock_irqrestore(&mgr->lock, flags);

	trace_aic_fw_event(netdev_name(adev->ndev), event_id, len);

	return 0;
}

/* ================================================================== */
/* Event Dequeue                                                        */
/* ================================================================== */

int aic_event_dequeue(struct aic_dev *adev, struct aic_event_entry *out)
{
	struct aic_event_mgr *mgr = &adev->event;
	unsigned long flags;
	int ret = -ENOENT;

	spin_lock_irqsave(&mgr->lock, flags);

	if (mgr->head != mgr->tail) {
		*out = mgr->queue[mgr->head];
		mgr->head = (mgr->head + 1) % AIC_EVENT_QUEUE_DEPTH;
		atomic_dec(&mgr->count);
		ret = 0;
	}

	spin_unlock_irqrestore(&mgr->lock, flags);

	return ret;
}

/* ================================================================== */
/* Event Schedule                                                       */
/* ================================================================== */

void aic_event_schedule(struct aic_dev *adev)
{
	if (!adev->removing && adev->wq)
		queue_work(adev->wq, &adev->event_work);
}

/* ================================================================== */
/* Event Work Callback                                                  */
/* ================================================================== */

void aic_event_work(struct work_struct *work)
{
	struct aic_dev *adev = container_of(work, struct aic_dev, event_work);
	struct aic_event_entry entry;

	while (aic_event_dequeue(adev, &entry) == 0) {
		/* Try to match to a pending command first */
		aic_cmd_complete_event(adev, entry.event_id,
				       entry.payload, entry.payload_len);

		/* Dispatch to specific handler */
		aic_event_dispatch(adev, &entry);
	}
}

/* ================================================================== */
/* Event Dispatch Table                                                 */
/* ================================================================== */

typedef int (*event_handler_t)(struct aic_dev *adev,
			       const u8 *payload, u16 len);

static const event_handler_t event_handlers[] = {
	[0x0001] = (event_handler_t)NULL,  /* FW_READY — handled in probe */
	[0x0010] = (event_handler_t)NULL,  /* SCAN_RESULT */
	[0x0011] = (event_handler_t)NULL,  /* SCAN_COMPLETE */
	[0x0020] = (event_handler_t)NULL,  /* CONNECT_RESULT */
	[0x0021] = (event_handler_t)NULL,  /* DISCONNECT */
	[0x00F0] = (event_handler_t)NULL,  /* HEARTBEAT */
	[0x00FE] = (event_handler_t)NULL,  /* FW_ERROR */
	[0x00FF] = (event_handler_t)NULL,  /* FW_CRASH */
};

int aic_event_dispatch(struct aic_dev *adev,
		       const struct aic_event_entry *entry)
{
	switch (entry->event_id) {
	case AIC_EVENT_FW_READY:
		return aic_event_handle_fw_ready(adev, entry->payload,
						 entry->payload_len);
	case AIC_EVENT_SCAN_RESULT:
		return aic_event_handle_scan_result(adev, entry->payload,
						    entry->payload_len);
	case AIC_EVENT_SCAN_COMPLETE:
		return aic_event_handle_scan_complete(adev, entry->payload,
						      entry->payload_len);
	case AIC_EVENT_CONNECT_RESULT:
		return aic_event_handle_connect_result(adev, entry->payload,
						       entry->payload_len);
	case AIC_EVENT_DISCONNECT:
	case AIC_EVENT_DEAUTH_IND:
		return aic_event_handle_disconnect(adev, entry->payload,
						   entry->payload_len);
	case AIC_EVENT_HEARTBEAT:
		return aic_event_handle_heartbeat(adev, entry->payload,
						  entry->payload_len);
	case AIC_EVENT_FW_ERROR:
	case AIC_EVENT_FW_CRASH:
		return aic_event_handle_fw_error(adev, entry->payload,
						 entry->payload_len);
	default:
		aic_dbg(adev, "unhandled event 0x%04x len=%u\n",
			entry->event_id, entry->payload_len);
		return 0;
	}
}

/* ================================================================== */
/* Specific Event Handlers                                              */
/* ================================================================== */

int aic_event_handle_fw_ready(struct aic_dev *adev,
			      const u8 *payload, u16 len)
{
	aic_info(adev, "firmware ready event received\n");
	adev->fw.loaded = true;
	adev->fw_ready = true;

	/* Transition state if still in FW_LOADING */
	if (adev->state == AIC_STATE_FW_LOADING)
		aic_state_set(adev, AIC_STATE_FW_READY);

	return 0;
}

int aic_event_handle_scan_result(struct aic_dev *adev,
				 const u8 *payload, u16 len)
{
	/*
	 * In a production driver, parse the scan result event:
	 * struct aic_scan_result_event {
	 *     u8  bssid[6];
	 *     u8  ssid[32];
	 *     u8  ssid_len;
	 *     u16 channel;
	 *     s8  signal;
	 *     u16 capability;
	 *     u8  ie_data[];
	 * };
	 */
	aic_dbg(adev, "scan result: %u bytes\n", len);

	/* Deliver BSS info to cfg80211 */
	/* cfg80211_inform_bss_data() would be called here with parsed data */

	return 0;
}

int aic_event_handle_scan_complete(struct aic_dev *adev,
				   const u8 *payload, u16 len)
{
	struct aic_cfg80211_priv *priv;

	aic_dbg(adev, "scan complete\n");

	priv = wiphy_priv(adev->wiphy);

	if (adev->state == AIC_STATE_SCANNING)
		aic_state_set(adev, adev->bssid[0] ?
			      AIC_STATE_CONNECTED : AIC_STATE_NETDEV_REGISTERED);

	/* Cancel scan timeout */
	cancel_delayed_work_sync(&priv->scan_timeout_work);
	priv->scan_aborted = false;

	/* Notify cfg80211 */
	if (priv->scan_req) {
		struct cfg80211_scan_info info = { .aborted = false };
		AIC_CFG80211_SCAN_DONE(adev->wiphy, priv->scan_req, false);
		priv->scan_req = NULL;
	}

	return 0;
}

int aic_event_handle_connect_result(struct aic_dev *adev,
				    const u8 *payload, u16 len)
{
	struct aic_cfg80211_priv *priv;
	u16 status = 0;

	aic_dbg(adev, "connect result: %u bytes\n", len);

	priv = wiphy_priv(adev->wiphy);

	/* Cancel connect timeout */
	cancel_delayed_work_sync(&priv->connect_timeout_work);

	/*
	 * In production, parse the connect result:
	 * struct aic_connect_result_event {
	 *     u8  bssid[6];
	 *     u16 status;
	 *     u16 aid;
	 *     u8  ie_data[];
	 * };
	 */

	if (status == 0) {
		aic_state_set(adev, AIC_STATE_CONNECTED);
		netif_carrier_on(adev->ndev);
		aic_stats_inc(&adev->stats.connect_success);

		/* Notify cfg80211 */
		aic_cfg80211_notify_connect(adev, 0, adev->bssid,
					    NULL, 0, NULL, 0);

		/* Start link watch */
		queue_delayed_work(adev->wq, &adev->link_watch_work,
				   msecs_to_jiffies(3000));
	} else {
		aic_warn(adev, "connect failed, status=%u\n", status);
		aic_stats_inc(&adev->stats.connect_failures);

		/* Notify cfg80211 of failure */
		aic_cfg80211_notify_connect(adev, status, adev->bssid,
					    NULL, 0, NULL, 0);

		aic_state_set(adev, AIC_STATE_NETDEV_REGISTERED);
	}

	return 0;
}

int aic_event_handle_disconnect(struct aic_dev *adev,
				const u8 *payload, u16 len)
{
	aic_info(adev, "disconnect event\n");

	aic_stats_inc(&adev->stats.disconnect_count);

	if (adev->state == AIC_STATE_CONNECTED) {
		netif_carrier_off(adev->ndev);
		aic_tx_flush_all(adev);

		/* Notify cfg80211 */
		aic_cfg80211_notify_disconnect(adev, 0);

		aic_state_set(adev, AIC_STATE_DISCONNECTING);
		aic_state_set(adev, AIC_STATE_NETDEV_REGISTERED);
	}

	return 0;
}

int aic_event_handle_heartbeat(struct aic_dev *adev,
			       const u8 *payload, u16 len)
{
	aic_stats_inc(&adev->stats.fw_heartbeat_rx);
	return 0;
}

int aic_event_handle_fw_error(struct aic_dev *adev,
			      const u8 *payload, u16 len)
{
	aic_err(adev, "firmware error event (len=%u)\n", len);
	aic_stats_inc(&adev->stats.fw_errors);

	/* Schedule recovery */
	aic_recovery_schedule(adev, AIC_RECOVERY_FW_SOFT_RESET,
			      AIC_RECOVERY_REASON_FW_ERROR);

	return 0;
}
