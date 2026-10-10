/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>

#include <osdp.h>
#include "test.h"

/* Test context for command tests */
struct test_command_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;

	/* Command tracking */
	bool cmd_seen;
	int last_cmd_id;
	void *last_cmd_data;

	/* Event tracking */
	bool event_seen;
	int last_event_type;
	void *last_event_data;

	/* Manufacturer command payload capture */
	bool mfg_nak_requested;
	uint32_t mfg_vendor_code;
	uint8_t mfg_data[64];
	int mfg_data_len;

	/* Event type the PD app must submit from within its CMD_MFG callback
	 * so that it rides out as the inline reply; 0 to defer to a poll. */
	int mfg_inline_reply;
	int last_mfg_event_type;

	/* When set, the PD app answers a biometric command from within its
	 * callback; otherwise it defers and the reply rides out on a poll. */
	bool bio_answer_inline;
	int last_bio_event_type;

	/* Command completion capture */
	/* Written by the completion callback on the refresh thread. */
	struct test_completion completion;
	/* last_mfg_event_type snapshot taken as the completion fired:
	 * non-zero means the dedicated event arrived in the same exchange
	 * as the command's reply (inline), not on a later poll. */
	atomic_int compl_mfg_event_type;
};

static struct test_command_ctx g_test_ctx = {0};

int test_commands_event_callback(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(pd);
	struct test_command_ctx *ctx = arg;

	ctx->event_seen = true;
	ctx->last_event_type = ev->type;

	if (ev->type == OSDP_EVENT_MFGREP ||
	    ev->type == OSDP_EVENT_MFGSTATR ||
	    ev->type == OSDP_EVENT_MFGERRR) {
		/* Tracked separately from last_event_type: a failed command also
		 * raises a notification event, which would clobber it. */
		ctx->last_mfg_event_type = ev->type;
		ctx->last_event_data = malloc(sizeof(struct osdp_event));
		memcpy(ctx->last_event_data, ev, sizeof(struct osdp_event));
	}

	if (ev->type == OSDP_EVENT_BIOREADR ||
	    ev->type == OSDP_EVENT_BIOMATCHR) {
		ctx->last_bio_event_type = ev->type;
		if (ctx->last_event_data) {
			free(ctx->last_event_data);
		}
		ctx->last_event_data = malloc(sizeof(struct osdp_event));
		memcpy(ctx->last_event_data, ev, sizeof(struct osdp_event));
	}

	return 0;
}

/*
 * Suite-local rather than test_cmd_completion_cb(): this one also snapshots
 * which manufacturer reply the PD had sent by the time the command completed.
 */
static void commands_cmd_completion_cb(void *arg, int pd,
				       struct osdp_cmd *cmd,
				       enum osdp_completion_status status)
{
	ARG_UNUSED(pd);
	struct test_command_ctx *ctx = arg;

	atomic_store(&ctx->compl_mfg_event_type, ctx->last_mfg_event_type);
	test_completion_record(&ctx->completion, cmd->id, (int)status);

	test_cmd_free(cmd);
}

/* Stands in for a scanned fingerprint template */
static const uint8_t bio_template[] = { 0xB1, 0x0C, 0x0D, 0xA7, 0xA0 };

/* Build the PD's answer to a biometric command */
static void bio_make_reply(struct osdp_event *ev, const struct osdp_cmd *cmd)
{
	memset(ev, 0, sizeof(*ev));

	if (cmd->id == OSDP_CMD_BIOREAD) {
		ev->type = OSDP_EVENT_BIOREADR;
		ev->bioreadr.reader = cmd->bioread.reader;
		ev->bioreadr.status = OSDP_BIO_STATUS_SUCCESS;
		ev->bioreadr.type = cmd->bioread.type;
		ev->bioreadr.quality = 0xC0;
		ev->bioreadr.length = sizeof(bio_template);
		memcpy(ev->bioreadr.data, bio_template, sizeof(bio_template));
	} else {
		ev->type = OSDP_EVENT_BIOMATCHR;
		ev->biomatchr.reader = cmd->biomatch.reader;
		ev->biomatchr.status = OSDP_BIO_STATUS_SUCCESS;
		ev->biomatchr.score = 0xFF;
	}
}

