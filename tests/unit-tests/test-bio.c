/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file Biometric command handling that is awkward to observe end-to-end.
 *
 * The NAK code a PD sends never reaches the CP application -- the CP only logs
 * it -- so an over-the-wire test cannot tell OSDP_PD_NAK_BIO_TYPE apart from
 * OSDP_PD_NAK_RECORD. These tests drive pd_decode_command() directly instead
 * and read pd->nak_code, which is what actually goes on the wire.
 *
 * It also white-box tests the opt-in multi-part BIOREADR reply by driving the
 * osdp_bio build/consume path directly (the over-the-wire round-trip lives in
 * the pytest suite). The single-part biometric round-trips live in
 * test-commands.c.
 */

#include <osdp.h>
#include "test.h"
#include "osdp_bio.h"
#include "osdp_file.h"

/* Exported as a test_<fn> alias under UNIT_TESTING via OSDP_TEST_ALIAS. */
extern int test_pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len);

struct test_bio_ctx {
	int nak_ret;   /* what the command callback returns */
	bool cmd_seen;
	struct osdp_cmd last_cmd;
	bool mp_done_seen;
	int mp_done_outcome;
};

static struct test_bio_ctx g_bio_ctx;

static int test_bio_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_bio_ctx *ctx = arg;

	if (cmd->id == OSDP_CMD_NOTIFICATION &&
	    cmd->notif.type == OSDP_NOTIFICATION_MP_DONE) {
		ctx->mp_done_seen = true;
		ctx->mp_done_outcome = cmd->notif.mp.outcome;
		return 0;
	}
	ctx->cmd_seen = true;
	ctx->last_cmd = *cmd;
	return ctx->nak_ret;
}

/* osdp_BIOREAD: reader, type, format, quality */
static int bio_make_bioread(uint8_t *buf, uint8_t type)
{
	int len = 0;

	buf[len++] = 0x73; /* CMD_BIOREAD */
	buf[len++] = 0;    /* reader */
	buf[len++] = type;
	buf[len++] = OSDP_BIO_FMT_ANSI_INCITS_378;
	buf[len++] = 0x80; /* quality */
	return len;
}

/*
 * The app selects the NAK code by returning its negated value; only the codes
 * an app can know are honoured. Everything else -- including the idiomatic -1
 * -- must fall back to OSDP_PD_NAK_RECORD. An app that accepts and submits no
 * reply event gets an ACK. Every row first checks the decoded fields.
 */
static bool test_bio_nak_code_mapping(struct osdp_pd *pd)
{
	struct {
		int ret;
		int expected_nak;
		const char *desc;
	} cases[] = {
		{ 0, 0, "accepted and deferred is ACK'd" },
		{ -OSDP_PD_NAK_BIO_TYPE, OSDP_PD_NAK_BIO_TYPE,
		  "-NAK_BIO_TYPE selects BIO_TYPE" },
		{ -OSDP_PD_NAK_BIO_FMT, OSDP_PD_NAK_BIO_FMT,
		  "-NAK_BIO_FMT selects BIO_FMT" },
		{ -OSDP_PD_NAK_RECORD, OSDP_PD_NAK_RECORD,
		  "-NAK_RECORD selects RECORD" },
		{ -1, OSDP_PD_NAK_RECORD,
		  "legacy -1 still means RECORD" },
		{ -OSDP_PD_NAK_SC_COND, OSDP_PD_NAK_RECORD,
		  "a LibOSDP-only code falls back to RECORD" },
		{ -100, OSDP_PD_NAK_RECORD,
		  "a garbage value falls back to RECORD" },
	};
	uint8_t buf[16];
	int i, len;
	bool result = true;

	for (i = 0; i < (int)ARRAY_SIZEOF(cases); i++) {
		g_bio_ctx.nak_ret = cases[i].ret;
		g_bio_ctx.cmd_seen = false;

		len = bio_make_bioread(buf, OSDP_BIO_TYPE_LEFT_IRIS_SCAN);
		test_pd_decode_command(pd, buf, len);

		if (!g_bio_ctx.cmd_seen ||
		    g_bio_ctx.last_cmd.id != OSDP_CMD_BIOREAD) {
			printf(SUB_2 "%s: callback not invoked\n", cases[i].desc);
			result = false;
			continue;
		}
		if (g_bio_ctx.last_cmd.bioread.type !=
			    OSDP_BIO_TYPE_LEFT_IRIS_SCAN ||
		    g_bio_ctx.last_cmd.bioread.format !=
			    OSDP_BIO_FMT_ANSI_INCITS_378 ||
		    g_bio_ctx.last_cmd.bioread.quality != 0x80) {
			printf(SUB_2 "%s: fields decoded incorrectly\n",
			       cases[i].desc);
			result = false;
			continue;
		}
		if (cases[i].ret == 0) {
			if (pd->reply_id != REPLY_ACK) {
				printf(SUB_2 "%s: expected ACK, got reply %02x\n",
				       cases[i].desc, pd->reply_id);
				result = false;
			}
			continue;
		}
		if (pd->reply_id != REPLY_NAK) {
			printf(SUB_2 "%s: expected NAK, got reply %02x\n",
			       cases[i].desc, pd->reply_id);
			result = false;
			continue;
		}
		if (pd->nak_code != cases[i].expected_nak) {
			printf(SUB_2 "%s: expected NAK code %d, got %d\n",
			       cases[i].desc, cases[i].expected_nak,
			       pd->nak_code);
			result = false;
			continue;
		}
		printf(SUB_2 "%s\n", cases[i].desc);
	}

	return result;
}


