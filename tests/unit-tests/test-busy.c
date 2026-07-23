/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "test.h"

/*
 * osdp_BUSY handling (SIA OSDP v2.2 section 7.19):
 *
 *   - a busy PD answers with REPLY_BUSY at sequence number 0, framed as a
 *     plain (non-SCB) packet even while a secure channel is active;
 *   - the ACU shall repeat the command in its original form -- same
 *     sequence number -- until the PD returns something other than BUSY.
 *
 * The PD core never volunteers a BUSY, so these tests splice one into the
 * wire with the mock-channel hook: the CP's command is swallowed and a
 * crafted BUSY frame is delivered in its place, exactly what a real reader
 * does when it receives a command it has no cycles to process.
 */

extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);
extern uint8_t test_osdp_compute_checksum(uint8_t *msg, int length);

struct busy_test_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;

	bool cmd_seen;
	int last_cmd_id;

	bool notif_cmd_seen;
	int notif_cmd_arg0;
	int notif_cmd_arg1;

	/*
	 * The command queue is app-owned and zero-copy: the submitted
	 * osdp_cmd must outlive every retry until completion, so it lives
	 * here rather than on the submitter's stack frame.
	 */
	struct osdp_cmd buzzer_cmd;
};

static struct busy_test_ctx g_ctx;

struct busy_hook_state {
	int busy_remaining;	/* target frames left to answer with BUSY */
	uint8_t target_cmd;	/* wire command id to intercept */
	bool busy_use_checksum;	/* frame the BUSY with checksum, not CRC */
	int seq_log[16];	/* TX sequence number per target frame seen */
	int seq_count;
};

static int busy_frame_build(uint8_t *out, int max_len, bool use_checksum)
{
	int len = 0, start, pkt_len;
	uint16_t crc;

	if (max_len < 9)
		return -1;
#ifndef OPT_OSDP_SKIP_MARK_BYTE
	out[len++] = 0xff;
#endif
	start = len;
	out[len++] = 0x53;		/* SOM */
	out[len++] = 0x65 | 0x80;	/* PD address 101, reply direction */
	pkt_len = 6 + (use_checksum ? 1 : 2); /* header + id + trailer */
	out[len++] = pkt_len & 0xff;
	out[len++] = (pkt_len >> 8) & 0xff;
	out[len++] = use_checksum ? 0x00 : 0x04; /* SQN 0, no SCB */
	out[len++] = REPLY_BUSY;
	if (use_checksum) {
		out[len] = test_osdp_compute_checksum(out + start, len - start);
		len += 1;
	} else {
		crc = test_osdp_compute_crc16(out + start, len - start);
		out[len++] = crc & 0xff;
		out[len++] = (crc >> 8) & 0xff;
	}
	return len;
}

static enum test_channel_hook_verdict busy_hook(void *arg, bool cp_to_pd,
						const uint8_t *frame, int len,
						uint8_t *out, int *out_len,
						int out_max)
{
	struct busy_hook_state *s = arg;
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
	if (data_off >= len || frame[data_off] != s->target_cmd)
		return TEST_HOOK_PASS;
	if (s->seq_count < (int)ARRAY_SIZEOF(s->seq_log))
		s->seq_log[s->seq_count++] = ctrl & 0x03;
	if (s->busy_remaining != 0) {
		if (s->busy_remaining > 0)
			s->busy_remaining--;
		rc = busy_frame_build(out, out_max, s->busy_use_checksum);
		if (rc < 0)
			return TEST_HOOK_PASS;
		*out_len = rc;
		return TEST_HOOK_INJECT_REPLY;
	}
	return TEST_HOOK_PASS;
}

static int busy_test_pd_command_callback(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);

	g_ctx.cmd_seen = true;
	g_ctx.last_cmd_id = cmd->id;
	return 0;
}

