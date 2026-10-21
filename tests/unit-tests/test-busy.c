/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
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
 * The CP-side tests splice a BUSY into the wire with the mock-channel hook:
 * the CP's command is swallowed and a crafted BUSY frame is delivered in its
 * place, exactly what a real reader does when it receives a command it has
 * no cycles to process. The PD-side tests have the PD app answer
 * OSDP_PD_CMD_BUSY and check the frames the PD core puts on the wire.
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

	/* The PD app answers busy_cmd_id with BUSY this many more times. */
	int busy_cmd_id;
	int busy_answers;
	int busy_cmd_calls;

	/* Written by the completion callback on the refresh thread. */
	struct test_completion completion;

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
	int busy_replies; /* well-formed BUSY frames the PD sent */
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

	/* The leading mark byte depends on build options and direction;
	 * detect it instead of assuming (a frame proper starts at SOM) */
	base = (len > 0 && frame[0] == 0xff) ? 1 : 0;
	if (len < base + 6)
		return TEST_HOOK_PASS;
	ctrl = frame[base + 4];
	if (!cp_to_pd) {
		/* Sequence 0 and no SCB, even under a secure channel */
		if ((ctrl & 0x0b) == 0 && frame[base + 5] == REPLY_BUSY)
			s->busy_replies++;
		return TEST_HOOK_PASS;
	}
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

	if ((int)cmd->id == g_ctx.busy_cmd_id) {
		g_ctx.busy_cmd_calls++;
		if (g_ctx.busy_answers > 0) {
			g_ctx.busy_answers--;
			return OSDP_PD_CMD_BUSY;
		}
	}
	g_ctx.cmd_seen = true;
	g_ctx.last_cmd_id = cmd->id;
	return 0;
}

