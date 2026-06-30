/*
 * AIC8800 USB WiFi Driver - TX Data Path with QoS
 *
 * TX path from ndo_start_xmit through QoS classification,
 * WRR scheduling, URB submission, and flow control.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_tx.h"
#include "../include/aic_usb.h"
#include "../include/aic_hci.h"
#include "../include/aic_trace.h"

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>

/* ================================================================== */
/* TX Queue Init / Deinit                                               */
/* ================================================================== */

int aic_txq_init(struct aic_txq *txq)
{
	int i;

	spin_lock_init(&txq->lock);

	for (i = 0; i < AIC_TX_AC_NUM; i++)
		skb_queue_head_init(&txq->ac[i]);

	txq->high_watermark = AIC_TX_HIGH_WATERMARK;
	txq->low_watermark  = AIC_TX_LOW_WATERMARK;

	atomic_set(&txq->stopped, 0);
	atomic_set(&txq->dropped, 0);
	atomic_set(&txq->completed, 0);

	txq->wrr_next_ac = AIC_TX_AC_VO;
	txq->wrr_credit[AIC_TX_AC_VO] = AIC_TXQ_WRR_WEIGHT_VO;
	txq->wrr_credit[AIC_TX_AC_VI] = AIC_TXQ_WRR_WEIGHT_VI;
	txq->wrr_credit[AIC_TX_AC_BE] = AIC_TXQ_WRR_WEIGHT_BE;
	txq->wrr_credit[AIC_TX_AC_BK] = AIC_TXQ_WRR_WEIGHT_BK;

	INIT_WORK(&txq->tx_work, aic_tx_work);

	return 0;
}

void aic_txq_deinit(struct aic_txq *txq)
{
	int i;

	for (i = 0; i < AIC_TX_AC_NUM; i++)
		skb_queue_purge(&txq->ac[i]);
}

/* ================================================================== */
/* QoS Classification — 802.11e/WMM Mapping                            */
/* ================================================================== */

enum aic_tx_ac aic_tx_classify_skb(struct sk_buff *skb)
{
	u8 dscp;
	u8 priority;

	/* Use skb->priority if set by upper layers */
	priority = skb->priority;

	/* Derive from IP DSCP if available */
	if (skb->protocol == cpu_to_be16(ETH_P_IP)) {
		struct iphdr *iph = ip_hdr(skb);
		dscp = iph->tos >> 2;
		priority = dscp >> 3;
	} else if (skb->protocol == cpu_to_be16(ETH_P_IPV6)) {
		/* IPv6 priority from traffic class */
		dscp = (ipv6_get_dsfield(ipv6_hdr(skb)) >> 2);
		priority = dscp >> 3;
	}

	/* Map priority to WMM AC */
	switch (priority & 0x7) {
	case 0: case 3:
		return AIC_TX_AC_BE;
	case 1: case 2:
		return AIC_TX_AC_BK;
	case 4: case 5:
		return AIC_TX_AC_VI;
	case 6: case 7:
		return AIC_TX_AC_VO;
	default:
		return AIC_TX_AC_BE;
	}
}

/* ================================================================== */
/* WRR Scheduler — Picks Next AC to Dequeue                            */
/* ================================================================== */

static enum aic_tx_ac aic_tx_wrr_pick(struct aic_txq *txq)
{
	enum aic_tx_ac start = txq->wrr_next_ac;
	enum aic_tx_ac ac = start;

	/* Refill credits if all queues exhausted */
	static const u8 weights[AIC_TX_AC_NUM] = {
		AIC_TXQ_WRR_WEIGHT_BK,
		AIC_TXQ_WRR_WEIGHT_BE,
		AIC_TXQ_WRR_WEIGHT_VI,
		AIC_TXQ_WRR_WEIGHT_VO,
	};

	do {
		if (txq->wrr_credit[ac] > 0 &&
		    !skb_queue_empty(&txq->ac[ac])) {
			txq->wrr_credit[ac]--;
			txq->wrr_next_ac = (ac + 1) % AIC_TX_AC_NUM;
			return ac;
		}
		ac = (ac + 1) % AIC_TX_AC_NUM;
	} while (ac != start);

