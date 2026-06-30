/*
 * AIC8800 USB WiFi Driver - Core Device Structure and State Machine
 *
 * Central device structure and lifecycle state machine.
 * All subsystem structures are embedded here for single-allocation.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_DEV_H__
#define __AIC_DEV_H__

#include "aic_compat.h"
#include "aic_usb.h"
#include "aic_fw.h"
#include "aic_cmd.h"
#include "aic_event.h"
#include "aic_tx.h"
#include "aic_rx.h"
#include "aic_cfg80211.h"
#include "aic_pm.h"
#include "aic_recovery.h"
#include "aic_stats.h"

/* ================================================================== */
/* Device State Machine                                                */
/* ================================================================== */

enum aic_dev_state {
	AIC_STATE_UNINIT = 0,
	AIC_STATE_USB_PROBED,
	AIC_STATE_FW_LOADING,
	AIC_STATE_FW_READY,
	AIC_STATE_HW_READY,
	AIC_STATE_NETDEV_REGISTERED,
	AIC_STATE_SCANNING,
	AIC_STATE_CONNECTING,
	AIC_STATE_CONNECTED,
	AIC_STATE_DISCONNECTING,
	AIC_STATE_SUSPENDING,
	AIC_STATE_SUSPENDED,
	AIC_STATE_RECOVERING,
	AIC_STATE_REMOVING,
	AIC_STATE_DEAD,
};

static inline const char *aic_state_name(enum aic_dev_state s)
{
	switch (s) {
	case AIC_STATE_UNINIT:            return "UNINIT";
	case AIC_STATE_USB_PROBED:        return "USB_PROBED";
	case AIC_STATE_FW_LOADING:        return "FW_LOADING";
	case AIC_STATE_FW_READY:          return "FW_READY";
	case AIC_STATE_HW_READY:          return "HW_READY";
	case AIC_STATE_NETDEV_REGISTERED: return "NETDEV_REGISTERED";
	case AIC_STATE_SCANNING:          return "SCANNING";
	case AIC_STATE_CONNECTING:        return "CONNECTING";
	case AIC_STATE_CONNECTED:         return "CONNECTED";
	case AIC_STATE_DISCONNECTING:     return "DISCONNECTING";
	case AIC_STATE_SUSPENDING:        return "SUSPENDING";
	case AIC_STATE_SUSPENDED:         return "SUSPENDED";
	case AIC_STATE_RECOVERING:        return "RECOVERING";
	case AIC_STATE_REMOVING:          return "REMOVING";
	case AIC_STATE_DEAD:              return "DEAD";
	default:                          return "UNKNOWN";
	}
}

/* State predicates — prefer these over raw enum comparisons */
static inline bool aic_state_can_tx(enum aic_dev_state s)
{
	return s == AIC_STATE_CONNECTED ||
	       s == AIC_STATE_SCANNING  ||
	       s == AIC_STATE_CONNECTING;
}

static inline bool aic_state_can_scan(enum aic_dev_state s)
{
	return s == AIC_STATE_NETDEV_REGISTERED ||
	       s == AIC_STATE_CONNECTED  ||
	       s == AIC_STATE_SCANNING;
}

static inline bool aic_state_is_online(enum aic_dev_state s)
{
	return s == AIC_STATE_HW_READY    ||
	       s == AIC_STATE_NETDEV_REGISTERED ||
	       s == AIC_STATE_SCANNING    ||
	       s == AIC_STATE_CONNECTING  ||
	       s == AIC_STATE_CONNECTED;
}

static inline bool aic_state_can_recover(enum aic_dev_state s)
{
	return s != AIC_STATE_UNINIT    &&
	       s != AIC_STATE_REMOVING  &&
	       s != AIC_STATE_DEAD;
}

/* ================================================================== */
/* Module Parameters                                                    */
/* ================================================================== */

#define AIC_DEFAULT_RX_URB_NUM    32
#define AIC_DEFAULT_TX_URB_NUM    32
#define AIC_DEFAULT_RX_BUF_SIZE   16384
#define AIC_DEFAULT_TX_BUF_SIZE   8192
#define AIC_TX_HIGH_WATERMARK     512
#define AIC_TX_LOW_WATERMARK      128
#define AIC_TX_TIMEOUT_MS         5000
#define AIC_SCAN_TIMEOUT_MS       8000
#define AIC_CONNECT_TIMEOUT_MS    15000
#define AIC_HEARTBEAT_INTERVAL_MS 5000
#define AIC_HEARTBEAT_MAX_MISS    3
#define AIC_RECOVERY_MAX_PER_MIN  3
#define AIC_USB_RESET_MAX_PER_10MIN 2

