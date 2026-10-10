/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cross-cutting completion behaviour that belongs to no single engine:
 * re-entrancy from inside a completion, and the teardown guard.
 */

#include <osdp.h>
#include "test.h"

struct completion_ctx {
	osdp_t *cp;
	osdp_t *pd;
	int cp_runner;
	int pd_runner;
	struct test_completion comp;
	/* Set by a callback that re-enters libosdp; read by the test. */
	atomic_int resubmit_rc;
	atomic_bool resubmit_armed;
	int add_pd_rc;
	int register_rc;
};

static struct completion_ctx g_comp;

/*
 * Completion callbacks can only be set before the first refresh, so each
 * context gets one that forwards to whatever the running case swaps in.
 */
static cp_command_completion_callback_t g_cp_completion;
static void *g_cp_completion_arg;
static pd_event_completion_callback_t g_pd_completion;
static void *g_pd_completion_arg;

static void cp_completion_dispatch(void *arg, int pd, struct osdp_cmd *cmd,
				   enum osdp_completion_status status)
{
	ARG_UNUSED(arg);
	g_cp_completion(g_cp_completion_arg, pd, cmd, status);
}

static void pd_completion_dispatch(void *arg, struct osdp_event *ev,
				   enum osdp_completion_status status)
{
	ARG_UNUSED(arg);
	g_pd_completion(g_pd_completion_arg, ev, status);
}

static void use_cp_completion(cp_command_completion_callback_t cb, void *arg)
{
	test_api_lock();
	g_cp_completion = cb;
	g_cp_completion_arg = arg;
	test_api_unlock();
}

static void use_pd_completion(pd_event_completion_callback_t cb, void *arg)
{
	test_api_lock();
	g_pd_completion = cb;
	g_pd_completion_arg = arg;
	test_api_unlock();
}

static struct osdp_cmd make_led_cmd(void)
{
	struct osdp_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.id = OSDP_CMD_LED;
	cmd.led.reader = 0;
	cmd.led.led_number = 0;
	cmd.led.temporary.control_code = OSDP_CMD_LED_TEMPORARY_CC_SET;
	cmd.led.temporary.on_count = 10;
	cmd.led.temporary.off_count = 10;
	cmd.led.temporary.on_color = OSDP_LED_COLOR_RED;
	cmd.led.temporary.off_color = OSDP_LED_COLOR_NONE;
	cmd.led.temporary.timer_count = 10;
	cmd.led.permanent.control_code = OSDP_CMD_LED_PERMANENT_CC_NOP;
	return cmd;
}

/* Completion that submits a fresh command from inside itself. */
static void resubmit_completion_cb(void *arg, int pd, struct osdp_cmd *cmd,
				   enum osdp_completion_status status)
{
	struct completion_ctx *c = arg;

	ARG_UNUSED(pd);
	test_completion_record(&c->comp, cmd->id, (int)status);
	if (atomic_exchange(&c->resubmit_armed, false)) {
		struct osdp_cmd next = make_led_cmd();
		bool queued = test_submit_command(c->cp, 0, &next) != NULL;

		atomic_store(&c->resubmit_rc, queued ? 1 : 0);
	}
	test_cmd_free(cmd);
}

/*
 * A command submitted from inside a FLUSHED completion belongs to the queue
 * that exists *after* the flush: flush removes what was queued when flush was
 * called, so the new command must survive.
 */
static bool test_submit_from_flush_completion(void)
{
	struct osdp_cmd cmd = make_led_cmd();

	test_completion_reset(&g_comp.comp);
	atomic_store(&g_comp.resubmit_rc, -1);
	atomic_store(&g_comp.resubmit_armed, true);

	if (!test_submit_command(g_comp.cp, 0, &cmd)) {
		printf(SUB_2 "flush: submit rejected\n");
		return false;
	}
	osdp_cp_flush_commands(g_comp.cp, 0);

	if (test_completion_count(&g_comp.comp) < 1) {
		printf(SUB_2 "flush: no completion fired\n");
		return false;
	}
	if (test_completion_status(&g_comp.comp) != OSDP_COMPLETION_FLUSHED) {
		printf(SUB_2 "flush: status %d, want FLUSHED\n",
		       test_completion_status(&g_comp.comp));
		return false;
	}
	if (atomic_load(&g_comp.resubmit_rc) != 1) {
		printf(SUB_2 "flush: re-entrant submit failed\n");
		return false;
	}
	/* The re-entrant command must still be queued: flushing again finds
	 * exactly it. */
	if (osdp_cp_flush_commands(g_comp.cp, 0) != 1) {
		printf(SUB_2 "flush: re-entrant command did not survive\n");
		return false;
	}
	return true;
}

