/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>

#include <osdp.h>
#include "test.h"

/* Fills the whole app-side buffer AND exceeds one packet's data space
 * (OSDP_PACKET_BUF_SIZE is 256), so the reply must fragment and the CP must
 * pull the tail fragments with polls. */
#define PIV_REPLY_LEN OSDP_PIV_DATA_MAX_LEN

struct test_piv_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;

	/* PD side */
	atomic_int cmd_count;
	struct osdp_cmd_pivdata last_req;
	struct osdp_cmd_auth last_auth;
	bool inline_reply;
	int reply_event_type;

	/* CP side */
	atomic_int reply_count;
	struct osdp_event_piv_reply last_reply;
	atomic_int mp_start;
	atomic_int mp_progress;
	atomic_int mp_done;
	atomic_int mp_done_outcome;
	atomic_int replies_at_completion;
};

static struct test_piv_ctx g_piv;

/* Events are queued by reference, so an inline reply submitted from within the
 * command callback must outlive that callback's stack frame. */
static struct osdp_event g_piv_inline_reply;

static struct test_completion g_piv_compl;

static void piv_fill_reply(struct osdp_event *ev, int event_type)
{
	int i;

	memset(ev, 0, sizeof(*ev));
	ev->type = event_type;
	ev->piv_reply.length = PIV_REPLY_LEN;
	for (i = 0; i < PIV_REPLY_LEN; i++) {
		ev->piv_reply.data[i] = (uint8_t)(0xC0 + i);
	}
}

static int piv_pd_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_piv_ctx *ctx = arg;

	if (cmd->id == OSDP_CMD_PIVDATA) {
		ctx->last_req = cmd->pivdata;
	} else if (cmd->id == OSDP_CMD_GENAUTH ||
		   cmd->id == OSDP_CMD_CRAUTH) {
		ctx->last_auth = cmd->auth;
	} else {
		return 0;
	}
	ctx->cmd_count++;
	if (ctx->inline_reply) {
		piv_fill_reply(&g_piv_inline_reply, ctx->reply_event_type);
		if (!test_submit_event(ctx->pd_ctx, &g_piv_inline_reply)) {
			return -1;
		}
	}
	return 0;
}

static int piv_cp_event_callback(void *arg, int pd, struct osdp_event *ev)
{
	struct test_piv_ctx *ctx = arg;

	ARG_UNUSED(pd);
	if (ev->type == OSDP_EVENT_PIVDATAR ||
	    ev->type == OSDP_EVENT_GENAUTHR ||
	    ev->type == OSDP_EVENT_CRAUTHR) {
		ctx->last_reply = ev->piv_reply;
		ctx->reply_count++;
	} else if (ev->type == OSDP_EVENT_NOTIFICATION &&
		   (ev->notif.type == OSDP_NOTIFICATION_MP_START ||
		    ev->notif.type == OSDP_NOTIFICATION_MP_PROGRESS ||
		    ev->notif.type == OSDP_NOTIFICATION_MP_DONE)) {
		switch (ev->notif.type) {
		case OSDP_NOTIFICATION_MP_START:
			ctx->mp_start++;
			break;
		case OSDP_NOTIFICATION_MP_PROGRESS:
			ctx->mp_progress++;
			break;
		case OSDP_NOTIFICATION_MP_DONE:
			ctx->mp_done_outcome = ev->notif.mp.outcome;
			ctx->mp_done++;
			break;
		default:
			break;
		}
	}
	return 0;
}

static bool piv_wait_for(atomic_int *counter, int min, int timeout_ms)
{
	int waited = 0;

	while (*counter < min) {
		if (waited > timeout_ms) {
			return false;
		}
		usleep(10 * 1000);
		waited += 10;
	}
	return true;
}

