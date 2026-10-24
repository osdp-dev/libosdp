/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "test.h"

extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);

/*
 * SCBK staging. A KEYSET on a real-key session is held beside the main key
 * on both sides until a session on it has carried a MAC-verified message.
 * The PD app sees the KEYSET only then; the CP app's KEYSET completes only
 * then, OK if the new key is in use and FAILED if the main key stays.
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

	/* Drop the next REPLY_CCRYPT the PD sends, or all of them. */
	atomic_bool drop_next_ccrypt;
	atomic_bool drop_all_ccrypt;
	atomic_int ccrypt_dropped;

	/* Replace the CP's next MAC'd POLL with a plaintext KEYSET for K2
	 * under SCS_13, as an on-path attacker without the key could. */
	atomic_bool forge_scs13_keyset;
	atomic_bool forged_sent;
	/* The PD's reply id to the forged frame; -1 until it is seen. */
	atomic_int forged_reply;

	/* Command id of the last frame the CP sent. */
	atomic_int last_cmd;
	/* Drop the PD's next ACK to a KEYSET. */
	atomic_bool drop_keyset_ack;
	/* Drop the PD's first SCS_16/SCS_18 reply after a KEYSET ACK. */
	atomic_bool drop_first_mac_reply;
	atomic_bool mac_reply_armed;
	/* Frames dropped by the two rules above. */
	atomic_int replies_dropped;
	/* Queue an LED, then a KEYSET for this key, while a POLL is
	 * outstanding and disable the PD, so the next session starts with
	 * both still queued. */
	const uint8_t *keyset_before_restart;
	atomic_bool force_restart;
	atomic_bool restart_forced;

	/* Command id of the first MAC'd command the CP sent, past the
	 * osdp_ACURXSIZE that opens every link; -1 for none. */
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

static struct osdp_pd *stg_cp_pd(void)
{
	return osdp_to_pd(g_stg.cp_ctx, 0);
}

/* `pd` runs on `key` as its main key and holds nothing staged. */
static bool stg_settled_on(struct osdp_pd *pd, const uint8_t *key)
{
	return memcmp(pd->sc.scbk, key, 16) == 0 &&
	       !ISSET_FLAG(pd, PD_FLAG_SCBK_STAGED);
}

/* The CP app got its KEYSET back exactly once, with `status`. */
static bool stg_keyset_completed(enum osdp_completion_status status,
				 int timeout_sec)
{
	if (!test_completion_wait(&g_stg.completion, 1, timeout_sec))
		return false;
	usleep(200 * 1000);
	return test_completion_count(&g_stg.completion) == 1 &&
	       test_completion_first_id(&g_stg.completion) == OSDP_CMD_KEYSET &&
	       test_completion_first_status(&g_stg.completion) == (int)status;
}

static enum test_channel_hook_verdict stg_hook_reply(int id, int scs_type)
{
	if (id == REPLY_ACK && atomic_load(&g_stg.last_cmd) == CMD_KEYSET) {
		if (atomic_exchange(&g_stg.drop_keyset_ack, false)) {
			atomic_fetch_add(&g_stg.replies_dropped, 1);
			return TEST_HOOK_DROP;
		}
		if (atomic_load(&g_stg.drop_first_mac_reply))
			atomic_store(&g_stg.mac_reply_armed, true);
		return TEST_HOOK_PASS;
	}
	if ((scs_type == 0x16 || scs_type == 0x18) &&
	    atomic_exchange(&g_stg.mac_reply_armed, false)) {
		atomic_store(&g_stg.drop_first_mac_reply, false);
		atomic_fetch_add(&g_stg.replies_dropped, 1);
		return TEST_HOOK_DROP;
	}
	if (id == REPLY_CCRYPT &&
	    (atomic_load(&g_stg.drop_all_ccrypt) ||
	     atomic_exchange(&g_stg.drop_next_ccrypt, false))) {
		atomic_fetch_add(&g_stg.ccrypt_dropped, 1);
		return TEST_HOOK_DROP;
	}
	return TEST_HOOK_PASS;
}

static bool stg_try_keyset(const uint8_t *key)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_KEYSET,
		.keyset = { .type = 1, .length = 16 },
	};

	memcpy(cmd.keyset.data, key, 16);
	return test_submit_command(g_stg.cp_ctx, 0, &cmd) != NULL;
}