/* A PD that reports no biometric capability must NAK without asking the app */
static bool test_bio_capability_gate(struct osdp_pd *pd)
{
	struct osdp_pd_cap saved;
	uint8_t buf[16];
	int len;
	bool result = true;

	printf(SUB_2 "testing BIOREAD is gated on OSDP_PD_CAP_BIOMETRICS\n");

	saved = pd->cap[OSDP_PD_CAP_BIOMETRICS];
	memset(&pd->cap[OSDP_PD_CAP_BIOMETRICS], 0, sizeof(struct osdp_pd_cap));

	g_bio_ctx.nak_ret = 0;
	g_bio_ctx.cmd_seen = false;

	len = bio_make_bioread(buf, OSDP_BIO_TYPE_RIGHT_THUMB_PRINT);
	test_pd_decode_command(pd, buf, len);

	if (g_bio_ctx.cmd_seen) {
		printf(SUB_2 "app callback must not run without the capability\n");
		result = false;
	}
	if (pd->reply_id != REPLY_NAK ||
	    pd->nak_code != OSDP_PD_NAK_CMD_UNKNOWN) {
		printf(SUB_2 "expected NAK(CMD_UNKNOWN), got reply %02x nak %d\n",
		       pd->reply_id, pd->nak_code);
		result = false;
	}

	pd->cap[OSDP_PD_CAP_BIOMETRICS] = saved;
	return result;
}

/*
 * Drive the opt-in multi-part BIOREADR reply path directly: build fragments on
 * a PD-role context and reassemble them on a CP-role context. `total_len` is
 * the template size and `max_len` bounds each fragment; a small max_len forces
 * several fragments, a large one keeps the reply single-part. `single` says
 * which: a single-part reply must engage neither side's multipart engine.
 */
