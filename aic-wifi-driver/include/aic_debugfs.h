/*
 * AIC8800 USB WiFi Driver - Debug Filesystem Interface
 *
 * Exposes driver state, statistics, firmware info, USB status,
 * and recovery controls via debugfs for field diagnostics.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_DEBUGFS_H__
#define __AIC_DEBUGFS_H__

#include <linux/debugfs.h>
#include <linux/seq_file.h>

/* ================================================================== */
/* debugfs Path                                                        */
/* ================================================================== */

#define AIC_DEBUGFS_ROOT    "aic8800"

/* ================================================================== */
/* Debug Filesystem API                                                */
/* ================================================================== */

int  aic_debugfs_init(struct aic_dev *adev);
void aic_debugfs_deinit(struct aic_dev *adev);

/* Per-device debugfs directory (under /sys/kernel/debug/aic8800/<ifname>/) */
int  aic_debugfs_create_device_nodes(struct aic_dev *adev);
void aic_debugfs_remove_device_nodes(struct aic_dev *adev);

/* Global debugfs root for module-level info */
struct dentry *aic_debugfs_get_root(void);
int  aic_debugfs_create_root(void);
void aic_debugfs_remove_root(void);

/* seq_file show callbacks */
int  aic_debugfs_state_show(struct seq_file *m, void *v);
int  aic_debugfs_stats_show(struct seq_file *m, void *v);
int  aic_debugfs_fw_version_show(struct seq_file *m, void *v);
int  aic_debugfs_usb_show(struct seq_file *m, void *v);
int  aic_debugfs_txq_show(struct seq_file *m, void *v);
int  aic_debugfs_rxq_show(struct seq_file *m, void *v);
int  aic_debugfs_recovery_show(struct seq_file *m, void *v);
int  aic_debugfs_last_events_show(struct seq_file *m, void *v);

/* Note: log_level and trigger_recovery write callbacks are
 * static in aic_debugfs.c with file_operations structs.
 * They are registered via debugfs_create_file() in
 * aic_debugfs_create_device_nodes().
 */

#endif /* __AIC_DEBUGFS_H__ */