static bool piv_run_op(const char *label, const struct osdp_cmd *cmd,
		       int reply_event_type, bool inline_reply)
{
	int i;

	g_piv.cmd_count = 0;
	g_piv.reply_count = 0;
	g_piv.mp_start = 0;
	g_piv.mp_progress = 0;
	g_piv.mp_done = 0;
	g_piv.mp_done_outcome = -1;
	g_piv.inline_reply = inline_reply;
	g_piv.reply_event_type = reply_event_type;
	memset(&g_piv.last_reply, 0, sizeof(g_piv.last_reply));
	memset(&g_piv.last_req, 0, sizeof(g_piv.last_req));
	memset(&g_piv.last_auth, 0, sizeof(g_piv.last_auth));

	if (!test_submit_command(g_piv.cp_ctx, 0, cmd)) {
		printf(SUB_2 "%s: submit failed\n", label);
		return false;
	}
	if (!piv_wait_for(&g_piv.cmd_count, 1, 2000)) {
		printf(SUB_2 "%s: PD never saw the command\n", label);
		return false;
	}
	if (!inline_reply) {
		struct osdp_event ev;

		usleep(100 * 1000); /* app takes its time; CP must ride ACKs */
		piv_fill_reply(&ev, reply_event_type);
		if (!test_submit_event(g_piv.pd_ctx, &ev)) {
			printf(SUB_2 "%s: submit_event failed\n", label);
			return false;
		}
	}
	if (!piv_wait_for(&g_piv.reply_count, 1, 5000)) {
		printf(SUB_2 "%s: CP never saw the reply\n", label);
		return false;
	}
	if (g_piv.last_reply.length != PIV_REPLY_LEN) {
		printf(SUB_2 "%s: reply length %u, want %u\n", label,
		       g_piv.last_reply.length, (unsigned int)PIV_REPLY_LEN);
		return false;
	}
	for (i = 0; i < PIV_REPLY_LEN; i++) {
		if (g_piv.last_reply.data[i] != (uint8_t)(0xC0 + i)) {
			printf(SUB_2 "%s: reply data mismatch at %d\n",
			       label, i);
			return false;
		}
	}
	/* Lifecycle: one START, one DONE(OK), and — since the reply spans
	 * more than one packet — at least one PROGRESS in between. */
	if (g_piv.mp_start != 1 || g_piv.mp_done != 1 ||
	    g_piv.mp_done_outcome != 0 || g_piv.mp_progress < 1) {
		printf(SUB_2 "%s: lifecycle start=%d progress=%d done=%d "
		       "outcome=%d\n", label, g_piv.mp_start,
		       g_piv.mp_progress, g_piv.mp_done, g_piv.mp_done_outcome);
		return false;
	}
	return true;
}

static bool test_pivdata_roundtrip(const char *label, bool inline_reply)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_PIVDATA,
		.pivdata = {
			.oid = { 0x5F, 0xC1, 0x02 },
			.element = 7,
			.offset = 0,
		},
	};

	printf(SUB_2 "PIVDATA %s reply round-trip\n", label);
	if (!piv_run_op(label, &cmd, OSDP_EVENT_PIVDATAR, inline_reply)) {
		return false;
	}
	if (g_piv.last_req.oid[0] != 0x5F || g_piv.last_req.oid[1] != 0xC1 ||
	    g_piv.last_req.oid[2] != 0x02 || g_piv.last_req.element != 7) {
		printf(SUB_2 "%s: request payload mangled\n", label);
		return false;
	}
	return true;
}

/* The challenge fills the app-side buffer, so the command leg itself spans
 * multiple CMD_GENAUTH/CMD_CRAUTH fragments (with the Algorithm/Key prefix
 * on the first one only) before the multi-fragment reply comes back. */
static bool test_auth_roundtrip(const char *name, const char *label,
				int cmd_id, int reply_event_type,
				bool inline_reply)
{
	struct osdp_cmd cmd = {
		.id = cmd_id,
		.auth = {
			.algorithm = 0xA7,
			.key = 0x9E,
			.length = OSDP_PIV_DATA_MAX_LEN,
		},
	};
	int i;

	for (i = 0; i < OSDP_PIV_DATA_MAX_LEN; i++) {
		cmd.auth.data[i] = (uint8_t)(0x10 + i);
	}

	printf(SUB_2 "%s %s reply round-trip\n", name, label);
	if (!piv_run_op(label, &cmd, reply_event_type, inline_reply)) {
		return false;
	}
	if (g_piv.last_auth.algorithm != 0xA7 || g_piv.last_auth.key != 0x9E) {
		printf(SUB_2 "%s: algorithm/key mangled (%02x/%02x)\n", label,
		       g_piv.last_auth.algorithm, g_piv.last_auth.key);
		return false;
	}
	if (g_piv.last_auth.length != OSDP_PIV_DATA_MAX_LEN) {
		printf(SUB_2 "%s: challenge length %u\n", label,
		       g_piv.last_auth.length);
		return false;
	}
	for (i = 0; i < OSDP_PIV_DATA_MAX_LEN; i++) {
		if (g_piv.last_auth.data[i] != (uint8_t)(0x10 + i)) {
			printf(SUB_2 "%s: challenge mismatch at %d\n", label,
			       i);
			return false;
		}
	}
	return true;
}

/* Stub file ops so a FILE_TX submit gets far enough to hit the §5.10.2
 * no-interleave check rather than failing on unregistered ops. */
static int piv_stub_fopen(void *arg, int file_id, uint32_t *size)
{
	ARG_UNUSED(arg); ARG_UNUSED(file_id);
	*size = 128;
	return 0;
}

static int piv_stub_fread(void *arg, void *buf, uint32_t size, uint32_t offset)
{
	ARG_UNUSED(arg); ARG_UNUSED(offset);
	memset(buf, 0xA5, size);
	return (int)size;
}

