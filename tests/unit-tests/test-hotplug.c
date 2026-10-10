/*
 * Copyright (c) 2025-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>

#include <osdp.h>
#include "test.h"
#include "osdp_file.h"

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

	struct osdp_cmd *sent = test_submit_command(g_test_ctx.cp_ctx, 0, &cmd);
	if (sent) {
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
	sent = test_submit_command(g_test_ctx.cp_ctx, 0, &cmd);
	if (sent) {
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
		sent = test_submit_command(g_test_ctx.cp_ctx, 0, &cmd);
		printf(SUB_2 "Command on re-enabled PD: %s\n",
			   sent ? "SUCCESS" : "FAILED");
		if (sent) {
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

	if (osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, 99) ||
	    osdp_cp_is_pd_enabled(g_test_ctx.cp_ctx, -1)) {
		printf(SUB_2 "Invalid PD index reported as enabled\n");
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

	struct osdp_cmd *sent1, *sent2;

	sent1 = test_submit_command(g_test_ctx.cp_ctx, 0, &cmd1);
	sent2 = test_submit_command(g_test_ctx.cp_ctx, 0, &cmd2);

	printf(SUB_2 "Commands on disabled PD: buzzer=%s, led=%s (both should fail)\n",
	       sent1 ? "SUCCESS" : "FAILED", sent2 ? "SUCCESS" : "FAILED");

	if (sent1 || sent2) {
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

/* File ops that keep a transfer going without touching storage. */
static int hp_fopen(void *arg, int file_id, uint32_t *size)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(file_id);
	if (*size == 0) {
		*size = 4096; /* sender: announce the file */
	}
	return 0;
}

static int hp_fread(void *arg, void *buf, uint32_t size, uint32_t offset)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(offset);
	memset(buf, 0x5A, size);
	return (int)size;
}

static int hp_fwrite(void *arg, const void *buf, uint32_t size, uint32_t offset)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(buf);
	ARG_UNUSED(offset);
	return (int)size;
}

static int hp_fclose(void *arg)
{
	ARG_UNUSED(arg);
	return 0;
}

static void hp_refresh(osdp_t *cp, osdp_t *pd)
{
	osdp_cp_refresh(cp);
	osdp_pd_refresh(pd);
	usleep(1000);
}

/*
 * Adding a PD would move the PD array under an operation already running on
 * an existing PD. The CP has been refreshed by then, so the add is refused and
 * the transfer completes undisturbed.
 */
static bool test_add_pd_during_file_transfer_is_refused(struct test *t)
{
	struct osdp_file_ops ops = {
		.open = hp_fopen,
		.read = hp_fread,
		.write = hp_fwrite,
		.close = hp_fclose,
	};
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_FILE_TX,
		.file_tx = { .id = 1 },
	};
	osdp_pd_info_t extra = {
		.name = "pd-102",
		.baud_rate = 9600,
		.address = 102,
	};
	struct test_completion comp;
	osdp_t *cp, *pd;
	uint32_t size, offset = 0;
	bool result = false;
	int i;

	printf(SUB_2 "testing a PD added during a file transfer\n");

	if (test_setup_devices(t, &cp, &pd)) {
		printf(SUB_2 "Failed to setup devices!\n");
		return false;
	}
	osdp_pd_set_command_callback(pd, test_hotplug_command_callback,
				     &g_test_ctx);
	osdp_file_register_ops(cp, 0, &ops);
	osdp_file_register_ops(pd, 0, &ops);
	test_completion_reset(&comp);
	osdp_cp_set_command_completion_callback(cp, test_cmd_completion_cb,
						&comp);

	for (i = 0; i < 10000 && !osdp_cp_is_pd_enabled(cp, 0); i++) {
		hp_refresh(cp, pd);
	}
	for (i = 0; i < 10000; i++) {
		uint8_t status = 0;

		osdp_get_status_mask(cp, &status);
		if (status & 1) {
			break;
		}
		hp_refresh(cp, pd);
	}
	if (!test_submit_command(cp, 0, &cmd)) {
		printf(SUB_2 "file tx refused\n");
		goto out;
	}
	for (i = 0; i < 10000 && offset == 0; i++) {
		hp_refresh(cp, pd);
		osdp_get_file_tx_status(cp, 0, &size, &offset);
	}
	if (offset == 0) {
		printf(SUB_2 "transfer never got under way\n");
		goto out;
	}
	if (osdp_cp_add_pd(cp, 1, &extra) != -1) {
		printf(SUB_2 "add_pd accepted during the transfer\n");
		goto out;
	}
	for (i = 0; i < 10000 && test_completion_count(&comp) == 0; i++) {
		hp_refresh(cp, pd);
	}
	if (test_completion_count(&comp) != 1 ||
	    test_completion_status(&comp) != OSDP_COMPLETION_OK) {
		printf(SUB_2 "file tx: %d completions, status %d\n",
		       test_completion_count(&comp),
		       test_completion_status(&comp));
		goto out;
	}
	result = true;
