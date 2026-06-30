/*
 * AIC8800 USB WiFi Driver - RX Data Path
 *
 * RX URB completion handler, data/event demultiplexing,
 * protocol stack delivery, and URB re-submission.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_rx.h"
#include "../include/aic_usb.h"
#include "../include/aic_hci.h"
#include "../include/aic_event.h"
#include "../include/aic_trace.h"

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/slab.h>

/* ================================================================== */
/* RX Queue Init / Deinit                                               */
/* ================================================================== */

int aic_rxq_init(struct aic_rxq *rxq)
{
	spin_lock_init(&rxq->lock);
	skb_queue_head_init(&rxq->pending);

	rxq->budget = 64;
	rxq->aggr_max = AIC_RX_AGGR_MAX;

	atomic_set(&rxq->received, 0);
	atomic_set(&rxq->dropped, 0);
	atomic_set(&rxq->errors, 0);

	return 0;
}

void aic_rxq_deinit(struct aic_rxq *rxq)
{
	skb_queue_purge(&rxq->pending);
}

/* ================================================================== */
/* RX URB Completion Handler                                            */
/* ================================================================== */

void aic_rx_complete(struct urb *urb)
{
	struct aic_rx_ctx *ctx = urb->context;
	struct aic_dev *adev = ctx->adev;

	usb_unanchor_urb(urb);

	atomic_dec(&adev->usb.rx_urb_inflight);

	if (adev->removing || adev->surprise_removed)
		return;

	aic_stats_inc(&adev->stats.urb_rx_completed);

	switch (urb->status) {
	case 0:
		/* Success — process received data */
		aic_rx_process_data(adev, urb->transfer_buffer,
				    urb->actual_length);
		break;

	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* Normal during disconnect/suspend */
		return;

	case -EPIPE:
		aic_err(adev, "RX endpoint stalled (EPIPE)\n");
		aic_recovery_schedule(adev, AIC_RECOVERY_CLEAR_HALT,
				      AIC_RECOVERY_REASON_USB_EP_HALT);
		return;

	case -EPROTO:
		aic_stats_inc(&adev->stats.urb_rx_errors);
		aic_warn(adev, "RX protocol error (EPROTO)\n");
		break;

	case -ETIMEDOUT:
		aic_stats_inc(&adev->stats.urb_rx_errors);
		aic_warn(adev, "RX URB timeout\n");
		break;

	default:
		aic_stats_inc(&adev->stats.urb_rx_errors);
		trace_aic_urb_error(netdev_name(adev->ndev), "RX",
				    urb->status, 1);
		aic_dbg(adev, "RX URB status=%d\n", urb->status);
		break;
	}

	/* Re-submit the RX URB */
	aic_usb_submit_rx_urb(adev, ctx);
}

/* ================================================================== */
/* RX Data Processing                                                    */
/* ================================================================== */

int aic_rx_process_data(struct aic_dev *adev, const u8 *data, size_t len)
{
	struct aic_hci_hdr *hdr;
	size_t offset = 0;

	if (len < AIC_HCI_HDR_LEN) {
		aic_stats_inc(&adev->stats.rx_errors);
		return -EINVAL;
	}

	while (offset + AIC_HCI_HDR_LEN <= len) {
		u16 payload_len;

		hdr = (struct aic_hci_hdr *)(data + offset);
		payload_len = le16_to_cpu(hdr->payload_len);

		if (offset + AIC_HCI_HDR_LEN + payload_len > len) {
			aic_stats_inc(&adev->stats.rx_errors);
			return -EINVAL;
		}

		/* Demux based on frame type */
		switch (hdr->type) {
		case AIC_HCI_TYPE_DATA: {
			/* Data frame — deliver to protocol stack */
			struct sk_buff *skb;

			skb = alloc_skb(payload_len + 64, GFP_ATOMIC);
			if (!skb) {
				aic_stats_inc(&adev->stats.rx_dropped);
				break;
			}

			skb_reserve(skb, 64);
			skb_put_data(skb, data + offset + AIC_HCI_HDR_LEN,
				     payload_len);
			skb->dev = adev->ndev;
			skb->protocol = eth_type_trans(skb, adev->ndev);
			skb->ip_summed = CHECKSUM_NONE;

			aic_rx_deliver_data(adev, skb);
			break;
		}

		case AIC_HCI_TYPE_EVENT: {
			/* Event frame — extract event ID and enqueue */
			u16 event_id;
			u16 event_len;
			struct aic_event_hdr *evt;
			const u8 *evt_data;

			if (payload_len < AIC_EVENT_HDR_LEN) {
				aic_stats_inc(&adev->stats.rx_errors);
				break;
			}

			evt = (struct aic_event_hdr *)(data + offset +
						       AIC_HCI_HDR_LEN);
			event_id  = le16_to_cpu(evt->event_id);
			event_len = le16_to_cpu(evt->event_len);

			if (event_len > payload_len - AIC_EVENT_HDR_LEN)
				event_len = payload_len - AIC_EVENT_HDR_LEN;

			evt_data = data + offset + AIC_HCI_HDR_LEN +
				   AIC_EVENT_HDR_LEN;

			aic_rx_deliver_event(adev, evt_data, event_len);

			/* Enqueue to event manager */
			aic_event_enqueue(adev, event_id, evt_data,
					  event_len);
			aic_event_schedule(adev);
			break;
		}

		case AIC_HCI_TYPE_COMMAND:
		case AIC_HCI_TYPE_MANAGEMENT:
		default:
			aic_dbg(adev, "RX unhandled type=%u len=%u\n",
				hdr->type, payload_len);
			break;
		}

		/* Move to next frame */
		offset += AIC_HCI_HDR_LEN + payload_len;
	}

	return 0;
}

/* ================================================================== */
/* Protocol Delivery                                                     */
/* ================================================================== */

int aic_rx_deliver_data(struct aic_dev *adev, struct sk_buff *skb)
{
	int ret;

	aic_stats_inc(&adev->stats.rx_packets);
	aic_stats_add(&adev->stats.rx_bytes, skb->len);

	trace_aic_rx_frame(netdev_name(adev->ndev), skb->len, false);

	ret = AIC_NETIF_RX(skb);
	if (ret == NET_RX_DROP) {
		aic_stats_inc(&adev->stats.rx_dropped);
	}

	return ret;
}

int aic_rx_deliver_event(struct aic_dev *adev, const u8 *data, size_t len)
{
	trace_aic_rx_frame(netdev_name(adev->ndev), len, true);

	/* Event delivery is handled by aic_event_enqueue */
	return 0;
}

/* ================================================================== */
/* RX URB Batch Submit                                                  */
/* ================================================================== */

int aic_rx_submit_urbs(struct aic_dev *adev)
{
	return aic_usb_submit_rx_urbs(adev);
}

void aic_rx_flush(struct aic_dev *adev)
{
	skb_queue_purge(&adev->rxq.pending);
}