int test_commands_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_command_ctx *ctx = arg;

	ctx->cmd_seen = true;
	ctx->last_cmd_id = cmd->id;

	if (cmd->id == OSDP_CMD_BIOREAD || cmd->id == OSDP_CMD_BIOMATCH) {
		if (ctx->bio_answer_inline) {
			struct osdp_event reply;

			bio_make_reply(&reply, cmd);
			if (!test_submit_event(ctx->pd_ctx, &reply)) {
				printf(SUB_2 "Failed to submit inline bio reply\n");
			}
		}
		return 0;
	}

	/* Capture manufacturer command payload for async event test */
	if (cmd->id == OSDP_CMD_MFG) {
		if (ctx->mfg_nak_requested) {
			return -1;
		}
		ctx->mfg_vendor_code = cmd->mfg.vendor_code;
		ctx->mfg_data_len = cmd->mfg.length;
		memcpy(ctx->mfg_data, cmd->mfg.data, cmd->mfg.length);

		/* Answer synchronously: this must ride out as the reply to the
		 * MFG command itself, not as a later poll response. */
		if (ctx->mfg_inline_reply) {
			struct osdp_event reply;
			struct osdp_event *ev = &reply;

			memset(ev, 0, sizeof(*ev));
			ev->type = ctx->mfg_inline_reply;
			if (ctx->mfg_inline_reply == OSDP_EVENT_MFGREP) {
				ev->mfgrep.vendor_code = cmd->mfg.vendor_code;
				ev->mfgrep.length = cmd->mfg.length;
				memcpy(ev->mfgrep.data, cmd->mfg.data,
				       cmd->mfg.length);
			} else {
				struct osdp_event_mfgstat *ms =
					(ctx->mfg_inline_reply ==
					 OSDP_EVENT_MFGSTATR) ?
						&ev->mfgstatr :
						&ev->mfgerrr;

				ms->length = cmd->mfg.length;
				memcpy(ms->data, cmd->mfg.data, cmd->mfg.length);
			}
			if (!test_submit_event(ctx->pd_ctx, ev)) {
				printf(SUB_2 "Failed to submit inline MFG reply\n");
			}
		}
		return 0;
	}

	/* Handle status commands - fill in nr_entries to match PD capabilities */
	if (cmd->id == OSDP_CMD_STATUS) {
		switch (cmd->status.type) {
		case OSDP_STATUS_REPORT_INPUT:
			cmd->status.nr_entries = 8; /* matches OSDP_PD_CAP_CONTACT_STATUS_MONITORING.num_items */
			memset(cmd->status.report, 0, 8);
			break;
		case OSDP_STATUS_REPORT_OUTPUT:
			cmd->status.nr_entries = 4; /* matches OSDP_PD_CAP_OUTPUT_CONTROL.num_items */
			memset(cmd->status.report, 0, 4);
			break;
		default:
			break;
		}
		return 0;
	}

	/* Handle COMSET command lifecycle notifications */
	if (cmd->id == OSDP_CMD_COMSET || cmd->id == OSDP_CMD_COMSET_DONE) {
		/* COMSET requires special handling - we need to acknowledge it */
		return 0;
	}

	return 0;
}

static int setup_test_environment(struct test *t)
{
	printf(SUB_1 "setting up OSDP devices\n");

	if (test_setup_devices(t, &g_test_ctx.cp_ctx, &g_test_ctx.pd_ctx)) {
		printf(SUB_1 "Failed to setup devices!\n");
		return -1;
	}

	osdp_cp_set_event_callback(g_test_ctx.cp_ctx, test_commands_event_callback, &g_test_ctx);
	osdp_cp_set_command_completion_callback(g_test_ctx.cp_ctx,
						commands_cmd_completion_cb,
						&g_test_ctx);
	osdp_pd_set_command_callback(g_test_ctx.pd_ctx, test_commands_command_callback, &g_test_ctx);

	printf(SUB_1 "starting async runners\n");

	g_test_ctx.cp_runner = async_runner_start(g_test_ctx.cp_ctx, osdp_cp_refresh);
	g_test_ctx.pd_runner = async_runner_start(g_test_ctx.pd_ctx, osdp_pd_refresh);

	if (g_test_ctx.cp_runner < 0 || g_test_ctx.pd_runner < 0) {
		printf(SUB_1 "Failed to created CP/PD runners\n");
		return -1;
	}

	/* Wait for devices to come online */
	int rc = 0; /* elapsed time in ms */
	uint8_t status = 0;
	while (1) {
		if (rc > 10 * 1000) { /* ~10s online timeout */
			printf(SUB_1 "PD failed to come online\n");
			return -1;
		}
		osdp_get_status_mask(g_test_ctx.cp_ctx, &status);
		if (status & 1)
			break;
		usleep(20 * 1000);
		rc += 20;
	}

	return 0;
}

static void teardown_test_environment()
{
	printf(SUB_1 "tearing down test environment\n");

	async_runner_stop(g_test_ctx.cp_runner);
	async_runner_stop(g_test_ctx.pd_runner);

	osdp_cp_teardown(g_test_ctx.cp_ctx);
	osdp_pd_teardown(g_test_ctx.pd_ctx);

	/* Clean up any allocated event data */
	if (g_test_ctx.last_event_data) {
		free(g_test_ctx.last_event_data);
		g_test_ctx.last_event_data = NULL;
	}

	memset(&g_test_ctx, 0, sizeof(g_test_ctx));
}

static void reset_test_state()
{
	g_test_ctx.cmd_seen = false;
	g_test_ctx.last_cmd_id = 0;
	g_test_ctx.event_seen = false;
	g_test_ctx.last_event_type = 0;
	g_test_ctx.mfg_nak_requested = false;
	g_test_ctx.mfg_inline_reply = 0;
	g_test_ctx.last_mfg_event_type = 0;
	g_test_ctx.bio_answer_inline = false;
	g_test_ctx.last_bio_event_type = 0;
	g_test_ctx.mfg_vendor_code = 0;
	g_test_ctx.mfg_data_len = 0;
	memset(g_test_ctx.mfg_data, 0, sizeof(g_test_ctx.mfg_data));
	test_completion_reset(&g_test_ctx.completion);
	atomic_store(&g_test_ctx.compl_mfg_event_type, 0);

	if (g_test_ctx.last_event_data) {
		free(g_test_ctx.last_event_data);
		g_test_ctx.last_event_data = NULL;
	}
}

static bool wait_for_command(int expected_cmd_id, int timeout_sec)
{
	int rc = 0; /* elapsed time in ms */
	int timeout_ms = timeout_sec * 1000;
	while (rc < timeout_ms) {
		if (g_test_ctx.cmd_seen && g_test_ctx.last_cmd_id == expected_cmd_id) {
			return true;
		}
		usleep(20 * 1000);
		rc += 20;
	}
	return false;
}

