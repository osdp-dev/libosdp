/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdlib.h>

#include "osdp_bio.h"

/* Fragment 0 rides the multipart engine's normal build/consume path with only
 * its header swapped between the standard REPLY_BIOREADR header and the W16
 * fragment header. That in-place swap is only valid because the two headers are
 * the same width. */
_Static_assert(REPLY_BIOREADR_DATA_LEN == OSDP_MP_HDR_SIZE(OSDP_MP_W16),
	       "BIOREADR header and W16 multipart header must be the same size");

#ifdef OPT_OSDP_STATIC
/* CP-mode PDs that can run BIO; the PD context's slot is extra. */
#ifndef OSDP_BIO_STATIC_SLOTS
#define OSDP_BIO_STATIC_SLOTS OSDP_CP_MAX_PDS
#endif
static inline struct osdp_bio *bio_static_slot_get(struct osdp_pd *pd)
{
	static struct osdp_bio g_osdp_bio_slots[OSDP_BIO_STATIC_SLOTS +
						OSDP_STATIC_PD_MODE_SLOTS];
	int i = osdp_static_slot_idx(pd, OSDP_BIO_STATIC_SLOTS);

	return i < 0 ? NULL : &g_osdp_bio_slots[i];
}
#endif /* OPT_OSDP_STATIC */

static struct osdp_bio *bio_get(struct osdp_pd *pd)
{
	if (!pd->bio) {
#ifdef OPT_OSDP_STATIC
		pd->bio = bio_static_slot_get(pd);
		if (pd->bio) {
			memset(pd->bio, 0, sizeof(struct osdp_bio));
		}
#else
		pd->bio = osdp_calloc(1, sizeof(struct osdp_bio));
#endif
		if (pd->bio == NULL) {
			LOG_ERR("BIO: failed to allocate context");
			return NULL;
		}
	}
	return pd->bio;
}

/* End the active op; the context (and its allocation) survives for reuse. */
static void bio_op_reset(struct osdp_bio *b)
{
	b->phase = OSDP_BIO_IDLE;
	osdp_mp_reset(&b->mp);
}

static void bio_emit_start(struct osdp_bio *b)
{
	if (!b->start_emitted) {
		b->start_emitted = true;
		osdp_mp_emit_start(&b->mp);
	}
}

/* Arm the engine for a fresh `total`-byte transfer; caller fixes the direction
 * with osdp_mp_tx_init / osdp_mp_rx_init and moves to OSDP_BIO_REPLY. */
static void bio_op_open(struct osdp_pd *pd, struct osdp_bio *b, uint8_t reader)
{
	osdp_bio_abort(pd); /* supersede whatever a previous op left behind */
	b->start_emitted = false;
	b->tstamp = osdp_millis_now();
	osdp_mp_reset(&b->mp);
	osdp_mp_set_event_cb(&b->mp, osdp_mp_pd_notify, pd);
	osdp_mp_set_identity(&b->mp, OSDP_MP_MSG_BIOREAD, reader);
}

bool osdp_bio_is_active(struct osdp_pd *pd)
{
	return TO_BIO(pd) && TO_BIO(pd)->phase != OSDP_BIO_IDLE;
}

bool osdp_bio_owns_cmd(struct osdp_pd *pd, int cmd_id)
{
	/* CMD_BIOREAD is the only wire command whose reply this engine
	 * reassembles; REPLY_BIOMATCHR is always single-part. */
	return osdp_bio_is_active(pd) && cmd_id == CMD_BIOREAD;
}

int osdp_bio_request_cancel(struct osdp_pd *pd)
{
	struct osdp_bio *b = TO_BIO(pd);

	if (!osdp_bio_is_active(pd)) {
		return -1;
	}
	/* Consumed by osdp_bio_cp_get_command(), which also sends CMD_ABORT. */
	osdp_mp_request_cancel(&b->mp);
	return 0;
}

void osdp_bio_abort(struct osdp_pd *pd)
{
	struct osdp_bio *b = TO_BIO(pd);

	if (!b || b->phase == OSDP_BIO_IDLE) {
		return;
	}
	/* DONE never precedes START, even for an op that dies early. */
	bio_emit_start(b);
	osdp_mp_finish(&b->mp, OSDP_MP_OUTCOME_ABORTED);
	bio_op_reset(b);
}

/* --- PD role --- */