static int busy_test_setup(struct test *t, bool secure)
{
	int rc = 0;
	uint8_t status = 0;

	if (secure ? test_setup_devices(t, &g_ctx.cp_ctx, &g_ctx.pd_ctx) :
		     test_setup_devices_plain(t, &g_ctx.cp_ctx, &g_ctx.pd_ctx,
					      0, 0)) {
		printf(SUB_1 "Failed to setup devices!\n");
		return -1;
	}
	osdp_pd_set_command_callback(g_ctx.pd_ctx,
				     busy_test_pd_command_callback, NULL);
	osdp_cp_set_command_completion_callback(g_ctx.cp_ctx,
						test_cmd_completion_cb,
						&g_ctx.completion);

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

	return test_submit_command(g_ctx.cp_ctx, 0, &g_ctx.buzzer_cmd) ? 0 : -1;
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
	test_completion_reset(&g_ctx.completion);
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

	/* The PD sees the command before its ACK reaches the CP; a later
	 * test must not pick up this completion as its own. */
	if (!test_completion_wait(&g_ctx.completion, 1, 5)) {
		printf("retried command never completed!\n");
		return false;
	}

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
	test_completion_reset(&g_ctx.completion);
	test_set_channel_hook(busy_hook, &s);

	if (submit_buzzer_command()) {
		printf("failed to submit command!\n");
		test_set_channel_hook(NULL, NULL);
		return false;
	}

	/* Budget: 1 original + OSDP_CMD_MAX_RETRIES retries, 800ms apart */
	while (rc < 150) {
		if (test_completion_count(&g_ctx.completion))
			break;
		usleep(100 * 1000);
		rc++;
	}
	test_set_channel_hook(NULL, NULL);

	if (!test_completion_count(&g_ctx.completion)) {
		printf("command never completed!\n");
		return false;
	}
	if (test_completion_id(&g_ctx.completion) != OSDP_CMD_BUZZER ||
	    test_completion_status(&g_ctx.completion) !=
		    OSDP_COMPLETION_FAILED) {
		printf("expected FAILED completion, got cmd=%d status=%d!\n",
		       test_completion_id(&g_ctx.completion),
		       test_completion_status(&g_ctx.completion));
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

static bool wait_for_secure_online(int timeout_sec)
{
	uint8_t online = 0, secure = 0;
	int rc = 0;

	while (rc < timeout_sec * 10) {
		osdp_get_status_mask(g_ctx.cp_ctx, &online);
		osdp_get_sc_status_mask(g_ctx.cp_ctx, &secure);
		if ((online & 1) && (secure & 1))
			return true;
		usleep(100 * 1000);
		rc++;
	}
	return false;
}

/* A buzzer command must still get through, and under the secure channel. */
static bool link_carries_next_command(void)
{
	g_ctx.cmd_seen = false;
	g_ctx.last_cmd_id = 0;
	test_completion_reset(&g_ctx.completion);
	if (!wait_for_secure_online(10)) {
		printf("secure channel did not come back!\n");
		return false;
	}
	if (submit_buzzer_command()) {
		printf("failed to submit follow-up command!\n");
		return false;
	}
	if (!wait_for_pd_command(OSDP_CMD_BUZZER, 5) ||
	    !test_completion_wait(&g_ctx.completion, 1, 5) ||
	    test_completion_status(&g_ctx.completion) != OSDP_COMPLETION_OK) {
		printf("follow-up command did not complete!\n");
		return false;
	}
	return true;
}

/*
 * The PD app answers BUSY twice, then ACKs. The PD must put real BUSY frames
 * on the wire, every repeat must reach the app, and since a BUSY leaves the
 * PD as it was, the link -- sequence numbers and secure channel -- must stay
 * in step for the next command.
 */
static bool test_pd_app_busy_then_ack(void)
{
	struct busy_hook_state s = {
		.target_cmd = CMD_BUZ,
	};

	printf(SUB_1 "PD app busy, then ACK -- ");

	g_ctx.busy_cmd_id = OSDP_CMD_BUZZER;
	g_ctx.busy_answers = 2;
	g_ctx.busy_cmd_calls = 0;
	test_completion_reset(&g_ctx.completion);
	test_set_channel_hook(busy_hook, &s);

	if (submit_buzzer_command()) {
		printf("failed to submit command!\n");
		test_set_channel_hook(NULL, NULL);
		return false;
	}
	if (!test_completion_wait(&g_ctx.completion, 1, 10)) {
		printf("command never completed!\n");
		test_set_channel_hook(NULL, NULL);
		return false;
	}
	test_set_channel_hook(NULL, NULL);
	g_ctx.busy_cmd_id = 0;

	if (test_completion_first_id(&g_ctx.completion) != OSDP_CMD_BUZZER ||
	    test_completion_first_status(&g_ctx.completion) !=
		    OSDP_COMPLETION_OK) {
		printf("expected OK completion, got cmd=%d status=%d!\n",
		       test_completion_first_id(&g_ctx.completion),
		       test_completion_first_status(&g_ctx.completion));
		return false;
	}
	if (g_ctx.busy_cmd_calls != 3) {
		printf("expected 3 calls into the app, saw %d!\n",
		       g_ctx.busy_cmd_calls);
		return false;
	}
	if (s.busy_replies != 2) {
		printf("expected 2 BUSY frames on the wire, saw %d!\n",
		       s.busy_replies);
		return false;
	}
	if (s.seq_count != 3 || s.seq_log[1] != s.seq_log[0] ||
	    s.seq_log[2] != s.seq_log[0]) {
		printf("repeats did not reuse the sequence number!\n");
		return false;
	}
	if (!link_carries_next_command())
		return false;

	printf("success!\n");
	return true;
}

/*
 * KEYSET is what BUSY is for on a PD that stores its key: the key reaches
 * the app once a session on it proves it, and the app answers that
 * session's first command with BUSY until the key is written. Every repeat
 * must verify under that session, and the PD switches to the new key only
 * when the app accepts it.
 */
static bool test_pd_app_busy_keyset(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_KEYSET,
		.keyset = {
			.type = 1,
			.length = 16,
		},
	};
	static const uint8_t key[16] = {
		0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
		0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
	};
	int i;

	printf(SUB_1 "PD app busy on a proven KEYSET -- ");

	memcpy(cmd.keyset.data, key, sizeof(key));
	g_ctx.busy_cmd_id = OSDP_CMD_KEYSET;
	g_ctx.busy_answers = 2;
	g_ctx.busy_cmd_calls = 0;
	test_completion_reset(&g_ctx.completion);

	if (!test_submit_command(g_ctx.cp_ctx, 0, &cmd)) {
		printf("failed to submit KEYSET!\n");
		return false;
	}
	if (!test_completion_wait(&g_ctx.completion, 1, 10)) {
		printf("KEYSET never completed!\n");
		return false;
	}
	for (i = 0; i < 100 && g_ctx.busy_cmd_calls < 3; i++)
		usleep(100 * 1000);
	g_ctx.busy_cmd_id = 0;

	if (test_completion_first_id(&g_ctx.completion) != OSDP_CMD_KEYSET ||
	    test_completion_first_status(&g_ctx.completion) !=
		    OSDP_COMPLETION_OK) {
		printf("expected OK completion, got cmd=%d status=%d!\n",
		       test_completion_first_id(&g_ctx.completion),
		       test_completion_first_status(&g_ctx.completion));
		return false;
	}
	if (g_ctx.busy_cmd_calls != 3) {
		printf("expected 3 calls into the app, saw %d!\n",
		       g_ctx.busy_cmd_calls);
		return false;
	}
	if (!link_carries_next_command())
		return false;

	printf("success!\n");
	return true;
}

/*
 * By COMSET_DONE the reply to COMSET is already on the wire, so an app
 * answering it with BUSY must change nothing: the PD still moves on to the
 * next sequence number, or the CP's next command would be out of step.
 */
static bool test_pd_app_busy_on_comset_done(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_COMSET,
		.comset = {
			.address = 101,
			.baud_rate = 9600,
		},
	};

	printf(SUB_1 "PD app busy on COMSET_DONE changes nothing -- ");

	g_ctx.busy_cmd_id = OSDP_CMD_COMSET_DONE;
	g_ctx.busy_answers = 1;
	g_ctx.busy_cmd_calls = 0;
	test_completion_reset(&g_ctx.completion);

	if (!test_submit_command(g_ctx.cp_ctx, 0, &cmd)) {
		printf("failed to submit COMSET!\n");
		return false;
	}
	if (!test_completion_wait(&g_ctx.completion, 1, 10) ||
	    test_completion_first_status(&g_ctx.completion) !=
		    OSDP_COMPLETION_OK) {
		printf("COMSET did not complete!\n");
		return false;
	}
	if (g_ctx.busy_cmd_calls != 1) {
		printf("expected 1 COMSET_DONE call, saw %d!\n",
		       g_ctx.busy_cmd_calls);
		return false;
	}
	g_ctx.busy_cmd_id = 0;
	if (!link_carries_next_command())
		return false;

	printf("success!\n");
	return true;
}

