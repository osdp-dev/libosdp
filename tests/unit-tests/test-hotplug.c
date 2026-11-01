/*
 * Copyright (c) 2025-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>

#include <osdp.h>
#include "test.h"

/* PD enabled/disabled states */
#define PD_STATE_DISABLED  false
#define PD_STATE_ENABLED   true

/* Test context for hot-plug tests */
struct test_hotplug_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;
};

static struct test_hotplug_ctx g_test_ctx = {0};

static bool wait_for_pd_online(int timeout_sec);

static int setup_test_environment(struct test *t)
{
	printf(SUB_1 "setting up OSDP devices\n");

	if (test_setup_devices(t, &g_test_ctx.cp_ctx, &g_test_ctx.pd_ctx)) {
		printf(SUB_1 "Failed to setup devices!\n");
		return -1;
	}

	printf(SUB_1 "starting async runners\n");

	g_test_ctx.cp_runner = async_runner_start(g_test_ctx.cp_ctx, osdp_cp_refresh);
	g_test_ctx.pd_runner = async_runner_start(g_test_ctx.pd_ctx, osdp_pd_refresh);

	if (g_test_ctx.cp_runner < 0 || g_test_ctx.pd_runner < 0) {
		printf(SUB_1 "Failed to created CP/PD runners\n");
		if (g_test_ctx.cp_runner >= 0)
			async_runner_stop(g_test_ctx.cp_runner);
		if (g_test_ctx.pd_runner >= 0)
			async_runner_stop(g_test_ctx.pd_runner);
		osdp_cp_teardown(g_test_ctx.cp_ctx);
		osdp_pd_teardown(g_test_ctx.pd_ctx);
		memset(&g_test_ctx, 0, sizeof(g_test_ctx));
		return -1;
	}

	if (!wait_for_pd_online(10)) {
		printf(SUB_1 "PD failed to come online\n");
		async_runner_stop(g_test_ctx.cp_runner);
		async_runner_stop(g_test_ctx.pd_runner);
		osdp_cp_teardown(g_test_ctx.cp_ctx);
		osdp_pd_teardown(g_test_ctx.pd_ctx);
		memset(&g_test_ctx, 0, sizeof(g_test_ctx));
		return -1;
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

	memset(&g_test_ctx, 0, sizeof(g_test_ctx));
}

static bool pd_is_online(void)
{
	uint8_t status_mask = 0;

	osdp_get_status_mask(g_test_ctx.cp_ctx, &status_mask);
	return (status_mask & 0x01) != 0;
}

static bool pd_enabled_is(void *arg)
{
	return osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 0) == *(bool *)arg;
}

static bool wait_for_pd_state(bool expected_state, int timeout_sec)
{
	return test_wait_until(pd_enabled_is, &expected_state,
			       timeout_sec * 1000);
}

static bool pd_is_enabled_and_online(void *arg)
{
	ARG_UNUSED(arg);
	return osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 0) && pd_is_online();
}

static bool wait_for_pd_online(int timeout_sec)
{
	return test_wait_until(pd_is_enabled_and_online, NULL,
			       timeout_sec * 1000);
}

/* A disabled PD takes no commands; once re-enabled it is back online. */
static bool test_pd_command_blocking()
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

	printf(SUB_2 "testing command blocking on disabled PD\n");

	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) != 0 ||
	    !wait_for_pd_state(PD_STATE_DISABLED, 3)) {
		printf(SUB_2 "PD did not disable\n");
		return false;
	}
	if (test_submit_command(g_test_ctx.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Command should fail on disabled PD\n");
		return false;
	}
	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) != 0 ||
	    !wait_for_pd_online(5)) {
		printf(SUB_2 "PD did not come back online\n");
		return false;
	}
	return true;
}

/*
 * Disable and enable each take effect once the CP's state machine gets to
 * them; a disabled PD reads as offline, and a repeat of either call, or one
 * on a PD index that does not exist, is refused.
 */
static bool test_pd_disable_enable()
{
	printf(SUB_2 "testing PD disable/enable\n");

	if (!wait_for_pd_online(5)) {
		printf(SUB_2 "PD not online at the start\n");
		return false;
	}

	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "First disable failed\n");
		return false;
	}
	if (!wait_for_pd_state(PD_STATE_DISABLED, 3)) {
		printf(SUB_2 "PD didn't reach disabled state within timeout\n");
		return false;
	}
	if (pd_is_online()) {
		printf(SUB_2 "Disabled PD is still online in status mask\n");
		return false;
	}
	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) == 0) {
		printf(SUB_2 "Disabling already disabled PD should return error\n");
		return false;
	}

	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Enable failed\n");
		return false;
	}
	if (!wait_for_pd_state(PD_STATE_ENABLED, 3)) {
		printf(SUB_2 "PD didn't reach enabled state within timeout\n");
		return false;
	}
	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) == 0) {
		printf(SUB_2 "Enabling already enabled PD should return error\n");
		return false;
	}
	if (!wait_for_pd_online(5)) {
		printf(SUB_2 "re-enabled PD did not come back online\n");
		return false;
	}

	/* Test invalid PD index */
	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 99) == 0) {
		printf(SUB_2 "Invalid PD index should fail\n");
		return false;
	}

	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 99) == 0) {
		printf(SUB_2 "Invalid PD index should fail\n");
		return false;
	}

	if (osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 99) ||
	    osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, -1)) {
		printf(SUB_2 "Invalid PD index reported as enabled\n");
		return false;
	}

	return true;
}

