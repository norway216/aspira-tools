/*
 * AIC8800 USB WiFi Driver - Debug Filesystem Interface
 *
 * Exposes driver state, statistics, firmware info, USB status,
 * TX/RX queue status, recovery history, and controls via debugfs.
 *
 * Path: /sys/kernel/debug/aic8800/<ifname>/
 *
 * Nodes:
 *   state        — RO: current device state
 *   stats        — RO: statistics dump
 *   fw_version   — RO: firmware version info
 *   usb          — RO: USB endpoint and URB status
 *   txq          — RO: TX queue depths
 *   rxq          — RO: RX queue status
 *   recovery     — RO: recovery history and rate limit info
 *   last_events  — RO: last 16 firmware events
 *   log_level    — RW: log level (0-4)
 *   trigger_recovery — WO: manually trigger recovery
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_debugfs.h"
#include "../include/aic_stats.h"
#include "../include/aic_fw.h"
#include "../include/aic_recovery.h"
#include "../include/aic_tx.h"
#include "../include/aic_rx.h"

#include <linux/uaccess.h>

/* Global debugfs root */
static struct dentry *aic_debugfs_root;

/* ================================================================== */
/* Global Root Management                                               */
/* ================================================================== */

struct dentry *aic_debugfs_get_root(void)
{
	return aic_debugfs_root;
}

int aic_debugfs_create_root(void)
{
	if (!aic_debugfs_ready())
		return -ENODEV;

	aic_debugfs_root = debugfs_create_dir(AIC_DEBUGFS_ROOT, NULL);
	if (IS_ERR(aic_debugfs_root)) {
		aic_debugfs_root = NULL;
		return PTR_ERR(aic_debugfs_root);
	}

	return 0;
}

void aic_debugfs_remove_root(void)
{
	debugfs_remove_recursive(aic_debugfs_root);
	aic_debugfs_root = NULL;
}

/* ================================================================== */
/* Show Callbacks                                                        */
/* ================================================================== */

int aic_debugfs_state_show(struct seq_file *m, void *v)
{
	struct aic_dev *adev = m->private;

	seq_printf(m, "state:      %s\n", aic_state_name(adev->state));
	seq_printf(m, "fw_ready:   %d\n", adev->fw_ready);
	seq_printf(m, "usb_online: %d\n", adev->usb.usb_online);
	seq_printf(m, "removing:   %d\n", adev->removing);
	seq_printf(m, "surprise:   %d\n", adev->surprise_removed);
	seq_printf(m, "bssid:      %pM\n", adev->bssid);
	seq_printf(m, "ssid:       %.*s\n", adev->ssid_len, adev->ssid);

	if (adev->ndev) {
		seq_printf(m, "ifname:     %s\n", netdev_name(adev->ndev));
		seq_printf(m, "carrier:    %d\n",
			   netif_carrier_ok(adev->ndev));
		seq_printf(m, "queue:      %s\n",
			   netif_queue_stopped(adev->ndev) ?
			   "stopped" : "running");
	}

	return 0;
}

int aic_debugfs_stats_show(struct seq_file *m, void *v)
{
	struct aic_dev *adev = m->private;
	return aic_stats_dump(&adev->stats, m);
}

int aic_debugfs_fw_version_show(struct seq_file *m, void *v)
{
	struct aic_dev *adev = m->private;

	seq_printf(m, "chip:      %s\n",
		   aic_fw_chip_name(adev->fw.chip_model));
	seq_printf(m, "fw_version: %s\n", adev->fw.fw_version);
	seq_printf(m, "driver_abi: %s\n", adev->fw.driver_abi);
	seq_printf(m, "fw_crc:     0x%08x\n", adev->fw.fw_crc);
	seq_printf(m, "heartbeat:  %u\n", adev->fw.heartbeat_counter);
	seq_printf(m, "loaded:     %d\n", adev->fw.loaded);
	seq_printf(m, "verified:   %d\n", adev->fw.verified);
	seq_printf(m, "path:       %s\n", adev->fw.fw_path);

	return 0;
}

