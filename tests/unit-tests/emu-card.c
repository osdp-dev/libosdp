/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "emu-card.h"

/* NIST SP 800-73 PIV application AID */
const uint8_t emu_card_piv_aid[11] = {
	0xA0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00
};

/* Stand-in FCI for a successful SELECT */
static const uint8_t emu_fci[] = {
	0x61, 0x11, 0x4F, 0x06, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00,
	0x79, 0x07, 0x4F, 0x05, 0xA0, 0x00, 0x00, 0x03, 0x08
};

/* The certificate-sized object; deterministic bytes so reads can be
 * verified against emu_card_object() */
static uint8_t emu_object[EMU_CARD_OBJECT_LEN];
static bool emu_object_ready;

static void emu_object_init(void)
{
	int i;

	if (emu_object_ready) {
		return;
	}
	for (i = 0; i < EMU_CARD_OBJECT_LEN; i++) {
		emu_object[i] = (uint8_t)(i * 7 + 3);
	}
	emu_object_ready = true;
}

const uint8_t *emu_card_object(int *len)
{
	emu_object_init();
	*len = EMU_CARD_OBJECT_LEN;
	return emu_object;
}

void emu_card_reset(struct emu_card *card)
{
	memset(card, 0, sizeof(*card));
}

void emu_card_set_present(struct emu_card *card, bool present)
{
	card->present = present;
	if (!present) {
		card->selected = false;
		card->chain_pos = 0;
		card->chain_rem = 0;
	}
}

static int emu_sw(uint8_t *rapdu, int pos, uint8_t sw1, uint8_t sw2)
{
	rapdu[pos] = sw1;
	rapdu[pos + 1] = sw2;
	return pos + 2;
}

/* Serve the next slice of the object; SW chains with 61xx while more waits */
static int emu_serve_chunk(struct emu_card *card, int le, uint8_t *rapdu,
			   int max_rlen)
{
	int n = le;

	if (n > card->chain_rem) {
		n = card->chain_rem;
	}
	if (n > max_rlen - 2) {
		n = max_rlen - 2;
	}
	memcpy(rapdu, emu_object + card->chain_pos, n);
	card->chain_pos += n;
	card->chain_rem -= n;
	if (card->chain_rem > 0) {
		return emu_sw(rapdu, n, 0x61,
			      card->chain_rem > 255 ? 0 : card->chain_rem);
	}
	return emu_sw(rapdu, n, 0x90, 0x00);
}

int emu_card_apdu(struct emu_card *card, const uint8_t *capdu, int clen,
		  uint8_t *rapdu, int max_rlen)
{
	uint8_t cla, ins, lc;
	int le;

	emu_object_init();

	if (!card->present) {
		return -1;
	}
	if (clen < 4 || max_rlen < 2) {
		return emu_sw(rapdu, 0, 0x67, 0x00); /* wrong length */
	}
	cla = capdu[0];
	ins = capdu[1];

	if (cla != 0x00) {
		return emu_sw(rapdu, 0, 0x6E, 0x00); /* class not supported */
	}

	switch (ins) {
	case 0xA4: /* SELECT */
		if (clen < 5) {
			return emu_sw(rapdu, 0, 0x67, 0x00);
		}
		lc = capdu[4];
		if (clen < 5 + lc) {
			return emu_sw(rapdu, 0, 0x67, 0x00);
		}
		/* The trailing 0x00 of the AID is the optional PIX end; accept
		 * a SELECT of the AID with or without it */
		if ((lc == sizeof(emu_card_piv_aid) ||
		     lc == sizeof(emu_card_piv_aid) - 2) &&
		    memcmp(capdu + 5, emu_card_piv_aid, lc) == 0) {
			card->selected = true;
			if (max_rlen < (int)sizeof(emu_fci) + 2) {
				return emu_sw(rapdu, 0, 0x67, 0x00);
			}
			memcpy(rapdu, emu_fci, sizeof(emu_fci));
			return emu_sw(rapdu, sizeof(emu_fci), 0x90, 0x00);
		}
		return emu_sw(rapdu, 0, 0x6A, 0x82); /* file not found */
	case 0xCB: /* GET DATA */
		if (!card->selected) {
			return emu_sw(rapdu, 0, 0x69, 0x82); /* security */
		}
		if (capdu[2] != 0x3F || capdu[3] != 0xFF) {
			return emu_sw(rapdu, 0, 0x6A, 0x82);
		}
		card->chain_pos = 0;
		card->chain_rem = EMU_CARD_OBJECT_LEN;
		le = capdu[clen - 1] ? capdu[clen - 1] : 256;
		return emu_serve_chunk(card, le, rapdu, max_rlen);
	case 0xC0: /* GET RESPONSE */
		if (card->chain_rem <= 0) {
			return emu_sw(rapdu, 0, 0x69, 0x85); /* not chained */
		}
		if (clen != 5) {
			return emu_sw(rapdu, 0, 0x67, 0x00);
		}
		le = capdu[4] ? capdu[4] : 256;
		return emu_serve_chunk(card, le, rapdu, max_rlen);
	default:
		return emu_sw(rapdu, 0, 0x6D, 0x00); /* INS not supported */
	}
}