static bool wait_for_event(int expected_event_type, int timeout_sec)
{
	int rc = 0; /* elapsed time in ms */
	int timeout_ms = timeout_sec * 1000;
	while (rc < timeout_ms) {
		if (g_test_ctx.event_seen && g_test_ctx.last_event_type == expected_event_type) {
			return true;
		}
		usleep(20 * 1000);
		rc += 20;
	}
	return false;
}

static bool wait_for_mfg_event(int expected_event_type, int timeout_sec)
{
	int rc = 0; /* elapsed time in ms */
	int timeout_ms = timeout_sec * 1000;
	while (rc < timeout_ms) {
		if (g_test_ctx.last_mfg_event_type == expected_event_type) {
			return true;
		}
		usleep(20 * 1000);
		rc += 20;
	}
	return false;
}

static bool wait_for_bio_event(int expected_event_type, int timeout_sec)
{
	int rc = 0; /* elapsed time in ms */
	int timeout_ms = timeout_sec * 1000;
	while (rc < timeout_ms) {
		if (g_test_ctx.last_bio_event_type == expected_event_type) {
			return true;
		}
		usleep(20 * 1000);
		rc += 20;
	}
	return false;
}

/*
 * Wait for the PD to (re)establish the online + secure-channel state. Needed
 * after COMSET/KEYSET, which tear down the session; without this the next
 * command can be submitted before the link recovers ("PD is not online").
 */
static bool wait_for_pd_online(int timeout_sec)
{
	int rc = 0; /* elapsed time in ms */
	int timeout_ms = timeout_sec * 1000;
	uint8_t status = 0, sc = 0;
	while (rc < timeout_ms) {
		osdp_get_status_mask(g_test_ctx.cp_ctx, &status);
		osdp_get_sc_status_mask(g_test_ctx.cp_ctx, &sc);
		if ((status & 1) && (sc & 1)) {
			return true;
		}
		usleep(20 * 1000);
		rc += 20;
	}
	return false;
}

/*
 * A biometric scan takes seconds in the real world, so the PD app normally
 * defers: the command is ACK'd and the reply rides out on a later poll. It may
 * also answer from within the callback, in which case the reply rides out on
 * the command itself. Both paths must deliver the same event to the CP.
 */
static bool test_bio_command(int cmd_id, int event_type, const char *name,
			     bool answer_inline)
{
	struct osdp_event *rx;
	struct osdp_cmd cmd = { .id = cmd_id };

	printf(SUB_2 "testing %s (%s reply)\n", name,
	       answer_inline ? "inline" : "deferred");
	reset_test_state();
	g_test_ctx.bio_answer_inline = answer_inline;

	if (cmd_id == OSDP_CMD_BIOREAD) {
		cmd.bioread.type = OSDP_BIO_TYPE_RIGHT_INDEX_FINGER_PRINT;
		cmd.bioread.format = OSDP_BIO_FMT_ANSI_INCITS_378;
		cmd.bioread.quality = 0x80;
	} else {
		cmd.biomatch.type = OSDP_BIO_TYPE_RIGHT_INDEX_FINGER_PRINT;
		cmd.biomatch.format = OSDP_BIO_FMT_ANSI_INCITS_378;
		cmd.biomatch.quality = 0x80;
		cmd.biomatch.length = sizeof(bio_template);
		memcpy(cmd.biomatch.data, bio_template, sizeof(bio_template));
	}

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send %s\n", name);
		return false;
	}

	if (!wait_for_command(cmd_id, 5)) {
		printf(SUB_2 "%s not received by PD\n", name);
		return false;
	}

	if (!answer_inline) {
		/* The app answers after its callback returned; this rides out
		 * as an unsolicited reply on the next poll. */
		struct osdp_event ev;
		struct osdp_cmd echo = { .id = cmd_id };

		if (cmd_id == OSDP_CMD_BIOREAD) {
			echo.bioread.type = OSDP_BIO_TYPE_RIGHT_INDEX_FINGER_PRINT;
		}
		bio_make_reply(&ev, &echo);
		if (!test_submit_event(g_test_ctx.pd_ctx, &ev)) {
			printf(SUB_2 "Failed to submit deferred %s reply\n", name);
			return false;
		}
	}

	if (!wait_for_bio_event(event_type, 5)) {
		printf(SUB_2 "%s reply not received by CP\n", name);
		return false;
	}

	rx = (struct osdp_event *)g_test_ctx.last_event_data;
	if (!rx) {
		printf(SUB_2 "%s reply data not captured\n", name);
		return false;
	}
	if (event_type == OSDP_EVENT_BIOREADR) {
		if (rx->bioreadr.status != OSDP_BIO_STATUS_SUCCESS ||
		    rx->bioreadr.type != OSDP_BIO_TYPE_RIGHT_INDEX_FINGER_PRINT ||
		    rx->bioreadr.quality != 0xC0 ||
		    rx->bioreadr.length != sizeof(bio_template) ||
		    memcmp(rx->bioreadr.data, bio_template,
			   sizeof(bio_template)) != 0) {
			printf(SUB_2 "BIOREADR data mismatch\n");
			return false;
		}
	} else {
		if (rx->biomatchr.status != OSDP_BIO_STATUS_SUCCESS ||
		    rx->biomatchr.score != 0xFF) {
			printf(SUB_2 "BIOMATCHR data mismatch\n");
			return false;
		}
	}

	return true;
}