/* Flushing from inside a completion must not re-enter the drain loop. */
static void flush_from_completion_cb(void *arg, int pd, struct osdp_cmd *cmd,
				     enum osdp_completion_status status)
{
	struct completion_ctx *c = arg;

	ARG_UNUSED(pd);
	test_completion_record(&c->comp, cmd->id, (int)status);
	if (atomic_exchange(&c->resubmit_armed, false)) {
		atomic_store(&c->resubmit_rc,
			     osdp_cp_flush_commands(c->cp, 0));
	}
	test_cmd_free(cmd);
}

static bool test_flush_from_completion(void)
{
	struct osdp_cmd a = make_led_cmd();
	struct osdp_cmd b = make_led_cmd();

	test_completion_reset(&g_comp.comp);
	atomic_store(&g_comp.resubmit_rc, -1);
	atomic_store(&g_comp.resubmit_armed, true);
	use_cp_completion(flush_from_completion_cb, &g_comp);

	if (!test_submit_command(g_comp.cp, 0, &a) ||
	    !test_submit_command(g_comp.cp, 0, &b)) {
		printf(SUB_2 "nested flush: submit rejected\n");
		return false;
	}
	/* The outer flush detaches both; the inner one, fired from the first
	 * completion, sees an already-empty live queue and returns 0. */
	if (osdp_cp_flush_commands(g_comp.cp, 0) != 2) {
		printf(SUB_2 "nested flush: outer flush miscounted\n");
		return false;
	}
	if (atomic_load(&g_comp.resubmit_rc) != 0) {
		printf(SUB_2 "nested flush: inner returned %d, want 0\n",
		       atomic_load(&g_comp.resubmit_rc));
		return false;
	}
	if (test_completion_count(&g_comp.comp) != 2) {
		printf(SUB_2 "nested flush: %d completions, want 2\n",
		       test_completion_count(&g_comp.comp));
		return false;
	}
	/* Restore the suite's default callback for the cases that follow. */
	use_cp_completion(resubmit_completion_cb, &g_comp);
	return true;
}

static struct test_completion g_pd_comp;

static void pd_resubmit_completion_cb(void *arg, struct osdp_event *ev,
				      enum osdp_completion_status status)
{
	struct completion_ctx *c = arg;

	test_completion_record(&g_pd_comp, ev->type, (int)status);
	if (atomic_exchange(&c->resubmit_armed, false)) {
		struct osdp_event next;
		bool queued;

		memset(&next, 0, sizeof(next));
		next.type = OSDP_EVENT_CARDREAD;
		next.cardread.reader_no = 0;
		next.cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
		next.cardread.length = 16;
		queued = test_submit_event(c->pd, &next) != NULL;
		atomic_store(&c->resubmit_rc, queued ? 1 : 0);
	}
	test_event_free(ev);
}

/* An event submitted from inside a FLUSHED completion survives the flush. */
static bool test_pd_submit_from_flush_completion(void)
{
	struct osdp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_CARDREAD;
	ev.cardread.reader_no = 0;
	ev.cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
	ev.cardread.length = 16;

	test_completion_reset(&g_pd_comp);
	atomic_store(&g_comp.resubmit_rc, -1);
	atomic_store(&g_comp.resubmit_armed, true);
	use_pd_completion(pd_resubmit_completion_cb, &g_comp);

	if (!test_submit_event(g_comp.pd, &ev)) {
		printf(SUB_2 "pd flush: submit rejected\n");
		return false;
	}
	osdp_pd_flush_events(g_comp.pd);

	if (test_completion_status(&g_pd_comp) != OSDP_COMPLETION_FLUSHED) {
		printf(SUB_2 "pd flush: status %d, want FLUSHED\n",
		       test_completion_status(&g_pd_comp));
		return false;
	}
	if (atomic_load(&g_comp.resubmit_rc) != 1) {
		printf(SUB_2 "pd flush: re-entrant submit failed\n");
		return false;
	}
	if (osdp_pd_flush_events(g_comp.pd) != 1) {
		printf(SUB_2 "pd flush: re-entrant event did not survive\n");
		return false;
	}
	return true;
}

