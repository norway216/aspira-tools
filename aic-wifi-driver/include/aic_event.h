/*
 * AIC8800 USB WiFi Driver - Firmware Event Handling
 *
 * Event demux, work scheduling, and event-to-handler dispatch.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_EVENT_H__
#define __AIC_EVENT_H__

#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include "aic_hci.h"

/* ================================================================== */
/* Event Queue Depth                                                   */
/* ================================================================== */

#define AIC_EVENT_QUEUE_DEPTH   64
#define AIC_EVENT_MAX_PAYLOAD   2048

/* ================================================================== */
/* Event Entry                                                         */
/* ================================================================== */

struct aic_event_entry {
	u16    event_id;
	u16    payload_len;
	u8     payload[AIC_EVENT_MAX_PAYLOAD];
	u64    timestamp_ms;
};

/* ================================================================== */
/* Event Manager                                                       */
/* ================================================================== */

struct aic_event_mgr {
	struct aic_event_entry queue[AIC_EVENT_QUEUE_DEPTH];
	spinlock_t             lock;
	u16                    head;
	u16                    tail;
	atomic_t               count;
	atomic_t               dropped;

	/* Last N events for debugfs */
	struct aic_event_entry last_events[16];
	int                    last_event_idx;
};

/* ================================================================== */
/* Event Handler Typedef                                               */
/* ================================================================== */

typedef int (*aic_event_handler_t)(struct aic_dev *adev,
				   const u8 *payload, u16 len);

/* ================================================================== */
/* Event API                                                           */
/* ================================================================== */

int  aic_event_mgr_init(struct aic_event_mgr *mgr);
void aic_event_mgr_deinit(struct aic_event_mgr *mgr);

/* Enqueue an event from RX path */
int  aic_event_enqueue(struct aic_dev *adev, u16 event_id,
		       const u8 *payload, u16 len);

/* Dequeue one event (called from event_work) */
int  aic_event_dequeue(struct aic_dev *adev, struct aic_event_entry *out);

/* Schedule event processing */
void aic_event_schedule(struct aic_dev *adev);

/* Event work callback */
void aic_event_work(struct work_struct *work);

/* Event dispatch table */
int  aic_event_dispatch(struct aic_dev *adev,
			const struct aic_event_entry *entry);

/* Specific event handlers */
int  aic_event_handle_fw_ready(struct aic_dev *adev,
			       const u8 *payload, u16 len);
int  aic_event_handle_scan_result(struct aic_dev *adev,
				  const u8 *payload, u16 len);
int  aic_event_handle_scan_complete(struct aic_dev *adev,
				    const u8 *payload, u16 len);
int  aic_event_handle_connect_result(struct aic_dev *adev,
				     const u8 *payload, u16 len);
int  aic_event_handle_disconnect(struct aic_dev *adev,
				 const u8 *payload, u16 len);
int  aic_event_handle_heartbeat(struct aic_dev *adev,
				const u8 *payload, u16 len);
int  aic_event_handle_fw_error(struct aic_dev *adev,
			       const u8 *payload, u16 len);

#endif /* __AIC_EVENT_H__ */