static bool test_bio_commands(void)
{
	bool result = true;

	result &= test_bio_command(OSDP_CMD_BIOREAD, OSDP_EVENT_BIOREADR,
				   "BIOREAD", false);
	result &= test_bio_command(OSDP_CMD_BIOREAD, OSDP_EVENT_BIOREADR,
				   "BIOREAD", true);
	result &= test_bio_command(OSDP_CMD_BIOMATCH, OSDP_EVENT_BIOMATCHR,
				   "BIOMATCH", false);
	result &= test_bio_command(OSDP_CMD_BIOMATCH, OSDP_EVENT_BIOMATCHR,
				   "BIOMATCH", true);

	return result;
}

static bool cp_sees_pd_online(void)
{
	uint8_t status = 0;

	osdp_get_status_mask(g_test_ctx.cp_ctx, &status);
	return (status & 1U) != 0;
}

static bool test_buzzer_command()
{
	printf(SUB_2 "testing buzzer command\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_BUZZER,
		.buzzer = {
			.control_code = 1,
			.on_count = 10,
			.off_count = 10,
			.rep_count = 1,
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send buzzer command\n");
		return false;
	}

	return wait_for_command(OSDP_CMD_BUZZER, 5);
}

static bool test_led_command()
{
	printf(SUB_2 "testing LED command\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_LED,
		.led = {
			.led_number = 0,
			.temporary = {
				.control_code = 1,
				.on_count = 10,
				.off_count = 10,
				.on_color = OSDP_LED_COLOR_RED,
				.off_color = OSDP_LED_COLOR_NONE,
				.timer_count = 100,
			},
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send LED command\n");
		return false;
	}

	return wait_for_command(OSDP_CMD_LED, 5);
}

static bool test_output_command()
{
	printf(SUB_2 "testing output command\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_OUTPUT,
		.output = {
			.output_no = 0,
			.control_code = 1,
			.timer_count = 100,
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send output command\n");
		return false;
	}

	return wait_for_command(OSDP_CMD_OUTPUT, 5);
}

/*
 * OSDP v2.2 subclause 6.9 lets the PD answer osdp_OUT with an osdp_OSTATR
 * carrying the new output state, or ACK it and send the osdp_OSTATR later on a
 * poll. The PD app picks between the two by reporting the output status from
 * within its command callback, or not.
 *
 * This is invisible end-to-end -- either way the CP ends up with the same
 * status event, only on a different packet -- so these cases drive
 * pd_decode_command() directly and read pd->reply_id, which is what actually
 * goes on the wire. They own their devices and run no async runners, so the
 * shared test environment must not be up while they run.
 */

/* Exported as a test_<fn> alias under UNIT_TESTING via OSDP_TEST_ALIAS. */
extern int test_pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len);

/* The PD's OSDP_PD_CAP_OUTPUT_CONTROL num_items; an osdp_OSTATR must report
 * exactly this many outputs. Keep in sync with test_setup_devices(). */
#define TEST_NUM_OUTPUTS 4

enum out_submit_choice {
	OUT_SUBMIT_NOTHING,
	OUT_SUBMIT_OUTPUT_STATUS, /* the answer to osdp_OUT */
	OUT_SUBMIT_INPUT_STATUS,  /* unrelated; must not be eaten by osdp_OUT */
};

/* Built once by out_init_events() and submitted from the command callback;
 * test_submit_event() copies them into the pool. */
static struct osdp_event g_out_status;
static struct osdp_event g_input_status;
static enum out_submit_choice g_out_submit;
static osdp_t *g_out_pd_ctx;
static bool g_out_cmd_seen;
/* What the callback handed to libosdp -- the pooled copy, not the template. */
static struct osdp_event *g_out_submitted;

static int test_out_command_callback(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);

	if (cmd->id != OSDP_CMD_OUTPUT) {
		return 0;
	}
	g_out_cmd_seen = true;

	switch (g_out_submit) {
	case OUT_SUBMIT_OUTPUT_STATUS:
		g_out_submitted = test_submit_event(g_out_pd_ctx,
						    &g_out_status);
		break;
	case OUT_SUBMIT_INPUT_STATUS:
		g_out_submitted = test_submit_event(g_out_pd_ctx,
						    &g_input_status);
		break;
	case OUT_SUBMIT_NOTHING:
		break;
	}
	return 0;
}

static bool out_check_reply(struct osdp_pd *pd, int expected, const char *desc)
{
	if (pd->reply_id != expected) {
		printf(SUB_2 "%s: expected %s(%02x), got %s(%02x)\n", desc,
		       osdp_reply_name(expected), expected,
		       osdp_reply_name(pd->reply_id), pd->reply_id);
		return false;
	}
	printf(SUB_2 "%s\n", desc);
	return true;
}

static void out_init_events(void)
{
	int i;

	memset(&g_out_status, 0, sizeof(g_out_status));
	g_out_status.type = OSDP_EVENT_STATUS;
	g_out_status.status.type = OSDP_STATUS_REPORT_OUTPUT;
	g_out_status.status.nr_entries = TEST_NUM_OUTPUTS;
	for (i = 0; i < TEST_NUM_OUTPUTS; i++) {
		g_out_status.status.report[i] = (i == 0) ? 1 : 0;
	}

	memset(&g_input_status, 0, sizeof(g_input_status));
	g_input_status.type = OSDP_EVENT_STATUS;
	g_input_status.status.type = OSDP_STATUS_REPORT_INPUT;
	g_input_status.status.nr_entries = 8;
}

static bool test_output_reply_selection(struct test *t)
{
	/* osdp_OUT: output 0, control code 1 (permanent off), timer 0 */
	uint8_t out_cmd[] = { 0x68, 0x00, 0x01, 0x00, 0x00 };
	uint8_t poll_cmd[] = { 0x60 };
	osdp_t *cp = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd;
	bool result = true;

	printf(SUB_2 "testing reply selection for osdp_OUT\n");

	if (test_setup_devices(t, &cp, &pd_ctx)) {
		printf(SUB_2 "Failed to setup devices for osdp_OUT tests\n");
		return false;
	}
	out_init_events();
	g_out_pd_ctx = pd_ctx;
	pd = osdp_to_pd(pd_ctx, 0);
	osdp_pd_set_command_callback(pd_ctx, test_out_command_callback, NULL);

	/* An app that reports nothing gets the ACK-now-report-later behaviour */
	g_out_submit = OUT_SUBMIT_NOTHING;
	g_out_cmd_seen = false;
	test_pd_decode_command(pd, out_cmd, sizeof(out_cmd));
	if (!g_out_cmd_seen) {
		printf(SUB_2 "osdp_OUT did not reach the app\n");
		result = false;
	}
	result &= out_check_reply(pd, REPLY_ACK,
				  "app reports nothing: osdp_OUT is ACK'd");

	/* Reporting the status from the callback makes it the reply itself */
	g_out_submit = OUT_SUBMIT_OUTPUT_STATUS;
	test_pd_decode_command(pd, out_cmd, sizeof(out_cmd));
	result &= out_check_reply(pd, REPLY_OSTATR,
				  "app reports status: osdp_OUT is answered"
				  " inline with osdp_OSTATR");
	if (pd->active_event != g_out_submitted) {
		printf(SUB_2 "the reported status is not the active event\n");
		result = false;
	}
	/* Dropping libosdp's reference by hand means no completion will fire
	 * for it, so return it to the pool here. */
	test_event_free((struct osdp_event *)pd->active_event);
	pd->active_event = NULL;

	/* Only the answer to osdp_OUT may ride out as its reply; an unrelated
	 * event at the head of the queue must be left for a later poll. */
	g_out_submit = OUT_SUBMIT_INPUT_STATUS;
	test_pd_decode_command(pd, out_cmd, sizeof(out_cmd));
	result &= out_check_reply(pd, REPLY_ACK,
				  "an unrelated event is not consumed by"
				  " osdp_OUT");

	g_out_submit = OUT_SUBMIT_NOTHING;
	test_pd_decode_command(pd, poll_cmd, sizeof(poll_cmd));
	result &= out_check_reply(pd, REPLY_ISTATR,
				  "it rides out on the following poll instead");
	test_event_free((struct osdp_event *)pd->active_event);
	pd->active_event = NULL;

	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd_ctx);
	return result;
}

