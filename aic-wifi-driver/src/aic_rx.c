/*
 * AIC8800 USB WiFi Driver - RX Data Path (High-Performance)
 *
 * RX URB completion -> NAPI schedule -> NAPI poll with GRO delivery.
 * Uses build_skb for zero-copy where possible, and batched URB
 * re-submission to maximize USB throughput.
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
#include <linux/mm.h>

/* ================================================================== */
/* RX Queue Init / Deinit                                               */
/* ================================================================== */

int aic_rxq_init(struct aic_dev *adev)
{
	struct aic_rxq *rxq = &adev->rxq;

	spin_lock_init(&rxq->lock);
	skb_queue_head_init(&rxq->pending);

	rxq->budget = 64;
	rxq->aggr_max = AIC_RX_AGGR_MAX;

	atomic_set(&rxq->received, 0);
	atomic_set(&rxq->dropped, 0);
	atomic_set(&rxq->errors, 0);

	/* NAPI is initialized after netdev setup via aic_rxq_napi_init() */
	return 0;
}

/* Called after netdev is set up */
int aic_rxq_napi_init(struct aic_dev *adev)
{
	netif_napi_add(adev->ndev, &adev->rxq.napi,
		       aic_rx_napi_poll, 64);
	napi_enable(&adev->rxq.napi);
	return 0;
}

void aic_rxq_deinit(struct aic_dev *adev)
{
	napi_disable(&adev->rxq.napi);
	netif_napi_del(&adev->rxq.napi);
	skb_queue_purge(&adev->rxq.pending);
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
		/* Success — queue for NAPI processing */
		spin_lock(&adev->rxq.lock);
		__skb_queue_tail(&adev->rxq.pending,
				 (struct sk_buff *)(unsigned long)ctx->index);
		spin_unlock(&adev->rxq.lock);

		/* Schedule NAPI — serializes RX processing on one CPU */
		if (napi_schedule_prep(&adev->rxq.napi))
			__napi_schedule(&adev->rxq.napi);
		return; /* URB re-submission done in NAPI poll */

	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* Normal during disconnect/suspend — re-submit */
		goto resubmit;

	case -EPIPE:
		aic_err(adev, "RX endpoint stalled (EPIPE)\n");
		aic_stats_inc(&adev->stats.urb_rx_errors);
		aic_recovery_schedule(adev, AIC_RECOVERY_CLEAR_HALT,
				      AIC_RECOVERY_REASON_USB_EP_HALT);
		return;

	case -EPROTO:
	case -ETIMEDOUT:
		aic_stats_inc(&adev->stats.urb_rx_errors);
		trace_aic_urb_error(netdev_name(adev->ndev), "RX",
				    urb->status, 1);
		goto resubmit;

	default:
		aic_stats_inc(&adev->stats.urb_rx_errors);
		trace_aic_urb_error(netdev_name(adev->ndev), "RX",
				    urb->status, 1);
		goto resubmit;
	}

resubmit:
	/* Re-submit immediately on error or non-data path */
	aic_usb_submit_rx_urb(adev, ctx);
}

/* ================================================================== */
/* NAPI Poll — Main RX Processing Loop                                  */
/* ================================================================== */

int aic_rx_napi_poll(struct napi_struct *napi, int budget)
{
	struct aic_rxq *rxq = container_of(napi, struct aic_rxq, napi);
	struct aic_dev *adev = container_of(rxq, struct aic_dev, rxq);
	int work_done = 0;
	int resubmit_count = 0;

	while (work_done < budget) {
		struct aic_rx_ctx *ctx;
		struct sk_buff *marker;
		int idx;

		/* Dequeue pending RX context index */
		spin_lock(&rxq->lock);
		marker = __skb_dequeue(&rxq->pending);
		spin_unlock(&rxq->lock);

		if (!marker)
			break;

		idx = (int)(unsigned long)marker;
		if (idx < 0 || idx >= adev->usb.rx_urb_num) {
			aic_stats_inc(&rxq->errors);
			continue;
		}

		ctx = &adev->usb.rx_ctxs[idx];

		/* Process the RX data */
		aic_rx_process_data(adev, ctx->buf, ctx->urb->actual_length);
		work_done++;

		/* Re-submit the URB */
		aic_usb_submit_rx_urb(adev, ctx);
		resubmit_count++;
	}

	if (work_done < budget) {
		/* All work done — complete NAPI */
		napi_complete_done(napi, work_done);
	}

	return work_done;
}

