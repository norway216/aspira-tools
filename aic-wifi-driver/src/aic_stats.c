/*
 * AIC8800 USB WiFi Driver - Statistics Counters
 *
 * Atomic statistics initialization, dump, and reset.
 * Exposed via debugfs for field diagnostics.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_stats.h"

#include <linux/seq_file.h>

/* ================================================================== */
/* Stats Init / Deinit                                                  */
/* ================================================================== */

void aic_stats_init(struct aic_stats *s)
{
	atomic64_set(&s->tx_packets, 0);
	atomic64_set(&s->tx_bytes, 0);
	atomic64_set(&s->tx_errors, 0);
	atomic64_set(&s->tx_dropped, 0);
	atomic64_set(&s->tx_timeout, 0);
	atomic64_set(&s->tx_completed, 0);

	atomic64_set(&s->rx_packets, 0);
	atomic64_set(&s->rx_bytes, 0);
	atomic64_set(&s->rx_errors, 0);
	atomic64_set(&s->rx_dropped, 0);
	atomic64_set(&s->rx_crc_errors, 0);

	atomic64_set(&s->urb_rx_submitted, 0);
	atomic64_set(&s->urb_rx_completed, 0);
	atomic64_set(&s->urb_rx_errors, 0);
	atomic64_set(&s->urb_tx_submitted, 0);
	atomic64_set(&s->urb_tx_completed, 0);
	atomic64_set(&s->urb_tx_errors, 0);

	atomic64_set(&s->fw_heartbeat_rx, 0);
	atomic64_set(&s->fw_heartbeat_timeout, 0);
	atomic64_set(&s->fw_errors, 0);
	atomic64_set(&s->fw_crashes, 0);

	atomic64_set(&s->recovery_count, 0);
	atomic64_set(&s->recovery_failures, 0);

	atomic64_set(&s->connect_success, 0);
	atomic64_set(&s->connect_failures, 0);
	atomic64_set(&s->disconnect_count, 0);
	atomic64_set(&s->scan_count, 0);
	atomic64_set(&s->scan_timeout, 0);

	atomic64_set(&s->usb_reset_count, 0);
	atomic64_set(&s->suspend_count, 0);
	atomic64_set(&s->resume_count, 0);
	atomic64_set(&s->watchdog_resets, 0);

	s->last_rx_jiffies = 0;
	s->last_tx_jiffies = 0;
	s->last_fw_event_jiffies = 0;
}

void aic_stats_deinit(struct aic_stats *s)
{
	/* No dynamic resources */
}

/* ================================================================== */
/* Stats Dump (for debugfs)                                             */
/* ================================================================== */

int aic_stats_dump(struct aic_stats *s, struct seq_file *m)
{
	seq_printf(m, "TX Statistics:\n");
	seq_printf(m, "  tx_packets:    %lld\n",
		   (long long)atomic64_read(&s->tx_packets));
	seq_printf(m, "  tx_bytes:      %lld\n",
		   (long long)atomic64_read(&s->tx_bytes));
	seq_printf(m, "  tx_errors:     %lld\n",
		   (long long)atomic64_read(&s->tx_errors));
	seq_printf(m, "  tx_dropped:    %lld\n",
		   (long long)atomic64_read(&s->tx_dropped));
	seq_printf(m, "  tx_timeout:    %lld\n",
		   (long long)atomic64_read(&s->tx_timeout));
	seq_printf(m, "  tx_completed:  %lld\n",
		   (long long)atomic64_read(&s->tx_completed));

	seq_printf(m, "\nRX Statistics:\n");
	seq_printf(m, "  rx_packets:    %lld\n",
		   (long long)atomic64_read(&s->rx_packets));
	seq_printf(m, "  rx_bytes:      %lld\n",
		   (long long)atomic64_read(&s->rx_bytes));
	seq_printf(m, "  rx_errors:     %lld\n",
		   (long long)atomic64_read(&s->rx_errors));
	seq_printf(m, "  rx_dropped:    %lld\n",
		   (long long)atomic64_read(&s->rx_dropped));
	seq_printf(m, "  rx_crc_errors: %lld\n",
		   (long long)atomic64_read(&s->rx_crc_errors));

	seq_printf(m, "\nURB Statistics:\n");
	seq_printf(m, "  urb_rx_submitted:  %lld\n",
		   (long long)atomic64_read(&s->urb_rx_submitted));
	seq_printf(m, "  urb_rx_completed:  %lld\n",
		   (long long)atomic64_read(&s->urb_rx_completed));
	seq_printf(m, "  urb_rx_errors:     %lld\n",
		   (long long)atomic64_read(&s->urb_rx_errors));
	seq_printf(m, "  urb_tx_submitted:  %lld\n",
		   (long long)atomic64_read(&s->urb_tx_submitted));
	seq_printf(m, "  urb_tx_completed:  %lld\n",
		   (long long)atomic64_read(&s->urb_tx_completed));
	seq_printf(m, "  urb_tx_errors:     %lld\n",
		   (long long)atomic64_read(&s->urb_tx_errors));

	seq_printf(m, "\nFirmware Statistics:\n");
	seq_printf(m, "  fw_heartbeat_rx:     %lld\n",
		   (long long)atomic64_read(&s->fw_heartbeat_rx));
	seq_printf(m, "  fw_heartbeat_timeout: %lld\n",
		   (long long)atomic64_read(&s->fw_heartbeat_timeout));
	seq_printf(m, "  fw_errors:           %lld\n",
		   (long long)atomic64_read(&s->fw_errors));
	seq_printf(m, "  fw_crashes:          %lld\n",
		   (long long)atomic64_read(&s->fw_crashes));

	seq_printf(m, "\nRecovery Statistics:\n");
	seq_printf(m, "  recovery_count:    %lld\n",
		   (long long)atomic64_read(&s->recovery_count));
	seq_printf(m, "  recovery_failures: %lld\n",
		   (long long)atomic64_read(&s->recovery_failures));

	seq_printf(m, "\nConnection Statistics:\n");
	seq_printf(m, "  connect_success:   %lld\n",
		   (long long)atomic64_read(&s->connect_success));
	seq_printf(m, "  connect_failures:  %lld\n",
		   (long long)atomic64_read(&s->connect_failures));
	seq_printf(m, "  disconnect_count:  %lld\n",
		   (long long)atomic64_read(&s->disconnect_count));
	seq_printf(m, "  scan_count:        %lld\n",
		   (long long)atomic64_read(&s->scan_count));
	seq_printf(m, "  scan_timeout:      %lld\n",
		   (long long)atomic64_read(&s->scan_timeout));

	seq_printf(m, "\nGeneral:\n");
	seq_printf(m, "  usb_reset_count:   %lld\n",
		   (long long)atomic64_read(&s->usb_reset_count));
	seq_printf(m, "  suspend_count:     %lld\n",
		   (long long)atomic64_read(&s->suspend_count));
	seq_printf(m, "  resume_count:      %lld\n",
		   (long long)atomic64_read(&s->resume_count));
	seq_printf(m, "  watchdog_resets:   %lld\n",
		   (long long)atomic64_read(&s->watchdog_resets));

	return 0;
}

/* ================================================================== */
/* Stats Reset                                                          */
/* ================================================================== */

void aic_stats_reset(struct aic_stats *s)
{
	aic_stats_init(s);
}
