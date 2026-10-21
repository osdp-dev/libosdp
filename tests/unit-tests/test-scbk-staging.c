/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "test.h"

extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);
extern void test_pd_stage_scbk(struct osdp_pd *pd, const uint8_t *scbk);

/*
 * SCBK staging on the PD. A KEYSET on a real-key session is held beside the
 * main key, and reaches the PD app only once a session on it has carried a
 * MAC-verified command. The CP here switches to the new key on the ACK, as
 * a CP that does not stage would.
 */

#define STG_LOG_MAX 16

static const uint8_t K1[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
static const uint8_t K2[16] = {
	0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
	0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf
};
static const uint8_t K3[16] = {
	0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
	0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf
};

struct stg_ctx {
	struct test *t;
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;

	/* Written by the PD refresh thread, read by the test thread: the
	 * first byte of the key in each KEYSET the PD app was handed. */
	atomic_int log[STG_LOG_MAX];
	atomic_int log_len;

	/* How the PD app answers a KEYSET: BUSY this many more times, then
	 * accept, or refuse outright. */
	atomic_int pd_busy_left;
	atomic_bool pd_refuse;

	/* Drop the next REPLY_CCRYPT the PD sends. */
	atomic_bool drop_next_ccrypt;
	atomic_int ccrypt_dropped;

	/* Replace the CP's next MAC'd POLL with a plaintext KEYSET for K2
	 * under SCS_13, as an on-path attacker without the key could. */
	atomic_bool forge_scs13_keyset;
	atomic_bool forged_sent;
	/* The PD's reply id to the forged frame; -1 until it is seen. */
	atomic_int forged_reply;

	/* Command id of the first MAC'd command the CP sent; -1 for none. */
	atomic_int first_mac_cmd;

	/* Key the CP submits in a KEYSET the moment the PD goes online. */
	const uint8_t *keyset_on_online;
	atomic_bool keyset_on_online_sent;

	struct test_completion completion;
};

static struct stg_ctx g_stg;

static void stg_log(int entry)
{
	int i = atomic_load(&g_stg.log_len);

	if (i < STG_LOG_MAX) {
		atomic_store(&g_stg.log[i], entry);
		atomic_store(&g_stg.log_len, i + 1);
	}
}

static void stg_print_log(void)
{
	int i, len = atomic_load(&g_stg.log_len);

	printf("log:");
	for (i = 0; i < len; i++)
		printf(" %#x", atomic_load(&g_stg.log[i]));
	printf("\n");
}

/* The KEYSETs the PD app saw are exactly `expect`. */
static bool stg_log_is(const int *expect, int n)
{
	int i;

	if (atomic_load(&g_stg.log_len) != n)
		goto mismatch;
	for (i = 0; i < n; i++) {
		if (atomic_load(&g_stg.log[i]) != expect[i])
			goto mismatch;
	}
	return true;
mismatch:
	stg_print_log();
	return false;
}

static bool stg_wait_for_log(int n, int timeout_sec)
{
	int i;

	for (i = 0; i < timeout_sec * 10; i++) {
		if (atomic_load(&g_stg.log_len) >= n)
			return true;
		usleep(100 * 1000);
	}
	return false;
}

static bool stg_secure_online(void)
{
	uint8_t online = 0, secure = 0;

	osdp_get_status_mask(g_stg.cp_ctx, &online);
	osdp_get_sc_status_mask(g_stg.cp_ctx, &secure);
	return (online & 1) && (secure & 1);
}

static bool stg_wait_for_secure_online(int timeout_sec)
{
	int i;

	for (i = 0; i < timeout_sec * 10; i++) {
		if (stg_secure_online())
			return true;
		usleep(100 * 1000);
	}
	return false;
}

static struct osdp_pd *stg_pd(void)
{
	return osdp_to_pd(g_stg.pd_ctx, 0);
}

/* The PD runs on `key` as its main key and holds nothing staged. */
static bool stg_pd_settled_on(const uint8_t *key)
{
	struct osdp_pd *pd = stg_pd();

	return memcmp(pd->sc.scbk, key, 16) == 0 &&
	       !ISSET_FLAG(pd, PD_FLAG_SCBK_STAGED);
}

/*
 * Frame a plaintext KEYSET for K2 under an SCS_13 security block, which
 * carries no MAC, reusing the address and sequence number of the CP frame
 * it replaces so that only the security block can get it refused.
 */
static int stg_frame_scs13_keyset(const uint8_t *frame, int base, uint8_t *out,
				  int out_max)
{
	const uint8_t sb[] = { 3, SCS_13, 1 };
	int len = 0, start, pkt_len;
	uint16_t crc;

	pkt_len = 5 + sizeof(sb) + 3 + sizeof(K2) + 2;
	if (out_max < base + pkt_len)
		return -1;
	if (base)
		out[len++] = 0xff;
	start = len;
	out[len++] = 0x53;
	out[len++] = frame[base + 1];
	out[len++] = pkt_len & 0xff;
	out[len++] = (pkt_len >> 8) & 0xff;
	out[len++] = (frame[base + 4] & 0x03) | 0x04 | 0x08; /* SQN, CRC, SCB */
	memcpy(out + len, sb, sizeof(sb));
	len += sizeof(sb);
	out[len++] = CMD_KEYSET;
	out[len++] = 1;
	out[len++] = sizeof(K2);
	memcpy(out + len, K2, sizeof(K2));
	len += sizeof(K2);
	crc = test_osdp_compute_crc16(out + start, len - start);
	out[len++] = crc & 0xff;
	out[len++] = (crc >> 8) & 0xff;
	return len;
}

static enum test_channel_hook_verdict stg_hook(void *arg, bool cp_to_pd,
					       const uint8_t *frame, int len,
					       uint8_t *out, int *out_len,
					       int out_max)
{
	int base, id_off, scs_type = -1;

	ARG_UNUSED(arg);

	base = (len > 0 && frame[0] == 0xff) ? 1 : 0;
	if (len < base + 6)
		return TEST_HOOK_PASS;
	id_off = base + 5;
	if (frame[base + 4] & 0x08) {
		if (len < base + 7)
			return TEST_HOOK_PASS;
		scs_type = frame[base + 6];
		id_off += frame[base + 5];
	}
	if (id_off >= len)
		return TEST_HOOK_PASS;

	if (cp_to_pd) {
		int expect_none = -1;

		if ((scs_type == 0x15 || scs_type == 0x17) &&
		    frame[id_off] == CMD_POLL &&
		    atomic_load(&g_stg.forge_scs13_keyset) &&
		    !atomic_exchange(&g_stg.forged_sent, true)) {
			*out_len = stg_frame_scs13_keyset(frame, base, out,
							  out_max);
			if (*out_len > 0)
				return TEST_HOOK_REPLACE;
		}
		if (scs_type == 0x15 || scs_type == 0x17)
			atomic_compare_exchange_strong(&g_stg.first_mac_cmd,
						       &expect_none,
						       frame[id_off]);
		return TEST_HOOK_PASS;
	}
	if (atomic_load(&g_stg.forged_sent) &&
	    atomic_load(&g_stg.forged_reply) == -1)
		atomic_store(&g_stg.forged_reply, frame[id_off]);
	if (!atomic_load(&g_stg.drop_next_ccrypt) ||
	    frame[id_off] != REPLY_CCRYPT)
		return TEST_HOOK_PASS;

	atomic_store(&g_stg.drop_next_ccrypt, false);
	atomic_fetch_add(&g_stg.ccrypt_dropped, 1);
	return TEST_HOOK_DROP;
}

static void stg_submit_keyset(const uint8_t *key)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_KEYSET,
		.keyset = { .type = 1, .length = 16 },
	};

	memcpy(cmd.keyset.data, key, 16);
	if (!test_submit_command(g_stg.cp_ctx, 0, &cmd))
		printf(SUB_1 "KEYSET submit failed\n");
}