static int busy_test_cp_event_callback(void *arg, int pd,
					struct osdp_event *ev)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(pd);

	if (ev->type == OSDP_EVENT_NOTIFICATION &&
	    ev->notif.type == OSDP_NOTIFICATION_COMMAND) {
		g_ctx.notif_cmd_seen = true;
		g_ctx.notif_cmd_arg0 = ev->notif.command.command;
		g_ctx.notif_cmd_arg1 = ev->notif.command.success ? 0 : -1;
	}
	return 0;
}

static int busy_test_setup(struct test *t)
{
	int rc = 0;
	uint8_t status = 0;

	if (test_setup_devices_ext(t, &g_ctx.cp_ctx, &g_ctx.pd_ctx,
				   OSDP_FLAG_ENABLE_NOTIFICATION, 0)) {
		printf(SUB_1 "Failed to setup devices!\n");
		return -1;
	}
	osdp_pd_set_command_callback(g_ctx.pd_ctx,
				     busy_test_pd_command_callback, NULL);
	osdp_cp_set_event_callback(g_ctx.cp_ctx,
				   busy_test_cp_event_callback, NULL);

	g_ctx.cp_runner = async_runner_start(g_ctx.cp_ctx, osdp_cp_refresh);
	g_ctx.pd_runner = async_runner_start(g_ctx.pd_ctx, osdp_pd_refresh);
	if (g_ctx.cp_runner < 0 || g_ctx.pd_runner < 0) {
		printf(SUB_1 "Failed to create CP/PD runners\n");
		return -1;
	}

	while (1) {
		if (rc > 10 * 1000) { /* ~10s online timeout (rc is ms) */
			printf(SUB_1 "PD failed to come online\n");
			return -1;
		}
		osdp_get_status_mask(g_ctx.cp_ctx, &status);
		if (status & 1)
			break;
		usleep(20 * 1000);
		rc += 20;
	}
	return 0;
}

static void busy_test_teardown(void)
{
	test_set_channel_hook(NULL, NULL);
	async_runner_stop(g_ctx.cp_runner);
	async_runner_stop(g_ctx.pd_runner);
	osdp_cp_teardown(g_ctx.cp_ctx);
	osdp_pd_teardown(g_ctx.pd_ctx);
	memset(&g_ctx, 0, sizeof(g_ctx));
}

static bool wait_for_pd_command(int expected_cmd_id, int timeout_sec)
{
	int rc = 0;

	while (rc < timeout_sec * 10) {
		if (g_ctx.cmd_seen && g_ctx.last_cmd_id == expected_cmd_id)
			return true;
		usleep(100 * 1000);
		rc++;
	}
	return false;
}

static int submit_buzzer_command(void)
{
	g_ctx.buzzer_cmd = (struct osdp_cmd){
		.id = OSDP_CMD_BUZZER,
		.buzzer = {
			.reader = 0,
			.control_code = 2,
			.on_count = 5,
			.off_count = 5,
			.rep_count = 1,
		},
	};

	return osdp_cp_submit_command(g_ctx.cp_ctx, 0, &g_ctx.buzzer_cmd);
}

/*
 * One BUSY, then let the retry through. The retried frame must carry the
 * same sequence number as the original -- a real PD accepts it as a fresh
 * command only then. Both integrity framings of the BUSY reply are valid
 * on the wire, so both must be recognized.
 */
