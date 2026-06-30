/*
 * AIC8800 USB WiFi Driver - RX Data Path
 *
 * RX URB pre-submission, bulk-in completion handling,
 * data/event demux, and protocol stack delivery.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_RX_H__
#define __AIC_RX_H__

#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>

/* ================================================================== */
/* RX Queue Structure                                                  */
/* ================================================================== */

struct aic_rxq {
	struct sk_buff_head  pending;
	spinlock_t           lock;

	struct napi_struct   napi;
	int                  budget;

	atomic_t             received;
	atomic_t             dropped;
	atomic_t             errors;

	/* RX aggregation */
	u16                  aggr_count;
	u16                  aggr_max;
};

#define AIC_RX_AGGR_MAX   8

/* ================================================================== */
/* RX Path API                                                         */
/* ================================================================== */

int  aic_rxq_init(struct aic_rxq *rxq);
void aic_rxq_deinit(struct aic_rxq *rxq);

/* URB completion handler */
void aic_rx_complete(struct urb *urb);

/* Process raw RX data (common for both URB and re-submit paths) */
int  aic_rx_process_data(struct aic_dev *adev, const u8 *data, size_t len);

/* NAPI poll callback (if using NAPI) */
int  aic_rx_napi_poll(struct napi_struct *napi, int budget);

/* Submit/re-submit RX URBs */
int  aic_rx_submit_urbs(struct aic_dev *adev);
void aic_rx_flush(struct aic_dev *adev);

/* Protocol delivery */
int  aic_rx_deliver_data(struct aic_dev *adev, struct sk_buff *skb);
int  aic_rx_deliver_event(struct aic_dev *adev, const u8 *data, size_t len);

#endif /* __AIC_RX_H__ */
