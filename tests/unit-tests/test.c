/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <signal.h>
#include <fnmatch.h>

#include <osdp.h>

#include "osdp_common.h"
#include "test.h"

#include <utils/workqueue.h>
#include <utils/circbuf.h>

#define MAX_TEST_WORK 20  /* Increased for async fuzz testing */
#define MOCK_BUF_LEN 512

#ifndef OPT_OSDP_RX_ZERO_COPY
CIRCBUF_DEF(uint8_t, cp_to_pd_buf, MOCK_BUF_LEN);
CIRCBUF_DEF(uint8_t, pd_to_cp_buf, MOCK_BUF_LEN);
#else
/*
 * Zero-copy mock channel: models the wire as whole packets, not a byte stream.
 * The production zero-copy channel (e.g. osprio's osdp_frame.c) delimits frames
 * by an inter-byte idle gap and hands libosdp one complete packet per recv_pkt;
 * here each send() enqueues exactly one packet and recv_pkt() dequeues one. That
 * preserves frame boundaries even when line-noise corrupts bytes within a frame,
 * which is precisely how the timing-delimited hardware framer behaves.
 *
 * Single-producer/single-consumer per direction (send writes head; recv/release
 * write tail), matching the lock-free discipline the byte CIRCBUF already relied
 * on for the threaded async-fuzz runners.
 */
#define TEST_PKTQ_DEPTH 8
#define TEST_PKT_MAXLEN OSDP_PACKET_BUF_SIZE

struct test_pktq {
	uint8_t pkt[TEST_PKTQ_DEPTH][TEST_PKT_MAXLEN];
	int len[TEST_PKTQ_DEPTH];
	volatile int head;
	volatile int tail;
};

static struct test_pktq cp_to_pd_q; /* CP send -> PD recv */
static struct test_pktq pd_to_cp_q; /* PD send -> CP recv */

static int pktq_push(struct test_pktq *q, const uint8_t *buf, int len)
{
	int next = (q->head + 1) % TEST_PKTQ_DEPTH;

	/* Mirror the production uart_send NULL guard: a channel must reject a
	 * NULL/empty buffer rather than transmit it. Keeps a libosdp defect
	 * that hands us a NULL packet_buf a graceful test failure, not a crash. */
	if (buf == NULL || len <= 0) {
		return -1;
	}
	if (next == q->tail) {
		return 0; /* full: transient, caller may retry next refresh */
	}
	if (len > TEST_PKT_MAXLEN) {
		len = TEST_PKT_MAXLEN;
	}
	memcpy(q->pkt[q->head], buf, len);
	q->len[q->head] = len;
	q->head = next;
	return len;
}

static int pktq_recv(struct test_pktq *q, const uint8_t **buf, int *max_len)
{
	if (q->tail == q->head) {
		return -1; /* empty */
	}
	*buf = q->pkt[q->tail];
	*max_len = q->len[q->tail];
	return 0;
}

static void pktq_release(struct test_pktq *q)
{
	if (q->tail != q->head) {
		q->tail = (q->tail + 1) % TEST_PKTQ_DEPTH;
	}
}

static void pktq_flush(struct test_pktq *q)
{
	q->tail = q->head;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

struct test_suite_entry {
	const char *name;
	void (*run)(struct test *t);
};

static struct test *g_active_test;
static bool g_color_enabled;

/* Run selection + interruption state, consulted at the per-case DO_TEST gate. */
static const char *g_case_glob;      /* NULL = run every case in the suite */
static const char *g_junit_path;     /* NULL = no JUnit XML emitted */
static volatile sig_atomic_t g_interrupted;

static void test_on_signal(int signum)
{
	ARG_UNUSED(signum);
	g_interrupted = 1;
}

int test_interrupted(void)
{
	return g_interrupted;
}

bool test_should_run_case(const char *name)
{
	if (g_interrupted)
		return false;
	if (g_case_glob && fnmatch(g_case_glob, name, 0) != 0)
		return false;
	return true;
}

#define C_RST "\x1b[0m"
#define C_DIM "\x1b[90m"
#define C_RED "\x1b[31m"
#define C_GRN "\x1b[32m"
#define C_YEL "\x1b[33m"
#define C_CYN "\x1b[36m"

static double test_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static const char *status_str(enum test_status status)
{
	switch (status) {
	case TEST_STATUS_PASS:
		return "PASS";
	case TEST_STATUS_FAIL:
		return "FAIL";
	case TEST_STATUS_SKIP:
		return "SKIP";
	case TEST_STATUS_ERROR:
	default:
		return "ERROR";
	}
}

static const char *status_color(enum test_status status)
{
	switch (status) {
	case TEST_STATUS_PASS:
		return C_GRN;
	case TEST_STATUS_SKIP:
		return C_YEL;
	case TEST_STATUS_FAIL:
	case TEST_STATUS_ERROR:
	default:
		return C_RED;
	}
}

static bool text_has(const char *s, const char *needle)
{
	return strstr(s, needle) != NULL;
}

static void format_duration(double ms, char *buf, size_t buf_len)
{
	if (ms >= 1000.0) {
		snprintf(buf, buf_len, "%.2fs", ms / 1000.0);
	} else {
		snprintf(buf, buf_len, "%.2fms", ms);
	}
}

static int test_vprintf_colored(const char *prefix_color, const char *fmt,
				va_list ap)
{
	int ret;

	if (g_color_enabled && prefix_color != NULL) {
		fputs(prefix_color, stdout);
		ret = vfprintf(stdout, fmt, ap);
		fputs(C_RST, stdout);
		return ret;
	}
	return vfprintf(stdout, fmt, ap);
}

int test_printf(const char *fmt, ...)
{
	va_list ap;
	const char *color = C_DIM;
	int ret;

	/*
	 * Colorize by message text as a visual hint only. Do NOT derive
	 * pass/fail from log text: case outcome comes from the DO_TEST/TEST_CASE
	 * return code, and libosdp itself logs legitimate "[ERROR] ..." lines
	 * (e.g. a deliberately-disabled PD) that must not fail the run.
	 */
	if (fmt && (text_has(fmt, "Warning") || text_has(fmt, "WARN") ||
		    text_has(fmt, "skip"))) {
		color = C_YEL;
	} else if (fmt && (text_has(fmt, "error") || text_has(fmt, "Error") ||
			   text_has(fmt, "failed") || text_has(fmt, "FAIL"))) {
		color = C_RED;
	}

	va_start(ap, fmt);
	ret = test_vprintf_colored(color, fmt, ap);
	va_end(ap);
	return ret;
}

void test_log_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	test_vprintf_colored(C_DIM, fmt, ap);
	va_end(ap);
}