int aic_debugfs_usb_show(struct seq_file *m, void *v)
{
	struct aic_dev *adev = m->private;

	seq_printf(m, "bulk_in_pipe:  0x%x\n", adev->usb.bulk_in_pipe);
	seq_printf(m, "bulk_out_pipe: 0x%x\n", adev->usb.bulk_out_pipe);
	seq_printf(m, "bulk_in_maxp:  %u\n", adev->usb.bulk_in_maxp);
	seq_printf(m, "bulk_out_maxp: %u\n", adev->usb.bulk_out_maxp);
	seq_printf(m, "usb_online:    %d\n", adev->usb.usb_online);
	seq_printf(m, "rx_urb_num:    %d\n", adev->usb.rx_urb_num);
	seq_printf(m, "tx_urb_num:    %d\n", adev->usb.tx_urb_num);
	seq_printf(m, "rx_buf_size:   %d\n", adev->usb.rx_buf_size);
	seq_printf(m, "tx_buf_size:   %d\n", adev->usb.tx_buf_size);
	seq_printf(m, "rx_inflight:   %d\n",
		   atomic_read(&adev->usb.rx_urb_inflight));
	seq_printf(m, "tx_inflight:   %d\n",
		   atomic_read(&adev->usb.tx_urb_inflight));

	return 0;
}

int aic_debugfs_txq_show(struct seq_file *m, void *v)
{
	struct aic_dev *adev = m->private;
	int i;

	for (i = 0; i < AIC_TX_AC_NUM; i++) {
		const char *names[] = { "BK", "BE", "VI", "VO" };
		seq_printf(m, "ac[%s]: %u frames\n", names[i],
			   skb_queue_len(&adev->txq.ac[i]));
	}

	seq_printf(m, "stopped:   %d\n",
		   atomic_read(&adev->txq.stopped));
	seq_printf(m, "dropped:   %d\n",
		   atomic_read(&adev->txq.dropped));
	seq_printf(m, "completed: %d\n",
		   atomic_read(&adev->txq.completed));
	seq_printf(m, "pending:   %d\n",
		   atomic_read(&adev->tx_pending));
	seq_printf(m, "high_watermark: %d\n", adev->txq.high_watermark);
	seq_printf(m, "low_watermark:  %d\n", adev->txq.low_watermark);

	return 0;
}

int aic_debugfs_rxq_show(struct seq_file *m, void *v)
{
	struct aic_dev *adev = m->private;

	seq_printf(m, "received:  %d\n",
		   atomic_read(&adev->rxq.received));
	seq_printf(m, "dropped:   %d\n",
		   atomic_read(&adev->rxq.dropped));
	seq_printf(m, "errors:    %d\n",
		   atomic_read(&adev->rxq.errors));
	seq_printf(m, "pending:   %u\n",
		   skb_queue_len(&adev->rxq.pending));
	seq_printf(m, "rx_pending: %d\n",
		   atomic_read(&adev->rx_pending));

	return 0;
}

int aic_debugfs_recovery_show(struct seq_file *m, void *v)
{
	struct aic_dev *adev = m->private;

	seq_printf(m, "enabled:     %d\n", adev->recovery.enabled);
	seq_printf(m, "level:       %d (%s)\n",
		   adev->recovery.level,
		   aic_recovery_level_name(adev->recovery.level));
	seq_printf(m, "last_reason: %s\n",
		   aic_recovery_reason_name(adev->recovery.last_reason));
	seq_printf(m, "total_count: %d\n", adev->recovery.total_count);
	seq_printf(m, "fail_count:  %d\n", adev->recovery.fail_count);
	seq_printf(m, "count_1min:  %d\n",
		   adev->recovery.rl.count_1min);
	seq_printf(m, "usb_reset_10min: %d\n",
		   adev->recovery.rl.usb_reset_count_10min);

	return 0;
}