static int stg_cp_event_cb(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(pd);

	if (ev->type == OSDP_EVENT_NOTIFICATION &&
	    ev->notif.type == OSDP_NOTIFICATION_PD_STATUS &&
	    ev->notif.pd_status.online && g_stg.keyset_on_online &&
	    !atomic_exchange(&g_stg.keyset_on_online_sent, true)) {
		stg_submit_keyset(g_stg.keyset_on_online);
	}
	return 0;
}

static int stg_pd_command_cb(void *arg, struct osdp_cmd *cmd)
{
	ARG_UNUSED(arg);

	if (cmd->id != OSDP_CMD_KEYSET)
		return 0;
	stg_log(cmd->keyset.data[0]);
	if (atomic_load(&g_stg.pd_refuse))
		return -1;
	if (atomic_load(&g_stg.pd_busy_left) > 0) {
		atomic_fetch_sub(&g_stg.pd_busy_left, 1);
		return OSDP_PD_CMD_BUSY;
	}
	return 0;
}

static void stg_stop(void)
{
	test_set_channel_hook(NULL, NULL);
	if (g_stg.cp_runner >= 0)
		async_runner_stop(g_stg.cp_runner);
	if (g_stg.pd_runner >= 0)
		async_runner_stop(g_stg.pd_runner);
	if (g_stg.cp_ctx)
		osdp_cp_teardown(g_stg.cp_ctx);
	if (g_stg.pd_ctx)
		osdp_pd_teardown(g_stg.pd_ctx);
	g_stg.cp_ctx = g_stg.pd_ctx = NULL;
	g_stg.cp_runner = g_stg.pd_runner = -1;
}