void test_log_warn(const char *fmt, ...)
{
	va_list ap;
	if (g_active_test && g_active_test->current_case_idx >= 0) {
		g_active_test->warnings++;
		g_active_test->cases[g_active_test->current_case_idx].warn_count++;
	}
	va_start(ap, fmt);
	test_vprintf_colored(C_YEL, fmt, ap);
	va_end(ap);
}

void test_log_error(const char *fmt, ...)
{
	va_list ap;
	if (g_active_test && g_active_test->current_case_idx >= 0) {
		g_active_test->errors++;
		g_active_test->cases[g_active_test->current_case_idx].error_count++;
	}
	va_start(ap, fmt);
	test_vprintf_colored(C_RED, fmt, ap);
	va_end(ap);
}

void test_skip(const char *reason)
{
	if (reason) {
		test_log_warn("%s\n", reason);
	}
}

static void test_mark_result(struct test *t, struct test_case_record *rec,
			     enum test_status status)
{
	t->tests++;
	rec->status = status;

	switch (status) {
	case TEST_STATUS_PASS:
		t->success++;
		break;
	case TEST_STATUS_SKIP:
		t->skipped++;
		break;
	case TEST_STATUS_FAIL:
		t->failure++;
		break;
	case TEST_STATUS_ERROR:
	default:
		t->failure++;
		t->errors++;
		break;
	}
}

void test_case_begin(struct test *t, const char *name)
{
	struct test_case_record *rec;
	int idx;

	if (t->tests >= TEST_MAX_CASES) {
		return;
	}

	idx = t->tests;
	t->current_case_idx = idx;
	rec = &t->cases[idx];
	memset(rec, 0, sizeof(*rec));
	snprintf(rec->suite, sizeof(rec->suite), "%s",
		 t->current_suite ? t->current_suite : "default");
	snprintf(rec->name, sizeof(rec->name), "%s", name ? name : "unnamed");

	t->case_start_ms = test_now_ms();
	if (g_color_enabled) {
		fprintf(stdout, C_CYN "[ RUN    ]" C_RST " %s.%s\n",
			rec->suite, rec->name);
	} else {
		fprintf(stdout, "[ RUN    ] %s.%s\n", rec->suite, rec->name);
	}
}

void test_case_end(struct test *t, int rc)
{
	struct test_case_record *rec;
	enum test_status status;
	const char *prefix;
	const char *color;
	char dur[24];

	if (t->current_case_idx < 0 || t->current_case_idx >= TEST_MAX_CASES) {
		return;
	}

	rec = &t->cases[t->current_case_idx];
	rec->duration_ms = test_now_ms() - t->case_start_ms;

	if (rc == 0) {
		status = TEST_STATUS_PASS;
	} else if (rc == TEST_SKIP_RC) {
		status = TEST_STATUS_SKIP;
		snprintf(rec->message, sizeof(rec->message), "skipped");
	} else {
		status = TEST_STATUS_FAIL;
		snprintf(rec->message, sizeof(rec->message), "rc=%d", rc);
	}

	test_mark_result(t, rec, status);
	prefix = status_str(status);
	color = status_color(status);
	format_duration(rec->duration_ms, dur, sizeof(dur));
	if (g_color_enabled) {
		fprintf(stdout, "%s[ %-6s ]%s %s.%s (%s)\n",
			color, prefix, C_RST, rec->suite, rec->name, dur);
	} else {
		fprintf(stdout, "[ %-6s ] %s.%s (%s)\n",
			prefix, rec->suite, rec->name, dur);
	}

	t->current_case_idx = -1;
}

