/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "test.h"

/*
 * Secure channel handshake checks on the CP side. The PD's RMAC_I reply
 * (SCS_14) is rewritten on the wire by the mock-channel hook; the CP must
 * only report the secure channel up for a reply it can verify.
 */

extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);
extern uint8_t test_osdp_compute_checksum(uint8_t *msg, int length);

#define SCS_14_STATUS_REJECTED 0xff

struct hs_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;
	atomic_int sc_active_notifs;
	atomic_int rmac_i_rewrites;
	struct test_completion completion;
	struct osdp_cmd buzzer_cmd;
};

static struct hs_ctx g_hs;

enum rmac_i_rewrite {
	REWRITE_NONE,
	REWRITE_FLIP_PAYLOAD_BYTE,
	REWRITE_STATUS_BYTE,
};

static enum test_channel_hook_verdict rmac_i_hook(void *arg, bool cp_to_pd,
						  const uint8_t *frame, int len,
						  uint8_t *out, int *out_len,
						  int out_max)
{
	enum rmac_i_rewrite *mode = arg;
	int base, id_off, data_off, body_len;

	if (cp_to_pd || *mode == REWRITE_NONE || len > out_max)
		return TEST_HOOK_PASS;

	base = (len > 0 && frame[0] == 0xff) ? 1 : 0;
	if (len < base + 9 || !(frame[base + 4] & 0x08))
		return TEST_HOOK_PASS;
	id_off = base + 5 + frame[base + 5];
	if (id_off >= len || frame[id_off] != REPLY_RMAC_I)
		return TEST_HOOK_PASS;
	data_off = id_off + 1;

	memcpy(out, frame, len);
	if (*mode == REWRITE_FLIP_PAYLOAD_BYTE)
		out[data_off + 5] ^= 0x01;
	else
		out[base + 7] = SCS_14_STATUS_REJECTED;

	body_len = len - base;
	if (frame[base + 4] & 0x04) {
		uint16_t crc =
			test_osdp_compute_crc16(out + base, body_len - 2);
		out[len - 2] = crc & 0xff;
		out[len - 1] = (crc >> 8) & 0xff;
	} else {
		out[len - 1] =
			test_osdp_compute_checksum(out + base, body_len - 1);
	}
	*out_len = len;
	atomic_fetch_add(&g_hs.rmac_i_rewrites, 1);
	return TEST_HOOK_REPLACE;
}

static int hs_cp_event_cb(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(pd);

	if (ev->type == OSDP_EVENT_NOTIFICATION &&
	    ev->notif.type == OSDP_NOTIFICATION_SC_STATUS &&
	    ev->notif.sc_status.active) {
		atomic_fetch_add(&g_hs.sc_active_notifs, 1);
	}
	return 0;
}

static int hs_pd_command_cb(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(cmd);
	return 0;
}

static int hs_setup(struct test *t, enum rmac_i_rewrite *mode)
{
	if (test_setup_devices_ext(t, &g_hs.cp_ctx, &g_hs.pd_ctx,
				   OSDP_FLAG_ENABLE_NOTIFICATION, 0)) {
		printf(SUB_1 "Failed to setup devices!\n");
		return -1;
	}
	osdp_cp_set_event_callback(g_hs.cp_ctx, hs_cp_event_cb, NULL);
	osdp_pd_set_command_callback(g_hs.pd_ctx, hs_pd_command_cb, NULL);
	osdp_cp_set_command_completion_callback(
		g_hs.cp_ctx, test_cmd_completion_cb, &g_hs.completion);
	test_set_channel_hook(rmac_i_hook, mode);

	g_hs.cp_runner = async_runner_start(g_hs.cp_ctx, osdp_cp_refresh);
	g_hs.pd_runner = async_runner_start(g_hs.pd_ctx, osdp_pd_refresh);
	if (g_hs.cp_runner < 0 || g_hs.pd_runner < 0) {
		printf(SUB_1 "Failed to create CP/PD runners\n");
		return -1;
	}
	return 0;
}

