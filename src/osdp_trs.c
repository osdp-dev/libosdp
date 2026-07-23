/*
 * Copyright (c) 2022 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file OSDP Transparent Reader Support (TRS)
 *
 * TRS tunnels raw smart-card APDUs from the CP, through a PD acting as a
 * transparent pipe, to a contact/contactless card in the reader. It rides on
 * the reserved OSDP CMD_XWR (0xA1) / REPLY_XRD (0xB1) command-reply pair.
 *
 * Every TRS payload begins with a 2-byte header: `mode` (0 = mode handshake,
 * 1 = card session) followed by a per-mode command/reply `code`. Mode-0
 * commands (mode-get / mode-set) drive the session lifecycle and are handled
 * internally by the library. Mode-1 commands (send-APDU, enter-PIN, card-scan,
 * terminate) carry application payloads:
 *
 *   CP  --CMD_XWR(mode-set 01)-->  PD      (osdp_trs_cmd_build / _cmd_decode)
 *   CP  <--REPLY_ACK-------------- PD      (mode accepted)
 *   CP  --CMD_XWR(send-apdu)----->  PD  -> app callback
 *   CP  <--REPLY_XRD(card-data)---- PD  <- app osdp_pd_submit_event()
 *   ... CP delivers OSDP_EVENT_TRS to its app for each REPLY_XRD ...
 */

#include "osdp_trs.h"

#define TO_TRS(pd) (&(pd)->trs)

/* Wire encoding of a transparent-mode command/reply: (mode << 8) | code */
#define MODE_CODE(mode, pcmnd) (uint16_t)(((mode) & 0xff) << 8u | ((pcmnd) & 0xff))

#define CMD_MODE_GET       MODE_CODE(0, 1)
#define CMD_MODE_SET       MODE_CODE(0, 2)
#define CMD_SEND_APDU      MODE_CODE(1, 1)
#define CMD_TERMINATE      MODE_CODE(1, 2)
#define CMD_ENTER_PIN      MODE_CODE(1, 3)
#define CMD_CARD_SCAN      MODE_CODE(1, 4)

#define REPLY_ERROR_MODE0         MODE_CODE(0, 0)
#define REPLY_CURRENT_MODE        MODE_CODE(0, 1)
#define REPLY_CARD_INFO_REPORT    MODE_CODE(0, 2)
#define REPLY_ERROR_MODE1         MODE_CODE(1, 0)
#define REPLY_CARD_PRESENT        MODE_CODE(1, 1)
#define REPLY_CARD_DATA           MODE_CODE(1, 2)
#define REPLY_PIN_ENTRY_COMPLETE  MODE_CODE(1, 3)

/* On-the-wire card-protocol codes (mapped to enum osdp_trs_card_protocol_e) */
#define TRS_WIRE_PROTOCOL_CONTACT_T0T1 0x00
#define TRS_WIRE_PROTOCOL_14443AB      0x01

/* On-the-wire card-present status codes (mapped to enum osdp_trs_card_status_e) */
#define TRS_WIRE_CARD_NOT_PRESENT	  0x00
#define TRS_WIRE_CARD_PRESENT_UNSPECIFIED 0x01
#define TRS_WIRE_CARD_PRESENT_CONTACTLESS 0x02
#define TRS_WIRE_CARD_PRESENT_CONTACT	  0x03

static int trs_card_status_from_wire(uint8_t wire,
				     enum osdp_trs_card_status_e *status)
{
	switch (wire) {
	case TRS_WIRE_CARD_NOT_PRESENT:
		*status = OSDP_TRS_CARD_NOT_PRESENT;
		return 0;
	case TRS_WIRE_CARD_PRESENT_UNSPECIFIED:
		*status = OSDP_TRS_CARD_PRESENT;
		return 0;
	case TRS_WIRE_CARD_PRESENT_CONTACTLESS:
		*status = OSDP_TRS_CARD_PRESENT_CONTACTLESS;
		return 0;
	case TRS_WIRE_CARD_PRESENT_CONTACT:
		*status = OSDP_TRS_CARD_PRESENT_CONTACT;
		return 0;
	default:
		return -1;
	}
}

static int trs_card_status_to_wire(enum osdp_trs_card_status_e status,
				   uint8_t *wire)
{
	switch (status) {
	case OSDP_TRS_CARD_NOT_PRESENT:
		*wire = TRS_WIRE_CARD_NOT_PRESENT;
		return 0;
	case OSDP_TRS_CARD_PRESENT:
		*wire = TRS_WIRE_CARD_PRESENT_UNSPECIFIED;
		return 0;
	case OSDP_TRS_CARD_PRESENT_CONTACTLESS:
		*wire = TRS_WIRE_CARD_PRESENT_CONTACTLESS;
		return 0;
	case OSDP_TRS_CARD_PRESENT_CONTACT:
		*wire = TRS_WIRE_CARD_PRESENT_CONTACT;
		return 0;
	default:
		return -1;
	}
}

