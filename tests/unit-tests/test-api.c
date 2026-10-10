/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>

#include "test.h"

static pthread_mutex_t test_api_mutex;
static pthread_once_t test_api_mutex_once = PTHREAD_ONCE_INIT;

static void test_api_mutex_init(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&test_api_mutex, &attr);
	pthread_mutexattr_destroy(&attr);
}

void test_api_lock(void)
{
	pthread_once(&test_api_mutex_once, test_api_mutex_init);
	pthread_mutex_lock(&test_api_mutex);
}

void test_api_unlock(void)
{
	pthread_mutex_unlock(&test_api_mutex);
}

void test_api_cp_refresh(osdp_t *ctx)
{
	test_api_lock();
	(osdp_cp_refresh)(ctx);
	test_api_unlock();
}

void test_api_pd_refresh(osdp_t *ctx)
{
	test_api_lock();
	(osdp_pd_refresh)(ctx);
	test_api_unlock();
}

void test_api_cp_teardown(osdp_t *ctx)
{
	test_api_lock();
	(osdp_cp_teardown)(ctx);
	test_api_unlock();
}

void test_api_pd_teardown(osdp_t *ctx)
{
	test_api_lock();
	(osdp_pd_teardown)(ctx);
	test_api_unlock();
}

int test_api_cp_submit_command(osdp_t *ctx, int pd, const struct osdp_cmd *cmd)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_submit_command)(ctx, pd, cmd);
	test_api_unlock();
	return rc;
}

int test_api_pd_submit_event(osdp_t *ctx, const struct osdp_event *event)
{
	int rc;

	test_api_lock();
	rc = (osdp_pd_submit_event)(ctx, event);
	test_api_unlock();
	return rc;
}

int test_api_cp_flush_commands(osdp_t *ctx, int pd)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_flush_commands)(ctx, pd);
	test_api_unlock();
	return rc;
}

int test_api_pd_flush_events(osdp_t *ctx)
{
	int rc;

	test_api_lock();
	rc = (osdp_pd_flush_events)(ctx);
	test_api_unlock();
	return rc;
}

int test_api_cp_cancel(osdp_t *ctx, int pd, enum osdp_mp_msg_type what)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_cancel)(ctx, pd, what);
	test_api_unlock();
	return rc;
}

int test_api_cp_add_pd(osdp_t *ctx, int num_pd, const osdp_pd_info_t *info)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_add_pd)(ctx, num_pd, info);
	test_api_unlock();
	return rc;
}

int test_api_cp_enable_pd(osdp_t *ctx, int pd)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_enable_pd)(ctx, pd);
	test_api_unlock();
	return rc;
}

int test_api_cp_disable_pd(osdp_t *ctx, int pd)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_disable_pd)(ctx, pd);
	test_api_unlock();
	return rc;
}

bool test_api_cp_is_pd_enabled(const osdp_t *ctx, int pd)
{
	bool rc;

	test_api_lock();
	rc = (osdp_cp_is_pd_enabled)(ctx, pd);
	test_api_unlock();
	return rc;
}

int test_api_cp_modify_flag(osdp_t *ctx, int pd, uint32_t flags, bool do_set)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_modify_flag)(ctx, pd, flags, do_set);
	test_api_unlock();
	return rc;
}

int test_api_cp_get_capability(const osdp_t *ctx, int pd,
			       struct osdp_pd_cap *cap)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_get_capability)(ctx, pd, cap);
	test_api_unlock();
	return rc;
}

int test_api_pd_set_capabilities(osdp_t *ctx, const struct osdp_pd_cap *cap)
{
	int rc;

	test_api_lock();
	rc = (osdp_pd_set_capabilities)(ctx, cap);
	test_api_unlock();
	return rc;
}

void test_api_cp_set_command_completion_callback(
	osdp_t *ctx, cp_command_completion_callback_t cb, void *arg)
{
	test_api_lock();
	(osdp_cp_set_command_completion_callback)(ctx, cb, arg);
	test_api_unlock();
}

void test_api_cp_set_event_callback(osdp_t *ctx, cp_event_callback_t cb,
				    void *arg)
{
	test_api_lock();
	(osdp_cp_set_event_callback)(ctx, cb, arg);
	test_api_unlock();
}

void test_api_pd_set_command_callback(osdp_t *ctx, pd_command_callback_t cb,
				      void *arg)
{
	test_api_lock();
	(osdp_pd_set_command_callback)(ctx, cb, arg);
	test_api_unlock();
}

void test_api_pd_set_event_completion_callback(
	osdp_t *ctx, pd_event_completion_callback_t cb, void *arg)
{
	test_api_lock();
	(osdp_pd_set_event_completion_callback)(ctx, cb, arg);
	test_api_unlock();
}

int test_api_file_register_ops(osdp_t *ctx, int pd,
			       const struct osdp_file_ops *ops)
{
	int rc;

	test_api_lock();
	rc = (osdp_file_register_ops)(ctx, pd, ops);
	test_api_unlock();
	return rc;
}

int test_api_get_metrics(osdp_t *ctx, int pd_idx, struct osdp_metrics *out)
{
	int rc;

	test_api_lock();
	rc = (osdp_get_metrics)(ctx, pd_idx, out);
	test_api_unlock();
	return rc;
}

int test_api_cp_trs_scan_enable(osdp_t *ctx, int pd,
				const struct osdp_trs_scan_params *params)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_trs_scan_enable)(ctx, pd, params);
	test_api_unlock();
	return rc;
}

int test_api_cp_trs_scan_disable(osdp_t *ctx, int pd)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_trs_scan_disable)(ctx, pd);
	test_api_unlock();
	return rc;
}

int test_api_cp_trs_get_max_apdu_len(const osdp_t *ctx, int pd)
{
	int rc;

	test_api_lock();
	rc = (osdp_cp_trs_get_max_apdu_len)(ctx, pd);
	test_api_unlock();
	return rc;
}