/* ================================================================== */
/* RX Data Processing (with zero-copy build_skb)                       */
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
		const u8 *payload;

		hdr = (struct aic_hci_hdr *)(data + offset);
		payload_len = le16_to_cpu(hdr->payload_len);

		if (offset + AIC_HCI_HDR_LEN + payload_len > len) {
			aic_stats_inc(&adev->stats.rx_errors);
			return -EINVAL;
		}

		payload = data + offset + AIC_HCI_HDR_LEN;

		switch (hdr->type) {
		case AIC_HCI_TYPE_DATA: {
			struct sk_buff *skb;

			/* Skip fragmented frames */
			if (!(hdr->flags & AIC_HCI_FLAG_LAST_FRAG)) {
				aic_stats_inc(&adev->stats.rx_dropped);
				break;
			}

			/*
			 * Use build_skb (zero-copy) for larger frames
			 * to avoid alloc_skb + skb_put_data copy.
			 * For small frames (< 256 bytes), fall back to
			 * alloc_skb to avoid page waste.
			 */
			if (payload_len >= 256) {
				skb = build_skb((void *)payload, 0);
				if (!skb) {
					aic_stats_inc(&adev->stats.rx_dropped);
					break;
				}
				skb_reserve(skb, 2); /* align IP header */
				skb_put(skb, payload_len);
			} else {
				skb = alloc_skb(payload_len + 2, GFP_ATOMIC);
				if (!skb) {
					aic_stats_inc(&adev->stats.rx_dropped);
					break;
				}
				skb_reserve(skb, 2);
				skb_put_data(skb, payload, payload_len);
			}

			skb->dev = adev->ndev;
			skb->protocol = eth_type_trans(skb, adev->ndev);
			skb->ip_summed = CHECKSUM_UNNECESSARY;

			/*
			 * Deliver via napi_gro_receive for hardware
			 * GRO aggregation — significantly reduces
			 * per-packet overhead for TCP streams.
			 */
			napi_gro_receive(&adev->rxq.napi, skb);

			aic_stats_inc(&adev->stats.rx_packets);
			aic_stats_add(&adev->stats.rx_bytes, payload_len);
			trace_aic_rx_frame(netdev_name(adev->ndev),
					   payload_len, false);
			break;
		}

		case AIC_HCI_TYPE_EVENT: {
			u16 event_id;
			u16 event_len;
			struct aic_event_hdr *evt;

			if (payload_len < AIC_EVENT_HDR_LEN) {
				aic_stats_inc(&adev->stats.rx_errors);
				break;
			}

			evt = (struct aic_event_hdr *)payload;
			event_id  = le16_to_cpu(evt->event_id);
			event_len = le16_to_cpu(evt->event_len);

			if (event_len > payload_len - AIC_EVENT_HDR_LEN)
				event_len = (u16)(payload_len -
						  AIC_EVENT_HDR_LEN);

			trace_aic_rx_frame(netdev_name(adev->ndev),
					   event_len, true);

			aic_event_enqueue(adev, event_id,
					  payload + AIC_EVENT_HDR_LEN,
					  event_len);
			aic_event_schedule(adev);
			break;
		}

		case AIC_HCI_TYPE_COMMAND:
			/* Command responses are handled by event path */
			break;

		default:
			break;
		}

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

	/* Deliver via GRO for TCP aggregation */
	ret = napi_gro_receive(&adev->rxq.napi, skb);
	if (ret == GRO_DROP) {
		aic_stats_inc(&adev->stats.rx_dropped);
	}

	return (ret == GRO_DROP) ? NET_RX_DROP : NET_RX_SUCCESS;
}

int aic_rx_deliver_event(struct aic_dev *adev, const u8 *data, size_t len)
{
	trace_aic_rx_frame(netdev_name(adev->ndev), len, true);
	return 0;
}

/* ================================================================== */
/* RX URB Management                                                    */
/* ================================================================== */

int aic_rx_submit_urbs(struct aic_dev *adev)
{
	return aic_usb_submit_rx_urbs(adev);
}

int aic_rx_resubmit_batch(struct aic_dev *adev, int count)
{
	int i, ret;
	int submitted = 0;

	/*
	 * Batch re-submission: iterate all RX contexts and re-submit
	 * those not currently in-flight. This is called after NAPI
	 * poll completes to restore the RX URB pool.
	 */
	for (i = 0; i < adev->usb.rx_urb_num && submitted < count; i++) {
		struct aic_rx_ctx *ctx = &adev->usb.rx_ctxs[i];

		/* Check if this URB is already in-flight (has skb marker) */
		if (ctx->urb->status == -EINPROGRESS)
			continue;

		ret = aic_usb_submit_rx_urb(adev, ctx);
		if (ret == 0)
			submitted++;
	}

	return submitted;
}

void aic_rx_flush(struct aic_dev *adev)
{
	skb_queue_purge(&adev->rxq.pending);
}
