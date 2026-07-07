/*
 * Copyright (c) 2026 Siddharth Chandrasekaran
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "osdp_common.h"
#include "test.h"

/*
 * Source:
 * SIA OSDP specification, Annex E ("Examples"), including CRC/checksum
 * and secure-channel (SCBK-D/session key/cryptogram/R-MAC-I) vectors.
 */

static uint8_t ref_checksum(const uint8_t *msg, int length)
{
	uint8_t checksum = 0;
	int i, whole_checksum = 0;

	for (i = 0; i < length; i++) {
		whole_checksum += msg[i];
		checksum = ~(0xff & whole_checksum) + 1;
	}
	return checksum;
}

static void u16_to_le_bytes(uint16_t val, uint8_t out[2])
{
	out[0] = BYTE_0(val);
	out[1] = BYTE_1(val);
}

static int check_array(const uint8_t *found, int found_len, const uint8_t *exp,
		       int exp_len, const char *name)
{
	if (found_len != exp_len || memcmp(found, exp, exp_len) != 0) {
		printf("error! %s mismatch\n", name);
		hexdump(exp, exp_len, SUB_1 "Expected");
		hexdump(found, found_len, SUB_1 "Found");
		return -1;
	}
	return 0;
}

static int test_crc_vectors(void *data)
{
	ARG_UNUSED(data);

	/* Annex E / CRC Example 1 */
	static const uint8_t msg1[] = {
		0x53, 0x7F, 0x0D, 0x00, 0x04, 0x6E, 0x00, 0x80,
		0x25, 0x00, 0x00
	};
	static const uint8_t exp1[] = { 0x6E, 0x38 };

	/* Annex E / CRC Example 2 */
	static const uint8_t msg2[] = {
		0x53, 0x00, 0x09, 0x00, 0x04, 0x61, 0x00
	};
	static const uint8_t exp2[] = { 0xC0, 0x66 };

	uint8_t got[2];
	uint16_t crc;

	crc = osdp_compute_crc16(msg1, sizeof(msg1));
	u16_to_le_bytes(crc, got);
	if (check_array(got, sizeof(got), exp1, sizeof(exp1), "crc vector 1")) {
		return -1;
	}

	crc = osdp_compute_crc16(msg2, sizeof(msg2));
	u16_to_le_bytes(crc, got);
	if (check_array(got, sizeof(got), exp2, sizeof(exp2), "crc vector 2")) {
		return -1;
	}

	return 0;
}

static int test_checksum_vectors(void *data)
{
	ARG_UNUSED(data);

	/* Annex E / Checksum Example 1 */
	static const uint8_t msg1[] = {
		0x53, 0x7F, 0x0C, 0x00, 0x00, 0x6E, 0x00, 0x80,
		0x25, 0x00, 0x00
	};

	/* Annex E / Checksum Example 2 */
	static const uint8_t msg2[] = {
		0x53, 0x00, 0x08, 0x00, 0x00, 0x61, 0x00
	};

	if (ref_checksum(msg1, sizeof(msg1)) != 0x0F) {
		printf("error! checksum example 1 mismatch\n");
		return -1;
	}

	if (ref_checksum(msg2, sizeof(msg2)) != 0x44) {
		printf("error! checksum example 2 mismatch\n");
		return -1;
	}

	return 0;
}

