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
#include <stdatomic.h>
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
		if (test_should_run_case(#m)) {                                \
			test_case_begin((t), #m);                              \
			test_case_end((t), (m((t)->mock_data)));               \
		}                                                              \
	} while (0)

/*
 * Report a boolean helper (true = pass) as its own named, timed case. Use for
 * suites whose sub-tests are `bool` helpers -- including parameterized ones that
 * DO_TEST's fixed `fn(mock_data)` signature cannot wrap. Honors the case filter.
 */
#define TEST_CASE(t, name, expr)                                               \
	do {                                                                   \
		if (test_should_run_case(name)) {                              \
			test_case_begin((t), (name));                          \
			test_case_end((t), (expr) ? 0 : -1);                   \
		}                                                              \
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

/*
 * Case-level gate consulted by DO_TEST: returns false when the run was
 * interrupted (Ctrl-C) or when a `suite:case-glob` filter excludes this name,
 * so the case is neither run nor recorded.
 */
bool test_should_run_case(const char *name);
int test_interrupted(void);
void test_write_junit(struct test *t, const char *path);

/*
 * Lifetime of submitted commands and events.
 *
 * osdp_cp_submit_command() and osdp_pd_submit_event() do not copy: they hold
 * the pointer until the completion callback fires, which happens on the
 * refresh thread after the submitting test has usually returned. Anything
 * submitted must therefore outlive the test that submitted it.
 *
 * Allocate it here. The object is zeroed, and the completion callback returns
 * it to the pool -- so a test allocates, submits, and forgets. Free it
 * yourself only when you allocated but did not submit (an early return, or a
 * submit that was rejected), since no completion will fire for it.
 *
 * Both free functions ignore pointers the pools did not issue, so a suite
 * with its own completion callback can call them unconditionally.
 */
struct osdp_cmd *test_cmd_alloc(void);
void test_cmd_free(struct osdp_cmd *cmd);
struct osdp_event *test_event_alloc(void);
void test_event_free(struct osdp_event *ev);

/*
 * Copy a stack-built object into the pool and return the copy. Lets a test
 * keep a designated initializer, which reads far better than a run of field
 * assignments, while still submitting something that outlives it. Submit the
 * returned pointer -- never the template it was copied from.
 */
struct osdp_cmd *test_cmd_dup(const struct osdp_cmd *src);
struct osdp_event *test_event_dup(const struct osdp_event *src);

/*
 * Submit a stack-built command or event: copies it into the pool, hands the
 * copy to libosdp, and returns that copy. Returns NULL if the pool is empty
 * or libosdp rejected the object, having freed the copy in the latter case,
 * since no completion fires for something never accepted.
 *
 * This is what a test should call. Reach for the alloc/dup helpers directly
 * only when the object has to be built somewhere other than the submit site.
 */
struct osdp_cmd *test_submit_command(osdp_t *ctx, int pd,
				     const struct osdp_cmd *cmd);
struct osdp_event *test_submit_event(osdp_t *ctx, const struct osdp_event *ev);

/*
 * What a completion callback saw, written through the callback's user pointer
 * into storage the test owns -- no file-scope hand-off, and the callback can
 * be shared by suites that keep several of these.
 *
 * The refresh thread writes while the test thread polls, so the fields are
 * atomic rather than volatile: volatile orders nothing between threads and
 * makes count++ a plain read-modify-write. Record id and status before
 * bumping count, so a test that waits on the count then reads the rest sees
 * values that belong together.
 *
 * The struct must outlive the registration -- register it at suite setup, or
 * point the callback elsewhere before it goes out of scope.
 */
struct test_completion {
	atomic_int count;  /* completions seen */
	atomic_int status; /* last enum osdp_completion_status */
	atomic_int id;     /* last osdp_cmd::id, or osdp_event::type */
	atomic_int first_status; /* the same two, as first recorded */
	atomic_int first_id;
};

static inline void test_completion_record(struct test_completion *c, int id,
					  int status)
{
	atomic_store(&c->id, id);
	atomic_store(&c->status, status);
	/*
	 * The first completion is the only one a test can assert on without
	 * racing a refresh thread that may land the next one at any moment.
	 * Written before count goes to 1, so a reader that saw the count is
	 * guaranteed to see these.
	 */
	if (atomic_load(&c->count) == 0) {
		atomic_store(&c->first_id, id);
		atomic_store(&c->first_status, status);
	}
	atomic_fetch_add(&c->count, 1);
}

static inline int test_completion_count(struct test_completion *c)
{
	return atomic_load(&c->count);
}

static inline int test_completion_status(struct test_completion *c)
{
	return atomic_load(&c->status);
}

static inline int test_completion_id(struct test_completion *c)
{
	return atomic_load(&c->id);
}

static inline int test_completion_first_status(struct test_completion *c)
{
	return atomic_load(&c->first_status);
}

static inline int test_completion_first_id(struct test_completion *c)
{
	return atomic_load(&c->first_id);
}

/* Clear to "nothing seen": count 0, status and id -1. */
void test_completion_reset(struct test_completion *c);

/* Poll until at least min_count completions have landed. */
bool test_completion_wait(struct test_completion *c, int min_count,
			  int timeout_sec);

/*
 * Ready-made callbacks for a suite whose only interest is what completed and
 * how. Register with a struct test_completion * as the user pointer; they
 * record into it and return the object to the pool.
 */
void test_cmd_completion_cb(void *arg, int pd, struct osdp_cmd *cmd,
			    enum osdp_completion_status status);
void test_event_completion_cb(void *arg, struct osdp_event *ev,
			      enum osdp_completion_status status);

/* Return every pooled object; for a suite that wants a clean slate. */
void test_alloc_reset(void);

/* Objects the pools have issued and not got back. Zero at the end of a
 * well-behaved suite: every submitted object completes, teardown included. */
int test_alloc_outstanding(void);

/* Helpers */
int test_setup_devices(struct test *t, osdp_t **cp, osdp_t **pd);
int test_setup_devices_ext(struct test *t, osdp_t **cp, osdp_t **pd,
			   uint32_t cp_flags, uint32_t pd_flags);
int test_setup_devices_plain(struct test *t, osdp_t **cp, osdp_t **pd,
			     uint32_t cp_flags, uint32_t pd_flags);
bool test_wait_for_online(osdp_t *cp_ctx, int pd_idx, int timeout_sec);

/*
 * Channel interceptor: a registered hook sees every frame crossing the mock
 * channel and can pass, swallow, or substitute it. INJECT_REPLY additionally
 * redirects: the CP's command is swallowed and `out` is delivered on the
 * PD->CP direction instead, as if the PD had answered with it -- the way a
 * real device answers osdp_BUSY without processing the command. On the
 * PD->CP direction INJECT_REPLY is meaningless and is treated as REPLACE.
 */
enum test_channel_hook_verdict {
	TEST_HOOK_PASS,		/* deliver the frame unmodified */
	TEST_HOOK_DROP,		/* swallow the frame, report it as sent */
	TEST_HOOK_REPLACE,	/* deliver `out` instead of the frame */
	TEST_HOOK_INJECT_REPLY,	/* swallow frame, deliver `out` as the reply */
};

typedef enum test_channel_hook_verdict (*test_channel_hook_fn)(
	void *arg, bool cp_to_pd, const uint8_t *frame, int len,
	uint8_t *out, int *out_len, int out_max);

void test_set_channel_hook(test_channel_hook_fn fn, void *arg);

/*
 * Serialization of API calls with the refresh runners.
 *
 * LibOSDP requires every call on a context to be serialized by the caller
 * (see osdp_cp_refresh()). The async runners refresh on workqueue threads
 * while a test submits, flushes and reshapes from its own thread, so each
 * such call takes one harness-wide lock that the runners also hold across
 * refresh. It is recursive because callbacks run inside refresh, submit
 * and flush, and may call back into the API.
 *
 * The macros below route every mutating call a suite makes through a
 * locked wrapper, so a suite cannot make an unserialized call by accident.
 * The getters the library documents as exempt stay unwrapped. Inside the
 * harness, a wrapper reaches the real function by parenthesizing its name,
 * which suppresses the macro.
 */
void test_api_lock(void);
void test_api_unlock(void);

void test_api_cp_refresh(osdp_t *ctx);
void test_api_pd_refresh(osdp_t *ctx);
void test_api_cp_teardown(osdp_t *ctx);
void test_api_pd_teardown(osdp_t *ctx);
int test_api_cp_submit_command(osdp_t *ctx, int pd, const struct osdp_cmd *cmd);
int test_api_pd_submit_event(osdp_t *ctx, const struct osdp_event *event);
int test_api_cp_flush_commands(osdp_t *ctx, int pd);
int test_api_pd_flush_events(osdp_t *ctx);
int test_api_cp_cancel(osdp_t *ctx, int pd, enum osdp_mp_msg_type what);
int test_api_cp_add_pd(osdp_t *ctx, int num_pd, const osdp_pd_info_t *info);
int test_api_cp_enable_pd(osdp_t *ctx, int pd);
int test_api_cp_disable_pd(osdp_t *ctx, int pd);
bool test_api_cp_is_pd_enabled(const osdp_t *ctx, int pd);
int test_api_cp_modify_flag(osdp_t *ctx, int pd, uint32_t flags, bool do_set);
int test_api_cp_get_capability(const osdp_t *ctx, int pd,
			       struct osdp_pd_cap *cap);
int test_api_pd_set_capabilities(osdp_t *ctx, const struct osdp_pd_cap *cap);
void test_api_cp_set_command_completion_callback(
	osdp_t *ctx, cp_command_completion_callback_t cb, void *arg);
void test_api_cp_set_event_callback(osdp_t *ctx, cp_event_callback_t cb,
				    void *arg);
void test_api_pd_set_command_callback(osdp_t *ctx, pd_command_callback_t cb,
				      void *arg);
void test_api_pd_set_event_completion_callback(
	osdp_t *ctx, pd_event_completion_callback_t cb, void *arg);
int test_api_file_register_ops(osdp_t *ctx, int pd,
			       const struct osdp_file_ops *ops);
int test_api_get_metrics(osdp_t *ctx, int pd_idx, struct osdp_metrics *out);
int test_api_cp_trs_scan_enable(osdp_t *ctx, int pd,
				const struct osdp_trs_scan_params *params);
int test_api_cp_trs_scan_disable(osdp_t *ctx, int pd);
int test_api_cp_trs_get_max_apdu_len(const osdp_t *ctx, int pd);

#define osdp_cp_refresh(...)	      test_api_cp_refresh(__VA_ARGS__)
#define osdp_pd_refresh(...)	      test_api_pd_refresh(__VA_ARGS__)
#define osdp_cp_teardown(...)	      test_api_cp_teardown(__VA_ARGS__)
#define osdp_pd_teardown(...)	      test_api_pd_teardown(__VA_ARGS__)
#define osdp_cp_submit_command(...)   test_api_cp_submit_command(__VA_ARGS__)
#define osdp_pd_submit_event(...)     test_api_pd_submit_event(__VA_ARGS__)
#define osdp_cp_flush_commands(...)   test_api_cp_flush_commands(__VA_ARGS__)
#define osdp_pd_flush_events(...)     test_api_pd_flush_events(__VA_ARGS__)
#define osdp_cp_cancel(...)	      test_api_cp_cancel(__VA_ARGS__)
#define osdp_cp_add_pd(...)	      test_api_cp_add_pd(__VA_ARGS__)
#define osdp_cp_enable_pd(...)	      test_api_cp_enable_pd(__VA_ARGS__)
#define osdp_cp_disable_pd(...)	      test_api_cp_disable_pd(__VA_ARGS__)
#define osdp_cp_is_pd_enabled(...)    test_api_cp_is_pd_enabled(__VA_ARGS__)
#define osdp_cp_modify_flag(...)      test_api_cp_modify_flag(__VA_ARGS__)
#define osdp_cp_get_capability(...)   test_api_cp_get_capability(__VA_ARGS__)
#define osdp_pd_set_capabilities(...) test_api_pd_set_capabilities(__VA_ARGS__)
#define osdp_cp_set_command_completion_callback(...)                           \
	test_api_cp_set_command_completion_callback(__VA_ARGS__)
#define osdp_cp_set_event_callback(...)                                        \
	test_api_cp_set_event_callback(__VA_ARGS__)
#define osdp_pd_set_command_callback(...)                                      \
	test_api_pd_set_command_callback(__VA_ARGS__)
#define osdp_pd_set_event_completion_callback(...)                             \
	test_api_pd_set_event_completion_callback(__VA_ARGS__)
#define osdp_file_register_ops(...)   test_api_file_register_ops(__VA_ARGS__)
#define osdp_get_metrics(...)	      test_api_get_metrics(__VA_ARGS__)
#define osdp_cp_trs_scan_enable(...)  test_api_cp_trs_scan_enable(__VA_ARGS__)
#define osdp_cp_trs_scan_disable(...) test_api_cp_trs_scan_disable(__VA_ARGS__)
#define osdp_cp_trs_get_max_apdu_len(...)                                      \
	test_api_cp_trs_get_max_apdu_len(__VA_ARGS__)

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
void run_busy_tests(struct test *t);
void run_cp_phy_fsm_tests(struct test *t);
void run_cp_phy_tests(struct test *t);
void run_pd_phy_tests(struct test *t);
void run_file_tx_tests(struct test *t, bool line_noise);
void run_file_tx_intermittent_tests(struct test *t);
void run_file_tx_cancel_tests(struct test *t);
void run_file_tx_no_notification_tests(struct test *t);
void run_file_tx_permanent_busy_tests(struct test *t);
void run_file_tx_pd_keep_alive_tests(struct test *t);
void run_file_tx_empty_file_tests(struct test *t);
void run_file_rx_idle_frame_tests(struct test *t);
void run_file_rx_reject_paths_tests(struct test *t);
void run_file_rx_finalize_tests(struct test *t);
void run_multipart_tests(struct test *t);
void run_piv_tests(struct test *t);
void run_command_tests(struct test *t);
void run_event_tests(struct test *t);
void run_bio_tests(struct test *t);
void run_hotplug_tests(struct test *t);
void run_notification_tests(struct test *t);
void run_completion_tests(struct test *t);
void run_metrics_tests(struct test *t);
void run_abi_tests(struct test *t);
void run_async_fuzz_tests(struct test *t);
void run_codec_fuzz_tests(struct test *t);
void run_sc_tests(struct test *t);
void run_vector_tests(struct test *t);
void run_sc_policy_tests(struct test *t);
void run_trs_tests(struct test *t); /* no-op unless OPT_BUILD_OSDP_TRS */
void run_trs_emu_tests(struct test *t); /* no-op unless OPT_BUILD_OSDP_TRS */
void run_pd_zc_tests(struct test *t); /* no-op unless OPT_OSDP_RX_ZERO_COPY */

#define printf(...) test_printf(__VA_ARGS__)

#endif