enum stat_answer {
	STAT_ANSWER_EVENT, /* submit an LSTAT reply event */
	STAT_ANSWER_IN_PLACE, /* fill in cmd->status */
	STAT_ANSWER_UNRELATED, /* submit an input status, fill in cmd->status */
};

static enum stat_answer g_stat_answer;
static struct osdp_event g_local_status;

static int test_stat_command_callback(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);

	if (cmd->id != OSDP_CMD_STATUS) {
		return 0;
	}
	switch (g_stat_answer) {
	case STAT_ANSWER_EVENT:
		g_out_submitted =
			test_submit_event(g_out_pd_ctx, &g_local_status);
		break;
	case STAT_ANSWER_UNRELATED:
		g_out_submitted =
			test_submit_event(g_out_pd_ctx, &g_input_status);
		/* fall through */
	case STAT_ANSWER_IN_PLACE:
		cmd->status = g_local_status.status;
		break;
	}
	return 0;
}

/*
 * A status query is answered with a status event submitted from inside the
 * command callback, like the other commands whose reply carries data, or --
 * the older way -- by filling in the command's status report.
 */
static bool test_status_reply_selection(struct test *t)
{
	uint8_t lstat_cmd[] = { 0x64 };
	uint8_t poll_cmd[] = { 0x60 };
	osdp_t *cp = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd;
	bool result = true;

	printf(SUB_2 "testing reply selection for osdp_LSTAT\n");

	if (test_setup_devices(t, &cp, &pd_ctx)) {
		printf(SUB_2 "Failed to setup devices for osdp_LSTAT tests\n");
		return false;
	}
	out_init_events();
	memset(&g_local_status, 0, sizeof(g_local_status));
	g_local_status.type = OSDP_EVENT_STATUS;
	g_local_status.status.type = OSDP_STATUS_REPORT_LOCAL;
	g_local_status.status.nr_entries = 2;
	g_local_status.status.report[0] = 1;
	g_out_pd_ctx = pd_ctx;
	pd = osdp_to_pd(pd_ctx, 0);
	osdp_pd_set_command_callback(pd_ctx, test_stat_command_callback, NULL);

	g_stat_answer = STAT_ANSWER_EVENT;
	test_pd_decode_command(pd, lstat_cmd, sizeof(lstat_cmd));
	result &= out_check_reply(pd, REPLY_LSTATR,
				  "status event: osdp_LSTAT is answered with"
				  " it");
	if (pd->active_event != g_out_submitted) {
		printf(SUB_2 "the reported status is not the active event\n");
		result = false;
	}
	test_event_free((struct osdp_event *)pd->active_event);
	pd->active_event = NULL;

	g_stat_answer = STAT_ANSWER_IN_PLACE;
	test_pd_decode_command(pd, lstat_cmd, sizeof(lstat_cmd));
	result &= out_check_reply(pd, REPLY_LSTATR,
				  "status filled in: osdp_LSTAT is answered"
				  " with it");
	pd->reply_prebuilt = false;

	g_stat_answer = STAT_ANSWER_UNRELATED;
	test_pd_decode_command(pd, lstat_cmd, sizeof(lstat_cmd));
	result &= out_check_reply(pd, REPLY_LSTATR,
				  "an unrelated event is not consumed by"
				  " osdp_LSTAT");
	pd->reply_prebuilt = false;
	test_pd_decode_command(pd, poll_cmd, sizeof(poll_cmd));
	result &= out_check_reply(pd, REPLY_ISTATR,
				  "it rides out on the following poll instead");
	test_event_free((struct osdp_event *)pd->active_event);
	pd->active_event = NULL;

	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd_ctx);
	return result;
}