static const struct osdp_file_ops g_flush_reshape_ops;

struct flush_reshape_ctx {
	osdp_t *ctx;
	int add_pd_rc;
	int register_rc;
};

static struct flush_reshape_ctx g_flush_reshape;

/* Completion that tries to reshape the CP whose flush fired it. */
static void reshape_from_flush_cb(void *arg, int pd, struct osdp_cmd *cmd,
				  enum osdp_completion_status status)
{
	struct flush_reshape_ctx *c = arg;
	osdp_pd_info_t extra = {
		.baud_rate = 9600,
		.address = 104,
	};

	ARG_UNUSED(pd);
	ARG_UNUSED(status);
	c->add_pd_rc = osdp_cp_add_pd(c->ctx, 1, &extra);
	c->register_rc =
		osdp_file_register_ops(c->ctx, 0, &g_flush_reshape_ops);
	test_cmd_free(cmd);
}

/*
 * A flush runs completions while it walks the PD; growing the PD array or
 * swapping the file ops from one of them would pull the PD out from under it.
 * Expects g_comp.cp online with its refresh runner stopped.
 */
static bool test_reshape_from_flush_completion_is_refused(void)
{
	struct osdp_cmd cmd = make_led_cmd();
	bool result = false;

	g_flush_reshape.ctx = g_comp.cp;
	g_flush_reshape.add_pd_rc = 1;
	g_flush_reshape.register_rc = 1;
	use_cp_completion(reshape_from_flush_cb, &g_flush_reshape);
	if (!test_submit_command(g_comp.cp, 0, &cmd) ||
	    !test_submit_command(g_comp.cp, 0, &cmd)) {
		printf(SUB_2 "reshape: submit rejected\n");
		goto out;
	}
	if (osdp_cp_flush_commands(g_comp.cp, 0) != 2) {
		printf(SUB_2 "reshape: flush miscounted\n");
		goto out;
	}
	if (g_flush_reshape.add_pd_rc != -1 ||
	    g_flush_reshape.register_rc != -1) {
		printf(SUB_2 "reshape: add_pd %d, register %d, want -1\n",
		       g_flush_reshape.add_pd_rc, g_flush_reshape.register_rc);
		goto out;
	}
	if (osdp_file_register_ops(g_comp.cp, 0, &g_flush_reshape_ops) != 0) {
		printf(SUB_2 "reshape: register refused after the flush\n");
		goto out;
	}
	result = true;
out:
	use_cp_completion(resubmit_completion_cb, &g_comp);
	return result;
}

static void pd_reshape_from_flush_cb(void *arg, struct osdp_event *ev,
				     enum osdp_completion_status status)
{
	struct flush_reshape_ctx *c = arg;

	ARG_UNUSED(status);
	c->register_rc =
		osdp_file_register_ops(c->ctx, 0, &g_flush_reshape_ops);
	test_event_free(ev);
}

/* Same for a PD: its flush runs completions too. */
static bool test_pd_reshape_from_flush_completion_is_refused(void)
{
	struct osdp_event ev;
	bool result = false;

	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_CARDREAD;
	ev.cardread.reader_no = 0;
	ev.cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
	ev.cardread.length = 16;

	g_flush_reshape.ctx = g_comp.pd;
	g_flush_reshape.register_rc = 1;
	use_pd_completion(pd_reshape_from_flush_cb, &g_flush_reshape);
	if (!test_submit_event(g_comp.pd, &ev)) {
		printf(SUB_2 "pd reshape: submit rejected\n");
		goto out;
	}
	osdp_pd_flush_events(g_comp.pd);
	if (g_flush_reshape.register_rc != -1) {
		printf(SUB_2 "pd reshape: register %d, want -1\n",
		       g_flush_reshape.register_rc);
		goto out;
	}
	if (osdp_file_register_ops(g_comp.pd, 0, &g_flush_reshape_ops) != 0) {
		printf(SUB_2 "pd reshape: register refused after the flush\n");
		goto out;
	}
	result = true;
out:
	use_pd_completion(pd_resubmit_completion_cb, &g_comp);
	return result;
}