/* ================================================================== */
/* Global Device Structure                                             */
/* ================================================================== */

struct aic_dev {
	/* USB layer */
	struct usb_device       *udev;
	struct usb_interface    *intf;
	struct device           *dev;

	/* Wireless core */
	struct wiphy            *wiphy;
	struct wireless_dev     *wdev;
	struct net_device       *ndev;

	/* State machine */
	enum aic_dev_state       state;
	spinlock_t               state_lock;    /* protects state field */
	struct mutex             op_mutex;      /* serializes big ops */

	/* Subsystems */
	struct aic_usb           usb;
	struct aic_fw            fw;
	struct aic_txq           txq;
	struct aic_rxq           rxq;
	struct aic_cmd_mgr       cmd;
	struct aic_event_mgr     event;
	struct aic_pm            pm;
	struct aic_recovery      recovery;
	struct aic_stats         stats;

	/* Workqueue and workers */
	struct workqueue_struct *wq;
	struct work_struct       event_work;
	struct work_struct       recovery_work;
	struct delayed_work      link_watch_work;
	struct delayed_work      health_check_work;

	/* Reference counting */
	atomic_t                 refcnt;
	atomic_t                 tx_pending;
	atomic_t                 rx_pending;
	atomic_t                 reset_pending;

	/* Removal / surprise flags */
	bool                     removing;
	bool                     fw_ready;
	bool                     surprise_removed;

	/* Connection context */
	u8                       bssid[ETH_ALEN];
	u8                       ssid[IEEE80211_MAX_SSID_LEN];
	u8                       ssid_len;
	u16                      aid;
	u32                      cipher_suite;

	/* debugfs root */
	struct dentry           *debugfs_root;

	/* Module parameter copies */
	int                      log_level;
	int                      rx_urb_num;
	int                      tx_urb_num;
	bool                     disable_autosuspend;
	bool                     power_save;
	bool                     low_latency;
	bool                     recovery_enable;
	bool                     firmware_verify;
};

/* ================================================================== */
/* State Machine API                                                   */
/* ================================================================== */

int  aic_state_transition(struct aic_dev *adev, enum aic_dev_state from,
			  enum aic_dev_state to, const char *reason);
void aic_state_set(struct aic_dev *adev, enum aic_dev_state new_state);

/* ================================================================== */
/* Module-global parameters (defined in aic_main.c)                    */
/* ================================================================== */

extern int aic_log_level;
extern int aic_rx_urb_num;
extern int aic_tx_urb_num;
extern bool aic_disable_autosuspend;
extern bool aic_power_save;
extern bool aic_low_latency;
extern bool aic_recovery_enable;
extern bool aic_firmware_verify;

/* ================================================================== */
/* Logging Macros                                                      */
/* ================================================================== */

#define AIC_LOG_ERR    0
#define AIC_LOG_WARN   1
#define AIC_LOG_INFO   2
#define AIC_LOG_DEBUG  3
#define AIC_LOG_TRACE  4

#define aic_log(adev, level, fmt, ...)                                 \
	do {                                                               \
		if ((level) <= (adev)->log_level) {                            \
			dev_printk(KERN_##level, (adev)->dev,                      \
				   "aic8800 %s: " fmt,                              \
				   (adev)->ndev ? netdev_name((adev)->ndev) : "no-dev", \
				   ##__VA_ARGS__);                                   \
		}                                                              \
	} while (0)

#define aic_err(adev, fmt, ...)    aic_log(adev, AIC_LOG_ERR,   "[ERR] " fmt, ##__VA_ARGS__)
#define aic_warn(adev, fmt, ...)   aic_log(adev, AIC_LOG_WARN,  "[WARN] " fmt, ##__VA_ARGS__)
#define aic_info(adev, fmt, ...)   aic_log(adev, AIC_LOG_INFO,  "[INFO] " fmt, ##__VA_ARGS__)
#define aic_dbg(adev, fmt, ...)    aic_log(adev, AIC_LOG_DEBUG, "[DBG] " fmt, ##__VA_ARGS__)

#define aic_log_raw(adev, tag, fmt, ...)                               \
	dev_info((adev)->dev, "aic8800 %s: [%s] " fmt,                    \
		 (adev)->ndev ? netdev_name((adev)->ndev) : "no-dev",        \
		 tag, ##__VA_ARGS__)

#endif /* __AIC_DEV_H__ */
