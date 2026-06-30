/*
 * AIC8800 USB WiFi Driver - Kernel Version Compatibility Layer
 *
 * Centralizes all kernel API differences between Linux 5.10 LTS and 6.6 LTS.
 * Business logic must never contain raw #if LINUX_VERSION_CODE blocks.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_COMPAT_H__
#define __AIC_COMPAT_H__

#include <linux/version.h>
#include <linux/usb.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

/* ------------------------------------------------------------------ */
/* cfg80211_scan_done signature changed in 5.15                        */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#define AIC_CFG80211_SCAN_DONE(wiphy, req, aborted) \
	cfg80211_scan_done(req, aborted)
#else
#define AIC_CFG80211_SCAN_DONE(wiphy, req, aborted)                    \
	do {                                                               \
		struct cfg80211_scan_info _info = { .aborted = (aborted) };    \
		cfg80211_scan_done(req, &_info);                               \
	} while (0)
#endif

/* ------------------------------------------------------------------ */
/* netif_rx / netif_receive_skb — netif_rx deprecated in 6.1+         */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define AIC_NETIF_RX(skb)    netif_receive_skb(skb)
#else
#define AIC_NETIF_RX(skb)    netif_rx(skb)
#endif

/* ------------------------------------------------------------------ */
/* eth_hw_addr_set / ether_addr_copy — introduced in 5.15              */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#define aic_eth_hw_addr_set(dev, addr)  memcpy((dev)->dev_addr, (addr), ETH_ALEN)
#else
#define aic_eth_hw_addr_set(dev, addr)  eth_hw_addr_set(dev, addr)
#endif

/* ------------------------------------------------------------------ */
/* netif_carrier_event — introduced in 5.16                            */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
#define AIC_NETIF_CARRIER_ON(ndev)    netif_carrier_on(ndev)
#define AIC_NETIF_CARRIER_OFF(ndev)   netif_carrier_off(ndev)
#else
#define AIC_NETIF_CARRIER_ON(ndev)    netif_carrier_on(ndev)
#define AIC_NETIF_CARRIER_OFF(ndev)   netif_carrier_off(ndev)
#endif

/* ------------------------------------------------------------------ */
/* dev_addr_mod — introduced in 5.17                                   */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
#include <linux/etherdevice.h>
#define aic_dev_addr_mod(dev, offset, addr, len)                       \
	do {                                                               \
		unsigned int _i;                                               \
		for (_i = 0; _i < (len); _i++)                                 \
			(dev)->dev_addr[(offset) + _i] = (addr)[_i];               \
	} while (0)
#else
#define aic_dev_addr_mod(dev, offset, addr, len) \
	dev_addr_mod(dev, offset, addr, len)
#endif

/* ------------------------------------------------------------------ */
/* cfg80211_connect_result — parameter count changed in 5.18           */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
static inline void
aic_cfg80211_connect_result(struct net_device *ndev, const u8 *bssid,
			     struct cfg80211_connect_resp_params *p, gfp_t gfp)
{
	cfg80211_connect_result(ndev, bssid,
				p->req_ie, p->req_ie_len,
				p->resp_ie, p->resp_ie_len,
				p->status, gfp);
}
#else
static inline void
aic_cfg80211_connect_result(struct net_device *ndev, const u8 *bssid,
			     struct cfg80211_connect_resp_params *p, gfp_t gfp)
{
	cfg80211_connect_result(ndev, bssid, p, gfp);
}
#endif

/* ------------------------------------------------------------------ */
/* cfg80211_roamed — signature changed in 5.18                         */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
static inline void
aic_cfg80211_roamed(struct net_device *ndev,
		    struct cfg80211_roam_info *info, gfp_t gfp)
{
	cfg80211_roamed(ndev, info, gfp);
}
#else
static inline void
aic_cfg80211_roamed(struct net_device *ndev,
		    struct cfg80211_roam_info *info, gfp_t gfp)
{
	cfg80211_roamed(ndev, info, gfp);
}
#endif

/* ------------------------------------------------------------------ */
/* wiphy lock/unlock — mac80211-style helpers; cfg80211 managed mode   */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
#define aic_wiphy_lock(wiphy)      wiphy_lock(wiphy)
#define aic_wiphy_unlock(wiphy)    wiphy_unlock(wiphy)
#else
#define aic_wiphy_lock(wiphy)      wiphy_lock(wiphy)
#define aic_wiphy_unlock(wiphy)    wiphy_unlock(wiphy)
#endif

/* ------------------------------------------------------------------ */
/* dev_err_probe — useful for deferred probe                           */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#define dev_err_probe(dev, err, fmt, ...)                              \
	({ int _e = (err); dev_err(dev, fmt, ##__VA_ARGS__); _e; })
#endif

/* ------------------------------------------------------------------ */
/* strscpy_pad — introduced in 5.10                                    */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 30)
static inline ssize_t
aic_strscpy_pad(char *dest, const char *src, size_t count)
{
	ssize_t written = strscpy(dest, src, count);
	if (written > 0 && written < count)
		memset(dest + written, 0, count - written);
	return written;
}
#define strscpy_pad  aic_strscpy_pad
#endif

/* ------------------------------------------------------------------ */
/* USB core API differences                                            */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
#define aic_usb_kill_anchored_urbs(a)  usb_kill_anchored_urbs(a)
#define aic_usb_scuttle_anchored_urbs(a) usb_kill_anchored_urbs(a)
#else
#define aic_usb_kill_anchored_urbs(a)  usb_kill_anchored_urbs(a)
#define aic_usb_scuttle_anchored_urbs(a) usb_scuttle_anchored_urbs(a)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
#define aic_usb_autosuspend_device(udev) usb_autopm_put_interface( \
	usb_ifnum_to_if(udev, 0))
#else
#define aic_usb_autosuspend_device(udev) usb_autosuspend_device(udev)
#endif

/* ------------------------------------------------------------------ */
/* timekeeping / jiffies helpers                                       */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#include <linux/ktime.h>
static inline u64 aic_ktime_get_ms(void)
{
	return ktime_get_ms();
}
#else
static inline u64 aic_ktime_get_ms(void)
{
	return jiffies_to_msecs(jiffies);
}
#endif

/* ------------------------------------------------------------------ */
/* Workqueue API                                                       */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define aic_alloc_workqueue(name, flags, max_active) \
	alloc_workqueue(name, flags, max_active)
#else
#define aic_alloc_workqueue(name, flags, max_active) \
	alloc_workqueue("%s", (flags), (max_active), (name))
#endif

/* ------------------------------------------------------------------ */
/* kfree_sensitive — introduced 5.12, was kzfree before               */
/* ------------------------------------------------------------------ */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
#define kfree_sensitive(ptr)  kzfree(ptr)
#endif

/* ------------------------------------------------------------------ */
/* debugfs_initialized — introduced 5.10                               */
/* ------------------------------------------------------------------ */
#ifndef debugfs_initialized
static inline bool aic_debugfs_ready(void)
{
	return true;
}
#else
static inline bool aic_debugfs_ready(void)
{
	return debugfs_initialized();
}
#endif

#endif /* __AIC_COMPAT_H__ */
