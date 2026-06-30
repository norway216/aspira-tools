/*
 * AIC8800 USB WiFi Driver - USB Layer Implementation
 *
 * usb_driver probe/disconnect/suspend/resume, endpoint parsing,
 * URB pool management, and USB lifecycle. All URBs are anchored
 * for safe bulk kill during disconnect and suspend.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_usb.h"
#include "../include/aic_hci.h"
#include "../include/aic_fw.h"
#include "../include/aic_rx.h"
#include "../include/aic_debugfs.h"
#include "../include/aic_trace.h"

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/dma-mapping.h>

/* ================================================================== */
/* USB Device ID Table                                                  */
/* ================================================================== */

const struct usb_device_id aic_usb_id_table[] = {
	{ USB_DEVICE(AIC_USB_VID_AIC, AIC_USB_PID_AIC8800) },
	{ USB_DEVICE(AIC_USB_VID_TP_LINK, AIC_USB_PID_TP_LINK_AIC) },
	{ }
};
MODULE_DEVICE_TABLE(usb, aic_usb_id_table);

/* ================================================================== */
/* Endpoint Parsing                                                     */
/* ================================================================== */

int aic_usb_parse_endpoints(struct aic_dev *adev)
{
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *ep;
	int i;

	iface_desc = adev->intf->cur_altsetting;

	adev->usb.bulk_in_pipe  = 0;
	adev->usb.bulk_out_pipe = 0;
	adev->usb.intr_in_pipe  = 0;
	adev->usb.ctrl_pipe     = usb_sndctrlpipe(adev->udev, 0);

	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		ep = &iface_desc->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(ep)) {
			adev->usb.bulk_in_pipe =
				usb_rcvbulkpipe(adev->udev,
						ep->bEndpointAddress &
						USB_ENDPOINT_NUMBER_MASK);
			adev->usb.bulk_in_maxp =
				usb_endpoint_maxp(ep);
			aic_dbg(adev, "found bulk IN ep 0x%02x maxp=%u\n",
				ep->bEndpointAddress, adev->usb.bulk_in_maxp);
		} else if (usb_endpoint_is_bulk_out(ep)) {
			adev->usb.bulk_out_pipe =
				usb_sndbulkpipe(adev->udev,
						ep->bEndpointAddress &
						USB_ENDPOINT_NUMBER_MASK);
			adev->usb.bulk_out_maxp =
				usb_endpoint_maxp(ep);
			aic_dbg(adev, "found bulk OUT ep 0x%02x maxp=%u\n",
				ep->bEndpointAddress, adev->usb.bulk_out_maxp);
		} else if (usb_endpoint_is_int_in(ep)) {
			adev->usb.intr_in_pipe =
				usb_rcvintpipe(adev->udev,
					       ep->bEndpointAddress &
					       USB_ENDPOINT_NUMBER_MASK);
			aic_dbg(adev, "found intr IN ep 0x%02x\n",
				ep->bEndpointAddress);
		}
	}

	if (!adev->usb.bulk_in_pipe || !adev->usb.bulk_out_pipe) {
		aic_err(adev, "required bulk endpoints not found\n");
		return -ENODEV;
	}

	return 0;
}

/* ================================================================== */
/* URB Pool Allocation                                                  */
/* ================================================================== */