static bool test_bio_multipart_transfer(struct osdp_pd *pd_tx,
					struct osdp_pd *pd_rx,
					int total_len, int max_len,
					bool single, const char *label)
{
	struct osdp_event ev, out;
	uint8_t frag[OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN + 32];
	const struct osdp_event *seed = &ev;
	int i, n, rc = 0, guard, frags = 0;

	printf(SUB_2 "%s\n", label);

	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_BIOREADR;
	ev.bioreadr.reader = 3;
	ev.bioreadr.status = OSDP_BIO_STATUS_SUCCESS;
	ev.bioreadr.type = OSDP_BIO_TYPE_RIGHT_THUMB_PRINT;
	ev.bioreadr.quality = 200;
	ev.bioreadr.length = total_len;
	for (i = 0; i < total_len; i++) {
		ev.bioreadr.data[i] = (uint8_t)(0xC0 + i);
	}
	memset(&out, 0, sizeof(out));

	/* A template the sender opens a transfer for, even one that then fits
	 * fragment 0, announces it with MP_DONE: listen for one. */
	g_bio_ctx.mp_done_seen = false;
	if (single)
		SET_FLAG(pd_tx, PD_FLAG_ENABLE_NOTIF);

	for (guard = 0; guard < 512; guard++) {
		bool first = (seed != NULL);

		if (!first && !osdp_bio_pd_reply_pending(pd_tx)) {
			printf(SUB_2 "%s: transfer stalled before completion\n",
			       label);
			return false;
		}
		n = osdp_bio_pd_reply_build(pd_tx, seed, frag, max_len);
		if (n < 0) {
			printf(SUB_2 "%s: fragment build failed\n", label);
			return false;
		}
		if (first) {
			/* Fragment 0 carries the standard BIOREADR header, with
			 * the `length` field holding the total template size. */
			if (frag[0] != 3 ||
			    frag[1] != OSDP_BIO_STATUS_SUCCESS ||
			    frag[2] != OSDP_BIO_TYPE_RIGHT_THUMB_PRINT ||
			    frag[3] != 200) {
				printf(SUB_2 "%s: fragment 0 header swap wrong\n",
				       label);
				return false;
			}
			if ((frag[4] | (frag[5] << 8)) != total_len) {
				printf(SUB_2 "%s: fragment 0 length %d != total\n",
				       label, frag[4] | (frag[5] << 8));
				return false;
			}
		}
		rc = osdp_bio_cp_reply_consume(pd_rx, frag, n, &out);
		seed = NULL;
		frags++;
		if (rc != 0) {
			break;
		}
	}
	if (rc != 1) {
		printf(SUB_2 "%s: reassembly did not complete (rc=%d)\n",
		       label, rc);
		return false;
	}
	CLEAR_FLAG(pd_tx, PD_FLAG_ENABLE_NOTIF);
	if (single ? (frags != 1 || g_bio_ctx.mp_done_seen ||
		      osdp_bio_is_active(pd_rx)) :
		     frags < 2) {
		printf(SUB_2 "%s: %d fragments, sender MP_DONE %d, receiver "
		       "active %d\n", label, frags, g_bio_ctx.mp_done_seen,
		       osdp_bio_is_active(pd_rx));
		return false;
	}
	osdp_bio_cp_reply_finish(pd_rx);
	if (out.type != OSDP_EVENT_BIOREADR || out.bioreadr.reader != 3 ||
	    out.bioreadr.status != OSDP_BIO_STATUS_SUCCESS ||
	    out.bioreadr.type != OSDP_BIO_TYPE_RIGHT_THUMB_PRINT ||
	    out.bioreadr.quality != 200 || out.bioreadr.length != total_len) {
		printf(SUB_2 "%s: reassembled header fields mismatch\n", label);
		return false;
	}
	for (i = 0; i < total_len; i++) {
		if (out.bioreadr.data[i] != (uint8_t)(0xC0 + i)) {
			printf(SUB_2 "%s: reassembled data mismatch at %d\n",
			       label, i);
			return false;
		}
	}
	return true;
}

/* Enough for a file transfer to open and stay open; never actually read. */
static int bio_stub_fopen(void *arg, int file_id, uint32_t *size)
{
	ARG_UNUSED(arg); ARG_UNUSED(file_id);
	*size = 4096;
	return 0;
}

static int bio_stub_fread(void *arg, void *buf, uint32_t size, uint32_t offset)
{
	ARG_UNUSED(arg); ARG_UNUSED(offset);
	memset(buf, 0x5A, size);
	return (int)size;
}

static int bio_stub_fwrite(void *arg, const void *buf, uint32_t size,
			   uint32_t offset)
{
	ARG_UNUSED(arg); ARG_UNUSED(buf); ARG_UNUSED(offset);
	return (int)size;
}

static int bio_stub_fclose(void *arg)
{
	ARG_UNUSED(arg);
	return 0;
}

/*
 * Two multipart engines really can be live at once on one PD:
 * osdp_mp_engine_busy() is consulted at submit only, and the CP bio engine
 * arms itself on reply receipt without asking. So a bio MP_DONE must not
 * settle a file transfer's command -- the completion hook is gated on the
 * multipart family that owns pd->engine_cmd, and the file command must still
 * be outstanding afterwards and later report its own outcome.
 */
