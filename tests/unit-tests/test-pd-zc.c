/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file Zero-copy-specific PD tests. Only compiled into the zero-copy test
 * binary (osdp_unit_test_zc); the general suites (commands, events, file_tx,
 * sc, ...) also run under zero-copy via the packet-queue mock channel in
 * test.c, this file pins behaviours unique to the OPT_OSDP_RX_ZERO_COPY path.
 *
 * Regression pinned here: a status reply is prebuilt into the TX staging
 * buffer during command dispatch (pd_prebuild_status_reply re-homes
 * pd->packet_buf there *before* the RX teardown runs). osdp_phy_release_packet()
 * used to null pd->packet_buf unconditionally, so pd_send_reply() handed a NULL
 * buffer to the channel and the finalized LSTATR/ISTATR/OSTATR/RSTATR frame
 * never went on the wire. ACK/NAK replies are immune because they are built
 * *after* the teardown by pd_build_reply_packet(), which re-homes packet_buf
 * itself. The test drives a real osdp_pd_refresh() and asserts the LSTATR reply
 * reaches the send channel.
 */

#include "test.h"

#ifdef OPT_OSDP_RX_ZERO_COPY

extern uint8_t test_osdp_compute_checksum(uint8_t *msg, int length);

/* Wire framing bytes (OSDP_PKT_MARK/SOM are file-local to osdp_phy.c). */
#define WIRE_MARK       0xff
#define WIRE_SOM        0x53
#define PD_TEST_ADDR    101
#define STATUS_TAMPER   0xa5
#define STATUS_POWER    0x5a

/* One-shot zero-copy RX channel: hands the PD a single hand-built command. */
static uint8_t g_cmd[64];
static int g_cmd_len;
static bool g_cmd_pending;

/* TX capture. Mirrors the production uart_send NULL-buffer guard so the bug
 * (NULL packet_buf reaching the channel) is observable as "nothing captured". */
static uint8_t g_tx[256];
static int g_tx_len;

static int zc_recv_pkt(void *data, const uint8_t **buf, int *max_len)
{
	ARG_UNUSED(data);
	if (!g_cmd_pending) {
		return -1;
	}
	*buf = g_cmd;
	*max_len = g_cmd_len;
	return 0;
}

static void zc_release_pkt(void *data, const uint8_t *buf)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	g_cmd_pending = false;
}

static int zc_send(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);
	if (buf == NULL || len <= 0) {
		return -1; /* same rejection the production uart_send does */
	}
	if (len > (int)sizeof(g_tx)) {
		len = (int)sizeof(g_tx);
	}
	memcpy(g_tx, buf, len);
	g_tx_len = len;
	return len;
}

static void zc_flush(void *data)
{
	ARG_UNUSED(data);
}

static int pd_command_cb(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);
	if (cmd->id != OSDP_CMD_STATUS ||
	    cmd->status.type != OSDP_STATUS_REPORT_LOCAL) {
		return -1;
	}
	cmd->status.nr_entries = 2;
	cmd->status.report[0] = STATUS_TAMPER;
	cmd->status.report[1] = STATUS_POWER;
	return 0;
}

/* Build a mark + checksum LSTAT command with seq=0 so a freshly set-up PD
 * accepts it regardless of its initial sequence state. */
static int build_lstat_cmd(uint8_t *out)
{
	int len = 0, cksum_start;
	int body_len = 5 /* header */ + 1 /* cmd_id */ + 1 /* checksum */;

	out[len++] = WIRE_MARK;
	cksum_start = len;
	out[len++] = WIRE_SOM;
	out[len++] = PD_TEST_ADDR;          /* MSB clear: CP -> PD */
	out[len++] = body_len & 0xff;
	out[len++] = (body_len >> 8) & 0xff;
	out[len++] = 0x00;                  /* control: seq=0, no CRC, no SCB */
	out[len++] = CMD_LSTAT;
	out[len] = test_osdp_compute_checksum(out + cksum_start,
					      len - cksum_start);
	len++;
	return len;
}