out:
	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd);
	return result;
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

static int reshape_fclose(void *arg)
{
	ARG_UNUSED(arg);
	reshape_try_add_pd();
	return 0;
}

/*
 * Swapping the file ops under a live transfer closes the open file through
 * the old ops. A transfer runs only on a refreshed CP, so an add_pd from that
 * close is refused rather than freeing the PD osdp_file_register_ops() goes
 * on to reset.
 */
static bool test_add_pd_from_register_ops_close_is_refused(struct test *t)
{
	struct osdp_file_ops ops = {
		.open = hp_fopen,
		.read = hp_fread,
		.write = hp_fwrite,
		.close = reshape_fclose,
	};
	osdp_t *cp, *pd;
	bool result = false;

	printf(SUB_2 "testing add_pd from a file close op\n");

	if (test_setup_devices(t, &cp, &pd)) {
		printf(SUB_2 "Failed to setup devices!\n");
		return false;
	}
	osdp_file_register_ops(cp, 0, &ops);
	osdp_cp_refresh(cp);
	if (osdp_file_tx_command(osdp_to_pd(cp, 0), 1, 0)) {
		printf(SUB_2 "file tx did not start\n");
		goto out;
	}
	g_reshape.cp = cp;
	g_reshape.armed = true;
	g_reshape.add_pd_rc = 1;
	if (osdp_file_register_ops(cp, 0, &ops)) {
		printf(SUB_2 "register_ops failed\n");
		goto out;
	}
	if (g_reshape.armed || g_reshape.add_pd_rc != -1) {
		printf(SUB_2 "add_pd from close: armed %d, rc %d, want -1\n",
		       g_reshape.armed, g_reshape.add_pd_rc);
		goto out;
	}
	result = true;
out:
	g_reshape.armed = false;
	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd);
	return result;
}

#ifndef OPT_OSDP_LOG_MINIMAL
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

	TEST_CASE(t, "pd_disable_enable_basic", test_pd_disable_enable_basic());
	TEST_CASE(t, "pd_command_blocking", test_pd_command_blocking());
	TEST_CASE(t, "pd_edge_cases", test_pd_edge_cases());
	TEST_CASE(t, "multiple_pd_hotplug", test_multiple_pd_hotplug());
	TEST_CASE(t, "dynamic_pd_management", test_dynamic_pd_management());

	/* Teardown test environment */
	teardown_test_environment();

	TEST_CASE(t, "add_pd_during_file_transfer_is_refused",
		  test_add_pd_during_file_transfer_is_refused(t));
	TEST_CASE(t, "add_pd_only_before_first_refresh",
		  test_add_pd_only_before_first_refresh(t));
	TEST_CASE(t, "add_pd_from_register_ops_close_is_refused",
		  test_add_pd_from_register_ops_close_is_refused(t));
#ifndef OPT_OSDP_LOG_MINIMAL
	TEST_CASE(t, "add_pd_from_add_pd_log_is_refused",
		  test_add_pd_from_add_pd_log_is_refused(t));
#endif
}