void run_busy_tests(struct test *t)
{
	printf("\nStarting BUSY reply tests\n");

	if (busy_test_setup(t, true)) {
		printf(SUB_1 "Failed to setup busy test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	TEST_CASE(t, "busy_retry_reuses_sequence_plain",
		  test_busy_retry_reuses_sequence(false));
	TEST_CASE(t, "busy_retry_reuses_sequence_checksum",
		  test_busy_retry_reuses_sequence(true));
	TEST_CASE(t, "pd_app_busy_then_ack", test_pd_app_busy_then_ack());
	TEST_CASE(t, "pd_app_busy_keyset", test_pd_app_busy_keyset());
	TEST_CASE(t, "pd_app_busy_on_comset_done",
		  test_pd_app_busy_on_comset_done());

	/* The hook answers for the PD, so through the whole retry budget the
	 * PD hears nothing; that is long enough for its SC session to lapse
	 * on a slow host, after which the CP drops the link. A real busy PD
	 * keeps hearing the CP, so run this case without SC. */
	busy_test_teardown();
	if (busy_test_setup(t, false)) {
		printf(SUB_1 "Failed to setup plain-text busy environment\n");
		TEST_REPORT(t, false);
		return;
	}
	TEST_CASE(t, "busy_permanent_fails_command_softly",
		  test_busy_permanent_fails_command_softly());

	busy_test_teardown();
}