static bool test_text_command()
{
	printf(SUB_2 "testing text command\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_TEXT,
		.text = {
			.control_code = 1,
			.temp_time = 30,
			.offset_row = 1,
			.offset_col = 1,
			.length = 7,
		},
	};
	strcpy((char *)cmd.text.data, "LibOSDP");

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send text command\n");
		return false;
	}

	return wait_for_command(OSDP_CMD_TEXT, 5);
}

static bool test_tdset_command()
{
	printf(SUB_2 "testing time and date set command\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_TDSET,
		.tdset = {
			.year = 2026,
			.month = 7,
			.day = 17,
			.hour = 12,
			.minute = 30,
			.second = 45,
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send tdset command\n");
		return false;
	}

	return wait_for_command(OSDP_CMD_TDSET, 5);
}

static bool test_mfg_command_simple()
{
	printf(SUB_2 "testing manufacturer command (simple)\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_MFG,
		.mfg = {
			.vendor_code = 0x00030201,
			.length = 10,
		},
	};
	uint8_t test_data[] = {9,1,9,2,6,3,1,7,7,0};
	memcpy(cmd.mfg.data, test_data, sizeof(test_data));

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send mfg command\n");
		return false;
	}

	return wait_for_command(OSDP_CMD_MFG, 5);
}

static bool test_mfg_command_with_reply()
{
	printf(SUB_2 "testing manufacturer command with async event\n");
	reset_test_state();

	uint8_t test_data[] = {9,1,9,2,6,3,1,7,7,0};

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_MFG,
		.mfg = {
			.vendor_code = 0x00030201,
			.length = sizeof(test_data),
		},
	};
	memcpy(cmd.mfg.data, test_data, sizeof(test_data));

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send mfg command with reply\n");
		return false;
	}

	/* Wait for command to be received */
	if (!wait_for_command(OSDP_CMD_MFG, 5)) {
		printf(SUB_2 "MFG command not received by PD\n");
		return false;
	}

	/* Submit async MFG reply event from PD app */
	struct osdp_event ev = {
		.type = OSDP_EVENT_MFGREP,
		.flags = 0,
	};
	ev.mfgrep.vendor_code = g_test_ctx.mfg_vendor_code;
	ev.mfgrep.length = g_test_ctx.mfg_data_len;
	memcpy(ev.mfgrep.data, g_test_ctx.mfg_data, g_test_ctx.mfg_data_len);
	if (!test_submit_event(g_test_ctx.pd_ctx, &ev)) {
		printf(SUB_2 "Failed to submit async MFGREP event\n");
		return false;
	}

	/* Wait for manufacturer reply event */
	if (!wait_for_event(OSDP_EVENT_MFGREP, 5)) {
		printf(SUB_2 "MFGREP event not received by CP\n");
		return false;
	}

	/* Verify the manufacturer reply event data */
	if (g_test_ctx.last_event_data) {
		struct osdp_event *ev = (struct osdp_event *)g_test_ctx.last_event_data;
		if (ev->mfgrep.vendor_code != 0x00030201 ||
		    ev->mfgrep.length != (int)sizeof(test_data) ||
		    memcmp(ev->mfgrep.data, test_data, sizeof(test_data)) != 0) {
			printf(SUB_2 "MFGREP event data mismatch\n");
			return false;
		}
	} else {
		printf(SUB_2 "MFGREP event data not captured\n");
		return false;
	}

	return true;
}

static bool test_mfg_command_nack_soft_fail()
{
	printf(SUB_2 "testing manufacturer command NAK soft-fail\n");
	reset_test_state();
	g_test_ctx.mfg_nak_requested = true;

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_MFG,
		.mfg = {
			.vendor_code = 0x00030201,
			.length = 4,
		},
	};
	uint8_t test_data[] = {1, 2, 3, 4};
	memcpy(cmd.mfg.data, test_data, sizeof(test_data));

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send mfg command (NAK path)\n");
		return false;
	}

	if (!wait_for_command(OSDP_CMD_MFG, 5)) {
		printf(SUB_2 "MFG command not received by PD (NAK path)\n");
		return false;
	}

	/* NAK on CMD_MFG is a soft failure; PD remains online. */
	usleep(200 * 1000);
	if (!cp_sees_pd_online()) {
		printf(SUB_2 "PD went offline after MFG NAK\n");
		return false;
	}

	return true;
}