bool osdp_bio_pd_reply_pending(struct osdp_pd *pd)
{
	struct osdp_bio *b = TO_BIO(pd);

	return b && b->phase == OSDP_BIO_REPLY && osdp_mp_is_active(&b->mp) &&
	       b->mp.offset < b->mp.total;
}

/* Write the fixed REPLY_BIOREADR header (reader, status, type, quality, total)
 * at `buf`. Used both for a single-part reply and to overwrite the W16 header
 * of multipart fragment 0. */
static int bio_write_std_header(struct osdp_bio *b, uint16_t total, uint8_t *buf)
{
	int pos = 0;

	buf[pos++] = b->reader;
	buf[pos++] = b->status;
	buf[pos++] = b->type;
	buf[pos++] = b->quality;
	bwrite_u16_le(total, buf, &pos);
	return pos;
}

int osdp_bio_pd_reply_build(struct osdp_pd *pd, const struct osdp_event *event,
			    uint8_t *buf, int max_len)
{
	struct osdp_bio *b;
	bool first;
	int avail, n;

	if (event) { /* first fragment: seed the transfer from the app event */
		b = bio_get(pd);
		if (!b) {
			return -1;
		}
		if (event->bioreadr.length > sizeof(b->data)) {
			LOG_ERR("BIO: template too long (%u)",
				event->bioreadr.length);
			return -1;
		}
		b->reader = event->bioreadr.reader;
		b->status = event->bioreadr.status;
		b->type = event->bioreadr.type;
		b->quality = event->bioreadr.quality;

		avail = max_len - REPLY_BIOREADR_DATA_LEN - OSDP_MP_SC_RESERVE;
		if ((int)event->bioreadr.length <= avail) {
			/* Whole template fits in one packet: plain single-part
			 * reply, no multipart transfer engaged. */
			n = bio_write_std_header(b, event->bioreadr.length, buf);
			memcpy(buf + n, event->bioreadr.data,
			       event->bioreadr.length);
			return n + event->bioreadr.length;
		}

		/* Falls short: fragment 0 carries as much as fits; the rest is
		 * pulled by later polls as W16 continuation fragments. */
		bio_op_open(pd, b, b->reader);
		memcpy(b->data, event->bioreadr.data, event->bioreadr.length);
		osdp_mp_bind_buffer(&b->mp, b->data, event->bioreadr.length);
		osdp_mp_tx_init(&b->mp, event->bioreadr.length, OSDP_MP_W16);
		bio_emit_start(b);
		b->phase = OSDP_BIO_REPLY;
	} else { /* continuation fragment */
		b = TO_BIO(pd);
		if (!osdp_bio_pd_reply_pending(pd)) {
			LOG_ERR("BIO: no reply in progress");
			return -1;
		}
	}

	first = (b->mp.offset == 0);
	n = osdp_mp_tx_build(&b->mp, buf, max_len - OSDP_MP_SC_RESERVE);
	if (n < 0) {
		LOG_ERR("BIO: reply fragment build failed; aborting");
		osdp_bio_abort(pd);
		return -1;
	}
	if (first) {
		/* Swap the W16 header for the standard BIOREADR header (same
		 * size, so the fragment data does not move). The `length` field
		 * carries the TOTAL template size — the CP's cue to reassemble. */
		bio_write_std_header(b, (uint16_t)b->mp.total, buf);
	}
	/* Commit immediately: a lost reply is replayed verbatim by the phy
	 * layer's sequence-repeat handling, never re-built. */
	osdp_mp_tx_commit(&b->mp);
	b->tstamp = osdp_millis_now();
	if (b->mp.offset >= b->mp.total) {
		osdp_mp_finish(&b->mp, OSDP_MP_OUTCOME_OK);
		bio_op_reset(b);
	}
	return n;
}

/* --- CP role --- */

int osdp_bio_cp_get_command(struct osdp_pd *pd)
{
	struct osdp_bio *b = TO_BIO(pd);

	if (!b || b->phase == OSDP_BIO_IDLE) {
		return 0;
	}
	if (osdp_mp_cancel_requested(&b->mp) ||
	    osdp_millis_since(b->tstamp) > OSDP_BIO_OP_TIMEOUT_MS) {
		LOG_ERR("BIO: operation %s; aborting",
			osdp_mp_cancel_requested(&b->mp) ? "cancelled" :
							   "timed out");
		osdp_bio_abort(pd);
		return CMD_ABORT;
	}
	/* Reply fragments are pulled with polls (the ACU drives retrieval). */
	return CMD_POLL;
}