/*
 * PDs can be added between setup and the first refresh -- a CP set up with
 * none and filled in afterwards -- but not once the CP has been refreshed.
 */
static bool test_add_pd_only_before_first_refresh(struct test *t)
{
	osdp_pd_info_t first = {
		.baud_rate = 9600,
		.address = 101,
	};
	osdp_pd_info_t second = {
		.baud_rate = 9600,
		.address = 102,
	};
	osdp_t *cp, *harness_cp, *harness_pd;
	bool result = false;

	printf(SUB_2 "testing add_pd before and after the first refresh\n");

	if (test_setup_devices(t, &harness_cp, &harness_pd)) {
		printf(SUB_2 "Failed to setup devices!\n");
		return false;
	}
	/* Refreshed once only, with nothing on the other end; it can borrow
	 * the harness channel. */
	cp = osdp_cp_setup(&TO_OSDP(harness_cp)->channel, 0, NULL);
	if (cp == NULL) {
		printf(SUB_2 "cp setup failed\n");
		goto out_harness;
	}
	if (osdp_cp_add_pd(cp, 1, &first) != 0) {
		printf(SUB_2 "add_pd refused before the first refresh\n");
		goto out;
	}
	osdp_cp_refresh(cp);
	if (osdp_cp_add_pd(cp, 1, &second) != -1) {
		printf(SUB_2 "add_pd accepted after the first refresh\n");
		goto out;
	}
	if (TO_OSDP(cp)->_num_pd != 1) {
		printf(SUB_2 "%d PDs, want 1\n", TO_OSDP(cp)->_num_pd);
		goto out;
	}
	result = true;
out:
	osdp_cp_teardown(cp);
out_harness:
	osdp_cp_teardown(harness_cp);
	osdp_pd_teardown(harness_pd);
	return result;
}

#ifndef OPT_OSDP_LOG_MINIMAL
struct reshape_reenter {
	osdp_t *cp;
	bool armed;
	int add_pd_rc;
};

static struct reshape_reenter g_reshape;

static void reshape_try_add_pd(void)
{
	osdp_pd_info_t extra = {
		.baud_rate = 9600,
		.address = 103,
	};

	if (!g_reshape.armed) {
		return;
	}
	g_reshape.armed = false;
	g_reshape.add_pd_rc = osdp_cp_add_pd(g_reshape.cp, 1, &extra);
}

static void reshape_log_cb(int pd, int log_level, const char *msg,
			   const char *file, unsigned long line)
{
	ARG_UNUSED(pd);
	ARG_UNUSED(log_level);
	ARG_UNUSED(msg);
	ARG_UNUSED(file);
	ARG_UNUSED(line);
	reshape_try_add_pd();
}

/*
 * Adding a PD without an SCBK logs a warning while the new PD array is
 * still being built; growing it again from the log callback would free
 * the array the outer call is filling in.
 */
static bool test_add_pd_from_add_pd_log_is_refused(struct test *t)
{
	uint8_t scbk[16] = { 0 };
	osdp_pd_info_t first = {
		.baud_rate = 9600,
		.address = 101,
		.scbk = scbk,
	};
	osdp_pd_info_t plain = {
		.baud_rate = 9600,
		.address = 102,
	};
	osdp_t *cp, *harness_cp, *harness_pd;
	bool result = false;

	printf(SUB_2 "testing add_pd from the log callback\n");

	if (test_setup_devices(t, &harness_cp, &harness_pd)) {
		printf(SUB_2 "Failed to setup devices!\n");
		return false;
	}
	/* The log callback is picked up at setup. This CP is never refreshed,
	 * so it can borrow the harness channel. */
	osdp_set_log_callback(reshape_log_cb);
	cp = osdp_cp_setup(&TO_OSDP(harness_cp)->channel, 1, &first);
	osdp_logger_init("osdp", t->loglevel, NULL);
	if (cp == NULL) {
		printf(SUB_2 "cp setup failed\n");
		goto out_harness;
	}
	g_reshape.cp = cp;
	g_reshape.armed = true;
	g_reshape.add_pd_rc = 1;
	if (osdp_cp_add_pd(cp, 1, &plain)) {
		printf(SUB_2 "add_pd failed\n");
		goto out;
	}
	if (g_reshape.armed || g_reshape.add_pd_rc != -1) {
		printf(SUB_2 "add_pd from log: armed %d, rc %d, want -1\n",
		       g_reshape.armed, g_reshape.add_pd_rc);
		goto out;
	}
	result = true;
out:
	g_reshape.armed = false;
	osdp_cp_teardown(cp);
out_harness:
	osdp_cp_teardown(harness_cp);
	osdp_pd_teardown(harness_pd);
	return result;
}
#endif /* OPT_OSDP_LOG_MINIMAL */

void run_hotplug_tests(struct test *t)
{
	printf("\nBegin Hot-Plug Tests\n");

	/* Setup test environment once */
	if (setup_test_environment(t) != 0) {
		printf(SUB_1 "Failed to setup test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	printf(SUB_1 "running hot-plug tests\n");

	TEST_CASE(t, "pd_disable_enable", test_pd_disable_enable());
	TEST_CASE(t, "pd_command_blocking", test_pd_command_blocking());

	/* Teardown test environment */
	teardown_test_environment();

	TEST_CASE(t, "add_pd_only_before_first_refresh",
		  test_add_pd_only_before_first_refresh(t));
#ifndef OPT_OSDP_LOG_MINIMAL
	TEST_CASE(t, "add_pd_from_add_pd_log_is_refused",
		  test_add_pd_from_add_pd_log_is_refused(t));
#endif
}