static int piv_stub_fwrite(void *arg, const void *buf, uint32_t size,
			   uint32_t offset)
{
	ARG_UNUSED(arg); ARG_UNUSED(buf); ARG_UNUSED(offset);
	return (int)size;
}

static int piv_stub_fclose(void *arg)
{
	ARG_UNUSED(arg);
	return 0;
}

static void piv_cmd_completion_cb(void *arg, int pd, struct osdp_cmd *cmd,
				  enum osdp_completion_status status)
{
	atomic_store(&g_piv.replies_at_completion,
		     atomic_load(&g_piv.reply_count));
	test_cmd_completion_cb(arg, pd, cmd, status);
}

/*
 * A PIVDATA completes when the operation ends, not when it is accepted: one
 * completion, carrying OK, arriving no earlier than MP_DONE.
 */
static bool test_pivdata_completes_at_mp_done(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_PIVDATA,
		.pivdata = { .oid = { 0x5F, 0xC1, 0x02 }, .element = 3 },
	};

	printf(SUB_2 "PIVDATA completes when the operation ends\n");

	g_piv.cmd_count = 0;
	g_piv.reply_count = 0;
	g_piv.mp_done = 0;
	g_piv.replies_at_completion = 0;
	g_piv.inline_reply = true;
	g_piv.reply_event_type = OSDP_EVENT_PIVDATAR;
	test_completion_reset(&g_piv_compl);

	if (!test_submit_command(g_piv.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "piv: submit rejected\n");
		return false;
	}
	/* Must NOT have completed synchronously. */
	if (test_completion_count(&g_piv_compl) != 0) {
		printf(SUB_2 "piv: completed at submit (count %d)\n",
		       test_completion_count(&g_piv_compl));
		return false;
	}
	if (!test_completion_wait(&g_piv_compl, 1, 5)) {
		printf(SUB_2 "piv: never completed\n");
		return false;
	}
	if (g_piv.mp_done != 1) {
		printf(SUB_2 "piv: mp_done %d, want 1\n", g_piv.mp_done);
		return false;
	}
	if (test_completion_count(&g_piv_compl) != 1) {
		printf(SUB_2 "piv: %d completions, want exactly 1\n",
		       test_completion_count(&g_piv_compl));
		return false;
	}
	if (test_completion_id(&g_piv_compl) != OSDP_CMD_PIVDATA) {
		printf(SUB_2 "piv: completed id %d, want PIVDATA\n",
		       test_completion_id(&g_piv_compl));
		return false;
	}
	if (test_completion_status(&g_piv_compl) != OSDP_COMPLETION_OK) {
		printf(SUB_2 "piv: status %d, want OK\n",
		       test_completion_status(&g_piv_compl));
		return false;
	}
	if (g_piv.replies_at_completion != 1) {
		printf(SUB_2 "piv: completed before its reply was delivered\n");
		return false;
	}
	return true;
}

static bool test_pivdata_busy_reject(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_PIVDATA,
		.pivdata = { .oid = { 1, 2, 3 }, .element = 1, .offset = 0 },
	};

	printf(SUB_2 "PIVDATA rejected while an op is in progress\n");

	g_piv.cmd_count = 0;
	g_piv.reply_count = 0;
	g_piv.mp_done = 0;
	g_piv.inline_reply = false; /* keeps the op open until we reply */
	g_piv.reply_event_type = OSDP_EVENT_PIVDATAR;
	test_completion_reset(&g_piv_compl);

	if (!test_submit_command(g_piv.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "busy: first submit failed\n");
		return false;
	}
	/* The engine owns it until the op ends; nothing settles at submit. */
	if (test_completion_count(&g_piv_compl) != 0) {
		printf(SUB_2 "pivdata completed at submit\n");
		return false;
	}
	if (test_submit_command(g_piv.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "busy: second submit was accepted\n");
		return false;
	}
	if (test_completion_count(&g_piv_compl) != 0) {
		printf(SUB_2 "unexpected completion fired for rejected pivdata submit\n");
		return false;
	}

	/* §5.10.2: a file transfer must also be refused while the PIV op is
	 * open -- the no-interleave rule cuts both ways. The suite registered
	 * stub file ops at setup. */
	struct osdp_cmd ftx = {
		.id = OSDP_CMD_FILE_TX,
		.file_tx = { .id = 1, .flags = 0 },
	};
	if (test_submit_command(g_piv.cp_ctx, 0, &ftx)) {
		printf(SUB_2 "busy: file tx accepted during PIV op\n");
		return false;
	}
	/* Let the op finish so later tests start clean. */
	if (!piv_wait_for(&g_piv.cmd_count, 1, 2000)) {
		printf(SUB_2 "busy: PD never saw the command\n");
		return false;
	}
	struct osdp_event ev;
	piv_fill_reply(&ev, OSDP_EVENT_PIVDATAR);
	if (!test_submit_event(g_piv.pd_ctx, &ev)) {
		printf(SUB_2 "busy: submit_event failed\n");
		return false;
	}
	if (!piv_wait_for(&g_piv.reply_count, 1, 5000)) {
		printf(SUB_2 "busy: op never completed\n");
		return false;
	}
	/* Only the accepted submit settles, and only once the op is over. */
	if (!test_completion_wait(&g_piv_compl, 1, 5) ||
	    test_completion_count(&g_piv_compl) != 1 ||
	    test_completion_status(&g_piv_compl) != OSDP_COMPLETION_OK) {
		printf(SUB_2 "busy: %d completions, last status %d\n",
		       test_completion_count(&g_piv_compl),
		       test_completion_status(&g_piv_compl));
		return false;
	}
	return true;
}