void test_report(struct test *t, const char *func, int line, bool status)
{
	struct test_case_record *rec;
	enum test_status st;
	const char *prefix;
	const char *color;
	double now_ms;
	int idx;
	char dur[24];

	if (t->tests >= TEST_MAX_CASES) {
		return;
	}

	idx = t->tests;
	rec = &t->cases[idx];
	memset(rec, 0, sizeof(*rec));
	snprintf(rec->suite, sizeof(rec->suite), "%s",
		 t->current_suite ? t->current_suite : "default");
	snprintf(rec->name, sizeof(rec->name), "%s:%d", func, line);

	now_ms = test_now_ms();
	rec->duration_ms = now_ms - t->report_last_ms;
	t->report_last_ms = now_ms;

	st = status ? TEST_STATUS_PASS : TEST_STATUS_FAIL;
	if (!status) {
		snprintf(rec->message, sizeof(rec->message), "assertion failed");
	}
	test_mark_result(t, rec, st);

	prefix = status_str(st);
	color = status_color(st);
	format_duration(rec->duration_ms, dur, sizeof(dur));
	if (g_color_enabled) {
		fprintf(stdout, "%s[ %-6s ]%s %s.%s (%s)\n",
			color, prefix, C_RST, rec->suite, rec->name, dur);
	} else {
		fprintf(stdout, "[ %-6s ] %s.%s (%s)\n",
			prefix, rec->suite, rec->name, dur);
	}
}

void test_suite_begin(struct test *t, const char *name)
{
	int idx = t->suite_count;
	struct test_suite_record *suite;

	if (idx >= TEST_MAX_SUITES) {
		return;
	}

	suite = &t->suites[idx];
	memset(suite, 0, sizeof(*suite));
	snprintf(suite->name, sizeof(suite->name), "%s", name);
	t->current_suite = suite->name;
	t->suite_start_ms = test_now_ms();
	t->report_last_ms = t->suite_start_ms;
}

void test_suite_end(struct test *t)
{
	struct test_suite_record *suite;
	int i;

	if (t->suite_count >= TEST_MAX_SUITES) {
		return;
	}

	suite = &t->suites[t->suite_count];
	suite->duration_ms = test_now_ms() - t->suite_start_ms;
	for (i = 0; i < t->tests; i++) {
		if (strcmp(t->cases[i].suite, suite->name) != 0) {
			continue;
		}
		suite->tests++;
		switch (t->cases[i].status) {
		case TEST_STATUS_PASS:
			suite->pass++;
			break;
		case TEST_STATUS_SKIP:
			suite->skip++;
			break;
		case TEST_STATUS_FAIL:
			suite->fail++;
			break;
		case TEST_STATUS_ERROR:
		default:
			suite->error++;
			break;
		}
	}
	t->suite_count++;
}

/* Runner types for independent CP and PD management */
enum runner_type {
	RUNNER_TYPE_CP,
	RUNNER_TYPE_PD
};

struct test_async_data {
	osdp_t *ctx;
	void (*refresh)(osdp_t *ctx);
	enum runner_type type;
	bool is_running;
	struct async_runner_s *runner;
};

workqueue_t test_wq;

int async_runner(void *data)
{
	struct test_async_data *td = data;

	if (!td->is_running) {
		return WORK_DONE; /* Stop if marked as not running */
	}

	td->refresh(td->ctx);
	usleep(10 * 1000);

	return WORK_YIELD; /* Continue running */
}

void async_runner_free(void *data)
{
	work_t *work = data;

	free(work->arg);
	free(work);
}

work_t *g_test_works[MAX_TEST_WORK];

/* Generic async runner start function */
static int async_runner_start_generic(osdp_t *ctx, void (*fn)(osdp_t *), enum runner_type type)
{
	int i, rc;

	struct test_async_data *data = malloc(sizeof(struct test_async_data));
	data->ctx = ctx;
	data->refresh = fn;
	data->type = type;
	data->is_running = true;

	for (i = 0; i < MAX_TEST_WORK; i++) {
		if (g_test_works[i] == NULL)
			break;
	}

	if (i == MAX_TEST_WORK) {
		printf("async_runner_start: test works exhausted\n");
		free(data);
		return -1;
	}

	g_test_works[i] = malloc(sizeof(work_t));
	g_test_works[i]->arg = data;
	g_test_works[i]->work_fn = async_runner;
	g_test_works[i]->complete_fn = NULL;

	rc = workqueue_add_work(&test_wq, g_test_works[i]);
	if (rc != 0) {
		printf("async_runner_start: test wq add work failed!\n");
		free(g_test_works[i]->arg);
		free(g_test_works[i]);
		g_test_works[i] = NULL;
		return -1;
	}

	return i;
}

/* Start CP runner independently */
int async_cp_runner_start(osdp_t *cp_ctx)
{
	printf("Starting CP async runner\n");
	return async_runner_start_generic(cp_ctx, osdp_cp_refresh, RUNNER_TYPE_CP);
}

/* Start PD runner independently */
int async_pd_runner_start(osdp_t *pd_ctx)
{
	printf("Starting PD async runner\n");
	return async_runner_start_generic(pd_ctx, osdp_pd_refresh, RUNNER_TYPE_PD);
}

/* Legacy function for backward compatibility */
int async_runner_start(osdp_t *ctx, void (*fn)(osdp_t *))
{
	enum runner_type type = (fn == osdp_cp_refresh) ? RUNNER_TYPE_CP : RUNNER_TYPE_PD;
	return async_runner_start_generic(ctx, fn, type);
}

