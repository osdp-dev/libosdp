/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "osdp_common.h"
#include "test.h"
#include "emu-card.h"

#ifdef OPT_BUILD_OSDP_TRS

/*
 * End-to-end TRS scenarios against an emulated ISO 7816 PIV card sitting
 * behind the PD's app layer, modeled on the workflows (and failure modes)
 * reported from real readers in issue #22: a full chained certificate
 * read, sessions that must survive BUSY spells, and a card leaving the
 * field mid-band.
 */

extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);

/* Le the CP asks for per chunk; fits the default 256-byte packet */
#define EMU_CHUNK_LE 200

struct trs_emu_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;

	struct emu_card card;

	/* PD reply event; submitted by reference from the command callback */
	struct osdp_event resp_event;

	/* CP side captures */
	bool event_seen;
	struct osdp_trs_reply last_reply;
	bool status_seen;
	enum osdp_trs_session_status_e last_status;
	bool completion_seen;
	enum osdp_completion_status last_completion;
	bool cardread_seen;
};

static struct trs_emu_ctx g_emu;

/* Answer BUSY (plaintext, CRC, SQN 0) to the next N XWR commands seen */
struct emu_busy_hook {
	int busy_remaining;
	int xwr_tx_count;
};

static int emu_busy_frame(uint8_t *out, int max_len)
{
	int len = 0, start;
	uint16_t crc;

	if (max_len < 10) {
		return -1;
	}
#ifndef OPT_OSDP_SKIP_MARK_BYTE
	out[len++] = 0xff;
#endif
	start = len;
	out[len++] = 0x53;
	out[len++] = 0x65 | 0x80;
	out[len++] = 8; /* header + id + CRC */
	out[len++] = 0;
	out[len++] = 0x04; /* SQN 0, CRC, no SCB */
	out[len++] = REPLY_BUSY;
	crc = test_osdp_compute_crc16(out + start, len - start);
	out[len++] = crc & 0xff;
	out[len++] = (crc >> 8) & 0xff;
	return len;
}

static enum test_channel_hook_verdict emu_busy_hook_fn(void *arg,
		bool cp_to_pd, const uint8_t *frame, int len,
		uint8_t *out, int *out_len, int out_max)
{
	struct emu_busy_hook *s = arg;
	int base, data_off, rc;
	uint8_t ctrl;

	if (!cp_to_pd)
		return TEST_HOOK_PASS;
	/* The leading mark byte depends on build options and direction;
	 * detect it instead of assuming (a frame proper starts at SOM) */
	base = (len > 0 && frame[0] == 0xff) ? 1 : 0;
	if (len < base + 6)
		return TEST_HOOK_PASS;
	ctrl = frame[base + 4];
	data_off = base + 5;
	if (ctrl & 0x08) /* security control block */
		data_off += frame[base + 5];
	if (data_off >= len || frame[data_off] != CMD_XWR)
		return TEST_HOOK_PASS;
	s->xwr_tx_count++;
	if (s->busy_remaining == 0)
		return TEST_HOOK_PASS;
	if (s->busy_remaining > 0)
		s->busy_remaining--;
	rc = emu_busy_frame(out, out_max);
	if (rc < 0)
		return TEST_HOOK_PASS;
	*out_len = rc;
	return TEST_HOOK_INJECT_REPLY;
}

static int emu_cp_event_callback(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(pd);
	struct trs_emu_ctx *ctx = arg;

	if (ev->type == OSDP_EVENT_TRS) {
		ctx->event_seen = true;
		memcpy(&ctx->last_reply, &ev->trs, sizeof(ctx->last_reply));
	}
	if (ev->type == OSDP_EVENT_CARDREAD) {
		ctx->cardread_seen = true;
	}
	if (ev->type == OSDP_EVENT_NOTIFICATION &&
	    ev->notif.type == OSDP_NOTIFICATION_TRS_STATUS) {
		ctx->status_seen = true;
		ctx->last_status = ev->notif.trs_status.status;
	}
	return 0;
}

static void emu_cp_completion_callback(void *arg, int pd,
				       const struct osdp_cmd *cmd,
				       enum osdp_completion_status status)
{
	struct trs_emu_ctx *ctx = arg;

