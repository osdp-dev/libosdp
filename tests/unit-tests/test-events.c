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

/* Stashed by run_event_tests() so cases that rebuild the pair mid-suite
 * (e.g. after a deliberately induced protocol failure) can call
 * setup_test_environment() again without a NULL struct test *. */
static struct test *g_test;

/* Completion tracking for event lifetime tests */
static struct test_completion g_ev_compl;

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
	osdp_pd_set_event_completion_callback(
		g_test_ctx.pd_ctx, test_event_completion_cb, &g_ev_compl);

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

static bool test_cardread_event()
{
	printf(SUB_2 "testing cardread event\n");
	reset_test_state();

	struct osdp_event event = {
		.type = OSDP_EVENT_CARDREAD,
		.cardread = {
			.format = OSDP_CARD_FMT_RAW_WIEGAND,
			.direction = 0,
			.length = 32,
		},
	};
	uint8_t card_data[] = {0x01, 0x23, 0x45, 0x67};
	memcpy(event.cardread.data, card_data, sizeof(card_data));

	if (!test_submit_event(g_test_ctx.pd_ctx, &event)) {
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
			.length = 4,
		},
	};
	uint8_t key_data[] = {1, 2, 3, 4};
	memcpy(event.keypress.data, key_data, sizeof(key_data));

	if (!test_submit_event(g_test_ctx.pd_ctx, &event)) {
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

	if (!test_submit_event(g_test_ctx.pd_ctx, &event)) {
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

	if (!test_submit_event(g_test_ctx.pd_ctx, &event)) {
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

	if (!test_submit_event(g_test_ctx.pd_ctx, &event)) {
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

static bool test_mfgstat_event(enum osdp_event_type type, const char *name,
			       const uint8_t *data, uint8_t length)
{
	printf(SUB_2 "testing %s event with %d byte payload\n", name, length);
	reset_test_state();

	struct osdp_event event = { .type = type };
	struct osdp_event_mfgstat *mfgstat =
		(type == OSDP_EVENT_MFGSTATR) ? &event.mfgstatr : &event.mfgerrr;

	mfgstat->length = length;
	memcpy(mfgstat->data, data, length);

	if (!test_submit_event(g_test_ctx.pd_ctx, &event)) {
		printf(SUB_2 "Failed to submit %s event\n", name);
		return false;
	}

	if (!wait_for_event(type, 5)) {
		printf(SUB_2 "%s event not received\n", name);
		return false;
	}

	/* Verify event data */
	if (g_test_ctx.last_event_data) {
		struct osdp_event *ev = (struct osdp_event *)g_test_ctx.last_event_data;
		struct osdp_event_mfgstat *rx = (type == OSDP_EVENT_MFGSTATR) ?
						&ev->mfgstatr : &ev->mfgerrr;
		if (rx->length != length ||
		    memcmp(rx->data, data, length) != 0) {
			printf(SUB_2 "%s event data mismatch\n", name);
			return false;
		}
	}

	return true;
}

static bool test_mfgstat_events()
{
	uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
	bool result = true;

	result &= test_mfgstat_event(OSDP_EVENT_MFGSTATR, "MFGSTATR",
				     data, sizeof(data));
	result &= test_mfgstat_event(OSDP_EVENT_MFGERRR, "MFGERRR",
				     data, sizeof(data));

	/* The spec places no lower bound on the payload of these replies */
	result &= test_mfgstat_event(OSDP_EVENT_MFGSTATR, "MFGSTATR", data, 0);

	return result;
}

/*
 * The completion callback is how event ownership returns to the app, so a
 * submission with none registered must be refused outright rather than
 * silently swallowing the event.
 */
static bool test_submit_requires_completion_callback()
{
	struct osdp_event event = {
		.type = OSDP_EVENT_CARDREAD,
		.cardread = {
			.format = OSDP_CARD_FMT_RAW_WIEGAND,
			.length = 32,
		},
	};

	osdp_pd_info_t info = {
		.baud_rate = 9600,
		.address = 101,
	};
	osdp_t *pd;
	bool result;

	printf(SUB_2 "testing submit without completion callback fails\n");
	reset_test_state();

	/* Callbacks are set only before the first refresh, so use a PD that
	 * never had one; it is never refreshed and can borrow the channel. */
	pd = osdp_pd_setup(&TO_OSDP(g_test_ctx.pd_ctx)->channel, &info);
	if (pd == NULL) {
		printf(SUB_2 "pd setup failed\n");
		return false;
	}
	result = !test_submit_event(pd, &event);
	if (!result) {
		printf(SUB_2 "submit accepted without a completion callback\n");
	}
	osdp_pd_teardown(pd);
	return result;
}

/*
 * LibOSDP does not support osdp_FMT, so an ASCII card read must be refused
 * at submit -- never queued, never completed -- instead of being answered
 * with a bare ACK while the app believes the read went out. Both raw
 * formats must still be accepted and delivered.
 */
static bool test_cardread_formats()
{
	static const enum osdp_event_cardread_format_e raw_formats[] = {
		OSDP_CARD_FMT_RAW_UNSPECIFIED,
		OSDP_CARD_FMT_RAW_WIEGAND,
	};
	struct osdp_event event = {
		.type = OSDP_EVENT_CARDREAD,
		.cardread = {
			.format = OSDP_CARD_FMT_ASCII,
			.length = 8,
			.data = {0x5a},
		},
	};
	size_t i;

	printf(SUB_2 "testing cardread format acceptance\n");
	reset_test_state();
	test_completion_reset(&g_ev_compl);

	if (test_submit_event(g_test_ctx.pd_ctx, &event)) {
		printf(SUB_2 "ASCII cardread accepted\n");
		return false;
	}

	for (i = 0; i < ARRAY_SIZEOF(raw_formats); i++) {
		reset_test_state();
		event.cardread.format = raw_formats[i];
		if (!test_submit_event(g_test_ctx.pd_ctx, &event)) {
			printf(SUB_2 "format %d cardread refused\n",
			       raw_formats[i]);
			return false;
		}
		if (!wait_for_event(OSDP_EVENT_CARDREAD, 5) ||
		    ((struct osdp_event *)g_test_ctx.last_event_data)
				    ->cardread.format != raw_formats[i]) {
			printf(SUB_2 "format %d cardread not delivered\n",
			       raw_formats[i]);
			return false;
		}
		if (!test_completion_wait(&g_ev_compl, i + 1, 5)) {
			printf(SUB_2 "format %d cardread not completed\n",
			       raw_formats[i]);
			return false;
		}
	}

	/* One completion per accepted read; none for the refused one. */
	usleep(200 * 1000);
	if (test_completion_count(&g_ev_compl) != ARRAY_SIZEOF(raw_formats)) {
		printf(SUB_2 "expected %zu completions, got %d\n",
		       ARRAY_SIZEOF(raw_formats),
		       test_completion_count(&g_ev_compl));
		return false;
	}
	return true;
}

/*
 * A reply whose *content* overflows the TX buffer is degraded to a NAK by
 * pd_build_reply()'s catch-all and the exchange itself succeeds -- but the
 * event's data never reached the CP, so its completion must be FAILED, not
 * OK. Clamp the PD's advertised TX capability to exactly
 * OSDP_MINIMUM_PACKET_SIZE (128): packet init succeeds, while a full-size
 * MFGREP payload (4 + 128 bytes) cannot fit and degrades.
 */
static bool test_event_reply_degraded_to_nak_completes_failed()
{
	static struct osdp_event event = {
		.type = OSDP_EVENT_MFGREP,
		.mfgrep = {
			.vendor_code = 0x00030201,
			.length = OSDP_EVENT_MFGREP_MAX_DATALEN,
		},
	};
	struct osdp_pd *pd;
	bool result;

	printf(SUB_2 "testing completion when reply degrades to NAK\n");
	reset_test_state();
	test_completion_reset(&g_ev_compl);

	pd = osdp_to_pd(g_test_ctx.pd_ctx, 0);
	pd->peer_rx_size = 128; /* OSDP_MINIMUM_PACKET_SIZE */

	if (!test_submit_event(g_test_ctx.pd_ctx, &event)) {
		printf(SUB_2 "failed to submit event\n");
		return false;
	}

	result = test_completion_wait(&g_ev_compl, 1, 5);
	if (!result) {
		printf(SUB_2 "no completion for degraded event\n");
	} else if (test_completion_id(&g_ev_compl) != OSDP_EVENT_MFGREP ||
		   test_completion_status(&g_ev_compl) !=
			   OSDP_COMPLETION_FAILED) {
		printf(SUB_2 "unexpected completion: status=%d\n",
		       test_completion_status(&g_ev_compl));
		result = false;
	} else {
		/* Exactly once: no second report for the same event. */
		usleep(500 * 1000);
		if (test_completion_count(&g_ev_compl) != 1) {
			printf(SUB_2 "event completed %d times\n",
			       test_completion_count(&g_ev_compl));
			result = false;
		}
	}

	/* The CP saw a NAK to its POLL and parks in its offline dwell;
	 * rebuild the pair -- on every exit path -- so later cases see a
	 * live link with a fresh (unclamped) PD. */
	teardown_test_environment();
	return (setup_test_environment(g_test) == 0) && result;
}

/*
 * A reply that cannot fit the TX buffer exercises the pd_build_reply_packet()
 * failure path. The dequeued event must come back to the app as a FAILED
 * completion -- nothing retries it -- and it must be reported exactly once.
 *
 * A content-only overflow (e.g. an oversized cardread payload) does *not*
 * reach this path: pd_build_reply()'s catch-all degrades any single
 * reply-builder failure into a 2-byte NAK and still reports success, so
 * pd_build_reply_packet() returns OK and the exchange completes normally.
 * To reach the structural failure at osdp_phy_packet_init() (still too small
 * to even fit a NAK), clamp the PD's advertised TX capability
 * (pd->peer_rx_size, normally set by CMD_ACURXSIZE) below
 * OSDP_MINIMUM_PACKET_SIZE for the reply to this event's POLL.
 *
 * The clamp is written from this thread while the PD refresh thread reads
 * it: a deliberate test-only poke at library-internal state, since no API
 * reaches it. Do not copy this pattern outside fault injection.
 */
static bool test_event_reply_build_failure_completes()
{
	static struct osdp_event bad_event = {
		.type = OSDP_EVENT_CARDREAD,
		.cardread = {
			.format = OSDP_CARD_FMT_RAW_WIEGAND,
			.length = 8,
		},
	};
	struct osdp_pd *pd;

	printf(SUB_2 "testing completion on reply build failure\n");
	reset_test_state();
	test_completion_reset(&g_ev_compl);

	if (!test_submit_event(g_test_ctx.pd_ctx, &bad_event)) {
		printf(SUB_2 "failed to submit event\n");
		return false;
	}

	pd = osdp_to_pd(g_test_ctx.pd_ctx, 0);
	pd->peer_rx_size = 8;

	if (!test_completion_wait(&g_ev_compl, 1, 5)) {
		printf(SUB_2 "no completion for un-sendable event\n");
		return false;
	}
	if (test_completion_id(&g_ev_compl) != OSDP_EVENT_CARDREAD ||
	    test_completion_status(&g_ev_compl) != OSDP_COMPLETION_FAILED) {
		printf(SUB_2 "unexpected completion: status=%d\n",
		       test_completion_status(&g_ev_compl));
		return false;
	}

	/* Exactly once: no second report for the same event. */
	usleep(500 * 1000);
	if (test_completion_count(&g_ev_compl) != 1) {
		printf(SUB_2 "event completed %d times\n",
		       test_completion_count(&g_ev_compl));
		return false;
	}

	/* pd->peer_rx_size stays clamped to 8, so every future reply on
	 * this PD keeps hitting the same packet_init failure; rebuild the
	 * pair so later cases see a live link with a fresh (unclamped) PD. */
	teardown_test_environment();
	return setup_test_environment(g_test) == 0;
}

void run_event_tests(struct test *t)
{
	printf("\nBegin Event Tests (pytest-style)\n");

	g_test = t;

	/* Setup test environment once */
	if (setup_test_environment(t) != 0) {
		printf(SUB_1 "Failed to setup test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	printf(SUB_1 "running event tests\n");

	TEST_CASE(t, "cardread_event", test_cardread_event());
	TEST_CASE(t, "keypress_event", test_keypress_event());
	TEST_CASE(t, "input_status_event", test_input_status_event());
	TEST_CASE(t, "output_status_event", test_output_status_event());
	TEST_CASE(t, "mfgrep_event", test_mfgrep_event());
	TEST_CASE(t, "mfgstat_events", test_mfgstat_events());
	TEST_CASE(t, "cardread_formats", test_cardread_formats());
	TEST_CASE(t, "submit_requires_completion_callback",
		  test_submit_requires_completion_callback());
	TEST_CASE(t, "reply_degraded_to_nak_completion",
		  test_event_reply_degraded_to_nak_completes_failed());
	TEST_CASE(t, "reply_build_failure_completion",
		  test_event_reply_build_failure_completes());

	/* Teardown test environment */
	teardown_test_environment();
}
