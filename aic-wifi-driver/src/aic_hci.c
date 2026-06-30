/*
 * AIC8800 USB WiFi Driver - HCI Protocol Implementation
 *
 * Builds and parses HCI headers for communication between host driver
 * and AIC8800 firmware over USB bulk endpoints.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/aic_hci.h"
#include "../include/aic_dev.h"

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/byteorder/generic.h>

/* ================================================================== */
/* HCI Header Parsing                                                  */
/* ================================================================== */

struct aic_hci_hdr *aic_hci_parse_header(const u8 *data, size_t len)
{
	struct aic_hci_hdr *hdr;

	if (len < AIC_HCI_HDR_LEN)
		return NULL;

	hdr = (struct aic_hci_hdr *)data;

	/* Validate payload length against actual buffer */
	if (le16_to_cpu(hdr->payload_len) > len - AIC_HCI_HDR_LEN)
		return NULL;

	return hdr;
}

/* ================================================================== */
/* HCI Header Construction                                             */
/* ================================================================== */

int aic_hci_build_header(struct sk_buff *skb, u8 type, u8 tid, u16 payload_len)
{
	struct aic_hci_hdr *hdr;

	if (skb_headroom(skb) < AIC_HCI_HDR_LEN) {
		if (pskb_expand_head(skb, AIC_HCI_HDR_LEN, 0, GFP_ATOMIC))
			return -ENOMEM;
	}

	hdr = skb_push(skb, AIC_HCI_HDR_LEN);
	memset(hdr, 0, AIC_HCI_HDR_LEN);

	hdr->type         = type;
	hdr->flags        = AIC_HCI_FLAG_FIRST_FRAG | AIC_HCI_FLAG_LAST_FRAG;
	hdr->tid           = tid;
	hdr->rsvd          = 0;
	hdr->payload_len   = cpu_to_le16(payload_len);
	hdr->seq           = 0; /* caller should set */

	return 0;
}

int aic_hci_build_cmd(struct sk_buff *skb, u16 cmd_id,
		      const void *payload, u16 payload_len)
{
	struct aic_cmd_hdr *cmd;
	int ret;

	ret = aic_hci_build_header(skb, AIC_HCI_TYPE_COMMAND, 0,
				   AIC_CMD_HDR_LEN + payload_len);
	if (ret)
		return ret;

	/* Append command header */
	if (skb_tailroom(skb) < AIC_CMD_HDR_LEN + payload_len) {
		if (pskb_expand_head(skb, 0,
		    AIC_CMD_HDR_LEN + payload_len - skb_tailroom(skb),
		    GFP_ATOMIC))
			return -ENOMEM;
	}

	cmd = skb_put(skb, AIC_CMD_HDR_LEN);
	cmd->cmd_id  = cpu_to_le16(cmd_id);
	cmd->cmd_len = cpu_to_le16(payload_len);
	cmd->flags   = 0;

	if (payload && payload_len) {
		u8 *p = skb_put(skb, payload_len);
		memcpy(p, payload, payload_len);
	}

	return 0;
}

/* ================================================================== */
/* Frame Type Detection                                                 */
/* ================================================================== */

bool aic_hci_is_event_frame(const u8 *data, size_t len)
{
	if (len < 1)
		return false;
	return data[0] == AIC_HCI_TYPE_EVENT;
}

bool aic_hci_is_data_frame(const u8 *data, size_t len)
{
	if (len < 1)
		return false;
	return data[0] == AIC_HCI_TYPE_DATA;
}

/* ================================================================== */
/* Event Header Access                                                  */
/* ================================================================== */

u16 aic_hci_get_event_id(const u8 *data, size_t len)
{
	struct aic_hci_hdr *hdr;
	struct aic_event_hdr *evt;

	if (len < AIC_HCI_HDR_LEN + AIC_EVENT_HDR_LEN)
		return 0;

	hdr = (struct aic_hci_hdr *)data;
	if (hdr->type != AIC_HCI_TYPE_EVENT)
		return 0;

	evt = (struct aic_event_hdr *)(data + AIC_HCI_HDR_LEN);
	return le16_to_cpu(evt->event_id);
}

int aic_hci_get_event_payload(const u8 *data, size_t len,
			      u8 *out, u16 *out_len)
{
	struct aic_hci_hdr *hdr;
	struct aic_event_hdr *evt;
	u16 payload_len;
	u16 event_len;

	if (len < AIC_HCI_HDR_LEN + AIC_EVENT_HDR_LEN)
		return -EINVAL;

	hdr = (struct aic_hci_hdr *)data;
	if (hdr->type != AIC_HCI_TYPE_EVENT)
		return -EINVAL;

	payload_len = le16_to_cpu(hdr->payload_len);

	evt = (struct aic_event_hdr *)(data + AIC_HCI_HDR_LEN);
	event_len = le16_to_cpu(evt->event_len);

	if (event_len > payload_len - AIC_EVENT_HDR_LEN)
		return -EINVAL;

	if (out && out_len) {
		*out_len = event_len;
		if (event_len > 0) {
			memcpy(out, data + AIC_HCI_HDR_LEN + AIC_EVENT_HDR_LEN,
			       event_len);
		}
	}

	return 0;
}

/* ================================================================== */
/* SKB Allocation for TX and Commands                                  */
/* ================================================================== */

struct sk_buff *aic_hci_alloc_tx_skb(u16 payload_len)
{
	struct sk_buff *skb;
	u16 total = AIC_HCI_HDR_LEN + payload_len;

	skb = alloc_skb(total + 64, GFP_ATOMIC); /* 64 bytes headroom */
	if (!skb)
		return NULL;

	skb_reserve(skb, 64);
	return skb;
}

struct sk_buff *aic_hci_alloc_cmd_skb(u16 cmd_id, u16 payload_len)
{
	struct sk_buff *skb;
	u16 total = AIC_HCI_HDR_LEN + AIC_CMD_HDR_LEN + payload_len;

	skb = alloc_skb(total + 64, GFP_KERNEL);
	if (!skb)
		return NULL;

	skb_reserve(skb, 64);

	if (aic_hci_build_cmd(skb, cmd_id, NULL, payload_len)) {
		kfree_skb(skb);
		return NULL;
	}

	return skb;
}