static void stg_submit_keyset(const uint8_t *key)
{
	if (!stg_try_keyset(key))
		printf(SUB_1 "KEYSET submit failed\n");
}

static bool stg_try_led(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_LED,
		.led = {
			.temporary = {
				.control_code = 1,
				.on_count = 10,
				.off_count = 10,
				.on_color = OSDP_LED_COLOR_RED,
				.timer_count = 100,
			},
		},
	};

	return test_submit_command(g_stg.cp_ctx, 0, &cmd) != NULL;
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

		atomic_store(&g_stg.last_cmd, frame[id_off]);
		if ((scs_type == 0x15 || scs_type == 0x17) &&
		    frame[id_off] == CMD_POLL &&
		    atomic_load(&g_stg.forge_scs13_keyset) &&
		    !atomic_exchange(&g_stg.forged_sent, true)) {
			*out_len = stg_frame_scs13_keyset(frame, base, out,
							  out_max);
			if (*out_len > 0)
				return TEST_HOOK_REPLACE;
		}
		if ((scs_type == 0x15 || scs_type == 0x17) &&
		    frame[id_off] != CMD_ACURXSIZE)
			atomic_compare_exchange_strong(&g_stg.first_mac_cmd,
						       &expect_none,
						       frame[id_off]);
		return TEST_HOOK_PASS;
	}
	if (atomic_load(&g_stg.forged_sent) &&
	    atomic_load(&g_stg.forged_reply) == -1)
		atomic_store(&g_stg.forged_reply, frame[id_off]);
	/* The PD refresh holds the (recursive) API lock here, so the CP is
	 * parked waiting for this reply: the LED and the KEYSET behind it
	 * stay queued, and the PD is disabled once the reply is in. */
	if (atomic_load(&g_stg.last_cmd) == CMD_POLL &&
	    atomic_exchange(&g_stg.force_restart, false) && stg_try_led() &&
	    stg_try_keyset(g_stg.keyset_before_restart) &&
	    osdp_cp_disable_pd(g_stg.cp_ctx, 0) == 0) {
		atomic_store(&g_stg.restart_forced, true);
	}
	return stg_hook_reply(frame[id_off], scs_type);
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
	atomic_store(&g_stg.drop_all_ccrypt, false);
	atomic_store(&g_stg.ccrypt_dropped, 0);
	atomic_store(&g_stg.forge_scs13_keyset, false);
	atomic_store(&g_stg.forged_sent, false);
	atomic_store(&g_stg.forged_reply, -1);
	atomic_store(&g_stg.last_cmd, -1);
	atomic_store(&g_stg.drop_keyset_ack, false);
	atomic_store(&g_stg.drop_first_mac_reply, false);
	atomic_store(&g_stg.mac_reply_armed, false);
	atomic_store(&g_stg.replies_dropped, 0);
	atomic_store(&g_stg.force_restart, false);
	atomic_store(&g_stg.restart_forced, false);
	atomic_store(&g_stg.first_mac_cmd, -1);
	atomic_store(&g_stg.keyset_on_online_sent, false);
	test_completion_reset(&g_stg.completion);
}

/*
 * A pd_staged key is staged in the PD before it first runs: a PD that took
 * a KEYSET and is still up while the CP restarted.
 */