/* Completion that calls back into libosdp; records what it got back. */
static void reenter_on_abort_cb(void *arg, int pd, struct osdp_cmd *cmd,
				enum osdp_completion_status status)
{
	struct completion_ctx *c = arg;
	osdp_pd_info_t extra = {
		.baud_rate = 9600,
		.address = 106,
	};

	ARG_UNUSED(pd);
	test_completion_record(&c->comp, cmd->id, (int)status);
	if (status == OSDP_COMPLETION_ABORTED &&
	    atomic_exchange(&c->resubmit_armed, false)) {
		struct osdp_cmd next = make_led_cmd();

		atomic_store(&c->resubmit_rc,
			     osdp_cp_submit_command(c->cp, 0, &next));
		/*
		 * Refresh returns void, so it cannot report the refusal --
		 * exercise it anyway: unguarded it would drive the state
		 * machine over a context teardown is dismantling.
		 */
		osdp_cp_refresh(c->cp);
		c->add_pd_rc = osdp_cp_add_pd(c->cp, 1, &extra);
		c->register_rc =
			osdp_file_register_ops(c->cp, 0, &g_flush_reshape_ops);
		/* Teardown is already running; this one must be a no-op. */
		osdp_cp_teardown(c->cp);
	}
	test_cmd_free(cmd);
}

/* Isolated from g_comp: this case tears its own devices down, and must not
 * disturb the suite-owned pair that later cases still rely on. */
static struct completion_ctx g_teardown_comp;

/*
 * An osdp_* call from inside an ABORTED completion must fail cleanly rather
 * than enqueue into, or reshape, a context that is being torn down.
 */
static bool test_submit_during_teardown_is_refused(struct test *t)
{
	osdp_t *cp = NULL, *pd = NULL;
	struct osdp_cmd cmd = make_led_cmd();
	int cp_runner, pd_runner;

	if (test_setup_devices(t, &cp, &pd)) {
		printf(SUB_2 "teardown: device setup failed\n");
		return false;
	}
	g_teardown_comp.cp = cp;
	test_completion_reset(&g_teardown_comp.comp);
	atomic_store(&g_teardown_comp.resubmit_rc, 0);
	atomic_store(&g_teardown_comp.resubmit_armed, true);
	g_teardown_comp.add_pd_rc = 1;
	g_teardown_comp.register_rc = 1;
	osdp_cp_set_command_completion_callback(cp, reenter_on_abort_cb,
						&g_teardown_comp);

	/* cp_submit_command() refuses while the PD is offline, so this pair
	 * needs its own brief run before it can be torn down. */
	cp_runner = async_runner_start(cp, osdp_cp_refresh);
	pd_runner = async_runner_start(pd, osdp_pd_refresh);
	if (cp_runner < 0 || pd_runner < 0 ||
	    !test_wait_for_online(cp, 0, 10)) {
		printf(SUB_2 "teardown: PD failed to come online\n");
		async_runner_stop(cp_runner);
		async_runner_stop(pd_runner);
		osdp_cp_teardown(cp);
		osdp_pd_teardown(pd);
		return false;
	}

	if (!test_submit_command(cp, 0, &cmd)) {
		printf(SUB_2 "teardown: submit rejected\n");
		async_runner_stop(cp_runner);
		async_runner_stop(pd_runner);
		osdp_cp_teardown(cp);
		osdp_pd_teardown(pd);
		return false;
	}
	async_runner_stop(cp_runner);
	async_runner_stop(pd_runner);
	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd);

	if (test_completion_status(&g_teardown_comp.comp) !=
	    OSDP_COMPLETION_ABORTED) {
		printf(SUB_2 "teardown: status %d, want ABORTED\n",
		       test_completion_status(&g_teardown_comp.comp));
		return false;
	}
	if (atomic_load(&g_teardown_comp.resubmit_rc) != -1) {
		printf(SUB_2 "teardown: submit returned %d, want -1\n",
		       atomic_load(&g_teardown_comp.resubmit_rc));
		return false;
	}
	if (g_teardown_comp.add_pd_rc != -1 ||
	    g_teardown_comp.register_rc != -1) {
		printf(SUB_2 "teardown: add_pd %d, register %d, want -1\n",
		       g_teardown_comp.add_pd_rc, g_teardown_comp.register_rc);
		return false;
	}
	return true;
}