int async_runner_stop(int work_id)
{
	if (work_id < 0 || work_id >= MAX_TEST_WORK || g_test_works[work_id] == NULL) {
		printf("async_runner_stop: invalid work id!\n");
		return -1;
	}

	/* Mark as not running to allow graceful shutdown */
	struct test_async_data *data = (struct test_async_data *)g_test_works[work_id]->arg;
	if (data) {
		data->is_running = false;
		printf(SUB_1 "Stopping %s async runner\n",
		       (data->type == RUNNER_TYPE_CP) ? "CP" : "PD");
	}

	workqueue_cancel_work(&test_wq, g_test_works[work_id]);
	while (workqueue_work_is_complete(&test_wq, g_test_works[work_id]) == false)
		usleep(50 * 1000);
	async_runner_free(g_test_works[work_id]);
	g_test_works[work_id] = NULL;

	return 0;
}

/* Stop CP runner specifically */
int async_cp_runner_stop(int work_id)
{
	return async_runner_stop(work_id);
}

/* Stop PD runner specifically */
int async_pd_runner_stop(int work_id)
{
	return async_runner_stop(work_id);
}

volatile bool g_introduce_line_noise;
unsigned long g_total_packets;
unsigned long g_corrupted_packets;

void enable_line_noise()
{
	g_introduce_line_noise = true;
}

void disable_line_noise()
{
	g_introduce_line_noise = false;
}

void print_line_noise_stats()
{
	printf(SUB_1 "LN-Stats: Total:%lu Corrupted:%lu",
	       g_total_packets, g_corrupted_packets);
}

void corrupt_buffer(uint8_t *buf, int len)
{
	int p, n = 3;

	while (n--) {
		p = randint(len);
		buf[p] = randint(255);
	}
}

void maybe_corrupt_buffer(uint8_t *buf, int len)
{
	if (!g_introduce_line_noise)
		return;
	g_total_packets++;
	if (randint(10*1000) < (5*1000))
		return;
	corrupt_buffer(buf, len);
	g_corrupted_packets++;
}

static test_channel_hook_fn g_channel_hook;
static void *g_channel_hook_arg;

void test_set_channel_hook(test_channel_hook_fn fn, void *arg)
{
	g_channel_hook = fn;
	g_channel_hook_arg = arg;
}

/*
 * Run the registered channel hook over an outbound frame. Returns the
 * verdict, with `out`/`out_len` filled for REPLACE and INJECT_REPLY. When
 * no hook is registered every frame passes.
 */
static enum test_channel_hook_verdict channel_hook_filter(
	bool cp_to_pd, const uint8_t *frame, int len,
	uint8_t *out, int *out_len, int out_max)
{
	enum test_channel_hook_verdict verdict;

	if (!g_channel_hook)
		return TEST_HOOK_PASS;

	*out_len = 0;
	verdict = g_channel_hook(g_channel_hook_arg, cp_to_pd, frame, len,
				 out, out_len, out_max);
	if (!cp_to_pd && verdict == TEST_HOOK_INJECT_REPLY)
		verdict = TEST_HOOK_REPLACE;
	if ((verdict == TEST_HOOK_REPLACE ||
	     verdict == TEST_HOOK_INJECT_REPLY) && *out_len <= 0)
		verdict = TEST_HOOK_DROP;
	return verdict;
}

#ifndef OPT_OSDP_RX_ZERO_COPY

static int circbuf_push_cp_to_pd(const uint8_t *buf, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (CIRCBUF_PUSH(cp_to_pd_buf, (uint8_t *)buf + i))
			break;
	}
	return i;
}

static int circbuf_push_pd_to_cp(const uint8_t *buf, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (CIRCBUF_PUSH(pd_to_cp_buf, (uint8_t *)buf + i))
			break;
	}
	return i;
}

int test_mock_cp_send(void *data, uint8_t *buf, int len)
{
	uint8_t out[MOCK_BUF_LEN];
	int out_len;
	ARG_UNUSED(data);
	assert(len < MOCK_BUF_LEN);

	maybe_corrupt_buffer(buf, len);
	switch (channel_hook_filter(true, buf, len, out, &out_len, sizeof(out))) {
	case TEST_HOOK_DROP:
		return len;
	case TEST_HOOK_REPLACE:
		circbuf_push_cp_to_pd(out, out_len);
		return len;
	case TEST_HOOK_INJECT_REPLY:
		circbuf_push_pd_to_cp(out, out_len);
		return len;
	case TEST_HOOK_PASS:
		break;
	}
	return circbuf_push_cp_to_pd(buf, len);
}

int test_mock_cp_receive(void *data, uint8_t *buf, int len)
{
	int i;
	ARG_UNUSED(data);
	ARG_UNUSED(len);

	for (i = 0; i < len; i++) {
		if (CIRCBUF_POP(pd_to_cp_buf, buf + i))
			break;
	}
	return i;
}

void test_mock_cp_flush(void *data)
{
	ARG_UNUSED(data);
	CIRCBUF_FLUSH(pd_to_cp_buf);
}