	ARG_UNUSED(pd);
	if (cmd->id == OSDP_CMD_XWR) {
		ctx->completion_seen = true;
		ctx->last_completion = status;
	}
}

/* The PD app: every C-APDU runs against the emulated card */
static int emu_pd_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct trs_emu_ctx *ctx = arg;
	struct osdp_event *resp = &ctx->resp_event;
	int rlen;

	if (cmd->id != OSDP_CMD_XWR ||
	    cmd->trs.command != OSDP_TRS_CMD_SEND_APDU) {
		return 0;
	}

	memset(resp, 0, sizeof(*resp));
	resp->type = OSDP_EVENT_TRS;
	rlen = emu_card_apdu(&ctx->card, cmd->trs.apdu.data,
			     cmd->trs.apdu.length,
			     resp->trs.card_data.apdu.data,
			     sizeof(resp->trs.card_data.apdu.data));
	if (rlen < 0) {
		/* No card in the field to answer */
		resp->trs.reply = OSDP_TRS_REPLY_ERROR;
		resp->trs.error.code = 0x01;
	} else {
		resp->trs.reply = OSDP_TRS_REPLY_CARD_DATA;
		resp->trs.card_data.reader = 0;
		resp->trs.card_data.status = 0;
		resp->trs.card_data.apdu.length = rlen;
	}
	osdp_pd_submit_event(ctx->pd_ctx, resp);
	return 0;
}

static int emu_setup(struct test *t)
{
	printf(SUB_1 "setting up OSDP devices with an emulated PIV card\n");

	if (test_setup_devices_ext(t, &g_emu.cp_ctx, &g_emu.pd_ctx,
				   OSDP_FLAG_ENABLE_NOTIFICATION, 0)) {
		printf(SUB_1 "Failed to setup devices!\n");
		return -1;
	}
	emu_card_reset(&g_emu.card);

	osdp_cp_set_event_callback(g_emu.cp_ctx, emu_cp_event_callback, &g_emu);
	osdp_cp_set_command_completion_callback(g_emu.cp_ctx,
						emu_cp_completion_callback,
						&g_emu);
	osdp_pd_set_command_callback(g_emu.pd_ctx, emu_pd_command_callback,
				     &g_emu);

	g_emu.cp_runner = async_runner_start(g_emu.cp_ctx, osdp_cp_refresh);
	g_emu.pd_runner = async_runner_start(g_emu.pd_ctx, osdp_pd_refresh);
	if (g_emu.cp_runner < 0 || g_emu.pd_runner < 0) {
		printf(SUB_1 "Failed to create CP/PD runners\n");
		return -1;
	}
	if (!test_wait_for_online(g_emu.cp_ctx, 0, 10)) {
		printf(SUB_1 "PD failed to come online\n");
		return -1;
	}
	return 0;
}

static void emu_teardown(void)
{
	printf(SUB_1 "tearing down emu test environment\n");
	test_set_channel_hook(NULL, NULL);
	async_runner_stop(g_emu.cp_runner);
	async_runner_stop(g_emu.pd_runner);
	osdp_cp_teardown(g_emu.cp_ctx);
	osdp_pd_teardown(g_emu.pd_ctx);
	memset(&g_emu, 0, sizeof(g_emu));
}

static bool emu_wait_status(enum osdp_trs_session_status_e want,
			    int timeout_sec)
{
	int rc = 0;

	while (rc++ < timeout_sec * 10) {
		if (g_emu.status_seen && g_emu.last_status == want) {
			return true;
		}
		usleep(100 * 1000);
	}
	return false;
}

static bool emu_session(enum osdp_trs_cmd_e command,
			enum osdp_trs_session_status_e want)
{
	static struct osdp_cmd cmd;

	g_emu.status_seen = false;
	cmd = (struct osdp_cmd){
		.id = OSDP_CMD_XWR,
		.trs = { .command = command },
	};
	if (osdp_cp_submit_command(g_emu.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "failed to submit session command %d\n", command);
		return false;
	}
	return emu_wait_status(want, 10);
}

