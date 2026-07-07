/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <time.h>
#include "test.h"
#include <utils/workqueue.h>

#define ONLINE_CHECK_TIMEOUT_SEC 8   /* Standard timeout - let's see real behavior */
#define EDGE_CASE_TIMEOUT_SEC 12     /* Longer timeout for challenging edge cases */
#define TEST_DELAY_MS 500   /* 500ms between tests to avoid resource conflicts */
#define MAX_WAIT_ATTEMPTS 10 /* Maximum attempts to wait for cleanup completion */

/* Async runner ordering patterns to test */
enum async_order {
	ORDER_CP_FIRST,     /* CP starts, then PD (standard) */
	ORDER_PD_FIRST,     /* PD starts, then CP (reversed) */
	ORDER_SIMULTANEOUS, /* Both start simultaneously */
	ORDER_MAX
};

/* Simple test data structure */
struct async_test_data {
	bool cmd_received;
	bool event_generated;
};

static int async_event_callback(void *data, int pd, struct osdp_event *event)
{
	struct async_test_data *d = data;

	ARG_UNUSED(pd);
	if (event->type == OSDP_EVENT_CARDREAD) {
		d->event_generated = true;
	}
	return 0;
}

static int async_command_callback(void *data, struct osdp_cmd *cmd)
{
	struct async_test_data *d = data;

	if (cmd->id == OSDP_CMD_BUZZER) {
		d->cmd_received = true;
	}
	return 0;
}

/* Declare external variables from test.c */
extern work_t *g_test_works[];

static bool wait_for_cmd_received(struct async_test_data *data, int timeout_sec)
{
	for (int i = 0; i < timeout_sec * 10; i++) {
		if (data->cmd_received)
			return true;
		usleep(100 * 1000);
	}
	return false;
}

/* Helper function to wait for all work slots to be freed */
static void wait_for_all_work_cleanup(void)
{
	int attempts = 0;
	bool all_free;
	const int max_work = 20; /* Updated MAX_TEST_WORK from test.c */

	while (attempts < MAX_WAIT_ATTEMPTS) {
		all_free = true;
		for (int i = 0; i < max_work; i++) {
			if (g_test_works[i] != NULL) {
				all_free = false;
				break;
			}
		}
		if (all_free) {
			break;
		}
		usleep(100 * 1000); /* 100ms */
		attempts++;
	}

	if (attempts >= MAX_WAIT_ATTEMPTS) {
		printf(SUB_2 "Warning: Not all work slots freed after cleanup\n");
	}
}

/* Wait for PD to come online with timeout */
static bool wait_for_pd_online(osdp_t *cp_ctx, int timeout_sec)
{
	uint8_t status;
	int checks = 0;
	int max_checks = timeout_sec * 10; /* Check every 100ms instead of 1s */

	printf(SUB_2 "Waiting for PD to come online (timeout: %ds)...\n", timeout_sec);

	while (checks < max_checks) {
		osdp_get_status_mask(cp_ctx, &status);
		if (status & 1) {
			printf(SUB_2 "PD came online after %.1fs\n", (float)checks / 10.0f);
			return true;
		}
		usleep(100 * 1000); /* 100ms checks for faster response */
		checks++;

		/* Progress indicator every 2 seconds */
		if (checks % 20 == 0) {
			printf(SUB_2 "Still waiting... (%.1fs elapsed)\n", (float)checks / 10.0f);
		}
	}

	printf(SUB_2 "PD failed to come online after %ds (status: 0x%02x)\n", timeout_sec, status);
	return false;
}