/* On-the-wire PIN-format codes in bmFormatString bits 1-0 (mapped to
 * enum osdp_trs_pin_format_e); 0x03 is reserved */
#define TRS_WIRE_PIN_FORMAT_BINARY 0x00
#define TRS_WIRE_PIN_FORMAT_BCD	   0x01
#define TRS_WIRE_PIN_FORMAT_ASCII  0x02

static int trs_pin_format_from_wire(uint8_t wire,
				    enum osdp_trs_pin_format_e *format)
{
	switch (wire) {
	case TRS_WIRE_PIN_FORMAT_BINARY:
		*format = OSDP_TRS_PIN_FORMAT_BINARY;
		return 0;
	case TRS_WIRE_PIN_FORMAT_BCD:
		*format = OSDP_TRS_PIN_FORMAT_BCD;
		return 0;
	case TRS_WIRE_PIN_FORMAT_ASCII:
		*format = OSDP_TRS_PIN_FORMAT_ASCII;
		return 0;
	default:
		return -1;
	}
}

static int trs_pin_format_to_wire(enum osdp_trs_pin_format_e format,
				  uint8_t *wire)
{
	switch (format) {
	case OSDP_TRS_PIN_FORMAT_BINARY:
		*wire = TRS_WIRE_PIN_FORMAT_BINARY;
		return 0;
	case OSDP_TRS_PIN_FORMAT_BCD:
		*wire = TRS_WIRE_PIN_FORMAT_BCD;
		return 0;
	case OSDP_TRS_PIN_FORMAT_ASCII:
		*wire = TRS_WIRE_PIN_FORMAT_ASCII;
		return 0;
	default:
		return -1;
	}
}

/*
 * The wire carries APDU positions as a 4-bit offset plus a bits/bytes unit
 * flag; the API expresses them plainly in bits. A position is representable
 * only if it is byte-aligned (up to 15 bytes) or fits the 4-bit offset as-is
 * (up to 15 bits).
 */
static int trs_pin_pos_to_wire(uint16_t pos_bits, uint8_t *offset,
			       bool *unit_bytes)
{
	if (pos_bits % 8 == 0 && pos_bits / 8 <= 15) {
		*offset = (uint8_t)(pos_bits / 8);
		*unit_bytes = true;
		return 0;
	}
	if (pos_bits <= 15) {
		*offset = (uint8_t)pos_bits;
		*unit_bytes = false;
		return 0;
	}
	return -1;
}

static int trs_pin_pos_from_wire(uint8_t offset, bool unit_bytes)
{
	return offset * (unit_bytes ? 8 : 1);
}

/* Validate the (mode, code) pair against the transparent-mode command space */
static bool trs_mode_code_valid(uint8_t mode, uint8_t code)
{
	return !(code == 0 || (mode != 0 && mode != 1) ||
		 (mode == 0 && code > 2) || (mode == 1 && code > 4));
}

/* --- CP: build outgoing CMD_XWR from an app command --- */

/*
 * Serialize the library-driven CMD_XWR for the current TRS session state: the
 * transparent-mode handshake (mode-set) and card-session teardown (terminate)
 * steps the CP sequences on its own. These never pass through a public
 * struct osdp_trs_cmd, so their wire form is emitted straight into the buffer.
 */
static int trs_local_cmd_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int len = 0;

	switch (pd->trs.state) {
	case TRS_STATE_SET_MODE:
	case TRS_STATE_TEARDOWN:
		if (max_len < 4) {
			return -1;
		}
		bwrite_u16_be(CMD_MODE_SET, buf, &len);
		buf[len++] = (pd->trs.state == TRS_STATE_SET_MODE) ?
				     TRS_MODE_01 : TRS_MODE_00;
		buf[len++] = TRS_DISABLE_CARD_INFO_REPORT;
		break;
	case TRS_STATE_DISCONNECT_CARD:
		if (max_len < 3) {
			return -1;
		}
		bwrite_u16_be(CMD_TERMINATE, buf, &len);
		buf[len++] = 0; /* reader (always 0) */
		break;
	default:
		return -1;
	}
	return len;
}

/* Map an app-facing TRS command to its wire (mode << 8 | code) encoding */
static int trs_cmd_to_wire(enum osdp_trs_cmd_e command, uint16_t *mode_code)
{
	switch (command) {
	case OSDP_TRS_CMD_SEND_APDU:
		*mode_code = CMD_SEND_APDU;
		return 0;
	case OSDP_TRS_CMD_ENTER_PIN:
		*mode_code = CMD_ENTER_PIN;
		return 0;
	case OSDP_TRS_CMD_CARD_SCAN:
		*mode_code = CMD_CARD_SCAN;
		return 0;
	default:
		return -1;
	}
}

int osdp_trs_max_apdu_len(struct osdp_pd *pd)
{
	/*
	 * Worst-case wire overhead around the APDU: mark byte, packet header,
	 * security control block, command id, TRS mode/code and reader bytes,
	 * AES block padding, MAC and CRC. Deliberately conservative -- what
	 * is admitted against this must always fit when the frame is built.
	 */
	const int overhead = 40;
	int max_len = get_tx_buf_size(pd) - overhead;

	if (max_len > OSDP_TRS_APDU_MAX_LEN) {
		max_len = OSDP_TRS_APDU_MAX_LEN;
	}
	return max_len;
}

