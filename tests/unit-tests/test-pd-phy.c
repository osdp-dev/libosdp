/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file PD-mode phy tests. Covers the sequence-repeat cached-reply retransmit
 * path that keeps the SC MAC chain from being corrupted when the CP retries a
 * command it believes the PD did not reply to.
 */

#include "test.h"

/* These tests push raw bytes into pd->rx_rb and drive osdp_phy_check_packet
 * directly. The ring buffer and byte-stream framer only exist without
 * OPT_OSDP_RX_ZERO_COPY; under zero-copy the channel delivers pre-framed
 * packets, so the seq-repeat cache path is exercised by the loopback suites
 * instead. */
#ifndef OPT_OSDP_RX_ZERO_COPY

extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);
extern uint8_t test_osdp_compute_checksum(uint8_t *msg, int length);

#define PD_TEST_ADDR 101

/* Captures bytes the PD pushes to its send channel so retransmitted
 * replies can be byte-compared against the cached tx_buf. */
#define TX_CAPTURE_MAX 256
static uint8_t g_tx_capture[TX_CAPTURE_MAX];
static int g_tx_capture_len;

static void tx_capture_reset(void)
{
	memset(g_tx_capture, 0, sizeof(g_tx_capture));
	g_tx_capture_len = 0;
}

static int pd_mock_send(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);
	if (len > TX_CAPTURE_MAX) {
		len = TX_CAPTURE_MAX;
	}
	memcpy(g_tx_capture, buf, len);
	g_tx_capture_len = len;
	return len;
}

static int pd_mock_recv(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return 0;
}

static void pd_mock_flush(void *data)
{
	ARG_UNUSED(data);
}

/* Build a CP->PD packet directly, bypassing the normal phy packet builder
 * so we control the sequence number and cmd_id precisely. */
static int pd_test_build_cmd_packet(uint8_t control, uint8_t cmd_id,
				    const uint8_t *data, int data_len,
				    uint8_t *out, int max_len)
{
	int len = 0;
	bool use_mark = true;
	bool use_crc = (control & 0x04) != 0;
	uint16_t crc16;
	uint8_t checksum;
	int pkt_len;

#ifdef OPT_OSDP_SKIP_MARK_BYTE
	use_mark = false;
#endif

	pkt_len = 5 + 1 + data_len + (use_crc ? 2 : 1);
	if (max_len < pkt_len + (use_mark ? 1 : 0)) {
		return -1;
	}

	if (use_mark) {
		out[len++] = 0xff;
	}
	out[len++] = 0x53;              /* SOM */
	out[len++] = PD_TEST_ADDR;       /* addr, MSB clear for CP->PD */
	out[len++] = pkt_len & 0xff;
	out[len++] = (pkt_len >> 8) & 0xff;
	out[len++] = control;
	out[len++] = cmd_id;
	if (data && data_len > 0) {
		memcpy(out + len, data, data_len);
		len += data_len;
	}

	if (use_crc) {
		crc16 = test_osdp_compute_crc16(out + (use_mark ? 1 : 0),
						len - (use_mark ? 1 : 0));
		out[len++] = crc16 & 0xff;
		out[len++] = (crc16 >> 8) & 0xff;
	} else {
		checksum = test_osdp_compute_checksum(out + (use_mark ? 1 : 0),
						      len - (use_mark ? 1 : 0));
		out[len++] = checksum;
	}
	return len;
}

/* Drop the PD back into a clean post-reply state and seed the retransmit
 * cache with a distinct byte pattern so we can recognise it on the wire. */
static void pd_prime_cached_reply(struct osdp_pd *p,
				  const uint8_t *pattern, int pattern_len,
				  uint8_t cmd_id, int seq_number)
{
	uint8_t *tx = osdp_tx_staging_buf(p);

	osdp_phy_state_reset(p, true);
	sc_deactivate(p);
	p->seq_number = seq_number;
	memcpy(tx, pattern, pattern_len);
	p->last_tx_len = (uint16_t)pattern_len;
	p->last_cmd_id = cmd_id;
}

/* Verify that a sequence-repeat command with a matching cmd_id causes
 * the PD to push the exact cached reply bytes back onto the wire without
 * re-entering the decode or reply-builder code paths. */