static bool test_led_permanent_command()
{
	printf(SUB_2 "testing LED command (permanent mode)\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_LED,
		.led = {
			.led_number = 0,
			.permanent = {
				.control_code = 1,
				.on_count = 10,
				.off_count = 10,
				.on_color = OSDP_LED_COLOR_RED,
				.off_color = OSDP_LED_COLOR_NONE,
			},
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send LED permanent command\n");
		return false;
	}

	return wait_for_command(OSDP_CMD_LED, 5);
}

static bool test_comset_command()
{
	printf(SUB_2 "testing communication set command\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_COMSET,
		.comset = {
			.address = 101,
			.baud_rate = 9600,
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send comset command\n");
		return false;
	}

	if (!wait_for_command(OSDP_CMD_COMSET_DONE, 5)) {
		printf(SUB_2 "COMSET_DONE callback not received\n");
		return false;
	}

	/* Address change re-establishes the session; wait for it to settle. */
	return wait_for_pd_online(5);
}

static bool test_keyset_command()
{
	printf(SUB_2 "testing key set command\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_KEYSET,
		.keyset = {
			.type = 1,
			.length = 16,
		},
	};
	uint8_t key_data[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
	};
	memcpy(cmd.keyset.data, key_data, sizeof(key_data));

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send keyset command\n");
		return false;
	}

	if (!wait_for_command(OSDP_CMD_KEYSET, 5))
		return false;

	/* Rekey drops the secure channel; wait for it to re-establish before
	 * the next test submits a command. */
	return wait_for_pd_online(5);
}

static bool wait_for_cmd_completion(int expected_cmd, int expected_status,
				    int timeout_sec)
{
	int rc = 0; /* elapsed time in ms */
	int timeout_ms = timeout_sec * 1000;
	while (rc < timeout_ms) {
		if (test_completion_count(&g_test_ctx.completion) &&
		    test_completion_id(&g_test_ctx.completion) ==
			    expected_cmd &&
		    test_completion_status(&g_test_ctx.completion) ==
			    expected_status) {
			return true;
		}
		usleep(20 * 1000);
		rc += 20;
	}
	return false;
}

static bool test_tdset_invalid_time_naks()
{
	printf(SUB_2 "testing tdset command with invalid month NAKs\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_TDSET,
		.tdset = {
			.year = 2026,
			.month = 13,
			.day = 17,
			.hour = 12,
			.minute = 30,
			.second = 45,
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send tdset command\n");
		return false;
	}

	if (!wait_for_cmd_completion(OSDP_CMD_TDSET, OSDP_COMPLETION_FAILED, 5)) {
		printf(SUB_2 "NAK not reported (compl cmd=%d status=%d seen=%d)\n",
		       test_completion_id(&g_test_ctx.completion),
		       test_completion_status(&g_test_ctx.completion),
		       test_completion_count(&g_test_ctx.completion));
		return false;
	}

	if (g_test_ctx.cmd_seen && g_test_ctx.last_cmd_id == OSDP_CMD_TDSET) {
		printf(SUB_2 "PD app callback must not run for invalid time\n");
		return false;
	}

	return true;
}

/*
 * The PD app submits its manufacturer reply from within the CMD_MFG callback,
 * so it must ride out as the reply to that command rather than as an ACK
 * followed by a poll response.
 *
 * The completion-time event snapshot is what tells the two apart: an inline
 * MFGREP or MFGSTATR arrives with the command's own reply, so the dedicated
 * event has already been seen when the completion callback fires. A deferred
 * reply ACKs the command first and the event rides a later poll. An inline
 * MFGERRR completes the command FAILED while keeping the PD online.
 */
static bool test_mfg_command_inline_reply(int event_type, const char *name,
					  bool expect_failure)
{
	uint8_t test_data[] = {0xC0, 0xFF, 0xEE};
	struct osdp_event *rx;
	bool result = false;

	printf(SUB_2 "testing inline %s reply to manufacturer command\n", name);
	reset_test_state();
	g_test_ctx.mfg_inline_reply = event_type;

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_MFG,
		.mfg = {
			.vendor_code = 0x00030201,
			.length = sizeof(test_data),
		},
	};
	memcpy(cmd.mfg.data, test_data, sizeof(test_data));

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send mfg command\n");
		goto out;
	}

	if (!wait_for_mfg_event(event_type, 5)) {
		printf(SUB_2 "%s event not received by CP\n", name);
		goto out;
	}

	if (expect_failure) {
		if (!wait_for_cmd_completion(OSDP_CMD_MFG,
					     OSDP_COMPLETION_FAILED, 5)) {
			printf(SUB_2 "%s did not fail the mfg command\n", name);
			goto out;
		}
		/* An error reply is a soft failure; PD remains online. */
		usleep(200 * 1000);
		if (!cp_sees_pd_online()) {
			printf(SUB_2 "PD went offline after inline %s\n", name);
			goto out;
		}
	} else {
		if (!wait_for_cmd_completion(OSDP_CMD_MFG,
					     OSDP_COMPLETION_OK, 5)) {
			printf(SUB_2 "%s mfg command did not complete\n", name);
			goto out;
		}
		if (atomic_load(&g_test_ctx.compl_mfg_event_type) !=
		    event_type) {
			printf(SUB_2 "%s was ACK'd and deferred, not sent inline\n",
			       name);
			goto out;
		}
	}

	rx = (struct osdp_event *)g_test_ctx.last_event_data;
	if (!rx) {
		printf(SUB_2 "%s event data not captured\n", name);
		goto out;
	}
	if (event_type == OSDP_EVENT_MFGREP) {
		if (rx->mfgrep.vendor_code != 0x00030201 ||
		    rx->mfgrep.length != (int)sizeof(test_data) ||
		    memcmp(rx->mfgrep.data, test_data, sizeof(test_data)) != 0) {
			printf(SUB_2 "%s event data mismatch\n", name);
			goto out;
		}
	} else {
		struct osdp_event_mfgstat *ms =
			(event_type == OSDP_EVENT_MFGSTATR) ? &rx->mfgstatr :
							      &rx->mfgerrr;

		if (ms->length != (int)sizeof(test_data) ||
		    memcmp(ms->data, test_data, sizeof(test_data)) != 0) {
			printf(SUB_2 "%s event data mismatch\n", name);
			goto out;
		}
	}

	result = true;
out:
	return result;
}