	/* All ACs empty or out of credit — refill and retry once */
	for (int i = 0; i < AIC_TX_AC_NUM; i++)
		txq->wrr_credit[i] = weights[i];

	ac = start;
	do {
		if (!skb_queue_empty(&txq->ac[ac])) {
			txq->wrr_credit[ac]--;
			txq->wrr_next_ac = (ac + 1) % AIC_TX_AC_NUM;
			return ac;
		}
		ac = (ac + 1) % AIC_TX_AC_NUM;
	} while (ac != start);

	return AIC_TX_AC_BE; /* best effort as fallback */
}

/* ================================================================== */
/* Queue Frame — Called from ndo_start_xmit                             */
/* ================================================================== */

netdev_tx_t aic_tx_queue_frame(struct aic_dev *adev, struct sk_buff *skb)
{
	enum aic_tx_ac ac;
	unsigned long flags;
	int qlen;

	if (!aic_state_can_tx(adev->state)) {
		aic_stats_inc(&adev->stats.tx_dropped);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (adev->removing || adev->surprise_removed) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	ac = aic_tx_classify_skb(skb);

	spin_lock_irqsave(&adev->txq.lock, flags);

	__skb_queue_tail(&adev->txq.ac[ac], skb);

	/* Flow control: stop queue if above high watermark */
	qlen = skb_queue_len(&adev->txq.ac[AIC_TX_AC_BE]) +
	       skb_queue_len(&adev->txq.ac[AIC_TX_AC_BK]) +
	       skb_queue_len(&adev->txq.ac[AIC_TX_AC_VI]) +
	       skb_queue_len(&adev->txq.ac[AIC_TX_AC_VO]);

	if (qlen >= adev->txq.high_watermark) {
		if (atomic_read(&adev->txq.stopped) == 0) {
			atomic_set(&adev->txq.stopped, 1);
			netif_stop_queue(adev->ndev);
			aic_dbg(adev, "TX queue stopped (qlen=%d)\n", qlen);
		}
	}

	spin_unlock_irqrestore(&adev->txq.lock, flags);

	trace_aic_tx_frame(netdev_name(adev->ndev), ac, skb->len, 0);

	/* Schedule TX work */
	queue_work(adev->wq, &adev->txq.tx_work);

	return NETDEV_TX_OK;
}

/* ================================================================== */
/* TX Work Callback                                                      */
/* ================================================================== */

void aic_tx_work(struct work_struct *work)
{
	struct aic_txq *txq = container_of(work, struct aic_txq, tx_work);
	struct aic_dev *adev = container_of(txq, struct aic_dev, txq);
	struct aic_tx_ctx *tx_ctx;
	struct sk_buff *skb;
	enum aic_tx_ac ac;
	int processed = 0;
	int ret;

	if (adev->removing || adev->surprise_removed)
		return;

	if (!aic_state_can_tx(adev->state))
		return;

	while (processed < 32) { /* batch limit per work invocation */
		spin_lock_irq(&adev->txq.lock);

		/* Select next queue via WRR */
		ac = aic_tx_wrr_pick(txq);
		skb = __skb_dequeue(&txq->ac[ac]);
		if (!skb) {
			spin_unlock_irq(&adev->txq.lock);
			break;
		}

		spin_unlock_irq(&adev->txq.lock);

		/* Get a free TX URB */
		tx_ctx = aic_usb_get_tx_ctx(adev);
		if (!tx_ctx) {
			/* No TX URB available — requeue and stop */
			spin_lock_irq(&adev->txq.lock);
			__skb_queue_head(&txq->ac[ac], skb);
			spin_unlock_irq(&adev->txq.lock);
			goto out;
		}

		/* Build HCI data header */
		ret = aic_hci_build_header(skb, AIC_HCI_TYPE_DATA,
					   ac, skb->len - AIC_HCI_HDR_LEN);
		if (ret) {
			aic_usb_put_tx_ctx(adev, tx_ctx);
			aic_stats_inc(&adev->stats.tx_dropped);
			dev_kfree_skb_any(skb);
			continue;
		}

		/* Submit TX URB */
		ret = aic_usb_submit_tx_urb(adev, tx_ctx, skb);
		if (ret) {
			aic_usb_put_tx_ctx(adev, tx_ctx);
			aic_stats_inc(&adev->stats.tx_errors);
			dev_kfree_skb_any(skb);
			continue;
		}

		atomic_inc(&adev->tx_pending);
		processed++;
	}

out:
	/* Wake queue if below low watermark */
	{
		int qlen;
		spin_lock_irq(&adev->txq.lock);
		qlen = skb_queue_len(&txq->ac[AIC_TX_AC_BE]) +
		       skb_queue_len(&txq->ac[AIC_TX_AC_BK]) +
		       skb_queue_len(&txq->ac[AIC_TX_AC_VI]) +
		       skb_queue_len(&txq->ac[AIC_TX_AC_VO]);
		spin_unlock_irq(&adev->txq.lock);

		if (qlen <= txq->low_watermark &&
		    atomic_read(&txq->stopped)) {
			atomic_set(&txq->stopped, 0);
			netif_wake_queue(adev->ndev);
		}
	}

	/* Re-schedule if more work remains */
	if (processed >= 32) {
		int remaining;
		spin_lock_irq(&adev->txq.lock);
		remaining = skb_queue_len(&txq->ac[AIC_TX_AC_BE]) +
			    skb_queue_len(&txq->ac[AIC_TX_AC_BK]) +
			    skb_queue_len(&txq->ac[AIC_TX_AC_VI]) +
			    skb_queue_len(&txq->ac[AIC_TX_AC_VO]);
		spin_unlock_irq(&adev->txq.lock);
		if (remaining > 0)
			queue_work(adev->wq, &txq->tx_work);
	}
}

/* ================================================================== */
/* TX Completion Callback                                                */
/* ================================================================== */

void aic_tx_complete_cb(struct urb *urb)
{
	struct aic_tx_ctx *ctx = urb->context;
	struct aic_dev *adev = ctx->adev;

	usb_unanchor_urb(urb);

	if (adev->removing || adev->surprise_removed) {
		aic_usb_put_tx_ctx(adev, ctx);
		return;
	}

	aic_stats_inc(&adev->stats.urb_tx_completed);

	if (urb->status != 0) {
		aic_stats_inc(&adev->stats.tx_errors);
		aic_stats_inc(&adev->stats.urb_tx_errors);
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN) {
			/* Normal during disconnect/suspend */
			aic_usb_put_tx_ctx(adev, ctx);
			return;
		}
		aic_warn(adev, "TX URB error status=%d\n", urb->status);
	} else {
		aic_stats_add(&adev->stats.tx_packets, 1);
		aic_stats_add(&adev->stats.tx_bytes, urb->actual_length);
		aic_stats_inc(&adev->stats.tx_completed);
	}

	/* Free the TX skb */
	if (ctx->skb && ctx->skb != (struct sk_buff *)0x1)
		dev_kfree_skb_any(ctx->skb);

	aic_usb_put_tx_ctx(adev, ctx);
	atomic_dec(&adev->tx_pending);
}

/* ================================================================== */
/* Flow Control                                                         */
/* ================================================================== */

void aic_tx_stop_queues(struct aic_dev *adev)
{
	if (adev->ndev) {
		netif_stop_queue(adev->ndev);
		atomic_set(&adev->txq.stopped, 1);
	}
}

void aic_tx_wake_queues(struct aic_dev *adev)
{
	if (adev->ndev) {
		netif_wake_queue(adev->ndev);
		atomic_set(&adev->txq.stopped, 0);
	}
}

void aic_tx_flush_all(struct aic_dev *adev)
{
	int i;

	for (i = 0; i < AIC_TX_AC_NUM; i++)
		skb_queue_purge(&adev->txq.ac[i]);

	atomic_set(&adev->tx_pending, 0);
}
