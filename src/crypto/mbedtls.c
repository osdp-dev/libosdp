/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <mbedtls/aes.h>
#include <mbedtls/platform_util.h>

#ifdef MBEDTLS_PSA_CRYPTO_C
#include <psa/crypto.h>
#else
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#endif

#include <osdp.h>

static mbedtls_aes_context aes_ctx;

#ifndef MBEDTLS_PSA_CRYPTO_C
static mbedtls_entropy_context entropy_ctx;
static mbedtls_ctr_drbg_context ctr_drbg_ctx;
#endif

void osdp_crypt_setup()
{
	mbedtls_aes_init(&aes_ctx);

#ifdef MBEDTLS_PSA_CRYPTO_C
	psa_status_t status = psa_crypto_init();
	assert(status == PSA_SUCCESS);
#else
	int rc;
	const char *version = osdp_get_version();

	mbedtls_entropy_init(&entropy_ctx);
	mbedtls_ctr_drbg_init(&ctr_drbg_ctx);
	rc = mbedtls_ctr_drbg_seed(&ctr_drbg_ctx,
				   mbedtls_entropy_func,
				   &entropy_ctx,
				   (const unsigned char *)version,
				   strlen(version));
	assert(rc == 0);
#endif
}

void osdp_encrypt(uint8_t *key, uint8_t *iv, uint8_t *data, int len)
{
	int rc;

	if (iv != NULL) {
		rc = mbedtls_aes_setkey_enc(&aes_ctx, key, 128);
		assert(rc == 0);
		rc = mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT,
					   len, iv, data, data);
		assert(rc == 0);
	} else {
		assert(len <= 16);
		rc = mbedtls_aes_setkey_enc(&aes_ctx, key, 128);
		assert(rc == 0);
		rc = mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT,
					   data, data);
		assert(rc == 0);
	}
}

void osdp_decrypt(uint8_t *key, uint8_t *iv, uint8_t *data, int len)
{
	int rc;

	if (iv != NULL) {
		rc = mbedtls_aes_setkey_dec(&aes_ctx, key, 128);
		assert(rc == 0);
		rc = mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_DECRYPT,
					   len, iv, data, data);
		assert(rc == 0);
	} else {
		assert(len <= 16);
		rc = mbedtls_aes_setkey_dec(&aes_ctx, key, 128);
		assert(rc == 0);
		rc = mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_DECRYPT,
					   data, data);
		assert(rc == 0);
	}
}

void osdp_fill_random(uint8_t *buf, int len)
{
#ifdef MBEDTLS_PSA_CRYPTO_C
	psa_status_t status = psa_generate_random(buf, len);
	assert(status == PSA_SUCCESS);
#else
	int rc = mbedtls_ctr_drbg_random(&ctr_drbg_ctx, buf, len);
	assert(rc == 0);
#endif
}

void osdp_fill_zeros(void *buf, int len)
{
	mbedtls_platform_zeroize(buf, (size_t)len);
}

void osdp_crypt_teardown()
{
#ifndef MBEDTLS_PSA_CRYPTO_C
	mbedtls_ctr_drbg_free(&ctr_drbg_ctx);
	mbedtls_entropy_free(&entropy_ctx);
#endif
	mbedtls_aes_free(&aes_ctx);
}