int test_mock_pd_send(void *data, uint8_t *buf, int len)
{
	uint8_t out[MOCK_BUF_LEN];
	int out_len;
	ARG_UNUSED(data);

	maybe_corrupt_buffer(buf, len);
	switch (channel_hook_filter(false, buf, len, out, &out_len, sizeof(out))) {
	case TEST_HOOK_DROP:
		return len;
	case TEST_HOOK_REPLACE:
		circbuf_push_pd_to_cp(out, out_len);
		return len;
	case TEST_HOOK_INJECT_REPLY: /* mapped to REPLACE by the filter */
	case TEST_HOOK_PASS:
		break;
	}
	return circbuf_push_pd_to_cp(buf, len);
}

int test_mock_pd_receive(void *data, uint8_t *buf, int len)
{
	int i;
	ARG_UNUSED(data);
	ARG_UNUSED(len);

	for (i = 0; i < len; i++) {
		if (CIRCBUF_POP(cp_to_pd_buf, buf + i))
			break;
	}
	return i;
}

void test_mock_pd_flush(void *data)
{
	ARG_UNUSED(data);
	CIRCBUF_FLUSH(cp_to_pd_buf);
}

#else /* OPT_OSDP_RX_ZERO_COPY */

int test_mock_cp_send(void *data, uint8_t *buf, int len)
{
	uint8_t out[TEST_PKT_MAXLEN];
	int out_len;
	ARG_UNUSED(data);
	if (buf == NULL || len <= 0) {
		return -1;
	}
	maybe_corrupt_buffer(buf, len);
	switch (channel_hook_filter(true, buf, len, out, &out_len, sizeof(out))) {
	case TEST_HOOK_DROP:
		return len;
	case TEST_HOOK_REPLACE:
		pktq_push(&cp_to_pd_q, out, out_len);
		return len;
	case TEST_HOOK_INJECT_REPLY:
		pktq_push(&pd_to_cp_q, out, out_len);
		return len;
	case TEST_HOOK_PASS:
		break;
	}
	return pktq_push(&cp_to_pd_q, buf, len);
}

int test_mock_cp_recv_pkt(void *data, const uint8_t **buf, int *max_len)
{
	ARG_UNUSED(data);
	return pktq_recv(&pd_to_cp_q, buf, max_len);
}

void test_mock_cp_release_pkt(void *data, const uint8_t *buf)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	pktq_release(&pd_to_cp_q);
}

void test_mock_cp_flush(void *data)
{
	ARG_UNUSED(data);
	pktq_flush(&pd_to_cp_q);
}

int test_mock_pd_send(void *data, uint8_t *buf, int len)
{
	uint8_t out[TEST_PKT_MAXLEN];
	int out_len;
	ARG_UNUSED(data);
	if (buf == NULL || len <= 0) {
		return -1;
	}
	maybe_corrupt_buffer(buf, len);
	switch (channel_hook_filter(false, buf, len, out, &out_len, sizeof(out))) {
	case TEST_HOOK_DROP:
		return len;
	case TEST_HOOK_REPLACE:
		pktq_push(&pd_to_cp_q, out, out_len);
		return len;
	case TEST_HOOK_INJECT_REPLY: /* mapped to REPLACE by the filter */
	case TEST_HOOK_PASS:
		break;
	}
	return pktq_push(&pd_to_cp_q, buf, len);
}

int test_mock_pd_recv_pkt(void *data, const uint8_t **buf, int *max_len)
{
	ARG_UNUSED(data);
	return pktq_recv(&cp_to_pd_q, buf, max_len);
}

void test_mock_pd_release_pkt(void *data, const uint8_t *buf)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	pktq_release(&cp_to_pd_q);
}

void test_mock_pd_flush(void *data)
{
	ARG_UNUSED(data);
	pktq_flush(&cp_to_pd_q);
}

#endif /* OPT_OSDP_RX_ZERO_COPY */