int osdp_trs_cmd_build(struct osdp_pd *pd, const struct osdp_cmd *cmd,
		       uint8_t *buf, int max_len)
{
	const struct osdp_trs_cmd *c;
	int len = 0;
	uint32_t apdu_length;
	uint16_t mode_code;
	uint8_t byte;

	/* No app payload: this is one of the mode-set / terminate steps the CP
	 * sequences for itself, built from the session state instead. */
	if (cmd == NULL) {
		return trs_local_cmd_build(pd, buf, max_len);
	}

	c = &cmd->trs;
	if (trs_cmd_to_wire(c->command, &mode_code)) {
		return -1;
	}

	/* App commands are all mode-1: 2-byte header + a reader byte */
	if (max_len < 3) {
		return -1;
	}
	bwrite_u16_be(mode_code, buf, &len);
	buf[len++] = 0; /* reader (always 0) */

	switch (c->command) {
	case OSDP_TRS_CMD_SEND_APDU:
		apdu_length = c->apdu.length;
		if (apdu_length > sizeof(c->apdu.data) ||
		    apdu_length > (uint32_t)(max_len - len)) {
			LOG_ERR("TRS: APDU length invalid! need/have: %d/%d",
				(int)apdu_length, (max_len - len));
			return -1;
		}
		memcpy(buf + len, c->apdu.data, apdu_length);
		len += apdu_length;
		break;
	case OSDP_TRS_CMD_ENTER_PIN: {
		const struct osdp_trs_pin_entry *pe = &c->pin_entry;
		bool unit_bytes;
		uint8_t offset;

		if (max_len - len < 17 ||
		    trs_pin_format_to_wire(pe->pin_block.format, &byte)) {
			return -1;
		}
		buf[len++] = pe->timeout_initial;
		buf[len++] = pe->timeout_digit;
		/* bmFormatString: [7]=offset unit is bytes, [6:3]=PIN offset,
		 * [2]=right justify, [1:0]=PIN format */
		if (trs_pin_pos_to_wire(pe->pin_block.offset_bits, &offset,
					&unit_bytes)) {
			LOG_ERR("TRS: PIN offset %u not wire-representable",
				pe->pin_block.offset_bits);
			return -1;
		}
		byte |= pe->pin_block.right_justify ? BIT(2) : 0;
		byte |= (uint8_t)(offset << 3);
		byte |= unit_bytes ? BIT(7) : 0;
		buf[len++] = byte;
		/* bmPINBlockString: [7:4]=PIN-length field size (bits),
		 * [3:0]=PIN block size (bytes). Both are 4-bit wire nibbles;
		 * reject an out-of-range value rather than masking it down to
		 * something the reader would silently act on. */
		if (pe->pin_length_field.size_bits > 0x0f ||
		    pe->pin_block.size_bytes > 0x0f) {
			LOG_ERR("TRS: PIN length/block size not wire-representable (%u/%u)",
				pe->pin_length_field.size_bits,
				pe->pin_block.size_bytes);
			return -1;
		}
		buf[len++] = (uint8_t)(pe->pin_length_field.size_bits << 4 |
				       pe->pin_block.size_bytes);
		/* bmPINLengthFormat: [4]=offset unit is bytes,
		 * [3:0]=PIN-length field offset */
		if (trs_pin_pos_to_wire(pe->pin_length_field.offset_bits,
					&offset, &unit_bytes)) {
			LOG_ERR("TRS: PIN-length offset %u not wire-representable",
				pe->pin_length_field.offset_bits);
			return -1;
		}
		byte = offset;
		byte |= unit_bytes ? BIT(4) : 0;
		buf[len++] = byte;
		buf[len++] = pe->min_digits;
		buf[len++] = pe->max_digits;

		/* bEntryValidationCondition:
		 * [0]=max digits reached,
		 * [1]=validation key pressed
		 * [2]=timeout */
		byte = 0;
		byte |= (pe->complete_on & OSDP_TRS_PIN_COMPLETE_ON_MAX_DIGITS) ? BIT(0) : 0;
		byte |= (pe->complete_on & OSDP_TRS_PIN_COMPLETE_ON_KEY) ? BIT(1) : 0;
		byte |= (pe->complete_on & OSDP_TRS_PIN_COMPLETE_ON_TIMEOUT) ? BIT(2) : 0;
		buf[len++] = byte;

		buf[len++] = pe->num_messages;
		bwrite_u16_be(pe->language_id, buf, &len);
		buf[len++] = pe->msg_index;
		buf[len++] = pe->teo_prologue[0];
		buf[len++] = pe->teo_prologue[1];
		buf[len++] = pe->teo_prologue[2];

		apdu_length = pe->apdu.length;
		/* Validate before emitting: the length field is written ahead
		 * of the payload, so a rejected APDU must not leave a stale
		 * length behind it. */
		if (apdu_length > sizeof(pe->apdu.data) ||
		    apdu_length > (uint32_t)(max_len - len - 2)) {
			LOG_ERR("TRS: PIN APDU length invalid! need/have: %d/%d",
				(int)apdu_length, (max_len - len - 2));
			return -1;
		}
		bwrite_u16_be((uint16_t)apdu_length, buf, &len);
		memcpy(buf + len, pe->apdu.data, apdu_length);
		len += apdu_length;
		break;
	}
	case OSDP_TRS_CMD_CARD_SCAN:
		break;
	default:
		return -1;
	}
	return len;
}