static int stg_start_ext(uint32_t pd_flags, const uint8_t *cp_main,
			 const uint8_t *pd_main, const uint8_t *pd_staged)
{
	stg_stop();
	stg_reset_records();
	if (test_setup_devices_keys(g_stg.t, &g_stg.cp_ctx, &g_stg.pd_ctx,
				    OSDP_FLAG_ENABLE_NOTIFICATION, pd_flags,
				    cp_main, pd_main)) {
		printf(SUB_1 "Failed to setup devices!\n");
		g_stg.cp_ctx = g_stg.pd_ctx = NULL;
		return -1;
	}
	if (pd_staged)
		osdp_stage_scbk(stg_pd(), pd_staged);
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

static int stg_start(const uint8_t *cp_main, const uint8_t *pd_main,
		     const uint8_t *pd_staged)
{
	return stg_start_ext(0, cp_main, pd_main, pd_staged);
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

/* A K1 pair, secure and online, rotating to K2. */
static bool stg_rotate_k1_to_k2(void)
{
	if (stg_start(K1, K1, NULL) || !stg_wait_for_secure_online(10))
		return false;
	stg_submit_keyset(K2);
	return true;
}

static bool rotation_completes_on_proof(struct test *t)
{
	static const int expect[] = { 0xb0 };

	stg_begin(t, "a rotation completes once a session proves the key");
	if (!stg_rotate_k1_to_k2())
		return stg_end(false, "K1 never came up");
	if (!stg_keyset_completed(OSDP_COMPLETION_OK, 10))
		return stg_end(false, "KEYSET did not complete OK once");
	/* The PD confirms before its first MAC'd reply on K2 goes out, and
	 * that reply is what completes the CP's KEYSET. */
	if (!stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "the PD app did not see K2 first");
	if (!stg_settled_on(stg_cp_pd(), K2) || !stg_settled_on(stg_pd(), K2))
		return stg_end(false, "both sides are not on K2");
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
	if (!stg_keyset_completed(OSDP_COMPLETION_OK, 10))
		return stg_end(false, "KEYSET did not complete OK");
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
	osdp_stage_scbk(pd, K2);

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

/* The CP restarted before the KEYSET settled and came back on K1 alone. */
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
	if (!stg_settled_on(stg_pd(), K1))
		return stg_end(false, "PD is not back on K1");
	return stg_end(true, NULL);
}

/* The CP restarted after the PD confirmed and came back on K2: the PD
 * still holds it staged, and its one attempt succeeds. */
static bool pd_confirms_for_a_restarted_cp(struct test *t)
{
	static const int expect[] = { 0xb0 };

	stg_begin(t, "a CP restarted on the staged key is confirmed");
	if (stg_start(K2, K1, K2))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K2 never came up");
	if (!stg_wait_for_log(1, 5) ||
	    !stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "the PD app never saw K2");
	if (!stg_settled_on(stg_pd(), K2))
		return stg_end(false, "PD is not on K2");
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
	if (!stg_keyset_completed(OSDP_COMPLETION_OK, 10))
		return stg_end(false, "KEYSET did not complete OK");
	if (!stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "wrong KEYSETs");
	if (!stg_settled_on(stg_pd(), K2) || !stg_settled_on(stg_cp_pd(), K2))
		return stg_end(false, "both sides are not on K2");
	return stg_end(true, NULL);
}

/* The PD app refuses the proven key: both sides fall back to K1 and the
 * CP's KEYSET fails. */
static bool pd_refusal_reverts_both(struct test *t)
{
	static const int expect[] = { 0xb0 };

	stg_begin(t, "a PD app refusing the proven key reverts both sides");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.pd_refuse, true);
	stg_submit_keyset(K2);
	/* Well inside the offline dwell a hard failure would otherwise
	 * cost. */
	if (!stg_keyset_completed(OSDP_COMPLETION_FAILED, 5))
		return stg_end(false, "KEYSET did not fail within 5s");
	if (!stg_log_is(expect, ARRAY_SIZEOF(expect)))
		return stg_end(false, "the app was asked more than once");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came back");
	if (!stg_settled_on(stg_pd(), K1) || !stg_settled_on(stg_cp_pd(), K1))
		return stg_end(false, "both sides are not on K1");
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

/* The PD staged the key; a NAK or timeout on the KEYSET does not show
 * that it did not. */
static bool lost_ack_still_confirms(struct test *t)
{
	stg_begin(t, "a lost KEYSET ACK still confirms on both sides");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.drop_keyset_ack, true);
	stg_submit_keyset(K2);
	if (!stg_keyset_completed(OSDP_COMPLETION_OK, 10))
		return stg_end(false, "KEYSET did not complete OK");
	if (atomic_load(&g_stg.replies_dropped) != 1)
		return stg_end(false, "no ACK was dropped");
	if (!stg_settled_on(stg_pd(), K2) || !stg_settled_on(stg_cp_pd(), K2))
		return stg_end(false, "both sides are not on K2");
	return stg_end(true, NULL);
}

/* The PD confirmed, but the reply that would have told the CP was lost. */
static bool first_reply_lost_still_confirms(struct test *t)
{
	stg_begin(t, "the first verified reply lost still confirms");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.drop_first_mac_reply, true);
	stg_submit_keyset(K2);
	if (!stg_keyset_completed(OSDP_COMPLETION_OK, 10))
		return stg_end(false, "KEYSET did not complete OK");
	if (atomic_load(&g_stg.replies_dropped) != 1)
		return stg_end(false, "no reply was dropped");
	if (!stg_settled_on(stg_cp_pd(), K2))
		return stg_end(false, "CP is not on K2");
	return stg_end(true, NULL);
}

