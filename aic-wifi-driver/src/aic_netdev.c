/*
 * AIC8800 USB WiFi Driver - net_device Operations
 *
 * Implements net_device_ops: open, stop, start_xmit,
 * tx_timeout, set_mac_address, and get_stats.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_dev.h"
#include "../include/aic_tx.h"
#include "../include/aic_rx.h"
#include "../include/aic_recovery.h"

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>

/* ================================================================== */
/* net_device_ops: open                                                 */
/* ================================================================== */

static int aic_netdev_open(struct net_device *ndev)
{
	struct aic_dev *adev = netdev_priv(ndev);

	aic_dbg(adev, "ndo_open\n");

	if (!aic_state_is_online(adev->state)) {
		aic_err(adev, "device not ready for open\n");
		return -ENODEV;
	}

	netif_start_queue(ndev);

	/* Submit RX URBs if not already running */
	if (atomic_read(&adev->usb.rx_urb_inflight) == 0)
		aic_usb_submit_rx_urbs(adev);

	return 0;
}

/* ================================================================== */
/* net_device_ops: stop                                                  */
/* ================================================================== */

static int aic_netdev_stop(struct net_device *ndev)
{
	struct aic_dev *adev = netdev_priv(ndev);

	aic_dbg(adev, "ndo_stop\n");

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);

	return 0;
}

/* ================================================================== */
/* net_device_ops: start_xmit                                            */
/* ================================================================== */

static netdev_tx_t aic_netdev_start_xmit(struct sk_buff *skb,
					 struct net_device *ndev)
{
	struct aic_dev *adev = netdev_priv(ndev);

	return aic_tx_queue_frame(adev, skb);
}

/* ================================================================== */
/* net_device_ops: tx_timeout                                             */
/* ================================================================== */

static void aic_netdev_tx_timeout(struct net_device *ndev,
				  unsigned int txqueue)
{
	struct aic_dev *adev = netdev_priv(ndev);

	aic_warn(adev, "TX timeout on queue %u\n", txqueue);
	aic_stats_inc(&adev->stats.tx_timeout);

	/* Schedule recovery */
	aic_recovery_schedule(adev, AIC_RECOVERY_RESTART_QUEUES,
			      AIC_RECOVERY_REASON_TX_TIMEOUT);
}

/* ================================================================== */
/* net_device_ops: set_mac_address                                        */
/* ================================================================== */

static int aic_netdev_set_mac(struct net_device *ndev, void *addr)
{
	struct aic_dev *adev = netdev_priv(ndev);
	struct sockaddr *sa = addr;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;

	aic_eth_hw_addr_set(ndev, sa->sa_data);

	aic_dbg(adev, "MAC set to %pM\n", sa->sa_data);

	return 0;
}

/* ================================================================== */
/* net_device_ops: get_stats / stats64                                   */
/* ================================================================== */

static struct net_device_stats *aic_netdev_get_stats(struct net_device *ndev)
{
	struct aic_dev *adev = netdev_priv(ndev);
	static struct net_device_stats stats;

	memset(&stats, 0, sizeof(stats));
	stats.tx_packets = atomic64_read(&adev->stats.tx_packets);
	stats.tx_bytes   = atomic64_read(&adev->stats.tx_bytes);
	stats.tx_errors  = atomic64_read(&adev->stats.tx_errors);
	stats.tx_dropped = atomic64_read(&adev->stats.tx_dropped);
	stats.rx_packets = atomic64_read(&adev->stats.rx_packets);
	stats.rx_bytes   = atomic64_read(&adev->stats.rx_bytes);
	stats.rx_errors  = atomic64_read(&adev->stats.rx_errors);
	stats.rx_dropped = atomic64_read(&adev->stats.rx_dropped);

	return &stats;
}

/* ================================================================== */
/* net_device_ops table                                                  */
/* ================================================================== */

static const struct net_device_ops aic_netdev_ops = {
	.ndo_open        = aic_netdev_open,
	.ndo_stop        = aic_netdev_stop,
	.ndo_start_xmit  = aic_netdev_start_xmit,
	.ndo_tx_timeout  = aic_netdev_tx_timeout,
	.ndo_set_mac_address = aic_netdev_set_mac,
	.ndo_get_stats   = aic_netdev_get_stats,
};

/* ================================================================== */
/* Network Device Setup                                                  */
/* ================================================================== */

int aic_netdev_setup(struct aic_dev *adev)
{
	struct wireless_dev *wdev;
	struct net_device *ndev;
	int ret;
	u8 mac[ETH_ALEN];

	/* Allocate wireless_dev */
	wdev = kzalloc(sizeof(*wdev), GFP_KERNEL);
	if (!wdev) {
		aic_err(adev, "failed to allocate wireless_dev\n");
		return -ENOMEM;
	}
	adev->wdev = wdev;

	/* Allocate net_device with private area for aic_dev pointer */
	ndev = alloc_netdev(sizeof(void *), "wlan%d", NET_NAME_ENUM,
			    ether_setup);
	if (!ndev) {
		aic_err(adev, "failed to allocate net_device\n");
		kfree(wdev);
		adev->wdev = NULL;
		return -ENOMEM;
	}

	adev->ndev = ndev;
	SET_NETDEV_DEV(ndev, adev->dev);

	/* Store aic_dev in netdev private data */
	*((struct aic_dev **)netdev_priv(ndev)) = adev;

	ndev->netdev_ops = &aic_netdev_ops;

	/* Set device features */
	ndev->features |= NETIF_F_HW_CSUM | NETIF_F_SG;
	ndev->hw_features = ndev->features;

	/* Set TX timeout */
	ndev->watchdog_timeo = msecs_to_jiffies(AIC_TX_TIMEOUT_MS);

	/* Set MAC address from wiphy */
	if (adev->wiphy)
		memcpy(mac, adev->wiphy->perm_addr, ETH_ALEN);
	else
		eth_random_addr(mac);

	aic_eth_hw_addr_set(ndev, mac);

	/* Register net device */
	ret = register_netdev(ndev);
	if (ret) {
		aic_err(adev, "register_netdev failed: %d\n", ret);
		free_netdev(ndev);
		adev->ndev = NULL;
		return ret;
	}

	aic_info(adev, "netdev registered: %s mac=%pM\n",
		 netdev_name(ndev), ndev->dev_addr);

	return 0;
}

void aic_netdev_teardown(struct aic_dev *adev)
{
	if (!adev->ndev)
		return;

	aic_info(adev, "unregistering netdev %s\n",
		 netdev_name(adev->ndev));

	unregister_netdev(adev->ndev);
	free_netdev(adev->ndev);
	adev->ndev = NULL;

	if (adev->wdev) {
		kfree(adev->wdev);
		adev->wdev = NULL;
	}
}
