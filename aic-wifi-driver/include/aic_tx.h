/*
 * AIC8800 USB WiFi Driver - TX Data Path with QoS
 *
 * TX queue management with 4 AC queues (BK/BE/VI/VO),
 * WRR scheduling, flow control with high/low watermarks.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_TX_H__
#define __AIC_TX_H__

#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>

/* ================================================================== */
/* QoS Access Categories                                              */
/* ================================================================== */

enum aic_tx_ac {
	AIC_TX_AC_BK = 0,   /* Background */
	AIC_TX_AC_BE = 1,   /* Best Effort */
	AIC_TX_AC_VI = 2,   /* Video */
	AIC_TX_AC_VO = 3,   /* Voice */
	AIC_TX_AC_NUM = 4,
};

/* WRR weights: VO:VI:BE:BK = 4:3:2:1 */
#define AIC_TXQ_WRR_WEIGHT_BK  1
#define AIC_TXQ_WRR_WEIGHT_BE  2
#define AIC_TXQ_WRR_WEIGHT_VI  3
#define AIC_TXQ_WRR_WEIGHT_VO  4

/* ================================================================== */
/* TX Queue Structure                                                  */
/* ================================================================== */

struct aic_txq {
	struct sk_buff_head ac[AIC_TX_AC_NUM];  /* per-AC queues */
	spinlock_t          lock;

	int                 high_watermark;
	int                 low_watermark;

	atomic_t            stopped;
	atomic_t            dropped;
	atomic_t            completed;
	atomic_t            qlen;         /* cached combined queue length */

	struct work_struct  tx_work;

	/* WRR scheduler state */
	u8                  wrr_credit[AIC_TX_AC_NUM];
	u8                  wrr_next_ac;

	/* Aggregation */
	u16                 aggr_limit;
	u16                 aggr_count;
};

/* ================================================================== */
/* TX Path API                                                         */
/* ================================================================== */

int  aic_txq_init(struct aic_txq *txq);
void aic_txq_deinit(struct aic_txq *txq);

/* Core TX: called from ndo_start_xmit */
netdev_tx_t aic_tx_queue_frame(struct aic_dev *adev, struct sk_buff *skb);

/* TX work callback */
void aic_tx_work(struct work_struct *work);

/* Flow control */
void aic_tx_stop_queues(struct aic_dev *adev);
void aic_tx_wake_queues(struct aic_dev *adev);
void aic_tx_flush_all(struct aic_dev *adev);

/* QoS classification */
enum aic_tx_ac aic_tx_classify_skb(struct sk_buff *skb);

/* TX completion from URB callback */
void aic_tx_complete(struct aic_dev *adev, struct sk_buff *skb, int status);

#endif /* __AIC_TX_H__ */