static int test_pd_phy_retransmit_cached_reply(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	const uint8_t pattern[] = {
		0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe,
		0x11, 0x22, 0x33, 0x44
	};
	uint8_t packet[32];
	int pkt_len, err;

	printf(SUB_1 "Testing PD seq-repeat retransmits cached reply -- ");

	pd_prime_cached_reply(p, pattern, sizeof(pattern), CMD_POLL, 1);

	pkt_len = pd_test_build_cmd_packet(0x01 /* seq=1, no CRC, no SCB */,
					   CMD_POLL, NULL, 0,
					   packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to build packet\n");
		return -1;
	}

	tx_capture_reset();
	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);

	if (err != OSDP_ERR_PKT_SKIP) {
		printf("expected OSDP_ERR_PKT_SKIP, got %d\n", err);
		return -1;
	}
	if (g_tx_capture_len != (int)sizeof(pattern) ||
	    memcmp(g_tx_capture, pattern, sizeof(pattern)) != 0) {
		printf("retransmitted bytes mismatch\n");
		hexdump(pattern, sizeof(pattern), SUB_1 "Expected");
		hexdump(g_tx_capture, g_tx_capture_len, SUB_1 "Found");
		return -1;
	}
	/* Cache must be preserved so a second repeat still works. */
	if (p->last_tx_len != sizeof(pattern) ||
	    p->last_cmd_id != CMD_POLL) {
		printf("cache was mutated by retransmit path\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

/* A sequence-repeat whose cmd_id does not match the cached reply must
 * NOT be answered from the cache. The cache is dropped and the packet
 * falls through to the normal handling path. */
static int test_pd_phy_retransmit_cmd_id_mismatch(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	const uint8_t pattern[] = { 0xaa, 0xbb, 0xcc, 0xdd };
	uint8_t packet[32];
	int pkt_len, err;

	printf(SUB_1 "Testing PD seq-repeat with cmd_id mismatch drops cache -- ");

	pd_prime_cached_reply(p, pattern, sizeof(pattern), CMD_POLL, 1);

	/* Same sequence number but a different command id. */
	pkt_len = pd_test_build_cmd_packet(0x01, CMD_ID, (uint8_t[]){ 0x00 }, 1,
					   packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to build packet\n");
		return -1;
	}

	tx_capture_reset();
	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);

	/* The retransmit path must not have executed. */
	if (g_tx_capture_len != 0) {
		printf("PD retransmitted cached reply on cmd_id mismatch\n");
		return -1;
	}
	/* Cache must be invalidated. */
	if (p->last_tx_len != 0) {
		printf("cache not cleared on cmd_id mismatch (last_tx_len=%u)\n",
		       p->last_tx_len);
		return -1;
	}
	/* err may be OSDP_ERR_PKT_NONE (packet fell through to normal
	 * processing) or an error from the downstream path. Either way the
	 * retransmit path was not taken, which is what we are asserting. */
	ARG_UNUSED(err);
	printf("success!\n");
	return 0;
}

/* When the CP sends a seq=0 packet to restart communication the PD resets
 * sequence state, deactivates SC, and must also invalidate any cached
 * reply left over from the previous conversation. */
static int test_pd_phy_seq_zero_clears_cache(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	const uint8_t pattern[] = { 0x01, 0x02, 0x03, 0x04 };
	uint8_t packet[32];
	int pkt_len, err;

	printf(SUB_1 "Testing PD seq=0 restart clears retransmit cache -- ");

	pd_prime_cached_reply(p, pattern, sizeof(pattern), CMD_POLL, 1);

	pkt_len = pd_test_build_cmd_packet(0x00 /* seq=0 */, CMD_POLL, NULL, 0,
					   packet, sizeof(packet));
	if (pkt_len < 0) {
		printf("failed to build packet\n");
		return -1;
	}

	tx_capture_reset();
	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);
	ARG_UNUSED(err);

	if (g_tx_capture_len != 0) {
		printf("unexpected retransmit on seq=0 restart\n");
		return -1;
	}
	if (p->last_tx_len != 0) {
		printf("cache not cleared on seq=0 restart\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

/* sc_deactivate() is the teardown path shared by error handling and
 * MAC-failure rejection. It must wipe the retransmit cache so a stale
 * reply from a defunct session cannot be replayed. */
static int test_pd_phy_sc_deactivate_clears_cache(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	const uint8_t pattern[] = { 0xff, 0xee, 0xdd };

	printf(SUB_1 "Testing sc_deactivate() clears retransmit cache -- ");

	pd_prime_cached_reply(p, pattern, sizeof(pattern), CMD_POLL, 2);
	/* pd_prime_cached_reply already calls sc_deactivate once; re-seed
	 * the cache directly to isolate what this test is asserting. */
	p->last_tx_len = (uint16_t)sizeof(pattern);
	p->last_cmd_id = CMD_POLL;

	sc_deactivate(p);

	if (p->last_tx_len != 0) {
		printf("sc_deactivate did not clear cache\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

/* osdp_sc_setup() runs at the start of every SC handshake and must
 * invalidate any cached reply so it cannot cross a session boundary. */
static int test_pd_phy_sc_setup_clears_cache(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	const uint8_t pattern[] = { 0x55, 0x66, 0x77, 0x88 };

	printf(SUB_1 "Testing osdp_sc_setup() clears retransmit cache -- ");

	pd_prime_cached_reply(p, pattern, sizeof(pattern), CMD_POLL, 3);
	p->last_tx_len = (uint16_t)sizeof(pattern);
	p->last_cmd_id = CMD_POLL;

	osdp_sc_setup(p);

	if (p->last_tx_len != 0) {
		printf("osdp_sc_setup did not clear cache\n");
		return -1;
	}
	printf("success!\n");
	return 0;
}

struct sc_notices {
	int count;
	bool last_active;
};

static int record_sc_notice(void *arg, struct osdp_cmd *cmd)
{
	struct sc_notices *n = arg;

	if (cmd->id == OSDP_CMD_NOTIFICATION &&
	    cmd->notif.type == OSDP_NOTIFICATION_SC_STATUS) {
		n->count++;
		n->last_active = cmd->notif.sc_status.active;
	}
	return 0;
}

/* Feed one command to a PD with a live session through a full refresh and
 * report the SC notices the app was given. */
static int pd_refresh_with_session(struct osdp *ctx, int seq_number,
				   const uint8_t *packet, int len,
				   struct sc_notices *notices)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);

	osdp_phy_state_reset(p, true);
	p->seq_number = seq_number;
	sc_activate(p);
	p->sc_tstamp = osdp_millis_now();
	p->tstamp = osdp_millis_now();
	SET_FLAG(p, PD_FLAG_ENABLE_NOTIF);
	/* osdp_pd_set_command_callback() is refused once the PD has been
	 * refreshed, and every case here refreshes it. */
	p->command_callback = record_sc_notice;
	p->command_callback_arg = notices;

	osdp_rb_push_buf(p->rx_rb, (uint8_t *)packet, len);
	osdp_pd_refresh((osdp_t *)ctx);

	p->command_callback = NULL;
	p->command_callback_arg = NULL;
	CLEAR_FLAG(p, PD_FLAG_ENABLE_NOTIF);
	return sc_is_active(p) ? -1 : 0;
}

static int test_pd_phy_seq_zero_notifies_sc_down(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	struct sc_notices notices = { 0 };
	uint8_t packet[32];
	int pkt_len;

	printf(SUB_1
	       "Testing PD notifies SC down when the CP restarts at seq=0 -- ");

	pkt_len = pd_test_build_cmd_packet(0x00 /* seq=0 */, CMD_POLL, NULL, 0,
					   packet, sizeof(packet));
	if (pd_refresh_with_session(ctx, 2, packet, pkt_len, &notices) ||
	    notices.count != 1 || notices.last_active) {
		printf("failed! sc=%d notices=%d last_active=%d\n",
		       sc_is_active(p), notices.count, notices.last_active);
		return -1;
	}
	printf("success!\n");
	return 0;
}

static int test_pd_phy_bad_mac_notifies_sc_down(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	struct sc_notices notices = { 0 };
	/* After the header: a 2-byte security block (SCS_15), the command
	 * and a MAC that cannot match the session's. */
	const uint8_t sec_block_tail[] = { SCS_15, CMD_POLL, 0, 0, 0, 0 };
	uint8_t packet[32];
	int pkt_len;

	printf(SUB_1 "Testing PD notifies SC down on a bad command MAC -- ");

	pkt_len =
		pd_test_build_cmd_packet(0x09 /* SCB, seq=1 */, 2,
					 sec_block_tail, sizeof(sec_block_tail),
					 packet, sizeof(packet));
	if (pd_refresh_with_session(ctx, 0, packet, pkt_len, &notices) ||
	    notices.count != 1 || notices.last_active) {
		printf("failed! sc=%d notices=%d last_active=%d\n",
		       sc_is_active(p), notices.count, notices.last_active);
		return -1;
	}
	printf("success!\n");
	return 0;
}

static int test_pd_phy_setup(struct test *t)
{
	static uint8_t scbk[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
	};
	static struct osdp_pd_cap cap[] = {
		{ OSDP_PD_CAP_READER_LED_CONTROL, 1, 1 },
		{ -1, -1, -1 }
	};
	struct osdp_channel channel = {
		.data = NULL,
		.send = pd_mock_send,
		.recv = pd_mock_recv,
		.flush = pd_mock_flush,
	};
	osdp_pd_info_t info = {
		.address = PD_TEST_ADDR,
		.baud_rate = 9600,
		.flags = 0,
		.id = {
			.version = 1,
			.model = 153,
			.vendor_code = 31337,
			.serial_number = 0x01020304,
			.firmware_version = 0x0A0B0C0D,
		},
		.cap = cap,
		.scbk = scbk,
	};
	struct osdp *ctx;

#ifndef OPT_OSDP_LOG_MINIMAL
	osdp_logger_init("osdp::pd", t->loglevel, NULL);
#else
	ARG_UNUSED(t);
#endif

	ctx = (struct osdp *)osdp_pd_setup(&channel, &info);
	if (ctx == NULL) {
		printf(SUB_1 "pd init failed!\n");
		return -1;
	}
	t->mock_data = (void *)ctx;
	return 0;
}

static void test_pd_phy_teardown(struct test *t)
{
	osdp_pd_teardown(t->mock_data);
	t->mock_data = NULL;
}

/* Build a raw CP->PD frame whose length field is set to `declared_len`
 * regardless of how many bytes we actually emit, so the framer's bound
 * check can be probed at the buffer-capacity boundary. */
static int pd_test_build_sized_packet(bool use_mark, int declared_len,
				      uint8_t *out, int max_len)
{
	int len = 0, body;
	uint8_t checksum;

	if (max_len < declared_len + (use_mark ? 1 : 0)) {
		return -1;
	}
	if (use_mark) {
		out[len++] = 0xff;
	}
	out[len++] = 0x53; /* SOM */
	out[len++] = PD_TEST_ADDR; /* addr, MSB clear for CP->PD */
	out[len++] = declared_len & 0xff;
	out[len++] = (declared_len >> 8) & 0xff;
	out[len++] = 0x00; /* control: seq 0, no CRC, no SCB */

	/* Pad the body out to the declared length, leaving room for the
	 * trailing checksum byte. The 5 accounts for the header bytes emitted
	 * above; the length field counts them but not the mark byte. */
	body = declared_len - 5 - 1;
	memset(out + len, 0x00, body);
	len += body;

	checksum = test_osdp_compute_checksum(out + (use_mark ? 1 : 0),
					      len - (use_mark ? 1 : 0));
	out[len++] = checksum;
	return len;
}

/* A packet that declares the full buffer capacity *behind a mark byte* needs
 * one more byte than the buffer holds, because the mark is buffered with the
 * packet but not counted in its length field. The framer must reject it
 * rather than overrun packet_buf by a byte. */
static int test_pd_phy_max_len_with_mark_rejected(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	uint8_t packet[OSDP_PACKET_BUF_SIZE + 1];
	int pkt_len, err;

	printf(SUB_1
	       "Testing max-length packet behind a mark byte is rejected -- ");

	osdp_phy_state_reset(p, true);
	sc_deactivate(p);
	osdp_rb_reset(p->rx_rb);

	pkt_len = pd_test_build_sized_packet(true, OSDP_PACKET_BUF_SIZE, packet,
					     sizeof(packet));
	if (pkt_len != OSDP_PACKET_BUF_SIZE + 1) {
		printf("failed to build packet\n");
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	err = osdp_phy_check_packet(p);

	if (err == OSDP_ERR_PKT_NONE) {
		printf("framer accepted an over-long packet\n");
		return -1;
	}
	if (p->packet_len > OSDP_PACKET_BUF_SIZE) {
		printf("packet_len %lu exceeds buffer capacity %d\n",
		       p->packet_len, OSDP_PACKET_BUF_SIZE);
		return -1;
	}
	printf("success!\n");
	return 0;
}

/* The PD advertises OSDP_PACKET_BUF_SIZE as its receive buffer size, so a
 * packet of exactly that length must still be framed when no mark byte eats
 * into the buffer. Guards the check above from being tightened into a silent
 * loss of one byte of advertised capacity. */
static int test_pd_phy_max_len_without_mark_accepted(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	uint8_t packet[OSDP_PACKET_BUF_SIZE + 1];
	int pkt_len;

	printf(SUB_1
	       "Testing max-length packet without a mark byte is framed -- ");

	osdp_phy_state_reset(p, true);
	sc_deactivate(p);
	osdp_rb_reset(p->rx_rb);

	pkt_len = pd_test_build_sized_packet(false, OSDP_PACKET_BUF_SIZE,
					     packet, sizeof(packet));
	if (pkt_len != OSDP_PACKET_BUF_SIZE) {
		printf("failed to build packet\n");
		return -1;
	}

	osdp_rb_push_buf(p->rx_rb, packet, pkt_len);
	osdp_phy_check_packet(p);

	if (p->packet_len != OSDP_PACKET_BUF_SIZE) {
		printf("framer did not accept a full-capacity packet; "
		       "packet_len %lu\n",
		       p->packet_len);
		return -1;
	}
	printf("success!\n");
	return 0;
}

extern int test_pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len);

/*
 * An ACU receive size too small to build any reply in is refused: taking it
 * would leave the PD unable to answer anything after it.
 */
static int test_pd_acu_rx_size_below_minimum_refused(struct osdp *ctx)
{
	struct osdp_pd *p = osdp_to_pd(ctx, 0);
	uint8_t too_small[] = { CMD_ACURXSIZE, 0x10, 0x00 };
	uint8_t minimum[] = { CMD_ACURXSIZE, OSDP_MINIMUM_PACKET_SIZE & 0xff,
			      OSDP_MINIMUM_PACKET_SIZE >> 8 };
	int rc = -1;

	printf(SUB_1 "Testing a too-small ACU receive size is refused -- ");

	p->peer_rx_size = 0;
	test_pd_decode_command(p, too_small, sizeof(too_small));
	if (p->reply_id != REPLY_NAK || p->nak_code != OSDP_PD_NAK_RECORD ||
	    p->peer_rx_size != 0) {
		printf("accepted %u bytes\n", p->peer_rx_size);
		goto out;
	}

	test_pd_decode_command(p, minimum, sizeof(minimum));
	if (p->reply_id != REPLY_ACK ||
	    p->peer_rx_size != OSDP_MINIMUM_PACKET_SIZE) {
		printf("refused the minimum size\n");
		goto out;
	}
	printf("success!\n");
	rc = 0;
out:
	p->peer_rx_size = 0;
	return rc;
}

void run_pd_phy_tests(struct test *t)
{
	printf("\nStarting pd_phy tests\n");

	if (test_pd_phy_setup(t)) {
		return;
	}

	DO_TEST(t, test_pd_phy_retransmit_cached_reply);
	DO_TEST(t, test_pd_phy_retransmit_cmd_id_mismatch);
	DO_TEST(t, test_pd_phy_seq_zero_clears_cache);
	DO_TEST(t, test_pd_phy_sc_deactivate_clears_cache);
	DO_TEST(t, test_pd_phy_sc_setup_clears_cache);
	DO_TEST(t, test_pd_phy_seq_zero_notifies_sc_down);
	DO_TEST(t, test_pd_phy_bad_mac_notifies_sc_down);
	DO_TEST(t, test_pd_phy_max_len_with_mark_rejected);
	DO_TEST(t, test_pd_phy_max_len_without_mark_accepted);
	DO_TEST(t, test_pd_acu_rx_size_below_minimum_refused);

	test_pd_phy_teardown(t);
}

#else /* OPT_OSDP_RX_ZERO_COPY */

void run_pd_phy_tests(struct test *t)
{
	ARG_UNUSED(t);
	printf(SUB_1 "pd_phy skipped: byte-stream framer path is non-zero-copy\n");
}

#endif /* OPT_OSDP_RX_ZERO_COPY */