static int test_scbkd_vectors(void *data)
{
	ARG_UNUSED(data);

	struct osdp_pd pd;

	static const uint8_t scbk_d[16] = {
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
		0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
	};

	/* ACU random / host random from Annex E */
	static const uint8_t cp_random[8] = {
		0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7
	};

	/* PD random from Annex E */
	static const uint8_t pd_random[8] = {
		0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7
	};

	static const uint8_t exp_smac1[16] = {
		0x5E, 0x86, 0xC6, 0x76, 0x60, 0x3B, 0xDE, 0xE2,
		0xD8, 0xBE, 0xAF, 0xE1, 0x78, 0x63, 0x73, 0x32
	};

	static const uint8_t exp_smac2[16] = {
		0x6F, 0xDA, 0x86, 0xE8, 0x57, 0x77, 0x7E, 0x81,
		0x13, 0x20, 0x35, 0x75, 0x82, 0x39, 0x17, 0x2E
	};

	static const uint8_t exp_senc[16] = {
		0xBF, 0x8D, 0xC2, 0xA8, 0x32, 0x9A, 0xCB, 0x8C,
		0x67, 0xC6, 0xD0, 0xCD, 0x9A, 0x45, 0x16, 0x82
	};

	/* "host cryptogram" in spec == cp_cryptogram in libosdp */
	static const uint8_t exp_cp_cryptogram[16] = {
		0x26, 0xD3, 0x35, 0x6E, 0x07, 0x76, 0x2D, 0x26,
		0x28, 0x01, 0xFC, 0x8E, 0x66, 0x65, 0xA8, 0x91
	};

	/* "client cryptogram" in spec == pd_cryptogram in libosdp */
	static const uint8_t exp_pd_cryptogram[16] = {
		0xFD, 0xE5, 0xD2, 0xF4, 0x28, 0xEC, 0x16, 0x31,
		0x24, 0x71, 0xEA, 0x3C, 0x02, 0xBD, 0x77, 0x96
	};

	static const uint8_t exp_rmac_i[16] = {
		0xB2, 0xA3, 0x00, 0x57, 0xEB, 0x98, 0xBA, 0x22,
		0x29, 0xEC, 0x1F, 0x87, 0x56, 0x62, 0xB5, 0x24
	};

	memset(&pd, 0, sizeof(pd));
	memcpy(pd.sc.scbk, scbk_d, sizeof(scbk_d));

	/*
	 * osdp_sc_setup() initializes crypto backend and preserves scbk.
	 * It may seed cp_random in CP mode, so overwrite with Annex E values
	 * after.
	 */
	osdp_sc_setup(&pd);
	memcpy(pd.sc.cp_random, cp_random, sizeof(cp_random));
	memcpy(pd.sc.pd_random, pd_random, sizeof(pd_random));

	osdp_compute_session_keys(&pd);
	if (check_array(pd.sc.s_mac1, sizeof(pd.sc.s_mac1),
			exp_smac1, sizeof(exp_smac1), "s_mac1")) {
		osdp_sc_teardown(&pd);
		return -1;
	}
	if (check_array(pd.sc.s_mac2, sizeof(pd.sc.s_mac2),
			exp_smac2, sizeof(exp_smac2), "s_mac2")) {
		osdp_sc_teardown(&pd);
		return -1;
	}
	if (check_array(pd.sc.s_enc, sizeof(pd.sc.s_enc),
			exp_senc, sizeof(exp_senc), "s_enc")) {
		osdp_sc_teardown(&pd);
		return -1;
	}

	osdp_compute_cp_cryptogram(&pd);
	if (check_array(pd.sc.cp_cryptogram, sizeof(pd.sc.cp_cryptogram),
			exp_cp_cryptogram, sizeof(exp_cp_cryptogram),
			"cp_cryptogram")) {
		osdp_sc_teardown(&pd);
		return -1;
	}

	osdp_compute_pd_cryptogram(&pd);
	if (check_array(pd.sc.pd_cryptogram, sizeof(pd.sc.pd_cryptogram),
			exp_pd_cryptogram, sizeof(exp_pd_cryptogram),
			"pd_cryptogram")) {
		osdp_sc_teardown(&pd);
		return -1;
	}

	/* Verify helpers too */
	memcpy(pd.sc.cp_cryptogram, exp_cp_cryptogram, sizeof(exp_cp_cryptogram));
	if (osdp_verify_cp_cryptogram(&pd) != 0) {
		printf("error! cp cryptogram verification failed\n");
		osdp_sc_teardown(&pd);
		return -1;
	}

	memcpy(pd.sc.pd_cryptogram, exp_pd_cryptogram, sizeof(exp_pd_cryptogram));
	if (osdp_verify_pd_cryptogram(&pd) != 0) {
		printf("error! pd cryptogram verification failed\n");
		osdp_sc_teardown(&pd);
		return -1;
	}

	memcpy(pd.sc.cp_cryptogram, exp_cp_cryptogram, sizeof(exp_cp_cryptogram));
	osdp_compute_rmac_i(&pd);
	if (check_array(pd.sc.r_mac, sizeof(pd.sc.r_mac),
			exp_rmac_i, sizeof(exp_rmac_i), "rmac_i")) {
		osdp_sc_teardown(&pd);
		return -1;
	}

	osdp_sc_teardown(&pd);
	return 0;
}

void run_vector_tests(struct test *t)
{
	printf("Annex E vector tests\n");

	DO_TEST(t, test_crc_vectors);
	DO_TEST(t, test_checksum_vectors);
	DO_TEST(t, test_scbkd_vectors);
}