/* --- CP: decode incoming REPLY_XRD into an app event --- */

/*
 * A decoded card sighting feeds the presence scan: during a probe it starts
 * (or, on a card-gone report, releases) the mode-1 hold that gives the app
 * time to open a band; outside one it restarts the mode-0 dwell so the scan
 * stays out of the way of whatever the sighting kicks off.
 */
static void trs_scan_note_card(struct osdp_pd *pd, bool present)
{
	struct osdp_trs *trs = &pd->trs;

	if (!trs->scan.enabled) {
		return;
	}
	if (!trs->probe) {
		osdp_trs_scan_note_activity(pd);
		return;
	}
	if (!present) {
		trs->scan.holding = false; /* card left; let the probe close */
		return;
	}
	trs->scan.holding = true;
	trs->scan.hold_tstamp = osdp_millis_now();
	trs->scan.backoff_ms = 0;
}

/*
 * Decode a REPLY_XRD payload into *event. Returns <0 on error, else one of
 * enum osdp_trs_reply_action_e telling osdp_cp.c whether the reply carried
 * anything for the app: the mode handshake is the library's own business, but
 * card data, card status and reader errors are dispatched as OSDP_EVENT_TRS.
 * An error report is dispatched and also fails the command it answered.
 */
int osdp_trs_reply_decode(struct osdp_pd *pd, uint8_t *buf, int len,
			  struct osdp_event *event)
{
	uint8_t card_protocol, card_status, csn_len, prot_data_len;
	uint16_t mode_code;
	int pos = 0, data_len, apdu_len;
	bool dispatch = true, is_error = false;

	if (len < 2) {
		return -1;
	}

	memset(event, 0, sizeof(*event));
	event->type = OSDP_EVENT_TRS;

	mode_code = bread_u16_be(buf, &pos);
	data_len = len - pos;

	switch (mode_code) {
	case REPLY_CURRENT_MODE:
		if (data_len < 1) {
			return -1;
		}
		/* internal handshake reply: record mode, don't notify app */
		pd->trs.mode = buf[pos++];
		dispatch = false;
		break;
	case REPLY_CARD_INFO_REPORT:
		event->trs.reply = OSDP_TRS_REPLY_CARD_INFO;
		if (data_len < 4) {
			return -1;
		}
		event->trs.card_info.reader = buf[pos++];
		card_protocol = buf[pos++];
		if (card_protocol == TRS_WIRE_PROTOCOL_CONTACT_T0T1) {
			event->trs.card_info.protocol =
				OSDP_TRS_CARD_PROTOCOL_CONTACT;
		} else if (card_protocol == TRS_WIRE_PROTOCOL_14443AB) {
			event->trs.card_info.protocol =
				OSDP_TRS_CARD_PROTOCOL_CONTACTLESS;
		} else {
			LOG_ERR("TRS: unsupported card protocol %02x",
				card_protocol);
			return -1;
		}
		csn_len = buf[pos++];
		prot_data_len = buf[pos++];
		if (csn_len > OSDP_TRS_CSN_MAX_LEN ||
		    prot_data_len > OSDP_TRS_PROTOCOL_DATA_MAX_LEN) {
			LOG_ERR("TRS: CSN/protocol-data too large (%d/%d)",
				csn_len, prot_data_len);
			return -1;
		}
		if (data_len < 4 + csn_len + prot_data_len) {
			LOG_ERR("TRS: truncated card-info report");
			return -1;
		}
		event->trs.card_info.csn_len = csn_len;
		event->trs.card_info.protocol_data_len = prot_data_len;
		memcpy(event->trs.card_info.csn, buf + pos, csn_len);
		pos += csn_len;
		memcpy(event->trs.card_info.protocol_data, buf + pos,
		       prot_data_len);
		pos += prot_data_len;
		trs_scan_note_card(pd, true);
		break;
	case REPLY_CARD_PRESENT:
		event->trs.reply = OSDP_TRS_REPLY_CARD_PRESENT;
		if (data_len < 1) {
			return -1;
		}
		event->trs.card_present.reader = buf[pos++];
		if (data_len < 2) {
			/* The status byte is optional (v2.2 section 7.26.8);
			 * without it the reply itself is the news that a card
			 * is there, on an unspecified interface. */
			event->trs.card_present.status = OSDP_TRS_CARD_PRESENT;
			trs_scan_note_card(pd, true);
			break;
		}
		card_status = buf[pos++];
		if (trs_card_status_from_wire(card_status,
					      &event->trs.card_present.status)) {
			LOG_ERR("TRS: reserved card-present status %02x",
				card_status);
			return -1;
		}
		trs_scan_note_card(pd, event->trs.card_present.status !=
					       OSDP_TRS_CARD_NOT_PRESENT);
		break;
	case REPLY_ERROR_MODE0:
	case REPLY_ERROR_MODE1:
		event->trs.reply = OSDP_TRS_REPLY_ERROR;
		if (data_len < 1) {
			return -1;
		}
		event->trs.error.code = buf[pos++];
		is_error = true;
		break;
	case REPLY_CARD_DATA:
		event->trs.reply = OSDP_TRS_REPLY_CARD_DATA;
		if (data_len < 2) {
			return -1;
		}
		event->trs.card_data.reader = buf[pos++];
		event->trs.card_data.status = buf[pos++];
		apdu_len = len - pos;
		if (apdu_len > (int)sizeof(event->trs.card_data.apdu.data)) {
			LOG_ERR("TRS: R-APDU too large (%d); rejecting reply",
				apdu_len);
			return -1;
		}
		event->trs.card_data.apdu.length = apdu_len;
		memcpy(event->trs.card_data.apdu.data, buf + pos, apdu_len);
		pos += apdu_len;
		break;
	case REPLY_PIN_ENTRY_COMPLETE:
		event->trs.reply = OSDP_TRS_REPLY_PIN_COMPLETE;
		if (data_len < 3) {
			return -1;
		}
		event->trs.pin_complete.reader = buf[pos++];
		event->trs.pin_complete.status = buf[pos++];
		event->trs.pin_complete.tries = buf[pos++];
		break;
	default:
		LOG_ERR("TRS: unknown reply mode/code %04x", mode_code);
		return -1;
	}

