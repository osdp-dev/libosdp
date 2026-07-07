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
	const char *color = NULL;
	bool warn = false;
	bool err = false;
	int ret;

	if (fmt && (text_has(fmt, "Warning") || text_has(fmt, "WARN") ||
		    text_has(fmt, "skip"))) {
		warn = true;
	} else if (fmt && (text_has(fmt, "error") || text_has(fmt, "Error") ||
			   text_has(fmt, "failed") || text_has(fmt, "FAIL"))) {
		err = true;
	}

	if (warn) {
		color = C_YEL;
		if (g_active_test && g_active_test->current_case_idx >= 0) {
			g_active_test->warnings++;
			g_active_test->cases[g_active_test->current_case_idx].warn_count++;
		}
	} else if (err) {
		color = C_RED;
		if (g_active_test && g_active_test->current_case_idx >= 0) {
			g_active_test->errors++;
			g_active_test->cases[g_active_test->current_case_idx].error_count++;
		}
	} else {
		color = C_DIM;
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

#ifndef OPT_OSDP_RX_ZERO_COPY

int test_mock_cp_send(void *data, uint8_t *buf, int len)
{
	int i;
	ARG_UNUSED(data);
	assert(len < MOCK_BUF_LEN);

	maybe_corrupt_buffer(buf, len);
	for (i = 0; i < len; i++) {
		if (CIRCBUF_PUSH(cp_to_pd_buf, buf + i))
			break;
	}
	return i;
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
	int i;
	ARG_UNUSED(data);

	maybe_corrupt_buffer(buf, len);
	for (i = 0; i < len; i++) {
		if (CIRCBUF_PUSH(pd_to_cp_buf, buf + i))
			break;
	}
	return i;
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
	ARG_UNUSED(data);
	if (buf == NULL || len <= 0) {
		return -1;
	}
	maybe_corrupt_buffer(buf, len);
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
	ARG_UNUSED(data);
	if (buf == NULL || len <= 0) {
		return -1;
	}
	maybe_corrupt_buffer(buf, len);
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

int test_setup_devices_ext(struct test *t, osdp_t **cp, osdp_t **pd,
			   uint32_t cp_flags, uint32_t pd_flags)
{
#ifndef OPT_OSDP_LOG_MINIMAL
	osdp_logger_init("osdp", t->loglevel, NULL);
#else
	ARG_UNUSED(t);
#endif /* OPT_OSDP_LOG_MINIMAL */

	/* Shared mock channel; drop stale traffic left by the previous suite so a
	 * fresh PD does not read a non-zero sequence on its first packet. */
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
		.scbk = scbk,
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

int test_setup_devices(struct test *t, osdp_t **cp, osdp_t **pd)
{
	return test_setup_devices_ext(t, cp, pd, 0, 0);
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

	if (t->failure > 0 || t->errors > 0)
		return -1;

	return 0;
}

static void run_file_tx_suite(struct test *t)
{
	run_file_tx_tests(t, false);
	run_file_tx_intermittent_tests(t);
	run_file_tx_permanent_busy_tests(t);
}

int main(int argc, char *argv[])
{
	int i, rc;
	struct test t;
	static const struct test_suite_entry suites[] = {
#ifdef OPT_OSDP_RX_ZERO_COPY
		/* Fast zero-copy canary first: gives a precise signal before the
		 * long general suites, which also cover the prebuilt-reply path
		 * but abort hard if a reply's packet_buf is lost. */
		{ "pd_zc", run_pd_zc_tests },
#endif
		{ "cp_phy", run_cp_phy_tests },
		{ "pd_phy", run_pd_phy_tests },
		{ "cp_fsm", run_cp_fsm_tests },
		{ "file_tx", run_file_tx_suite },
		{ "commands", run_command_tests },
		{ "events", run_event_tests },
		{ "hotplug", run_hotplug_tests },
		{ "notifications", run_notification_tests },
		{ "async_fuzz", run_async_fuzz_tests },
		{ "codec_fuzz", run_codec_fuzz_tests },
		{ "sc", run_sc_tests },
		{ "vectors", run_vector_tests },
	};

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	srand(time(NULL));

	workqueue_create(&test_wq, MAX_TEST_WORK);

	test_start(&t, OSDP_LOG_INFO);

	for (i = 0; i < (int)(sizeof(suites) / sizeof(suites[0])); i++) {
		test_suite_begin(&t, suites[i].name);
		if (suites[i].run) {
			suites[i].run(&t);
		}
		test_suite_end(&t);
	}

	rc = test_end(&t);

	workqueue_destroy(&test_wq);

	return rc;
}