int aic_usb_alloc_urb_pools(struct aic_dev *adev)
{
	struct aic_rx_ctx *rx;
	struct aic_tx_ctx *tx;
	int i, ret;

	adev->usb.rx_urb_num = adev->rx_urb_num;
	adev->usb.tx_urb_num = adev->tx_urb_num;
	adev->usb.rx_buf_size = AIC_RX_BUF_DEFAULT;
	adev->usb.tx_buf_size = AIC_TX_BUF_DEFAULT;

	/* Allocate RX context array */
	adev->usb.rx_ctxs = kcalloc(adev->usb.rx_urb_num,
				    sizeof(*adev->usb.rx_ctxs), GFP_KERNEL);
	if (!adev->usb.rx_ctxs)
		return -ENOMEM;

	/* Allocate TX context array */
	adev->usb.tx_ctxs = kcalloc(adev->usb.tx_urb_num,
				    sizeof(*adev->usb.tx_ctxs), GFP_KERNEL);
	if (!adev->usb.tx_ctxs) {
		ret = -ENOMEM;
		goto err_free_rx_ctxs;
	}

	/* Initialize URB anchors */
	init_usb_anchor(&adev->usb.rx_anchor);
	init_usb_anchor(&adev->usb.tx_anchor);
	init_usb_anchor(&adev->usb.ctrl_anchor);

	spin_lock_init(&adev->usb.urb_lock);

	/* Pre-allocate RX URBs and buffers */
	for (i = 0; i < adev->usb.rx_urb_num; i++) {
		rx = &adev->usb.rx_ctxs[i];
		rx->adev = adev;
		rx->index = i;

		rx->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!rx->urb) {
			ret = -ENOMEM;
			goto err_free_rx_urbs;
		}

		rx->buf = kmalloc(adev->usb.rx_buf_size, GFP_KERNEL);
		if (!rx->buf) {
			ret = -ENOMEM;
			goto err_free_rx_urbs;
		}
	}

	/* Pre-allocate TX URBs */
	for (i = 0; i < adev->usb.tx_urb_num; i++) {
		tx = &adev->usb.tx_ctxs[i];
		tx->adev = adev;
		tx->index = i;

		tx->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!tx->urb) {
			ret = -ENOMEM;
			goto err_free_tx_urbs;
		}
	}

	atomic_set(&adev->usb.rx_urb_inflight, 0);
	atomic_set(&adev->usb.tx_urb_inflight, 0);
	adev->usb.usb_online = true;

	aic_info(adev, "URB pools: RX=%d bufsize=%d TX=%d bufsize=%d\n",
		 adev->usb.rx_urb_num, adev->usb.rx_buf_size,
		 adev->usb.tx_urb_num, adev->usb.tx_buf_size);

	return 0;

err_free_tx_urbs:
	for (i = 0; i < adev->usb.tx_urb_num; i++) {
		tx = &adev->usb.tx_ctxs[i];
		if (tx->urb)
			usb_free_urb(tx->urb);
	}
	kfree(adev->usb.tx_ctxs);
	adev->usb.tx_ctxs = NULL;
err_free_rx_urbs:
	for (i = 0; i < adev->usb.rx_urb_num; i++) {
		rx = &adev->usb.rx_ctxs[i];
		if (rx->urb)
			usb_free_urb(rx->urb);
		kfree(rx->buf);
	}
err_free_rx_ctxs:
	kfree(adev->usb.rx_ctxs);
	adev->usb.rx_ctxs = NULL;
	return ret;
}

void aic_usb_free_urb_pools(struct aic_dev *adev)
{
	int i;

	adev->usb.usb_online = false;

	/* Kill all URBs first */
	aic_usb_kill_all_urbs(adev);

	/* Free RX contexts */
	if (adev->usb.rx_ctxs) {
		for (i = 0; i < adev->usb.rx_urb_num; i++) {
			struct aic_rx_ctx *rx = &adev->usb.rx_ctxs[i];
			if (rx->urb)
				usb_free_urb(rx->urb);
			kfree(rx->buf);
			rx->buf = NULL;
		}
		kfree(adev->usb.rx_ctxs);
		adev->usb.rx_ctxs = NULL;
	}

	/* Free TX contexts */
	if (adev->usb.tx_ctxs) {
		for (i = 0; i < adev->usb.tx_urb_num; i++) {
			struct aic_tx_ctx *tx = &adev->usb.tx_ctxs[i];
			if (tx->urb)
				usb_free_urb(tx->urb);
		}
		kfree(adev->usb.tx_ctxs);
		adev->usb.tx_ctxs = NULL;
	}
}

/* ================================================================== */
/* URB Submission                                                       */
/* ================================================================== */