#ifndef OPT_OSDP_RX_ZERO_COPY
struct nested_ctx {
	osdp_t *cp;
	int depth;
	int max_depth;
	int register_rc;
	int add_pd_rc;
};

static struct nested_ctx g_nested;

static const struct osdp_file_ops g_nested_ops;

/* Channel that, the first time the CP reads it, calls back into the CP. */
static int nested_recv(void *data, uint8_t *buf, int maxlen)
{
	struct nested_ctx *c = data;
	osdp_pd_info_t extra = {
		.baud_rate = 9600,
		.address = 102,
	};

	ARG_UNUSED(buf);
	ARG_UNUSED(maxlen);
	c->depth++;
	if (c->depth > c->max_depth) {
		c->max_depth = c->depth;
	}
	if (c->depth == 1 && c->register_rc == 1) {
		osdp_cp_refresh(c->cp);
		c->register_rc =
			osdp_file_register_ops(c->cp, 0, &g_nested_ops);
		c->add_pd_rc = osdp_cp_add_pd(c->cp, 1, &extra);
	}
	c->depth--;
	return 0;
}

static int nested_send(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	return len;
}

/*
 * From inside a callback, a refresh must not recurse and the calls that would
 * reshape the context must fail; outside one, file ops can be registered.
 */
static bool test_nested_calls_from_a_callback_are_refused(void)
{
	struct osdp_channel channel = {
		.data = &g_nested,
		.recv = nested_recv,
		.send = nested_send,
	};
	osdp_pd_info_t info = {
		.baud_rate = 9600,
		.address = 101,
	};
	bool result = false;
	int i;

	memset(&g_nested, 0, sizeof(g_nested));
	g_nested.register_rc = 1;
	g_nested.cp = osdp_cp_setup(&channel, 1, &info);
	if (g_nested.cp == NULL) {
		printf(SUB_2 "nested: setup failed\n");
		return false;
	}
	for (i = 0; i < 1000 && g_nested.register_rc == 1; i++) {
		osdp_cp_refresh(g_nested.cp);
		usleep(1000);
	}
	if (g_nested.max_depth != 1) {
		printf(SUB_2 "nested: refresh recursed to depth %d\n",
		       g_nested.max_depth);
		goto out;
	}
	if (g_nested.register_rc != -1 || g_nested.add_pd_rc != -1) {
		printf(SUB_2 "nested: register %d, add_pd %d, want -1\n",
		       g_nested.register_rc, g_nested.add_pd_rc);
		goto out;
	}
	if (osdp_file_register_ops(g_nested.cp, 0, &g_nested_ops) != 0) {
		printf(SUB_2 "nested: refused outside a callback too\n");
		goto out;
	}
	result = true;
out:
	osdp_cp_teardown(g_nested.cp);
	return result;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

static int noop_event_cb(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(pd);
	ARG_UNUSED(ev);
	return 0;
}

static int noop_command_cb(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(cmd);
	return 0;
}

static int set_all_callbacks(osdp_t *cp, osdp_t *pd)
{
	int rc = 0;

	rc |= osdp_cp_set_event_callback(cp, noop_event_cb, NULL);
	rc |= osdp_cp_set_command_completion_callback(cp, test_cmd_completion_cb,
						      NULL);
	rc |= osdp_pd_set_command_callback(pd, noop_command_cb, NULL);
	rc |= osdp_pd_set_event_completion_callback(pd, test_event_completion_cb,
						    NULL);
	return rc;
}

/*
 * Swapping a completion callback while commands are in flight would hand
 * their completions to an arg that never owned them, so the callbacks are
 * fixed once a context has been refreshed.
 */
static bool test_callbacks_are_set_only_before_refresh(struct test *t)
{
	osdp_t *cp, *pd;
	bool result = false;

	if (test_setup_devices(t, &cp, &pd)) {
		printf(SUB_2 "callbacks: device setup failed\n");
		return false;
	}
	if (set_all_callbacks(cp, pd) != 0) {
		printf(SUB_2 "callbacks: refused before the first refresh\n");
		goto out;
	}
	osdp_cp_refresh(cp);
	osdp_pd_refresh(pd);
	if (osdp_cp_set_event_callback(cp, noop_event_cb, NULL) != -1 ||
	    osdp_cp_set_command_completion_callback(
		    cp, test_cmd_completion_cb, NULL) != -1 ||
	    osdp_pd_set_command_callback(pd, noop_command_cb, NULL) != -1 ||
	    osdp_pd_set_event_completion_callback(
		    pd, test_event_completion_cb, NULL) != -1) {
		printf(SUB_2 "callbacks: accepted after the first refresh\n");
		goto out;
	}
	result = true;
out:
	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd);
	return result;
}