static void stg_reset_records(void)
{
	int i;

	for (i = 0; i < STG_LOG_MAX; i++)
		atomic_store(&g_stg.log[i], 0);
	atomic_store(&g_stg.log_len, 0);
	atomic_store(&g_stg.pd_busy_left, 0);
	atomic_store(&g_stg.pd_refuse, false);
	atomic_store(&g_stg.drop_next_ccrypt, false);
	atomic_store(&g_stg.ccrypt_dropped, 0);
	atomic_store(&g_stg.forge_scs13_keyset, false);
	atomic_store(&g_stg.forged_sent, false);
	atomic_store(&g_stg.forged_reply, -1);
	atomic_store(&g_stg.first_mac_cmd, -1);
	atomic_store(&g_stg.keyset_on_online_sent, false);
	test_completion_reset(&g_stg.completion);
}

/*
 * A pd_staged key is staged in the PD before it first runs: a PD that took
 * a KEYSET and is still up while the CP restarted.
 */
static int stg_start(const uint8_t *cp_main, const uint8_t *pd_main,
		     const uint8_t *pd_staged)
{
	stg_stop();
	stg_reset_records();
	if (test_setup_devices_keys(g_stg.t, &g_stg.cp_ctx, &g_stg.pd_ctx,
				    OSDP_FLAG_ENABLE_NOTIFICATION, 0, cp_main,
				    pd_main)) {
		printf(SUB_1 "Failed to setup devices!\n");
		g_stg.cp_ctx = g_stg.pd_ctx = NULL;
		return -1;
	}
	if (pd_staged)
		test_pd_stage_scbk(stg_pd(), pd_staged);
	osdp_cp_set_event_callback(g_stg.cp_ctx, stg_cp_event_cb, NULL);
	osdp_pd_set_command_callback(g_stg.pd_ctx, stg_pd_command_cb, NULL);
	osdp_cp_set_command_completion_callback(
		g_stg.cp_ctx, test_cmd_completion_cb, &g_stg.completion);
	test_set_channel_hook(stg_hook, NULL);

	g_stg.cp_runner = async_runner_start(g_stg.cp_ctx, osdp_cp_refresh);
	g_stg.pd_runner = async_runner_start(g_stg.pd_ctx, osdp_pd_refresh);
	if (g_stg.cp_runner < 0 || g_stg.pd_runner < 0) {
		printf(SUB_1 "Failed to create CP/PD runners\n");
		return -1;
	}
	return 0;
}

static bool stg_begin(struct test *t, const char *what)
{
	printf(SUB_1 "%s -- ", what);
	fflush(stdout);
	g_stg.t = t;
	g_stg.cp_runner = g_stg.pd_runner = -1;
	return true;
}