int aic_usb_submit_rx_urb(struct aic_dev *adev, struct aic_rx_ctx *ctx)
{
	int ret;

	if (adev->removing || adev->surprise_removed)
		return -ENODEV;

	if (!adev->usb.usb_online)
		return -ENODEV;

	usb_fill_bulk_urb(ctx->urb, adev->udev, adev->usb.bulk_in_pipe,
			  ctx->buf, adev->usb.rx_buf_size,
			  aic_rx_complete, ctx);

	usb_anchor_urb(ctx->urb, &adev->usb.rx_anchor);

	ret = usb_submit_urb(ctx->urb, GFP_ATOMIC);
	if (ret) {
		usb_unanchor_urb(ctx->urb);
		aic_stats_inc(&adev->stats.urb_rx_errors);
		return ret;
	}

	atomic_inc(&adev->usb.rx_urb_inflight);
	aic_stats_inc(&adev->stats.urb_rx_submitted);

	return 0;
}

int aic_usb_submit_rx_urbs(struct aic_dev *adev)
{
	int i, ret;
	int submitted = 0;

	for (i = 0; i < adev->usb.rx_urb_num; i++) {
		struct aic_rx_ctx *ctx = &adev->usb.rx_ctxs[i];
		ret = aic_usb_submit_rx_urb(adev, ctx);
		if (ret) {
			aic_warn(adev, "failed to submit RX URB %d: %d\n",
				 i, ret);
			/* Continue trying to submit remaining URBs */
		} else {
			submitted++;
		}
	}

	aic_dbg(adev, "submitted %d/%d RX URBs\n",
		submitted, adev->usb.rx_urb_num);

	return (submitted > 0) ? 0 : -EIO;
}

int aic_usb_submit_tx_urb(struct aic_dev *adev, struct aic_tx_ctx *ctx,
			  struct sk_buff *skb)
{
	int ret;

	if (adev->removing || adev->surprise_removed)
		return -ENODEV;

	if (!adev->usb.usb_online)
		return -ENODEV;

	ctx->skb = skb;

	usb_fill_bulk_urb(ctx->urb, adev->udev, adev->usb.bulk_out_pipe,
			  skb->data, skb->len,
			  aic_tx_complete_cb, ctx);

	usb_anchor_urb(ctx->urb, &adev->usb.tx_anchor);

	ret = usb_submit_urb(ctx->urb, GFP_ATOMIC);
	if (ret) {
		usb_unanchor_urb(ctx->urb);
		ctx->skb = NULL;
		aic_stats_inc(&adev->stats.urb_tx_errors);
		return ret;
	}

	atomic_inc(&adev->usb.tx_urb_inflight);
	aic_stats_inc(&adev->stats.urb_tx_submitted);

	return 0;
}

/* ================================================================== */
/* URB Pool Management                                                  */
/* ================================================================== */

/* Forward declaration for TX completion - defined in aic_tx.c */
void aic_tx_complete_cb(struct urb *urb);

struct aic_tx_ctx *aic_usb_get_tx_ctx(struct aic_dev *adev)
{
	int i;

	spin_lock(&adev->usb.urb_lock);
	for (i = 0; i < adev->usb.tx_urb_num; i++) {
		struct aic_tx_ctx *ctx = &adev->usb.tx_ctxs[i];
		if (!ctx->skb) {
			/* Mark as in-use */
			ctx->skb = (struct sk_buff *)0x1; /* sentinel */
			spin_unlock(&adev->usb.urb_lock);
			return ctx;
		}
	}
	spin_unlock(&adev->usb.urb_lock);
	return NULL;
}

void aic_usb_put_tx_ctx(struct aic_dev *adev, struct aic_tx_ctx *ctx)
{
	spin_lock(&adev->usb.urb_lock);
	ctx->skb = NULL;
	spin_unlock(&adev->usb.urb_lock);
}

/* ================================================================== */
/* URB Kill All                                                         */
/* ================================================================== */

void aic_usb_kill_all_urbs(struct aic_dev *adev)
{
	aic_dbg(adev, "killing all URBs\n");

	usb_kill_anchored_urbs(&adev->usb.rx_anchor);
	usb_kill_anchored_urbs(&adev->usb.tx_anchor);
	usb_kill_anchored_urbs(&adev->usb.ctrl_anchor);

	atomic_set(&adev->usb.rx_urb_inflight, 0);
	atomic_set(&adev->usb.tx_urb_inflight, 0);

	aic_dbg(adev, "all URBs killed\n");
}