static int test_setup_devices_impl(struct test *t, osdp_t **cp, osdp_t **pd,
				   uint32_t cp_flags, uint32_t pd_flags,
				   bool with_scbk)
{
#ifndef OPT_OSDP_LOG_MINIMAL
	osdp_logger_init("osdp", t->loglevel, NULL);
#else
	ARG_UNUSED(t);
#endif /* OPT_OSDP_LOG_MINIMAL */

	/* Shared mock channel; drop stale traffic left by the previous suite so a
	 * fresh PD does not read a non-zero sequence on its first packet. The
	 * channel hook is likewise per-suite state that must not leak. */
	test_set_channel_hook(NULL, NULL);
#ifndef OPT_OSDP_RX_ZERO_COPY
	CIRCBUF_FLUSH(cp_to_pd_buf);
	CIRCBUF_FLUSH(pd_to_cp_buf);
#else
	pktq_flush(&cp_to_pd_q);
	pktq_flush(&pd_to_cp_q);
#endif

	uint8_t scbk[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
	};
	/* mock application data */
	struct osdp_channel cp_channel = {
		.data = NULL,
		.send = test_mock_cp_send,
#ifndef OPT_OSDP_RX_ZERO_COPY
		.recv = test_mock_cp_receive,
#else
		.recv_pkt = test_mock_cp_recv_pkt,
		.release_pkt = test_mock_cp_release_pkt,
#endif
		.flush = test_mock_cp_flush,
	};
	osdp_pd_info_t info_cp = {
		.address = 101,
		.baud_rate = 9600,
		.flags = cp_flags,
		.scbk = with_scbk ? scbk : NULL,
	};

	*cp = osdp_cp_setup(&cp_channel, 1, &info_cp);
	if (*cp == NULL) {
		printf(SUB_1 "cp init failed!\n");
		return -1;
	}

	struct osdp_pd_cap cap[] = {
		{ OSDP_PD_CAP_READER_AUDIBLE_OUTPUT, 1, 1 },
		{ OSDP_PD_CAP_READER_LED_CONTROL, 1, 1 },
		{ OSDP_PD_CAP_OUTPUT_CONTROL, 1, 4 },
		{ OSDP_PD_CAP_READER_TEXT_OUTPUT, 1, 1 },
		{ OSDP_PD_CAP_CONTACT_STATUS_MONITORING, 1, 8 },
		{ OSDP_PD_CAP_BIOMETRICS, 1, 1 },
		{ OSDP_PD_CAP_TIME_KEEPING, 1, 0 },
		{ OSDP_PD_CAP_SMART_CARD_SUPPORT, 1, 1 },
		{ -1, -1, -1 }
	};

	struct osdp_channel pd_channel = {
		.data = NULL,
		.send = test_mock_pd_send,
#ifndef OPT_OSDP_RX_ZERO_COPY
		.recv = test_mock_pd_receive,
#else
		.recv_pkt = test_mock_pd_recv_pkt,
		.release_pkt = test_mock_pd_release_pkt,
#endif
		.flush = test_mock_pd_flush,
	};
	osdp_pd_info_t info_pd = {
		.address = 101,
		.baud_rate = 9600,
		.flags = pd_flags,
		.id = {
			.version = 1,
			.model = 153,
			.vendor_code = 31337,
			.serial_number = 0x01020304,
			.firmware_version = 0x0A0B0C0D,
		},
		.cap = cap,
		.scbk = scbk,
	};

	*pd = (struct osdp *)osdp_pd_setup(&pd_channel, &info_pd);
	if (*pd == NULL) {
		printf(SUB_1 "pd init failed!\n");
		osdp_cp_teardown(*cp);
		return -1;
	}

	return 0;
}

int test_setup_devices_ext(struct test *t, osdp_t **cp, osdp_t **pd,
			   uint32_t cp_flags, uint32_t pd_flags)
{
	return test_setup_devices_impl(t, cp, pd, cp_flags, pd_flags, true);
}

/* No SCBK on the CP: the link comes up and stays in plaintext, the way
 * legacy readers on unprovisioned installs run. */
int test_setup_devices_plain(struct test *t, osdp_t **cp, osdp_t **pd,
			     uint32_t cp_flags, uint32_t pd_flags)
{
	return test_setup_devices_impl(t, cp, pd, cp_flags, pd_flags, false);
}

int test_setup_devices(struct test *t, osdp_t **cp, osdp_t **pd)
{
	return test_setup_devices_ext(t, cp, pd, 0, 0);
}

bool test_wait_for_online(osdp_t *cp_ctx, int pd_idx, int timeout_sec)
{
	uint8_t status = 0;
	int rc = 0;

	while (rc++ < timeout_sec) {
		osdp_get_status_mask(cp_ctx, &status);
		if (status & (1 << pd_idx)) {
			return true;
		}
		usleep(1000 * 1000);
	}
	return false;
}

void test_start(struct test *t, int log_level)
{
	memset(t, 0, sizeof(*t));
	t->loglevel = log_level;
	t->current_case_idx = -1;
	t->run_start_ms = test_now_ms();
	g_active_test = t;
	g_color_enabled = isatty(STDOUT_FILENO) != 0;

	fprintf(stdout, "\n");
	fprintf(stdout, "===============================================================\n");
	if (g_color_enabled) {
		fprintf(stdout, C_CYN "LibOSDP Unit Tests\n" C_RST);
	} else {
		fprintf(stdout, "LibOSDP Unit Tests\n");
	}
	fprintf(stdout, "===============================================================\n");
}

static void print_test_results_table(const struct test *t)
{
	int i;
	char dur[24];

	fprintf(stdout, "\nTest Results\n");
	fprintf(stdout, "+-----+----------------------+------------------------------+--------+----------+\n");
	fprintf(stdout, "| #   | Suite                | Test                         | Status | Time     |\n");
	fprintf(stdout, "+-----+----------------------+------------------------------+--------+----------+\n");
	for (i = 0; i < t->tests; i++) {
		const struct test_case_record *rec = &t->cases[i];
		const char *s = status_str(rec->status);
		format_duration(rec->duration_ms, dur, sizeof(dur));
		if (g_color_enabled) {
			fprintf(stdout, "| %-3d | %-20.20s | %-28.28s | %s%-6s%s | %-8.8s |\n",
				i + 1, rec->suite, rec->name,
				status_color(rec->status), s, C_RST, dur);
		} else {
			fprintf(stdout, "| %-3d | %-20.20s | %-28.28s | %-6s | %-8.8s |\n",
				i + 1, rec->suite, rec->name, s, dur);
		}
	}
	fprintf(stdout, "+-----+----------------------+------------------------------+--------+----------+\n");
}

