/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file Structural fuzz of the frame codecs. cp_decode_response() and
 * pd_decode_command() are handed a raw payload (an id byte followed by data)
 * that the framer has already length- and checksum-validated but not otherwise
 * interpreted; they then walk it field by field. This is where an over-read
 * would hide, so the goal is to get *past* each message's length gate and run
 * the field walk, not just bounce off the gate.
 *
 * Two things make this more than a random-byte hose:
 *
 *  - It sweeps the payload length from 1..FUZZ_SWEEP_MAX for every id, so each
 *    gate -- whether "len == N", "len % N == 0" or "len >= min" -- is satisfied
 *    for some trial and the decode past it actually executes. Random-length
 *    trials on top reach multi-entry payloads and oversized variable bodies.
 *
 *  - It sets the little bit of PD state a few gates demand: SC is cleared before
 *    every call (so the SC replies/commands take their decode path instead of
 *    the "out of order" bail), REPLY_CCRYPT/REPLY_RMAC_I are told which command
 *    they answer, and two capability item-counts are seeded so the status-report
 *    replies decode instead of failing their length-vs-caps check.
 *
 * Each payload lives in its own exact-size heap allocation, so a read at or past
 * buf[len] trips AddressSanitizer at once. The bytes come from rand(), which
 * the harness seeds per case; re-run with the printed OSDP_TEST_SEED to replay
 * a failing run byte-for-byte.
 */

#include "test.h"

/*
 * Frame codecs, exported as test_<fn> aliases under UNIT_TESTING via
 * OSDP_TEST_ALIAS. No public header declares them, so forward-declare here.
 */
extern int test_cp_decode_response(struct osdp_pd *pd, uint8_t *buf, int len);
extern int test_pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len);

#define FUZZ_SWEEP_MAX      40  /* > every fixed gate; CCRYPT is 32 data + id */
#define FUZZ_RANDOM_MAX     128
#define FUZZ_TRIALS_PER_LEN 8
#define FUZZ_RANDOM_TRIALS  256

/* Decode one exact-size payload: id in buf[0], the remaining bytes random. */
static void fuzz_call(int (*decode)(struct osdp_pd *, uint8_t *, int),
		      struct osdp_pd *pd, uint8_t id, int len)
{
	uint8_t *buf = malloc(len);
	int i;

	if (buf == NULL) {
		return;
	}
	buf[0] = id;
	for (i = 1; i < len; i++) {
		buf[i] = (uint8_t)rand();
	}
	(void)decode(pd, buf, len);
	free(buf);
}

static int test_codec_fuzz_pd_decode_command(struct osdp *pd_ctx)
{
	struct osdp_pd *pd = osdp_to_pd(pd_ctx, 0);
	int id, len, t;

	printf(SUB_1 "Fuzzing pd_decode_command() over every id and length -- ");
	for (id = 0; id <= 0xff; id++) {
		for (len = 1; len <= FUZZ_SWEEP_MAX; len++) {
			for (t = 0; t < FUZZ_TRIALS_PER_LEN; t++) {
				sc_deactivate(pd);
				fuzz_call(test_pd_decode_command, pd,
					  (uint8_t)id, len);
			}
		}
		for (t = 0; t < FUZZ_RANDOM_TRIALS; t++) {
			sc_deactivate(pd);
			len = 1 + rand() % FUZZ_RANDOM_MAX;
			fuzz_call(test_pd_decode_command, pd, (uint8_t)id, len);
		}
	}
	printf("success!\n");
	return 0;
}

/* Put the CP-side PD into the state a couple of reply gates check for. */
static void cp_prep(struct osdp_pd *pd, uint8_t reply_id)
{
	sc_deactivate(pd);
	/* REPLY_CCRYPT/REPLY_RMAC_I bail unless they answer the matching cmd. */
	pd->cmd_id = (reply_id == REPLY_CCRYPT) ? CMD_CHLNG :
		     (reply_id == REPLY_RMAC_I) ? CMD_SCRYPT : CMD_POLL;
	/* Status-report replies gate on len == cap.num_items; seed two counts so
	 * the sweep reaches their decode loop. Re-seeded every call because a
	 * prior REPLY_PDCAP trial may have overwritten the cap table. */
	pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL].function_code =
		OSDP_PD_CAP_OUTPUT_CONTROL;
	pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL].num_items = 8;
	pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING].function_code =
		OSDP_PD_CAP_CONTACT_STATUS_MONITORING;
	pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING].num_items = 8;
}

static int test_codec_fuzz_cp_decode_response(struct osdp *cp_ctx)
{
	struct osdp_pd *pd = osdp_to_pd(cp_ctx, 0);
	int id, len, t;

	printf(SUB_1 "Fuzzing cp_decode_response() over every id and length -- ");
	for (id = 0; id <= 0xff; id++) {
		for (len = 1; len <= FUZZ_SWEEP_MAX; len++) {
			for (t = 0; t < FUZZ_TRIALS_PER_LEN; t++) {
				cp_prep(pd, (uint8_t)id);
				fuzz_call(test_cp_decode_response, pd,
					  (uint8_t)id, len);
			}
		}
		for (t = 0; t < FUZZ_RANDOM_TRIALS; t++) {
			cp_prep(pd, (uint8_t)id);
			len = 1 + rand() % FUZZ_RANDOM_MAX;
			fuzz_call(test_cp_decode_response, pd, (uint8_t)id, len);
		}
	}
	printf("success!\n");
	return 0;
}

void run_codec_fuzz_tests(struct test *t)
{
	osdp_t *cp = NULL, *pd = NULL;

	printf("\nStarting codec_fuzz tests\n");

	if (test_setup_devices(t, &cp, &pd)) {
		printf(SUB_1 "device setup failed!\n");
		return;
	}

	/*
	 * Every garbage id the sweep feeds in draws an expected "Unknown CMD"
	 * error from the decoder -- ~150k of them per pass. That is the point
	 * of the fuzz, not a signal, so gag everything below CRIT while it runs
	 * (a real crash-adjacent log still gets through) and restore after. The
	 * threshold lives on each context (copied from the global default at
	 * setup), so poke the two contexts under test directly.
	 */
#ifndef OPT_OSDP_LOG_MINIMAL
	int pd_level = TO_OSDP(pd)->logger.log_level;
	int cp_level = TO_OSDP(cp)->logger.log_level;

	TO_OSDP(pd)->logger.log_level = OSDP_LOG_CRIT;
	TO_OSDP(cp)->logger.log_level = OSDP_LOG_CRIT;
#endif

	t->mock_data = pd;
	DO_TEST(t, test_codec_fuzz_pd_decode_command);
	t->mock_data = cp;
	DO_TEST(t, test_codec_fuzz_cp_decode_response);

#ifndef OPT_OSDP_LOG_MINIMAL
	TO_OSDP(pd)->logger.log_level = pd_level;
	TO_OSDP(cp)->logger.log_level = cp_level;
#endif

	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd);
	t->mock_data = NULL;
}