/* Every submitted object must have come back by the time teardown returns. */
static bool test_no_objects_outstanding_after_teardown(void)
{
	if (test_alloc_outstanding() != 0) {
		printf(SUB_2 "outstanding: %d object(s) never completed\n",
		       test_alloc_outstanding());
		return false;
	}
	return true;
}

void run_completion_tests(struct test *t)
{
	printf("\nBegin completion tests\n");

	if (test_setup_devices(t, &g_comp.cp, &g_comp.pd)) {
		printf(SUB_1 "completion: device setup failed\n");
		return;
	}
	osdp_cp_set_command_completion_callback(g_comp.cp,
						cp_completion_dispatch, NULL);
	osdp_pd_set_event_completion_callback(g_comp.pd,
					      pd_completion_dispatch, NULL);
	use_cp_completion(resubmit_completion_cb, &g_comp);
	use_pd_completion(test_event_completion_cb, NULL);

	g_comp.cp_runner = async_runner_start(g_comp.cp, osdp_cp_refresh);
	g_comp.pd_runner = async_runner_start(g_comp.pd, osdp_pd_refresh);
	if (g_comp.cp_runner < 0 || g_comp.pd_runner < 0) {
		printf(SUB_1 "completion: failed to create CP/PD runners\n");
		goto stop_runners;
	}
	if (!test_wait_for_online(g_comp.cp, 0, 10)) {
		printf(SUB_1 "completion: PD failed to come online\n");
		goto stop_runners;
	}

	TEST_CASE(t, "submit_from_flush_completion",
		  test_submit_from_flush_completion());
	TEST_CASE(t, "flush_from_completion", test_flush_from_completion());
	TEST_CASE(t, "pd_submit_from_flush_completion",
		  test_pd_submit_from_flush_completion());

	async_runner_stop(g_comp.cp_runner);
	async_runner_stop(g_comp.pd_runner);

	TEST_CASE(t, "reshape_from_flush_completion_is_refused",
		  test_reshape_from_flush_completion_is_refused());
	TEST_CASE(t, "pd_reshape_from_flush_completion_is_refused",
		  test_pd_reshape_from_flush_completion_is_refused());

	/*
	 * The mock channel is a single shared pair of buffers, not one per
	 * CP/PD pair, so the teardown case's own devices cannot come online
	 * while g_comp's runners are still driving it. Its pair is set up and
	 * torn down here, between g_comp's runners stopping and g_comp itself
	 * being torn down below.
	 */
	TEST_CASE(t, "submit_during_teardown_is_refused",
		  test_submit_during_teardown_is_refused(t));
	TEST_CASE(t, "callbacks_are_set_only_before_refresh",
		  test_callbacks_are_set_only_before_refresh(t));
#ifndef OPT_OSDP_RX_ZERO_COPY
	TEST_CASE(t, "nested_calls_from_a_callback_are_refused",
		  test_nested_calls_from_a_callback_are_refused());
#endif
	goto teardown;

stop_runners:
	async_runner_stop(g_comp.cp_runner);
	async_runner_stop(g_comp.pd_runner);
teardown:
	osdp_cp_teardown(g_comp.cp);
	osdp_pd_teardown(g_comp.pd);

	/*
	 * Teardown is itself a source of leaks, so this has to be checked once
	 * both contexts are gone -- and before test_alloc_reset() drops the
	 * accounting that would show them.
	 */
	TEST_CASE(t, "no_objects_outstanding_after_teardown",
		  test_no_objects_outstanding_after_teardown());
	test_alloc_reset();

	printf("End completion tests\n\n");
}