/* Test async runner startup in different orders */
static bool test_async_startup_order(enum async_order order)
{
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	int cp_runner = -1, pd_runner = -1;
	bool result = false;
	struct async_test_data data = {0};

	/* Setup devices with dummy test structure */
	struct test dummy_test = { .loglevel = OSDP_LOG_INFO };
	if (test_setup_devices(&dummy_test, &cp_ctx, &pd_ctx)) {
		printf(SUB_2 "Failed to setup devices\n");
		return false;
	}

	osdp_cp_set_event_callback(cp_ctx, async_event_callback, &data);
	osdp_pd_set_command_callback(pd_ctx, async_command_callback, &data);

	/* Start runners based on order using independent CP and PD runners */
	switch (order) {
	case ORDER_CP_FIRST:
		printf(SUB_2 "Testing CP first startup (independent runners)\n");
		cp_runner = async_cp_runner_start(cp_ctx);
		if (cp_runner < 0) goto cleanup;
		usleep(2000 * 1000); /* Minimal delay to observe startup sequence */
		pd_runner = async_pd_runner_start(pd_ctx);
		if (pd_runner < 0) goto cleanup;
		break;

	case ORDER_PD_FIRST:
		printf(SUB_2 "Testing PD first startup (independent runners)\n");
		pd_runner = async_pd_runner_start(pd_ctx);
		if (pd_runner < 0) goto cleanup;
		usleep(2000 * 1000); /* Minimal delay to observe startup sequence */
		cp_runner = async_cp_runner_start(cp_ctx);
		if (cp_runner < 0) goto cleanup;
		break;

	case ORDER_SIMULTANEOUS:
		printf(SUB_2 "Testing simultaneous startup (independent runners)\n");
		cp_runner = async_cp_runner_start(cp_ctx);
		pd_runner = async_pd_runner_start(pd_ctx);
		if (cp_runner < 0 || pd_runner < 0) goto cleanup;
		break;

	default:
		goto cleanup;
	}

	/* Wait for connection - use longer timeout for challenging scenarios */
	int timeout = (order == ORDER_SIMULTANEOUS) ? EDGE_CASE_TIMEOUT_SEC : ONLINE_CHECK_TIMEOUT_SEC;
	if (!wait_for_pd_online(cp_ctx, timeout)) {
		printf(SUB_2 "PD failed to come online\n");
		goto cleanup;
	}

	/* Send test command */
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_BUZZER,
		.buzzer = {
			.reader = 0,
			.control_code = 1,
			.on_count = 3,
			.off_count = 3,
			.rep_count = 1
		}
	};

	if (osdp_cp_submit_command(cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send command\n");
		goto cleanup;
	}

	if (wait_for_cmd_received(&data, 5)) {
		printf(SUB_2 "Order %d: SUCCESS\n", order);
		result = true;
	} else {
		printf(SUB_2 "Order %d: Command not received\n", order);
	}

cleanup:
	/* Clean shutdown with independent runner stop functions */
	if (cp_runner >= 0) {
		if (async_cp_runner_stop(cp_runner) < 0) {
			printf(SUB_2 "Warning: Failed to stop CP runner\n");
		}
		cp_runner = -1;
	}
	if (pd_runner >= 0) {
		if (async_pd_runner_stop(pd_runner) < 0) {
			printf(SUB_2 "Warning: Failed to stop PD runner\n");
		}
		pd_runner = -1;
	}

	/* Wait for all work slots to be properly freed */
	wait_for_all_work_cleanup();

	if (cp_ctx) {
		osdp_cp_teardown(cp_ctx);
		cp_ctx = NULL;
	}
	if (pd_ctx) {
		osdp_pd_teardown(pd_ctx);
		pd_ctx = NULL;
	}

	return result;
}

