/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "osdp_common.h"
#include "osdp_file.h"
#include "osdp_diag.h"
#include "osdp_metrics.h"
#include "osdp_trs.h"

#ifndef OPT_OSDP_STATIC
#include <stdlib.h>
#endif

#define CMD_POLL_DATA_LEN              0
#define CMD_LSTAT_DATA_LEN             0
#define CMD_ISTAT_DATA_LEN             0
#define CMD_OSTAT_DATA_LEN             0
#define CMD_RSTAT_DATA_LEN             0
#define CMD_ID_DATA_LEN                1
#define CMD_CAP_DATA_LEN               1
#define CMD_OUT_DATA_LEN               4
#define CMD_LED_DATA_LEN               14
#define CMD_BUZ_DATA_LEN               5
#define CMD_TEXT_DATA_LEN              6   /* variable length command */
#define CMD_COMSET_DATA_LEN            5
#define CMD_KEYSET_DATA_LEN            18
#define CMD_CHLNG_DATA_LEN             8
#define CMD_SCRYPT_DATA_LEN            16
#define CMD_ABORT_DATA_LEN             0
#define CMD_ACURXSIZE_DATA_LEN         2
#define CMD_KEEPACTIVE_DATA_LEN        2
#define CMD_MFG_DATA_LEN               3 /* variable length command */

#define REPLY_ACK_LEN                  1
#define REPLY_PDID_LEN                 13
#define REPLY_PDCAP_LEN                1   /* variable length command */
#define REPLY_PDCAP_ENTITY_LEN         3
#define REPLY_LSTATR_LEN               3
#define REPLY_RSTATR_LEN               2
#define REPLY_COM_LEN                  6
#define REPLY_NAK_LEN                  2
#define REPLY_CCRYPT_LEN               33
#define REPLY_RMAC_I_LEN               17
#define REPLY_KEYPAD_LEN               2
#define REPLY_RAW_LEN                  4
#define REPLY_MFGREP_LEN               4 /* variable length command */

enum osdp_pd_error_e {
	OSDP_PD_ERR_NONE = 0,
	OSDP_PD_ERR_WAIT = -1,
	OSDP_PD_ERR_GENERIC = -2,
	OSDP_PD_ERR_REPLY = -3,
	OSDP_PD_ERR_IGNORE = -4,
	OSDP_PD_ERR_NO_DATA = -5,
	/* Reply was built (or prebuilt) but the channel was momentarily not
	 * ready to queue it. The finalized packet is cached on pd; the caller
	 * must yield and re-invoke pd_send_reply on the next refresh. */
	OSDP_PD_ERR_RETRY_SEND = -6,
};

/* Implicit capabilities */
static struct osdp_pd_cap osdp_pd_cap[] = {
	{
		OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT,
		1, /* The PD supports the 16-bit CRC-16 mode */
		0, /* N/A */
	},
	{
		OSDP_PD_CAP_COMMUNICATION_SECURITY,
		1, /* (Bit-0) AES128 support */
		0, /* N/A */
	},
	{
		OSDP_PD_CAP_RECEIVE_BUFFERSIZE,
		BYTE_0(OSDP_PACKET_BUF_SIZE),
		BYTE_1(OSDP_PACKET_BUF_SIZE),
	},
	{
		OSDP_PD_CAP_OSDP_VERSION,
		2, /* SIA OSDP 2.2 */
		0, /* N/A */
	},
	{ -1, 0, 0 } /* Sentinel */
};

static int pd_event_queue_init(struct osdp_pd *pd)
{
	queue_init(&pd->event_queue);
	return 0;
}

static int pd_event_enqueue(struct osdp_pd *pd, const struct osdp_event *event)
{
	queue_enqueue(&pd->event_queue, (queue_node_t *)&event->_node);
	return 0;
}

static int pd_event_dequeue(struct osdp_pd *pd, const struct osdp_event **event)
{
	queue_node_t *node;

	if (queue_dequeue(&pd->event_queue, &node))
		return -1;
	*event = CONTAINER_OF(node, struct osdp_event, _node);
	return 0;
}

static inline void pd_complete_event(struct osdp_pd *pd,
				     const struct osdp_event *event,
				     enum osdp_completion_status status)
{
	if (!event || !pd->event_completion_callback)
		return;
	pd->event_completion_callback(pd->event_completion_callback_arg,
				      event, status);
	osdp_metrics_report(pd, OSDP_METRIC_EVENT);
}

static int pd_translate_event(struct osdp_pd *pd, const struct osdp_event *event)
{
	int reply_code = 0;

	switch (event->type) {
	case OSDP_EVENT_CARDREAD:
		if (event->cardread.format == OSDP_CARD_FMT_RAW_UNSPECIFIED ||
		    event->cardread.format == OSDP_CARD_FMT_RAW_WIEGAND) {
			reply_code = REPLY_RAW;
		} else if (event->cardread.format == OSDP_CARD_FMT_ASCII) {
			/**
			 * osdp_FMT was underspecified by SIA from get-go. It
			 * was marked for deprecation in v2.2.2.
			 *
			 * See: https://github.com/osdp-dev/libosdp/issues/206
			 */
			LOG_WRN("Event CardRead::format::OSDP_CARD_FMT_ASCII"
				" is deprecated. Ignoring");
		} else {
			LOG_ERR("Event: cardread; Error: unknown format");
			break;
		}
		break;
	case OSDP_EVENT_KEYPRESS:
		reply_code = REPLY_KEYPAD;
		break;
	case OSDP_EVENT_STATUS:
		switch(event->status.type) {
		case OSDP_STATUS_REPORT_INPUT:
			reply_code = REPLY_ISTATR;
			break;
		case OSDP_STATUS_REPORT_OUTPUT:
			reply_code = REPLY_OSTATR;
			break;
		case OSDP_STATUS_REPORT_LOCAL:
			reply_code = REPLY_LSTATR;
			break;
		case OSDP_STATUS_REPORT_REMOTE:
			reply_code = REPLY_RSTATR;
			break;
		}
		break;
	case OSDP_EVENT_MFGREP:
		reply_code = REPLY_MFGREP;
		break;
	default:
		LOG_ERR("Unknown event type %d", event->type);
		BUG();
	}
	if (reply_code == 0) {
		/* POLL command cannot fail even when there are errors here */
		return REPLY_ACK;
	}
	return reply_code;
}