/* ================================================================== */
/* USB Core: Probe / Disconnect / Suspend / Resume                     */
/* ================================================================== */

int aic_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct aic_dev *adev;
	struct device *dev = &intf->dev;
	int ret;

	aic_log_raw(NULL, "USB", "probing VID=%04x PID=%04x\n",
		    le16_to_cpu(udev->descriptor.idVendor),
		    le16_to_cpu(udev->descriptor.idProduct));

	/* Allocate device structure */
	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->udev = usb_get_dev(udev);
	adev->intf = intf;
	adev->dev   = dev;

	/* Copy module parameters */
	adev->log_level          = aic_log_level;
	adev->rx_urb_num         = aic_rx_urb_num;
	adev->tx_urb_num         = aic_tx_urb_num;
	adev->disable_autosuspend = aic_disable_autosuspend;
	adev->power_save         = aic_power_save;
	adev->low_latency        = aic_low_latency;
	adev->recovery_enable    = aic_recovery_enable;
	adev->firmware_verify    = aic_firmware_verify;

	usb_set_intfdata(intf, adev);

	aic_state_set(adev, AIC_STATE_USB_PROBED);

	/* Initialize locks */
	spin_lock_init(&adev->state_lock);
	mutex_init(&adev->op_mutex);
	atomic_set(&adev->refcnt, 1);
	atomic_set(&adev->tx_pending, 0);
	atomic_set(&adev->rx_pending, 0);
	atomic_set(&adev->reset_pending, 0);

	/* Parse USB endpoints */
	ret = aic_usb_parse_endpoints(adev);
	if (ret) {
		aic_err(adev, "failed to parse endpoints: %d\n", ret);
		goto err_free;
	}

	/* Allocate URB pools */
	ret = aic_usb_alloc_urb_pools(adev);
	if (ret) {
		aic_err(adev, "failed to allocate URB pools: %d\n", ret);
		goto err_free;
	}

	/* Initialize subsystems */
	aic_txq_init(&adev->txq);
	aic_rxq_init(&adev->rxq);
	aic_cmd_mgr_init(&adev->cmd);
	aic_event_mgr_init(&adev->event);
	aic_stats_init(&adev->stats);
	aic_pm_init(&adev->pm);
	aic_recovery_init(&adev->recovery);

	/* Identify chip model from USB descriptors */
	ret = aic_fw_identify_chip(adev);
	if (ret) {
		aic_err(adev, "unable to identify chip: %d\n", ret);
		goto err_free_pools;
	}

	/* Create workqueue */
	adev->wq = aic_alloc_workqueue("aic8800_wq",
				       WQ_HIGHPRI | WQ_MEM_RECLAIM |
				       WQ_UNBOUND, 1);
	if (!adev->wq) {
		ret = -ENOMEM;
		goto err_free_pools;
	}

	INIT_WORK(&adev->event_work, aic_event_work);
	INIT_WORK(&adev->recovery_work, aic_recovery_work);
	INIT_DELAYED_WORK(&adev->health_check_work, aic_health_check_work);
	INIT_DELAYED_WORK(&adev->link_watch_work, aic_link_watch_work);

	/* Disable USB autosuspend for industrial/medical use */
	if (adev->disable_autosuspend) {
		usb_disable_autosuspend(adev->udev);
		aic_info(adev, "USB autosuspend disabled\n");
	}

	/* Load firmware */
	aic_state_set(adev, AIC_STATE_FW_LOADING);
	ret = aic_fw_load_all(adev);
	if (ret) {
		aic_err(adev, "firmware load failed: %d\n", ret);
		goto err_destroy_wq;
	}

	aic_state_set(adev, AIC_STATE_FW_READY);

	/* Set up network device (needed before cfg80211 register) */
	ret = aic_netdev_setup(adev);
	if (ret) {
		aic_err(adev, "netdev setup failed: %d\n", ret);
		goto err_fw_release;
	}

	/* Register with cfg80211 */
	ret = aic_cfg80211_register(adev);
	if (ret) {
		aic_err(adev, "cfg80211 register failed: %d\n", ret);
		goto err_netdev_teardown;
	}

	aic_state_set(adev, AIC_STATE_HW_READY);
	aic_state_set(adev, AIC_STATE_NETDEV_REGISTERED);

	/* Start RX URBs */
	ret = aic_usb_submit_rx_urbs(adev);
	if (ret) {
		aic_warn(adev, "initial RX URB submit failed: %d\n", ret);
		/* Non-fatal — recovery can restart them */
	}

	/* Create debugfs nodes */
	aic_debugfs_create_device_nodes(adev);

	/* Start health check */
	queue_delayed_work(adev->wq, &adev->health_check_work,
			   msecs_to_jiffies(AIC_HEARTBEAT_INTERVAL_MS));

	aic_info(adev, "probe complete: chip=%s fw=%s\n",
		 aic_fw_chip_name(adev->fw.chip_model),
		 adev->fw.fw_version);

	return 0;