static bool stg_end(bool ok, const char *why)
{
	stg_stop();
	g_stg.keyset_on_online = NULL;
	if (ok)
		printf("success!\n");
	else
		printf("%s\n", why);
	return ok;
}

static bool pd_app_sees_keyset_on_proof(struct test *t)
{
	static const int expect[] = { 0xb0 };

	stg_begin(t, "the PD app sees a KEYSET once a session proves it");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	stg_submit_keyset(K2);
	if (!stg_wait_for_log(1, 10))
		return stg_end(false, "the app never saw the KEYSET");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K2 never came up");
	if (!stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "wrong KEYSETs");
	if (!stg_pd_settled_on(K2))
		return stg_end(false, "PD is not on K2");
	if (stg_start(K2, K2, NULL))
		return stg_end(false, "restart failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K2 never came up after a restart");
	return stg_end(true, NULL);
}

static bool pd_staged_attempt_survives_resent_chlng(struct test *t)
{
	static const int expect[] = { 0xb0 };

	stg_begin(t, "a resent CHLNG is the same attempt");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.drop_next_ccrypt, true);
	stg_submit_keyset(K2);
	if (!stg_wait_for_log(1, 10))
		return stg_end(false, "never confirmed");
	if (atomic_load(&g_stg.ccrypt_dropped) != 1)
		return stg_end(false, "no CCRYPT was dropped");
	if (!stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "wrong KEYSETs");
	return stg_end(true, NULL);
}

extern int test_pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len);

static void stg_decode_chlng(struct osdp_pd *pd, uint8_t rnd_a_fill)
{
	uint8_t buf[1 + 8];

	buf[0] = CMD_CHLNG;
	memset(buf + 1, rnd_a_fill, 8);
	test_pd_decode_command(pd, buf, sizeof(buf));
}

/* The RND.A rule without the timing of a live pair: two CHLNGs with one
 * RND.A, then a third with another. */
static bool pd_same_rnd_a_is_one_attempt(struct test *t)
{
	struct osdp_pd *pd;
	bool ok = false;

	stg_begin(t, "a CHLNG with the same RND.A is the same attempt");
	stg_reset_records();
	if (test_setup_devices_keys(t, &g_stg.cp_ctx, &g_stg.pd_ctx, 0, 0, K1,
				    K1) ||
	    osdp_pd_set_command_callback(g_stg.pd_ctx, stg_pd_command_cb, NULL))
		return stg_end(false, "setup failed");
	pd = stg_pd();
	test_pd_stage_scbk(pd, K2);

	stg_decode_chlng(pd, 0xa1);
	stg_decode_chlng(pd, 0xa1);
	if (!ISSET_FLAG(pd, PD_FLAG_SC_USE_STAGED))
		return stg_end(false, "a resent CHLNG ended the attempt");
	stg_decode_chlng(pd, 0xa2);
	ok = atomic_load(&g_stg.log_len) == 0 &&
	     !ISSET_FLAG(pd, PD_FLAG_SC_USE_STAGED) &&
	     !ISSET_FLAG(pd, PD_FLAG_SCBK_STAGED);
	return stg_end(ok, "a new RND.A did not revert");
}

static bool pd_reverts_after_failed_attempt(struct test *t)
{
	stg_begin(t, "PD reverts after the staged key's attempt fails");
	if (stg_start(K1, K1, K2))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	if (atomic_load(&g_stg.log_len) != 0) {
		stg_print_log();
		return stg_end(false, "the app saw a key that was not proven");
	}
	if (!stg_pd_settled_on(K1))
		return stg_end(false, "PD is not back on K1");
	return stg_end(true, NULL);
}

/* The app answers the proving session's first command with BUSY while it
 * persists the key; the PD switches only once it accepts. */
static bool pd_busy_defers_confirm(struct test *t)
{
	static const int expect[] = { 0xb0, 0xb0, 0xb0 };

	stg_begin(t, "a busy PD app defers the confirm");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.pd_busy_left, 2);
	stg_submit_keyset(K2);
	if (!stg_wait_for_log(3, 10))
		return stg_end(false, "the app was not asked again after BUSY");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K2 never came up");
	if (!stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "wrong KEYSETs");
	if (!stg_pd_settled_on(K2))
		return stg_end(false, "PD is not on K2");
	return stg_end(true, NULL);
}

