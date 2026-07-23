/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "osdp_common.h"
#include "osdp_file.h"
#include "osdp_piv.h"
#include "osdp_bio.h"
#include "osdp_diag.h"
#include "osdp_metrics.h"
#include "osdp_trs.h"

#ifndef OPT_OSDP_STATIC
#include <stdlib.h>
#endif

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
	{ OSDP_PD_CAP_SENTINEL, 0, 0 }
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

static int pd_event_peek(struct osdp_pd *pd, const struct osdp_event **event)
{
	queue_node_t *node;

	if (queue_peek_first(&pd->event_queue, &node))
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

/**
 * Commands whose reply carries application data are answered by the app with an
 * event. Is this event the answer to the command we're currently handling?
 */
static bool event_is_reply_to(int cmd_id, const struct osdp_event *event)
{
	switch (cmd_id) {
	case CMD_MFG:
		return event->type == OSDP_EVENT_MFGREP ||
		       event->type == OSDP_EVENT_MFGSTATR ||
		       event->type == OSDP_EVENT_MFGERRR;
	case CMD_BIOREAD:
		return event->type == OSDP_EVENT_BIOREADR;
	case CMD_BIOMATCH:
		return event->type == OSDP_EVENT_BIOMATCHR;
	case CMD_PIVDATA:
		return event->type == OSDP_EVENT_PIVDATAR;
	case CMD_GENAUTH:
		return event->type == OSDP_EVENT_GENAUTHR;
	case CMD_CRAUTH:
		return event->type == OSDP_EVENT_CRAUTHR;
	case CMD_OUT:
		/* The PD may answer osdp_OUT with the resulting output status
		 * instead of an ACK. See OSDP v2.2 subclause 6.9. */
		return event->type == OSDP_EVENT_STATUS &&
		       event->status.type == OSDP_STATUS_REPORT_OUTPUT;
	default:
		return false;
	}
}

static int pd_translate_event(struct osdp_pd *pd,
			      const struct osdp_event *event)
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
		switch (event->status.type) {
		case OSDP_STATUS_REPORT_INPUT:
			reply_code = REPLY_ISTATR;
			break;
		case OSDP_STATUS_REPORT_OUTPUT:
			reply_code = REPLY_OSTATR;
			break;
		case OSDP_STATUS_REPORT_LOCAL:
			reply_code = REPLY_LSTATR;
			break;
		case OSDP_STATUS_REPORT_READER:
			reply_code = REPLY_RSTATR;
			break;
		}
		break;
	case OSDP_EVENT_MFGREP:
		reply_code = REPLY_MFGREP;
		break;
	case OSDP_EVENT_MFGSTATR:
		reply_code = REPLY_MFGSTATR;
		break;
	case OSDP_EVENT_MFGERRR:
		reply_code = REPLY_MFGERRR;
		break;
	case OSDP_EVENT_BIOREADR:
		reply_code = REPLY_BIOREADR;
		break;
	case OSDP_EVENT_BIOMATCHR:
		reply_code = REPLY_BIOMATCHR;
		break;
	case OSDP_EVENT_PIVDATAR:
		reply_code = REPLY_PIVDATAR;
		break;
	case OSDP_EVENT_GENAUTHR:
		reply_code = REPLY_GENAUTHR;
		break;
	case OSDP_EVENT_CRAUTHR:
		reply_code = REPLY_CRAUTHR;
		break;
	case OSDP_EVENT_TRS:
		/* Deferred TRS reply: a card-data/APDU response the app
		 * submitted after its CMD_XWR callback returned. Delivered as an
		 * unsolicited REPLY_XRD on this poll. */
		reply_code = REPLY_XRD;
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

/**
 * The inverse of pd_translate_event(): the event type that must back a reply
 * which carries application data. Returns -1 for replies built purely from PD
 * state, which need no event at all.
 */
/*
 * Poll-driven continuation fragments of a multi-part engine reply are built
 * from the engine's context alone and carry no backing event; the backing-
 * event guard in pd_build_reply() must wave them through. One rule for every
 * engine -- PIV reply ids happen to have no backing-event mapping today, but
 * listing them here keeps the exemption explicit rather than accidental.
 */
static bool pd_engine_reply_pending(struct osdp_pd *pd, int reply_id)
{
	if (reply_id == REPLY_BIOREADR && osdp_bio_pd_reply_pending(pd)) {
		return true;
	}
	if (osdp_piv_pd_reply_pending(pd) &&
	    reply_id == osdp_piv_pd_reply_id(pd)) {
		return true;
	}
	return false;
}

static int reply_backing_event_type(int reply_id)
{
	switch (reply_id) {
	case REPLY_OSTATR:
	case REPLY_ISTATR:
	case REPLY_LSTATR:
	case REPLY_RSTATR:
		return OSDP_EVENT_STATUS;
	case REPLY_KEYPAD:
		return OSDP_EVENT_KEYPRESS;
	case REPLY_RAW:
		return OSDP_EVENT_CARDREAD;
	case REPLY_MFGREP:
		return OSDP_EVENT_MFGREP;
	case REPLY_MFGSTATR:
		return OSDP_EVENT_MFGSTATR;
	case REPLY_MFGERRR:
		return OSDP_EVENT_MFGERRR;
	case REPLY_BIOREADR:
		return OSDP_EVENT_BIOREADR;
	case REPLY_BIOMATCHR:
		return OSDP_EVENT_BIOMATCHR;
	default:
		return -1;
	}
}

/**
 * Consume a reply that the app submitted from within its command callback so it
 * rides out as the reply to this command instead of a later poll response.
 *
 * Only the answer to the command in flight is consumed; an unrelated event at
 * the queue head must ride out on a poll, not be eaten here.
 */
static bool pd_take_inline_reply(struct osdp_pd *pd)
{
	const struct osdp_event *event;

	if (pd_event_peek(pd, &event) ||
	    !event_is_reply_to(pd->cmd_id, event)) {
		return false;
	}
	pd_event_dequeue(pd, &event);
	pd->active_event = event;
	pd->reply_id = pd_translate_event(pd, event);
	return true;
}

static bool validate_command(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	bool result = true;

	/*
	 * A control code is a byte off the wire, so a peer can hand us one the
	 * spec does not define. Reject those here rather than passing them to
	 * the app, which has no way to act on a code it cannot name.
	 */
	switch (cmd->id) {
	case OSDP_CMD_OUTPUT:
		if (cmd->output.control_code >= OSDP_CMD_OUTPUT_CC_SENTINEL) {
			result = false;
		}
		break;
	case OSDP_CMD_LED:
		if (cmd->led.temporary.control_code >=
			    OSDP_CMD_LED_TEMPORARY_CC_SENTINEL ||
		    cmd->led.permanent.control_code >=
			    OSDP_CMD_LED_PERMANENT_CC_SENTINEL) {
			result = false;
		}
		/* The ON Time OFF Time values cannot both be set to zero */
		if (cmd->led.temporary.control_code ==
			    OSDP_CMD_LED_TEMPORARY_CC_SET &&
		    cmd->led.temporary.on_count == 0 &&
		    cmd->led.temporary.off_count == 0) {
			result = false;
		}
		if (cmd->led.permanent.control_code ==
			    OSDP_CMD_LED_PERMANENT_CC_SET &&
		    cmd->led.permanent.on_count == 0 &&
		    cmd->led.permanent.off_count == 0) {
			result = false;
		}
		break;
	case OSDP_CMD_BUZZER:
		if (cmd->buzzer.control_code >= OSDP_CMD_BUZZER_CC_SENTINEL) {
			result = false;
		}
		/* ON duration must nonzero unless the buzzer is being silenced */
		if (cmd->buzzer.on_count == 0 &&
		    cmd->buzzer.control_code != OSDP_CMD_BUZZER_CC_OFF) {
			result = false;
		}
		break;
	case OSDP_CMD_TEXT:
		/* Text control codes are 1-indexed; zero is not a valid value */
		if (cmd->text.control_code <
			    OSDP_CMD_TEXT_CC_PERMANENT_NO_WRAP ||
		    cmd->text.control_code >= OSDP_CMD_TEXT_CC_SENTINEL) {
			result = false;
		}
		break;
	case OSDP_CMD_TDSET:
		if (cmd->tdset.month < 1 || cmd->tdset.month > 12 ||
		    cmd->tdset.day < 1 || cmd->tdset.day > 31 ||
		    cmd->tdset.hour > 23 || cmd->tdset.minute > 59 ||
		    cmd->tdset.second > 59) {
			result = false;
		}
		break;
	default:
		break;
	}

	/*
	 * A command may target a sub-reader (0 is this reader, 1 the first
	 * attached reader, and so on). Reject a CP that addresses a reader
	 * beyond what we advertised via the READERS capability.
	 */
	if (osdp_cmd_reader_no(cmd) > pd->cap[OSDP_PD_CAP_READERS].num_items) {
		result = false;
	}

	if (!result) {
		LOG_ERR("Command validation failed!");
	}
	return result;
}

/**
 * The app selects the NAK code by returning its negated value. Only the codes
 * an app can know are honoured; the rest describe protocol faults that only
 * LibOSDP can detect and are set by the phy/SC layers themselves.
 */
static enum osdp_pd_nak_code_e pd_nak_code_from_callback(struct osdp_pd *pd,
							 int ret)
{
	int code = -ret;

	switch (code) {
	case OSDP_PD_NAK_BIO_TYPE:
	case OSDP_PD_NAK_BIO_FMT:
	case OSDP_PD_NAK_RECORD:
		return (enum osdp_pd_nak_code_e)code;
	}

	/* A plain -1 is the idiomatic "reject this command"; don't nag about it
	 * even though it collides with OSDP_PD_NAK_MSG_CHK. */
	if (code > OSDP_PD_NAK_MSG_CHK && code < OSDP_PD_NAK_SENTINEL) {
		LOG_WRN("App returned invalid NAK code %d replying with "
			"RECORD(%d)", code, OSDP_PD_NAK_RECORD);
	}
	return OSDP_PD_NAK_RECORD;
}

static bool do_command_callback(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	int ret = -1;

	if (validate_command(pd, cmd) && pd->command_callback) {
		ret = pd->command_callback(pd->command_callback_arg, cmd);
	}
	if (ret != 0) {
		pd->reply_id = REPLY_NAK;
		pd->nak_code = pd_nak_code_from_callback(pd, ret);
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
	case CMD_TDSET:
		cap = &pd->cap[OSDP_PD_CAP_TIME_KEEPING];
		if (cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_BIOREAD:
	case CMD_BIOMATCH:
		cap = &pd->cap[OSDP_PD_CAP_BIOMETRICS];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_PIVDATA:
	case CMD_GENAUTH:
	case CMD_CRAUTH:
		cap = &pd->cap[OSDP_PD_CAP_SMART_CARD_SUPPORT];
		if (cap->compliance_level == 0) {
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
	int len = 0, n, hdr_len, data_off, ret;
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
	if (max_len <= 0) {
		return -1;
	}

	buf[len++] = reply_id;
	max_len -= 1;

	switch (reply_id) {
	case REPLY_OSTATR:
		n = pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL].num_items;
		if (status->nr_entries != n || max_len < n ||
		    n > OSDP_STATUS_REPORT_MAX_LEN) {
			return -1;
		}
		memcpy(buf + len, status->report, n);
		len += n;
		break;
	case REPLY_ISTATR:
		n = pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING].num_items;
		if (status->nr_entries != n || max_len < n ||
		    n > OSDP_STATUS_REPORT_MAX_LEN) {
			return -1;
		}
		memcpy(buf + len, status->report, n);
		len += n;
		break;
	case REPLY_LSTATR:
		if (status->nr_entries < 2 || max_len < REPLY_LSTATR_DATA_LEN) {
			return -1;
		}
		buf[len++] = status->report[0];
		buf[len++] = status->report[1];
		break;
	case REPLY_RSTATR:
		n = pd->cap[OSDP_PD_CAP_READERS].num_items;
		if (status->nr_entries != n || max_len < n ||
		    n > OSDP_STATUS_REPORT_MAX_LEN) {
			return -1;
		}
		memcpy(buf + len, status->report, n);
		len += n;
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
				    const struct osdp_notification *n)
{
	struct osdp_cmd cmd = { 0 };

	if (!pd->command_callback || !is_notifications_enabled(pd)) {
		return;
	}
	cmd.id = OSDP_CMD_NOTIFICATION;
	cmd.notif = *n;
	(void)pd->command_callback(pd->command_callback_arg, &cmd);
	osdp_metrics_report(pd, OSDP_METRIC_EVENT);
}

static void notify_pd_status(struct osdp_pd *pd, bool is_online)
{
	struct osdp_notification n = { .type = OSDP_NOTIFICATION_PD_STATUS };

	n.pd_status.online = is_online;
	pd_deliver_notification(pd, &n);
}

static void notify_sc_status(struct osdp_pd *pd)
{
	struct osdp_notification n = { .type = OSDP_NOTIFICATION_SC_STATUS };

	n.sc_status.active = sc_is_active(pd);
	n.sc_status.scbk_d = sc_use_scbkd(pd);
	pd_deliver_notification(pd, &n);
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

	/* Consume the command ID byte; buf, pos and len now all describe the
	 * data block that follows it. */
	pd->cmd_id = cmd.id = buf[0];
	buf += 1;
	len -= 1;

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
	case CMD_POLL: {
		const struct osdp_event *queued_event;

		if (len != 0) {
			break;
		}
		/* A multi-part smartcard reply in flight continues before any
		 * queued event; the CP is polling to pull its fragments. */
		if (osdp_piv_pd_reply_pending(pd)) {
			pd->reply_id = osdp_piv_pd_reply_id(pd);
			ret = OSDP_PD_ERR_NONE;
			break;
		}
		/* A multi-part biometric read reply in flight likewise continues
		 * on poll cycles until the whole template is out. */
		if (osdp_bio_pd_reply_pending(pd)) {
			pd->reply_id = REPLY_BIOREADR;
			ret = OSDP_PD_ERR_NONE;
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
		if (len != 0) {
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
		if (len != 0) {
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
		if (len != 0) {
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
		if (len != 0) {
			break;
		}
		cmd.id = OSDP_CMD_STATUS;
		cmd.status.type = OSDP_STATUS_REPORT_READER;
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
		pos++; /* Skip reply type info. */
		pd->reply_id = REPLY_PDID;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_CAP:
		if (len != CMD_CAP_DATA_LEN) {
			break;
		}
		pos++; /* Skip reply type info. */
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
			/* The app may report the resulting output status from
			 * within its callback; if it didn't, ACK and let it
			 * send an osdp_OSTATR later, on a poll. */
			if (!pd_take_inline_reply(pd)) {
				pd->reply_id = REPLY_ACK;
			}
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
	case CMD_TDSET:
		if (len != CMD_TDSET_DATA_LEN) {
			break;
		}
		cmd.id = OSDP_CMD_TDSET;
		cmd.tdset.year = bread_u16_le(buf, &pos);
		cmd.tdset.month = buf[pos++];
		cmd.tdset.day = buf[pos++];
		cmd.tdset.hour = buf[pos++];
		cmd.tdset.minute = buf[pos++];
		cmd.tdset.second = buf[pos++];
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
		memcpy(cmd.mfg.data, buf + pos, cmd.mfg.length);
		pos += cmd.mfg.length;

		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		/* App deferred: ACK. The reply rides out on a later poll once
		 * the app submits it. */
		if (!pd_take_inline_reply(pd)) {
			pd->reply_id = REPLY_ACK;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_BIOREAD:
		if (len != CMD_BIOREAD_DATA_LEN) {
			break;
		}
		if (!pd_cmd_cap_ok(pd, NULL)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		cmd.id = OSDP_CMD_BIOREAD;
		cmd.bioread.reader = buf[pos++];
		cmd.bioread.type = buf[pos++];
		cmd.bioread.format = buf[pos++];
		cmd.bioread.quality = buf[pos++];

		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		if (!pd_take_inline_reply(pd)) {
			pd->reply_id = REPLY_ACK;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_BIOMATCH:
		if (len < CMD_BIOMATCH_DATA_LEN) {
			break;
		}
		if (!pd_cmd_cap_ok(pd, NULL)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		cmd.id = OSDP_CMD_BIOMATCH;
		cmd.biomatch.reader = buf[pos++];
		cmd.biomatch.type = buf[pos++];
		cmd.biomatch.format = buf[pos++];
		cmd.biomatch.quality = buf[pos++];
		cmd.biomatch.length = bread_u16_le(buf, &pos);
		if (cmd.biomatch.length != len - CMD_BIOMATCH_DATA_LEN ||
		    cmd.biomatch.length > OSDP_CMD_BIOMATCH_MAX_TEMPLATE_LEN) {
			LOG_ERR("BIOMATCH template length error (%d)",
				cmd.biomatch.length);
			pd->nak_code = OSDP_PD_NAK_CMD_LEN;
			break;
		}
		memcpy(cmd.biomatch.data, buf + pos, cmd.biomatch.length);

		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		if (!pd_take_inline_reply(pd)) {
			pd->reply_id = REPLY_ACK;
		}
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
		if (len != 0) {
			break;
		}
		osdp_engines_abort(pd);
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
	case CMD_PIVDATA:
		if (len != CMD_PIVDATA_DATA_LEN) {
			break;
		}
		if (!pd_cmd_cap_ok(pd, NULL)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		cmd.id = OSDP_CMD_PIVDATA;
		cmd.pivdata.oid[0] = buf[pos++];
		cmd.pivdata.oid[1] = buf[pos++];
		cmd.pivdata.oid[2] = buf[pos++];
		cmd.pivdata.element = buf[pos++];
		cmd.pivdata.offset = buf[pos++];
		if (osdp_piv_pd_open(pd, CMD_PIVDATA, cmd.pivdata.element)) {
			break;
		}
		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		/* App deferred: ACK. The reply rides out on a later poll once
		 * the app submits it. */
		if (!pd_take_inline_reply(pd)) {
			pd->reply_id = REPLY_ACK;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_GENAUTH:
	case CMD_CRAUTH: {
		int frag;

		if (len < OSDP_MP_HDR_SIZE(OSDP_MP_W16)) {
			break;
		}
		if (!pd_cmd_cap_ok(pd, NULL)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		frag = osdp_piv_pd_cmd_frag(pd, pd->cmd_id, buf + pos, len,
					    &cmd);
		if (frag < 0) {
			break;
		}
		if (frag == 0) { /* more fragments to come */
			pd->reply_id = REPLY_ACK;
			ret = OSDP_PD_ERR_NONE;
			break;
		}
		if (!do_command_callback(pd, &cmd)) {
			osdp_piv_abort(pd);
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		if (!pd_take_inline_reply(pd)) {
			pd->reply_id = REPLY_ACK;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case CMD_XWR: {
		const struct osdp_event *trs_event;
		int trs_ret;

		trs_ret = osdp_trs_cmd_decode(pd, &cmd, buf + pos, len);
		if (trs_ret < 0) {
			break; /* NAK */
		}
		if (trs_ret == OSDP_TRS_DECODE_ACK) {
			/* library-handled command (mode set / card terminate):
			 * acknowledge per spec Table 36/42, no app payload */
			pd->reply_id = REPLY_ACK;
			ret = OSDP_PD_ERR_NONE;
			break;
		}
		if (trs_ret == OSDP_TRS_DECODE_MODE_REPORT) {
			/* mode-read (Table 39): reply with the current mode */
			pd->reply_id = REPLY_XRD;
			ret = OSDP_PD_ERR_NONE;
			break;
		}
		/* OSDP_TRS_DECODE_TO_APP: mode-1 command; hand it to the app */
		if (!do_command_callback(pd, &cmd)) {
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		/* Only consume the app's TRS answer; an older non-TRS event at
		 * the queue head must ride out on a poll, not be eaten here. */
		if (pd_event_peek(pd, &trs_event) == 0 &&
		    trs_event->type == OSDP_EVENT_TRS) {
			/* app answered synchronously: reply now (fast card) */
			pd_event_dequeue(pd, &trs_event);
			pd->active_event = trs_event;
			pd->reply_id = REPLY_XRD;
		} else {
			/* app deferred: ACK ("working"). The R-APDU rides out as
			 * a REPLY_XRD on a later poll once the app submits it. */
			pd->reply_id = REPLY_ACK;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	}
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

/**
 * Build the reply in pd->reply_id into buf. The reply ID byte is written here,
 * ahead of the switch, so each case deals only with its data block and max_len
 * is the space that remains for it. Likewise, a reply that must be backed by an
 * event is matched to it here, so the cases can use pd->active_event directly.
 *
 * Returns:
 * +ve: length of the reply (ID byte inclusive)
 * -ve: error
 */
static int pd_build_reply(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int ret = OSDP_PD_ERR_GENERIC;
	int i, len = 0, event_type;
	const struct osdp_event *event = pd->active_event;
	const struct osdp_event_mfgstat *mfgstat;
	int data_off = osdp_phy_packet_get_data_offset(pd, buf);
	uint8_t *smb = osdp_phy_packet_get_smb(pd, buf);

	buf += data_off;
	max_len -= data_off;
	if (max_len <= 0) {
		return OSDP_PD_ERR_GENERIC;
	}

	buf[len++] = pd->reply_id;
	max_len -= 1;

	event_type = reply_backing_event_type(pd->reply_id);
	if (event_type >= 0 && (!event || (int)event->type != event_type) &&
	    !pd_engine_reply_pending(pd, pd->reply_id)) {
		LOG_ERR("REPLY: %s(%02x) has no matching event to build from",
			osdp_reply_name(pd->reply_id), pd->reply_id);
		goto out;
	}

	switch (pd->reply_id) {
	case REPLY_ACK:
		/* ACK is just the reply ID byte; it carries no data */
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PDID:
		if (max_len < REPLY_PDID_DATA_LEN) {
			break;
		}
		bwrite_u24_le(pd->id.vendor_code, buf, &len);
		buf[len++] = pd->id.model;
		buf[len++] = pd->id.version;
		bwrite_u32_le(pd->id.serial_number, buf, &len);
		bwrite_u24_be(pd->id.firmware_version, buf, &len);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PDCAP:
		/* Capability entities are bounds checked as they are emitted */
		for (i = 1; i < OSDP_PD_CAP_SENTINEL; i++) {
			if ((int)pd->cap[i].function_code != i) {
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
		if (event->status.nr_entries != n ||
		    n > OSDP_STATUS_REPORT_MAX_LEN) {
			break;
		}
		if (max_len < n) {
			break;
		}
		memcpy(buf + len, event->status.report, n);
		len += n;
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case REPLY_ISTATR: {
		int n = pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING].num_items;
		if (event->status.nr_entries != n ||
		    n > OSDP_STATUS_REPORT_MAX_LEN) {
			break;
		}
		if (max_len < n) {
			break;
		}
		memcpy(buf + len, event->status.report, n);
		len += n;
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case REPLY_LSTATR:
		if (event->status.nr_entries < 2) {
			break;
		}
		if (max_len < REPLY_LSTATR_DATA_LEN) {
			break;
		}
		buf[len++] = event->status.report[0]; // tamper
		buf[len++] = event->status.report[1]; // power
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RSTATR: {
		int n = pd->cap[OSDP_PD_CAP_READERS].num_items;
		if (event->status.nr_entries != n ||
		    n > OSDP_STATUS_REPORT_MAX_LEN) {
			break;
		}
		if (max_len < n) {
			break;
		}
		memcpy(buf + len, event->status.report, n);
		len += n;
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case REPLY_KEYPAD:
		if (max_len < REPLY_KEYPAD_DATA_LEN + event->keypress.length) {
			break;
		}
		buf[len++] = (uint8_t)event->keypress.reader_no;
		buf[len++] = (uint8_t)event->keypress.length;
		memcpy(buf + len, event->keypress.data, event->keypress.length);
		len += event->keypress.length;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RAW: {
		int len_bytes;

		len_bytes = BITS_TO_BYTES(event->cardread.length);
		if (max_len < REPLY_RAW_DATA_LEN + len_bytes) {
			break;
		}
		buf[len++] = (uint8_t)event->cardread.reader_no;
		buf[len++] = (uint8_t)event->cardread.format;
		bwrite_u16_le(event->cardread.length, buf, &len);
		memcpy(buf + len, event->cardread.data, len_bytes);
		len += len_bytes;
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case REPLY_COM:
		if (max_len < REPLY_COM_DATA_LEN) {
			break;
		}
		/**
		 * If COMSET succeeds, the PD must reply with the old params and
		 * then switch to the new params from then then on. We cache the
		 * pending values in pd->comset_pending while decoding CMD_COMSET
		 * and use them here in REPLY_COM.
		 */
		buf[len++] = pd->comset_pending.address;
		bwrite_u32_le(pd->comset_pending.baud_rate, buf, &len);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_NAK:
		if (max_len < REPLY_NAK_DATA_LEN) {
			break;
		}
		buf[len++] = pd->nak_code;
		osdp_metrics_report(pd, OSDP_METRIC_NAK);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_MFGREP:
		if (max_len < REPLY_MFGREP_DATA_LEN + event->mfgrep.length) {
			break;
		}
		bwrite_u24_le(event->mfgrep.vendor_code, buf, &len);
		memcpy(buf + len, event->mfgrep.data, event->mfgrep.length);
		len += event->mfgrep.length;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PIVDATAR:
	case REPLY_GENAUTHR:
	case REPLY_CRAUTHR: {
		int n;

		/* The backing event seeds the reply leg on its first
		 * fragment; continuation fragments (poll-driven) build from
		 * the smartcard context alone. */
		n = osdp_piv_pd_reply_build(
			pd,
			(event && TO_PIV(pd) &&
			 (int)event->type == TO_PIV(pd)->event_type) ? event :
								       NULL,
			buf + len, max_len);
		if (n <= 0) {
			break;
		}
		len += n;
		ret = OSDP_PD_ERR_NONE;
		break;
	}
	case REPLY_MFGSTATR:
	case REPLY_MFGERRR:
		mfgstat = (event->type == OSDP_EVENT_MFGSTATR) ?
				  &event->mfgstatr :
				  &event->mfgerrr;
		if (max_len < mfgstat->length) {
			break;
		}
		memcpy(buf + len, mfgstat->data, mfgstat->length);
		len += mfgstat->length;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_BIOREADR:
		if (is_bioreadr_multipart(pd)) {
			/* The backing event seeds the transfer on its first
			 * fragment; poll-driven continuations build from the bio
			 * context alone. */
			int n = osdp_bio_pd_reply_build(
				pd,
				(event &&
				 (int)event->type == OSDP_EVENT_BIOREADR) ?
					event : NULL,
				buf + len, max_len);
			if (n < 0) {
				break;
			}
			len += n;
			ret = OSDP_PD_ERR_NONE;
			break;
		}
		if (event->bioreadr.length > OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN) {
			LOG_ERR("BIOREADR template too long (%d)",
				event->bioreadr.length);
			break;
		}
		if (max_len < REPLY_BIOREADR_DATA_LEN + event->bioreadr.length) {
			break;
		}
		buf[len++] = event->bioreadr.reader;
		buf[len++] = event->bioreadr.status;
		buf[len++] = event->bioreadr.type;
		buf[len++] = event->bioreadr.quality;
		bwrite_u16_le(event->bioreadr.length, buf, &len);
		memcpy(buf + len, event->bioreadr.data, event->bioreadr.length);
		len += event->bioreadr.length;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_BIOMATCHR:
		if (max_len < REPLY_BIOMATCHR_DATA_LEN) {
			break;
		}
		buf[len++] = event->biomatchr.reader;
		buf[len++] = event->biomatchr.status;
		buf[len++] = event->biomatchr.score;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_FTSTAT:
		ret = osdp_file_cmd_stat_build(pd, buf + len, max_len);
		if (ret <= 0) {
			break;
		}
		len += ret;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_XRD:
		ret = osdp_trs_reply_build(pd, buf + len, max_len - len);
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
		if (max_len < REPLY_CCRYPT_DATA_LEN) {
			break;
		}
		osdp_fill_random(pd->sc.pd_random, 8);
		osdp_compute_session_keys(pd);
		osdp_compute_pd_cryptogram(pd);
		memcpy(buf + len, pd->sc.pd_client_uid, 8);
		memcpy(buf + len + 8, pd->sc.pd_random, 8);
		memcpy(buf + len + 16, pd->sc.pd_cryptogram, 16);
		len += 32;
		smb[0] = 3; /* length */
		smb[1] = SCS_12; /* type */
		smb[2] = sc_use_scbkd(pd) ? 0 : 1;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RMAC_I:
		if (smb == NULL) {
			break;
		}
		if (max_len < REPLY_RMAC_I_DATA_LEN) {
			break;
		}
		osdp_compute_rmac_i(pd);
		memcpy(buf + len, pd->sc.r_mac, 16);
		len += 16;
		smb[0] = 3; /* length */
		smb[1] = SCS_14; /* type */
		smb[2] = 1; /* CP auth succeeded */
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
	default:
		BUG();
	}

out:
	if (ret != 0) {
		/* catch all errors and report it as a RECORD error to CP */
		LOG_ERR("Failed to build REPLY: %s(%02x); Sending NAK instead!",
			osdp_reply_name(pd->reply_id), pd->reply_id);
		if (max_len < REPLY_NAK_DATA_LEN) {
			LOG_ERR("Out of buffer space to build NAK; have:%d",
				max_len);
			return OSDP_PD_ERR_GENERIC;
		}
		buf[0] = REPLY_NAK;
		buf[1] = OSDP_PD_NAK_RECORD;
		len = 2;
		osdp_metrics_report(pd, OSDP_METRIC_NAK);
	}

	/**
	 * Pick the SCS type only after the reply is final: a failed build is
	 * rewritten as a NAK above, and that carries data bytes even when the
	 * reply it replaced did not.
	 */
	if (smb && (smb[1] > SCS_14) && sc_is_active(pd)) {
		smb[0] = 2; /* length */
		smb[1] = (len > 1) ? SCS_18 : SCS_16;
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
		osdp_engines_abort(pd);
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
			pd->baud_rate = pd->comset_pending.baud_rate;
			LOG_INF("COMSET Succeeded! New PD-Addr: %d; Baud: %" PRIu32,
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
	if (flags & OSDP_FLAG_BIOREADR_MULTIPART) {
		SET_FLAG(pd, PD_FLAG_BIOREADR_MP);
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
	safe_free(pd->piv);
	safe_free(pd->bio);
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

/**
 * Number of status entries a report of the given type carries; one byte per
 * tracked entity. Input and output status report one byte per monitored contact
 * or controlled output (from the PD's capabilities); local status is always the
 * two bytes tamper and power; reader status reports one byte per attached reader
 * (from OSDP_PD_CAP_READERS). Returns -1 for an unknown status type.
 */
static int pd_status_report_capacity(struct osdp_pd *pd,
				     enum osdp_status_report_type type)
{
	switch (type) {
	case OSDP_STATUS_REPORT_INPUT:
		return pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING].num_items;
	case OSDP_STATUS_REPORT_OUTPUT:
		return pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL].num_items;
	case OSDP_STATUS_REPORT_LOCAL:
		return 2; /* tamper, power */
	case OSDP_STATUS_REPORT_READER:
		return pd->cap[OSDP_PD_CAP_READERS].num_items;
	default:
		return -1;
	}
}

int osdp_pd_submit_event(osdp_t *ctx, const struct osdp_event *event)
{
	input_check(ctx);
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);

	if (event->type <= 0 || event->type >= OSDP_EVENT_SENTINEL) {
		return -1;
	}

	if (event->type == OSDP_EVENT_STATUS) {
		int cap = pd_status_report_capacity(pd, event->status.type);
		/* A status report must be full: one entry per tracked entity,
		 * no partial reports. */
		if (cap < 0 || event->status.nr_entries != cap) {
			LOG_ERR("Event: status report has %d entries; PD "
				"requires exactly %d for this status type",
				event->status.nr_entries, cap);
			return -1;
		}
	}

	/*
	 * An event that names a sub-reader (0 is this reader, 1 the first
	 * attached reader, ...) must fit within the READERS capability we
	 * advertised; reject a report for a reader we do not support.
	 */
	if (osdp_event_reader_no(event) >
	    pd->cap[OSDP_PD_CAP_READERS].num_items) {
		LOG_ERR("Event targets reader %d but PD advertises only %d",
			osdp_event_reader_no(event),
			pd->cap[OSDP_PD_CAP_READERS].num_items);
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