/*
 * A cancel ends a reassembly at the engine's next tick, and says so on the
 * wire with CMD_ABORT. Driven fragment-by-fragment rather than over a live
 * link: on a real pair this template reassembles in a couple of polls, so
 * there is no reliable window to cancel in.
 */
static bool test_bio_cancel_ends_reassembly(struct osdp_pd *pd_tx,
					    struct osdp_pd *pd_rx)
{
	struct osdp_event ev, out;
	uint8_t frag[OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN + 32];
	int n;

	printf(SUB_2 "a cancel ends a bio reassembly\n");

	/* Nothing running yet: the request must be refused. */
	if (osdp_bio_request_cancel(pd_rx) == 0) {
		printf(SUB_2 "cancel accepted while idle\n");
		return false;
	}

	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_BIOREADR;
	ev.bioreadr.reader = 3;
	ev.bioreadr.status = OSDP_BIO_STATUS_SUCCESS;
	ev.bioreadr.type = OSDP_BIO_TYPE_RIGHT_THUMB_PRINT;
	ev.bioreadr.quality = 200;
	ev.bioreadr.length = 200;
	memset(&out, 0, sizeof(out));

	/* Fragment 0 only: enough to arm the CP engine, short of completing. */
	n = osdp_bio_pd_reply_build(pd_tx, &ev, frag, 64);
	if (n < 0 || osdp_bio_cp_reply_consume(pd_rx, frag, n, &out) != 0) {
		printf(SUB_2 "failed to arm the CP reassembly\n");
		return false;
	}
	if (!osdp_bio_is_active(pd_rx)) {
		printf(SUB_2 "engine not active after fragment 0\n");
		return false;
	}

	if (osdp_bio_request_cancel(pd_rx)) {
		printf(SUB_2 "cancel refused a running reassembly\n");
		return false;
	}
	/* Still running: the request is cooperative, not immediate. */
	if (!osdp_bio_is_active(pd_rx)) {
		printf(SUB_2 "cancel tore the op down synchronously\n");
		return false;
	}
	if (osdp_bio_cp_get_command(pd_rx) != CMD_ABORT) {
		printf(SUB_2 "cancelled op did not yield CMD_ABORT\n");
		return false;
	}
	if (osdp_bio_is_active(pd_rx)) {
		printf(SUB_2 "engine still active after the abort tick\n");
		return false;
	}
	osdp_bio_abort(pd_tx); /* leave the sender idle for later cases */
	return true;
}

static bool test_bio_done_does_not_steal_file_cmd(osdp_t *cp_ctx,
						  struct osdp_pd *pd,
						  struct osdp_pd *cp_pd)
{
	struct osdp_file_ops fops = {
		.arg = NULL,
		.open = bio_stub_fopen, .read = bio_stub_fread,
		.write = bio_stub_fwrite, .close = bio_stub_fclose,
	};
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_FILE_TX,
		.file_tx = { .id = 1, .flags = 0 },
	};
	struct test_completion comp;
	enum osdp_cp_state_e saved = cp_pd->state;
	bool result = false;

	printf(SUB_2 "a bio MP_DONE does not settle the file tx command\n");

	test_completion_reset(&comp);
	osdp_cp_set_command_completion_callback(cp_ctx, test_cmd_completion_cb,
						&comp);
	if (osdp_file_register_ops(cp_ctx, 0, &fops)) {
		printf(SUB_2 "file ops registration failed\n");
		goto out;
	}

	/* Submission is gated on ONLINE and this suite drives no link. */
	cp_pd->state = OSDP_CP_STATE_ONLINE;
	if (!test_submit_command(cp_ctx, 0, &cmd)) {
		printf(SUB_2 "file tx submit rejected\n");
		goto out;
	}
	if (!osdp_file_tx_is_active(cp_pd) || cp_pd->engine_cmd == NULL) {
		printf(SUB_2 "file tx did not take the engine slot\n");
		goto out;
	}

	/* Now run a whole multi-fragment BIOREADR to DONE on the same PD. */
	if (!test_bio_multipart_transfer(pd, cp_pd, 200, 64, false,
					 "bio reply alongside a file tx")) {
		goto out;
	}
	if (test_completion_count(&comp) != 0) {
		printf(SUB_2 "bio DONE settled the file cmd (count %d, "
		       "status %d)\n", test_completion_count(&comp),
		       test_completion_status(&comp));
		goto out;
	}
	if (cp_pd->engine_cmd == NULL) {
		printf(SUB_2 "bio DONE cleared the file tx engine slot\n");
		goto out;
	}

	/* The file transfer's own terminal path still reports to it. */
	osdp_file_tx_abort(cp_pd);
	if (test_completion_count(&comp) != 1 ||
	    test_completion_id(&comp) != OSDP_CMD_FILE_TX ||
	    test_completion_status(&comp) != OSDP_COMPLETION_FAILED) {
		printf(SUB_2 "file tx: %d completions, id %d, status %d\n",
		       test_completion_count(&comp),
		       test_completion_id(&comp),
		       test_completion_status(&comp));
		goto out;
	}
	result = true;