static bool test_busy_retry_reuses_sequence(bool use_checksum)
{
	struct busy_hook_state s = {
		.busy_remaining = 1,
		.target_cmd = CMD_BUZ,
		.busy_use_checksum = use_checksum,
	};
	uint8_t status = 0;

	printf(SUB_1 "busy retry reuses sequence (%s framed busy) -- ",
	       use_checksum ? "checksum" : "CRC");

	g_ctx.cmd_seen = false;
	g_ctx.last_cmd_id = 0;
	test_set_channel_hook(busy_hook, &s);

	if (submit_buzzer_command()) {
		printf("failed to submit command!\n");
		test_set_channel_hook(NULL, NULL);
		return false;
	}

	if (!wait_for_pd_command(OSDP_CMD_BUZZER, 5)) {
		printf("PD never saw the retried command!\n");
		test_set_channel_hook(NULL, NULL);
		return false;
	}
	test_set_channel_hook(NULL, NULL);

	if (s.seq_count < 2) {
		printf("expected a retry; saw %d transmission(s)!\n",
		       s.seq_count);
		return false;
	}
	if (s.seq_log[1] != s.seq_log[0]) {
		printf("retry changed sequence number (%d -> %d)!\n",
		       s.seq_log[0], s.seq_log[1]);
		return false;
	}

	osdp_get_status_mask(g_ctx.cp_ctx, &status);
	if (!(status & 1)) {
		printf("PD went offline after busy retry!\n");
		return false;
	}

	printf("success!\n");
	return true;
}

/*
 * A PD that never stops being busy must cost the app one failed command,
 * not the link: after the retry budget runs out the command completes as
 * FAILED, the PD stays online, and the next command goes through.
 */
static bool test_busy_permanent_fails_command_softly(void)
{
	struct busy_hook_state s = {
		.busy_remaining = -1, /* never let the command through */
		.target_cmd = CMD_BUZ,
	};
	uint8_t status = 0;
	int rc = 0;

	printf(SUB_1 "permanent busy fails command softly -- ");

	g_ctx.cmd_seen = false;
	g_ctx.last_cmd_id = 0;
	g_ctx.notif_cmd_seen = false;
	test_set_channel_hook(busy_hook, &s);

	if (submit_buzzer_command()) {
		printf("failed to submit command!\n");
		test_set_channel_hook(NULL, NULL);
		return false;
	}

	/* Budget: 1 original + OSDP_CMD_MAX_RETRIES retries, 800ms apart */
	while (rc < 150) {
		if (g_ctx.notif_cmd_seen)
			break;
		usleep(100 * 1000);
		rc++;
	}
	test_set_channel_hook(NULL, NULL);

	if (!g_ctx.notif_cmd_seen) {
		printf("command never completed!\n");
		return false;
	}
	if (g_ctx.notif_cmd_arg0 != OSDP_CMD_BUZZER ||
	    g_ctx.notif_cmd_arg1 != -1) {
		printf("expected failure notification, got arg0=%d arg1=%d!\n",
		       g_ctx.notif_cmd_arg0, g_ctx.notif_cmd_arg1);
		return false;
	}
	if (s.seq_count != 1 + OSDP_CMD_MAX_RETRIES) {
		printf("expected %d transmissions, saw %d!\n",
		       1 + OSDP_CMD_MAX_RETRIES, s.seq_count);
		return false;
	}
	if (g_ctx.cmd_seen) {
		printf("PD saw a command that was never delivered!\n");
		return false;
	}

	osdp_get_status_mask(g_ctx.cp_ctx, &status);
	if (!(status & 1)) {
		printf("PD went offline after busy exhaustion!\n");
		return false;
	}

	/* The link must still carry the next command */
	if (submit_buzzer_command()) {
		printf("failed to submit follow-up command!\n");
		return false;
	}
	if (!wait_for_pd_command(OSDP_CMD_BUZZER, 5)) {
		printf("follow-up command never reached the PD!\n");
		return false;
	}

	printf("success!\n");
	return true;
}

void run_busy_tests(struct test *t)
{
	printf("\nStarting BUSY reply tests\n");

	if (busy_test_setup(t)) {
		printf(SUB_1 "Failed to setup busy test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	TEST_CASE(t, "busy_retry_reuses_sequence_plain",
		  test_busy_retry_reuses_sequence(false));
	TEST_CASE(t, "busy_retry_reuses_sequence_checksum",
		  test_busy_retry_reuses_sequence(true));
	TEST_CASE(t, "busy_permanent_fails_command_softly",
		  test_busy_permanent_fails_command_softly());

	busy_test_teardown();
}