/*
 * The KEYSET goes out, behind an LED, in a session whose handshake already
 * tried the staged key and matched main. Neither the LED's ACK nor the
 * KEYSET's may revert the key that KEYSET carries.
 */
static bool queued_keyset_survives_restart(struct test *t)
{
	int i;

	stg_begin(t, "a KEYSET queued across a restart still confirms");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	g_stg.keyset_before_restart = K2;
	atomic_store(&g_stg.force_restart, true);
	for (i = 0; i < 50 && osdp_cp_is_pd_enabled(g_stg.cp_ctx, 0); i++)
		usleep(100 * 1000);
	if (!atomic_load(&g_stg.restart_forced) ||
	    osdp_cp_enable_pd(g_stg.cp_ctx, 0))
		return stg_end(false,
			       "PD was never disabled with KEYSET queued");
	if (!test_completion_wait(&g_stg.completion, 2, 10))
		return stg_end(false, "LED and KEYSET never completed");
	if (test_completion_status(&g_stg.completion) != OSDP_COMPLETION_OK)
		return stg_end(false, "KEYSET did not complete OK");
	if (!stg_settled_on(stg_pd(), K2) || !stg_settled_on(stg_cp_pd(), K2))
		return stg_end(false, "both sides are not on K2");
	return stg_end(true, NULL);
}

static bool cp_refuses_keyset_while_staged(struct test *t)
{
	stg_begin(t, "CP refuses a KEYSET while a key is staged");
	if (!stg_rotate_k1_to_k2())
		return stg_end(false, "K1 never came up");
	if (stg_try_keyset(K3))
		return stg_end(false, "second KEYSET accepted while staged");
	if (!stg_keyset_completed(OSDP_COMPLETION_OK, 10))
		return stg_end(false, "K2 never confirmed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K2 session never came up");
	test_completion_reset(&g_stg.completion);
	if (!stg_try_keyset(K3))
		return stg_end(false, "KEYSET refused after K2 confirmed");
	if (!stg_keyset_completed(OSDP_COMPLETION_OK, 10))
		return stg_end(false, "K3 never confirmed");
	if (!stg_settled_on(stg_cp_pd(), K3))
		return stg_end(false, "CP is not on K3");
	return stg_end(true, NULL);
}

/* A KEYSET held while the PD is out of service stays held, and settles
 * once the PD is back. The dropped CCRYPTs used up the staged key's one
 * attempt, so it settles on the main key. */
static bool held_keyset_survives_disable(struct test *t)
{
	int i;

	stg_begin(t, "a held KEYSET survives disabling the PD");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.drop_all_ccrypt, true);
	stg_submit_keyset(K2);
	for (i = 0; i < 50 && !stg_cp_pd()->keyset_cmd; i++)
		usleep(100 * 1000);
	if (!stg_cp_pd()->keyset_cmd)
		return stg_end(false, "KEYSET was never held");
	if (osdp_cp_disable_pd(g_stg.cp_ctx, 0))
		return stg_end(false, "disable failed");
	/* Disabling waits out the CHLNG retries the dropped CCRYPTs cause. */
	for (i = 0; i < 150 && osdp_cp_is_pd_enabled(g_stg.cp_ctx, 0); i++)
		usleep(100 * 1000);
	if (test_completion_count(&g_stg.completion) != 0)
		return stg_end(false, "disabling the PD completed the KEYSET");
	atomic_store(&g_stg.drop_all_ccrypt, false);
	if (osdp_cp_enable_pd(g_stg.cp_ctx, 0))
		return stg_end(false, "enable failed");
	if (!stg_keyset_completed(OSDP_COMPLETION_FAILED, 10))
		return stg_end(false, "KEYSET did not fail");
	if (!stg_settled_on(stg_pd(), K1) || !stg_settled_on(stg_cp_pd(), K1))
		return stg_end(false, "both sides are not on K1");
	return stg_end(true, NULL);
}

/* Teardown hands a held KEYSET back unsettled: the app cannot tell which
 * key the PD holds and keeps both. */
