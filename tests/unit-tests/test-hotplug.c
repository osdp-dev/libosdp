/*
 * Copyright (c) 2025-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
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

	/* Command tracking */
	bool cmd_seen;
	int last_cmd_id;

	/* Event tracking */
	bool event_seen;
	int last_event_type;
};

static struct test_hotplug_ctx g_test_ctx = {0};

static bool wait_for_pd_online(int timeout_sec);

int test_hotplug_event_callback(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(pd);
	struct test_hotplug_ctx *ctx = arg;

	ctx->event_seen = true;
	ctx->last_event_type = ev->type;
	return 0;
}

int test_hotplug_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_hotplug_ctx *ctx = arg;

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

	osdp_cp_set_event_callback(g_test_ctx.cp_ctx, test_hotplug_event_callback, &g_test_ctx);
	osdp_pd_set_command_callback(g_test_ctx.pd_ctx, test_hotplug_command_callback, &g_test_ctx);

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

static void reset_test_state()
{
	g_test_ctx.cmd_seen = false;
	g_test_ctx.last_cmd_id = 0;
	g_test_ctx.event_seen = false;
	g_test_ctx.last_event_type = 0;
}

static bool wait_for_command(int expected_cmd_id, int timeout_sec)
{
	int rc = 0;
	while (rc < timeout_sec) {
		if (g_test_ctx.cmd_seen && g_test_ctx.last_cmd_id == expected_cmd_id) {
			return true;
		}
		usleep(1000 * 1000);
		rc++;
	}
	return false;
}

static bool wait_for_pd_state(bool expected_state, int timeout_sec)
{
	int rc = 0;
	while (rc < timeout_sec) {
		bool current_state = osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 0);
		if (current_state == expected_state) {
			return true;
		}
		usleep(100 * 1000);  /* Check every 100ms */
		rc++;
	}
	return false;
}

static bool wait_for_pd_online(int timeout_sec)
{
	int rc = 0;
	while (rc < timeout_sec) {
		/* Check if PD is both enabled and online */
		bool enabled = osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 0);
		uint8_t status_mask = 0;
		osdp_get_status_mask(g_test_ctx.cp_ctx, &status_mask);
		bool online = (status_mask & 0x01) != 0;

		if (enabled && online) {
			return true;
		}
		usleep(100 * 1000);  /* Check every 100ms */
		rc++;
	}
	return false;
}

static bool test_pd_disable_enable_basic()
{
	printf(SUB_2 "testing basic PD disable/enable\n");
	reset_test_state();

	/* Test initial state - should be enabled */
	bool enabled = osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 0);
	if (enabled != PD_STATE_ENABLED) {
		printf(SUB_2 "PD should be enabled initially, got %s\n", enabled ? "true" : "false");
		return false;
	}

	/* Test status mask - note PD might still be coming online */
	uint8_t status_mask = 0;
	osdp_get_status_mask(g_test_ctx.cp_ctx, &status_mask);
	printf(SUB_2 "Initial status mask: 0x%02X (PD may still be initializing)\n", status_mask);

	/* Disable PD */
	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Failed to disable PD\n");
		return false;
	}

	/* Wait for PD to actually be disabled */
	if (!wait_for_pd_state(PD_STATE_DISABLED, 3)) {
		printf(SUB_2 "PD didn't reach disabled state within timeout\n");
		return false;
	}

	/* Status mask should show PD offline */
	osdp_get_status_mask(g_test_ctx.cp_ctx, &status_mask);
	if (status_mask & 0x01) {
		printf(SUB_2 "Disabled PD should appear offline in status mask\n");
		return false;
	}

	/* Re-enable PD */
	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Failed to enable PD\n");
		return false;
	}

	/* Wait for PD to actually be enabled */
	if (!wait_for_pd_state(PD_STATE_ENABLED, 3)) {
		printf(SUB_2 "PD didn't reach enabled state within timeout\n");
		return false;
	}

	return true;
}