/*
 * Round-trip one C-APDU through the session. Returns the R-APDU length
 * (data + SW), or -1. @expect_fail flips the meaning of the completion.
 */
static int emu_xfer(const uint8_t *capdu, int clen, uint8_t *rapdu,
		    int max_rlen)
{
	static struct osdp_cmd cmd;
	int rc = 0, rlen;

	g_emu.event_seen = false;
	g_emu.completion_seen = false;

	cmd = (struct osdp_cmd){
		.id = OSDP_CMD_XWR,
		.trs = { .command = OSDP_TRS_CMD_SEND_APDU },
	};
	cmd.trs.apdu.length = clen;
	memcpy(cmd.trs.apdu.data, capdu, clen);

	if (osdp_cp_submit_command(g_emu.cp_ctx, 0, &cmd)) {
		return -1;
	}
	while (rc++ < 150 && !g_emu.completion_seen) {
		usleep(100 * 1000);
	}
	if (!g_emu.completion_seen || !g_emu.event_seen ||
	    g_emu.last_reply.reply != OSDP_TRS_REPLY_CARD_DATA) {
		return -1;
	}
	rlen = g_emu.last_reply.card_data.apdu.length;
	if (rlen > max_rlen) {
		return -1;
	}
	memcpy(rapdu, g_emu.last_reply.card_data.apdu.data, rlen);
	return rlen;
}

/*
 * The headline workflow: detect the card, open a session, SELECT the PIV
 * application, read a >1KB object with 61xx/GET RESPONSE chaining, close.
 */
static bool test_emu_piv_certificate_read(void)
{
	uint8_t capdu[16], rapdu[OSDP_TRS_APDU_MAX_LEN];
	uint8_t object[EMU_CARD_OBJECT_LEN];
	const uint8_t *want;
	int rlen, pos = 0, want_len, clen;

	printf(SUB_2 "testing emu PIV certificate read\n");

	emu_card_set_present(&g_emu.card, true);

	if (!emu_session(OSDP_TRS_CMD_START, OSDP_TRS_SESSION_OPENED)) {
		printf(SUB_2 "session did not open\n");
		return false;
	}

	/* SELECT the PIV AID */
	capdu[0] = 0x00;
	capdu[1] = 0xA4;
	capdu[2] = 0x04;
	capdu[3] = 0x00;
	capdu[4] = sizeof(emu_card_piv_aid);
	memcpy(capdu + 5, emu_card_piv_aid, sizeof(emu_card_piv_aid));
	clen = 5 + sizeof(emu_card_piv_aid);
	rlen = emu_xfer(capdu, clen, rapdu, sizeof(rapdu));
	if (rlen < 2 || rapdu[rlen - 2] != 0x90 || rapdu[rlen - 1] != 0x00) {
		printf(SUB_2 "SELECT PIV AID failed (rlen %d)\n", rlen);
		return false;
	}

	/* GET DATA, then GET RESPONSE until the chain runs dry */
	capdu[0] = 0x00;
	capdu[1] = 0xCB;
	capdu[2] = 0x3F;
	capdu[3] = 0xFF;
	capdu[4] = EMU_CHUNK_LE;
	clen = 5;
	while (1) {
		rlen = emu_xfer(capdu, clen, rapdu, sizeof(rapdu));
		if (rlen < 2) {
			printf(SUB_2 "chained read failed at offset %d\n", pos);
			return false;
		}
		if (pos + rlen - 2 > (int)sizeof(object)) {
			printf(SUB_2 "read overflowed the object\n");
			return false;
		}
		memcpy(object + pos, rapdu, rlen - 2);
		pos += rlen - 2;
		if (rapdu[rlen - 2] == 0x90 && rapdu[rlen - 1] == 0x00) {
			break;
		}
		if (rapdu[rlen - 2] != 0x61) {
			printf(SUB_2 "unexpected SW %02x%02x\n",
			       rapdu[rlen - 2], rapdu[rlen - 1]);
			return false;
		}
		capdu[0] = 0x00;
		capdu[1] = 0xC0;
		capdu[2] = 0x00;
		capdu[3] = 0x00;
		capdu[4] = EMU_CHUNK_LE;
		clen = 5;
	}

	want = emu_card_object(&want_len);
	if (pos != want_len || memcmp(object, want, want_len) != 0) {
		printf(SUB_2 "object mismatch: read %d of %d bytes\n",
		       pos, want_len);
		return false;
	}

	if (!emu_session(OSDP_TRS_CMD_STOP, OSDP_TRS_SESSION_CLOSED)) {
		printf(SUB_2 "session did not close\n");
		return false;
	}
	return true;
}