void run_piv_tests(struct test *t)
{
	struct osdp_file_ops stub_fops = {
		.open = piv_stub_fopen, .read = piv_stub_fread,
		.write = piv_stub_fwrite, .close = piv_stub_fclose,
	};
	int rc = 0;
	uint8_t status = 0;

	printf("\nBegin smartcard/PIV multipart tests\n");

	memset(&g_piv, 0, sizeof(g_piv));
	if (test_setup_devices_ext(t, &g_piv.cp_ctx, &g_piv.pd_ctx,
				   OSDP_FLAG_ENABLE_NOTIFICATION, 0)) {
		printf(SUB_1 "Failed to setup devices!\n");
		TEST_REPORT(t, false);
		return;
	}
	osdp_cp_set_event_callback(g_piv.cp_ctx, piv_cp_event_callback, &g_piv);
	osdp_pd_set_command_callback(g_piv.pd_ctx, piv_pd_command_callback,
				     &g_piv);
	osdp_cp_set_command_completion_callback(g_piv.cp_ctx,
						 piv_cmd_completion_cb,
						 &g_piv_compl);
	if (osdp_file_register_ops(g_piv.cp_ctx, 0, &stub_fops)) {
		printf(SUB_1 "Failed to register file ops\n");
		TEST_REPORT(t, false);
		osdp_cp_teardown(g_piv.cp_ctx);
		osdp_pd_teardown(g_piv.pd_ctx);
		return;
	}

	g_piv.cp_runner = async_runner_start(g_piv.cp_ctx, osdp_cp_refresh);
	g_piv.pd_runner = async_runner_start(g_piv.pd_ctx, osdp_pd_refresh);
	if (g_piv.cp_runner < 0 || g_piv.pd_runner < 0) {
		printf(SUB_1 "Failed to create CP/PD runners\n");
		TEST_REPORT(t, false);
		goto teardown;
	}

	while (1) {
		if (rc++ > 20) {
			printf(SUB_1 "PD failed to come online\n");
			TEST_REPORT(t, false);
			goto teardown;
		}
		osdp_get_status_mask(g_piv.cp_ctx, &status);
		if (status & 1) {
			break;
		}
		usleep(100 * 1000);
	}

	TEST_CASE(t, "pivdata_roundtrip_inline",
		  test_pivdata_roundtrip("inline", true));
	TEST_CASE(t, "pivdata_roundtrip_deferred",
		  test_pivdata_roundtrip("deferred", false));
	TEST_CASE(t, "genauth_inline",
		  test_auth_roundtrip("GENAUTH", "inline", OSDP_CMD_GENAUTH,
				      OSDP_EVENT_GENAUTHR, true));
	TEST_CASE(t, "genauth_deferred",
		  test_auth_roundtrip("GENAUTH", "deferred", OSDP_CMD_GENAUTH,
				      OSDP_EVENT_GENAUTHR, false));
	TEST_CASE(t, "crauth_inline",
		  test_auth_roundtrip("CRAUTH", "inline", OSDP_CMD_CRAUTH,
				      OSDP_EVENT_CRAUTHR, true));
	TEST_CASE(t, "crauth_deferred",
		  test_auth_roundtrip("CRAUTH", "deferred", OSDP_CMD_CRAUTH,
				      OSDP_EVENT_CRAUTHR, false));
	TEST_CASE(t, "pivdata_completes_at_mp_done",
		  test_pivdata_completes_at_mp_done());
	TEST_CASE(t, "pivdata_busy_reject", test_pivdata_busy_reject());

teardown:
	async_runner_stop(g_piv.cp_runner);
	async_runner_stop(g_piv.pd_runner);
	osdp_cp_teardown(g_piv.cp_ctx);
	osdp_pd_teardown(g_piv.pd_ctx);
}