	if (!dispatch) {
		return OSDP_TRS_REPLY_ACTION_NONE;
	}
	return is_error ? OSDP_TRS_REPLY_ACTION_DISPATCH_ERROR :
			  OSDP_TRS_REPLY_ACTION_DISPATCH;
}

/* --- PD: build outgoing REPLY_XRD --- */

/* Map an app-facing TRS reply to its wire (mode << 8 | code) encoding */
static uint16_t trs_reply_to_wire(enum osdp_trs_reply_e reply)
{
	switch (reply) {
	case OSDP_TRS_REPLY_CARD_INFO:
		return REPLY_CARD_INFO_REPORT;
	case OSDP_TRS_REPLY_CARD_PRESENT:
		return REPLY_CARD_PRESENT;
	case OSDP_TRS_REPLY_CARD_DATA:
		return REPLY_CARD_DATA;
	case OSDP_TRS_REPLY_PIN_COMPLETE:
		return REPLY_PIN_ENTRY_COMPLETE;
	case OSDP_TRS_REPLY_ERROR:
		return REPLY_ERROR_MODE1;
	default:
		return 0;
	}
}

int osdp_trs_reply_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int len = 0, csn_len, prot_data_len, apdu_len;
	const struct osdp_event *event = pd->active_event;
	const struct osdp_trs_reply *reply;
	uint16_t mode_code;
	uint8_t card_status;

	if (event == NULL || event->type != OSDP_EVENT_TRS) {
		/* No app payload: answer with the current-mode report (Table 71),
		 * which carries both the mode code and its config byte. */
		if (max_len < 4) {
			return -1;
		}
		bwrite_u16_be(REPLY_CURRENT_MODE, buf, &len);
		buf[len++] = pd->trs.mode;
		buf[len++] = TRS_DISABLE_CARD_INFO_REPORT; /* mode config */
		return len;
	}

	reply = &event->trs;
	mode_code = trs_reply_to_wire(reply->reply);
	if (mode_code == 0 || max_len < 2) {
		return -1;
	}
	bwrite_u16_be(mode_code, buf, &len);

	switch (reply->reply) {
	case OSDP_TRS_REPLY_CARD_INFO:
		csn_len = reply->card_info.csn_len;
		prot_data_len = reply->card_info.protocol_data_len;
		if (csn_len > OSDP_TRS_CSN_MAX_LEN) {
			csn_len = OSDP_TRS_CSN_MAX_LEN;
		}
		if (prot_data_len > OSDP_TRS_PROTOCOL_DATA_MAX_LEN) {
			prot_data_len = OSDP_TRS_PROTOCOL_DATA_MAX_LEN;
		}
		if (max_len - len < 4 + csn_len + prot_data_len) {
			return -1;
		}
		buf[len++] = reply->card_info.reader;
		buf[len++] = (reply->card_info.protocol ==
			      OSDP_TRS_CARD_PROTOCOL_CONTACTLESS) ?
				     TRS_WIRE_PROTOCOL_14443AB :
				     TRS_WIRE_PROTOCOL_CONTACT_T0T1;
		buf[len++] = (uint8_t)csn_len;
		buf[len++] = (uint8_t)prot_data_len;
		memcpy(buf + len, reply->card_info.csn, csn_len);
		len += csn_len;
		memcpy(buf + len, reply->card_info.protocol_data,
		       prot_data_len);
		len += prot_data_len;
		break;
	case OSDP_TRS_REPLY_CARD_PRESENT:
		if (max_len - len < 2 ||
		    trs_card_status_to_wire(reply->card_present.status,
					    &card_status)) {
			return -1;
		}
		buf[len++] = reply->card_present.reader;
		buf[len++] = card_status;
		break;
	case OSDP_TRS_REPLY_ERROR:
		if (max_len - len < 1) {
			return -1;
		}
		buf[len++] = reply->error.code;
		break;
	case OSDP_TRS_REPLY_CARD_DATA:
		apdu_len = reply->card_data.apdu.length;
		if (apdu_len > (int)sizeof(reply->card_data.apdu.data)) {
			LOG_ERR("TRS: R-APDU length invalid (%d)", apdu_len);
			return -1;
		}
		if (max_len - len < 2 + apdu_len) {
			return -1;
		}
		buf[len++] = reply->card_data.reader;
		buf[len++] = reply->card_data.status;
		memcpy(buf + len, reply->card_data.apdu.data, apdu_len);
		len += apdu_len;
		break;
	case OSDP_TRS_REPLY_PIN_COMPLETE:
		if (max_len - len < 3) {
			return -1;
		}
		buf[len++] = reply->pin_complete.reader;
		buf[len++] = reply->pin_complete.status;
		buf[len++] = reply->pin_complete.tries;
		break;
	default:
		return -1;
	}
	return len;
}