static bool test_mfg_command_inline_replies()
{
	bool result = true;

	result &= test_mfg_command_inline_reply(OSDP_EVENT_MFGREP, "MFGREP", false);
	result &= test_mfg_command_inline_reply(OSDP_EVENT_MFGSTATR, "MFGSTATR", false);
	result &= test_mfg_command_inline_reply(OSDP_EVENT_MFGERRR, "MFGERRR", true);

	return result;
}

/*
 * Regression test for https://github.com/osdp-dev/libosdp/issues/262:
 * the PD used to unconditionally ACK multi-record commands (OUT/LED/BUZ)
 * even when pd_cmd_cap_ok() had set REPLY_NAK. The fix preserves the NAK
 * so the CP completes the command FAILED.
 */
static bool test_led_unsupported_capability_naks()
{
	printf(SUB_2 "testing LED command on unsupported led_number NAKs\n");
	reset_test_state();

	/* PD is configured with OSDP_PD_CAP_READER_LED_CONTROL num_items=1,
	 * so led_number=5 is out of range and must be NAK'd. */
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_LED,
		.led = {
			.led_number = 5,
			.temporary = {
				.control_code = 1,
				.on_count = 10,
				.off_count = 10,
				.on_color = OSDP_LED_COLOR_RED,
				.off_color = OSDP_LED_COLOR_NONE,
				.timer_count = 100,
			},
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send LED command\n");
		return false;
	}

	if (!wait_for_cmd_completion(OSDP_CMD_LED, OSDP_COMPLETION_FAILED, 5)) {
		printf(SUB_2 "NAK not reported (compl cmd=%d status=%d seen=%d)\n",
		       test_completion_id(&g_test_ctx.completion),
		       test_completion_status(&g_test_ctx.completion),
		       test_completion_count(&g_test_ctx.completion));
		return false;
	}

	if (g_test_ctx.cmd_seen && g_test_ctx.last_cmd_id == OSDP_CMD_LED) {
		printf(SUB_2 "PD app callback must not run for unsupported cap\n");
		return false;
	}

	return true;
}

/*
 * The completion callback is how command ownership returns to the app, so a
 * submission with none registered must be refused outright rather than
 * silently swallowing the command.
 */
static bool test_submit_requires_completion_callback()
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_BUZZER,
		.buzzer = {
			.control_code = 1,
			.on_count = 10,
			.off_count = 10,
			.rep_count = 1,
		},
	};

	osdp_pd_info_t info = {
		.baud_rate = 9600,
		.address = 101,
	};
	osdp_t *cp;
	bool result;

	printf(SUB_2 "testing submit without completion callback fails\n");
	reset_test_state();

	/* Callbacks are set only before the first refresh, so use a CP that
	 * never had one; it is never refreshed and can borrow the channel. */
	cp = osdp_cp_setup(&TO_OSDP(g_test_ctx.cp_ctx)->channel, 1, &info);
	if (cp == NULL) {
		printf(SUB_2 "cp setup failed\n");
		return false;
	}
	result = !test_submit_command(cp, 0, &cmd);
	if (!result) {
		printf(SUB_2 "submit accepted without a completion callback\n");
	}
	osdp_cp_teardown(cp);
	return result;
}

static bool test_status_command()
{
	printf(SUB_2 "testing status command\n");
	reset_test_state();

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_STATUS,
		.status = {
			.type = OSDP_STATUS_REPORT_INPUT,
		},
	};

	if (!test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send status command\n");
		return false;
	}

	return wait_for_command(OSDP_CMD_STATUS, 5);
}

void run_command_tests(struct test *t)
{
	printf("\nBegin Command Tests (pytest-style)\n");

	/* Owns its devices and drives the PD codec directly; must run before
	 * the shared environment brings up the async runners. */
	TEST_CASE(t, "output_reply_selection", test_output_reply_selection(t));
	TEST_CASE(t, "status_reply_selection", test_status_reply_selection(t));

	/* Setup test environment once */
	if (setup_test_environment(t) != 0) {
		printf(SUB_1 "Failed to setup test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	printf(SUB_1 "running command tests\n");

	TEST_CASE(t, "buzzer_command", test_buzzer_command());
	TEST_CASE(t, "led_command", test_led_command());
	TEST_CASE(t, "led_permanent_command", test_led_permanent_command());
	TEST_CASE(t, "output_command", test_output_command());
	TEST_CASE(t, "text_command", test_text_command());
	TEST_CASE(t, "tdset_command", test_tdset_command());
	TEST_CASE(t, "tdset_invalid_time_naks", test_tdset_invalid_time_naks());
	TEST_CASE(t, "comset_command", test_comset_command());
	TEST_CASE(t, "status_command", test_status_command());
	TEST_CASE(t, "keyset_command", test_keyset_command());
	TEST_CASE(t, "mfg_command_simple", test_mfg_command_simple());
	TEST_CASE(t, "mfg_command_with_reply", test_mfg_command_with_reply());
	TEST_CASE(t, "mfg_command_inline_replies",
		  test_mfg_command_inline_replies());
	TEST_CASE(t, "mfg_command_nack_soft_fail",
		  test_mfg_command_nack_soft_fail());
	TEST_CASE(t, "bio_commands", test_bio_commands());
	TEST_CASE(t, "submit_requires_completion_callback",
		  test_submit_requires_completion_callback());
	TEST_CASE(t, "led_unsupported_capability_naks",
		  test_led_unsupported_capability_naks());

	/* Teardown test environment */
	teardown_test_environment();
}