out:
	cp_pd->state = saved;
	osdp_cp_set_command_completion_callback(cp_ctx, NULL, NULL);
	return result;
}

/*
 * A live CP/PD pair with multipart BIOREADR enabled on both roles. The
 * white-box cases above drive no link, so the completion path -- which only
 * exists inside a real transaction -- needs a pair of its own.
 */
#define BIO_MP_TEMPLATE_LEN OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN

struct test_bio_mp_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;
	atomic_int mp_done;
	atomic_int reply_len;
	atomic_int reply_len_at_completion;
	struct test_completion comp;
};

static struct test_bio_mp_ctx g_bio_mp;

/* Answer the scan inline, so the CP's own BIOREAD transaction carries
 * fragment 0 -- the moment the command used to complete at. */
static int bio_mp_pd_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_bio_mp_ctx *ctx = arg;
	struct osdp_event ev;
	int i;

	if (cmd->id != OSDP_CMD_BIOREAD) {
		return 0;
	}
	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_BIOREADR;
	ev.bioreadr.reader = cmd->bioread.reader;
	ev.bioreadr.status = OSDP_BIO_STATUS_SUCCESS;
	ev.bioreadr.type = cmd->bioread.type;
	ev.bioreadr.quality = 0x80;
	ev.bioreadr.length = BIO_MP_TEMPLATE_LEN;
	for (i = 0; i < BIO_MP_TEMPLATE_LEN; i++) {
		ev.bioreadr.data[i] = (uint8_t)(0xC0 + i);
	}
	return test_submit_event(ctx->pd_ctx, &ev) ? 0 : -1;
}

static int bio_mp_cp_event_callback(void *arg, int pd, struct osdp_event *ev)
{
	struct test_bio_mp_ctx *ctx = arg;

	ARG_UNUSED(pd);
	if (ev->type == OSDP_EVENT_BIOREADR) {
		ctx->reply_len = ev->bioreadr.length;
	} else if (ev->type == OSDP_EVENT_NOTIFICATION &&
		   ev->notif.type == OSDP_NOTIFICATION_MP_DONE) {
		ctx->mp_done++;
	}
	return 0;
}

static void bio_mp_cmd_completion_cb(void *arg, int pd, struct osdp_cmd *cmd,
				     enum osdp_completion_status status)
{
	atomic_store(&g_bio_mp.reply_len_at_completion,
		     atomic_load(&g_bio_mp.reply_len));
	test_cmd_completion_cb(arg, pd, cmd, status);
}

/*
 * With multipart BIOREADR on, the command owns the whole reassembly: it must
 * not complete when the first fragment lands, but once -- with OK -- after the
 * template is whole and its reply has been delivered.
 */
