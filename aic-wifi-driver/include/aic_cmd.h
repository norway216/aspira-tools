/*
 * AIC8800 USB WiFi Driver - Command Queue Management
 *
 * Command submission, timeout tracking, and response matching.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_CMD_H__
#define __AIC_CMD_H__

#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include "aic_hci.h"

/* ================================================================== */
/* Command Queue Depth                                                 */
/* ================================================================== */

#define AIC_CMD_QUEUE_DEPTH     32
#define AIC_CMD_TIMEOUT_MS      3000
#define AIC_CMD_MAX_RETRIES     3

/* ================================================================== */
/* Command State                                                       */
/* ================================================================== */

enum aic_cmd_state {
	AIC_CMD_FREE    = 0,
	AIC_CMD_PENDING = 1,
	AIC_CMD_DONE    = 2,
	AIC_CMD_TIMEOUT = 3,
	AIC_CMD_ERROR   = 4,
};

/* ================================================================== */
/* Command Descriptor                                                  */
/* ================================================================== */

struct aic_cmd_desc {
	u16                  cmd_id;
	u16                  seq;
	enum aic_cmd_state   state;

	void                *payload;
	u16                  payload_len;

	void                *resp;
	u16                  resp_len;
	u16                  resp_expected_id;

	struct completion    done;
	int                  retries;
	unsigned long        submit_time;

	struct sk_buff      *skb;
};

/* ================================================================== */
/* Command Manager                                                     */
/* ================================================================== */

struct aic_cmd_mgr {
	struct aic_cmd_desc  queue[AIC_CMD_QUEUE_DEPTH];
	spinlock_t           lock;
	atomic_t             seq_counter;
	atomic_t             pending_count;

	u16                  head;
	u16                  tail;
};

/* ================================================================== */
/* Command API                                                         */
/* ================================================================== */

int  aic_cmd_mgr_init(struct aic_cmd_mgr *mgr);
void aic_cmd_mgr_deinit(struct aic_cmd_mgr *mgr);

int  aic_cmd_send(struct aic_dev *adev, u16 cmd_id,
		  const void *payload, u16 payload_len,
		  void *resp, u16 *resp_len, u16 resp_event_id,
		  unsigned long timeout_ms);

int  aic_cmd_send_async(struct aic_dev *adev, u16 cmd_id,
			const void *payload, u16 payload_len);

/* Called from event handler when a command response event arrives */
int  aic_cmd_complete_event(struct aic_dev *adev, u16 event_id,
			    const u8 *data, u16 len);

/* Flush all pending commands (called during disconnect/recovery) */
void aic_cmd_flush_all(struct aic_dev *adev);

#endif /* __AIC_CMD_H__ */
