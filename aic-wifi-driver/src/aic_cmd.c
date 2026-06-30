/*
 * AIC8800 USB WiFi Driver - Command Queue Management
 *
 * Command submission with timeout tracking, response matching,
 * retry logic, and bulk flush during disconnect/recovery.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_cmd.h"
#include "../include/aic_usb.h"
#include "../include/aic_hci.h"

#include <linux/slab.h>
#include <linux/completion.h>

/* ================================================================== */
/* Command Manager Init / Deinit                                       */
/* ================================================================== */

int aic_cmd_mgr_init(struct aic_cmd_mgr *mgr)
{
	int i;

	spin_lock_init(&mgr->lock);
	atomic_set(&mgr->seq_counter, 0);
	atomic_set(&mgr->pending_count, 0);
	mgr->head = 0;
	mgr->tail = 0;

	for (i = 0; i < AIC_CMD_QUEUE_DEPTH; i++) {
		mgr->queue[i].state = AIC_CMD_FREE;
		init_completion(&mgr->queue[i].done);
	}

	return 0;
}

void aic_cmd_mgr_deinit(struct aic_cmd_mgr *mgr)
{
	aic_cmd_flush_all(NULL); /* flush with NULL dev for final cleanup */
	/* Note: full flush uses mgr directly */
	spin_lock(&mgr->lock);
	for (int i = 0; i < AIC_CMD_QUEUE_DEPTH; i++) {
		if (mgr->queue[i].state != AIC_CMD_FREE) {
			mgr->queue[i].state = AIC_CMD_ERROR;
			complete_all(&mgr->queue[i].done);
		}
	}
	spin_unlock(&mgr->lock);
}

/* ================================================================== */
/* Allocate Command Slot                                                */
/* ================================================================== */

static struct aic_cmd_desc *aic_cmd_alloc_slot(struct aic_cmd_mgr *mgr)
{
	struct aic_cmd_desc *cmd = NULL;
	unsigned long flags;
	u16 idx;

	spin_lock_irqsave(&mgr->lock, flags);

	idx = mgr->tail;
	if (mgr->queue[idx].state == AIC_CMD_FREE) {
		cmd = &mgr->queue[idx];
		cmd->state = AIC_CMD_PENDING;
		cmd->seq = (u16)atomic_inc_return(&mgr->seq_counter);
		cmd->retries = 0;
		cmd->submit_time = jiffies;

		mgr->tail = (mgr->tail + 1) % AIC_CMD_QUEUE_DEPTH;
		atomic_inc(&mgr->pending_count);
	}

	spin_unlock_irqrestore(&mgr->lock, flags);

	return cmd;
}

/* ================================================================== */
/* Free Command Slot                                                    */
/* ================================================================== */

static void aic_cmd_free_slot(struct aic_cmd_mgr *mgr,
			      struct aic_cmd_desc *cmd)
{
	unsigned long flags;

	spin_lock_irqsave(&mgr->lock, flags);

	if (cmd->state != AIC_CMD_FREE) {
		cmd->state = AIC_CMD_FREE;
		cmd->skb = NULL;
		cmd->payload = NULL;
		cmd->payload_len = 0;
		cmd->resp = NULL;
		cmd->resp_len = 0;
		kfree(cmd->payload);
		cmd->payload = NULL;
		atomic_dec(&mgr->pending_count);
	}

	spin_unlock_irqrestore(&mgr->lock, flags);
}

/* ================================================================== */
/* Synchronous Command Send                                             */
/* ================================================================== */

int aic_cmd_send(struct aic_dev *adev, u16 cmd_id,
		 const void *payload, u16 payload_len,
		 void *resp, u16 *resp_len, u16 resp_event_id,
		 unsigned long timeout_ms)
{
	struct aic_cmd_desc *cmd;
	struct aic_tx_ctx *tx_ctx;
	struct sk_buff *skb;
	int ret;

	if (!aic_state_is_online(adev->state)) {
		aic_err(adev, "device not online for cmd 0x%04x\n", cmd_id);
		return -ENODEV;
	}

	cmd = aic_cmd_alloc_slot(&adev->cmd);
	if (!cmd) {
		aic_err(adev, "command queue full for cmd 0x%04x\n", cmd_id);
		return -EBUSY;
	}

	cmd->cmd_id = cmd_id;
	cmd->resp = resp;
	cmd->resp_expected_id = resp_event_id;
	if (resp_len)
		cmd->resp_len = *resp_len;

	/* Build command SKB */
	skb = aic_hci_alloc_cmd_skb(cmd_id, payload_len);
	if (!skb) {
		ret = -ENOMEM;
		goto err_free_cmd;
	}

	if (payload && payload_len) {
		if (skb_tailroom(skb) < payload_len) {
			ret = -ENOMEM;
			kfree_skb(skb);
			goto err_free_cmd;
		}
		u8 *p = skb_put(skb, payload_len);
		memcpy(p, payload, payload_len);
	}

	/* Update sequence number in HCI header */
	{
		struct aic_hci_hdr *hdr = (struct aic_hci_hdr *)skb->data;
		hdr->seq = cpu_to_le16(cmd->seq);
	}

	cmd->skb = skb;
	reinit_completion(&cmd->done);

	/* Submit via TX path */
	tx_ctx = aic_usb_get_tx_ctx(adev);
	if (!tx_ctx) {
		ret = -EBUSY;
		goto err_free_skb;
	}

	ret = aic_usb_submit_tx_urb(adev, tx_ctx, skb);
	if (ret) {
		aic_usb_put_tx_ctx(adev, tx_ctx);
		goto err_free_skb;
	}

	/* Wait for completion */
	if (!timeout_ms)
		timeout_ms = AIC_CMD_TIMEOUT_MS;

	ret = wait_for_completion_timeout(&cmd->done,
					  msecs_to_jiffies(timeout_ms));
	if (ret == 0) {
		/* Timeout */
		aic_warn(adev, "cmd 0x%04x timeout after %lums\n",
			 cmd_id, timeout_ms);
		cmd->state = AIC_CMD_TIMEOUT;
		ret = -ETIMEDOUT;
	} else if (cmd->state == AIC_CMD_DONE) {
		ret = 0;
		if (resp && resp_len)
			*resp_len = cmd->resp_len;
	} else {
		ret = -EIO;
	}

	aic_cmd_free_slot(&adev->cmd, cmd);
	return ret;

err_free_skb:
	kfree_skb(skb);
err_free_cmd:
	aic_cmd_free_slot(&adev->cmd, cmd);
	return ret;
}

