/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"

/* These tests drive the byte-stream framer directly (they push raw bytes into
 * pd->rx_rb and call osdp_phy_check_packet). That ring buffer and the framing
 * code only exist without OPT_OSDP_RX_ZERO_COPY; under zero-copy the channel
 * hands libosdp pre-framed packets, so there is nothing here to exercise. */
#ifndef OPT_OSDP_RX_ZERO_COPY

extern int test_osdp_phy_finalize_packet(struct osdp_pd *pd, uint8_t *buf,
					 int len, int max_len);
extern int test_osdp_phy_packet_init(struct osdp_pd *pd, uint8_t *buf, int max_len);
extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);
extern uint8_t test_osdp_compute_checksum(uint8_t *msg, int length);

struct refresh_yield_test_data {
	int send_count;
	int last_pd_address;
};

static void reset_pd_packet_state(struct osdp_pd *pd)
{
	osdp_phy_state_reset(pd, true);
	pd->seq_number = -1;
}

static int test_cp_refresh_yield_send(void *data, uint8_t *buf, int len)
{
	struct refresh_yield_test_data *t = data;
	int addr_offset;

	if (len <= 0) {
		return len;
	}

#ifndef OPT_OSDP_SKIP_MARK_BYTE
	addr_offset = 2;
#else
	addr_offset = 1;
#endif

	t->send_count++;
	t->last_pd_address = buf[addr_offset] & 0x7f;
	return len;
}

static int test_cp_refresh_yield_recv(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);

	return 0;
}

/* Helper function to create valid test packets using a simple approach */
static int test_osdp_create_packet(uint8_t pd_addr, uint8_t control, uint8_t *data,
				   int data_len, uint8_t *out_buf, int max_len)
{
	/* Create basic packet structure manually with known constants */
	int len = 0;
	bool use_mark = true;
	bool use_crc = (control & 0x04) != 0;  /* PKT_CONTROL_CRC = 0x04 */
	uint16_t crc16;
	uint8_t checksum;

#ifdef OPT_OSDP_SKIP_MARK_BYTE
	use_mark = false;
#endif

	/* Check buffer size */
	int min_size = (use_mark ? 1 : 0) + 5 + data_len + (use_crc ? 2 : 1);
	if (max_len < min_size) {
		return -1;
	}

	/* Add mark byte */
	if (use_mark) {
		out_buf[len++] = 0xff;  /* OSDP_PKT_MARK */
	}

	/* Add header */
	out_buf[len++] = 0x53;  /* OSDP_PKT_SOM */
	out_buf[len++] = pd_addr;

	/* Calculate and add length (header + data + checksum, excluding mark) */
	int pkt_len = 5 + data_len + (use_crc ? 2 : 1);
	out_buf[len++] = pkt_len & 0xff;  /* len_lsb */
	out_buf[len++] = (pkt_len >> 8) & 0xff;  /* len_msb */
	out_buf[len++] = control;

	/* Add data */
	if (data && data_len > 0) {
		memcpy(out_buf + len, data, data_len);
		len += data_len;
	}

	/* Add checksum/CRC */
	if (use_crc) {
		crc16 = test_osdp_compute_crc16(out_buf + (use_mark ? 1 : 0),
						len - (use_mark ? 1 : 0));
		out_buf[len++] = crc16 & 0xff;
		out_buf[len++] = (crc16 >> 8) & 0xff;
	} else {
		checksum = test_osdp_compute_checksum(out_buf + (use_mark ? 1 : 0),
						      len - (use_mark ? 1 : 0));
		out_buf[len++] = checksum;
	}

	return len;
}

static int test_cp_build_and_send_packet(struct osdp_pd *p, uint8_t *buf, int len,
				int maxlen)
{
	int cmd_len;
	uint8_t cmd_buf[128];

	if (len > 128) {
		printf("cmd_buf len err - %d/%d\n", len, 128);
		return -1;
	}
	cmd_len = len;
	memcpy(cmd_buf, buf, len);

	if ((len = osdp_phy_packet_init(p, buf, maxlen)) < 0) {
		printf("failed to phy_build_packet_head\n");
		return -1;
	}
	memcpy(buf + len, cmd_buf, cmd_len);
	len += cmd_len;
	if ((len = test_osdp_phy_finalize_packet(p, buf, len, maxlen)) < 0) {
		printf("failed to build command\n");
		return -1;
	}
	return len;
}

int test_cp_build_packet_poll(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[512] = { CMD_POLL };
	uint8_t expected[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0x65, 0x08, 0x00, 0x04, 0x60, 0x60, 0x90
	};

	printf(SUB_1 "Testing cp_build_and_send_packet(CMD_POLL) -- ");
	if ((len = test_cp_build_and_send_packet(p, packet, 1, 512)) < 0) {
		return -1;
	}
	CHECK_ARRAY(packet, len, expected);
	printf("success!\n");
	return 0;
}