err_netdev_teardown:
	aic_netdev_teardown(adev);
err_fw_release:
	aic_fw_release_all(adev);
err_destroy_wq:
	destroy_workqueue(adev->wq);
err_free_pools:
	aic_usb_free_urb_pools(adev);
err_free:
	usb_set_intfdata(intf, NULL);
	usb_put_dev(adev->udev);
	kfree(adev);
	return ret;
}

void aic_usb_disconnect(struct usb_interface *intf)
{
	struct aic_dev *adev = usb_get_intfdata(intf);

	aic_log_raw(adev, "USB", "disconnect\n");

	usb_set_intfdata(intf, NULL);

	if (!adev)
		return;

	mutex_lock(&adev->op_mutex);

	adev->removing = true;
	adev->surprise_removed = true;
	aic_state_set(adev, AIC_STATE_REMOVING);

	/* Stop network queues immediately */
	if (adev->ndev) {
		netif_carrier_off(adev->ndev);
		netif_stop_queue(adev->ndev);
	}

	/* Cancel all pending work */
	cancel_delayed_work_sync(&adev->health_check_work);
	cancel_delayed_work_sync(&adev->link_watch_work);
	cancel_work_sync(&adev->event_work);
	cancel_work_sync(&adev->recovery_work);
	cancel_work_sync(&adev->txq.tx_work);

	/* Kill all USB URBs */
	aic_usb_kill_all_urbs(adev);

	/* Flush TX queues */
	aic_tx_flush_all(adev);

	/* Unregister from cfg80211 */
	aic_cfg80211_unregister(adev);

	/* Remove debugfs */
	aic_debugfs_remove_device_nodes(adev);

	aic_state_set(adev, AIC_STATE_DEAD);

	mutex_unlock(&adev->op_mutex);

	/* Clean up resources */
	aic_fw_release_all(adev);
	aic_cmd_mgr_deinit(&adev->cmd);
	aic_event_mgr_deinit(&adev->event);
	aic_txq_deinit(&adev->txq);
	aic_rxq_deinit(&adev->rxq);

	if (adev->wq) {
		flush_workqueue(adev->wq);
		destroy_workqueue(adev->wq);
	}

	aic_usb_free_urb_pools(adev);

	mutex_destroy(&adev->op_mutex);

	usb_put_dev(adev->udev);
	kfree(adev);

	aic_log_raw(NULL, "USB", "disconnect complete\n");
}

int aic_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct aic_dev *adev = usb_get_intfdata(intf);

	if (!adev)
		return 0;

	aic_log_raw(adev, "PM", "suspend\n");
	return aic_pm_suspend(adev);
}

int aic_usb_resume(struct usb_interface *intf)
{
	struct aic_dev *adev = usb_get_intfdata(intf);

	if (!adev)
		return 0;

	aic_log_raw(adev, "PM", "resume\n");
	return aic_pm_resume(adev);
}

int aic_usb_reset_resume(struct usb_interface *intf)
{
	struct aic_dev *adev = usb_get_intfdata(intf);

	if (!adev)
		return 0;

	aic_log_raw(adev, "PM", "reset_resume — forcing recovery\n");
	aic_recovery_schedule(adev, AIC_RECOVERY_USB_RESET,
			      AIC_RECOVERY_REASON_USB_EP_HALT);
	return 0;
}