static bool validate_command(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	bool result = true;

	switch (cmd->id) {
	case OSDP_CMD_LED:
		/* The ON Time OFF Time values cannot both be set to zero */
		if (cmd->led.temporary.control_code == 0x02 && /* SET */
		    cmd->led.temporary.on_count == 0 &&
		    cmd->led.temporary.off_count == 0) {
			result = false;
		}
		if (cmd->led.permanent.control_code == 0x01 &&  /* SET */
		    cmd->led.permanent.on_count == 0 &&
		    cmd->led.permanent.off_count == 0) {
			result = false;
		}
		break;
	case OSDP_CMD_BUZZER:
		/* ON duration must nonzero unless the control_code is 0x01 */
		if (cmd->buzzer.on_count == 0 &&
		    cmd->buzzer.control_code != 1) {
			result = false;
		}
		break;
	default:
		break;
	}

	if (!result) {
		LOG_ERR("Command validation failed!");
	}
	return result;
}

static bool do_command_callback(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	int ret = -1;

	if (validate_command(pd, cmd) && pd->command_callback) {
		ret = pd->command_callback(pd->command_callback_arg, cmd);
	}
	if (ret != 0) {
		pd->reply_id = REPLY_NAK;
		pd->nak_code = OSDP_PD_NAK_RECORD;
		return false;
	}
	osdp_metrics_report(pd, OSDP_METRIC_COMMAND);
	return true;
}

static int pd_cmd_cap_ok(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	struct osdp_pd_cap *cap = NULL;

	/* Validate the cmd_id against a PD capabilities where applicable */
	switch (pd->cmd_id) {
	case CMD_ISTAT:
		cap = &pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_OSTAT:
		cap = &pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_OUT:
		cap = &pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL];
		if (!cmd || cap->compliance_level == 0 ||
		    cmd->output.output_no + 1 > cap->num_items) {
			break;
		}
		return 1;
	case CMD_LED:
		cap = &pd->cap[OSDP_PD_CAP_READER_LED_CONTROL];
		if (!cmd || cap->compliance_level == 0 ||
		    cmd->led.led_number + 1 > cap->num_items) {
			break;
		}
		return 1;
	case CMD_BUZ:
		cap = &pd->cap[OSDP_PD_CAP_READER_AUDIBLE_OUTPUT];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_TEXT:
		cap = &pd->cap[OSDP_PD_CAP_READER_TEXT_OUTPUT];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_CHLNG:
	case CMD_SCRYPT:
	case CMD_KEYSET:
		cap = &pd->cap[OSDP_PD_CAP_COMMUNICATION_SECURITY];
		if (cap->compliance_level == 0) {
			pd->reply_id = REPLY_NAK;
			pd->nak_code = OSDP_PD_NAK_SC_UNSUP;
			return 0;
		}
		return 1;
	}

	pd->reply_id = REPLY_NAK;
	pd->nak_code = OSDP_PD_NAK_CMD_UNKNOWN;
	LOG_ERR("PD is not capable of handling CMD(%02x); ", pd->cmd_id);
	return 0;
}

static int pd_prebuild_status_reply(struct osdp_pd *pd, int reply_id,
				    const struct osdp_status_report *status)
{
	int i, len = 0, n, hdr_len, data_off, ret;
	int packet_buf_size = get_tx_buf_size(pd);
	uint8_t *pkt = osdp_tx_staging_buf(pd);
	uint8_t *buf;
	int max_len;

	hdr_len = osdp_phy_packet_init(pd, pkt, packet_buf_size);
	if (hdr_len < 0) {
		return -1;
	}
	data_off = osdp_phy_packet_get_data_offset(pd, pkt);
	buf = pkt + data_off;
	max_len = packet_buf_size - data_off;

	switch (reply_id) {
	case REPLY_OSTATR:
		n = pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL].num_items;
		if (status->nr_entries != n || max_len < n + 1) {
			return -1;
		}
		buf[len++] = REPLY_OSTATR;
		for (i = 0; i < n; i++) {
			buf[len++] = status->report[i];
		}
		break;
	case REPLY_ISTATR:
		n = pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING].num_items;
		if (status->nr_entries != n || max_len < n + 1) {
			return -1;
		}
		buf[len++] = REPLY_ISTATR;
		for (i = 0; i < n; i++) {
			buf[len++] = status->report[i];
		}
		break;
	case REPLY_LSTATR:
		if (status->nr_entries < 2 || max_len < REPLY_LSTATR_LEN) {
			return -1;
		}
		buf[len++] = REPLY_LSTATR;
		buf[len++] = status->report[0];
		buf[len++] = status->report[1];
		break;
	case REPLY_RSTATR:
		if (status->nr_entries < 1 || max_len < REPLY_RSTATR_LEN) {
			return -1;
		}
		buf[len++] = REPLY_RSTATR;
		buf[len++] = status->report[0];
		break;
	default:
		return -1;
	}

	pd->packet_buf = pkt;
	pd->packet_buf_len = hdr_len + len;

	ret = osdp_phy_finalize_packet(pd, pkt, pd->packet_buf_len,
				       packet_buf_size);
	if (ret < 0) {
		return -1;
	}
	pd->packet_buf_len = ret;
	pd->reply_prebuilt = true;
	return 0;
}

/* Deliver a libosdp-synthesized notification to the app via the PD's
 * command callback. Gated by OSDP_FLAG_ENABLE_NOTIFICATION. Return value
 * is ignored -- these are informational, not wire commands. */
static void pd_deliver_notification(struct osdp_pd *pd,
				    enum osdp_notification_type type,
				    int arg0, int arg1)
{
	struct osdp_cmd cmd = { 0 };

	if (!pd->command_callback || !is_notifications_enabled(pd)) {
		return;
	}
	cmd.id = OSDP_CMD_NOTIFICATION;
	cmd.notif.type = type;
	cmd.notif.arg0 = arg0;
	cmd.notif.arg1 = arg1;
	(void)pd->command_callback(pd->command_callback_arg, &cmd);
	osdp_metrics_report(pd, OSDP_METRIC_EVENT);
}

static void notify_pd_status(struct osdp_pd *pd, bool is_online)
{
	pd_deliver_notification(pd, OSDP_NOTIFICATION_PD_STATUS,
				is_online, 0);
}

static void notify_sc_status(struct osdp_pd *pd)
{
	pd_deliver_notification(pd, OSDP_NOTIFICATION_SC_STATUS,
				sc_is_active(pd), sc_use_scbkd(pd));
}

/* Edge-triggered SC deactivate: only notify the app when the flag
 * actually flips. Keeps noisy paths (pd_error_reset on an already-
 * inactive SC) from spamming duplicate "SC inactive" events. */
static inline void pd_sc_deactivate(struct osdp_pd *pd)
{
	bool was_active = sc_is_active(pd);

	sc_deactivate(pd);
	if (was_active) {
		notify_sc_status(pd);
	}
}

