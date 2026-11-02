/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file Link lifecycle with both sides refreshing on their own threads: the
 * link comes up whichever side starts first, comes back after it was lost,
 * accepts a CP that was replaced under a running PD, and tears down cleanly
 * mid-traffic. Start delays and teardown points are drawn from rand(), which
 * the harness seeds per case.
 */

#include "test.h"

#define LINK_ONLINE_TOUT_SEC	8
#define LINK_OFFLINE_TOUT_MS	6000
#define LINK_CMD_TOUT_MS	3000
#define LINK_ORDER_ROUNDS	2

/*
 * A CP that hears nothing goes offline after its retry budget (about 2.4s
 * at the test build's timings) and then dwells there for minutes. Keep the
 * gap before the second side starts well inside that budget.
 */
#define LINK_START_GAP_MAX_MS	1000

#define LINK_TRAFFIC_CMDS	8
#define LINK_TRAFFIC_EVENTS	8
#define LINK_TEARDOWN_MAX_MS	300
#define LINK_TEARDOWN_ROUNDS	4

static const uint8_t link_scbk[16] = {
	0x4c, 0x49, 0x4e, 0x4b, 0x00, 0x01, 0x02, 0x03,
	0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
};

static struct {
	struct test *t;
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;
	atomic_int buzzers_seen;
	atomic_bool drop_cp_to_pd;
	atomic_bool drop_pd_to_cp;
	struct test_completion cmd_done;
	struct test_completion event_done;
} g_link;

static int link_pd_command_cb(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);

	if (cmd->id == OSDP_CMD_BUZZER)
		atomic_fetch_add(&g_link.buzzers_seen, 1);
	return 0;
}

static enum test_channel_hook_verdict link_hook(void *arg, bool cp_to_pd,
						const uint8_t *frame, int len,
						uint8_t *out, int *out_len,
						int out_max)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(frame);
	ARG_UNUSED(len);
	ARG_UNUSED(out);
	ARG_UNUSED(out_len);
	ARG_UNUSED(out_max);

	if (cp_to_pd ? atomic_load(&g_link.drop_cp_to_pd) :
		       atomic_load(&g_link.drop_pd_to_cp))
		return TEST_HOOK_DROP;
	return TEST_HOOK_PASS;
}

static void link_wire_cp(osdp_t *cp_ctx)
{
	osdp_cp_set_command_completion_callback(cp_ctx, test_cmd_completion_cb,
						&g_link.cmd_done);
}

static bool link_setup(void)
{
	g_link.cp_runner = g_link.pd_runner = -1;
	atomic_store(&g_link.buzzers_seen, 0);
	atomic_store(&g_link.drop_cp_to_pd, false);
	atomic_store(&g_link.drop_pd_to_cp, false);
	test_completion_reset(&g_link.cmd_done);
	test_completion_reset(&g_link.event_done);

	if (test_setup_devices_keys(g_link.t, &g_link.cp_ctx, &g_link.pd_ctx,
				    0, 0, link_scbk, link_scbk)) {
		printf(SUB_1 "device setup failed\n");
		return false;
	}
	link_wire_cp(g_link.cp_ctx);
	osdp_pd_set_command_callback(g_link.pd_ctx, link_pd_command_cb, NULL);
	osdp_pd_set_event_completion_callback(g_link.pd_ctx,
					      test_event_completion_cb,
					      &g_link.event_done);
	test_set_channel_hook(link_hook, NULL);
	return true;
}

static bool link_start_cp(void)
{
	g_link.cp_runner = async_runner_start(g_link.cp_ctx, osdp_cp_refresh);
	return g_link.cp_runner >= 0;
}

static bool link_start_pd(void)
{
	g_link.pd_runner = async_runner_start(g_link.pd_ctx, osdp_pd_refresh);
	return g_link.pd_runner >= 0;
}

static void link_stop_runners(void)
{
	if (g_link.cp_runner >= 0)
		async_runner_stop(g_link.cp_runner);
	if (g_link.pd_runner >= 0)
		async_runner_stop(g_link.pd_runner);
	g_link.cp_runner = g_link.pd_runner = -1;
}

static bool link_teardown(bool ok)
{
	link_stop_runners();
	test_set_channel_hook(NULL, NULL);
	if (g_link.cp_ctx)
		osdp_cp_teardown(g_link.cp_ctx);
	if (g_link.pd_ctx)
		osdp_pd_teardown(g_link.pd_ctx);
	g_link.cp_ctx = g_link.pd_ctx = NULL;
	return ok;
}

static bool link_fail(const char *why)
{
	printf(SUB_1 "%s (seed %u)\n", why, test_seed());
	return link_teardown(false);
}

