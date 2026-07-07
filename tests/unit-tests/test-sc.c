/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"

extern int test_osdp_compute_mac(struct osdp_pd *pd, int is_cmd,
				 const uint8_t *data, int len);
extern int test_osdp_encrypt_data(struct osdp_pd *pd, int is_cmd,
				  uint8_t *data, int len);
extern int test_osdp_decrypt_data(struct osdp_pd *pd, int is_cmd,
				  uint8_t *data, int len);

/* Source: NIST SP 800-38A, F.2.1 AES-128 CBC vectors. */
static const uint8_t tv_key[16] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};
static const uint8_t tv_iv[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
static const uint8_t tv_pt_4blk[64] = {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
	0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
	0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
	0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
	0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
	0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
	0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
	0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
};
static const uint8_t tv_ct_1blk[16] = {
	0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
	0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d
};
static const uint8_t tv_ct_4blk[64] = {
	0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
	0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
	0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee,
	0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2,
	0x73, 0xbe, 0xd6, 0xb8, 0xe3, 0xc1, 0x74, 0x3b,
	0x71, 0x16, 0xe6, 0x9e, 0x22, 0x22, 0x95, 0x16,
	0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac, 0x09,
	0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7
};
static const uint8_t tv_ct_4blk_last[16] = {
	0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac, 0x09,
	0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7
};
/* Source: derived from NIST AES-CBC values above (OpenSSL cross-checked).
 * Generated from AES-128-CBC with key tv_key and IV tv_iv over:
 * 00..0e || 80 (single OSDP EOM-padded block)
 */
static const uint8_t tv_osdp_data_ct_15b[16] = {
	0xc5, 0x31, 0x1f, 0x3d, 0x23, 0xd0, 0x86, 0xea,
	0x86, 0xf5, 0xee, 0x7f, 0x85, 0x24, 0x7b, 0x40
};
/* Source: derived from NIST AES-CBC values above (OpenSSL cross-checked).
 * Generated from AES-128-CBC with key tv_key and IV tv_iv over:
 * 00..0f || 80 || 00*15 (two OSDP-padded blocks)
 */
static const uint8_t tv_osdp_data_ct_16b[32] = {
	0x7d, 0xf7, 0x6b, 0x0c, 0x1a, 0xb8, 0x99, 0xb3,
	0x3e, 0x42, 0xf0, 0x47, 0xb9, 0x1b, 0x54, 0x6f,
	0xe2, 0xfe, 0x5b, 0xd6, 0xc1, 0xdf, 0xcd, 0xd1,
	0x91, 0x24, 0xf0, 0x3e, 0x1a, 0x13, 0x4d, 0x3b
};
/* Source: derived from NIST AES-CBC values above (OpenSSL cross-checked).
 * Generated from AES-128-CBC with key tv_key and IV tv_iv over:
 * 80 || 00*15 (zero-length OSDP payload with EOM marker/padding)
 */
static const uint8_t tv_osdp_data_ct_0b[16] = {
	0x4c, 0x08, 0x22, 0x0c, 0x79, 0xd9, 0x19, 0x10,
	0x22, 0xdc, 0x66, 0x74, 0x87, 0x4c, 0xea, 0xf8
};
static const uint8_t tv_osdp_data_pt_15b[15] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e
};
static const uint8_t tv_osdp_data_pt_16b[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};

static void init_sc_mac_ctx(struct osdp_pd *pd, bool is_cmd)
{
	memset(pd, 0, sizeof(*pd));
	memcpy(pd->sc.s_mac1, tv_key, sizeof(tv_key));
	memcpy(pd->sc.s_mac2, tv_key, sizeof(tv_key));
	if (is_cmd) {
		memcpy(pd->sc.r_mac, tv_iv, sizeof(tv_iv));
	} else {
		memcpy(pd->sc.c_mac, tv_iv, sizeof(tv_iv));
	}
}

