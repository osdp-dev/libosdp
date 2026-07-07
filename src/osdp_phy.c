/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "osdp_common.h"
#include "osdp_diag.h"
#include "osdp_metrics.h"

#define OSDP_PKT_MARK                  0xFF
#define OSDP_PKT_SOM                   0x53
#define PKT_CONTROL_SQN                0x03
#define PKT_CONTROL_CRC                0x04
#define PKT_CONTROL_SCB                0x08
#define PKT_TRACE_MANGLED              0x80

PACK(struct osdp_packet_header {
	uint8_t som;
	uint8_t pd_address;
	uint8_t len_lsb;
	uint8_t len_msb;
	uint8_t control;
	uint8_t data[];
});

static inline bool packet_has_mark(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_PKT_HAS_MARK);
}

/*
 * Contract (see osdp_write_fn_t in osdp.h): one call, all-or-nothing.
 *   ret == len : entire buffer queued for TX
 *   ret == 0   : transient EAGAIN; caller must yield and retry with the
 *                same finalized buffer on the next refresh
 *   ret  < 0   : fatal channel error
 *
 * No partial-write loop: the callback never returns 0 < ret < len.
 */
static int osdp_channel_send(struct osdp_pd *pd, uint8_t *buf, int len)
{
	struct osdp_channel *channel = &pd_to_osdp(pd)->channel;

	/* flush rx to remove any invalid data. */
	if (channel->flush) {
		channel->flush(channel->data);
	}

	return channel->send(channel->data, buf, len);
}