/*
 * The app refuses the proven key: the PD stays on its main key. This CP
 * switched on the ACK and holds only K2, so the link does not come back;
 * a CP that stages falls back to K1.
 */
static bool pd_refusal_keeps_main(struct test *t)
{
	static const int expect[] = { 0xb0 };

	stg_begin(t, "a PD app refusing the proven key keeps the main key");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.pd_refuse, true);
	stg_submit_keyset(K2);
	if (!stg_wait_for_log(1, 10))
		return stg_end(false, "the app never saw the KEYSET");
	usleep(500 * 1000);
	if (!stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "the app was asked more than once");
	if (!stg_pd_settled_on(K1))
		return stg_end(false, "PD left K1");
	return stg_end(true, NULL);
}

static bool pd_confirms_before_dispatching_first_command(struct test *t)
{
	static const int expect[] = { 0xb0, 0xc0 };

	stg_begin(t, "PD confirms before dispatching the first command");
	/* Queued from the CP's online notification, which follows the
	 * handshake, the KEYSET is the first command of the session. */
	g_stg.keyset_on_online = K3;
	if (stg_start(K2, K1, K2))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_log(2, 10))
		return stg_end(false, "never confirmed both keys");
	if (!atomic_load(&g_stg.keyset_on_online_sent))
		return stg_end(false, "KEYSET was never sent");
	if (atomic_load(&g_stg.first_mac_cmd) != CMD_KEYSET)
		return stg_end(false, "KEYSET was not the first command");
	if (!stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "wrong order");
	return stg_end(true, NULL);
}

/*
 * SCS_11..14 carry no MAC. If the PD honoured one in an active session for
 * anything but the handshake message it belongs to, an attacker with no key
 * could have a KEYSET staged and then play CP to confirm it.
 */
static bool pd_naks_keyset_under_handshake_scs(struct test *t)
{
	int i;

	stg_begin(t, "a KEYSET under SCS_13 in an active session is NAKed");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.forge_scs13_keyset, true);
	for (i = 0; i < 50 && atomic_load(&g_stg.forged_reply) == -1; i++)
		usleep(100 * 1000);
	if (!atomic_load(&g_stg.forged_sent))
		return stg_end(false, "no POLL was replaced");
	if (atomic_load(&g_stg.forged_reply) != REPLY_NAK)
		return stg_end(false, "the forged KEYSET was not NAKed");
	if (atomic_load(&g_stg.log_len) != 0 ||
	    ISSET_FLAG(stg_pd(), PD_FLAG_SCBK_STAGED)) {
		stg_print_log();
		return stg_end(false, "the forged KEYSET was staged");
	}
	return stg_end(true, NULL);
}

void run_scbk_staging_tests(struct test *t)
{
	printf("\nStarting SCBK staging tests\n");

	TEST_CASE(t, "pd_app_sees_keyset_on_proof",
		  pd_app_sees_keyset_on_proof(t));
	TEST_CASE(t, "pd_staged_attempt_survives_resent_chlng",
		  pd_staged_attempt_survives_resent_chlng(t));
	TEST_CASE(t, "pd_same_rnd_a_is_one_attempt",
		  pd_same_rnd_a_is_one_attempt(t));
	TEST_CASE(t, "pd_reverts_after_failed_attempt",
		  pd_reverts_after_failed_attempt(t));
	TEST_CASE(t, "pd_busy_defers_confirm", pd_busy_defers_confirm(t));
	TEST_CASE(t, "pd_refusal_keeps_main", pd_refusal_keeps_main(t));
	TEST_CASE(t, "pd_confirms_before_dispatching_first_command",
		  pd_confirms_before_dispatching_first_command(t));
	TEST_CASE(t, "pd_naks_keyset_under_handshake_scs",
		  pd_naks_keyset_under_handshake_scs(t));
}