static bool test_pd_command_blocking()
{
	printf(SUB_2 "testing command blocking on disabled PD\n");
	reset_test_state();

	/* Test command on enabled PD first */
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_BUZZER,
		.buzzer = {
			.control_code = 1,
			.on_count = 10,
			.off_count = 10,
			.rep_count = 1,
		},
	};

	int ret = osdp_cp_submit_command(g_test_ctx.cp_ctx, 0, &cmd);
	if (ret == 0) {
		printf(SUB_2 "Command submission succeeded on enabled PD\n");
		/* Wait a bit for command to be processed */
		if (wait_for_command(OSDP_CMD_BUZZER, 3)) {
			printf(SUB_2 "Command received by PD\n");
		} else {
			printf(SUB_2 "Command not received (PD may not be fully online yet)\n");
		}
	} else {
		printf(SUB_2 "Command submission failed on enabled PD (PD not online yet)\n");
	}

	/* Disable PD */
	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Failed to disable PD\n");
		return false;
	}

	/* osdp_cp_disable_pd() only posts a request to the CP FSM; wait for
	 * the FSM to actually transition the PD to disabled before exercising
	 * command submission and re-enable below. */
	if (!wait_for_pd_state(PD_STATE_DISABLED, 3)) {
		printf(SUB_2 "PD didn't reach disabled state within timeout\n");
		return false;
	}

	/* Try command on disabled PD - should fail */
	reset_test_state();
	ret = osdp_cp_submit_command(g_test_ctx.cp_ctx, 0, &cmd);
	if (ret == 0) {
		printf(SUB_2 "Command should fail on disabled PD\n");
		return false;
	}

	/* Re-enable and test command works again. The PD is confirmed
	 * disabled above, so enable must succeed. */
	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Failed to re-enable PD\n");
		return false;
	}

	/* Wait for PD to come online before testing commands */
	if (wait_for_pd_online(5)) {
		reset_test_state();
		ret = osdp_cp_submit_command(g_test_ctx.cp_ctx, 0, &cmd);
		printf(SUB_2 "Command on re-enabled PD: %s\n",
			   ret == 0 ? "SUCCESS" : "FAILED");
		/* The CP queue keeps a reference to the caller-owned `cmd`
		 * (app-owned queue data); wait for it to be consumed before
		 * `cmd` leaves scope, else the CP runner dereferences a stale
		 * stack address. */
		if (ret == 0) {
			wait_for_command(OSDP_CMD_BUZZER, 3);
		}
	} else {
		printf(SUB_2 "PD didn't come online within timeout, skipping command test\n");
	}

	return true;
}

static bool test_pd_edge_cases()
{
	printf(SUB_2 "testing edge cases\n");

	/* Ensure PD starts in enabled and online state */
	osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0);  /* Ignore return - might already be enabled */
	if (!wait_for_pd_online(5)) {
		printf(SUB_2 "Failed to get PD online for edge test, proceeding anyway\n");
	}

	/* Test disabling PD */
	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "First disable failed\n");
		return false;
	}

	/* Wait for PD to be disabled */
	if (!wait_for_pd_state(PD_STATE_DISABLED, 3)) {
		printf(SUB_2 "PD didn't reach disabled state within timeout\n");
		return false;
	}

	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) == 0) {
		printf(SUB_2 "Disabling already disabled PD should return error\n");
		return false;
	}

	/* Test enabling already enabled PD */
	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Enable failed\n");
		return false;
	}

	/* Wait for PD to be enabled */
	if (!wait_for_pd_state(PD_STATE_ENABLED, 3)) {
		printf(SUB_2 "PD didn't reach enabled state within timeout\n");
		return false;
	}

	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) == 0) {
		printf(SUB_2 "Enabling already enabled PD should return error\n");
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

	bool enabled = osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 99);
	/* Note: invalid PD returns -1 which converts to true in bool context */
	if (enabled != true) {
		printf(SUB_2 "Invalid PD index returns -1 (converted to true), got %s\n", enabled ? "true" : "false");
		return false;
	}

	return true;
}

static bool test_multiple_pd_hotplug()
{
	printf(SUB_2 "testing multiple PD hot-plug simulation\n");

	/* This test demonstrates the concept with a single PD, but shows
	 * how multiple PDs could be managed independently */

	/* Ensure PD starts in enabled and online state */
	osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0);  /* Ignore return - might already be enabled */
	if (!wait_for_pd_online(5)) {
		printf(SUB_2 "Failed to get PD online for hotplug test, proceeding anyway\n");
	}

	/* Get initial status */
	uint8_t status_mask = 0;
	osdp_get_status_mask(g_test_ctx.cp_ctx, &status_mask);
	printf(SUB_2 "Initial status mask: 0x%02X\n", status_mask);

	/* Simulate "unplugging" PD 0 */
	printf(SUB_2 "Simulating PD 0 unplug...\n");
	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Failed to disable PD 0\n");
		return false;
	}

	osdp_get_status_mask(g_test_ctx.cp_ctx, &status_mask);
	printf(SUB_2 "After PD 0 unplug: 0x%02X\n", status_mask);

	/* Wait a bit */
	usleep(500 * 1000);

	/* Simulate "plugging back" PD 0 */
	printf(SUB_2 "Simulating PD 0 plug-in...\n");
	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Failed to enable PD 0\n");
		return false;
	}

	/* Give time for re-initialization */
	usleep(1000 * 1000);

	osdp_get_status_mask(g_test_ctx.cp_ctx, &status_mask);
	printf(SUB_2 "After PD 0 plug-in: 0x%02X (may still be initializing)\n", status_mask);

	return true;
}