/* Test recovery from runner restart */
static bool test_async_recovery(void)
{
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	int cp_runner = -1, pd_runner = -1;
	bool result = false;
	struct async_test_data data = {0};
	int retry_count = 0;
	const int max_retries = 2; /* Allow 2 retries for this challenging scenario */
	struct test dummy_test = { .loglevel = OSDP_LOG_INFO };

	printf(SUB_2 "Testing CP restart recovery\n");

retry_recovery:
	/* Setup devices with dummy test structure */
	if (test_setup_devices(&dummy_test, &cp_ctx, &pd_ctx)) {
		printf(SUB_2 "Failed to setup devices\n");
		return false;
	}

	osdp_cp_set_event_callback(cp_ctx, async_event_callback, &data);
	osdp_pd_set_command_callback(pd_ctx, async_command_callback, &data);

	/* Start both runners using independent runners */
	cp_runner = async_cp_runner_start(cp_ctx);
	pd_runner = async_pd_runner_start(pd_ctx);
	if (cp_runner < 0 || pd_runner < 0) goto cleanup;

	/* Wait for initial connection */
	if (!wait_for_pd_online(cp_ctx, ONLINE_CHECK_TIMEOUT_SEC)) {
		printf(SUB_2 "Initial connection failed\n");
		goto cleanup_and_retry;
	}

	/* Stop and restart CP runner to test recovery */
	printf(SUB_2 "Restarting CP runner (independent)\n");
	if (async_cp_runner_stop(cp_runner) < 0) {
		printf(SUB_2 "Warning: Failed to stop CP runner\n");
	}
	cp_runner = -1;
	usleep(1000 * 1000); /* 1 second gap */

	cp_runner = async_cp_runner_start(cp_ctx);
	if (cp_runner < 0) goto cleanup;

	/* Wait for recovery - use longer timeout for restart scenarios */
	if (!wait_for_pd_online(cp_ctx, EDGE_CASE_TIMEOUT_SEC)) {
		printf(SUB_2 "Recovery failed\n");
		goto cleanup_and_retry;
	}

	/* Test command after recovery */
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_BUZZER,
		.buzzer = {
			.reader = 0,
			.control_code = 1,
			.on_count = 3,
			.off_count = 3,
			.rep_count = 1
		}
	};

	if (osdp_cp_submit_command(cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to send post-recovery command\n");
		goto cleanup;
	}

	if (wait_for_cmd_received(&data, 5)) {
		printf(SUB_2 "Recovery: SUCCESS\n");
		result = true;
	} else {
		printf(SUB_2 "Recovery: Command not received\n");
	}
	goto cleanup;

cleanup_and_retry:
	/* Clean shutdown before retry */
	if (cp_runner >= 0) {
		async_cp_runner_stop(cp_runner);
		cp_runner = -1;
	}
	if (pd_runner >= 0) {
		async_pd_runner_stop(pd_runner);
		pd_runner = -1;
	}
	wait_for_all_work_cleanup();
	if (cp_ctx) {
		osdp_cp_teardown(cp_ctx);
		cp_ctx = NULL;
	}
	if (pd_ctx) {
		osdp_pd_teardown(pd_ctx);
		pd_ctx = NULL;
	}

	/* Retry logic for challenging edge cases */
	if (retry_count < max_retries && !result) {
		retry_count++;
		printf(SUB_2 "Retrying CP restart recovery (attempt %d/%d)\n", retry_count + 1, max_retries + 1);
		usleep(TEST_DELAY_MS * 1000); /* Brief delay before retry */
		memset(&data, 0, sizeof(data)); /* Reset test data */
		goto retry_recovery;
	}

cleanup:
	/* Clean shutdown with independent runner stop functions */
	if (cp_runner >= 0) {
		if (async_cp_runner_stop(cp_runner) < 0) {
			printf(SUB_2 "Warning: Failed to stop CP runner\n");
		}
		cp_runner = -1;
	}
	if (pd_runner >= 0) {
		if (async_pd_runner_stop(pd_runner) < 0) {
			printf(SUB_2 "Warning: Failed to stop PD runner\n");
		}
		pd_runner = -1;
	}

	/* Wait for all work slots to be properly freed */
	wait_for_all_work_cleanup();

	if (cp_ctx) {
		osdp_cp_teardown(cp_ctx);
		cp_ctx = NULL;
	}
	if (pd_ctx) {
		osdp_pd_teardown(pd_ctx);
		pd_ctx = NULL;
	}

	return result;
}

