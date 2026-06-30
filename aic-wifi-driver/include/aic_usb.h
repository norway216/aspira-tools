/*
 * AIC8800 USB WiFi Driver - USB Layer Structures
 *
 * Defines USB endpoint configuration, URB management structures,
 * and the USB subsystem embedded in struct aic_dev.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_USB_H__
#define __AIC_USB_H__

#include <linux/usb.h>
#include <linux/skbuff.h>
#include "aic_compat.h"

/* ------------------------------------------------------------------ */
/* USB Endpoint Configuration                                          */
/* ------------------------------------------------------------------ */

#define AIC_USB_BULK_IN_EP       0x82
#define AIC_USB_BULK_OUT_EP      0x02
#define AIC_USB_INTR_IN_EP       0x84

/* USB vendor/device IDs — update with actual lsusb values */
#define AIC_USB_VID_AIC          0xa69c
#define AIC_USB_PID_AIC8800      0x5721
#define AIC_USB_VID_TP_LINK      0x2357
#define AIC_USB_PID_TP_LINK_AIC  0x014e

/* URB pool sizes (overridable via module param) */
#define AIC_RX_URB_DEFAULT       32
#define AIC_TX_URB_DEFAULT       32
#define AIC_RX_BUF_DEFAULT       16384
#define AIC_TX_BUF_DEFAULT       8192

/* ------------------------------------------------------------------ */
/* URB Context Structures                                              */
/* ------------------------------------------------------------------ */

struct aic_rx_ctx {
	struct aic_dev   *adev;
	struct urb       *urb;
	void             *buf;
	dma_addr_t        dma;
	int               index;
};

struct aic_tx_ctx {
	struct aic_dev   *adev;
	struct urb       *urb;
	struct sk_buff   *skb;
	int               index;
};

/* ------------------------------------------------------------------ */
/* USB Subsystem Structure                                             */
/* ------------------------------------------------------------------ */

struct aic_usb {
	/* Pipe identifiers */
	unsigned int      bulk_in_pipe;
	unsigned int      bulk_out_pipe;
	unsigned int      intr_in_pipe;
	unsigned int      ctrl_pipe;

	/* Max packet sizes */
	u16               bulk_in_maxp;
	u16               bulk_out_maxp;

	/* URB anchors — every submitted URB must be anchored */
	struct usb_anchor rx_anchor;
	struct usb_anchor tx_anchor;
	struct usb_anchor ctrl_anchor;

	/* Pre-allocated RX URB pool */
	struct aic_rx_ctx *rx_ctxs;
	int               rx_urb_num;
	int               rx_buf_size;
	atomic_t          rx_urb_inflight;

	/* Pre-allocated TX URB pool */
	struct aic_tx_ctx *tx_ctxs;
	int               tx_urb_num;
	int               tx_buf_size;
	atomic_t          tx_urb_inflight;

	/* USB online flag */
	bool              usb_online;
	spinlock_t        urb_lock;  /* protects URB pool operations */
};

/* ================================================================== */
/* USB Layer API                                                       */
/* ================================================================== */

int  aic_usb_parse_endpoints(struct aic_dev *adev);
int  aic_usb_alloc_urb_pools(struct aic_dev *adev);
void aic_usb_free_urb_pools(struct aic_dev *adev);

int  aic_usb_submit_rx_urbs(struct aic_dev *adev);
int  aic_usb_submit_rx_urb(struct aic_dev *adev, struct aic_rx_ctx *ctx);
int  aic_usb_submit_tx_urb(struct aic_dev *adev, struct aic_tx_ctx *ctx,
			   struct sk_buff *skb);

void aic_usb_kill_all_urbs(struct aic_dev *adev);

struct aic_tx_ctx *aic_usb_get_tx_ctx(struct aic_dev *adev);
void aic_usb_put_tx_ctx(struct aic_dev *adev, struct aic_tx_ctx *ctx);

/* USB core operations */
int  aic_usb_probe(struct usb_interface *intf, const struct usb_device_id *id);
void aic_usb_disconnect(struct usb_interface *intf);
int  aic_usb_suspend(struct usb_interface *intf, pm_message_t message);
int  aic_usb_resume(struct usb_interface *intf);
int  aic_usb_reset_resume(struct usb_interface *intf);

/* USB device ID table */
extern const struct usb_device_id aic_usb_id_table[];

#endif /* __AIC_USB_H__ */