int test_cp_build_packet_id(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[512] = { CMD_ID, 0x00 };
	uint8_t expected[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0x65, 0x09, 0x00, 0x04, 0x61, 0x00, 0xd9, 0x7a
	};

	printf(SUB_1 "Testing cp_build_and_send_packet(CMD_ID) -- ");
	if ((len = test_cp_build_and_send_packet(p, packet, 2, 512)) < 0) {
		return -1;
	}
	CHECK_ARRAY(packet, len, expected);
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_ack(struct osdp *ctx)
{
	uint8_t *buf;
	int len, err, pkt_len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t reply_data[] = { REPLY_ACK };
	uint8_t packet[32];
	uint8_t expected[] = { REPLY_ACK };

	printf(SUB_1 "Testing phy_decode_packet(REPLY_ACK) -- ");

	/* Skip sequence check for this test to focus on basic decode functionality */
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);

	/* Create a valid ACK packet from PD (address with MSB set) */
	uint8_t control = 0x01; /* sequence 1, no CRC */
	pkt_len = test_osdp_create_packet(0xe5, control, reply_data, 1, packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);
	if (err) {
		printf("check failed with error %d!\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	if ((len = osdp_phy_decode_packet(p, &buf)) < 0) {
		printf("decode failed!\n");
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CHECK_ARRAY(buf, len, expected);
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

static int test_phy_parse_hardcoded_plain_checksum_ack(struct osdp *ctx)
{
	uint8_t *buf;
	int len, err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	static const uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x07, 0x00, 0x01, 0x40, 0x80
	};
	static const uint8_t expected[] = { REPLY_ACK };

	printf(SUB_1 "Testing hardcoded parser fixture (plain+checksum ACK) -- ");
	reset_pd_packet_state(p);
	sc_deactivate(p);
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NONE) {
		printf("failed! check returned %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	if ((len = osdp_phy_decode_packet(p, &buf)) < 0) {
		printf("failed! decode returned %d\n", len);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CHECK_ARRAY(buf, len, expected);
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

static int test_phy_parse_hardcoded_plain_crc_nak(struct osdp *ctx)
{
	uint8_t *buf;
	int len, err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	static const uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x09, 0x00, 0x05, 0x41, 0x01, 0x0e, 0x8f
	};
	static const uint8_t expected[] = { REPLY_NAK, 0x01 };

	printf(SUB_1 "Testing hardcoded parser fixture (plain+CRC NAK) -- ");
	reset_pd_packet_state(p);
	sc_deactivate(p);
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NONE) {
		printf("failed! check returned %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	if ((len = osdp_phy_decode_packet(p, &buf)) < 0) {
		printf("failed! decode returned %d\n", len);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CHECK_ARRAY(buf, len, expected);
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

static int test_phy_parse_hardcoded_sc_scs12_checksum_ccrypt(struct osdp *ctx)
{
	uint8_t *buf;
	int len, err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	static const uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x2a, 0x00, 0x09, 0x03, 0x12, 0x01, 0x76,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
		0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
		0x19
	};
	static const uint8_t expected[] = {
		REPLY_CCRYPT,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
		0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
	};

	printf(SUB_1 "Testing hardcoded parser fixture (SC SCS_12 + checksum) -- ");
	reset_pd_packet_state(p);
	sc_deactivate(p);
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NONE) {
		printf("failed! check returned %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	if ((len = osdp_phy_decode_packet(p, &buf)) < 0) {
		printf("failed! decode returned %d\n", len);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CHECK_ARRAY(buf, len, expected);
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

static int test_phy_parse_hardcoded_sc_scs14_crc_rmaci(struct osdp *ctx)
{
	uint8_t *buf;
	int len, err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	static const uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x1b, 0x00, 0x0d, 0x03, 0x14, 0x01, 0x78,
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
		0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
		0xca, 0xba
	};
	static const uint8_t expected[] = {
		REPLY_RMAC_I,
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
		0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf
	};

	printf(SUB_1 "Testing hardcoded parser fixture (SC SCS_14 + CRC) -- ");
	reset_pd_packet_state(p);
	sc_deactivate(p);
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NONE) {
		printf("failed! check returned %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	if ((len = osdp_phy_decode_packet(p, &buf)) < 0) {
		printf("failed! decode returned %d\n", len);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CHECK_ARRAY(buf, len, expected);
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

static int test_phy_parse_hardcoded_sc_invalid_type_reject(struct osdp *ctx)
{
	int err;
	uint8_t *buf = NULL;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	static const uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x09, 0x00, 0x09, 0x02, 0x19, 0x40, 0x5b
	};

	printf(SUB_1 "Testing hardcoded parser fixture (invalid SC type reject) -- ");
	reset_pd_packet_state(p);
	sc_deactivate(p);
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NONE) {
		printf("failed! check returned %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	err = osdp_phy_decode_packet(p, &buf);
	if (err != OSDP_ERR_PKT_NACK || p->nak_code != OSDP_PD_NAK_SC_COND) {
		printf("failed! decode returned %d nak=%d\n", err, p->nak_code);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

static int test_phy_parse_hardcoded_sc_inactive_scs16_reject(struct osdp *ctx)
{
	int err;
	uint8_t *buf = NULL;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	static const uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x09, 0x00, 0x09, 0x02, 0x16, 0x40, 0x5e
	};

	printf(SUB_1 "Testing hardcoded parser fixture (inactive SC SCS_16 reject) -- ");
	reset_pd_packet_state(p);
	sc_deactivate(p);
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NONE) {
		printf("failed! check returned %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	err = osdp_phy_decode_packet(p, &buf);
	if (err != OSDP_ERR_PKT_NACK || p->nak_code != OSDP_PD_NAK_SC_COND) {
		printf("failed! decode returned %d nak=%d\n", err, p->nak_code);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

static int test_phy_parse_hardcoded_sc_active_invalid_mac_reject(struct osdp *ctx)
{
	int err;
	uint8_t *buf = NULL;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	static const uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x0d, 0x00, 0x09, 0x02, 0x16, 0x40,
		0x00, 0x00, 0x00, 0x00, 0x5a
	};

	printf(SUB_1 "Testing hardcoded parser fixture (active SC invalid MAC reject) -- ");
	reset_pd_packet_state(p);
	memset(&p->sc, 0, sizeof(p->sc));
	sc_activate(p);
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NONE) {
		printf("failed! check returned %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	err = osdp_phy_decode_packet(p, &buf);
	if (err != OSDP_ERR_PKT_NACK || p->nak_code != OSDP_PD_NAK_SC_COND ||
	    sc_is_active(p)) {
		printf("failed! decode returned %d nak=%d sc=%d\n",
		       err, p->nak_code, sc_is_active(p));
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

static int test_phy_parse_hardcoded_plaintext_when_sc_active_reject(struct osdp *ctx)
{
	int err;
	uint8_t *buf = NULL;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	static const uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x07, 0x00, 0x01, 0x40, 0x80
	};

	printf(SUB_1 "Testing hardcoded parser fixture (plaintext while SC active reject) -- ");
	reset_pd_packet_state(p);
	sc_activate(p);
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NONE) {
		printf("failed! check returned %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	err = osdp_phy_decode_packet(p, &buf);
	if (err != OSDP_ERR_PKT_NACK || p->nak_code != OSDP_PD_NAK_SC_COND) {
		printf("failed! decode returned %d nak=%d\n", err, p->nak_code);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	sc_deactivate(p);
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_ignore_leading_mark_bytes(struct osdp *ctx)
{
	uint8_t *buf;
	int len, err, pkt_len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t reply_data[] = { REPLY_ACK };
	uint8_t packet[64];
	uint8_t expected[] = { REPLY_ACK };

	printf(SUB_1 "Testing test_phy_decode_packet_ignore_leading_mark_bytes -- ");

	/* Skip sequence check for this test to focus on mark byte handling */
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);

	/* Create a valid ACK packet */
	uint8_t control = 0x01; /* sequence 1, no CRC */
	pkt_len = test_osdp_create_packet(0xe5, control, reply_data, 1, packet + 8, sizeof(packet) - 16);
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}

	/* Add leading mark bytes */
	for (int i = 0; i < 8; i++) {
		packet[i] = 0xff;
	}
	/* Add trailing mark bytes */
	for (int i = 8 + pkt_len; i < 8 + pkt_len + 8; i++) {
		packet[i] = 0xff;
	}

	osdp_rb_push_buf(p->rx_rb, packet, 8 + pkt_len + 8);
	err = osdp_phy_check_packet(p);
	if (err) {
		printf("check failed with error %d!\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	if ((len = osdp_phy_decode_packet(p, &buf)) < 0) {
		printf("decode failed!\n");
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CHECK_ARRAY(buf, len, expected);
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

int test_cp_build_packet_chlng(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[512] = { CMD_CHLNG, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
	uint8_t expected[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0x65, 0x10, 0x00, 0x04, 0x76, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0d, 0x20
	};

	printf(SUB_1 "Testing cp_build_and_send_packet(CMD_CHLNG) -- ");
	if ((len = test_cp_build_and_send_packet(p, packet, 9, 512)) < 0) {
		return -1;
	}
	CHECK_ARRAY(packet, len, expected);
	printf("success!\n");
	return 0;
}

int test_cp_build_packet_with_crc(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[512] = { CMD_POLL };

	printf(SUB_1 "Testing cp_build_and_send_packet with CRC -- ");
	SET_FLAG(p, PD_FLAG_CP_USE_CRC);

	if ((len = test_cp_build_and_send_packet(p, packet, 1, 512)) < 0) {
		CLEAR_FLAG(p, PD_FLAG_CP_USE_CRC);
		return -1;
	}

	/* Check if the control byte has CRC flag set (bit 2) */
	int mark_offset = 0;
#ifndef OPT_OSDP_SKIP_MARK_BYTE
	mark_offset = 1;
#endif
	uint8_t control_byte = packet[mark_offset + 4]; /* SOM + addr + len_lsb + len_msb + control */

	CLEAR_FLAG(p, PD_FLAG_CP_USE_CRC);

	if (!(control_byte & 0x04)) {
		printf("failed! CRC flag not set in control byte (0x%02x)\n", control_byte);
		return -1;
	}

	/* Verify the packet length includes CRC (2 bytes) instead of checksum (1 byte) */
	if (len != (mark_offset + 5 + 1 + 2)) { /* header + data + CRC */
		printf("failed! Expected CRC packet length %d, got %d\n", mark_offset + 5 + 1 + 2, len);
		return -1;
	}

	printf("success!\n");
	return 0;
}

int test_cp_build_packet_checksum_mode_trailer(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	uint8_t packet[512] = { CMD_POLL };
	uint8_t expected[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0x65, 0x07, 0x00, 0x00, 0x60, 0xe1
	};

	printf(SUB_1 "Testing cp_build_and_send_packet checksum trailer -- ");
	reset_pd_packet_state(p);
	CLEAR_FLAG(p, PD_FLAG_CP_USE_CRC);
	if ((len = test_cp_build_and_send_packet(p, packet, 1, sizeof(packet))) < 0) {
		return -1;
	}
	CHECK_ARRAY(packet, len, expected);
	printf("success!\n");
	return 0;
}

int test_cp_build_packet_crc_mode_trailer(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	uint8_t packet[512] = { CMD_POLL };
	uint8_t expected[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0x65, 0x08, 0x00, 0x04, 0x60, 0x60, 0x90
	};

	printf(SUB_1 "Testing cp_build_and_send_packet CRC trailer -- ");
	reset_pd_packet_state(p);
	SET_FLAG(p, PD_FLAG_CP_USE_CRC);
	if ((len = test_cp_build_and_send_packet(p, packet, 1, sizeof(packet))) < 0) {
		CLEAR_FLAG(p, PD_FLAG_CP_USE_CRC);
		return -1;
	}
	CLEAR_FLAG(p, PD_FLAG_CP_USE_CRC);
	CHECK_ARRAY(packet, len, expected);
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_nak(struct osdp *ctx)
{
	uint8_t *buf;
	int len, err, pkt_len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t reply_data[] = { REPLY_NAK, 0x01 };
	uint8_t packet[32];
	uint8_t expected[] = { REPLY_NAK, 0x01 };

	printf(SUB_1 "Testing phy_decode_packet(REPLY_NAK) -- ");

	/* Skip sequence check for this test to focus on basic decode functionality */
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);

	/* Create a valid NAK packet from PD (address with MSB set) */
	uint8_t control = 0x01; /* sequence 1, no CRC */
	pkt_len = test_osdp_create_packet(0xe5, control, reply_data, 2, packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);
	if (err) {
		printf("check failed with error %d!\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	if ((len = osdp_phy_decode_packet(p, &buf)) < 0) {
		printf("decode failed!\n");
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}
	CHECK_ARRAY(buf, len, expected);
	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_busy(struct osdp *ctx)
{
	int err, pkt_len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t reply_data[] = { REPLY_BUSY };
	uint8_t packet[32];

	printf(SUB_1 "Testing phy_decode_packet(REPLY_BUSY) -- ");

	/* Create a valid BUSY packet with sequence 0 */
	pkt_len = test_osdp_create_packet(0xe5, 0x00, reply_data, 1, packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_BUSY) {
		printf("failed! Expected BUSY error\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_invalid_checksum(struct osdp *ctx)
{
	int err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x08, 0x00, 0x04, 0x40, 0x00, 0x00
	};

	printf(SUB_1 "Testing phy_decode_packet with invalid checksum -- ");
	osdp_rb_push_buf(p->rx_rb, packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_FMT) {
		printf("failed! Expected format error\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_invalid_crc(struct osdp *ctx)
{
	int err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x08, 0x00, 0x04, 0x60, 0x00, 0x00
	};

	printf(SUB_1 "Testing phy_decode_packet with invalid CRC -- ");
	osdp_rb_push_buf(p->rx_rb, packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_FMT) {
		printf("failed! Expected format error\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_wrong_address(struct osdp *ctx)
{
	int err, pkt_len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t reply_data[] = { REPLY_ACK };
	uint8_t packet[32];

	printf(SUB_1 "Testing phy_decode_packet with wrong address -- ");

	/* Create a packet with wrong address (0x80 instead of 0xe5) */
	pkt_len = test_osdp_create_packet(0x80, 0x04, reply_data, 1, packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_CHECK) {
		printf("failed! Expected address check error\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_sequence_mismatch(struct osdp *ctx)
{
	int err, pkt_len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t reply_data[] = { REPLY_ACK };
	uint8_t packet[32];

	printf(SUB_1 "Testing phy_decode_packet with sequence mismatch -- ");

	/* Set PD to expect sequence 1 (after reset, seq_number = -1, so expects 0) */
	p->seq_number = 0; /* Now expects sequence 1 */

	/* Create a packet with wrong sequence (2 instead of 1) */
	uint8_t control = 0x00 | 2; /* Wrong sequence number */
	pkt_len = test_osdp_create_packet(0xe5, control, reply_data, 1, packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_NACK) {
		printf("failed! Expected OSDP_ERR_PKT_NACK, got %d\n", err);
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_invalid_som(struct osdp *ctx)
{
	int err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x44, 0xe5, 0x08, 0x00, 0x04, 0x40, 0xe7, 0xa1
	};

	printf(SUB_1 "Testing phy_decode_packet with invalid SOM -- ");
	osdp_rb_push_buf(p->rx_rb, packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	/* Invalid SOM can trigger various errors: format, wait, or no_data */
	if (err != OSDP_ERR_PKT_FMT && err != OSDP_ERR_PKT_WAIT && err != OSDP_ERR_PKT_NO_DATA) {
		printf("failed! Expected format/wait/no_data error for invalid SOM, got %d\n", err);
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_broadcast(struct osdp *ctx)
{
	uint8_t *buf;
	int len, err, pkt_len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t reply_data[] = { REPLY_ACK };
	uint8_t packet[32];
	uint8_t expected[] = { REPLY_ACK };

	printf(SUB_1 "Testing phy_decode_packet broadcast address -- ");

	/* Skip sequence check to focus on broadcast address handling */
	SET_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);

	/*
	 * A PD that received a broadcast command keeps the broadcast address
	 * (0x7f) in its reply and sets the reply bit, so the CP sees 0xff on
	 * the wire. The CP must accept it instead of rejecting it as an
	 * address mismatch.
	 */
	uint8_t control = 0x01; /* sequence 1, no CRC */
	pkt_len = test_osdp_create_packet(0xff, control, reply_data, 1, packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);
	if (err) {
		printf("failed! Broadcast reply should be accepted, got error %d\n", err);
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}

	if ((len = osdp_phy_decode_packet(p, &buf)) < 0) {
		printf("decode failed!\n");
		CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
		return -1;
	}

	CHECK_ARRAY(buf, len, expected);

	CLEAR_FLAG(p, PD_FLAG_SKIP_SEQ_CHECK);
	printf("success!\n");
	return 0;
}

int test_phy_decode_packet_foreign_cp_command(struct osdp *ctx)
{
	int err, pkt_len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t cmd_data[] = { CMD_POLL };
	uint8_t packet[32];

	printf(SUB_1 "Testing phy_decode_packet ignores a foreign CP's command -- ");

	/*
	 * A command-direction packet (address MSB clear) on the bus can only
	 * have come from a second CP, since a bus must have exactly one. The
	 * CP must skip it instead of mistaking it for a reply.
	 */
	pkt_len = test_osdp_create_packet(0x65, 0x01, cmd_data, 1, packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_SKIP) {
		printf("failed! Expected OSDP_ERR_PKT_SKIP, got %d\n", err);
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_packet_too_large(struct osdp *ctx)
{
	int err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0xff, 0xff, 0x04, 0x40, 0xe7, 0xa1
	};

	printf(SUB_1 "Testing phy_decode_packet with oversized length -- ");
	osdp_rb_push_buf(p->rx_rb, packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	if (err != OSDP_ERR_PKT_WAIT) {
		printf("failed! Expected wait for re-scan\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_packet_too_small(struct osdp *ctx)
{
	int err;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x06, 0x00, 0x04, 0x40, 0xe7, 0xa1
	};

	printf(SUB_1 "Testing phy_decode_packet with undersized length -- ");
	osdp_rb_push_buf(p->rx_rb, packet, sizeof(packet));
	err = osdp_phy_check_packet(p);
	/* Undersized packets might trigger wait for re-scan or format error */
	if (err != OSDP_ERR_PKT_WAIT && err != OSDP_ERR_PKT_FMT) {
		printf("failed! Expected wait or format error for undersized packet, got %d\n", err);
		return -1;
	}
	printf("success!\n");
	return 0;
}

int test_phy_build_packet_without_mark(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[512] = { CMD_POLL };

	printf(SUB_1 "Testing cp_build_and_send_packet without mark byte -- ");

	/* The issue might be that the flag needs to be set before packet_init */
	/* Let's just test that we can detect when mark byte is skipped */
	if ((len = test_cp_build_and_send_packet(p, packet, 1, 512)) < 0) {
		printf("failed to build packet!\n");
		return -1;
	}

#ifdef OPT_OSDP_SKIP_MARK_BYTE
	/* If mark byte is globally skipped, verify no mark byte */
	if (packet[0] != 0x53) {
		printf("failed! Expected no mark byte but got 0x%02x\n", packet[0]);
		return -1;
	}
	printf("success! (globally skip mark)\n");
#else
	/* If mark byte is globally enabled, packet should have mark byte */
	if (packet[0] != 0xff) {
		printf("failed! Expected mark byte but got 0x%02x\n", packet[0]);
		return -1;
	}
	printf("success! (mark byte present)\n");
#endif
	return 0;
}

int test_phy_packet_multiple_commands(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[512] = { CMD_BUZ, 0x02, 0x05, 0x0a, 0x14, 0x28 };

	printf(SUB_1 "Testing cp_build_and_send_packet(CMD_BUZ) -- ");
	if ((len = test_cp_build_and_send_packet(p, packet, 6, 512)) < 0) {
		printf("failed to build packet!\n");
		return -1;
	}

	/* Basic validation: check it has the right command and starts correctly */
#ifndef OPT_OSDP_SKIP_MARK_BYTE
	if (packet[0] != 0xff || packet[1] != 0x53) {
		printf("failed! Invalid packet header\n");
		return -1;
	}
	int data_offset = 6;
#else
	if (packet[0] != 0x53) {
		printf("failed! Invalid packet header\n");
		return -1;
	}
	int data_offset = 5;
#endif

	/* Check that the command is correctly placed */
	if (packet[data_offset] != CMD_BUZ) {
		printf("failed! Wrong command in packet (0x%02x)\n", packet[data_offset]);
		return -1;
	}

	/* Check that the parameter bytes are present */
	if (packet[data_offset + 1] != 0x02 || packet[data_offset + 2] != 0x05) {
		printf("failed! Wrong command parameters\n");
		return -1;
	}

	printf("success!\n");
	return 0;
}

int test_phy_packet_zero_data(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[512] = { CMD_POLL }; /* Command with no additional data */

	printf(SUB_1 "Testing packet with zero additional data -- ");
	if ((len = test_cp_build_and_send_packet(p, packet, 1, 512)) < 0) {
		printf("failed to build packet!\n");
		return -1;
	}

	/* Verify minimum packet size - adjust based on actual packet structure */
#ifndef OPT_OSDP_SKIP_MARK_BYTE
	int expected_min_len = 8; /* mark + header(5) + data(1) + checksum(1) = 8 minimum */
#else
	int expected_min_len = 7; /* header(5) + data(1) + checksum(1) = 7 minimum */
#endif

	if (len < expected_min_len) {
		printf("failed! Packet too small: %d < %d\n", len, expected_min_len);
		return -1;
	}

	printf("success!\n");
	return 0;
}

int test_phy_packet_data_offset(struct osdp *ctx)
{
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);
	uint8_t packet[32];
	uint8_t reply_data[] = { REPLY_ACK };
	int pkt_len, offset;

	printf(SUB_1 "Testing packet data offset calculation -- ");

	/* Create a simple packet */
	pkt_len = test_osdp_create_packet(0xe5, 0x00, reply_data, 1, packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to create packet!\n");
		return -1;
	}

	/* Test data offset calculation */
	offset = osdp_phy_packet_get_data_offset(p, packet);

#ifndef OPT_OSDP_SKIP_MARK_BYTE
	int expected_offset = 6; /* mark + header(5) */
#else
	int expected_offset = 5; /* header(5) */
#endif

	if (offset != expected_offset) {
		printf("failed! Wrong data offset: %d != %d\n", offset, expected_offset);
		return -1;
	}

	printf("success!\n");
	return 0;
}

int test_phy_packet_different_commands(struct osdp *ctx)
{
	int len;
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	reset_pd_packet_state(p);

	printf(SUB_1 "Testing different command types -- ");

	/* Test LED command */
	uint8_t led_packet[512] = { CMD_LED, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d };
	if ((len = test_cp_build_and_send_packet(p, led_packet, 15, 512)) < 0) {
		printf("failed to build LED packet!\n");
		return -1;
	}

	/* Reset for next test */
	reset_pd_packet_state(p);

	/* Test text output command */
	uint8_t text_packet[512] = { CMD_TEXT, 0x01, 0x02, 'H', 'e', 'l', 'l', 'o' };
	if ((len = test_cp_build_and_send_packet(p, text_packet, 8, 512)) < 0) {
		printf("failed to build TEXT packet!\n");
		return -1;
	}

	printf("success!\n");
	return 0;
}

int test_phy_state_reset_functionality(struct osdp *ctx)
{
	struct osdp_pd *p = GET_CURRENT_PD(ctx);

	printf(SUB_1 "Testing PHY state reset functionality -- ");

	/* Simulate some packet state */
	p->packet_buf_len = 10;
	p->packet_len = 20;
	p->phy_state = 5;

	/* Reset state */
	osdp_phy_state_reset(p, true);

	/* Verify reset */
	if (p->packet_buf_len != 0 || p->packet_len != 0 || p->phy_state != 0) {
		printf("failed! State not properly reset\n");
		return -1;
	}

	printf("success!\n");
	return 0;
}

int test_phy_state_reset_clears_rx_ring_on_error(struct osdp *ctx)
{
	struct osdp_pd *p = GET_CURRENT_PD(ctx);
	uint8_t stale[] = { 0xde, 0xad, 0xbe, 0xef, 0x53, 0x65, 0x01, 0x02 };
	uint8_t drain;

	printf(SUB_1 "Testing rx ring drain on abnormal phy reset -- ");

	/* Seed rx ring with stale/garbage bytes, as would happen after a
	 * rogue PD spews junk onto the bus and overflows the ring. */
	osdp_rb_push_buf(p->rx_rb, stale, sizeof(stale));
	if (p->rx_rb->head == p->rx_rb->tail) {
		printf("precondition broken: rx ring should not be empty\n");
		return -1;
	}

	/* Abnormal reset must leave no stale bytes behind for the next
	 * packet parse, otherwise the framer keeps chewing on garbage. */
	osdp_phy_state_reset(p, true);

	if (osdp_rb_pop(p->rx_rb, &drain) == 0) {
		printf("stale data still in rx ring after abnormal reset\n");
		return -1;
	}

	printf("success!\n");
	return 0;
}

int test_cp_refresh_yields_from_waiting_pd(struct osdp *ctx)
{
	struct refresh_yield_test_data data = {};
	struct osdp_channel channel = {
		.data = &data,
		.send = test_cp_refresh_yield_send,
		.recv = test_cp_refresh_yield_recv,
		.flush = NULL,
	};
	osdp_pd_info_t info[] = {
		{
			.address = 101,
			.baud_rate = 9600,
			.flags = 0,
			.scbk = NULL,
		},
		{
			.address = 102,
			.baud_rate = 9600,
			.flags = 0,
			.scbk = NULL,
		},
	};
	struct osdp *cp_ctx;
	struct osdp_pd *pd0, *pd1;

	ARG_UNUSED(ctx);

	printf(SUB_1 "Testing osdp_cp_refresh() yields during probe wait -- ");

	cp_ctx = (struct osdp *)osdp_cp_setup(&channel, 2, info);
	if (cp_ctx == NULL) {
		printf("setup failed!\n");
		return -1;
	}

	pd0 = osdp_to_pd(cp_ctx, 0);
	pd1 = osdp_to_pd(cp_ctx, 1);
	SET_CURRENT_PD(cp_ctx, 0);

	pd0->phy_state = OSDP_CP_PHY_STATE_WAIT;
	pd0->wait_ms = OSDP_CMD_RETRY_WAIT_MS;
	pd0->phy_tstamp = osdp_millis_now();

	osdp_cp_refresh((osdp_t *)cp_ctx);
	if (GET_CURRENT_PD(cp_ctx) != pd1 ||
	    pd1->phy_state != OSDP_CP_PHY_STATE_SEND_CMD ||
	    data.send_count != 0) {
		printf("scheduler did not advance to next PD\n");
		osdp_cp_teardown((osdp_t *)cp_ctx);
		return -1;
	}

	osdp_cp_refresh((osdp_t *)cp_ctx);
	if (data.send_count != 1 || data.last_pd_address != pd1->address) {
		printf("wrong PD used after yielding (%d, addr=%d)\n",
		       data.send_count, data.last_pd_address);
		osdp_cp_teardown((osdp_t *)cp_ctx);
		return -1;
	}

	osdp_cp_teardown((osdp_t *)cp_ctx);
	printf("success!\n");
	return 0;
}

int test_cp_refresh_yields_between_probe_retries(struct osdp *ctx)
{
	struct refresh_yield_test_data data = {};
	struct osdp_channel channel = {
		.data = &data,
		.send = test_cp_refresh_yield_send,
		.recv = test_cp_refresh_yield_recv,
		.flush = NULL,
	};
	osdp_pd_info_t info[] = {
		{
			.address = 101,
			.baud_rate = 9600,
			.flags = 0,
			.scbk = NULL,
		},
		{
			.address = 102,
			.baud_rate = 9600,
			.flags = 0,
			.scbk = NULL,
		},
	};
	struct osdp *cp_ctx;
	struct osdp_pd *pd0, *pd1;

	ARG_UNUSED(ctx);

	printf(SUB_1 "Testing osdp_cp_refresh() yields between probe retries -- ");

	cp_ctx = (struct osdp *)osdp_cp_setup(&channel, 2, info);
	if (cp_ctx == NULL) {
		printf("setup failed!\n");
		return -1;
	}

	pd0 = osdp_to_pd(cp_ctx, 0);
	pd1 = osdp_to_pd(cp_ctx, 1);
	SET_CURRENT_PD(cp_ctx, 0);

	pd0->phy_state = OSDP_CP_PHY_STATE_WAIT;
	pd0->cmd_id = CMD_POLL;
	pd0->phy_retry_count = 1;
	pd0->wait_ms = OSDP_CMD_RETRY_WAIT_MS;
	pd0->phy_tstamp = osdp_millis_now() - pd0->wait_ms;

	osdp_cp_refresh((osdp_t *)cp_ctx);
	if (GET_CURRENT_PD(cp_ctx) != pd1 ||
	    pd0->phy_state != OSDP_CP_PHY_STATE_RETRY_CMD ||
	    pd1->phy_state != OSDP_CP_PHY_STATE_SEND_CMD ||
	    data.send_count != 0) {
		printf("scheduler did not yield retry probe to next PD\n");
		osdp_cp_teardown((osdp_t *)cp_ctx);
		return -1;
	}

	osdp_cp_refresh((osdp_t *)cp_ctx);
	if (GET_CURRENT_PD(cp_ctx) != pd1 ||
	    pd1->phy_state != OSDP_CP_PHY_STATE_REPLY_WAIT ||
	    data.send_count != 1 ||
	    data.last_pd_address != pd1->address) {
		printf("next PD did not get the channel after yield\n");
		osdp_cp_teardown((osdp_t *)cp_ctx);
		return -1;
	}

	osdp_cp_teardown((osdp_t *)cp_ctx);
	printf("success!\n");
	return 0;
}

int test_cp_refresh_retries_on_next_turn_for_single_pd(struct osdp *ctx)
{
	struct refresh_yield_test_data data = {};
	struct osdp_channel channel = {
		.data = &data,
		.send = test_cp_refresh_yield_send,
		.recv = test_cp_refresh_yield_recv,
		.flush = NULL,
	};
	osdp_pd_info_t info = {
		.address = 101,
		.baud_rate = 9600,
		.flags = 0,
		.scbk = NULL,
	};
	struct osdp *cp_ctx;
	struct osdp_pd *pd0;

	ARG_UNUSED(ctx);

	printf(SUB_1 "Testing osdp_cp_refresh() retries on next turn for single PD -- ");

	cp_ctx = (struct osdp *)osdp_cp_setup(&channel, 1, &info);
	if (cp_ctx == NULL) {
		printf("setup failed!\n");
		return -1;
	}

	pd0 = osdp_to_pd(cp_ctx, 0);
	SET_CURRENT_PD(cp_ctx, 0);

	pd0->phy_state = OSDP_CP_PHY_STATE_WAIT;
	pd0->cmd_id = CMD_POLL;
	pd0->phy_retry_count = 1;
	pd0->wait_ms = OSDP_CMD_RETRY_WAIT_MS;
	pd0->phy_tstamp = osdp_millis_now() - pd0->wait_ms;

	osdp_cp_refresh((osdp_t *)cp_ctx);
	if (GET_CURRENT_PD(cp_ctx) != pd0 ||
	    pd0->phy_state != OSDP_CP_PHY_STATE_RETRY_CMD ||
	    data.send_count != 0) {
		printf("single PD retry was not deferred to next turn\n");
		osdp_cp_teardown((osdp_t *)cp_ctx);
		return -1;
	}

	osdp_cp_refresh((osdp_t *)cp_ctx);
	if (GET_CURRENT_PD(cp_ctx) != pd0 ||
	    pd0->phy_state != OSDP_CP_PHY_STATE_REPLY_WAIT ||
	    data.send_count != 1 ||
	    data.last_pd_address != pd0->address) {
		printf("single PD retry was not sent on next turn\n");
		osdp_cp_teardown((osdp_t *)cp_ctx);
		return -1;
	}

	osdp_cp_teardown((osdp_t *)cp_ctx);
	printf("success!\n");
	return 0;
}

int test_cp_phy_setup(struct test *t)
{
	/* mock application data */
	struct osdp_channel channel = {
		.data = NULL,
		.send = NULL,
		.recv = NULL,
		.flush = NULL,
	};
	osdp_pd_info_t info = {
		.address = 101,
		.baud_rate = 9600,
		.flags = 0,
		.scbk = NULL,
	};
#ifndef OPT_OSDP_LOG_MINIMAL
	osdp_logger_init("osdp::cp", t->loglevel, NULL);
#endif /* OPT_OSDP_LOG_MINIMAL */
	struct osdp *ctx = (struct osdp *)osdp_cp_setup(&channel, 1, &info);
	if (ctx == NULL) {
		printf(SUB_1 "init failed!\n");
		return -1;
	}
	SET_CURRENT_PD(ctx, 0);
	t->mock_data = (void *)ctx;
	return 0;
}

void test_cp_phy_teardown(struct test *t)
{
	osdp_cp_teardown(t->mock_data);
}

void run_cp_phy_tests(struct test *t)
{
	printf("\nStarting cp_phy tests\n");

	if (test_cp_phy_setup(t))
		return;

	DO_TEST(t, test_cp_build_packet_poll);
	DO_TEST(t, test_cp_build_packet_id);
	DO_TEST(t, test_cp_build_packet_chlng);
	DO_TEST(t, test_cp_build_packet_with_crc);
	DO_TEST(t, test_cp_build_packet_checksum_mode_trailer);
	DO_TEST(t, test_cp_build_packet_crc_mode_trailer);
	DO_TEST(t, test_phy_parse_hardcoded_plain_checksum_ack);
	DO_TEST(t, test_phy_parse_hardcoded_plain_crc_nak);
	DO_TEST(t, test_phy_parse_hardcoded_sc_scs12_checksum_ccrypt);
	DO_TEST(t, test_phy_parse_hardcoded_sc_scs14_crc_rmaci);
	DO_TEST(t, test_phy_parse_hardcoded_sc_invalid_type_reject);
	DO_TEST(t, test_phy_parse_hardcoded_sc_inactive_scs16_reject);
	DO_TEST(t, test_phy_parse_hardcoded_sc_active_invalid_mac_reject);
	DO_TEST(t, test_phy_parse_hardcoded_plaintext_when_sc_active_reject);
	DO_TEST(t, test_phy_build_packet_without_mark);
	DO_TEST(t, test_phy_packet_multiple_commands);
	DO_TEST(t, test_phy_decode_packet_ack);
	DO_TEST(t, test_phy_decode_packet_nak);
	DO_TEST(t, test_phy_decode_packet_busy);
	DO_TEST(t, test_phy_decode_packet_ignore_leading_mark_bytes);
	DO_TEST(t, test_phy_decode_packet_invalid_checksum);
	DO_TEST(t, test_phy_decode_packet_invalid_crc);
	DO_TEST(t, test_phy_decode_packet_wrong_address);
	DO_TEST(t, test_phy_decode_packet_sequence_mismatch);
	DO_TEST(t, test_phy_decode_packet_invalid_som);
	DO_TEST(t, test_phy_decode_packet_broadcast);
	DO_TEST(t, test_phy_decode_packet_foreign_cp_command);
	DO_TEST(t, test_phy_packet_too_large);
	DO_TEST(t, test_phy_packet_too_small);
	DO_TEST(t, test_phy_packet_zero_data);
	DO_TEST(t, test_phy_packet_data_offset);
	DO_TEST(t, test_phy_packet_different_commands);
	DO_TEST(t, test_phy_state_reset_functionality);
	DO_TEST(t, test_phy_state_reset_clears_rx_ring_on_error);
	DO_TEST(t, test_cp_refresh_yields_from_waiting_pd);
	DO_TEST(t, test_cp_refresh_yields_between_probe_retries);
	DO_TEST(t, test_cp_refresh_retries_on_next_turn_for_single_pd);

	printf(SUB_1 "cp_phy tests %s\n", t->failure == 0 ? "succeeded" : "failed");

	test_cp_phy_teardown(t);
}

#else /* OPT_OSDP_RX_ZERO_COPY */

void run_cp_phy_tests(struct test *t)
{
	ARG_UNUSED(t);
	printf(SUB_1 "cp_phy skipped: byte-stream framer path is non-zero-copy\n");
}

#endif /* OPT_OSDP_RX_ZERO_COPY */