/* A busy spell mid-session must cost latency, never the session */
static bool test_emu_session_survives_busy(void)
{
	struct emu_busy_hook s = { .busy_remaining = 2 };
	uint8_t capdu[16], rapdu[OSDP_TRS_APDU_MAX_LEN];
	int rlen, clen;

	printf(SUB_2 "testing emu session survives a busy spell\n");

	emu_card_set_present(&g_emu.card, true);
	if (!emu_session(OSDP_TRS_CMD_START, OSDP_TRS_SESSION_OPENED)) {
		printf(SUB_2 "session did not open\n");
		return false;
	}

	g_emu.status_seen = false;
	test_set_channel_hook(emu_busy_hook_fn, &s);
	capdu[0] = 0x00;
	capdu[1] = 0xA4;
	capdu[2] = 0x04;
	capdu[3] = 0x00;
	capdu[4] = sizeof(emu_card_piv_aid);
	memcpy(capdu + 5, emu_card_piv_aid, sizeof(emu_card_piv_aid));
	clen = 5 + sizeof(emu_card_piv_aid);
	rlen = emu_xfer(capdu, clen, rapdu, sizeof(rapdu));
	test_set_channel_hook(NULL, NULL);

	if (rlen < 2 || rapdu[rlen - 2] != 0x90) {
		printf(SUB_2 "APDU did not survive the busy spell (%d)\n",
		       rlen);
		return false;
	}
	if (s.xwr_tx_count != 3) {
		printf(SUB_2 "want 2 busy retries (3 TX); saw %d\n",
		       s.xwr_tx_count);
		return false;
	}
	if (g_emu.status_seen) {
		printf(SUB_2 "busy spell must not end the session (%d)\n",
		       g_emu.last_status);
		return false;
	}

	return emu_session(OSDP_TRS_CMD_STOP, OSDP_TRS_SESSION_CLOSED);
}

/* A permanently-busy reader costs the app one failed command and the
 * session keeps running for whatever the app tries next */
static bool test_emu_permanent_busy_fails_softly(void)
{
	struct emu_busy_hook s = { .busy_remaining = -1 };
	uint8_t capdu[] = { 0x00, 0xA4, 0x04, 0x00, 0x00 };
	uint8_t rapdu[OSDP_TRS_APDU_MAX_LEN];
	uint8_t status = 0;
	int rc = 0, rlen;

	printf(SUB_2 "testing emu permanently busy reader\n");

	emu_card_set_present(&g_emu.card, true);
	if (!emu_session(OSDP_TRS_CMD_START, OSDP_TRS_SESSION_OPENED)) {
		printf(SUB_2 "session did not open\n");
		return false;
	}

	g_emu.status_seen = false;
	g_emu.completion_seen = false;
	test_set_channel_hook(emu_busy_hook_fn, &s);
	if (emu_xfer(capdu, sizeof(capdu), rapdu, sizeof(rapdu)) >= 0) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "a never-answered APDU cannot succeed\n");
		return false;
	}
	test_set_channel_hook(NULL, NULL);

	while (rc++ < 20 && !g_emu.completion_seen) {
		usleep(100 * 1000);
	}
	if (!g_emu.completion_seen ||
	    g_emu.last_completion != OSDP_COMPLETION_FAILED) {
		printf(SUB_2 "busy-exhausted APDU must complete as failed\n");
		return false;
	}
	osdp_get_status_mask(g_emu.cp_ctx, &status);
	if (!(status & 1)) {
		printf(SUB_2 "PD must stay online through busy exhaustion\n");
		return false;
	}

	/* The session is still usable */
	uint8_t select_apdu[16];

	select_apdu[0] = 0x00;
	select_apdu[1] = 0xA4;
	select_apdu[2] = 0x04;
	select_apdu[3] = 0x00;
	select_apdu[4] = sizeof(emu_card_piv_aid);
	memcpy(select_apdu + 5, emu_card_piv_aid, sizeof(emu_card_piv_aid));
	rlen = emu_xfer(select_apdu, 5 + sizeof(emu_card_piv_aid),
			rapdu, sizeof(rapdu));
	if (rlen < 2 || rapdu[rlen - 2] != 0x90) {
		printf(SUB_2 "session unusable after busy exhaustion\n");
		return false;
	}

	return emu_session(OSDP_TRS_CMD_STOP, OSDP_TRS_SESSION_CLOSED);
}