static int link_rand_ms(int max_ms)
{
	return rand() % (max_ms + 1);
}

static bool link_is_online(void *arg)
{
	uint8_t mask = 0;

	ARG_UNUSED(arg);
	osdp_get_status_mask(g_link.cp_ctx, &mask);
	return mask & 1;
}

static bool link_is_offline(void *arg)
{
	return !link_is_online(arg);
}

static bool link_buzzer_seen(void *arg)
{
	return atomic_load(&g_link.buzzers_seen) >= *(int *)arg;
}

/* One command end to end: the PD app sees it and the CP completes it OK. */
static bool link_roundtrip(void)
{
	const struct osdp_cmd cmd = {
		.id = OSDP_CMD_BUZZER,
		.buzzer = { .control_code = 1, .on_count = 1, .off_count = 1,
			    .rep_count = 1 },
	};
	int want = atomic_load(&g_link.buzzers_seen) + 1;
	int done = test_completion_count(&g_link.cmd_done) + 1;

	if (!test_submit_command(g_link.cp_ctx, 0, &cmd)) {
		printf(SUB_1 "submit refused\n");
		return false;
	}
	if (!test_wait_until(link_buzzer_seen, &want, LINK_CMD_TOUT_MS)) {
		printf(SUB_1 "PD never saw the command\n");
		return false;
	}
	if (!test_completion_wait(&g_link.cmd_done, done,
				  LINK_CMD_TOUT_MS / 1000) ||
	    test_completion_status(&g_link.cmd_done) != OSDP_COMPLETION_OK) {
		printf(SUB_1 "command did not complete OK (status %d)\n",
		       test_completion_status(&g_link.cmd_done));
		return false;
	}
	return true;
}

static bool link_up_in_order(bool cp_first)
{
	int round, gap;

	for (round = 0; round < LINK_ORDER_ROUNDS; round++) {
		gap = link_rand_ms(LINK_START_GAP_MAX_MS);
		printf(SUB_1 "%s first, other side after %dms\n",
		       cp_first ? "CP" : "PD", gap);
		if (!link_setup())
			return false;
		/* What a CP sends before the PD listens is lost on a real
		 * wire; the mock channel would otherwise queue it up. */
		atomic_store(&g_link.drop_cp_to_pd, cp_first);
		if (!(cp_first ? link_start_cp() : link_start_pd()))
			return link_fail("runner start failed");
		usleep(gap * 1000);
		atomic_store(&g_link.drop_cp_to_pd, false);
		if (!(cp_first ? link_start_pd() : link_start_cp()))
			return link_fail("runner start failed");
		if (!test_wait_for_online(g_link.cp_ctx, 0,
					  LINK_ONLINE_TOUT_SEC))
			return link_fail("PD never came online");
		if (!link_roundtrip())
			return link_fail("no round-trip once online");
		link_teardown(true);
	}
	return true;
}

static bool link_up_cp_first(void)
{
	return link_up_in_order(true);
}

static bool link_up_pd_first(void)
{
	return link_up_in_order(false);
}

static bool link_bring_up(void)
{
	if (!link_setup())
		return false;
	if (!link_start_cp() || !link_start_pd())
		return link_fail("runner start failed");
	if (!test_wait_for_online(g_link.cp_ctx, 0, LINK_ONLINE_TOUT_SEC))
		return link_fail("PD never came online");
	return true;
}

/*
 * Cut one direction of the wire until the CP gives up on the PD, then
 * restore it. The CP's offline dwell is cut short by backdating the moment
 * it went offline, so recovery runs the same INIT path a real reconnect
 * does, without waiting out the production dwell.
 */
static bool link_recovers(atomic_bool *cut)
{
	struct osdp_pd *pd;

	if (!link_bring_up())
		return false;

	atomic_store(cut, true);
	if (!test_wait_until(link_is_offline, NULL, LINK_OFFLINE_TOUT_MS))
		return link_fail("CP never noticed the link loss");
	atomic_store(cut, false);

	test_api_lock();
	pd = osdp_to_pd(g_link.cp_ctx, 0);
	if (pd->state != OSDP_CP_STATE_OFFLINE) {
		test_api_unlock();
		return link_fail("CP is not in its offline dwell");
	}
	pd->tstamp = osdp_millis_now() - pd->wait_ms - 1;
	test_api_unlock();

	if (!test_wait_for_online(g_link.cp_ctx, 0, LINK_ONLINE_TOUT_SEC))
		return link_fail("PD never came back online");
	if (!link_roundtrip())
		return link_fail("no round-trip after recovery");
	return link_teardown(true);
}