/* --- PD: decode incoming CMD_XWR ---
 *
 * Returns <0 on error (NAK), else one of enum osdp_trs_decode_e telling the
 * caller how to answer: ACK a library-handled command (mode set / card
 * terminate), send an XRD mode report (mode read), or deliver the decoded
 * card-session command to the app.
 */
int osdp_trs_cmd_decode(struct osdp_pd *pd, struct osdp_cmd *cmd, uint8_t *buf,
			int len)
{
	int pos = 0, apdu_length;
	uint8_t mode, code, new_mode;
	uint16_t mode_code;

	if (len < 2) {
		return -1;
	}
	mode_code = bread_u16_be(buf, &pos);
	mode = BYTE_1(mode_code);
	code = BYTE_0(mode_code);

	if (!trs_mode_code_valid(mode, code)) {
		return -1;
	}

	/* mode-0 (handshake) commands are handled by the library itself */
	if (mode == 0) {
		if (mode_code == CMD_MODE_SET) {
			/* The config byte is optional (Table 36: absent means
			 * 0x00) and currently unused either way. */
			if (len - pos < 1) {
				return -1;
			}
			new_mode = buf[pos++];
			if (new_mode != TRS_MODE_00 && new_mode != TRS_MODE_01) {
				LOG_ERR("TRS: unsupported mode %02x requested",
					new_mode);
				return -1;
			}
			pd->trs.mode = new_mode;
			return OSDP_TRS_DECODE_ACK; /* Table 36: Mode-Set is ACK'd */
		}
		/* CMD_MODE_GET (Table 39): answer with the current-mode report */
		return OSDP_TRS_DECODE_MODE_REPORT;
	}

	/* Card-session commands are only meaningful once the CP has put us in
	 * transparent mode; refuse them otherwise. */
	if (pd->trs.mode != TRS_MODE_01) {
		LOG_ERR("TRS: mode-1 command received in mode %02x",
			pd->trs.mode);
		return -1;
	}

	/* Card-session teardown is library-driven; ACK it without the app */
	if (mode_code == CMD_TERMINATE) {
		return OSDP_TRS_DECODE_ACK;
	}

	/* mode-1 commands carry a reader byte and are delivered to the app */
	if (len - pos < 1) {
		return -1;
	}
	pos++; /* reader -- always 0 */

	cmd->id = OSDP_CMD_XWR;

	switch (mode_code) {
	case CMD_SEND_APDU:
		cmd->trs.command = OSDP_TRS_CMD_SEND_APDU;
		apdu_length = len - pos;
		if (apdu_length > (int)sizeof(cmd->trs.apdu.data)) {
			LOG_ERR("TRS: C-APDU too large (%d); NAK-ing command",
				apdu_length);
			return -1;
		}
		cmd->trs.apdu.length = apdu_length;
		memcpy(cmd->trs.apdu.data, buf + pos, apdu_length);
		pos += apdu_length;
		break;
	case CMD_ENTER_PIN: {
		struct osdp_trs_pin_entry *pe = &cmd->trs.pin_entry;
		uint8_t byte;

		cmd->trs.command = OSDP_TRS_CMD_ENTER_PIN;
		if (len - pos < 17) {
			return -1;
		}
		pe->timeout_initial = buf[pos++];
		pe->timeout_digit = buf[pos++];
		/* bmFormatString: [7]=offset unit is bytes, [6:3]=PIN offset,
		 * [2]=right justify, [1:0]=PIN format */
		byte = buf[pos++];
		if (trs_pin_format_from_wire(byte & 0x03, &pe->pin_block.format)) {
			LOG_ERR("TRS: reserved PIN format in %02x", byte);
			return -1;
		}
		pe->pin_block.right_justify = byte & BIT(2);
		pe->pin_block.offset_bits =
			trs_pin_pos_from_wire((byte >> 3) & 0x0f, byte & BIT(7));
		/* bmPINBlockString: [7:4]=PIN-length field size (bits),
		 * [3:0]=PIN block size (bytes) */
		byte = buf[pos++];
		pe->pin_length_field.size_bits = (byte >> 4) & 0x0f;
		pe->pin_block.size_bytes = byte & 0x0f;
		/* bmPINLengthFormat: [4]=offset unit is bytes,
		 * [3:0]=PIN-length field offset */
		byte = buf[pos++];
		pe->pin_length_field.offset_bits =
			trs_pin_pos_from_wire(byte & 0x0f, byte & BIT(4));
		pe->min_digits = buf[pos++];
		pe->max_digits = buf[pos++];
		/* bEntryValidationCondition: [0]=max digits reached,
		 * [1]=validation key pressed, [2]=timeout */
		byte = buf[pos++];
		pe->complete_on = 0;
		pe->complete_on |= (byte & BIT(0)) ?
			OSDP_TRS_PIN_COMPLETE_ON_MAX_DIGITS : 0;
		pe->complete_on |= (byte & BIT(1)) ?
			OSDP_TRS_PIN_COMPLETE_ON_KEY : 0;
		pe->complete_on |= (byte & BIT(2)) ?
			OSDP_TRS_PIN_COMPLETE_ON_TIMEOUT : 0;
		pe->num_messages = buf[pos++];
		pe->language_id = bread_u16_be(buf, &pos);
		pe->msg_index = buf[pos++];
		pe->teo_prologue[0] = buf[pos++];
		pe->teo_prologue[1] = buf[pos++];
		pe->teo_prologue[2] = buf[pos++];
		apdu_length = bread_u16_be(buf, &pos);
		pe->apdu.length = apdu_length;
		if (apdu_length > (int)sizeof(pe->apdu.data) ||
		    apdu_length > (len - pos)) {
			LOG_ERR("TRS: PIN APDU length invalid! need/have: %d/%d",
				apdu_length, (len - pos));
			return -1;
		}
		memcpy(pe->apdu.data, buf + pos, apdu_length);
		pos += apdu_length;
		break;
	}
	case CMD_CARD_SCAN:
		cmd->trs.command = OSDP_TRS_CMD_CARD_SCAN;
		/* no additional payload */
		break;
	default:
		return -1;
	}
	return OSDP_TRS_DECODE_TO_APP;
}

