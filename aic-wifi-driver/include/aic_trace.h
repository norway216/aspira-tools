/*
 * AIC8800 USB WiFi Driver - Tracepoint Definitions
 *
 * Kernel tracepoints for performance analysis and debugging.
 * Use with: echo 1 > /sys/kernel/debug/tracing/events/aic8800/enable
 *
 * Copyright (C) 2026 AIC WiFi Driver Project
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __AIC_TRACE_H__
#define __AIC_TRACE_H__

#undef TRACE_SYSTEM
#define TRACE_SYSTEM aic8800

#if !defined(__AIC_TRACE_EVENTS__) || defined(TRACE_HEADER_MULTI_READ)
#define __AIC_TRACE_EVENTS__

#include <linux/tracepoint.h>

/* State transition */
TRACE_EVENT(aic_state_change,
	TP_PROTO(const char *ifname, const char *from, const char *to,
		 const char *reason),
	TP_ARGS(ifname, from, to, reason),
	TP_STRUCT__entry(
		__string(ifname, ifname)
		__string(from,   from)
		__string(to,     to)
		__string(reason, reason)
	),
	TP_fast_assign(
		__assign_str(ifname, ifname);
		__assign_str(from,   from);
		__assign_str(to,     to);
		__assign_str(reason, reason);
	),
	TP_printk("%s: %s -> %s reason=%s",
		  __get_str(ifname), __get_str(from),
		  __get_str(to), __get_str(reason))
);

/* TX frame */
TRACE_EVENT(aic_tx_frame,
	TP_PROTO(const char *ifname, u8 ac, u16 len, u16 seq),
	TP_ARGS(ifname, ac, len, seq),
	TP_STRUCT__entry(
		__string(ifname, ifname)
		__field(u8,  ac)
		__field(u16, len)
		__field(u16, seq)
	),
	TP_fast_assign(
		__assign_str(ifname, ifname);
		__entry->ac  = ac;
		__entry->len = len;
		__entry->seq = seq;
	),
	TP_printk("%s: TX ac=%u len=%u seq=%u",
		  __get_str(ifname), __entry->ac, __entry->len, __entry->seq)
);

/* RX frame */
TRACE_EVENT(aic_rx_frame,
	TP_PROTO(const char *ifname, u16 len, bool is_event),
	TP_ARGS(ifname, len, is_event),
	TP_STRUCT__entry(
		__string(ifname, ifname)
		__field(u16, len)
		__field(bool, is_event)
	),
	TP_fast_assign(
		__assign_str(ifname, ifname);
		__entry->len = len;
		__entry->is_event = is_event;
	),
	TP_printk("%s: RX len=%u is_event=%d",
		  __get_str(ifname), __entry->len, __entry->is_event)
);

/* Firmware event */
TRACE_EVENT(aic_fw_event,
	TP_PROTO(const char *ifname, u16 event_id, u16 len),
	TP_ARGS(ifname, event_id, len),
	TP_STRUCT__entry(
		__string(ifname, ifname)
		__field(u16, event_id)
		__field(u16, len)
	),
	TP_fast_assign(
		__assign_str(ifname, ifname);
		__entry->event_id = event_id;
		__entry->len = len;
	),
	TP_printk("%s: FW event 0x%04x len=%u",
		  __get_str(ifname), __entry->event_id, __entry->len)
);

/* Recovery */
TRACE_EVENT(aic_recovery,
	TP_PROTO(const char *ifname, int level, const char *reason),
	TP_ARGS(ifname, level, reason),
	TP_STRUCT__entry(
		__string(ifname, ifname)
		__field(int, level)
		__string(reason, reason)
	),
	TP_fast_assign(
		__assign_str(ifname, ifname);
		__entry->level = level;
		__assign_str(reason, reason);
	),
	TP_printk("%s: recovery level=%d reason=%s",
		  __get_str(ifname), __entry->level, __get_str(reason))
);

/* URB error */
TRACE_EVENT(aic_urb_error,
	TP_PROTO(const char *ifname, const char *dir, int status, int count),
	TP_ARGS(ifname, dir, status, count),
	TP_STRUCT__entry(
		__string(ifname, ifname)
		__string(dir, dir)
		__field(int, status)
		__field(int, count)
	),
	TP_fast_assign(
		__assign_str(ifname, ifname);
		__assign_str(dir, dir);
		__entry->status = status;
		__entry->count  = count;
	),
	TP_printk("%s: %s URB error status=%d count=%d",
		  __get_str(ifname), __get_str(dir),
		  __entry->status, __entry->count)
);

#endif /* __AIC_TRACE_EVENTS__ */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../include
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE aic_trace

#include <trace/define_trace.h>

#endif /* __AIC_TRACE_H__ */