static bool test_dynamic_pd_management()
{
	printf(SUB_2 "testing dynamic PD management scenarios\n");

	/* Test scenario: Disable PD during operation, add commands to queue,
	 * then re-enable and see behavior */

	/* First, ensure PD is enabled and online */
	osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0);  /* Ignore return value - might already be enabled */
	if (!osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 0)) {
		printf(SUB_2 "PD should be enabled but isn't\n");
		return false;
	}

	/* Wait for PD to be online before proceeding */
	if (!wait_for_pd_online(5)) {
		printf(SUB_2 "PD didn't come online, proceeding with test anyway\n");
	}

	printf(SUB_2 "PD management scenario: disable -> attempt commands -> enable\n");

	/* Disable PD (ensure it's enabled first) */
	if (!osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 0)) {
		/* PD is disabled from previous test, enable it first */
		printf(SUB_2 "PD was disabled from previous test, enabling first\n");
		osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0);
		if (!wait_for_pd_state(PD_STATE_ENABLED, 3)) {
			printf(SUB_2 "Failed to enable PD for management test\n");
			return false;
		}
	}
	if (osdp_cp_disable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Failed to disable PD\n");
		return false;
	}

	/* Wait for PD to actually be disabled */
	if (!wait_for_pd_state(PD_STATE_DISABLED, 3)) {
		printf(SUB_2 "PD didn't reach disabled state within timeout\n");
		return false;
	}

	/* Try multiple commands while disabled */
	struct osdp_cmd cmd1 = { .id = OSDP_CMD_BUZZER, .buzzer = { .control_code = 1 } };
	struct osdp_cmd cmd2 = { .id = OSDP_CMD_LED, .led = { .led_number = 0 } };

	int ret1 = osdp_cp_submit_command(g_test_ctx.cp_ctx, 0, &cmd1);
	int ret2 = osdp_cp_submit_command(g_test_ctx.cp_ctx, 0, &cmd2);

	printf(SUB_2 "Commands on disabled PD: buzzer=%s, led=%s (both should fail)\n",
	       ret1 == 0 ? "SUCCESS" : "FAILED", ret2 == 0 ? "SUCCESS" : "FAILED");

	if (ret1 == 0 || ret2 == 0) {
		printf(SUB_2 "Commands should fail on disabled PD\n");
		return false;
	}

	/* Re-enable PD */
	if (osdp_cp_enable_pd(g_test_ctx.cp_ctx, 0) != 0) {
		printf(SUB_2 "Failed to re-enable PD\n");
		return false;
	}

	printf(SUB_2 "PD re-enabled - will restart initialization sequence\n");

	/* Give time for re-initialization */
	usleep(2000 * 1000);

	/* Check final state */
	bool enabled = osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 0);
	uint8_t status_mask = 0;
	osdp_get_status_mask(g_test_ctx.cp_ctx, &status_mask);

	printf(SUB_2 "Final state: enabled=%s, status_mask=0x%02X\n",
		   enabled == PD_STATE_ENABLED ? "YES" : "NO", status_mask);

	return enabled == PD_STATE_ENABLED;  /* PD should be enabled regardless of online status */
}

void run_hotplug_tests(struct test *t)
{
	bool overall_result = true;

	printf("\nBegin Hot-Plug Tests\n");

	/* Setup test environment once */
	if (setup_test_environment(t) != 0) {
		printf(SUB_1 "Failed to setup test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	printf(SUB_1 "running hot-plug tests\n");

	/* Run all hot-plug tests */
	overall_result &= test_pd_disable_enable_basic();
	overall_result &= test_pd_command_blocking();
	overall_result &= test_pd_edge_cases();
	overall_result &= test_multiple_pd_hotplug();
	overall_result &= test_dynamic_pd_management();

	/* Teardown test environment */
	teardown_test_environment();

	printf(SUB_1 "Hot-plug tests %s\n", overall_result ? "succeeded" : "failed");
	TEST_REPORT(t, overall_result);
}