/*
 * Readers do not reliably announce a card leaving the field (issue #22);
 * the app discovers it by the next APDU failing. That failure must cost
 * the transaction, not the session, and STOP must still work.
 */
static bool test_emu_card_removed_mid_band(void)
{
	uint8_t capdu[16], rapdu[OSDP_TRS_APDU_MAX_LEN];
	int rlen, clen, rc = 0;

	printf(SUB_2 "testing emu card removed mid-band\n");

	emu_card_set_present(&g_emu.card, true);
	if (!emu_session(OSDP_TRS_CMD_START, OSDP_TRS_SESSION_OPENED)) {
		printf(SUB_2 "session did not open\n");
		return false;
	}

	capdu[0] = 0x00;
	capdu[1] = 0xA4;
	capdu[2] = 0x04;
	capdu[3] = 0x00;
	capdu[4] = sizeof(emu_card_piv_aid);
	memcpy(capdu + 5, emu_card_piv_aid, sizeof(emu_card_piv_aid));
	clen = 5 + sizeof(emu_card_piv_aid);
	rlen = emu_xfer(capdu, clen, rapdu, sizeof(rapdu));
	if (rlen < 2 || rapdu[rlen - 2] != 0x90) {
		printf(SUB_2 "SELECT before removal failed\n");
		return false;
	}

	/* Card leaves the field; the probe comes back as an error report */
	emu_card_set_present(&g_emu.card, false);
	g_emu.status_seen = false;
	g_emu.event_seen = false;
	g_emu.completion_seen = false;
	if (emu_xfer(capdu, clen, rapdu, sizeof(rapdu)) >= 0) {
		printf(SUB_2 "APDU to an absent card cannot succeed\n");
		return false;
	}
	while (rc++ < 50 && !g_emu.completion_seen) {
		usleep(100 * 1000);
	}
	if (!g_emu.completion_seen ||
	    g_emu.last_completion != OSDP_COMPLETION_FAILED ||
	    !g_emu.event_seen ||
	    g_emu.last_reply.reply != OSDP_TRS_REPLY_ERROR) {
		printf(SUB_2 "removal must surface as error + failed command\n");
		return false;
	}
	if (g_emu.status_seen) {
		printf(SUB_2 "removal must not end the session by itself\n");
		return false;
	}

	return emu_session(OSDP_TRS_CMD_STOP, OSDP_TRS_SESSION_CLOSED);
}

/* Sample the emulated reader's mode every 10ms until it reads @want */
static bool emu_wait_mode(uint8_t want, int timeout_ms)
{
	struct osdp_pd *pd = osdp_to_pd(g_emu.pd_ctx, 0);
	int waited = 0;

	while (waited < timeout_ms) {
		if (pd->trs.mode == want) {
			return true;
		}
		usleep(10 * 1000);
		waited += 10;
	}
	return false;
}

/* Event submitted by reference; must outlive the callback that ships it */
static struct osdp_event g_rpk40_ev;

/* An RPK40 reports ordinary credentials only in mode 00: submit one standard
 * read the next time the reader is in its default mode, and confirm the CP
 * app received it */