/* Fill `event` from the cached header and the reassembled staging buffer. */
static void bio_cp_fill_event(struct osdp_bio *b, struct osdp_event *event)
{
	memset(event, 0, sizeof(*event));
	event->type = OSDP_EVENT_BIOREADR;
	event->bioreadr.reader = b->reader;
	event->bioreadr.status = b->status;
	event->bioreadr.type = b->type;
	event->bioreadr.quality = b->quality;
	event->bioreadr.length = (uint16_t)b->mp.total;
	memcpy(event->bioreadr.data, b->data, b->mp.total);
}

void osdp_bio_cp_reply_finish(struct osdp_pd *pd)
{
	struct osdp_bio *b = TO_BIO(pd);

	if (!b || b->phase != OSDP_BIO_REPLY) {
		return; /* single-part: no operation was opened */
	}
	osdp_mp_finish(&b->mp, OSDP_MP_OUTCOME_OK);
	bio_op_reset(b);
}

int osdp_bio_cp_reply_consume(struct osdp_pd *pd, const uint8_t *buf, int len,
			      struct osdp_event *event)
{
	struct osdp_bio *b = TO_BIO(pd);
	enum osdp_mp_rc rc;

	if (!b || b->phase == OSDP_BIO_IDLE) {
		/* Fragment 0: a standard REPLY_BIOREADR whose `length` field is
		 * the TOTAL template size. */
		uint8_t tmp[REPLY_BIOREADR_DATA_LEN +
			    OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN];
		uint32_t total;
		int chunk, pos = 0;

		if (len < REPLY_BIOREADR_DATA_LEN) {
			return -1;
		}
		b = bio_get(pd);
		if (!b) {
			return -1;
		}
		b->reader = buf[pos++];
		b->status = buf[pos++];
		b->type = buf[pos++];
		b->quality = buf[pos++];
		total = bread_u16_le(buf, &pos);
		chunk = len - REPLY_BIOREADR_DATA_LEN;
		if (total > sizeof(b->data) || chunk > (int)total) {
			LOG_ERR("BIO: bad first fragment (total %" PRIu32
				", got %d)",
				total, chunk);
			return -1;
		}
		if (chunk == (int)total) {
			/* Whole template in one packet: single-part reply. */
			b->phase = OSDP_BIO_IDLE;
			bio_cp_fill_event(b, event);
			event->bioreadr.length = (uint16_t)total;
			memcpy(event->bioreadr.data,
			       buf + REPLY_BIOREADR_DATA_LEN, total);
			return 1;
		}

		/* Falls short: begin reassembly and feed fragment 0 through the
		 * engine by synthesizing its W16 header. */
		bio_op_open(pd, b, b->reader);
		osdp_mp_bind_buffer(&b->mp, b->data, sizeof(b->data));
		osdp_mp_rx_init(&b->mp, OSDP_MP_W16, total);
		bio_emit_start(b);
		b->phase = OSDP_BIO_REPLY;

		osdp_mp_hdr_write(OSDP_MP_W16, total, 0, (uint16_t)chunk, tmp);
		memcpy(tmp + REPLY_BIOREADR_DATA_LEN,
		       buf + REPLY_BIOREADR_DATA_LEN, chunk);
		rc = osdp_mp_rx_consume(&b->mp, tmp,
					REPLY_BIOREADR_DATA_LEN + chunk);
	} else { /* continuation fragment: a bare W16 body */
		rc = osdp_mp_rx_consume(&b->mp, buf, len);
	}

	switch (rc) {
	case OSDP_MP_RC_MORE:
		osdp_mp_rx_commit(&b->mp);
		b->tstamp = osdp_millis_now();
		return 0;
	case OSDP_MP_RC_DONE:
		osdp_mp_rx_commit(&b->mp);
		bio_cp_fill_event(b, event);
		return 1;
	case OSDP_MP_RC_EARLY_TERM:
		LOG_ERR("BIO: PD terminated the reply early; aborting");
		osdp_bio_abort(pd);
		return 0;
	default:
		LOG_ERR("BIO: bad reply fragment at off:%" PRIu32 "; aborting",
			b->mp.offset);
		osdp_bio_abort(pd);
		return 0;
	}
}