static bool test_bioread_multipart_completes_at_reassembly(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_BIOREAD,
		.bioread = {
			.reader = 0,
			.type = OSDP_BIO_TYPE_RIGHT_THUMB_PRINT,
			.format = OSDP_BIO_FMT_ANSI_INCITS_378,
			.quality = 0x80,
		},
	};

	printf(SUB_2 "a multipart BIOREAD completes at reassembly\n");

	test_completion_reset(&g_bio_mp.comp);
	g_bio_mp.mp_done = 0;
	g_bio_mp.reply_len = 0;
	g_bio_mp.reply_len_at_completion = 0;

	if (!test_submit_command(g_bio_mp.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "bio: submit rejected\n");
		return false;
	}
	if (test_completion_count(&g_bio_mp.comp) != 0) {
		printf(SUB_2 "bio: completed at submit\n");
		return false;
	}
	if (!test_completion_wait(&g_bio_mp.comp, 1, 5)) {
		printf(SUB_2 "bio: never completed\n");
		return false;
	}
	if (g_bio_mp.mp_done != 1) {
		printf(SUB_2 "bio: completed before reassembly finished "
		       "(mp_done %d)\n", (int)g_bio_mp.mp_done);
		return false;
	}
	if (g_bio_mp.reply_len != BIO_MP_TEMPLATE_LEN) {
		printf(SUB_2 "bio: reassembled %d bytes, want %d\n",
		       (int)g_bio_mp.reply_len, BIO_MP_TEMPLATE_LEN);
		return false;
	}
	if (g_bio_mp.reply_len_at_completion != BIO_MP_TEMPLATE_LEN) {
		printf(SUB_2 "bio: completed before its reply was delivered\n");
		return false;
	}
	if (test_completion_count(&g_bio_mp.comp) != 1) {
		printf(SUB_2 "bio: %d completions, want exactly 1\n",
		       test_completion_count(&g_bio_mp.comp));
		return false;
	}
	if (test_completion_id(&g_bio_mp.comp) != OSDP_CMD_BIOREAD) {
		printf(SUB_2 "bio: completed id %d, want BIOREAD\n",
		       test_completion_id(&g_bio_mp.comp));
		return false;
	}
	if (test_completion_status(&g_bio_mp.comp) != OSDP_COMPLETION_OK) {
		printf(SUB_2 "bio: status %d, want OK\n",
		       test_completion_status(&g_bio_mp.comp));
		return false;
	}
	return true;
}

static void run_bio_multipart_link_tests(struct test *t)
{
	memset(&g_bio_mp, 0, sizeof(g_bio_mp));
	g_bio_mp.cp_runner = g_bio_mp.pd_runner = -1; /* 0 is a valid work id */
	if (test_setup_devices_ext(t, &g_bio_mp.cp_ctx, &g_bio_mp.pd_ctx,
				   OSDP_FLAG_BIOREADR_MULTIPART |
					   OSDP_FLAG_ENABLE_NOTIFICATION,
				   OSDP_FLAG_BIOREADR_MULTIPART)) {
		printf(SUB_1 "multipart device setup failed!\n");
		TEST_REPORT(t, false);
		return;
	}
	osdp_cp_set_event_callback(g_bio_mp.cp_ctx, bio_mp_cp_event_callback,
				   &g_bio_mp);
	osdp_pd_set_command_callback(g_bio_mp.pd_ctx,
				     bio_mp_pd_command_callback, &g_bio_mp);
	osdp_cp_set_command_completion_callback(
		g_bio_mp.cp_ctx, bio_mp_cmd_completion_cb, &g_bio_mp.comp);

	g_bio_mp.cp_runner = async_runner_start(g_bio_mp.cp_ctx,
						osdp_cp_refresh);
	g_bio_mp.pd_runner = async_runner_start(g_bio_mp.pd_ctx,
						osdp_pd_refresh);
	if (g_bio_mp.cp_runner < 0 || g_bio_mp.pd_runner < 0) {
		printf(SUB_1 "Failed to create CP/PD runners\n");
		TEST_REPORT(t, false);
		goto teardown;
	}
	if (!test_wait_for_online(g_bio_mp.cp_ctx, 0, 10)) {
		printf(SUB_1 "PD failed to come online\n");
		TEST_REPORT(t, false);
		goto teardown;
	}

	TEST_CASE(t, "bioread_multipart_completes_at_reassembly",
		  test_bioread_multipart_completes_at_reassembly());

teardown:
	if (g_bio_mp.cp_runner >= 0) {
		async_runner_stop(g_bio_mp.cp_runner);
	}
	if (g_bio_mp.pd_runner >= 0) {
		async_runner_stop(g_bio_mp.pd_runner);
	}
	osdp_cp_teardown(g_bio_mp.cp_ctx);
	osdp_pd_teardown(g_bio_mp.pd_ctx);
}