#ifdef OPT_OSDP_RX_ZERO_COPY
static int osdp_channel_recv_pkt(struct osdp_pd *pd)
{
	const uint8_t *buf = NULL;
	int max_len = 0;
	struct osdp_channel *channel = &pd_to_osdp(pd)->channel;

	if (pd->rx_pkt->buf) {
		LOG_WRN("Previous packet not released! Forcing release");
		osdp_phy_release_packet(pd);
	}

	if (channel->recv_pkt(channel->data, &buf, &max_len) != 0 ||
	    !buf || max_len <= 0) {
		return 0;
	}

	pd->rx_pkt->buf = buf;
	pd->rx_pkt->max_len = (unsigned long)max_len;
	pd->rx_pkt->len = 0;
	pd->packet_buf = (uint8_t *)buf;
	return 1;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

void osdp_phy_release_packet(struct osdp_pd *pd)
{
#ifdef OPT_OSDP_RX_ZERO_COPY
	struct osdp_channel *channel = &pd_to_osdp(pd)->channel;

	if (pd->rx_pkt && pd->rx_pkt->buf && channel->release_pkt) {
		channel->release_pkt(channel->data, pd->rx_pkt->buf);
	}
	if (pd->rx_pkt) {
		pd->rx_pkt->buf = NULL;
		pd->rx_pkt->len = 0;
		pd->rx_pkt->max_len = 0;
	}
	pd->packet_len = 0;
	/* A staged reply (pd_prebuild_status_reply) re-homes packet_buf to the
	 * TX staging buffer during command dispatch, before this teardown runs.
	 * Only reset the RX-side pointer when no such reply is pending, else we
	 * null the finalized reply out from under pd_send_reply(). */
	if (!pd->reply_prebuilt) {
		CLEAR_FLAG(pd, PD_FLAG_PKT_HAS_MARK);
		pd->packet_buf = NULL;
	}
#else /* OPT_OSDP_RX_ZERO_COPY */
	ARG_UNUSED(pd);
#endif /* OPT_OSDP_RX_ZERO_COPY */
}

#ifndef OPT_OSDP_RX_ZERO_COPY
static int osdp_channel_receive(struct osdp_pd *pd)
{
	uint8_t buf[64];
	int recv, total_recv = 0;
	struct osdp_channel *channel = &pd_to_osdp(pd)->channel;

#ifdef UNIT_TESTING
	if (!channel->recv) {
		return 0;
	}
#endif

	do {
		recv = channel->recv(channel->data, buf, sizeof(buf));
		if (recv <= 0) {
			break;
		}
		if (osdp_rb_push_buf(pd->rx_rb, buf, recv) != recv) {
			LOG_EM("RX ring buffer overflow!");
			return -1;
		}
		total_recv += recv;
	} while (recv == sizeof(buf));

	return total_recv;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

static uint8_t osdp_compute_checksum(uint8_t *msg, int length)
{
	uint8_t checksum = 0;
	int i, whole_checksum;

	whole_checksum = 0;
	for (i = 0; i < length; i++) {
		whole_checksum += msg[i];
		checksum = ~(0xff & whole_checksum) + 1;
	}
	return checksum;
}

static inline int phy_get_next_seq_number(struct osdp_pd *pd)
{
	int next_seq = pd->seq_number;

	next_seq += 1;
	if (next_seq > 3) {
		next_seq = 1;
	}
	return next_seq;
}

static inline void phy_rollback_seq_number(struct osdp_pd *pd)
{
	pd->seq_number -= 1;
	if (pd->seq_number < 1) { /* rollback to zero is not supported */
		pd->seq_number = 3;
	}
}

static inline void phy_reset_seq_number(struct osdp_pd *pd)
{
	pd->seq_number = -1;
}

int osdp_phy_packet_get_data_offset(struct osdp_pd *pd, const uint8_t *buf)
{
	int sb_len = 0, mark_byte_len = 0;
	struct osdp_packet_header *pkt;

	ARG_UNUSED(pd);
	if (packet_has_mark(pd)) {
		mark_byte_len = 1;
		buf += 1;
	}
	pkt = (struct osdp_packet_header *)buf;
	if (pkt->control & PKT_CONTROL_SCB) {
		sb_len = pkt->data[0];
	}
	return mark_byte_len + sizeof(struct osdp_packet_header) + sb_len;
}

uint8_t *osdp_phy_packet_get_smb(struct osdp_pd *pd, const uint8_t *buf)
{
	struct osdp_packet_header *pkt;

	pkt = (struct osdp_packet_header *)(buf + packet_has_mark(pd));
	return (pkt->control & PKT_CONTROL_SCB) ? pkt->data : NULL;
}

int osdp_phy_in_sc_handshake(int is_reply, int id)
{
	if (is_reply) {
		return (id == REPLY_CCRYPT || id == REPLY_RMAC_I);
	} else {
		return (id == CMD_CHLNG || id == CMD_SCRYPT);
	}
}

int osdp_phy_packet_init(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int id, scb_len = 0;
	struct osdp_packet_header *pkt;

	if (max_len < OSDP_MINIMUM_PACKET_SIZE) {
		LOG_ERR("packet_init: packet size too small");
		return OSDP_ERR_PKT_FMT;
	}

	/**
	 * In PD mode just follow what we received from CP. In CP mode, as we
	 * initiate the transaction, choose based on OPT_OSDP_SKIP_MARK_BYTE.
	 */
	if ((is_pd_mode(pd) && packet_has_mark(pd)) ||
	    (is_cp_mode(pd) && !ISSET_FLAG(pd, PD_FLAG_PKT_SKIP_MARK))) {
		buf[0] = OSDP_PKT_MARK;
		buf++;
		SET_FLAG(pd, PD_FLAG_PKT_HAS_MARK);
	}

	/* Fill packet header */
	pkt = (struct osdp_packet_header *)buf;
	pkt->som = OSDP_PKT_SOM;
	pkt->pd_address = pd->address & 0x7F;	/* Use only the lower 7 bits */
	if (ISSET_FLAG(pd, PD_FLAG_PKT_BROADCAST)) {
		pkt->pd_address = 0x7F;
		CLEAR_FLAG(pd, PD_FLAG_PKT_BROADCAST);
	}
	/* PD must reply with MSB of it's address set */
	if (is_pd_mode(pd)) {
		pkt->pd_address |= 0x80;
		id = pd->reply_id;
	} else {
		id = pd->cmd_id;
	}
	pkt->control = phy_get_next_seq_number(pd);
	pd->phy_tx_seq = pkt->control & PKT_CONTROL_SQN;
	if (is_pd_mode(pd) ||
	    (is_cp_mode(pd) && ISSET_FLAG(pd, PD_FLAG_CP_USE_CRC))) {
		pkt->control |= PKT_CONTROL_CRC;
	}

	if (sc_is_active(pd)) {
		pkt->control |= PKT_CONTROL_SCB;
		pkt->data[0] = scb_len = 2;
		pkt->data[1] = SCS_15;
	} else if (osdp_phy_in_sc_handshake(is_pd_mode(pd), id)) {
		pkt->control |= PKT_CONTROL_SCB;
		pkt->data[0] = scb_len = 3;
		pkt->data[1] = SCS_11;
	}

	return (packet_has_mark(pd) +
	        sizeof(struct osdp_packet_header) + scb_len);
}

int osdp_phy_finalize_packet(struct osdp_pd *pd, uint8_t *buf,
			     int len, int max_len)
{
	uint16_t crc16;
	struct osdp_packet_header *pkt;
	uint8_t *data;
	int data_len, checksum_len;

	/* Do a sanity check only; we expect header to be pre-filled */
	if ((unsigned long)len <= sizeof(struct osdp_packet_header)) {
		LOG_ERR("PKT_F: Invalid header");
		return OSDP_ERR_PKT_FMT;
	}

	if (packet_has_mark(pd)) {
		if (buf[0] != OSDP_PKT_MARK) {
			LOG_ERR("PKT_F: MARK validation failed! ID: 0x%02x",
				is_cp_mode(pd) ? pd->cmd_id : pd->reply_id);
			return OSDP_ERR_PKT_FMT;
		}
		/* temporarily get rid of mark byte */
		buf += 1;
		len -= 1;
		max_len -= 1;
	}
	pkt = (struct osdp_packet_header *)buf;
	if (pkt->som != OSDP_PKT_SOM) {
		LOG_ERR("PKT_F: header SOM validation failed! ID: 0x%02x",
			is_cp_mode(pd) ? pd->cmd_id : pd->reply_id);
		return OSDP_ERR_PKT_FMT;
	}

	/* len: with CRC (2 bytes) or checksum (1 byte) */
	checksum_len = (pkt->control & PKT_CONTROL_CRC) ? 2 : 1;
	pkt->len_lsb = BYTE_0(len + checksum_len);
	pkt->len_msb = BYTE_1(len + checksum_len);

	if (is_data_trace_enabled(pd)) {
		uint8_t control;

		/**
		 * We can potentially avoid having to set PKT_TRACE_MANGLED
		 * here if we can get the dissector accept fully formed SCB
		 * but non-encrypted data block. But that might lead to the
		 * dissector parsing malformed packets as valid ones.
		 *
		 * See the counterpart of this in osdp_phy_decode_packet() for
		 * more details.
		 */
		control = pkt->control;
		pkt->control |= PKT_TRACE_MANGLED;
		osdp_capture_packet(pd, (uint8_t *)pkt, len + 2);
		pkt->control = control;
	}

	if (sc_is_active(pd) &&
	    pkt->control & PKT_CONTROL_SCB && pkt->data[1] >= SCS_15) {
		if (pkt->data[1] == SCS_17 || pkt->data[1] == SCS_18) {
			/**
			 * Only the data portion of message (after id byte)
			 * is encrypted. While (en)decrypting, we must skip
			 * header, security block, and cmd/reply ID byte.
			 *
			 * Note: if cmd/reply has no data, we must set type to
			 * SCS_15/SCS_16 and send them.
			 */
			data = pkt->data + pkt->data[0] + 1;
			data_len = len - (sizeof(struct osdp_packet_header) +
					  pkt->data[0] + 1);
			len -= data_len;
			/**
			 * check if the passed buffer can hold the encrypted
			 * data where length may be rounded up to the nearest
			 * 16 byte block boundary.
			 */
			if (AES_PAD_LEN(data_len + 1) > max_len) {
				/* data_len + 1 for OSDP_SC_EOM_MARKER */
				goto out_of_space_error;
			}
			len += osdp_encrypt_data(pd, is_cp_mode(pd), data, data_len);
		}
		/* len: with 4bytes MAC; with CRC (2 bytes) or checksum (1 byte); without 1 byte mark */
		if (len + 4 > max_len) {
			goto out_of_space_error;
		}

		/* len: with CRC/checksum; with 4 byte MAC */
		checksum_len = (pkt->control & PKT_CONTROL_CRC) ? 2 : 1;
		pkt->len_lsb = BYTE_0(len + checksum_len + 4);
		pkt->len_msb = BYTE_1(len + checksum_len + 4);

		/* compute and extend the buf with 4 MAC bytes */
		osdp_compute_mac(pd, is_cp_mode(pd), buf, len);
		data = is_cp_mode(pd) ? pd->sc.c_mac : pd->sc.r_mac;
		memcpy(buf + len, data, 4);
		len += 4;
	}

	/* fill crc16 or checksum */
	if (pkt->control & PKT_CONTROL_CRC) {
		if (len + 2 > max_len) {
			goto out_of_space_error;
		}
		crc16 = osdp_compute_crc16(buf, len);
		bwrite_u16_le(crc16, buf, &len);
	} else {
		if (len + 1 > max_len) {
			goto out_of_space_error;
		}
		buf[len] = osdp_compute_checksum(buf, len);
		len += 1;
	}

	len += packet_has_mark(pd);
	if (packet_has_mark(pd)) {
		buf -= 1; /* restore mark for capture */
	}
	if (is_packet_trace_enabled(pd)) {
		osdp_capture_packet(pd, buf, len);
	}
	return len;

out_of_space_error:
	LOG_ERR("PKT_F: Out of buffer space! CMD(%02x)", pd->cmd_id);
	return OSDP_ERR_PKT_FMT;
}

/* Caller guarantees buf[0..len) is a finalized packet (header + payload +
 * SC/MAC + CRC, sequence-encoded). This function only queues the bytes to
 * the transport; it never re-runs finalize. See osdp_phy_finalize_packet(). */
int osdp_phy_send_packet(struct osdp_pd *pd, uint8_t *buf, int len)
{
	int ret = osdp_channel_send(pd, buf, len);

	if (ret == 0) {
		return OSDP_ERR_PKT_WAIT_TX;
	}
	if (ret < 0 || ret != len) {
		LOG_ERR("Channel send for %d bytes failed! ret: %d", len, ret);
		return OSDP_ERR_PKT_BUILD;
	}
	osdp_metrics_report(pd, OSDP_METRIC_PACKET_SENT);
	return len;
}

static int phy_validate_header(struct osdp_pd *pd, uint8_t *buf,
                               unsigned long buf_len, unsigned long max_len)
{
	struct osdp_packet_header *pkt;
	unsigned long pkt_len;
	int mark = 0;

	if (buf_len < sizeof(struct osdp_packet_header)) {
		return OSDP_ERR_PKT_WAIT;
	}

	/* Check for packet mark and SOM */
	if (buf[0] == OSDP_PKT_MARK && buf_len > 1 && buf[1] == OSDP_PKT_SOM) {
		mark = 1;
		SET_FLAG(pd, PD_FLAG_PKT_HAS_MARK);
	} else if (buf[0] == OSDP_PKT_SOM) {
		CLEAR_FLAG(pd, PD_FLAG_PKT_HAS_MARK);
	} else {
		return OSDP_ERR_PKT_FMT;
	}

	pkt = (struct osdp_packet_header *)(buf + mark);
	if (pkt->som != OSDP_PKT_SOM) {
		LOG_ERR("Invalid SOM 0x%02x", pkt->som);
		return OSDP_ERR_PKT_FMT;
	}

	pkt_len = (pkt->len_msb << 8) | pkt->len_lsb;
	if (pkt_len > max_len ||
	    pkt_len < sizeof(struct osdp_packet_header) + 1) {
		return OSDP_ERR_PKT_FMT;
	}

	/**
	 * A PD on a multi-drop bus sees every other PD's replies; silently
	 * skip those wrong-direction packets to play nice with others.
	 */
	if (is_pd_mode(pd) && (pkt->pd_address & 0x80)) {
		return OSDP_ERR_PKT_SKIP;
	}

	/**
	 * A bus must have exactly one CP. A command-direction packet seen by
	 * a CP can only have come from a second CP contending for the bus;
	 * warn about the misconfiguration and skip the packet.
	 */
	if (is_cp_mode(pd) && !(pkt->pd_address & 0x80)) {
		LOG_WRN("Saw a command from another CP on the bus; skipping it");
		return OSDP_ERR_PKT_SKIP;
	}

	return (int)(pkt_len + mark);
}

#ifndef OPT_OSDP_RX_ZERO_COPY
static bool phy_rescan_packet_buf(struct osdp_pd *pd)
{
	unsigned long j = packet_has_mark(pd);
	unsigned long i = j + 1; /* +1 to skip current SoM */

	while (i < pd->packet_buf_len && pd->packet_buf[i] != OSDP_PKT_SOM) {
		i++;
	}

	if (i < pd->packet_buf_len) {
		/* found another SoM; move the rest of the bytes down */
		if (i && pd->packet_buf[i - 1] == OSDP_PKT_MARK) {
			pd->packet_buf[0] = OSDP_PKT_MARK;
			j = 1;
			SET_FLAG(pd, PD_FLAG_PKT_HAS_MARK);
		} else {
			j = 0;
			CLEAR_FLAG(pd, PD_FLAG_PKT_HAS_MARK);
		}
		while (i < pd->packet_buf_len) {
			pd->packet_buf[j++] = pd->packet_buf[i++];
		}
		pd->packet_buf_len = j;
		return true;
	}

	/* nothing found, discarded all */
	pd->packet_buf_len = 0;
	return false;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

#ifndef OPT_OSDP_RX_ZERO_COPY
static int phy_check_header(struct osdp_pd *pd)
{
	int ret, len;
	uint8_t cur_byte = 0, prev_byte = 0;
	uint8_t *buf = pd->packet_buf;

	/* Scan for packet start */
	while (pd->packet_buf_len == 0) {
		if (osdp_rb_pop(pd->rx_rb, &cur_byte)) {
			return OSDP_ERR_PKT_NO_DATA;
		}
		if (cur_byte == OSDP_PKT_SOM) {
			if (prev_byte == OSDP_PKT_MARK) {
				buf[0] = OSDP_PKT_MARK;
				buf[1] = OSDP_PKT_SOM;
				pd->packet_buf_len = 2;
			} else {
				buf[0] = OSDP_PKT_SOM;
				pd->packet_buf_len = 1;
			}
			break;
		}
		if (cur_byte != OSDP_PKT_MARK) {
			pd->packet_scan_skip++;
		}
		prev_byte = cur_byte;
	}

	/* Found start of a new packet; wait until we have atleast the header */
	len = osdp_rb_pop_buf(pd->rx_rb, buf + pd->packet_buf_len,
			      sizeof(struct osdp_packet_header) - 1);
	pd->packet_buf_len += len;

	/* Validate header using shared function */
	ret = phy_validate_header(pd, buf, pd->packet_buf_len, OSDP_PACKET_BUF_SIZE);
	if (ret == OSDP_ERR_PKT_FMT) {
		/*
		 * Since SoM byte was encountered and the packet structure is
		 * invalid, we cannot just discard all bytes extracted so far
		 * as there may another valid SoM in the subsequent stream. So
		 * we need to re-scan rest of the extracted bytes for another
		 * SoM before we can discard the extracted bytes.
		 */
		if (phy_rescan_packet_buf(pd)) {
			LOG_DBG("Found nested SoM in re-scan; re-parsing");
		}
		return OSDP_ERR_PKT_WAIT;
	}

	return ret;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

static int phy_check_packet(struct osdp_pd *pd, uint8_t *buf, int pkt_len)
{
	int pd_addr;
	uint16_t comp, cur;
	struct osdp_packet_header *pkt;

	if (packet_has_mark(pd)) {
		buf += 1;
		pkt_len -= 1;
	}
	pkt = (struct osdp_packet_header *)buf;

	/* Frame passed framing checks upstream; account it as received
	 * regardless of integrity-check outcome. */
	osdp_metrics_report(pd, OSDP_METRIC_PACKET_RECEIVED);

	/* validate CRC/checksum */
	if (pkt->control & PKT_CONTROL_CRC) {
		pkt_len -= 2; /* consume CRC */
		cur = (buf[pkt_len + 1] << 8) | buf[pkt_len];
		comp = osdp_compute_crc16(buf, pkt_len);
		if (comp != cur) {
			LOG_ERR("Invalid crc 0x%04x/0x%04x", comp, cur);
			osdp_metrics_report(pd, OSDP_METRIC_PACKET_CHECK_ERROR);
			return OSDP_ERR_PKT_FMT;
		}
	} else {
		pkt_len -= 1; /* consume checksum */
		cur = buf[pkt_len];
		comp = osdp_compute_checksum(buf, pkt_len);
		if (comp != cur) {
			LOG_ERR("Invalid checksum %02x/%02x", comp, cur);
			osdp_metrics_report(pd, OSDP_METRIC_PACKET_CHECK_ERROR);
			return OSDP_ERR_PKT_FMT;
		}
	}

	/* validate PD address */
	pd_addr = pkt->pd_address & 0x7F;
	if (pd_addr != pd->address && pd_addr != 0x7F) {
		/* not addressed to us and was not broadcasted */
		if (is_cp_mode(pd)) {
			LOG_ERR("Invalid pd address %d", pd_addr);
			return OSDP_ERR_PKT_CHECK;
		}
		return OSDP_ERR_PKT_SKIP;
	}

	/* validate sequence number */
	comp = pkt->control & PKT_CONTROL_SQN;
	if (is_pd_mode(pd)) {
		if (comp == 0) {
			/**
			 * CP is trying to restart communication by sending a 0.
			 * The current PD implementation does not hold any state
			 * between commands so we can just set seq_number to -1
			 * (so it gets incremented to 0 with a call to
			 * phy_get_seq_number()) and invalidate any established
			 * secure channels.
			 */
			phy_reset_seq_number(pd);
			pd->last_tx_len = 0;
			sc_deactivate(pd);
		}
		else if (comp == pd->seq_number) {
			/**
			 * CP re-sent the same command without incrementing the
			 * sequence number. Per OSDP spec, we must re-emit the
			 * previous reply bit-identically. Rebuilding it here would
			 * corrupt the SC MAC chain (r_mac/c_mac have already
			 * advanced) and re-consume state (event/card/file data).
			 *
			 * If we have a cached reply and the command id matches,
			 * re-send the bytes still sitting in tx_buf (RX goes to a
			 * separate rx_buf in PD mode, so tx_buf is preserved).
			 */
			if (pd->last_tx_len > 0) {
				int scb_len = (pkt->control & PKT_CONTROL_SCB) ?
					pkt->data[0] : 0;
				uint8_t rx_cmd_id = buf[sizeof(*pkt) + scb_len];

				if (rx_cmd_id == pd->last_cmd_id) {
					LOG_DBG("Seq repeat: retransmitting cached reply");
					osdp_channel_send(pd,
						osdp_tx_staging_buf(pd),
						pd->last_tx_len);
					return OSDP_ERR_PKT_SKIP;
				}
				/* seq matches but cmd id doesn't: drop the cache
				 * and fall through to the normal path which will
				 * MAC-verify and NAK if the packet is bogus. */
				pd->last_tx_len = 0;
			}
			phy_rollback_seq_number(pd);
			LOG_INF("Received a sequence repeat packet!");
		}
		/**
		 * For packets addressed to the broadcast address, the reply
		 * must have address set to 0x7F and not the current PD's
		 * address. To handle this case, let's capture this state in PD
		 * flags.
		 */
		if (pd_addr == 0x7F) {
			SET_FLAG(pd, PD_FLAG_PKT_BROADCAST);
		}
	} else {
		if (comp == 0) {
			/**
			 * Check for receiving a busy reply from the PD which would
			 * have a sequence number of 0, come in an unsecured packet
			 * of minimum length, and have the reply ID REPLY_BUSY.
			 */
			if ((pkt_len == 6) && (pkt->data[0] == REPLY_BUSY)) {
				pd->seq_number -= 1;
				return OSDP_ERR_PKT_BUSY;
			}
		}
	}
	cur = phy_get_next_seq_number(pd);
	if (cur != comp && !ISSET_FLAG(pd, PD_FLAG_SKIP_SEQ_CHECK)) {
		LOG_ERR("Packet sequence mismatch (expected: %d, got: %d)",
			cur, comp);
		pd->reply_id = REPLY_NAK;
		pd->nak_code = OSDP_PD_NAK_SEQ_NUM;
		return OSDP_ERR_PKT_NACK;
	}

	return OSDP_ERR_PKT_NONE;
}

int osdp_phy_check_packet(struct osdp_pd *pd)
{
	int ret;

	/* Acquire new data: either full packet or stream bytes */
#ifdef OPT_OSDP_RX_ZERO_COPY
	{
		ret = osdp_channel_recv_pkt(pd);
	}
#else /* OPT_OSDP_RX_ZERO_COPY */
	{
		ret = osdp_channel_receive(pd);
	}
#endif /* OPT_OSDP_RX_ZERO_COPY */

	/**
	 * PD mode does not maintain state. When we receive the anything
	 * from CP, we need to capture the timestamp so we can timeout and
	 * clear the buffer on errors and stray RX data.
	 */
	if (is_pd_mode(pd) && pd->packet_buf_len == 0 && ret > 0) {
		pd->tstamp = osdp_millis_now();
	}

	/* Parse and validate packet header */
	if (pd->packet_len == 0) {
#ifdef OPT_OSDP_RX_ZERO_COPY
		{
			if (!pd->rx_pkt->buf) {
				return OSDP_ERR_PKT_NO_DATA;
			}
			ret = phy_validate_header(pd, pd->packet_buf,
						  pd->rx_pkt->max_len,
						  pd->rx_pkt->max_len);
			if (ret < 0) {
				if (ret == OSDP_ERR_PKT_FMT) {
					osdp_phy_release_packet(pd);
				}
				return ret;
			}
			pd->packet_len = ret;
			pd->packet_buf_len = ret;
			pd->rx_pkt->len = ret;
		}
#else /* OPT_OSDP_RX_ZERO_COPY */
		{
			ret = phy_check_header(pd);
			if (ret < 0) {
				return ret;
			}
			pd->packet_len = ret;
			if (pd->packet_scan_skip) {
				LOG_DBG("Packet scan skipped:%u mark:%d",
					pd->packet_scan_skip,
					ISSET_FLAG(pd, PD_FLAG_PKT_HAS_MARK));
				pd->packet_scan_skip = 0;
			}
		}
#endif /* OPT_OSDP_RX_ZERO_COPY */
	}

#ifndef OPT_OSDP_RX_ZERO_COPY
	{
		/* Traditional: collect remaining packet bytes from ring buffer */
		ret = osdp_rb_pop_buf(pd->rx_rb,
				      pd->packet_buf + pd->packet_buf_len,
				      pd->packet_len - pd->packet_buf_len);
		pd->packet_buf_len += ret;
		if (pd->packet_buf_len != pd->packet_len) {
			return OSDP_ERR_PKT_WAIT;
		}
	}
#endif /* OPT_OSDP_RX_ZERO_COPY */

	/* Packet complete: trace and validate */
	if (is_packet_trace_enabled(pd)) {
		osdp_capture_packet(pd, pd->packet_buf, pd->packet_buf_len);
	}

	ret = phy_check_packet(pd, pd->packet_buf, pd->packet_len);

	/* Relase packet buffers on errors */
#ifdef OPT_OSDP_RX_ZERO_COPY
	if (ret != OSDP_ERR_PKT_NONE) {
		osdp_phy_release_packet(pd);
	}
#endif /* OPT_OSDP_RX_ZERO_COPY */

	return ret;
}

int osdp_phy_decode_packet(struct osdp_pd *pd, uint8_t **pkt_start)
{
	uint8_t *data, *mac, *buf = pd->packet_buf;
	int mac_offset, is_cmd, len = pd->packet_buf_len;
	struct osdp_packet_header *pkt;
	bool is_sc_active = sc_is_active(pd);

	if (packet_has_mark(pd)) {
		buf += 1;
		len -= 1;
	}
	pkt = (struct osdp_packet_header *)buf;
	len -= pkt->control & PKT_CONTROL_CRC ? 2 : 1;
	mac_offset = len - 4;
	data = pkt->data;
	len -= sizeof(struct osdp_packet_header);

	if (pkt->control & PKT_CONTROL_SCB) {
		if (is_pd_mode(pd) && !sc_is_capable(pd)) {
			LOG_ERR("PD is not SC capable");
			pd->reply_id = REPLY_NAK;
			pd->nak_code = OSDP_PD_NAK_SC_UNSUP;
			return OSDP_ERR_PKT_NACK;
		}
		if (pkt->data[1] < SCS_11 || pkt->data[1] > SCS_18) {
			LOG_ERR("Invalid SB Type");
			pd->reply_id = REPLY_NAK;
			pd->nak_code = OSDP_PD_NAK_SC_COND;
			return OSDP_ERR_PKT_NACK;
		}
		if (!is_sc_active && pkt->data[1] > SCS_14) {
			LOG_ERR("Invalid SCS type (%x)", pkt->data[1]);
			pd->reply_id = REPLY_NAK;
			pd->nak_code = OSDP_PD_NAK_SC_COND;
			return OSDP_ERR_PKT_NACK;
		}
		if (pkt->data[1] == SCS_11 || pkt->data[1] == SCS_13) {
			/**
			 * CP signals PD to use SCBKD by setting SCB data byte
			 * to 0. But since SCBK-D is insecure access, it's
			 * usage is limited to install mode (a provisioning time
			 * mode) only.
			 */
			if (is_install_mode(pd) && pkt->data[2] == 0) {
				SET_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
			}
		}
		data = pkt->data + pkt->data[0];
		len -= pkt->data[0]; /* consume security block */
	} else {
		if (is_cp_mode(pd)) {
			/**
			 * If the current packet is an ACK for a KEYSET, the PD
			 * might have discarded the secure channel session keys
			 * in favour of the new key we sent and hence this packet
			 * may reach us in plain text. To work with such PDs, we
			 * must also discard our secure session.
			 *
			 * For now, let's just pretend that SC is deactivated so
			 * the rest of this method finishes normally. The actual
			 * secure channel is actually discarded from the CP
			 * state machine.
			 */
			if (pd->cmd_id == CMD_KEYSET && pkt->data[0] == REPLY_ACK) {
				is_sc_active = false;
			}
			/**
			 * When the PD discards it's secure channel for some
			 * reason, it responds with NACK(6) in plaintext. There
			 * may be other cases too. So we will allow NAKs in
			 */
			if (is_sc_active && pkt->data[0] == REPLY_NAK) {
				is_sc_active = false;
			}
		}
		if (is_sc_active) {
			LOG_ERR("Received plain-text message in SC");
			pd->reply_id = REPLY_NAK;
			pd->nak_code = OSDP_PD_NAK_SC_COND;
			return OSDP_ERR_PKT_NACK;
		}
	}

	if (is_sc_active &&
	    pkt->control & PKT_CONTROL_SCB && pkt->data[1] >= SCS_15) {
		/* validate MAC */
		is_cmd = is_pd_mode(pd);
		osdp_compute_mac(pd, is_cmd, buf, mac_offset);
		mac = is_cmd ? pd->sc.c_mac : pd->sc.r_mac;
		if (memcmp(buf + mac_offset, mac, 4) != 0) {
			LOG_ERR("Invalid MAC; discarding SC");
			sc_deactivate(pd);
			pd->reply_id = REPLY_NAK;
			pd->nak_code = OSDP_PD_NAK_SC_COND;
			return OSDP_ERR_PKT_NACK;
		}
		len -= 4; /* consume MAC */

		/* decrypt data block */
		if (pkt->data[1] == SCS_17 || pkt->data[1] == SCS_18) {
			/**
			 * Only the data portion of message (after id byte)
			 * is encrypted. While (en)decrypting, we must skip
			 * header (6), security block (2) and cmd/reply id (1)
			 * bytes if cmd/reply has no data, use SCS_15/SCS_16.
			 *
			 * At this point, the header and security block is
			 * already consumed. So we can just skip the cmd/reply
			 * ID (data[0])  when calling osdp_decrypt_data().
			 */
			len = osdp_decrypt_data(pd, is_cmd, data + 1, len - 1);
			if (len < 0) {
				LOG_ERR("Failed at decrypt; discarding SC");
				sc_deactivate(pd);
				pd->reply_id = REPLY_NAK;
				pd->nak_code = OSDP_PD_NAK_SC_COND;
				return OSDP_ERR_PKT_NACK;
			}
			if (len == 0) {
				/**
				 * If cmd/reply has no data, PD "should" have
				 * used SCS_15/SCS_16 but we will be tolerant
				 * towards those faulty implementations.
				 */
				LOG_WRN_ONCE("Received encrypted data block with 0 "
					"length; tolerating non-conformance!");
			}
			len += 1; /* put back cmd/reply ID */
		}
	}

	if (is_data_trace_enabled(pd)) {
		int ret = len;

		/**
		 * Here, we move the decrypted data block right after the
		 * header so we can pass the entire packet to the tracing
		 * infrastructure allowing us to reuse the same protocol
		 * dissector for regular packet trace as well as data trace
		 * files.
		 *
		 * We also touch up the packet so that it can be parsed/decoded
		 * correctly. Since none of the later states require the
		 * header, we are free to mangle it as needed for our
		 * convenience.
		 *
		 * The following changes performed:
		 *   - Update the length field with the modifications
		 *   - Remove `PKT_CONTROL_SCB` bit to show no sign of a secure
		 *     channels' existence.
		 *   - Set a new bit `PKT_TRACE_MANGLED` so the dissector can
		 *     skip the PacketCheck bytes (maybe other housekeeping?).
		 */
		memmove(pkt->data, data, len);
		*pkt_start = pkt->data;

		len += sizeof(struct osdp_packet_header);
		pkt->control &= ~PKT_CONTROL_SCB;
		pkt->control |= PKT_TRACE_MANGLED;
		pkt->len_lsb = BYTE_0(len);
		pkt->len_msb = BYTE_1(len);
		osdp_capture_packet(pd, (uint8_t *)pkt, len);
		return ret;
	}

	*pkt_start = data;
	return len;
}

void osdp_phy_state_reset(struct osdp_pd *pd, bool is_error)
{
	struct osdp_channel *channel = &pd_to_osdp(pd)->channel;

#ifdef OPT_OSDP_RX_ZERO_COPY
	if (pd->rx_pkt && pd->rx_pkt->buf) {
		osdp_phy_release_packet(pd);
	}
#endif
	pd->packet_buf_len = 0;
	pd->packet_len = 0;
	pd->phy_state = 0;
	pd->packet_buf = pd_to_osdp(pd)->rx_buf;
	if (is_error) {
		pd->phy_retry_count = 0;
		pd->phy_tx_seq = 0;
		pd->last_tx_len = 0;
		phy_reset_seq_number(pd);
		if (channel->flush) {
			channel->flush(channel->data);
		}
#ifndef OPT_OSDP_RX_ZERO_COPY
		osdp_rb_reset(pd->rx_rb);
#endif
	}
}

void osdp_phy_progress_sequence(struct osdp_pd *pd)
{
	pd->seq_number = phy_get_next_seq_number(pd);
}

/* Export packet build/verify internals to the unit tests. */
OSDP_TEST_ALIAS(osdp_phy_finalize_packet);
OSDP_TEST_ALIAS(osdp_phy_packet_init);
OSDP_TEST_ALIAS(osdp_compute_checksum);