static bool teardown_aborts_held_keyset(struct test *t)
{
	int i;

	stg_begin(t, "teardown aborts a held KEYSET");
	if (stg_start(K1, K1, NULL))
		return stg_end(false, "setup failed");
	if (!stg_wait_for_secure_online(10))
		return stg_end(false, "K1 never came up");
	atomic_store(&g_stg.drop_all_ccrypt, true);
	stg_submit_keyset(K2);
	for (i = 0; i < 50 && !stg_cp_pd()->keyset_cmd; i++)
		usleep(100 * 1000);
	if (!stg_cp_pd()->keyset_cmd)
		return stg_end(false, "KEYSET was never held");
	async_runner_stop(g_stg.cp_runner);
	g_stg.cp_runner = -1;
	if (test_completion_count(&g_stg.completion) != 0)
		return stg_end(false, "KEYSET completed before teardown");
	osdp_cp_teardown(g_stg.cp_ctx);
	g_stg.cp_ctx = NULL;
	if (test_completion_count(&g_stg.completion) != 1 ||
	    test_completion_first_status(&g_stg.completion) !=
		    OSDP_COMPLETION_ABORTED)
		return stg_end(false, "KEYSET was not aborted");
	return stg_end(true, NULL);
}

/*
 * An install-mode session on SCBK-D ends in the CP's own KEYSET for K2,
 * which the PD app takes on arrival and the PD commits on the ACK.
 */
static bool stg_install_k2(const uint8_t *pd_main, const uint8_t *pd_staged)
{
	static const int expect[] = { 0xb0 };

	if (stg_start_ext(OSDP_FLAG_INSTALL_MODE, K2, pd_main, pd_staged))
		return false;
	/* Only a real-key session goes online: an SCBK-D one ends in the
	 * KEYSET, and K2 is the only real key the CP has. */
	if (!stg_wait_for_secure_online(10))
		return false;
	return stg_log_is(expect, ARRAY_SIZEOF(expect)) &&
	       stg_settled_on(stg_pd(), K2);
}

static bool install_mode_keyset_commits_directly(struct test *t)
{
	stg_begin(t, "an install-mode KEYSET commits directly");
	if (!stg_install_k2(NULL, NULL))
		return stg_end(false, "keyless PD did not commit K2");

	/* A staged key is no fallback once install mode keyed the PD: K1 is
	 * the PD's main key and K3 its staged one, neither held by the CP,
	 * which falls back to SCBK-D. */
	if (!stg_install_k2(K1, K3))
		return stg_end(false, "keyed PD did not commit K2");
	return stg_end(true, NULL);
}

void run_scbk_staging_tests(struct test *t)
{
	printf("\nStarting SCBK staging tests\n");

	TEST_CASE(t, "rotation_completes_on_proof",
		  rotation_completes_on_proof(t));
	TEST_CASE(t, "pd_staged_attempt_survives_resent_chlng",
		  pd_staged_attempt_survives_resent_chlng(t));
	TEST_CASE(t, "pd_same_rnd_a_is_one_attempt",
		  pd_same_rnd_a_is_one_attempt(t));
	TEST_CASE(t, "pd_reverts_after_failed_attempt",
		  pd_reverts_after_failed_attempt(t));
	TEST_CASE(t, "pd_confirms_for_a_restarted_cp",
		  pd_confirms_for_a_restarted_cp(t));
	TEST_CASE(t, "pd_busy_defers_confirm", pd_busy_defers_confirm(t));
	TEST_CASE(t, "pd_refusal_reverts_both", pd_refusal_reverts_both(t));
	TEST_CASE(t, "pd_confirms_before_dispatching_first_command",
		  pd_confirms_before_dispatching_first_command(t));
	TEST_CASE(t, "pd_naks_keyset_under_handshake_scs",
		  pd_naks_keyset_under_handshake_scs(t));
	TEST_CASE(t, "lost_ack_still_confirms", lost_ack_still_confirms(t));
	TEST_CASE(t, "first_reply_lost_still_confirms",
		  first_reply_lost_still_confirms(t));
	TEST_CASE(t, "queued_keyset_survives_restart",
		  queued_keyset_survives_restart(t));
	TEST_CASE(t, "cp_refuses_keyset_while_staged",
		  cp_refuses_keyset_while_staged(t));
	TEST_CASE(t, "held_keyset_survives_disable",
		  held_keyset_survives_disable(t));
	TEST_CASE(t, "teardown_aborts_held_keyset",
		  teardown_aborts_held_keyset(t));
	TEST_CASE(t, "install_mode_keyset_commits_directly",
		  install_mode_keyset_commits_directly(t));
}
