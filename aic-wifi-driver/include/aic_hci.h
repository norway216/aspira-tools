/*
 * AIC8800 USB WiFi Driver - Host Controller Interface Protocol
 *
 * Defines the HCI protocol header format for communication between
 * the host driver and AIC8800 firmware over USB bulk endpoints.
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_HCI_H__
#define __AIC_HCI_H__

#include <linux/types.h>
#include <linux/skbuff.h>

/* ================================================================== */
/* HCI Frame Types                                                     */
/* ================================================================== */

#define AIC_HCI_TYPE_DATA        0x00
#define AIC_HCI_TYPE_COMMAND     0x01
#define AIC_HCI_TYPE_EVENT       0x02
#define AIC_HCI_TYPE_MANAGEMENT  0x03

#define AIC_HCI_FLAG_MORE_FRAG   0x01
#define AIC_HCI_FLAG_FIRST_FRAG  0x02
#define AIC_HCI_FLAG_LAST_FRAG   0x04

/* ================================================================== */
/* HCI Header Format (8 bytes, packed)                                 */
/* ================================================================== */

struct aic_hci_hdr {
	u8   type;           /* frame type: DATA/CMD/EVENT/MGMT */
	u8   flags;          /* fragmentation flags */
	u8   tid;            /* traffic identifier / AC */
	u8   rsvd;           /* reserved */
	__le16 payload_len;   /* payload length in bytes */
	__le16 seq;           /* sequence number */
} __packed;

#define AIC_HCI_HDR_LEN   sizeof(struct aic_hci_hdr)

/* ================================================================== */
/* HCI Command Header (after aic_hci_hdr when type == COMMAND)         */
/* ================================================================== */

struct aic_cmd_hdr {
	__le16 cmd_id;
	__le16 cmd_len;
	__le32 flags;
} __packed;

#define AIC_CMD_HDR_LEN   sizeof(struct aic_cmd_hdr)
#define AIC_CMD_MAX_PAYLOAD  2048

/* ================================================================== */
/* HCI Event Header (after aic_hci_hdr when type == EVENT)             */
/* ================================================================== */

struct aic_event_hdr {
	__le16 event_id;
	__le16 event_len;
} __packed;

#define AIC_EVENT_HDR_LEN  sizeof(struct aic_event_hdr)

/* ================================================================== */
/* Firmware Event IDs                                                  */
/* ================================================================== */

enum aic_event_id {
	AIC_EVENT_FW_READY            = 0x0001,
	AIC_EVENT_SCAN_RESULT         = 0x0010,
	AIC_EVENT_SCAN_COMPLETE       = 0x0011,
	AIC_EVENT_CONNECT_RESULT      = 0x0020,
	AIC_EVENT_DISCONNECT          = 0x0021,
	AIC_EVENT_AUTH_STATUS         = 0x0022,
	AIC_EVENT_ASSOC_STATUS        = 0x0023,
	AIC_EVENT_DEAUTH_IND          = 0x0024,
	AIC_EVENT_SIGNAL_CHANGE       = 0x0030,
	AIC_EVENT_BEACON_LOST         = 0x0031,
	AIC_EVENT_HEARTBEAT           = 0x00F0,
	AIC_EVENT_FW_ERROR            = 0x00FE,
	AIC_EVENT_FW_CRASH            = 0x00FF,
};

/* ================================================================== */
/* Command IDs                                                         */
/* ================================================================== */

enum aic_cmd_id {
	AIC_CMD_SCAN                  = 0x0100,
	AIC_CMD_SCAN_ABORT            = 0x0101,
	AIC_CMD_CONNECT               = 0x0200,
	AIC_CMD_DISCONNECT            = 0x0201,
	AIC_CMD_ADD_KEY               = 0x0300,
	AIC_CMD_DEL_KEY               = 0x0301,
	AIC_CMD_SET_DEFAULT_KEY       = 0x0302,
	AIC_CMD_SET_POWER_MGMT        = 0x0400,
	AIC_CMD_SET_CHANNEL           = 0x0500,
	AIC_CMD_GET_STATS             = 0x0600,
	AIC_CMD_HEARTBEAT             = 0x0F00,
	AIC_CMD_FW_RESET              = 0x0FF0,
};

/* ================================================================== */
/* HCI API                                                             */
/* ================================================================== */

struct aic_hci_hdr *aic_hci_parse_header(const u8 *data, size_t len);
int  aic_hci_build_header(struct sk_buff *skb, u8 type, u8 tid, u16 payload_len);
int  aic_hci_build_cmd(struct sk_buff *skb, u16 cmd_id,
		       const void *payload, u16 payload_len);
bool aic_hci_is_event_frame(const u8 *data, size_t len);
bool aic_hci_is_data_frame(const u8 *data, size_t len);
u16  aic_hci_get_event_id(const u8 *data, size_t len);
int  aic_hci_get_event_payload(const u8 *data, size_t len,
			       u8 *out, u16 *out_len);

/* HCI TX buffer allocation */
struct sk_buff *aic_hci_alloc_tx_skb(u16 payload_len);
struct sk_buff *aic_hci_alloc_cmd_skb(u16 cmd_id, u16 payload_len);

#endif /* __AIC_HCI_H__ */