int aic_debugfs_last_events_show(struct seq_file *m, void *v)
{
	struct aic_dev *adev = m->private;
	int i;

	seq_printf(m, "Last 16 firmware events:\n");
	for (i = 0; i < 16; i++) {
		struct aic_event_entry *e =
			&adev->event.last_events[i];
		if (e->event_id == 0)
			continue;
		seq_printf(m, "  [%llu] event=0x%04x len=%u\n",
			   e->timestamp_ms, e->event_id,
			   e->payload_len);
	}

	return 0;
}

/* ================================================================== */
/* Write Callbacks                                                       */
/* ================================================================== */

static ssize_t aic_debugfs_log_level_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	struct aic_dev *adev = file->private_data;
	char kbuf[8];
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtoint(kbuf, 0, &val))
		return -EINVAL;

	if (val < AIC_LOG_ERR || val > AIC_LOG_TRACE)
		return -EINVAL;

	adev->log_level = val;
	aic_info(adev, "log level changed to %d\n", val);

	return count;
}

static const struct file_operations aic_debugfs_log_level_fops = {
	.write = aic_debugfs_log_level_write,
	.open  = simple_open,
	.llseek = no_llseek,
};

static ssize_t aic_debugfs_trigger_recovery_write(struct file *file,
						  const char __user *buf,
						  size_t count, loff_t *ppos)
{
	struct aic_dev *adev = file->private_data;
	char kbuf[8];
	int level;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (kstrtoint(kbuf, 0, &level))
		return -EINVAL;

	if (level < 1 || level > 6)
		return -EINVAL;

	aic_recovery_schedule(adev, level, AIC_RECOVERY_REASON_NONE);

	return count;
}

static const struct file_operations aic_debugfs_trigger_recovery_fops = {
	.write = aic_debugfs_trigger_recovery_write,
	.open  = simple_open,
	.llseek = no_llseek,
};

/* ================================================================== */
/* Device Node Creation                                                  */
/* ================================================================== */

int aic_debugfs_create_device_nodes(struct aic_dev *adev)
{
	struct dentry *dir;
	char name[32];

	if (!aic_debugfs_root)
		return -ENODEV;

	/* Create per-device directory */
	snprintf(name, sizeof(name), "%s",
		 adev->ndev ? netdev_name(adev->ndev) : "unknown");

	adev->debugfs_root = debugfs_create_dir(name, aic_debugfs_root);
	if (IS_ERR(adev->debugfs_root)) {
		adev->debugfs_root = NULL;
		return PTR_ERR(adev->debugfs_root);
	}

	dir = adev->debugfs_root;

	debugfs_create_devm_seqfile(adev->dev, "state", dir,
				    aic_debugfs_state_show);
	debugfs_create_devm_seqfile(adev->dev, "stats", dir,
				    aic_debugfs_stats_show);
	debugfs_create_devm_seqfile(adev->dev, "fw_version", dir,
				    aic_debugfs_fw_version_show);
	debugfs_create_devm_seqfile(adev->dev, "usb", dir,
				    aic_debugfs_usb_show);
	debugfs_create_devm_seqfile(adev->dev, "txq", dir,
				    aic_debugfs_txq_show);
	debugfs_create_devm_seqfile(adev->dev, "rxq", dir,
				    aic_debugfs_rxq_show);
	debugfs_create_devm_seqfile(adev->dev, "recovery", dir,
				    aic_debugfs_recovery_show);
	debugfs_create_devm_seqfile(adev->dev, "last_events", dir,
				    aic_debugfs_last_events_show);

	/* RW: log_level */
	debugfs_create_file("log_level", 0644, dir, adev,
			    &aic_debugfs_log_level_fops);

	/* WO: trigger_recovery */
	debugfs_create_file("trigger_recovery", 0200, dir, adev,
			    &aic_debugfs_trigger_recovery_fops);

	aic_dbg(adev, "debugfs nodes created at %s/%s\n",
		AIC_DEBUGFS_ROOT, name);

	return 0;
}

void aic_debugfs_remove_device_nodes(struct aic_dev *adev)
{
	debugfs_remove_recursive(adev->debugfs_root);
	adev->debugfs_root = NULL;
}
