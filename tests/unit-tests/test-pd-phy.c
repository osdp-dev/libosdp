/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
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

	test_pd_phy_teardown(t);
}

#else /* OPT_OSDP_RX_ZERO_COPY */

void run_pd_phy_tests(struct test *t)
{
	ARG_UNUSED(t);
	printf(SUB_1 "pd_phy skipped: byte-stream framer path is non-zero-copy\n");
}

#endif /* OPT_OSDP_RX_ZERO_COPY */
