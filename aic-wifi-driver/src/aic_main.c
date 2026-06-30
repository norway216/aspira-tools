/*
 * AIC8800 USB WiFi Driver - Module Entry Point
 *
 * module_init / module_exit, module parameters, USB driver
 * registration, and global debugfs root creation.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_usb.h"
#include "../include/aic_debugfs.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>

/* ================================================================== */
/* Module Parameters (overridable via modprobe / kernel cmdline)       */
/* ================================================================== */

int aic_log_level = AIC_LOG_INFO;
module_param_named(log_level, aic_log_level, int, 0644);
MODULE_PARM_DESC(log_level,
		 "Log level: 0=ERR 1=WARN 2=INFO 3=DEBUG 4=TRACE (default=2)");

int aic_rx_urb_num = AIC_DEFAULT_RX_URB_NUM;
module_param_named(rx_urb_num, aic_rx_urb_num, int, 0644);
MODULE_PARM_DESC(rx_urb_num,
		 "Number of RX URBs (default=32, range=8-64)");

int aic_tx_urb_num = AIC_DEFAULT_TX_URB_NUM;
module_param_named(tx_urb_num, aic_tx_urb_num, int, 0644);
MODULE_PARM_DESC(tx_urb_num,
		 "Number of TX URBs (default=32, range=8-64)");

bool aic_disable_autosuspend = true;
module_param_named(disable_usb_autosuspend, aic_disable_autosuspend,
		   bool, 0644);
MODULE_PARM_DESC(disable_usb_autosuspend,
		 "Disable USB autosuspend (default=1, recommended for "
		 "industrial/medical)");

bool aic_power_save = false;
module_param_named(power_save, aic_power_save, bool, 0644);
MODULE_PARM_DESC(power_save,
		 "Enable power saving (default=0, recommended=0 for "
		 "industrial/medical)");

bool aic_low_latency = true;
module_param_named(low_latency, aic_low_latency, bool, 0644);
MODULE_PARM_DESC(low_latency,
		 "Low latency mode (default=1, recommended for "
		 "industrial/medical)");

bool aic_recovery_enable = true;
module_param_named(recovery_enable, aic_recovery_enable, bool, 0644);
MODULE_PARM_DESC(recovery_enable,
		 "Enable auto-recovery (default=1)");

bool aic_firmware_verify = true;
module_param_named(firmware_verify, aic_firmware_verify, bool, 0644);
MODULE_PARM_DESC(firmware_verify,
		 "Verify firmware manifest and SHA256 (default=1)");

/* ================================================================== */
/* USB Driver Registration                                              */
/* ================================================================== */

static struct usb_driver aic_usb_driver = {
	.name       = KBUILD_MODNAME,
	.id_table   = aic_usb_id_table,
	.probe      = aic_usb_probe,
	.disconnect = aic_usb_disconnect,
	.suspend    = aic_usb_suspend,
	.resume     = aic_usb_resume,
	.reset_resume = aic_usb_reset_resume,
	.supports_autosuspend = 1,
	.soft_unbind = 1,
	.disable_hub_initiated_lpm = 1,
};

/* ================================================================== */
/* State Machine Implementation                                        */
/* ================================================================== */

int aic_state_transition(struct aic_dev *adev, enum aic_dev_state from,
			 enum aic_dev_state to, const char *reason)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&adev->state_lock, flags);

	if (adev->state != from) {
		aic_warn(adev, "state transition rejected: expected %s, "
			 "actual %s (to %s, reason: %s)\n",
			 aic_state_name(from), aic_state_name(adev->state),
			 aic_state_name(to), reason ? reason : "");
		ret = -EINVAL;
		goto out;
	}

	trace_aic_state_change(adev->ndev ? netdev_name(adev->ndev) : "no-dev",
			       aic_state_name(from), aic_state_name(to),
			       reason ? reason : "");

	aic_dbg(adev, "state: %s -> %s (reason: %s)\n",
		aic_state_name(from), aic_state_name(to),
		reason ? reason : "");

	adev->state = to;

out:
	spin_unlock_irqrestore(&adev->state_lock, flags);
	return ret;
}

void aic_state_set(struct aic_dev *adev, enum aic_dev_state new_state)
{
	unsigned long flags;

	spin_lock_irqsave(&adev->state_lock, flags);

	trace_aic_state_change(adev->ndev ? netdev_name(adev->ndev) : "no-dev",
			       aic_state_name(adev->state),
			       aic_state_name(new_state), "set");

	aic_dbg(adev, "state: %s -> %s\n",
		aic_state_name(adev->state),
		aic_state_name(new_state));

	adev->state = new_state;

	spin_unlock_irqrestore(&adev->state_lock, flags);
}

/* ================================================================== */
/* Module Init / Exit                                                    */
/* ================================================================== */

static int __init aic_module_init(void)
{
	int ret;

	pr_info("aic8800: AIC8800 USB WiFi driver %s\n",
		AIC_FW_ABI_VERSION);
	pr_info("aic8800: loading with rx_urb=%d tx_urb=%d "
		"autosuspend=%s power_save=%s recovery=%s\n",
		aic_rx_urb_num, aic_tx_urb_num,
		aic_disable_autosuspend ? "off" : "on",
		aic_power_save ? "on" : "off",
		aic_recovery_enable ? "on" : "off");

	/* Create global debugfs root */
	ret = aic_debugfs_create_root();
	if (ret && ret != -ENODEV)
		pr_warn("aic8800: debugfs init failed: %d\n", ret);

	/* Register USB driver */
	ret = usb_register(&aic_usb_driver);
	if (ret) {
		pr_err("aic8800: usb_register failed: %d\n", ret);
		aic_debugfs_remove_root();
		return ret;
	}

	pr_info("aic8800: module loaded successfully\n");
	return 0;
}

static void __exit aic_module_exit(void)
{
	pr_info("aic8800: unloading module\n");

	usb_deregister(&aic_usb_driver);
	aic_debugfs_remove_root();

	pr_info("aic8800: module unloaded\n");
}

module_init(aic_module_init);
module_exit(aic_module_exit);

/* ================================================================== */
/* Module Metadata                                                      */
/* ================================================================== */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AIC WiFi Driver Project");
MODULE_DESCRIPTION("AIC8800 USB WiFi 6 Driver — High-Performance cfg80211 FullMAC");
MODULE_VERSION(AIC_FW_ABI_VERSION);
MODULE_FIRMWARE("aic8800/aic8800d80/fw_patch.bin");
MODULE_FIRMWARE("aic8800/aic8800d80/wifi_fw.bin");
MODULE_FIRMWARE("aic8800/aic8800d80/rf_config.bin");
MODULE_SOFTDEP("pre: cfg80211");