static void init_sc_data_ctx(struct osdp_pd *pd, bool is_cmd)
{
	size_t i;

	memset(pd, 0, sizeof(*pd));
	memcpy(pd->sc.s_enc, tv_key, sizeof(tv_key));
	if (is_cmd) {
		for (i = 0; i < sizeof(tv_iv); i++) {
			pd->sc.r_mac[i] = (uint8_t)~tv_iv[i];
		}
	} else {
		for (i = 0; i < sizeof(tv_iv); i++) {
			pd->sc.c_mac[i] = (uint8_t)~tv_iv[i];
		}
	}
}

static int test_aes_cbc_encrypt_vector(struct osdp *ctx)
{
	uint8_t buf[sizeof(tv_pt_4blk)];
	uint8_t iv[sizeof(tv_iv)];

	ARG_UNUSED(ctx);

	memcpy(buf, tv_pt_4blk, sizeof(buf));
	memcpy(iv, tv_iv, sizeof(iv));
	osdp_encrypt((uint8_t *)tv_key, iv, buf, sizeof(buf));
	if (memcmp(buf, tv_ct_4blk, sizeof(buf)) != 0) {
		printf("AES-CBC encrypt vector mismatch\n");
		hexdump(tv_ct_4blk, sizeof(tv_ct_4blk), SUB_1 "Expected");
		hexdump(buf, sizeof(buf), SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_aes_cbc_decrypt_vector(struct osdp *ctx)
{
	uint8_t buf[sizeof(tv_ct_4blk)];
	uint8_t iv[sizeof(tv_iv)];

	ARG_UNUSED(ctx);

	memcpy(buf, tv_ct_4blk, sizeof(buf));
	memcpy(iv, tv_iv, sizeof(iv));
	osdp_decrypt((uint8_t *)tv_key, iv, buf, sizeof(buf));
	if (memcmp(buf, tv_pt_4blk, sizeof(buf)) != 0) {
		printf("AES-CBC decrypt vector mismatch\n");
		hexdump(tv_pt_4blk, sizeof(tv_pt_4blk), SUB_1 "Expected");
		hexdump(buf, sizeof(buf), SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_sc_mac_cmd_one_block(struct osdp *ctx)
{
	struct osdp_pd pd;

	ARG_UNUSED(ctx);
	init_sc_mac_ctx(&pd, true);

	if (test_osdp_compute_mac(&pd, 1, tv_pt_4blk, 16) != 0) {
		printf("osdp_compute_mac failed for 1 block command\n");
		return -1;
	}
	if (memcmp(pd.sc.c_mac, tv_ct_1blk, 16) != 0) {
		printf("MAC mismatch for 1 block command\n");
		hexdump(tv_ct_1blk, 16, SUB_1 "Expected");
		hexdump(pd.sc.c_mac, 16, SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_sc_mac_cmd_four_blocks(struct osdp *ctx)
{
	struct osdp_pd pd;

	ARG_UNUSED(ctx);
	init_sc_mac_ctx(&pd, true);

	if (test_osdp_compute_mac(&pd, 1, tv_pt_4blk, sizeof(tv_pt_4blk)) != 0) {
		printf("osdp_compute_mac failed for 4 block command\n");
		return -1;
	}
	if (memcmp(pd.sc.c_mac, tv_ct_4blk_last, 16) != 0) {
		printf("MAC mismatch for 4 block command\n");
		hexdump(tv_ct_4blk_last, 16, SUB_1 "Expected");
		hexdump(pd.sc.c_mac, 16, SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_sc_mac_reply_one_block(struct osdp *ctx)
{
	struct osdp_pd pd;

	ARG_UNUSED(ctx);
	init_sc_mac_ctx(&pd, false);

	if (test_osdp_compute_mac(&pd, 0, tv_pt_4blk, 16) != 0) {
		printf("osdp_compute_mac failed for 1 block reply\n");
		return -1;
	}
	if (memcmp(pd.sc.r_mac, tv_ct_1blk, 16) != 0) {
		printf("MAC mismatch for 1 block reply\n");
		hexdump(tv_ct_1blk, 16, SUB_1 "Expected");
		hexdump(pd.sc.r_mac, 16, SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_sc_mac_cmd_partial_block(struct osdp *ctx)
{
	struct osdp_pd pd;

	ARG_UNUSED(ctx);
	init_sc_mac_ctx(&pd, true);

	if (test_osdp_compute_mac(&pd, 1, tv_osdp_data_pt_15b,
				  sizeof(tv_osdp_data_pt_15b)) != 0) {
		printf("osdp_compute_mac failed for partial block command\n");
		return -1;
	}
	if (memcmp(pd.sc.c_mac, tv_osdp_data_ct_15b, 16) != 0) {
		printf("MAC mismatch for partial block command\n");
		hexdump(tv_osdp_data_ct_15b, 16, SUB_1 "Expected");
		hexdump(pd.sc.c_mac, 16, SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_sc_encrypt_data_cmd_15b(struct osdp *ctx)
{
	struct osdp_pd pd;
	uint8_t buf[32];
	int rc;

	ARG_UNUSED(ctx);
	init_sc_data_ctx(&pd, true);

	memset(buf, 0, sizeof(buf));
	memcpy(buf, tv_osdp_data_pt_15b, sizeof(tv_osdp_data_pt_15b));
	rc = test_osdp_encrypt_data(&pd, 1, buf, sizeof(tv_osdp_data_pt_15b));
	if (rc != 16) {
		printf("osdp_encrypt_data returned %d, expected 16\n", rc);
		return -1;
	}
	if (memcmp(buf, tv_osdp_data_ct_15b, 16) != 0) {
		printf("osdp_encrypt_data 15-byte vector mismatch\n");
		hexdump(tv_osdp_data_ct_15b, sizeof(tv_osdp_data_ct_15b), SUB_1 "Expected");
		hexdump(buf, 16, SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_sc_encrypt_data_reply_16b(struct osdp *ctx)
{
	struct osdp_pd pd;
	uint8_t buf[32];
	int rc;

	ARG_UNUSED(ctx);
	init_sc_data_ctx(&pd, false);

	memset(buf, 0, sizeof(buf));
	memcpy(buf, tv_osdp_data_pt_16b, sizeof(tv_osdp_data_pt_16b));
	rc = test_osdp_encrypt_data(&pd, 0, buf, sizeof(tv_osdp_data_pt_16b));
	if (rc != 32) {
		printf("osdp_encrypt_data returned %d, expected 32\n", rc);
		return -1;
	}
	if (memcmp(buf, tv_osdp_data_ct_16b, 32) != 0) {
		printf("osdp_encrypt_data 16-byte vector mismatch\n");
		hexdump(tv_osdp_data_ct_16b, sizeof(tv_osdp_data_ct_16b), SUB_1 "Expected");
		hexdump(buf, 32, SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_sc_encrypt_data_cmd_0b(struct osdp *ctx)
{
	struct osdp_pd pd;
	uint8_t buf[16];
	int rc;

	ARG_UNUSED(ctx);
	init_sc_data_ctx(&pd, true);

	memset(buf, 0, sizeof(buf));
	rc = test_osdp_encrypt_data(&pd, 1, buf, 0);
	if (rc != 16) {
		printf("osdp_encrypt_data returned %d, expected 16\n", rc);
		return -1;
	}
	if (memcmp(buf, tv_osdp_data_ct_0b, sizeof(buf)) != 0) {
		printf("osdp_encrypt_data 0-byte vector mismatch\n");
		hexdump(tv_osdp_data_ct_0b, sizeof(tv_osdp_data_ct_0b), SUB_1 "Expected");
		hexdump(buf, sizeof(buf), SUB_1 "Found");
		return -1;
	}

	return 0;
}

static int test_sc_decrypt_data_cmd_15b(struct osdp *ctx)
{
	struct osdp_pd pd;
	uint8_t buf[16];
	int rc;

	ARG_UNUSED(ctx);
	init_sc_data_ctx(&pd, true);

	memcpy(buf, tv_osdp_data_ct_15b, sizeof(buf));
	rc = test_osdp_decrypt_data(&pd, 1, buf, sizeof(buf));
	if (rc != 15) {
		printf("osdp_decrypt_data returned %d, expected 15\n", rc);
		return -1;
	}
	if (memcmp(buf, tv_osdp_data_pt_15b, sizeof(tv_osdp_data_pt_15b)) != 0 ||
	    buf[15] != 0x00) {
		printf("osdp_decrypt_data 15-byte vector mismatch\n");
		hexdump(tv_osdp_data_pt_15b, sizeof(tv_osdp_data_pt_15b), SUB_1 "Expected[0..14]");
		hexdump(buf, sizeof(buf), SUB_1 "Found[0..15]");
		return -1;
	}

	return 0;
}

static int test_sc_decrypt_data_reply_16b(struct osdp *ctx)
{
	struct osdp_pd pd;
	uint8_t buf[32];
	int rc;

	ARG_UNUSED(ctx);
	init_sc_data_ctx(&pd, false);

	memcpy(buf, tv_osdp_data_ct_16b, sizeof(buf));
	rc = test_osdp_decrypt_data(&pd, 0, buf, sizeof(buf));
	if (rc != 16) {
		printf("osdp_decrypt_data returned %d, expected 16\n", rc);
		return -1;
	}
	if (memcmp(buf, tv_osdp_data_pt_16b, sizeof(tv_osdp_data_pt_16b)) != 0 ||
	    buf[16] != 0x00) {
		printf("osdp_decrypt_data 16-byte vector mismatch\n");
		hexdump(tv_osdp_data_pt_16b, sizeof(tv_osdp_data_pt_16b), SUB_1 "Expected[0..15]");
		hexdump(buf, sizeof(buf), SUB_1 "Found[0..31]");
		return -1;
	}

	return 0;
}

static int test_sc_decrypt_data_invalid_len(struct osdp *ctx)
{
	struct osdp_pd pd;
	uint8_t buf[15] = { 0 };

	ARG_UNUSED(ctx);
	init_sc_data_ctx(&pd, true);

	if (test_osdp_decrypt_data(&pd, 1, buf, sizeof(buf)) != -1) {
		printf("osdp_decrypt_data must fail for non block length\n");
		return -1;
	}
	return 0;
}

static int test_sc_decrypt_data_invalid_marker(struct osdp *ctx)
{
	struct osdp_pd pd;
	uint8_t buf[16];
	uint8_t iv[16];

	ARG_UNUSED(ctx);
	init_sc_data_ctx(&pd, true);

	/* Encrypt a block that does not contain OSDP EOM marker (0x80). */
	memset(buf, 0x00, sizeof(buf));
	memcpy(iv, tv_iv, sizeof(iv));
	osdp_encrypt((uint8_t *)tv_key, iv, buf, sizeof(buf));
	if (test_osdp_decrypt_data(&pd, 1, buf, sizeof(buf)) != -1) {
		printf("osdp_decrypt_data must fail without EOM marker\n");
		return -1;
	}

	return 0;
}

void run_sc_tests(struct test *t)
{
	printf("\n");
	printf("SC MAC Tests:\n");
	printf("-------------\n");

	osdp_crypt_setup();

	DO_TEST(t, test_aes_cbc_encrypt_vector);
	DO_TEST(t, test_aes_cbc_decrypt_vector);
	DO_TEST(t, test_sc_mac_cmd_one_block);
	DO_TEST(t, test_sc_mac_cmd_four_blocks);
	DO_TEST(t, test_sc_mac_reply_one_block);
	DO_TEST(t, test_sc_mac_cmd_partial_block);
	DO_TEST(t, test_sc_encrypt_data_cmd_0b);
	DO_TEST(t, test_sc_encrypt_data_cmd_15b);
	DO_TEST(t, test_sc_encrypt_data_reply_16b);
	DO_TEST(t, test_sc_decrypt_data_cmd_15b);
	DO_TEST(t, test_sc_decrypt_data_reply_16b);
	DO_TEST(t, test_sc_decrypt_data_invalid_len);
	DO_TEST(t, test_sc_decrypt_data_invalid_marker);

	osdp_crypt_teardown();
}
