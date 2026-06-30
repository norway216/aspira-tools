/*
 * AIC8800 USB WiFi Driver - cfg80211 Integration
 *
 * FullMAC cfg80211_ops implementation: scan, connect, disconnect,
 * key management, power management, and station info.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_CFG80211_H__
#define __AIC_CFG80211_H__

#include <net/cfg80211.h>
#include "aic_compat.h"

/* ================================================================== */
/* Supported Bands and Channels                                        */
/* ================================================================== */

#define AIC_2GHZ_CHANNELS   14
#define AIC_5GHZ_CHANNELS   25

#define AIC_CIPHER_SUITES  4
#define AIC_MAX_SCAN_SSIDS  10

/* ================================================================== */
/* Band Capability Flags                                               */
/* ================================================================== */

#define AIC_2GHZ_BAND_CAPS  (IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454 | \
			     IEEE80211_VHT_CAP_SHORT_GI_80)

/* ================================================================== */
/* cfg80211 Private Data (embedded in wiphy private area)              */
/* ================================================================== */

struct aic_cfg80211_priv {
	struct aic_dev       *adev;
	struct ieee80211_channel channels_2ghz[AIC_2GHZ_CHANNELS];
	struct ieee80211_channel channels_5ghz[AIC_5GHZ_CHANNELS];
	struct ieee80211_supported_band band_2ghz;
	struct ieee80211_supported_band band_5ghz;

	/* Scan state */
	struct cfg80211_scan_request *scan_req;
	bool                  scan_aborted;
	struct delayed_work   scan_timeout_work;

	/* Connect state */
	struct delayed_work   connect_timeout_work;

	/* Cipher suites */
	u32                   cipher_suites[AIC_CIPHER_SUITES];
	int                   n_cipher_suites;
};

/* ================================================================== */
/* cfg80211 API                                                        */
/* ================================================================== */

int  aic_cfg80211_register(struct aic_dev *adev);
void aic_cfg80211_unregister(struct aic_dev *adev);

/* cfg80211_ops entry points */
int  aic_cfg80211_scan(struct wiphy *wiphy,
		       struct cfg80211_scan_request *request);
int  aic_cfg80211_connect(struct wiphy *wiphy, struct net_device *ndev,
			  struct cfg80211_connect_params *params);
int  aic_cfg80211_disconnect(struct wiphy *wiphy, struct net_device *ndev,
			     u16 reason_code);
int  aic_cfg80211_add_key(struct wiphy *wiphy, struct net_device *ndev,
			  u8 key_index, bool pairwise, const u8 *mac_addr,
			  struct key_params *params);
int  aic_cfg80211_del_key(struct wiphy *wiphy, struct net_device *ndev,
			  u8 key_index, bool pairwise, const u8 *mac_addr);
int  aic_cfg80211_set_default_key(struct wiphy *wiphy,
				  struct net_device *ndev,
				  u8 key_index, bool unicast, bool multicast);
int  aic_cfg80211_get_station(struct wiphy *wiphy,
			      struct net_device *ndev,
			      const u8 *mac, struct station_info *sinfo);
int  aic_cfg80211_set_power_mgmt(struct wiphy *wiphy,
				 struct net_device *ndev,
				 bool enabled, int timeout);
int  aic_cfg80211_change_iface(struct wiphy *wiphy,
			       struct net_device *ndev,
			       enum nl80211_iftype type,
			       struct vif_params *params);
int  aic_cfg80211_set_tx_power(struct wiphy *wiphy,
			       struct wireless_dev *wdev,
			       enum nl80211_tx_power_setting type,
			       int mbm);

/* Scan timeout */
void aic_cfg80211_scan_timeout(struct work_struct *work);

/* Connect timeout */
void aic_cfg80211_connect_timeout(struct work_struct *work);

/* Notify cfg80211 of firmware events */
void aic_cfg80211_notify_connect(struct aic_dev *adev, u16 status,
				 const u8 *bssid, const u8 *req_ie,
				 size_t req_ie_len, const u8 *resp_ie,
				 size_t resp_ie_len);
void aic_cfg80211_notify_disconnect(struct aic_dev *adev, u16 reason);

/* Channel helpers */
int  aic_cfg80211_init_channels(struct aic_cfg80211_priv *priv);

/* External ops table */
extern struct cfg80211_ops aic_cfg80211_ops;

/* Network device setup/teardown (defined in aic_netdev.c) */
int  aic_netdev_setup(struct aic_dev *adev);
void aic_netdev_teardown(struct aic_dev *adev);

#endif /* __AIC_CFG80211_H__ */
