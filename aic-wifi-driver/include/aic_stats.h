/*
 * AIC8800 USB WiFi Driver - Statistics Counters
 *
 * Atomic statistics for TX/RX, errors, recovery, and health monitoring.
 * Exposed via debugfs for diagnostics.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_STATS_H__
#define __AIC_STATS_H__

#include <linux/atomic.h>
#include <linux/types.h>

/* ================================================================== */
/* Statistics Structure                                                */
/* ================================================================== */

struct aic_stats {
	/* TX statistics */
	atomic64_t   tx_packets;
	atomic64_t   tx_bytes;
	atomic64_t   tx_errors;
	atomic64_t   tx_dropped;
	atomic64_t   tx_timeout;
	atomic64_t   tx_completed;

	/* RX statistics */
	atomic64_t   rx_packets;
	atomic64_t   rx_bytes;
	atomic64_t   rx_errors;
	atomic64_t   rx_dropped;
	atomic64_t   rx_crc_errors;

	/* URB statistics */
	atomic64_t   urb_rx_submitted;
	atomic64_t   urb_rx_completed;
	atomic64_t   urb_rx_errors;
	atomic64_t   urb_tx_submitted;
	atomic64_t   urb_tx_completed;
	atomic64_t   urb_tx_errors;

	/* Firmware statistics */
	atomic64_t   fw_heartbeat_rx;
	atomic64_t   fw_heartbeat_timeout;
	atomic64_t   fw_errors;
	atomic64_t   fw_crashes;

	/* Recovery statistics */
	atomic64_t   recovery_count;
	atomic64_t   recovery_failures;

	/* Connection statistics */
	atomic64_t   connect_success;
	atomic64_t   connect_failures;
	atomic64_t   disconnect_count;
	atomic64_t   scan_count;
	atomic64_t   scan_timeout;

	/* General */
	atomic64_t   usb_reset_count;
	atomic64_t   suspend_count;
	atomic64_t   resume_count;
	atomic64_t   watchdog_resets;

	/* Timestamps */
	u64          last_rx_jiffies;
	u64          last_tx_jiffies;
	u64          last_fw_event_jiffies;
};

/* ================================================================== */
/* Stats API                                                           */
/* ================================================================== */

void aic_stats_init(struct aic_stats *s);
void aic_stats_deinit(struct aic_stats *s);

/* Atomic increment helpers */
static inline void aic_stats_inc(atomic64_t *counter)
{
	atomic64_inc(counter);
}

static inline void aic_stats_add(atomic64_t *counter, s64 val)
{
	atomic64_add(val, counter);
}

/* Dump stats to a seq_file (for debugfs) */
int  aic_stats_dump(struct aic_stats *s, struct seq_file *m);

/* Reset all counters */
void aic_stats_reset(struct aic_stats *s);

#endif /* __AIC_STATS_H__ */