/* ================================================================== */
/* Asynchronous Command Send                                            */
/* ================================================================== */

int aic_cmd_send_async(struct aic_dev *adev, u16 cmd_id,
		       const void *payload, u16 payload_len)
{
	struct aic_cmd_desc *cmd;
	struct aic_tx_ctx *tx_ctx;
	struct sk_buff *skb;
	int ret;

	if (!aic_state_is_online(adev->state))
		return -ENODEV;

	cmd = aic_cmd_alloc_slot(&adev->cmd);
	if (!cmd)
		return -EBUSY;

	skb = aic_hci_alloc_cmd_skb(cmd_id, payload_len);
	if (!skb) {
		aic_cmd_free_slot(&adev->cmd, cmd);
		return -ENOMEM;
	}

	if (payload && payload_len) {
		u8 *p = skb_put(skb, payload_len);
		memcpy(p, payload, payload_len);
	}

	{
		struct aic_hci_hdr *hdr = (struct aic_hci_hdr *)skb->data;
		hdr->seq = cpu_to_le16(cmd->seq);
	}

	cmd->skb = skb;

	tx_ctx = aic_usb_get_tx_ctx(adev);
	if (!tx_ctx) {
		kfree_skb(skb);
		aic_cmd_free_slot(&adev->cmd, cmd);
		return -EBUSY;
	}

	ret = aic_usb_submit_tx_urb(adev, tx_ctx, skb);
	if (ret) {
		aic_usb_put_tx_ctx(adev, tx_ctx);
		kfree_skb(skb);
		aic_cmd_free_slot(&adev->cmd, cmd);
		return ret;
	}

	/* Fire-and-forget: slot will be freed when response arrives or
	 * during periodic cleanup */
	return 0;
}

/* ================================================================== */
/* Command Completion from Event                                        */
/* ================================================================== */

int aic_cmd_complete_event(struct aic_dev *adev, u16 event_id,
			   const u8 *data, u16 len)
{
	unsigned long flags;
	int matched = 0;

	spin_lock_irqsave(&adev->cmd.lock, flags);

	for (int i = 0; i < AIC_CMD_QUEUE_DEPTH; i++) {
		struct aic_cmd_desc *cmd = &adev->cmd.queue[i];

		if (cmd->state != AIC_CMD_PENDING)
			continue;

		/* Match by expected response event ID */
		if (cmd->resp_expected_id != event_id &&
		    cmd->resp_expected_id != 0)
			continue;

		/* Copy response */
		if (cmd->resp && cmd->resp_len > 0 && data && len > 0) {
			u16 copy_len = min_t(u16, len, cmd->resp_len);
			memcpy(cmd->resp, data, copy_len);
			cmd->resp_len = copy_len;
		}

		cmd->state = AIC_CMD_DONE;
		complete_all(&cmd->done);
		matched++;
	}

	spin_unlock_irqrestore(&adev->cmd.lock, flags);

	return matched;
}

/* ================================================================== */
/* Flush All Commands                                                    */
/* ================================================================== */

void aic_cmd_flush_all(struct aic_dev *adev)
{
	unsigned long flags;
	struct aic_cmd_mgr *mgr;

	if (!adev) {
		/* Called from deinit without device context */
		return;
	}

	mgr = &adev->cmd;

	spin_lock_irqsave(&mgr->lock, flags);

	for (int i = 0; i < AIC_CMD_QUEUE_DEPTH; i++) {
		struct aic_cmd_desc *cmd = &mgr->queue[i];

		if (cmd->state == AIC_CMD_PENDING) {
			cmd->state = AIC_CMD_ERROR;
			complete_all(&cmd->done);
		}
	}

	spin_unlock_irqrestore(&mgr->lock, flags);
}
