/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_TEST_H_
#define _OSDP_TEST_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include "osdp_common.h"

#define SUB_1 "    -- "
#define SUB_2 "        -- "

#define TEST_MAX_CASES 512
#define TEST_MAX_SUITES 64
#define TEST_MAX_NAME_LEN 96
#define TEST_MAX_MSG_LEN 160
#define TEST_SKIP_RC 77

enum test_status {
	TEST_STATUS_PASS = 0,
	TEST_STATUS_FAIL,
	TEST_STATUS_SKIP,
	TEST_STATUS_ERROR,
};

struct test_case_record {
	char suite[TEST_MAX_NAME_LEN];
	char name[TEST_MAX_NAME_LEN];
	char message[TEST_MAX_MSG_LEN];
	enum test_status status;
	double duration_ms;
	int warn_count;
	int error_count;
};

struct test_suite_record {
	char name[TEST_MAX_NAME_LEN];
	int tests;
	int pass;
	int fail;
	int skip;
	int error;
	double duration_ms;
};

#define CHECK_ARRAY(a, l, e)                                                   \
	do {                                                                   \
		if (l < 0)                                                     \
			printf("error! invalid length %d\n", len);             \
		else if (l != sizeof(e) || memcmp(a, e, sizeof(e))) {          \
			printf("error! comparison failed!\n");                 \
			hexdump(e, sizeof(e), SUB_1 "Expected");               \
			hexdump(a, l, SUB_1 "Found");                          \
			return -1;                                             \
		}                                                              \
	} while (0)

struct test {
	int loglevel;
	int tests;
	int success;
	int failure;
	int skipped;
	int errors;
	int warnings;
	void *mock_data;
	const char *current_suite;
	int current_case_idx;
	double run_start_ms;
	double suite_start_ms;
	double case_start_ms;
	double report_last_ms;
	int suite_count;
	struct test_case_record cases[TEST_MAX_CASES];
	struct test_suite_record suites[TEST_MAX_SUITES];
};

#define DO_TEST(t, m)                                                          \
	do {                                                                   \
		test_case_begin((t), #m);                                      \
		test_case_end((t), (m((t)->mock_data)));                       \
	} while (0)

#define TEST_REPORT(t, s)                                                      \
	do {                                                                   \
		test_report((t), __func__, __LINE__, (s));                     \
	} while (0)

#define TEST_SKIP(msg)                                                         \
	do {                                                                   \
		test_skip((msg));                                              \
		return TEST_SKIP_RC;                                           \
	} while (0)

#define TEST_LOG_INFO(...) test_log_info(__VA_ARGS__)
#define TEST_LOG_WARN(...) test_log_warn(__VA_ARGS__)
#define TEST_LOG_ERROR(...) test_log_error(__VA_ARGS__)

int test_printf(const char *fmt, ...);
void test_log_info(const char *fmt, ...);
void test_log_warn(const char *fmt, ...);
void test_log_error(const char *fmt, ...);
void test_skip(const char *reason);
void test_case_begin(struct test *t, const char *name);
void test_case_end(struct test *t, int rc);
void test_report(struct test *t, const char *func, int line, bool status);
void test_suite_begin(struct test *t, const char *name);
void test_suite_end(struct test *t);

/* Helpers */
int test_setup_devices(struct test *t, osdp_t **cp, osdp_t **pd);
int test_setup_devices_ext(struct test *t, osdp_t **cp, osdp_t **pd,
			   uint32_t cp_flags, uint32_t pd_flags);
int async_runner_start(osdp_t *ctx, void (*fn)(osdp_t *));
int async_runner_stop(int runner);
int async_cp_runner_start(osdp_t *cp_ctx);
int async_pd_runner_start(osdp_t *pd_ctx);
int async_cp_runner_stop(int work_id);
int async_pd_runner_stop(int work_id);
void enable_line_noise();
void disable_line_noise();
void print_line_noise_stats();

void run_cp_fsm_tests(struct test *t);
void run_cp_phy_fsm_tests(struct test *t);
void run_cp_phy_tests(struct test *t);
void run_pd_phy_tests(struct test *t);
void run_file_tx_tests(struct test *t, bool line_noise);
void run_file_tx_intermittent_tests(struct test *t);
void run_file_tx_permanent_busy_tests(struct test *t);
void run_command_tests(struct test *t);
void run_event_tests(struct test *t);
void run_hotplug_tests(struct test *t);
void run_notification_tests(struct test *t);
void run_async_fuzz_tests(struct test *t);
void run_codec_fuzz_tests(struct test *t);
void run_sc_tests(struct test *t);
void run_vector_tests(struct test *t);
void run_pd_zc_tests(struct test *t); /* no-op unless OPT_OSDP_RX_ZERO_COPY */

#define printf(...) test_printf(__VA_ARGS__)

#endif
