/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _EMU_CARD_H_
#define _EMU_CARD_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * A minimal ISO 7816-4 smart card, PIV-shaped: SELECT of the PIV AID, GET
 * DATA of a certificate-sized object, and 61xx/GET RESPONSE chaining. Kept
 * free of libosdp so it models only the card; whatever carries the APDUs
 * (a TRS session, a future example app) is someone else's business.
 */

#define EMU_CARD_OBJECT_LEN 1200

struct emu_card {
	bool present;	/* a card is in the reader's field */
	bool selected;	/* PIV application selected */
	int chain_pos;	/* GET RESPONSE read position into the object */
	int chain_rem;	/* bytes still to be fetched via GET RESPONSE */
};

extern const uint8_t emu_card_piv_aid[11];

void emu_card_reset(struct emu_card *card);
void emu_card_set_present(struct emu_card *card, bool present);

/* The certificate-sized object GET DATA serves, for asserting reads */
const uint8_t *emu_card_object(int *len);

/*
 * Run one C-APDU against the card. Returns the R-APDU length written to
 * @rapdu (always >= 2: data then SW1 SW2), or -1 when no card is present
 * to answer.
 */
int emu_card_apdu(struct emu_card *card, const uint8_t *capdu, int clen,
		  uint8_t *rapdu, int max_rlen);

#endif /* _EMU_CARD_H_ */