static bool link_recovers_from_lost_commands(void)
{
	return link_recovers(&g_link.drop_cp_to_pd);
}

static bool link_recovers_from_lost_replies(void)
{
	return link_recovers(&g_link.drop_pd_to_cp);
}

/*
 * The CP process restarts while the PD keeps running: a fresh CP context
 * opens with sequence 0 and a new secure-channel handshake, which the PD
 * must accept in place of the session it still holds.
 */
static bool link_pd_accepts_restarted_cp(void)
{
	int gap;

	if (!link_bring_up())
		return false;
	if (!link_roundtrip())
		return link_fail("no round-trip before the restart");

	async_runner_stop(g_link.cp_runner);
	g_link.cp_runner = -1;
	osdp_cp_teardown(g_link.cp_ctx);

	gap = link_rand_ms(LINK_START_GAP_MAX_MS);
	printf(SUB_1 "new CP after %dms\n", gap);
	usleep(gap * 1000);

	g_link.cp_ctx = test_setup_cp(0, link_scbk);
	if (g_link.cp_ctx == NULL)
		return link_fail("CP setup failed");
	link_wire_cp(g_link.cp_ctx);
	if (!link_start_cp())
		return link_fail("runner start failed");
	if (!test_wait_for_online(g_link.cp_ctx, 0, LINK_ONLINE_TOUT_SEC))
		return link_fail("PD never came online for the new CP");
	if (!link_roundtrip())
		return link_fail("no round-trip with the new CP");
	return link_teardown(true);
}

/*
 * Stop both sides at a random point while commands and events are in
 * flight. Teardown owes every submitted object exactly one completion,
 * whether it got onto the wire or not.
 */
static bool link_teardown_once(void)
{
	const struct osdp_cmd cmd = {
		.id = OSDP_CMD_BUZZER,
		.buzzer = { .control_code = 1, .on_count = 1, .off_count = 1,
			    .rep_count = 1 },
	};
	const struct osdp_event ev = {
		.type = OSDP_EVENT_CARDREAD,
		.cardread = { .format = OSDP_CARD_FMT_RAW_WIEGAND,
			      .length = 8, .data = { 0xa5 } },
	};
	int i, at, cmds = 0, events = 0;

	if (!link_bring_up())
		return false;

	for (i = 0; i < LINK_TRAFFIC_CMDS; i++)
		cmds += test_submit_command(g_link.cp_ctx, 0, &cmd) != NULL;
	for (i = 0; i < LINK_TRAFFIC_EVENTS; i++)
		events += test_submit_event(g_link.pd_ctx, &ev) != NULL;
	if (cmds != LINK_TRAFFIC_CMDS || events != LINK_TRAFFIC_EVENTS)
		return link_fail("traffic submit refused");

	at = link_rand_ms(LINK_TEARDOWN_MAX_MS);
	printf(SUB_1 "tearing down %dms into the traffic\n", at);
	usleep(at * 1000);
	link_teardown(true);

	if (test_completion_count(&g_link.cmd_done) != cmds ||
	    test_completion_count(&g_link.event_done) != events) {
		printf(SUB_1 "completions: %d/%d commands, %d/%d events "
		       "(seed %u)\n", test_completion_count(&g_link.cmd_done),
		       cmds, test_completion_count(&g_link.event_done), events,
		       test_seed());
		return false;
	}
	if (test_alloc_outstanding() != 0) {
		printf(SUB_1 "%d pooled objects never returned (seed %u)\n",
		       test_alloc_outstanding(), test_seed());
		return false;
	}
	return true;
}

/* Each round lands the teardown at a different point of the exchange. */
static bool link_teardown_mid_traffic(void)
{
	int round;

	for (round = 0; round < LINK_TEARDOWN_ROUNDS; round++) {
		if (!link_teardown_once())
			return false;
	}
	return true;
}

void run_link_tests(struct test *t)
{
	printf("\nBegin Link Lifecycle Tests\n");

	g_link.t = t;
	TEST_CASE(t, "link_up_cp_first", link_up_cp_first());
	TEST_CASE(t, "link_up_pd_first", link_up_pd_first());
	TEST_CASE(t, "link_recovers_from_lost_commands",
		  link_recovers_from_lost_commands());
	TEST_CASE(t, "link_recovers_from_lost_replies",
		  link_recovers_from_lost_replies());
	TEST_CASE(t, "link_pd_accepts_restarted_cp",
		  link_pd_accepts_restarted_cp());
	TEST_CASE(t, "link_teardown_mid_traffic", link_teardown_mid_traffic());
}