/* --- CP: library-driven session state machine --- */

/*
 * Advance the TRS sub-state after the PD accepted a command, and return the next
 * CP role state. The XMIT->DISCONNECT transition is driven by a stop request in
 * cp_get_trs_command(); here we only handle post-reply advances.
 */
enum osdp_cp_state_e osdp_trs_state_update(struct osdp_pd *pd)
{
	switch (pd->trs.state) {
	case TRS_STATE_SET_MODE:
		/*
		 * A PD that entered transparent mode ACKs the mode-set; one that
		 * declined answers with a mode report naming the mode it stayed
		 * in (recorded by osdp_trs_reply_decode). Streaming APDUs to a
		 * PD that is not in mode 01 would just draw errors, so give up
		 * on the session instead.
		 */
		if (pd->reply_id == REPLY_ACK) {
			pd->trs.mode = TRS_MODE_01;
		}
		if (pd->trs.mode != TRS_MODE_01) {
			LOG_ERR("TRS: PD refused transparent mode; aborting");
			pd->trs.state = TRS_STATE_DONE;
			pd->trs.failed = true;
			return OSDP_CP_STATE_ONLINE;
		}
		pd->trs.state = TRS_STATE_XMIT;
		return OSDP_CP_STATE_TRS_RUN;
	case TRS_STATE_XMIT:
		return OSDP_CP_STATE_TRS_RUN;
	case TRS_STATE_DISCONNECT_CARD:
		pd->trs.state = TRS_STATE_TEARDOWN;
		return OSDP_CP_STATE_TRS_RUN;
	case TRS_STATE_TEARDOWN:
		pd->trs.state = TRS_STATE_DONE;
		__fallthrough;
	case TRS_STATE_DONE:
	default:
		return OSDP_CP_STATE_ONLINE;
	}
}

/*
 * A TRS command failed (NAK, or no usable reply). If the PD already entered
 * transparent mode, it must not be left there -- a reader in mode 01 stops
 * reporting ordinary card reads -- so make one attempt to restore mode 00
 * before letting go of the session. If that attempt is what failed, the PD is
 * past helping; hand it back to the online path, which takes it offline if it
 * stays unresponsive.
 */