static int pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len)
{
	int i, ret = OSDP_PD_ERR_GENERIC, pos = 0;
	struct osdp_cmd cmd = {};

	pd->reply_id = REPLY_NAK;
	pd->nak_code = OSDP_PD_NAK_RECORD;
	pd->cmd_id = cmd.id = buf[pos++];
	len--;

	if (is_enforce_secure(pd) && !sc_is_active(pd)) {
		/**
		 * Only CMD_ID, CMD_CAP and SC handshake commands (CMD_CHLNG
		 * and CMD_SCRYPT) are allowed when SC is inactive and
		 * ENFORCE_SECURE was requested.
		 */
		if (pd->cmd_id != CMD_ID && pd->cmd_id != CMD_CAP &&
		    pd->cmd_id != CMD_CHLNG && pd->cmd_id != CMD_SCRYPT) {
			LOG_ERR("CMD: %s(%02x) not allowed due to ENFORCE_SECURE",
				osdp_cmd_name(pd->cmd_id), pd->cmd_id);
			pd->nak_code = OSDP_PD_NAK_SC_COND;
			return OSDP_PD_ERR_REPLY;
		}
	}

	switch (pd->cmd_id) {
	case CMD_POLL:
	{
		const struct osdp_event *queued_event;

		if (len != CMD_POLL_DATA_LEN) {
			break;
		}
		/* Check if we have external events in the queue */
		if (pd_event_dequeue(pd, &queued_event) == 0) {
			ret = pd_translate_event(pd, queued_event);
			pd->reply_id = ret;
			pd->active_event = queued_event;
		} else {
			pd->reply_id = REPLY_ACK;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case CMD_LSTAT:
		if (len != CMD_LSTAT_DATA_LEN) {
			break;
		}
		cmd.id = OSDP_CMD_STATUS;
		cmd.status.type = OSDP_STATUS_REPORT_LOCAL;
		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		if (pd_prebuild_status_reply(pd, REPLY_LSTATR, &cmd.status)) {
			break;
		}
		pd->reply_id = REPLY_LSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ISTAT:
		if (len != CMD_ISTAT_DATA_LEN) {
			break;
		}
		if (!pd_cmd_cap_ok(pd, NULL)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		cmd.id = OSDP_CMD_STATUS;
		cmd.status.type = OSDP_STATUS_REPORT_INPUT;
		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		if (pd_prebuild_status_reply(pd, REPLY_ISTATR, &cmd.status)) {
			break;
		}
		pd->reply_id = REPLY_ISTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_OSTAT:
		if (len != CMD_OSTAT_DATA_LEN) {
			break;
		}
		if (!pd_cmd_cap_ok(pd, NULL)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		cmd.id = OSDP_CMD_STATUS;
		cmd.status.type = OSDP_STATUS_REPORT_OUTPUT;
		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		if (pd_prebuild_status_reply(pd, REPLY_OSTATR, &cmd.status)) {
			break;
		}
		pd->reply_id = REPLY_OSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_RSTAT:
		if (len != CMD_RSTAT_DATA_LEN) {
			break;
		}
		cmd.id = OSDP_CMD_STATUS;
		cmd.status.type = OSDP_STATUS_REPORT_REMOTE;
		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		if (pd_prebuild_status_reply(pd, REPLY_RSTATR, &cmd.status)) {
			break;
		}
		pd->reply_id = REPLY_RSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ID:
		if (len != CMD_ID_DATA_LEN) {
			break;
		}
		pos++;		/* Skip reply type info. */
		pd->reply_id = REPLY_PDID;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_CAP:
		if (len != CMD_CAP_DATA_LEN) {
			break;
		}
		pos++;		/* Skip reply type info. */
		pd->reply_id = REPLY_PDCAP;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_OUT: {
		int count;
		if ((len % CMD_OUT_DATA_LEN) != 0) {
			break;
		}
		count = len / CMD_OUT_DATA_LEN;
		ret = OSDP_PD_ERR_REPLY;
		for (i = 0; i < count; i++) {
			cmd.id = OSDP_CMD_OUTPUT;
			cmd.output.output_no = buf[pos++];
			cmd.output.control_code = buf[pos++];
			cmd.output.timer_count = bread_u16_le(buf, &pos);
			if (!pd_cmd_cap_ok(pd, &cmd)) {
				break;
			}
			if (!do_command_callback(pd, &cmd)) {
				break;
			}
		}
		if (i == count) {
			pd->reply_id = REPLY_ACK;
			ret = OSDP_PD_ERR_NONE;
		}
		break;
	}
	case CMD_LED: {
		int count;
		if ((len % CMD_LED_DATA_LEN) != 0) {
			break;
		}
		count = len / CMD_LED_DATA_LEN;
		ret = OSDP_PD_ERR_REPLY;
		for (i = 0; i < count; i++) {
			cmd.id = OSDP_CMD_LED;
			cmd.led.reader = buf[pos++];
			cmd.led.led_number = buf[pos++];

			cmd.led.temporary.control_code = buf[pos++];
			cmd.led.temporary.on_count = buf[pos++];
			cmd.led.temporary.off_count = buf[pos++];
			cmd.led.temporary.on_color = buf[pos++];
			cmd.led.temporary.off_color = buf[pos++];
			cmd.led.temporary.timer_count = bread_u16_le(buf, &pos);

			cmd.led.permanent.control_code = buf[pos++];
			cmd.led.permanent.on_count = buf[pos++];
			cmd.led.permanent.off_count = buf[pos++];
			cmd.led.permanent.on_color = buf[pos++];
			cmd.led.permanent.off_color = buf[pos++];
			if (!pd_cmd_cap_ok(pd, &cmd)) {
				break;
			}
			if (!do_command_callback(pd, &cmd)) {
				break;
			}
		}
		if (i == count) {
			pd->reply_id = REPLY_ACK;
			ret = OSDP_PD_ERR_NONE;
		}
		break;
	}
	case CMD_BUZ: {
		int count;
		if ((len % CMD_BUZ_DATA_LEN) != 0) {
			break;
		}
		count = len / CMD_BUZ_DATA_LEN;
		ret = OSDP_PD_ERR_REPLY;
		for (i = 0; i < count; i++) {
			cmd.id = OSDP_CMD_BUZZER;
			cmd.buzzer.reader = buf[pos++];
			cmd.buzzer.control_code = buf[pos++];
			cmd.buzzer.on_count = buf[pos++];
			cmd.buzzer.off_count = buf[pos++];
			cmd.buzzer.rep_count = buf[pos++];
			if (!pd_cmd_cap_ok(pd, &cmd)) {
				break;
			}
			if (!do_command_callback(pd, &cmd)) {
				break;
			}
		}
		if (i == count) {
			pd->reply_id = REPLY_ACK;
			ret = OSDP_PD_ERR_NONE;
		}
		break;
	}
	case CMD_TEXT:
		if (len < CMD_TEXT_DATA_LEN) {
			break;
		}
		cmd.id = OSDP_CMD_TEXT;
		cmd.text.reader = buf[pos++];
		cmd.text.control_code = buf[pos++];
		cmd.text.temp_time = buf[pos++];
		cmd.text.offset_row = buf[pos++];
		cmd.text.offset_col = buf[pos++];
		cmd.text.length = buf[pos++];
		if (cmd.text.length > OSDP_CMD_TEXT_MAX_LEN ||
		    ((len - CMD_TEXT_DATA_LEN) < cmd.text.length) ||
		    cmd.text.length > OSDP_CMD_TEXT_MAX_LEN) {
			break;
		}
		memcpy(cmd.text.data, buf + pos, cmd.text.length);
		ret = OSDP_PD_ERR_REPLY;
		if (!pd_cmd_cap_ok(pd, &cmd)) {
			break;
		}
		if (!do_command_callback(pd, &cmd)) {
			break;
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_COMSET:
		if (len != CMD_COMSET_DATA_LEN) {
			break;
		}
		cmd.id = OSDP_CMD_COMSET;
		cmd.comset.address = buf[pos++];
		cmd.comset.baud_rate = bread_u32_le(buf, &pos);
		if (cmd.comset.address >= 0x7F) {
			LOG_ERR("COMSET Failed! command discarded");
			cmd.comset.address = pd->address;
			cmd.comset.baud_rate = pd->baud_rate;
			break;
		}
		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		pd->comset_pending.address = cmd.comset.address;
		pd->comset_pending.baud_rate = cmd.comset.baud_rate;
		pd->reply_id = REPLY_COM;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_MFG:
		if (len < CMD_MFG_DATA_LEN) {
			break;
		}
		cmd.id = OSDP_CMD_MFG;
		cmd.mfg.vendor_code = bread_u24_le(buf, &pos);
		cmd.mfg.length = len - CMD_MFG_DATA_LEN;
		if (cmd.mfg.length > OSDP_CMD_MFG_MAX_DATALEN) {
			LOG_ERR("cmd length error");
			break;
		}
		for (i = 0; i < cmd.mfg.length; i++) {
			cmd.mfg.data[i] = buf[pos++];
		}

		if (pd->command_callback) {
			ret = pd->command_callback(pd->command_callback_arg, &cmd);
			if (ret < 0) {
				pd->nak_code = OSDP_PD_NAK_RECORD;
				ret = OSDP_PD_ERR_REPLY;
				break;
			}
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ACURXSIZE:
		if (len < CMD_ACURXSIZE_DATA_LEN) {
			break;
		}
		pd->peer_rx_size = bread_u16_le(buf, &pos);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_KEEPACTIVE:
		if (len < CMD_KEEPACTIVE_DATA_LEN) {
			break;
		}
		pd->sc_tstamp += bread_u16_le(buf, &pos);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ABORT:
		if (len != CMD_ABORT_DATA_LEN) {
			break;
		}
		osdp_file_tx_abort(pd);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_FILETRANSFER:
		ret = osdp_file_cmd_tx_decode(pd, buf + pos, len);
		if (ret == 0) {
			ret = OSDP_PD_ERR_NONE;
			pd->reply_id = REPLY_FTSTAT;
			break;
		}
		break;
	case CMD_XWR:
		ret = osdp_trs_cmd_decode(pd, buf + pos, len);
		if (ret == 0) {
			ret = OSDP_PD_ERR_NONE;
			pd->reply_id = REPLY_XRD;
			break;
		}
		break;
	case CMD_KEYSET:
		if (len != CMD_KEYSET_DATA_LEN) {
			break;
		}
		/* only key_type == 1 (SCBK) and key_len == 16 is supported */
		if (buf[pos] != 1 || buf[pos + 1] != 16) {
			LOG_ERR("Keyset invalid len/type: %d/%d",
				buf[pos], buf[pos + 1]);
			break;
		}
		ret = OSDP_PD_ERR_REPLY;
		pd->nak_code = OSDP_PD_NAK_SC_COND;
		if (!pd_cmd_cap_ok(pd, NULL)) {
			break;
		}
		if (!sc_is_active(pd)) {
			LOG_ERR("Keyset with SC inactive");
			break;
		}
		if (!pd->command_callback) {
			LOG_ERR("Keyset not permitted without setting a command"
				" callback; rejecting new KEY");
			break;
		}
		cmd.id = OSDP_CMD_KEYSET;
		cmd.keyset.type = buf[pos++];
		cmd.keyset.length = buf[pos++];
		memcpy(cmd.keyset.data, buf + pos, 16);
		if (!do_command_callback(pd, &cmd)) {
			pd->nak_code = OSDP_PD_NAK_SC_COND;
			LOG_ERR("Keyset with SC inactive");
			break;
		}
		ret = OSDP_PD_ERR_NONE;
		pd->reply_id = REPLY_ACK;
			memcpy(pd->keyset_pending, cmd.keyset.data, 16);
		break;
	case CMD_CHLNG:
		if (len != CMD_CHLNG_DATA_LEN) {
			break;
		}
		ret = OSDP_PD_ERR_REPLY;
		if (!pd_cmd_cap_ok(pd, NULL)) {
			break;
		}
		pd_sc_deactivate(pd);
		osdp_sc_setup(pd);
		osdp_metrics_report(pd, OSDP_METRIC_SC_HANDSHAKE);
		memcpy(pd->sc.cp_random, buf + pos, 8);
		pd->reply_id = REPLY_CCRYPT;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_SCRYPT:
		if (len != CMD_SCRYPT_DATA_LEN) {
			break;
		}
		ret = OSDP_PD_ERR_REPLY;
		if (!pd_cmd_cap_ok(pd, NULL)) {
			break;
		}
		if (sc_is_active(pd)) {
			pd->nak_code = OSDP_PD_NAK_SC_COND;
			LOG_EM("Out of order CMD_SCRYPT; has CP gone rogue?");
			break;
		}
		memcpy(pd->sc.cp_cryptogram, buf + pos, CMD_SCRYPT_DATA_LEN);
		if (osdp_verify_cp_cryptogram(pd)) {
			/**
			 * The PD can respond with NAK(5) when it fails to
			 * verify the CP_crypt.
			 */
			pd->nak_code = OSDP_PD_NAK_SC_UNSUP;
			osdp_metrics_report(pd, OSDP_METRIC_SC_FAILURE);
			LOG_WRN("failed to verify CP_crypt");
			break;
		}
		pd->reply_id = REPLY_RMAC_I;
		ret = OSDP_PD_ERR_NONE;
		break;
	default:
		LOG_ERR("Unknown CMD(%02x)", pd->cmd_id);
		pd->reply_id = REPLY_NAK;
		pd->nak_code = OSDP_PD_NAK_CMD_UNKNOWN;
		return OSDP_PD_ERR_REPLY;
	}

	if (ret == OSDP_PD_ERR_GENERIC) {
		LOG_ERR("Failed to decode command: CMD(%02x) Len:%d ret:%d",
			pd->cmd_id, len, ret);
		pd->reply_id = REPLY_NAK;
		pd->nak_code = OSDP_PD_NAK_CMD_LEN;
		ret = OSDP_PD_ERR_REPLY;
	}

	if (pd->cmd_id != CMD_POLL) {
		LOG_DBG("CMD: %s(%02x) REPLY: %s(%02x)",
			osdp_cmd_name(pd->cmd_id), pd->cmd_id,
			osdp_reply_name(pd->reply_id), pd->reply_id);
	}

	return ret;
}

static inline void assert_buf_len(int need, int have)
{
	__ASSERT(need < have, "OOM at build command: need:%d have:%d",
		 need, have);
	(void)need;
	(void)have;
}

/**
 * Returns:
 * +ve: length of command
 * -ve: error
 */
static int pd_build_reply(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int ret = OSDP_PD_ERR_GENERIC;
	int i, len = 0;
	const struct osdp_event *event = pd->active_event;
	int data_off = osdp_phy_packet_get_data_offset(pd, buf);
	uint8_t *smb = osdp_phy_packet_get_smb(pd, buf);

	buf += data_off;
	max_len -= data_off;

	switch (pd->reply_id) {
	case REPLY_ACK:
		assert_buf_len(REPLY_ACK_LEN, max_len);
		buf[len++] = pd->reply_id;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PDID:
		assert_buf_len(REPLY_PDID_LEN, max_len);
		buf[len++] = pd->reply_id;
		bwrite_u24_le(pd->id.vendor_code, buf, &len);
		buf[len++] = pd->id.model;
		buf[len++] = pd->id.version;
		bwrite_u32_le(pd->id.serial_number, buf, &len);
		bwrite_u24_be(pd->id.firmware_version, buf, &len);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PDCAP:
		assert_buf_len(REPLY_PDCAP_LEN, max_len);
		buf[len++] = pd->reply_id;
		for (i = 1; i < OSDP_PD_CAP_SENTINEL; i++) {
			if (pd->cap[i].function_code != i) {
				continue;
			}
			if (max_len < REPLY_PDCAP_ENTITY_LEN) {
				LOG_ERR("Out of buffer space!");
				break;
			}
			buf[len++] = i;
			buf[len++] = pd->cap[i].compliance_level;
			buf[len++] = pd->cap[i].num_items;
			max_len -= REPLY_PDCAP_ENTITY_LEN;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_OSTATR: {
		int n = pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL].num_items;
		if (!event || event->type != OSDP_EVENT_STATUS) {
			break;
		}
		if (event->status.nr_entries != n) {
			break;
		}
		assert_buf_len(n + 1, max_len);
		buf[len++] = pd->reply_id;
		for (i = 0; i < n; i++) {
			buf[len++] = event->status.report[i];
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case REPLY_ISTATR: {
		int n = pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING].num_items;
		if (!event || event->type != OSDP_EVENT_STATUS) {
			break;
		}
		if (event->status.nr_entries != n) {
			break;
		}
		assert_buf_len(n + 1, max_len);
		buf[len++] = pd->reply_id;
		for (i = 0; i < n; i++) {
			buf[len++] = event->status.report[i];
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case REPLY_LSTATR:
		assert_buf_len(REPLY_LSTATR_LEN, max_len);
		if (!event || event->type != OSDP_EVENT_STATUS ||
		    event->status.nr_entries < 2) {
			break;
		}
		buf[len++] = pd->reply_id;
		buf[len++] = event->status.report[0]; // tamper
		buf[len++] = event->status.report[1]; // power
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RSTATR:
		assert_buf_len(REPLY_RSTATR_LEN, max_len);
		if (!event || event->type != OSDP_EVENT_STATUS ||
		    event->status.nr_entries < 1) {
			break;
		}
		buf[len++] = pd->reply_id;
		buf[len++] = event->status.report[0]; // power
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_KEYPAD:
		if (!event || event->type != OSDP_EVENT_KEYPRESS) {
			break;
		}
		assert_buf_len(REPLY_KEYPAD_LEN + event->keypress.length, max_len);
		buf[len++] = pd->reply_id;
		buf[len++] = (uint8_t)event->keypress.reader_no;
		buf[len++] = (uint8_t)event->keypress.length;
		memcpy(buf + len, event->keypress.data, event->keypress.length);
		len += event->keypress.length;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RAW: {
		int len_bytes;

		if (!event || event->type != OSDP_EVENT_CARDREAD) {
			break;
		}
		len_bytes = (event->cardread.length + 7) / 8;
		assert_buf_len(REPLY_RAW_LEN + len_bytes, max_len);
		buf[len++] = pd->reply_id;
		buf[len++] = (uint8_t)event->cardread.reader_no;
		buf[len++] = (uint8_t)event->cardread.format;
		bwrite_u16_le(event->cardread.length, buf, &len);
		memcpy(buf + len, event->cardread.data, len_bytes);
		len += len_bytes;
		ret = OSDP_PD_ERR_NONE;
		break;
	}
		case REPLY_COM:
			assert_buf_len(REPLY_COM_LEN, max_len);
			/**
			 * If COMSET succeeds, the PD must reply with the old params and
			 * then switch to the new params from then then on. We cache
			 * the pending values in pd->comset_pending while decoding CMD_COMSET
			 * and use them here in REPLY_COM.
			 */
				buf[len++] = pd->reply_id;
				buf[len++] = pd->comset_pending.address;
		bwrite_u32_le(pd->comset_pending.baud_rate, buf, &len);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_NAK:
		assert_buf_len(REPLY_NAK_LEN, max_len);
		buf[len++] = pd->reply_id;
		buf[len++] = pd->nak_code;
		osdp_metrics_report(pd, OSDP_METRIC_NAK);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_MFGREP:
		if (!event || event->type != OSDP_EVENT_MFGREP) {
			break;
		}
		assert_buf_len(REPLY_MFGREP_LEN + event->mfgrep.length, max_len);
		buf[len++] = pd->reply_id;
		bwrite_u24_le(event->mfgrep.vendor_code, buf, &len);
		memcpy(buf + len, event->mfgrep.data, event->mfgrep.length);
		len += event->mfgrep.length;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_FTSTAT:
		buf[len++] = pd->reply_id;
		ret = osdp_file_cmd_stat_build(pd, buf + len, max_len);
		if (ret <= 0) {
			break;
		}
		len += ret;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_XRD:
		buf[len++] = pd->reply_id;
		ret = osdp_trs_reply_build(pd, buf + len, max_len);
		if (ret <= 0) {
			break;
		}
		len += ret;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_CCRYPT:
		if (smb == NULL) {
			break;
		}
		assert_buf_len(REPLY_CCRYPT_LEN, max_len);
		osdp_fill_random(pd->sc.pd_random, 8);
		osdp_compute_session_keys(pd);
		osdp_compute_pd_cryptogram(pd);
		buf[len++] = pd->reply_id;
		memcpy(buf + len, pd->sc.pd_client_uid, 8);
		memcpy(buf + len + 8, pd->sc.pd_random, 8);
		memcpy(buf + len + 16, pd->sc.pd_cryptogram, 16);
		len += 32;
		smb[0] = 3;      /* length */
		smb[1] = SCS_12; /* type */
		smb[2] = sc_use_scbkd(pd) ? 0 : 1;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RMAC_I:
		if (smb == NULL) {
			break;
		}
		assert_buf_len(REPLY_RMAC_I_LEN, max_len);
		osdp_compute_rmac_i(pd);
		buf[len++] = pd->reply_id;
		memcpy(buf + len, pd->sc.r_mac, 16);
		len += 16;
		smb[0] = 3;       /* length */
		smb[1] = SCS_14;  /* type */
		smb[2] = 1;       /* CP auth succeeded */
		sc_activate(pd);
		notify_sc_status(pd);
		pd->sc_tstamp = osdp_millis_now();
		if (sc_use_scbkd(pd)) {
			LOG_WRN("SC Active with SCBK-D");
		} else {
			LOG_INF("SC Active");
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	default: BUG();
	}

	if (smb && (smb[1] > SCS_14) && sc_is_active(pd)) {
		smb[0] = 2; /* length */
		smb[1] = (len > 1) ? SCS_18 : SCS_16;
	}

	if (ret != 0) {
		/* catch all errors and report it as a RECORD error to CP */
		LOG_ERR("Failed to build REPLY: %s(%02x); Sending NAK instead!",
			osdp_reply_name(pd->reply_id), pd->reply_id);
		assert_buf_len(REPLY_NAK_LEN, max_len);
		buf[0] = REPLY_NAK;
		buf[1] = OSDP_PD_NAK_RECORD;
		len = 2;
		osdp_metrics_report(pd, OSDP_METRIC_NAK);
	}

	return len;
}

/* Build + finalize the reply into the staging buffer. Sets reply_prebuilt
 * to mark packet_buf[0..packet_buf_len) as a finalized wire packet awaiting
 * channel queueing. pd_send_reply() will drain it. */
static int pd_build_reply_packet(struct osdp_pd *pd)
{
	int ret, packet_buf_size = get_tx_buf_size(pd);

	pd->packet_buf = osdp_tx_staging_buf(pd);

	ret = osdp_phy_packet_init(pd, pd->packet_buf, packet_buf_size);
	if (ret < 0) {
		return OSDP_PD_ERR_GENERIC;
	}
	pd->packet_buf_len = ret;

	ret = pd_build_reply(pd, pd->packet_buf, packet_buf_size);
	if (ret <= 0) {
		return OSDP_PD_ERR_GENERIC;
	}
	pd->packet_buf_len += ret;

	ret = osdp_phy_finalize_packet(pd, pd->packet_buf, pd->packet_buf_len,
				       packet_buf_size);
	if (ret < 0) {
		return OSDP_PD_ERR_GENERIC;
	}
	pd->packet_buf_len = ret;
	pd->reply_prebuilt = true;
	return OSDP_PD_ERR_NONE;
}

/* Queue the finalized reply parked in packet_buf onto the channel. On
 * OSDP_ERR_PKT_WAIT_TX (transport momentarily not ready) leaves
 * reply_prebuilt set so the next refresh re-invokes this path with the
 * same bytes. */
static int pd_send_reply(struct osdp_pd *pd)
{
	int ret = osdp_phy_send_packet(pd, pd->packet_buf, pd->packet_buf_len);

	if (ret == OSDP_ERR_PKT_WAIT_TX) {
		return OSDP_PD_ERR_RETRY_SEND;
	}
	pd->reply_prebuilt = false;
	/* packet_buf_len doubles as the staged TX reply length; clear it now
	 * the reply is on the wire so the next RX cycle starts from zero. The
	 * sent length lives on in last_tx_len for the seq-repeat cache. */
	pd->packet_buf_len = 0;
	if (ret < 0) {
		return OSDP_PD_ERR_GENERIC;
	}
	pd->last_tx_len = (uint16_t)ret;
	pd->last_cmd_id = (uint8_t)pd->cmd_id;
	return OSDP_PD_ERR_NONE;
}

static int pd_receive_and_process_command(struct osdp_pd *pd)
{
	int err, len;
	uint8_t *buf;

	pd->reply_prebuilt = false;

	err = osdp_phy_check_packet(pd);

	/* Translate phy error codes to PD errors */
	switch (err) {
	case OSDP_ERR_PKT_NONE:
		break;
	case OSDP_ERR_PKT_NACK:
		return OSDP_PD_ERR_REPLY;
	case OSDP_ERR_PKT_NO_DATA:
		return OSDP_PD_ERR_NO_DATA;
	case OSDP_ERR_PKT_WAIT:
		return OSDP_PD_ERR_WAIT;
	case OSDP_ERR_PKT_SKIP:
		osdp_phy_state_reset(pd, false);
		return OSDP_PD_ERR_IGNORE;
	case OSDP_ERR_PKT_FMT:
		return OSDP_PD_ERR_GENERIC;
	default:
		return err; /* propagate other errors as-is */
	}

	len = osdp_phy_decode_packet(pd, &buf);
	if (len <= 0) {
		if (len == OSDP_ERR_PKT_NACK) {
			return OSDP_PD_ERR_REPLY; /* Send a NAK */
		}
		return OSDP_PD_ERR_GENERIC; /* fatal errors */
	}

	return pd_decode_command(pd, buf, len);
}

static inline void pd_error_reset(struct osdp_pd *pd)
{
	pd_sc_deactivate(pd);
	osdp_phy_state_reset(pd, false);
}

static void osdp_pd_update(struct osdp_pd *pd)
{
	int ret;

	/**
	 * If secure channel is established, we need to make sure that
	 * the session is valid before accepting a command.
	 */
	if (sc_is_active(pd) &&
	    osdp_millis_since(pd->sc_tstamp) > OSDP_PD_SC_TIMEOUT_MS) {
		LOG_INF("PD SC session timeout!");
		pd_sc_deactivate(pd);
	}

	/**
	 * Track online/offline based on recent CP activity. pd->tstamp is
	 * updated by the phy layer on any inbound bytes from the CP, which
	 * matches how osdp_get_status_mask() already infers link health.
	 */
	if (is_pd_online(pd) &&
	    osdp_millis_since(pd->tstamp) > OSDP_PD_ONLINE_TOUT_MS) {
		LOG_INF("PD offline; lost CP activity");
		pd_set_offline(pd);
		osdp_file_tx_abort(pd);
		notify_pd_status(pd, false);
	}

	/* If a previous refresh left a finalized reply parked in packet_buf
	 * (channel EAGAIN, or pd_prebuild_status_reply staged one), skip RX
	 * entirely — accepting new RX would advance the sequence counter and
	 * stale the pending reply. Fall through to the send stage below. */
	if (!pd->reply_prebuilt) {
		ret = pd_receive_and_process_command(pd);

		if (IS_ENABLED(OPT_OSDP_RX_ZERO_COPY)) {
			osdp_phy_release_packet(pd);
		}

		if (ret == OSDP_PD_ERR_IGNORE || ret == OSDP_PD_ERR_NO_DATA) {
			return;
		}

		if (ret == OSDP_PD_ERR_WAIT &&
		    osdp_millis_since(pd->tstamp) < OSDP_RESP_TOUT_MS) {
			return;
		}

		if (ret != OSDP_PD_ERR_NONE && ret != OSDP_PD_ERR_REPLY) {
			LOG_ERR("CMD receive error/timeout - err:%d", ret);
			pd_error_reset(pd);
			return;
		}

		/* ret is NONE or REPLY here: either way, a valid packet was
		 * decoded from the CP, so the link is active. */
		if (!is_pd_online(pd)) {
			LOG_INF("PD online; CP link active");
			pd_set_online(pd);
			notify_pd_status(pd, true);
		}

		if (ret == OSDP_PD_ERR_NONE && sc_is_active(pd)) {
			pd->sc_tstamp = osdp_millis_now();
		}

		/* pd_prebuild_status_reply() may already have finalized a
		 * status reply during dispatch; otherwise build one now. */
		if (!pd->reply_prebuilt) {
			ret = pd_build_reply_packet(pd);
			if (ret != OSDP_PD_ERR_NONE) {
				LOG_ERR("Failed to build reply for CMD(%02x)",
					pd->cmd_id);
				pd_error_reset(pd);
				return;
			}
		}
	}

	ret = pd_send_reply(pd);
	if (ret == OSDP_PD_ERR_RETRY_SEND) {
		/* Channel not ready. Reply stays parked in packet_buf and
		 * reply_prebuilt stays set; next refresh re-enters the send
		 * stage directly. */
		return;
	}
	if (ret == OSDP_PD_ERR_NONE) {
		if (pd->active_event) {
			pd_complete_event(pd, pd->active_event, OSDP_COMPLETION_OK);
			pd->active_event = NULL;
		}
		if (pd->cmd_id == CMD_KEYSET && pd->reply_id == REPLY_ACK) {
			memcpy(pd->sc.scbk, pd->keyset_pending, 16);
			osdp_fill_zeros(pd->keyset_pending, 16);
			CLEAR_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
			CLEAR_FLAG(pd, PD_FLAG_INSTALL_MODE);
			pd_sc_deactivate(pd);
		} else if (pd->cmd_id == CMD_COMSET && pd->reply_id == REPLY_COM) {
			struct osdp_cmd comset_done_cmd = { 0 };
			/* COMSET command succeeded all the way:
			 *
			 * - CP requested the change (with OSDP_CMD_COMSET)
			 * - PD app ack-ed this change (but didn't commit
			 *   the change to it's non-volatile storage)
			 * - CP was notified that the command succeeded. So
			 *   it should have switched to the new settings
			 *
			 *  Now we must notify the PD app so it can actually
			 *  switch the channel speed, reset any other state
			 *  it held and commit this change to non-volatile
			 *  storage.
			 */
			comset_done_cmd.id = OSDP_CMD_COMSET_DONE;
			comset_done_cmd.comset.address = pd->comset_pending.address;
			comset_done_cmd.comset.baud_rate = pd->comset_pending.baud_rate;
			do_command_callback(pd, &comset_done_cmd);
			pd->address = (int)pd->comset_pending.address;
			pd->baud_rate = (int)pd->comset_pending.baud_rate;
			LOG_INF("COMSET Succeeded! New PD-Addr: %d; Baud: %d",
				pd->address, pd->baud_rate);
		}
		osdp_phy_progress_sequence(pd);
	} else {
		if (pd->active_event) {
			pd_complete_event(pd, pd->active_event, OSDP_COMPLETION_FAILED);
			pd->active_event = NULL;
		}
		/**
		 * PD received and decoded a valid command from CP but failed to
		 * send the intended response?? This should not happen; but if
		 * it did, we cannot do anything about it, just complain about
		 * it and limp back home.
		 */
		LOG_EM("REPLY send failed! CP may be waiting..");
	}
	osdp_phy_state_reset(pd, false);
}

static void osdp_pd_set_attributes(struct osdp_pd *pd,
				   const struct osdp_pd_cap *cap,
				   const struct osdp_pd_id *id)
{
	int fc;

	while (cap && ((fc = cap->function_code) > 0)) {
		if (fc >= OSDP_PD_CAP_SENTINEL) {
			break;
		}
		pd->cap[fc].function_code = cap->function_code;
		pd->cap[fc].compliance_level = cap->compliance_level;
		pd->cap[fc].num_items = cap->num_items;
		cap++;
	}
	if (id != NULL) {
		memcpy(&pd->id, id, sizeof(struct osdp_pd_id));
	}
}

static void pd_collect_init_flags(struct osdp_pd *pd, uint32_t flags)
{
	if (flags & OSDP_FLAG_ENFORCE_SECURE) {
		SET_FLAG(pd, PD_FLAG_ENFORCE_SECURE);
	}
	if (flags & OSDP_FLAG_INSTALL_MODE) {
		SET_FLAG(pd, PD_FLAG_INSTALL_MODE);
	}
	if (flags & OSDP_FLAG_ENABLE_NOTIFICATION) {
		SET_FLAG(pd, PD_FLAG_ENABLE_NOTIF);
	}
	if (flags & OSDP_FLAG_CAPTURE_PACKETS) {
		SET_FLAG(pd, PD_FLAG_CAPTURE_PKT);
	}
	if (flags & OSDP_FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK) {
		SET_FLAG(pd, PD_FLAG_ALLOW_EMPTY_EDB);
	}
}

static int pd_setup_rx_storage(const struct osdp_channel *channel,
			       struct osdp_pd *pd)
{
#ifdef OPT_OSDP_RX_ZERO_COPY
	if (!channel->recv_pkt || !channel->release_pkt) {
		LOG_ERR("recv_pkt/release_pkt cannot be NULL in OPT_OSDP_RX_ZERO_COPY");
		return -1;
	}

	pd->rx_pkt = pd_rx_pkt_alloc();
	if (!pd->rx_pkt) {
		LOG_ERR("Failed to allocate rx packet store");
		return -1;
	}
#else /* OPT_OSDP_RX_ZERO_COPY */
	ARG_UNUSED(channel);

	pd->rx_rb = pd_rx_rb_alloc();
	if (!pd->rx_rb) {
		LOG_ERR("Failed to allocate rx ring buffer");
		return -1;
	}
#endif /* OPT_OSDP_RX_ZERO_COPY */

	return 0;
}

/* --- Exported Methods --- */

osdp_t *osdp_pd_setup(struct osdp_channel *channel, const osdp_pd_info_t *info)
{
	struct osdp_pd *pd;
	struct osdp *ctx;

	assert(info);
	assert(channel);

	ctx = pd_ctx_alloc();
	if (ctx == NULL) {
		LOG_PRINT("Failed to allocate osdp context");
		return NULL;
	}

	ctx->pd = pd_instance_alloc();
	if (ctx->pd == NULL) {
		LOG_PRINT("Failed to allocate osdp_pd context");
		goto error;
	}

	input_check_init(ctx);
	ctx->_num_pd = 1;

#ifndef OPT_OSDP_LOG_MINIMAL
	logger_get_default(&ctx->logger);
#endif

	SET_CURRENT_PD(ctx, 0);
	pd = osdp_to_pd(ctx, 0);

	pd->osdp_ctx = ctx;
	pd->idx = 0;
	pd->packet_buf = osdp_tx_staging_buf(pd);
	if (info->name) {
		strncpy(pd->name, info->name, OSDP_PD_NAME_MAXLEN - 1);
	} else {
		snprintf(pd->name, OSDP_PD_NAME_MAXLEN, "PD-%d", info->address);
	}
	pd->baud_rate = info->baud_rate;
	pd->address = info->address;
	pd->flags = 0;
	pd->seq_number = -1;

	memcpy(&ctx->channel, channel, sizeof(struct osdp_channel));

	if (pd_setup_rx_storage(channel, pd)) {
		goto error;
	}

	pd_collect_init_flags(pd, info->flags);

	if (pd_event_queue_init(pd)) {
		goto error;
	}

	if (info->scbk == NULL) {
		if (is_enforce_secure(pd)) {
			LOG_ERR("SCBK must be provided in ENFORCE_SECURE");
			goto error;
		}
		LOG_WRN("SCBK not provided. PD is in INSTALL_MODE");
		SET_FLAG(pd, PD_FLAG_INSTALL_MODE);
	} else {
		memcpy(pd->sc.scbk, info->scbk, 16);
	}
	SET_FLAG(pd, PD_FLAG_SC_CAPABLE);
	if (IS_ENABLED(OPT_OSDP_SKIP_MARK_BYTE)) {
		SET_FLAG(pd, PD_FLAG_PKT_SKIP_MARK);
	}
	osdp_pd_set_attributes(pd, info->cap, &info->id);
	osdp_pd_set_attributes(pd, osdp_pd_cap, NULL);

	SET_FLAG(pd, PD_FLAG_PD_MODE); /* used in checks in phy */

	if (is_capture_enabled(pd)) {
		osdp_packet_capture_init(pd);
	}

	LOG_PRINT("PD Setup complete; LibOSDP-%s %s",
		  osdp_get_version(), osdp_get_source_info());

	return (osdp_t *)ctx;
error:
	osdp_pd_teardown((osdp_t *)ctx);
	return NULL;
}

void osdp_pd_teardown(osdp_t *ctx)
{
	assert(ctx);
	struct osdp *pd_ctx = TO_OSDP(ctx);
	struct osdp_pd *pd = osdp_to_pd(ctx, 0);
	const struct osdp_event *ev;

	while (pd_event_dequeue(pd, &ev) == 0) {
		pd_complete_event(pd, ev, OSDP_COMPLETION_ABORTED);
	}
	pd_complete_event(pd, pd->active_event, OSDP_COMPLETION_ABORTED);
	pd->active_event = NULL;

	if (is_capture_enabled(pd)) {
		osdp_packet_capture_finish(pd);
	}

	osdp_fill_zeros(&pd->sc, sizeof(struct osdp_secure_channel));

	if (pd_ctx->channel.close) {
		pd_ctx->channel.close(pd_ctx->channel.data);
	}

#ifndef OPT_OSDP_STATIC
#ifdef OPT_OSDP_RX_ZERO_COPY
	{
		safe_free(pd->rx_pkt);
	}
#else /* OPT_OSDP_RX_ZERO_COPY */
	{
		safe_free(pd->rx_rb);
	}
#endif /* OPT_OSDP_RX_ZERO_COPY */
	safe_free(pd->file);
	safe_free(pd_ctx->rx_buf);
	safe_free(pd);
	safe_free(ctx);
#endif
}

void osdp_pd_refresh(osdp_t *ctx)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	osdp_pd_update(pd);
}

void osdp_pd_set_capabilities(osdp_t *ctx, const struct osdp_pd_cap *cap)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	osdp_pd_set_attributes(pd, cap, NULL);
}

void osdp_pd_set_command_callback(osdp_t *ctx, pd_command_callback_t cb,
				  void *arg)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	pd->command_callback_arg = arg;
	pd->command_callback = cb;
}

void osdp_pd_set_event_completion_callback(osdp_t *ctx,
					   pd_event_completion_callback_t cb,
					   void *arg)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	pd->event_completion_callback = cb;
	pd->event_completion_callback_arg = arg;
}

int osdp_pd_submit_event(osdp_t *ctx, const struct osdp_event *event)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	if (event->type <= 0 ||
	    event->type >= OSDP_EVENT_SENTINEL) {
		return -1;
	}

	return pd_event_enqueue(pd, event);
}

int osdp_pd_notify_event(osdp_t *ctx, const struct osdp_event *event)
{
	return osdp_pd_submit_event(ctx, event);
}

int osdp_pd_flush_events(osdp_t *ctx)
{
	input_check(ctx);
	int count = 0;
	const struct osdp_event *ev;
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	while (pd_event_dequeue(pd, &ev) == 0) {
		pd_complete_event(pd, ev, OSDP_COMPLETION_FLUSHED);
		count++;
	}

	return count;
}

/* Export the PD frame codecs to out-of-module drivers (tests, embedders). */
OSDP_TEST_ALIAS(pd_decode_command);
OSDP_TEST_ALIAS(pd_build_reply);