static int find_som(const uint8_t *buf, int len)
{
	int i;

	for (i = 0; i + 1 < len; i++) {
		if (buf[i] == WIRE_SOM &&
		    buf[i + 1] == (uint8_t)(PD_TEST_ADDR | 0x80)) {
			return i;
		}
	}
	return -1;
}

/* Feed one LSTAT command and assert the LSTATR reply reaches the wire. With
 * the release-packet bug the send channel is handed a NULL buffer and captures
 * nothing; with the fix the full LSTATR frame is captured. */
static int test_pd_zc_status_reply_reaches_wire(void *mock_data)
{
	osdp_t *ctx = mock_data;
	int som;

	printf(SUB_1 "Testing zero-copy status reply reaches the wire -- ");

	g_cmd_len = build_lstat_cmd(g_cmd);
	g_cmd_pending = true;
	g_tx_len = 0;

	/* One refresh dispatches + replies; the rest are no-ops (recv_pkt
	 * reports no data) and must not clobber the captured reply. */
	for (int i = 0; i < 4; i++) {
		osdp_pd_refresh(ctx);
	}

	if (g_tx_len == 0) {
		printf("no reply on the wire (status reply lost)\n");
		return -1;
	}
	som = find_som(g_tx, g_tx_len);
	if (som < 0) {
		printf("captured %d bytes but no PD reply header\n", g_tx_len);
		return -1;
	}
	/* Header is 5 bytes; an unsecured reply carries no SCB, so the reply id
	 * is the first data byte and the status payload follows it. */
	if (g_tx[som + 5] != REPLY_LSTATR) {
		printf("expected LSTATR(0x%02x), got 0x%02x\n",
		       REPLY_LSTATR, g_tx[som + 5]);
		return -1;
	}
	if (g_tx[som + 6] != STATUS_TAMPER || g_tx[som + 7] != STATUS_POWER) {
		printf("LSTATR payload mismatch: 0x%02x 0x%02x\n",
		       g_tx[som + 6], g_tx[som + 7]);
		return -1;
	}
	printf("success! (%d bytes)\n", g_tx_len);
	return 0;
}

static int test_pd_zc_setup(struct test *t)
{
	static struct osdp_pd_cap cap[] = {
		{ OSDP_PD_CAP_READER_LED_CONTROL, 1, 1 },
		{ -1, -1, -1 }
	};
	struct osdp_channel channel = {
		.data = NULL,
		.recv_pkt = zc_recv_pkt,
		.release_pkt = zc_release_pkt,
		.send = zc_send,
		.flush = zc_flush,
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
		.scbk = NULL,
	};
	osdp_t *ctx;

#ifndef OPT_OSDP_LOG_MINIMAL
	osdp_logger_init("osdp::pd_zc", t->loglevel, NULL);
#else
	ARG_UNUSED(t);
#endif

	ctx = osdp_pd_setup(&channel, &info);
	if (ctx == NULL) {
		printf(SUB_1 "pd init failed!\n");
		return -1;
	}
	osdp_pd_set_command_callback(ctx, pd_command_cb, NULL);
	t->mock_data = (void *)ctx;
	return 0;
}

static void test_pd_zc_teardown(struct test *t)
{
	osdp_pd_teardown(t->mock_data);
	t->mock_data = NULL;
}

void run_pd_zc_tests(struct test *t)
{
	printf("\nStarting pd_zc tests\n");

	if (test_pd_zc_setup(t)) {
		return;
	}

	DO_TEST(t, test_pd_zc_status_reply_reaches_wire);

	test_pd_zc_teardown(t);
}

#else /* OPT_OSDP_RX_ZERO_COPY */

/* Zero-copy-only suite: compiles to a no-op in a non-zero-copy build so the
 * file can sit in the shared source list without a link dependency. */
void run_pd_zc_tests(struct test *t)
{
	ARG_UNUSED(t);
}

#endif /* OPT_OSDP_RX_ZERO_COPY */