static void print_suite_summary_table(const struct test *t)
{
	int i;
	char dur[24];

	fprintf(stdout, "\nSuite Summary\n");
	fprintf(stdout, "+----------------------+-------+------+------+------+-------+----------+\n");
	fprintf(stdout, "| Suite                | Tests | Pass | Fail | Skip | Error | Time     |\n");
	fprintf(stdout, "+----------------------+-------+------+------+------+-------+----------+\n");
	for (i = 0; i < t->suite_count; i++) {
		const struct test_suite_record *s = &t->suites[i];
		format_duration(s->duration_ms, dur, sizeof(dur));
		fprintf(stdout, "| %-20.20s | %5d | %4d | %4d | %4d | %5d | %-8.8s |\n",
			s->name, s->tests, s->pass, s->fail, s->skip,
			s->error, dur);
	}
	fprintf(stdout, "+----------------------+-------+------+------+------+-------+----------+\n");
}

static void print_failed_tests(const struct test *t)
{
	int i, count = 0;

	for (i = 0; i < t->tests; i++) {
		if (t->cases[i].status == TEST_STATUS_FAIL ||
		    t->cases[i].status == TEST_STATUS_ERROR) {
			count++;
		}
	}
	if (!count) {
		return;
	}

	fprintf(stdout, "\nFailures\n");
	fprintf(stdout, "+-----+----------------------+------------------------------+-------------------------------+\n");
	fprintf(stdout, "| #   | Suite                | Test                         | Message                       |\n");
	fprintf(stdout, "+-----+----------------------+------------------------------+-------------------------------+\n");
	for (i = 0; i < t->tests; i++) {
		const struct test_case_record *rec = &t->cases[i];
		if (rec->status != TEST_STATUS_FAIL &&
		    rec->status != TEST_STATUS_ERROR) {
			continue;
		}
		fprintf(stdout, "| %-3d | %-20.20s | %-28.28s | %-29.29s |\n",
			i + 1, rec->suite, rec->name,
			rec->message[0] ? rec->message : "failure");
	}
	fprintf(stdout, "+-----+----------------------+------------------------------+-------------------------------+\n");
}

static void print_slowest_tests(const struct test *t, int n)
{
	int i, j, used[TEST_MAX_CASES] = { 0 };
	char dur[24];

	if (n > t->tests) {
		n = t->tests;
	}
	fprintf(stdout, "\nSlowest Tests\n");
	for (i = 0; i < n; i++) {
		int best = -1;
		for (j = 0; j < t->tests; j++) {
			if (used[j]) {
				continue;
			}
			if (best < 0 || t->cases[j].duration_ms > t->cases[best].duration_ms) {
				best = j;
			}
		}
		if (best < 0) {
			break;
		}
		used[best] = 1;
		format_duration(t->cases[best].duration_ms, dur, sizeof(dur));
		fprintf(stdout, "  %d. %s.%s (%s)\n", i + 1,
			t->cases[best].suite, t->cases[best].name,
			dur);
	}
}

static void junit_escape(FILE *f, const char *s)
{
	for (; *s; s++) {
		switch (*s) {
		case '&': fputs("&amp;", f); break;
		case '<': fputs("&lt;", f); break;
		case '>': fputs("&gt;", f); break;
		case '"': fputs("&quot;", f); break;
		case '\'': fputs("&apos;", f); break;
		default: fputc((unsigned char)*s, f); break;
		}
	}
}

void test_write_junit(struct test *t, const char *path)
{
	const struct test_suite_record *s;
	const struct test_case_record *rec;
	FILE *f;
	int i, j;

	if (!path)
		return;
	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "test: cannot open JUnit file '%s'\n", path);
		return;
	}

	fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	fprintf(f, "<testsuites tests=\"%d\" failures=\"%d\" skipped=\"%d\">\n",
		t->tests, t->failure + t->errors, t->skipped);

	for (i = 0; i < t->suite_count; i++) {
		s = &t->suites[i];
		fprintf(f, "  <testsuite name=\"");
		junit_escape(f, s->name);
		fprintf(f, "\" tests=\"%d\" failures=\"%d\" skipped=\"%d\" time=\"%.3f\">\n",
			s->tests, s->fail + s->error, s->skip,
			s->duration_ms / 1000.0);

		for (j = 0; j < t->tests; j++) {
			rec = &t->cases[j];
			if (strcmp(rec->suite, s->name) != 0)
				continue;
			fprintf(f, "    <testcase name=\"");
			junit_escape(f, rec->name);
			fprintf(f, "\" classname=\"");
			junit_escape(f, rec->suite);
			fprintf(f, "\" time=\"%.3f\"", rec->duration_ms / 1000.0);
			if (rec->status == TEST_STATUS_PASS) {
				fprintf(f, "/>\n");
			} else if (rec->status == TEST_STATUS_SKIP) {
				fprintf(f, ">\n      <skipped/>\n    </testcase>\n");
			} else {
				const char *tag = rec->status == TEST_STATUS_ERROR ?
						  "error" : "failure";
				fprintf(f, ">\n      <%s message=\"", tag);
				junit_escape(f, rec->message[0] ? rec->message : tag);
				fprintf(f, "\"/>\n    </testcase>\n");
			}
		}
		fprintf(f, "  </testsuite>\n");
	}
	fprintf(f, "</testsuites>\n");
	fclose(f);
}