static bool emu_rpk40_standard_read(void)
{
	int rc = 0;

	g_emu.cardread_seen = false;
	if (!emu_wait_mode(TRS_MODE_00, 5000)) {
		printf(SUB_2 "reader never in default mode\n");
		return false;
	}
	g_rpk40_ev = (struct osdp_event){
		.type = OSDP_EVENT_CARDREAD,
		.cardread = {
			.reader_no = 0,
			.format = OSDP_CARD_FMT_RAW_WIEGAND,
			.length = 32, /* raw formats count bits, not bytes */
			.data = { 0xde, 0xad, 0xbe, 0xef },
		},
	};
	if (osdp_pd_submit_event(g_emu.pd_ctx, &g_rpk40_ev)) {
		printf(SUB_2 "failed to submit standard read\n");
		return false;
	}
	while (rc++ < 30 && !g_emu.cardread_seen) {
		usleep(100 * 1000);
	}
	if (!g_emu.cardread_seen) {
		printf(SUB_2 "standard read never reached the app\n");
		return false;
	}
	return true;
}

/*
 * The RPK40 workflow from issue #22: the reader reports ordinary credentials
 * only in its default mode and gives no smart-card indication there, so the
 * presence scan is the only way to serve both credential types. A standard
 * read works; a smart card is sighted during a probe; the full chained PIV
 * certificate read runs over the adopted session; a standard read works
 * again once the scan resumes.
 */
static bool test_emu_rpk40_time_slice(void)
{
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 100,
		.mode1_dwell_ms = 300,
		.hold_ms = 3000,
	};

	printf(SUB_2 "testing emu RPK40 mode time-slicing\n");

	emu_card_set_present(&g_emu.card, false);
	if (osdp_cp_trs_scan_enable(g_emu.cp_ctx, 0, &p)) {
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}

	/* A standard credential, between probes */
	if (!emu_rpk40_standard_read()) {
		return false;
	}

	/* A smart card arrives; only a probe can see it */
	g_emu.event_seen = false;
	emu_card_set_present(&g_emu.card, true);
	if (!emu_wait_mode(TRS_MODE_01, 5000)) {
		printf(SUB_2 "no probe to sight the smart card in\n");
		return false;
	}
	g_rpk40_ev = (struct osdp_event){
		.type = OSDP_EVENT_TRS,
		.trs = {
			.reply = OSDP_TRS_REPLY_CARD_PRESENT,
			.card_present = {
				.reader = 0,
				.status = OSDP_TRS_CARD_PRESENT_CONTACTLESS,
			},
		},
	};
	if (osdp_pd_submit_event(g_emu.pd_ctx, &g_rpk40_ev)) {
		printf(SUB_2 "failed to submit sighting\n");
		return false;
	}
	int rc = 0;
	while (rc++ < 30 && !g_emu.event_seen) {
		usleep(100 * 1000);
	}
	if (!g_emu.event_seen ||
	    g_emu.last_reply.reply != OSDP_TRS_REPLY_CARD_PRESENT) {
		printf(SUB_2 "sighting never reached the app\n");
		return false;
	}

	/* The app answers with a band: the whole PIV workflow, adopted */
	if (!test_emu_piv_certificate_read()) {
		printf(SUB_2 "PIV read over the adopted session failed\n");
		return false;
	}

	/* The card leaves; ordinary credentials must keep working */
	emu_card_set_present(&g_emu.card, false);
	if (!emu_rpk40_standard_read()) {
		return false;
	}

	osdp_cp_trs_scan_disable(g_emu.cp_ctx, 0);
	return emu_wait_mode(TRS_MODE_00, 3000);
}

void run_trs_emu_tests(struct test *t)
{
	bool result = true;

	printf("\nBegin TRS card-emulation Tests\n");

	if (emu_setup(t) != 0) {
		printf(SUB_1 "Failed to setup emu test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	result &= test_emu_piv_certificate_read();
	result &= test_emu_session_survives_busy();
	result &= test_emu_permanent_busy_fails_softly();
	result &= test_emu_card_removed_mid_band();
	result &= test_emu_rpk40_time_slice();

	emu_teardown();

	printf(SUB_1 "TRS card-emulation tests %s\n",
	       result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

#else /* OPT_BUILD_OSDP_TRS */

void run_trs_emu_tests(struct test *t)
{
	ARG_UNUSED(t);
}

#endif /* OPT_BUILD_OSDP_TRS */
