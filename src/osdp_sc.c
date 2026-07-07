/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "osdp_common.h"

#define OSDP_SC_EOM_MARKER             0x80  /* End of Message Marker */

/* Default key as specified in OSDP specification */
static const uint8_t osdp_scbk_default[16] = {
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
};

void osdp_compute_scbk(struct osdp_pd *pd, uint8_t *master_key, uint8_t *scbk)
{
	int i;

	memcpy(scbk, pd->sc.pd_client_uid, 8);
	for (i = 8; i < 16; i++) {
		scbk[i] = ~scbk[i - 8];
	}
	osdp_encrypt(master_key, NULL, scbk, 16);
}

void osdp_compute_session_keys(struct osdp_pd *pd)
{
	int i;
	uint8_t scbk[16];

	if (ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD)) {
		memcpy(scbk, osdp_scbk_default, 16);
	} else {
		memcpy(scbk, pd->sc.scbk, 16);
	}

	memset(pd->sc.s_enc, 0, 16);
	memset(pd->sc.s_mac1, 0, 16);
	memset(pd->sc.s_mac2, 0, 16);

	pd->sc.s_enc[0] = 0x01;
	pd->sc.s_enc[1] = 0x82;
	pd->sc.s_mac1[0] = 0x01;
	pd->sc.s_mac1[1] = 0x01;
	pd->sc.s_mac2[0] = 0x01;
	pd->sc.s_mac2[1] = 0x02;

	for (i = 2; i < 8; i++) {
		pd->sc.s_enc[i] = pd->sc.cp_random[i - 2];
		pd->sc.s_mac1[i] = pd->sc.cp_random[i - 2];
		pd->sc.s_mac2[i] = pd->sc.cp_random[i - 2];
	}

	osdp_encrypt(scbk, NULL, pd->sc.s_enc, 16);
	osdp_encrypt(scbk, NULL, pd->sc.s_mac1, 16);
	osdp_encrypt(scbk, NULL, pd->sc.s_mac2, 16);
	osdp_fill_zeros(scbk, sizeof(scbk));
}

void osdp_compute_cp_cryptogram(struct osdp_pd *pd)
{
	/* cp_cryptogram = AES-ECB( pd_random[8] || cp_random[8], s_enc ) */
	memcpy(pd->sc.cp_cryptogram + 0, pd->sc.pd_random, 8);
	memcpy(pd->sc.cp_cryptogram + 8, pd->sc.cp_random, 8);
	osdp_encrypt(pd->sc.s_enc, NULL, pd->sc.cp_cryptogram, 16);
}

/**
 * Like memcmp; but operates at constant time.
 *
 * Returns 0 if memory pointed to by s1 and and s2 are identical; non-zero
 * otherwise.
 */
static int osdp_ct_compare(const void *s1, const void *s2, size_t len)
{
	size_t i, ret = 0;
	const uint8_t *_s1 = s1;
	const uint8_t *_s2 = s2;

	for (i = 0; i < len; i++) {
		ret |= _s1[i] ^ _s2[i];
	}
	return (int)ret;
}

int osdp_verify_cp_cryptogram(struct osdp_pd *pd)
{
	uint8_t cp_crypto[16];

	/* cp_cryptogram = AES-ECB( pd_random[8] || cp_random[8], s_enc ) */
	memcpy(cp_crypto + 0, pd->sc.pd_random, 8);
	memcpy(cp_crypto + 8, pd->sc.cp_random, 8);
	osdp_encrypt(pd->sc.s_enc, NULL, cp_crypto, 16);

	int ret = osdp_ct_compare(pd->sc.cp_cryptogram, cp_crypto, 16) == 0 ? 0 : -1;
	osdp_fill_zeros(cp_crypto, sizeof(cp_crypto));
	return ret;
}

void osdp_compute_pd_cryptogram(struct osdp_pd *pd)
{
	/* pd_cryptogram = AES-ECB( cp_random[8] || pd_random[8], s_enc ) */
	memcpy(pd->sc.pd_cryptogram + 0, pd->sc.cp_random, 8);
	memcpy(pd->sc.pd_cryptogram + 8, pd->sc.pd_random, 8);
	osdp_encrypt(pd->sc.s_enc, NULL, pd->sc.pd_cryptogram, 16);
}

int osdp_verify_pd_cryptogram(struct osdp_pd *pd)
{
	uint8_t pd_crypto[16];

	/* pd_cryptogram = AES-ECB( cp_random[8] || pd_random[8], s_enc ) */
	memcpy(pd_crypto + 0, pd->sc.cp_random, 8);
	memcpy(pd_crypto + 8, pd->sc.pd_random, 8);
	osdp_encrypt(pd->sc.s_enc, NULL, pd_crypto, 16);

	int ret = osdp_ct_compare(pd->sc.pd_cryptogram, pd_crypto, 16) == 0 ? 0 : -1;
	osdp_fill_zeros(pd_crypto, sizeof(pd_crypto));
	return ret;
}

void osdp_compute_rmac_i(struct osdp_pd *pd)
{
	/* rmac_i = AES-ECB( AES-ECB( cp_cryptogram, s_mac1 ), s_mac2 ) */
	memcpy(pd->sc.r_mac, pd->sc.cp_cryptogram, 16);
	osdp_encrypt(pd->sc.s_mac1, NULL, pd->sc.r_mac, 16);
	osdp_encrypt(pd->sc.s_mac2, NULL, pd->sc.r_mac, 16);
}