/*
 * Losing CP activity mid multi-part BIOREADR reply must tear the bio op down
 * exactly like CMD_ABORT does: the op goes inactive and the app receives
 * MP_DONE(ABORTED). Drives the real osdp_pd_refresh() offline path by
 * backdating pd->tstamp past OSDP_PD_ONLINE_TOUT_MS.
 */
static bool test_bio_abort_on_pd_offline(struct osdp_pd *pd, osdp_t *pd_ctx)
{
	struct osdp_event ev;
	uint8_t frag[96];
	bool result = false;

	printf(SUB_2 "testing bio abort on PD link loss\n");

	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_BIOREADR;
	ev.bioreadr.reader = 1;
	ev.bioreadr.status = OSDP_BIO_STATUS_SUCCESS;
	ev.bioreadr.type = OSDP_BIO_TYPE_RIGHT_THUMB_PRINT;
	ev.bioreadr.quality = 100;
	ev.bioreadr.length = 200;

	if (osdp_bio_pd_reply_build(pd, &ev, frag, 64) < 0 ||
	    !osdp_bio_is_active(pd)) {
		printf(SUB_2 "failed to start multi-part reply\n");
		return false;
	}

	g_bio_ctx.mp_done_seen = false;
	SET_FLAG(pd, PD_FLAG_ENABLE_NOTIF);
	pd_set_online(pd);
	pd->tstamp = osdp_millis_now() - (OSDP_PD_ONLINE_TOUT_MS + 100);
	osdp_pd_refresh(pd_ctx);

	if (osdp_bio_is_active(pd)) {
		printf(SUB_2 "bio op still active after link loss\n");
		goto out;
	}
	if (!g_bio_ctx.mp_done_seen) {
		printf(SUB_2 "no MP_DONE notification on link loss\n");
		goto out;
	}
	if (g_bio_ctx.mp_done_outcome != OSDP_MP_OUTCOME_ABORTED) {
		printf(SUB_2 "MP_DONE outcome %d, want ABORTED\n",
		       g_bio_ctx.mp_done_outcome);
		goto out;
	}
	result = true;
out:
	CLEAR_FLAG(pd, PD_FLAG_ENABLE_NOTIF);
	return result;
}

void run_bio_tests(struct test *t)
{
	osdp_t *cp = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd, *cp_pd;

	printf("\nBegin Biometric Tests\n");

	if (test_setup_devices(t, &cp, &pd_ctx)) {
		printf(SUB_1 "device setup failed!\n");
		TEST_REPORT(t, false);
		return;
	}

	pd = osdp_to_pd(pd_ctx, 0);
	cp_pd = osdp_to_pd(cp, 0);
	osdp_pd_set_command_callback(pd_ctx, test_bio_command_callback,
				     &g_bio_ctx);

	TEST_CASE(t, "bio_nak_code_mapping", test_bio_nak_code_mapping(pd));
	TEST_CASE(t, "bio_capability_gate", test_bio_capability_gate(pd));
	/* bio_done_does_not_steal_file_cmd runs the multi-fragment
	 * round-trip; this is its single-packet counterpart. */
	TEST_CASE(t, "bio_multipart_single_packet",
		  test_bio_multipart_transfer(
			  pd, cp_pd, 30, 200, true,
			  "single-packet BIOREADR stays single-part"));
	TEST_CASE(t, "bio_done_does_not_steal_file_cmd",
		  test_bio_done_does_not_steal_file_cmd(cp, pd, cp_pd));
	TEST_CASE(t, "bio_cancel_ends_reassembly",
		  test_bio_cancel_ends_reassembly(pd, cp_pd));
	TEST_CASE(t, "bio_abort_on_pd_offline",
		  test_bio_abort_on_pd_offline(pd, pd_ctx));

	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd_ctx);

	/* Needs a live link, so it runs on a pair of its own. */
	run_bio_multipart_link_tests(t);
}