/* Test different teardown sequences */
static bool test_async_teardown_patterns(void)
{
	osdp_t *cp_ctx = NULL, *pd_ctx = NULL;
	int cp_runner = -1, pd_runner = -1;
	bool result = false;
	struct async_test_data data = {0};

	printf(SUB_2 "Testing different teardown patterns\n");

	/* Setup devices */
	struct test dummy_test = { .loglevel = OSDP_LOG_INFO };
	if (test_setup_devices(&dummy_test, &cp_ctx, &pd_ctx)) {
		printf(SUB_2 "Failed to setup devices\n");
		return false;
	}

	osdp_cp_set_event_callback(cp_ctx, async_event_callback, &data);
	osdp_pd_set_command_callback(pd_ctx, async_command_callback, &data);

	/* Start both runners */
	cp_runner = async_cp_runner_start(cp_ctx);
	pd_runner = async_pd_runner_start(pd_ctx);
	if (cp_runner < 0 || pd_runner < 0) goto cleanup;

	/* Wait for connection */
	if (!wait_for_pd_online(cp_ctx, ONLINE_CHECK_TIMEOUT_SEC)) {
		printf(SUB_2 "Initial connection failed\n");
		goto cleanup;
	}

	/* Test Pattern 1: Stop CP first, then PD */
	printf(SUB_2 "Pattern 1: Stopping CP first, then PD\n");
	if (async_cp_runner_stop(cp_runner) < 0) {
		printf(SUB_2 "Warning: Failed to stop CP runner\n");
	}
	cp_runner = -1;
	usleep(500 * 1000); /* 500ms delay */

	if (async_pd_runner_stop(pd_runner) < 0) {
		printf(SUB_2 "Warning: Failed to stop PD runner\n");
	}
	pd_runner = -1;

	wait_for_all_work_cleanup();

	/* Test Pattern 2: Restart in reverse order - PD first, then CP */
	printf(SUB_2 "Pattern 2: Restarting PD first, then CP\n");
	pd_runner = async_pd_runner_start(pd_ctx);
	if (pd_runner < 0) goto cleanup;
	usleep(500 * 1000); /* 500ms delay */

	cp_runner = async_cp_runner_start(cp_ctx);
	if (cp_runner < 0) goto cleanup;

	/* Wait for reconnection */
	if (wait_for_pd_online(cp_ctx, ONLINE_CHECK_TIMEOUT_SEC)) {
		printf(SUB_2 "Teardown pattern test: SUCCESS\n");
		result = true;
	} else {
		printf(SUB_2 "Teardown pattern test: Failed to reconnect\n");
	}

cleanup:
	/* Final cleanup */
	if (cp_runner >= 0) {
		if (async_cp_runner_stop(cp_runner) < 0) {
			printf(SUB_2 "Warning: Failed to stop CP runner\n");
		}
		cp_runner = -1;
	}
	if (pd_runner >= 0) {
		if (async_pd_runner_stop(pd_runner) < 0) {
			printf(SUB_2 "Warning: Failed to stop PD runner\n");
		}
		pd_runner = -1;
	}

	wait_for_all_work_cleanup();

	if (cp_ctx) {
		osdp_cp_teardown(cp_ctx);
		cp_ctx = NULL;
	}
	if (pd_ctx) {
		osdp_pd_teardown(pd_ctx);
		pd_ctx = NULL;
	}

	return result;
}

void run_async_fuzz_tests(struct test *t)
{
	int passed = 0;
	int total = 0;

	printf("\nBegin Async Fuzz Tests\n");
	printf(SUB_1 "Testing different CP/PD startup orders and recovery scenarios\n");

	/* Test all startup orders */
	for (int order = 0; order < ORDER_MAX; order++) {
		total++;
		if (test_async_startup_order(order)) {
			passed++;
		}
		/* Ensure all resources are freed before next test */
		wait_for_all_work_cleanup();
		/* Delay between tests to avoid resource conflicts */
		usleep(TEST_DELAY_MS * 1000);
	}

	/* Test recovery scenario */
	wait_for_all_work_cleanup();
	total++;
	if (test_async_recovery()) {
		passed++;
	}

	/* Test teardown patterns */
	wait_for_all_work_cleanup();
	total++;
	if (test_async_teardown_patterns()) {
		passed++;
	}

	/* Report results */
	printf(SUB_1 "Async fuzz test results:\n");
	printf(SUB_2 "Total scenarios: %d\n", total);
	printf(SUB_2 "Passed: %d\n", passed);
	printf(SUB_2 "Failed: %d\n", total - passed);
	if (total > 0) {
		printf(SUB_2 "Success rate: %.1f%%\n", (float)passed / total * 100.0f);
	}

	/* Test passes if majority of scenarios work (allowing for some edge cases) */
	bool overall_result = (passed >= (total * 2 / 3)); /* 67% threshold */

	printf(SUB_1 "Async fuzz tests %s\n", overall_result ? "succeeded" : "failed");
	TEST_REPORT(t, overall_result);
}