int osdp_decrypt_data(struct osdp_pd *pd, int is_cmd, uint8_t *data, int length)
{
	int i;
	uint8_t iv[16];

	if (length % 16 != 0) {
		return -1;
	}

	if (sc_allow_empty_encrypted_data_block(pd) && length == 0) {
		return 0;
	}

	memcpy(iv, is_cmd ? pd->sc.r_mac : pd->sc.c_mac, 16);
	for (i = 0; i < 16; i++) {
		iv[i] = ~iv[i];
	}

	osdp_decrypt(pd->sc.s_enc, iv, data, length);

	length--;
	while (length && data[length] == 0x00) {
		length--;
	}
	if (data[length] != OSDP_SC_EOM_MARKER) {
		return -1;
	}
	data[length] = 0;

	return length;
}

int osdp_encrypt_data(struct osdp_pd *pd, int is_cmd, uint8_t *data, int length)
{
	int i, pad_len;
	uint8_t iv[16];

	data[length] = OSDP_SC_EOM_MARKER;  /* append EOM marker */
	pad_len = AES_PAD_LEN(length + 1);
	if ((pad_len - length - 1) > 0) {
		memset(data + length + 1, 0, pad_len - length - 1);
	}
	memcpy(iv, is_cmd ? pd->sc.r_mac : pd->sc.c_mac, 16);
	for (i = 0; i < 16; i++) {
		iv[i] = ~iv[i];
	}

	osdp_encrypt(pd->sc.s_enc, iv, data, pad_len);

	return pad_len;
}

int osdp_compute_mac(struct osdp_pd *pd, int is_cmd,
		     const uint8_t *data, int len)
{
	int i, j, n, full_blocks, rem;
	const uint8_t *p;
	uint8_t block[16] = { 0 };
	uint8_t iv[16];

	/**
	 * MAC for data blocks B[1] .. B[N] (post padding) is computed as:
	 * IV1 = R_MAC (or) C_MAC  -- depending on is_cmd
	 * IV2 = B[N-1] after -- AES-CBC ( IV1, B[1] to B[N-1], SMAC-1 )
	 * MAC = AES-ECB ( IV2, B[N], SMAC-2 )
	 */

	memcpy(iv, is_cmd ? pd->sc.r_mac : pd->sc.c_mac, 16);
	p = data;
	full_blocks = len / 16;
	rem = len % 16;
	n = (rem == 0) ? full_blocks : (full_blocks + 1);
	if (n == 0) {
		/* Empty message still contributes a padded terminal block. */
		n = 1;
	}

	/* Process B[1]..B[N-1] with SMAC-1 in CBC fashion. */
	for (i = 0; i < n - 1; i++, p += 16) {
		memcpy(block, p, 16);
		for (j = 0; j < 16; j++) {
			block[j] ^= iv[j];
		}
		osdp_encrypt(pd->sc.s_mac1, NULL, block, 16);
		memcpy(iv, block, 16);
	}

	/* Build B[N], using 0x80 + zero padding when len is not block aligned. */
	memset(block, 0, sizeof(block));
	if (rem == 0 && full_blocks > 0) {
			memcpy(block, p, 16);
	} else {
		memcpy(block, p, rem);
		block[rem] = 0x80; /* end marker */
	}
	for (i = 0; i < 16; i++) {
		block[i] ^= iv[i];
	}

	/* B[N] encrypted with SMAC-2 == MAC */
	osdp_encrypt(pd->sc.s_mac2, NULL, block, 16);
	memcpy(is_cmd ? pd->sc.c_mac : pd->sc.r_mac, block, 16);

	return 0;
}

void osdp_sc_setup(struct osdp_pd *pd)
{
	uint8_t scbk[16];

	osdp_crypt_setup();

	/* Any cached retransmit reply belongs to the previous session and
	 * must not be re-emitted after a handshake. */
	pd->last_tx_len = 0;

	memcpy(scbk, pd->sc.scbk, 16);
	memset(&pd->sc, 0, sizeof(struct osdp_secure_channel));
	memcpy(pd->sc.scbk, scbk, 16);

	if (is_pd_mode(pd)) {
		pd->sc.pd_client_uid[0] = BYTE_0(pd->id.vendor_code);
		pd->sc.pd_client_uid[1] = BYTE_1(pd->id.vendor_code);
		pd->sc.pd_client_uid[2] = BYTE_0(pd->id.model);
		pd->sc.pd_client_uid[3] = BYTE_1(pd->id.version);
		pd->sc.pd_client_uid[4] = BYTE_0(pd->id.serial_number);
		pd->sc.pd_client_uid[5] = BYTE_1(pd->id.serial_number);
		pd->sc.pd_client_uid[6] = BYTE_2(pd->id.serial_number);
		pd->sc.pd_client_uid[7] = BYTE_3(pd->id.serial_number);
	} else {
		osdp_fill_random(pd->sc.cp_random, 8);
	}
}

void osdp_sc_teardown(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
	osdp_crypt_teardown();
}

/* Export the secure-channel crypto primitives to the unit tests. */
OSDP_TEST_ALIAS(osdp_compute_mac);
OSDP_TEST_ALIAS(osdp_encrypt_data);
OSDP_TEST_ALIAS(osdp_decrypt_data);
