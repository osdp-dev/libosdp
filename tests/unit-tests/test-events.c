/*
 * Copyright (c) 2025-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>

#include <osdp.h>
#include "test.h"

/* Test context for event tests */
struct test_event_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;

	/* Event tracking */
	bool event_seen;
	int last_event_type;
	void *last_event_data;

	/* Command tracking */
	bool cmd_seen;
	int last_cmd_id;
};

static struct test_event_ctx g_test_ctx = {0};

int test_events_event_callback(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(pd);
	struct test_event_ctx *ctx = arg;

	ctx->event_seen = true;
	ctx->last_event_type = ev->type;

	/* Store a copy of the event data for verification */
	if (ctx->last_event_data) {
		free(ctx->last_event_data);
	}
	ctx->last_event_data = malloc(sizeof(struct osdp_event));
	memcpy(ctx->last_event_data, ev, sizeof(struct osdp_event));

	return 0;
}

int test_events_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_event_ctx *ctx = arg;

	ctx->cmd_seen = true;
	ctx->last_cmd_id = cmd->id;

	return 0;
}

static int setup_test_environment(struct test *t)
{
	printf(SUB_1 "setting up OSDP devices\n");

	if (test_setup_devices(t, &g_test_ctx.cp_ctx, &g_test_ctx.pd_ctx)) {
		printf(SUB_1 "Failed to setup devices!\n");
		return -1;
	}

	osdp_cp_set_event_callback(g_test_ctx.cp_ctx, test_events_event_callback, &g_test_ctx);
	osdp_pd_set_command_callback(g_test_ctx.pd_ctx, test_events_command_callback, &g_test_ctx);

	printf(SUB_1 "starting async runners\n");

	g_test_ctx.cp_runner = async_runner_start(g_test_ctx.cp_ctx, osdp_cp_refresh);
	g_test_ctx.pd_runner = async_runner_start(g_test_ctx.pd_ctx, osdp_pd_refresh);

	if (g_test_ctx.cp_runner < 0 || g_test_ctx.pd_runner < 0) {
		printf(SUB_1 "Failed to created CP/PD runners\n");
		return -1;
	}

	/* Wait for devices to come online */
	int rc = 0;
	uint8_t status = 0;
	while (1) {
		if (rc > 10) {
			printf(SUB_1 "PD failed to come online\n");
			return -1;
		}
		osdp_get_status_mask(g_test_ctx.cp_ctx, &status);
		if (status & 1)
			break;
		usleep(1000 * 1000);
		rc++;
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
	g_test_ctx.event_seen = false;
	g_test_ctx.last_event_type = 0;
	g_test_ctx.cmd_seen = false;
	g_test_ctx.last_cmd_id = 0;

	if (g_test_ctx.last_event_data) {
		free(g_test_ctx.last_event_data);
		g_test_ctx.last_event_data = NULL;
	}
}

static bool wait_for_event(int expected_event_type, int timeout_sec)
{
	int rc = 0;
	while (rc < timeout_sec) {
		if (g_test_ctx.event_seen && g_test_ctx.last_event_type == expected_event_type) {
			return true;
		}
		usleep(1000 * 1000);
		rc++;
	}
	return false;
}

static bool test_cardread_event()
{
	printf(SUB_2 "testing cardread event\n");
	reset_test_state();

	struct osdp_event event = {
		.type = OSDP_EVENT_CARDREAD,
		.cardread = {
			.reader_no = 1,
			.format = OSDP_CARD_FMT_RAW_WIEGAND,
			.direction = 0,
			.length = 32,
		},
	};
	uint8_t card_data[] = {0x01, 0x23, 0x45, 0x67};
	memcpy(event.cardread.data, card_data, sizeof(card_data));

	if (osdp_pd_submit_event(g_test_ctx.pd_ctx, &event)) {
		printf(SUB_2 "Failed to submit cardread event\n");
		return false;
	}

	if (!wait_for_event(OSDP_EVENT_CARDREAD, 5)) {
		printf(SUB_2 "Cardread event not received\n");
		return false;
	}

	/* Verify event data */
	if (g_test_ctx.last_event_data) {
		struct osdp_event *ev = (struct osdp_event *)g_test_ctx.last_event_data;
		if (ev->cardread.reader_no != event.cardread.reader_no ||
		    ev->cardread.format != event.cardread.format ||
		    ev->cardread.length != event.cardread.length ||
		    memcmp(ev->cardread.data, event.cardread.data, 4) != 0) {
			printf(SUB_2 "Cardread event data mismatch\n");
			return false;
		}
	}

	return true;
}

static bool test_keypress_event()
{
	printf(SUB_2 "testing keypress event\n");
	reset_test_state();

	struct osdp_event event = {
		.type = OSDP_EVENT_KEYPRESS,
		.keypress = {
			.reader_no = 1,
			.length = 4,
		},
	};
	uint8_t key_data[] = {1, 2, 3, 4};
	memcpy(event.keypress.data, key_data, sizeof(key_data));

	if (osdp_pd_submit_event(g_test_ctx.pd_ctx, &event)) {
		printf(SUB_2 "Failed to submit keypress event\n");
		return false;
	}

	if (!wait_for_event(OSDP_EVENT_KEYPRESS, 5)) {
		printf(SUB_2 "Keypress event not received\n");
		return false;
	}

	/* Verify event data */
	if (g_test_ctx.last_event_data) {
		struct osdp_event *ev = (struct osdp_event *)g_test_ctx.last_event_data;
		if (ev->keypress.reader_no != event.keypress.reader_no ||
		    ev->keypress.length != event.keypress.length ||
		    memcmp(ev->keypress.data, event.keypress.data, 4) != 0) {
			printf(SUB_2 "Keypress event data mismatch\n");
			return false;
		}
	}

	return true;
}

static bool test_input_status_event()
{
	printf(SUB_2 "testing input status event\n");
	reset_test_state();

	struct osdp_event event = {
		.type = OSDP_EVENT_STATUS,
		.status = {
			.type = OSDP_STATUS_REPORT_INPUT,
			.nr_entries = 8,
		},
	};
	uint8_t status_data[] = {0, 1, 0, 1, 0, 1, 0, 1};
	memcpy(event.status.report, status_data, sizeof(status_data));

	if (osdp_pd_submit_event(g_test_ctx.pd_ctx, &event)) {
		printf(SUB_2 "Failed to submit input status event\n");
		return false;
	}

	if (!wait_for_event(OSDP_EVENT_STATUS, 5)) {
		printf(SUB_2 "Input status event not received\n");
		return false;
	}

	/* Verify event data */
	if (g_test_ctx.last_event_data) {
		struct osdp_event *ev = (struct osdp_event *)g_test_ctx.last_event_data;
		if (ev->status.type != event.status.type ||
		    ev->status.nr_entries != event.status.nr_entries ||
		    memcmp(ev->status.report, event.status.report, 8) != 0) {
			printf(SUB_2 "Input status event data mismatch\n");
			return false;
		}
	}

	return true;
}

static bool test_output_status_event()
{
	printf(SUB_2 "testing output status event\n");
	reset_test_state();

	struct osdp_event event = {
		.type = OSDP_EVENT_STATUS,
		.status = {
			.type = OSDP_STATUS_REPORT_OUTPUT,
			.nr_entries = 4,
		},
	};
	uint8_t status_data[] = {1, 0, 1, 0};
	memcpy(event.status.report, status_data, sizeof(status_data));

	if (osdp_pd_submit_event(g_test_ctx.pd_ctx, &event)) {
		printf(SUB_2 "Failed to submit output status event\n");
		return false;
	}

	if (!wait_for_event(OSDP_EVENT_STATUS, 5)) {
		printf(SUB_2 "Output status event not received\n");
		return false;
	}

	/* Verify event data */
	if (g_test_ctx.last_event_data) {
		struct osdp_event *ev = (struct osdp_event *)g_test_ctx.last_event_data;
		if (ev->status.type != event.status.type ||
		    ev->status.nr_entries != event.status.nr_entries ||
		    memcmp(ev->status.report, event.status.report, 4) != 0) {
			printf(SUB_2 "Output status event data mismatch\n");
			return false;
		}
	}

	return true;
}

static bool test_mfgrep_event()
{
	printf(SUB_2 "testing manufacturer reply event\n");
	reset_test_state();

	struct osdp_event event = {
		.type = OSDP_EVENT_MFGREP,
		.mfgrep = {
			.vendor_code = 0x00030201,
			.length = 8,
		},
	};
	uint8_t mfg_data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
	memcpy(event.mfgrep.data, mfg_data, sizeof(mfg_data));

	if (osdp_pd_submit_event(g_test_ctx.pd_ctx, &event)) {
		printf(SUB_2 "Failed to submit mfgrep event\n");
		return false;
	}

	if (!wait_for_event(OSDP_EVENT_MFGREP, 5)) {
		printf(SUB_2 "MFGREP event not received\n");
		return false;
	}

	/* Verify event data */
	if (g_test_ctx.last_event_data) {
		struct osdp_event *ev = (struct osdp_event *)g_test_ctx.last_event_data;
		if (ev->mfgrep.vendor_code != event.mfgrep.vendor_code ||
		    ev->mfgrep.length != event.mfgrep.length ||
		    memcmp(ev->mfgrep.data, event.mfgrep.data, 8) != 0) {
			printf(SUB_2 "MFGREP event data mismatch\n");
			return false;
		}
	}

	return true;
}

void run_event_tests(struct test *t)
{
	bool overall_result = true;

	printf("\nBegin Event Tests (pytest-style)\n");

	/* Setup test environment once */
	if (setup_test_environment(t) != 0) {
		printf(SUB_1 "Failed to setup test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	printf(SUB_1 "running event tests\n");

	/* Run all event tests */
	overall_result &= test_cardread_event();
	overall_result &= test_keypress_event();
	overall_result &= test_input_status_event();
	overall_result &= test_output_status_event();
	overall_result &= test_mfgrep_event();

	/* Teardown test environment */
	teardown_test_environment();

	printf(SUB_1 "Event tests %s\n", overall_result ? "succeeded" : "failed");
	TEST_REPORT(t, overall_result);
}