int test_end(struct test *t)
{
	double total_ms = test_now_ms() - t->run_start_ms;
	double pass_rate = (t->tests > 0) ? ((double)t->success * 100.0 / t->tests) : 0.0;
	char dur[24];

	print_test_results_table(t);
	print_suite_summary_table(t);
	print_failed_tests(t);
	print_slowest_tests(t, 5);

	fprintf(stdout, "\nSummary\n");
	fprintf(stdout, "  Suites:   %d\n", t->suite_count);
	fprintf(stdout, "  Tests:    %d\n", t->tests);
	fprintf(stdout, "  Pass:     %d\n", t->success);
	fprintf(stdout, "  Fail:     %d\n", t->failure);
	fprintf(stdout, "  Skip:     %d\n", t->skipped);
	fprintf(stdout, "  Errors:   %d\n", t->errors);
	fprintf(stdout, "  Warnings: %d\n", t->warnings);
	fprintf(stdout, "  PassRate: %.1f%%\n", pass_rate);
	format_duration(total_ms, dur, sizeof(dur));
	fprintf(stdout, "  Time:     %s\n\n", dur);

	test_write_junit(t, g_junit_path);

	if (t->failure > 0 || t->errors > 0)
		return -1;

	return 0;
}

static void run_file_tx_suite(struct test *t)
{
	run_file_tx_tests(t, false);
	run_file_tx_intermittent_tests(t);
	run_file_tx_permanent_busy_tests(t);
	run_file_tx_pd_keep_alive_tests(t);
	run_file_rx_idle_frame_tests(t);
	run_file_rx_reject_paths_tests(t);
}

static void test_usage(const char *prog, const struct test_suite_entry *suites,
		       int n_suites)
{
	int i;

	fprintf(stdout,
		"Usage: %s [options] [suite[:case-glob] ...]\n"
		"\n"
		"With no suite arguments every suite runs. Name one or more suites\n"
		"to run only those; append ':<glob>' to run only matching cases\n"
		"(glob matches the DO_TEST case name, e.g. piv:test_piv_*).\n"
		"\n"
		"Options:\n"
		"  --list           list suite names, one per line\n"
		"  --junit <path>   write JUnit XML results to <path>\n"
		"  -h, --help       show this help\n"
		"\nSuites:\n", prog);
	for (i = 0; i < n_suites; i++)
		fprintf(stdout, "  %s\n", suites[i].name);
}

int main(int argc, char *argv[])
{
	static const struct test_suite_entry suites[] = {
#define SUITE(name, fn) { #name, fn },
#ifdef OPT_OSDP_RX_ZERO_COPY
/* Zero-copy canary runs first: a precise signal before the long general suites. */
#define SUITE_ZC(name, fn) SUITE(name, fn)
#else
#define SUITE_ZC(name, fn)
#endif
#include "test-suites.def"
#undef SUITE
#undef SUITE_ZC
	};
	const int n_suites = (int)(sizeof(suites) / sizeof(suites[0]));
	bool run_it[TEST_MAX_SUITES];
	bool any_selected = false;
	int i, rc;
	struct test t;

	for (i = 0; i < n_suites; i++)
		run_it[i] = false;

	for (i = 1; i < argc; i++) {
		char *a = argv[i];
		char *colon;
		int j, found = -1;

		if (!strcmp(a, "--list")) {
			for (j = 0; j < n_suites; j++)
				fprintf(stdout, "%s\n", suites[j].name);
			return 0;
		}
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			test_usage(argv[0], suites, n_suites);
			return 0;
		}
		if (!strcmp(a, "--junit")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "test: --junit needs a path\n");
				return 2;
			}
			g_junit_path = argv[++i];
			continue;
		}
		if (a[0] == '-') {
			fprintf(stderr, "test: unknown option '%s'\n", a);
			return 2;
		}

		/* positional: suite name, optionally suffixed with :case-glob */
		colon = strchr(a, ':');
		if (colon) {
			*colon = '\0';
			g_case_glob = colon + 1;
		}
		for (j = 0; j < n_suites; j++) {
			if (!strcmp(a, suites[j].name)) {
				found = j;
				break;
			}
		}
		if (found < 0) {
			fprintf(stderr, "test: unknown suite '%s'\n", a);
			return 2;
		}
		run_it[found] = true;
		any_selected = true;
	}

	if (!any_selected) {
		for (i = 0; i < n_suites; i++)
			run_it[i] = true;
	}

	signal(SIGINT, test_on_signal);
	signal(SIGTERM, test_on_signal);

	srand(time(NULL));

	workqueue_create(&test_wq, MAX_TEST_WORK);

	test_start(&t, OSDP_LOG_INFO);

	for (i = 0; i < n_suites && !g_interrupted; i++) {
		if (!run_it[i])
			continue;
		test_suite_begin(&t, suites[i].name);
		if (suites[i].run) {
			suites[i].run(&t);
		}
		test_suite_end(&t);
	}

	rc = test_end(&t);

	workqueue_destroy(&test_wq);

	if (g_interrupted)
		return 130;

	return rc;
}