enum osdp_cp_state_e osdp_trs_state_update_err(struct osdp_pd *pd)
{
	pd->trs.failed = true;

	switch (pd->trs.state) {
	case TRS_STATE_SET_MODE:
		/*
		 * The reader gave no usable ACK, but that does not prove it
		 * stayed in mode 00 -- it may have entered transparent mode
		 * and had its reply lost or mangled, and a reader stuck in
		 * mode 01 stops reporting ordinary card reads. Mode-set(00)
		 * is idempotent, so pay one extra round trip to be sure.
		 */
		__fallthrough;
	case TRS_STATE_XMIT:
	case TRS_STATE_DISCONNECT_CARD:
		LOG_WRN("TRS: session error; restoring transparent mode off");
		pd->trs.state = TRS_STATE_TEARDOWN;
		return OSDP_CP_STATE_TRS_RUN;
	case TRS_STATE_TEARDOWN:
		LOG_ERR("TRS: failed to restore transparent mode off");
		__fallthrough;
	default:
		pd->trs.state = TRS_STATE_DONE;
		return OSDP_CP_STATE_ONLINE;
	}
}

/* --- CP: background presence scan --- */

/*
 * A probe is a library-initiated TRS session with no app band behind it: it
 * enters mode 01, polls for a card sighting for scan.mode1_dwell_ms (longer if
 * one arrives and the hold kicks in), and restores mode 00. The ordinary
 * session machinery drives it on the wire; the pd->trs.probe flag is what
 * keeps it from raising session notifications or flushing anything.
 */

bool osdp_trs_scan_probe_due(struct osdp_pd *pd)
{
	struct osdp_trs *trs = &pd->trs;
	tick_t wait = trs->scan.mode0_dwell_ms;

	if (!trs->scan.enabled || !trs_capable(pd) || trs->band_open) {
		return false;
	}
	if (trs->scan.backoff_ms > wait) {
		wait = trs->scan.backoff_ms;
	}
	return osdp_millis_since(trs->scan.tstamp) > wait;
}

bool osdp_trs_probe_expired(struct osdp_pd *pd)
{
	struct osdp_trs *trs = &pd->trs;

	if (!trs->scan.enabled) {
		return true;
	}
	if (trs->scan.holding) {
		return osdp_millis_since(trs->scan.hold_tstamp) >
		       trs->scan.hold_ms;
	}
	return osdp_millis_since(trs->scan.probe_tstamp) >
	       trs->scan.mode1_dwell_ms;
}

/* The probe's session opened; undo the app-band bookkeeping TRS_SETUP set */
void osdp_trs_probe_note_open(struct osdp_pd *pd)
{
	pd->trs.probe = true;
	pd->trs.stop_pending = false;
	pd->trs.scan.holding = false;
}

/* Mode 01 confirmed; the probe's dwell clock starts now */
void osdp_trs_probe_note_run(struct osdp_pd *pd)
{
	pd->trs.scan.probe_tstamp = osdp_millis_now();
}

/* An app band takes over the probe's transparent session as-is */
void osdp_trs_probe_adopted(struct osdp_pd *pd)
{
	pd->trs.probe = false;
	pd->trs.stop_pending = true;
	pd->trs.scan.holding = false;
}

/*
 * The probe's session is over (back to ONLINE). Returns true when the reader
 * refused it, in which case the retry backoff has been advanced and the
 * caller should raise OSDP_TRS_SCAN_SUSPENDED.
 */
bool osdp_trs_probe_close(struct osdp_pd *pd)
{
	struct osdp_trs *trs = &pd->trs;

	trs->probe = false;
	trs->scan.holding = false;
	trs->scan.tstamp = osdp_millis_now();
	if (!trs->failed) {
		trs->scan.backoff_ms = 0;
		return false;
	}
	if (trs->scan.backoff_ms == 0) {
		trs->scan.backoff_ms = trs->scan.mode0_dwell_ms;
	}
	trs->scan.backoff_ms *= 2;
	if (trs->scan.backoff_ms > OSDP_TRS_SCAN_BACKOFF_MAX_MS) {
		trs->scan.backoff_ms = OSDP_TRS_SCAN_BACKOFF_MAX_MS;
	}
	return true;
}

/* Forget any probe in flight (PD is re-initializing) */
void osdp_trs_probe_reset(struct osdp_pd *pd)
{
	pd->trs.probe = false;
	pd->trs.scan.holding = false;
	pd->trs.scan.tstamp = osdp_millis_now();
}

/*
 * Ordinary (mode 00) card or keypad activity: restart the mode-0 dwell so a
 * probe never slices into a read the user is in the middle of.
 */
void osdp_trs_scan_note_activity(struct osdp_pd *pd)
{
	if (pd->trs.scan.enabled && !pd->trs.probe) {
		pd->trs.scan.tstamp = osdp_millis_now();
	}
}