static void hs_teardown(void)
{
	test_set_channel_hook(NULL, NULL);
	async_runner_stop(g_hs.cp_runner);
	async_runner_stop(g_hs.pd_runner);
	osdp_cp_teardown(g_hs.cp_ctx);
	osdp_pd_teardown(g_hs.pd_ctx);
	memset(&g_hs, 0, sizeof(g_hs));
}

static uint8_t cp_sc_mask(void)
{
	uint8_t mask = 0;

	osdp_get_sc_status_mask(g_hs.cp_ctx, &mask);
	return mask;
}

static bool sc_stays_down_for(struct test *t, enum rmac_i_rewrite mode,
			      const char *what)
{
	const char *why = NULL;
	int rewrites;

	printf(SUB_1 "%s -- ", what);
	if (hs_setup(t, &mode)) {
		hs_teardown();
		return false;
	}
	sleep(4);
	/* With no RMAC_I rewritten, SC staying down proves nothing. */
	rewrites = atomic_load(&g_hs.rmac_i_rewrites);
	if (rewrites == 0)
		why = "no RMAC_I was rewritten!";
	else if (atomic_load(&g_hs.sc_active_notifs) != 0 || (cp_sc_mask() & 1))
		why = "SC was reported up!";
	hs_teardown();
	if (why) {
		printf("%s\n", why);
		return false;
	}
	printf("success! (%d RMAC_I rewritten)\n", rewrites);
	return true;
}

/* Every RMAC_I has one payload byte flipped: SC must never be reported up. */
static bool rmac_i_forged_keeps_sc_down(struct test *t)
{
	return sc_stays_down_for(t, REWRITE_FLIP_PAYLOAD_BYTE,
				 "forged RMAC_I keeps SC down");
}

/* RMAC_I value intact but SCS_14 status byte rewritten to 0xFF. */
static bool rmac_i_rejected_status_keeps_sc_down(struct test *t)
{
	return sc_stays_down_for(t, REWRITE_STATUS_BYTE,
				 "rejected SCS_14 status keeps SC down");
}

/* No hook: the CP reports SC up exactly once and a buzzer command completes OK. */
static bool rmac_i_genuine_brings_sc_up(struct test *t)
{
	enum rmac_i_rewrite mode = REWRITE_NONE;
	bool ok = false;

	printf(SUB_1 "genuine RMAC_I brings SC up once -- ");
	if (hs_setup(t, &mode))
		goto out;
	if (!test_wait_for_online(g_hs.cp_ctx, 0, 10)) {
		printf("PD never came online!\n");
		goto out;
	}
	g_hs.buzzer_cmd = (struct osdp_cmd){
		.id = OSDP_CMD_BUZZER,
		.buzzer = {
			.reader = 0,
			.control_code = 2,
			.on_count = 5,
			.off_count = 5,
			.rep_count = 1,
		},
	};
	if (!test_submit_command(g_hs.cp_ctx, 0, &g_hs.buzzer_cmd)) {
		printf("failed to submit buzzer!\n");
		goto out;
	}
	if (!test_completion_wait(&g_hs.completion, 1, 10) ||
	    test_completion_first_status(&g_hs.completion) !=
		    OSDP_COMPLETION_OK) {
		printf("buzzer did not complete OK!\n");
		goto out;
	}
	if (atomic_load(&g_hs.sc_active_notifs) != 1) {
		printf("expected 1 SC up notification, saw %d!\n",
		       atomic_load(&g_hs.sc_active_notifs));
		goto out;
	}
	ok = true;
	printf("success!\n");
out:
	hs_teardown();
	return ok;
}

void run_sc_handshake_tests(struct test *t)
{
	printf("\nStarting SC handshake tests\n");

	TEST_CASE(t, "rmac_i_forged_keeps_sc_down",
		  rmac_i_forged_keeps_sc_down(t));
	TEST_CASE(t, "rmac_i_rejected_status_keeps_sc_down",
		  rmac_i_rejected_status_keeps_sc_down(t));
	TEST_CASE(t, "rmac_i_genuine_brings_sc_up",
		  rmac_i_genuine_brings_sc_up(t));
}
