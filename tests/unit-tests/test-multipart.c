/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "test.h"
#include "osdp_multipart.h"

/*
 * W16 headers are pinned byte for byte by the frame tests below, but no
 * transfer they drive needs more than 16 bits: check the W32 fields carry
 * all of theirs, and that a header cut short is refused.
 */
static int test_mp_hdr_w32(void)
{
	uint8_t buf[OSDP_MP_HDR_SIZE(OSDP_MP_W32)];
	uint32_t total = 0, offset = 0;
	uint16_t data_len = 0;
	int n;

	n = osdp_mp_hdr_write(OSDP_MP_W32, 0x00ABCDEF, 0x00012345, 0x0100, buf);
	if (n != 10) {
		printf(SUB_1 "w32 hdr write len %d != 10\n", n);
		return -1;
	}
	n = osdp_mp_hdr_read(OSDP_MP_W32, buf, sizeof(buf), &total, &offset,
			     &data_len);
	if (n != 10 || total != 0x00ABCDEF || offset != 0x00012345 ||
	    data_len != 0x0100) {
		printf(SUB_1 "w32 rt total:%x off:%x dlen:%x\n", total, offset,
		       data_len);
		return -1;
	}
	if (osdp_mp_hdr_read(OSDP_MP_W32, buf, 4, NULL, NULL, NULL) != -1) {
		printf(SUB_1 "w32 hdr read accepted 4 bytes\n");
		return -1;
	}
	return 0;
}

/* Build all frames of a buffer-backed transfer into `out`, committing each,
 * until every byte is sent (EOF). The sender parks in WAIT at EOF rather than
 * self-terminating, so drive on bytes-remaining. Returns total bytes emitted,
 * or -1. */
static int drive_tx_to_buffer(struct osdp_multipart *mp, uint8_t *out,
			      int per_frame_max)
{
	int total = 0, n;

	while (mp->offset < mp->total) {
		n = osdp_mp_tx_build(mp, out + total, per_frame_max);
		if (n < 0) {
			return -1;
		}
		total += n;
		osdp_mp_tx_commit(mp);
	}
	return total;
}

static int test_mp_tx_single_frame(void)
{
	struct osdp_multipart mp;
	uint8_t src[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t out[64];
	int n;

	osdp_mp_reset(&mp);
	osdp_mp_bind_buffer(&mp, src, sizeof(src));
	if (osdp_mp_tx_init(&mp, sizeof(src), OSDP_MP_W16) != 0) {
		return -1;
	}
	/* Big packet: whole payload fits one frame. */
	n = drive_tx_to_buffer(&mp, out, 64);
	/* [total=4][off=0][dlen=4][DE AD BE EF] = 6 + 4 = 10 */
	if (n != 10 || out[0] != 4 || out[2] != 0 || out[4] != 4 ||
	    memcmp(out + 6, src, 4) != 0) {
		printf(SUB_1 "tx single frame n=%d\n", n);
		return -1;
	}
	if (mp.offset != sizeof(src)) {
		return -1;
	}
	return 0;
}

static int test_mp_tx_multi_frame(void)
{
	struct osdp_multipart mp;
	uint8_t src[10];
	uint8_t out[128];
	int n, i;

	for (i = 0; i < 10; i++) {
		src[i] = (uint8_t)i;
	}
	osdp_mp_reset(&mp);
	osdp_mp_bind_buffer(&mp, src, sizeof(src));
	osdp_mp_tx_init(&mp, sizeof(src), OSDP_MP_W16);
	/* per_frame_max = hdr(6) + 4 chunk bytes -> 3 frames: 4 + 4 + 2 */
	n = drive_tx_to_buffer(&mp, out, 6 + 4);
	/* frame0: total10 off0 dlen4 | frame1: off4 dlen4 | frame2: off8 dlen2 */
	if (n != (6 + 4) + (6 + 4) + (6 + 2)) {
		printf(SUB_1 "tx multi frame n=%d\n", n);
		return -1;
	}
	/* second frame's OFFSET field (LSB at out[10+2]) must be 4 */
	if (out[10 + 2] != 4) {
		printf(SUB_1 "tx frame1 offset=%d\n", out[10 + 2]);
		return -1;
	}
	if (mp.offset != mp.total) {
		return -1;
	}
	return 0;
}

static int test_mp_rx_roundtrip(void)
{
	struct osdp_multipart tx, rx;
	uint8_t src[10], dst[10], wire[128];
	int i, wlen;
	enum osdp_mp_rc rc = OSDP_MP_RC_MORE;
	int pos = 0;

	for (i = 0; i < 10; i++) {
		src[i] = (uint8_t)(0xA0 + i);
	}
	/* Produce a 3-frame wire stream (4 + 4 + 2). */
	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	wlen = drive_tx_to_buffer(&tx, wire, 6 + 4);

	/* Reassemble it. */
	memset(dst, 0, sizeof(dst));
	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);
	while (pos < wlen) {
		uint32_t total, off;
		uint16_t dlen;
		int flen;

		osdp_mp_hdr_read(OSDP_MP_W16, wire + pos, wlen - pos, &total,
				 &off, &dlen);
		flen = OSDP_MP_HDR_SIZE(OSDP_MP_W16) + dlen;
		rc = osdp_mp_rx_consume(&rx, wire + pos, flen);
		if (rc == OSDP_MP_RC_ERR) {
			return -1;
		}
		osdp_mp_rx_commit(&rx);
		pos += flen;
	}
	if (rc != OSDP_MP_RC_DONE || memcmp(src, dst, sizeof(src)) != 0) {
		printf(SUB_1 "rx roundtrip rc=%d\n", rc);
		return -1;
	}
	return 0;
}

static int test_mp_rx_reject_gap(void)
{
	struct osdp_multipart rx;
	uint8_t dst[10], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);
	/* First frame declares offset 4 (should be 0) -> forward gap -> ERR. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 10, 4, 2, frame);
	frame[flen++] = 0x11;
	frame[flen++] = 0x22;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_ERR) {
		return -1;
	}
	return 0;
}

static int test_mp_rx_early_terminate(void)
{
	struct osdp_multipart rx;
	uint8_t dst[10], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);
	/* offset(10) >= total(10), dlen 0 -> EARLY_TERM. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 10, 10, 0, frame);
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_EARLY_TERM) {
		return -1;
	}
	return 0;
}

struct mp_stream_src {
	const uint8_t *data;
	uint32_t len;
	int busy_first_n; /* return 0 for the first N read attempts */
	int reads;
};

static int stream_read(void *arg, void *buf, uint32_t size, uint32_t offset)
{
	struct mp_stream_src *s = arg;

	s->reads++;
	if (s->busy_first_n > 0) {
		s->busy_first_n--;
		return 0; /* source temporarily empty */
	}
	if (offset >= s->len) {
		return 0;
	}
	if (offset + size > s->len) {
		size = s->len - offset;
	}
	memcpy(buf, s->data + offset, size);
	return (int)size;
}

struct mp_stream_dst {
	uint8_t data[64];
	uint32_t len;
};

static int stream_write(void *arg, const void *buf, uint32_t size,
			uint32_t offset)
{
	struct mp_stream_dst *d = arg;

	if (offset + size > sizeof(d->data)) {
		return OSDP_MP_RX_ABORT;
	}
	memcpy(d->data + offset, buf, size);
	if (offset + size > d->len) {
		d->len = offset + size;
	}
	return OSDP_MP_RX_OK;
}

static int test_mp_stream_roundtrip(void)
{
	static const uint8_t payload[20] = { 1,	 2,  3,	 4,  5,	 6,  7,
					     8,	 9,  10, 11, 12, 13, 14,
					     15, 16, 17, 18, 19, 20 };
	struct mp_stream_src src = { .data = payload, .len = sizeof(payload) };
	struct mp_stream_dst dst = { 0 };
	struct osdp_mp_ops tx_ops = { .read = stream_read, .arg = &src };
	struct osdp_mp_ops rx_ops = { .write = stream_write, .arg = &dst };
	struct osdp_multipart tx, rx;
	uint8_t frame[32];
	enum osdp_mp_rc rc = OSDP_MP_RC_MORE;
	int n;

	osdp_mp_reset(&tx);
	osdp_mp_bind_ops(&tx, &tx_ops);
	osdp_mp_tx_init(&tx, sizeof(payload), OSDP_MP_W32);

	osdp_mp_reset(&rx);
	osdp_mp_bind_ops(&rx, &rx_ops);
	osdp_mp_rx_init(&rx, OSDP_MP_W32, 0);

	/* Pipe each TX frame straight into RX; 8 chunk bytes per frame. Drive
	 * until every byte is sent: the sender parks in WAIT (keep-alive) at
	 * EOF rather than self-terminating, so loop on bytes-remaining, not on
	 * liveness. */
	while (tx.offset < tx.total) {
		n = osdp_mp_tx_build(&tx, frame,
				     OSDP_MP_HDR_SIZE(OSDP_MP_W32) + 8);
		if (n < 0) {
			return -1;
		}
		osdp_mp_tx_commit(&tx);
		rc = osdp_mp_rx_consume(&rx, frame, n);
		if (rc == OSDP_MP_RC_ERR) {
			return -1;
		}
		osdp_mp_rx_commit(&rx);
	}
	if (rc != OSDP_MP_RC_DONE || dst.len != sizeof(payload) ||
	    memcmp(dst.data, payload, sizeof(payload)) != 0) {
		printf(SUB_1 "stream rt rc=%d len=%u\n", rc, dst.len);
		return -1;
	}
	return 0;
}

static int g_retry_writes;

static int retry_once_write(void *arg, const void *src, uint32_t size,
			    uint32_t offset)
{
	(void)arg; (void)src; (void)size; (void)offset;
	/* First write asks for retry, second accepts. */
	return (g_retry_writes++ == 0) ? OSDP_MP_RX_RETRY : OSDP_MP_RX_OK;
}

static int test_mp_rx_retry(void)
{
	struct osdp_multipart rx;
	struct osdp_mp_ops ops = { .read = NULL, .write = retry_once_write,
				   .arg = NULL };
	uint8_t frame[16];
	int flen;

	g_retry_writes = 0;
	osdp_mp_reset(&rx);
	osdp_mp_bind_ops(&rx, &ops);
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);
	/* total=4, off=0, dlen=4 */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 4, 0, 4, frame);
	frame[flen++] = 1; frame[flen++] = 2; frame[flen++] = 3; frame[flen++] = 4;
	/* First consume: write returns RETRY -> RC_RETRY, offset frozen. */
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_RETRY ||
	    rx.offset != 0) {
		return -1;
	}
	/* Resend same frame: write returns OK -> DONE, offset advances. */
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_DONE) {
		return -1;
	}
	osdp_mp_rx_commit(&rx);
	if (rx.offset != 4) {
		return -1;
	}
	return 0;
}

static int test_mp_tx_keepalive_on_busy(void)
{
	static const uint8_t payload[4] = { 9, 8, 7, 6 };
	struct mp_stream_src src = { .data = payload,
				     .len = sizeof(payload),
				     .busy_first_n = 1 };
	struct osdp_mp_ops tx_ops = { .read = stream_read, .arg = &src };
	struct osdp_multipart tx;
	uint8_t frame[32];
	int n;

	osdp_mp_reset(&tx);
	osdp_mp_bind_ops(&tx, &tx_ops);
	osdp_mp_tx_init(&tx, sizeof(payload), OSDP_MP_W32);

	/* First build: source busy -> header-only keep-alive, state WAIT. */
	n = osdp_mp_tx_build(&tx, frame, sizeof(frame));
	if (n != OSDP_MP_HDR_SIZE(OSDP_MP_W32) || tx.state != OSDP_MP_WAIT ||
	    tx.last_len != 0) {
		printf(SUB_1 "keepalive frame n=%d state=%d\n", n, tx.state);
		return -1;
	}
	osdp_mp_tx_commit(&tx); /* no-op: offset unchanged */
	if (tx.offset != 0) {
		return -1;
	}
	/* Second build: source recovered -> real data, back to INPROG. */
	n = osdp_mp_tx_build(&tx, frame, sizeof(frame));
	if (n != OSDP_MP_HDR_SIZE(OSDP_MP_W32) + 4 ||
	    tx.state != OSDP_MP_INPROG) {
		printf(SUB_1 "recovered frame n=%d state=%d\n", n, tx.state);
		return -1;
	}
	return 0;
}

struct mp_event_probe {
	int start;
	int progress;
	int done;
	int last_outcome;
	uint32_t last_offset;
	uint32_t start_total;
};

static void mp_event_probe_fn(void *arg, enum osdp_mp_phase phase,
			      const struct osdp_mp_progress *p)
{
	struct mp_event_probe *e = arg;

	switch (phase) {
	case OSDP_MP_PHASE_START:
		e->start++;
		e->start_total = p->total;
		break;
	case OSDP_MP_PHASE_PROGRESS:
		e->progress++;
		e->last_offset = p->offset;
		break;
	case OSDP_MP_PHASE_DONE:
		e->done++;
		e->last_outcome = p->outcome;
		break;
	}
}

static int test_mp_start_latch(void)
{
	struct osdp_multipart tx;
	struct mp_event_probe probe = { 0 };
	uint8_t src[4] = { 1, 2, 3, 4 };

	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_set_event_cb(&tx, mp_event_probe_fn, &probe);
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	if (probe.start != 0) {
		printf(SUB_1 "latch: START fired at init\n");
		return -1;
	}
	osdp_mp_emit_start(&tx); /* consumer picks the context */
	osdp_mp_emit_start(&tx); /* second call is a no-op */
	if (probe.start != 1) {
		printf(SUB_1 "latch: start=%d (want 1)\n", probe.start);
		return -1;
	}
	return 0;
}

static int test_mp_finish_outcome(void)
{
	struct osdp_multipart tx;
	struct mp_event_probe probe = { 0 };
	uint8_t src[10] = { 0 };

	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_set_event_cb(&tx, mp_event_probe_fn, &probe);
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	osdp_mp_finish(&tx, -2);
	if (osdp_mp_is_active(&tx) || probe.done != 1 ||
	    probe.last_outcome != -2) {
		return -1;
	}
	/* Second finish is a no-op. */
	osdp_mp_finish(&tx, -2);
	if (probe.done != 1 || tx.state != OSDP_MP_DONE) {
		printf(SUB_1 "second finish: done=%d state=%d\n", probe.done,
		       tx.state);
		return -1;
	}
	return 0;
}

static int test_mp_rx_reject_differing_total(void)
{
	struct osdp_multipart rx;
	uint8_t dst[20], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);
	/* frame 0: total=8, off=0, dlen=4 -> MORE */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 8, 0, 4, frame);
	frame[flen++] = 1;
	frame[flen++] = 2;
	frame[flen++] = 3;
	frame[flen++] = 4;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_MORE) {
		return -1;
	}
	osdp_mp_rx_commit(&rx);
	/* frame 1: total=9 (changed!) -> ERR */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 9, 4, 4, frame);
	frame[flen++] = 5;
	frame[flen++] = 6;
	frame[flen++] = 7;
	frame[flen++] = 8;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_ERR) {
		return -1;
	}
	return 0;
}

static int test_mp_rx_first_frame_error_keeps_total_clean(void)
{
	struct osdp_multipart rx;
	uint8_t dst[4], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst)); /* cap 4 */
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);
	/* First frame declares total 10 > cap 4 -> ERR; mp->total must stay 0
	 * so it is not poisoned for a subsequent corrected first frame. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 10, 0, 2, frame);
	frame[flen++] = 0x11;
	frame[flen++] = 0x22;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_ERR) {
		return -1;
	}
	if (rx.total != 0) {
		printf(SUB_1 "rx total poisoned: %u\n", rx.total);
		return -1;
	}
	return 0;
}

/* A frame whose declared DATA_LEN exceeds the bytes actually present must be
 * rejected before it reaches the data plane. */
static int test_mp_rx_reject_truncated(void)
{
	struct osdp_multipart rx;
	uint8_t dst[8], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);
	/* Declares 4 data bytes but only 2 are present. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 8, 0, 4, frame);
	frame[flen++] = 0x11;
	frame[flen++] = 0x22;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_ERR) {
		return -1;
	}
	return 0;
}

/* A resend of an already-committed chunk (lost reply recovery) is accepted:
 * the offset stays put and PROGRESS does not fire twice. */
static int test_mp_rx_retransmit_tolerated(void)
{
	struct osdp_multipart rx;
	struct mp_event_probe probe = { 0 };
	uint8_t dst[8], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_set_event_cb(&rx, mp_event_probe_fn, &probe);
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 8, 0, 4, frame);
	frame[flen++] = 1;
	frame[flen++] = 2;
	frame[flen++] = 3;
	frame[flen++] = 4;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_MORE) {
		return -1;
	}
	osdp_mp_rx_commit(&rx);
	if (rx.offset != 4 || probe.progress != 1) {
		return -1;
	}
	/* The sender missed our reply and re-sends the same frame. */
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_MORE) {
		printf(SUB_1 "retransmit: resend rejected\n");
		return -1;
	}
	osdp_mp_rx_commit(&rx);
	if (rx.offset != 4 || probe.progress != 1) {
		printf(SUB_1 "retransmit: off=%u progress=%d\n", rx.offset,
		       probe.progress);
		return -1;
	}
	/* And the transfer still completes. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 8, 4, 4, frame);
	frame[flen++] = 5;
	frame[flen++] = 6;
	frame[flen++] = 7;
	frame[flen++] = 8;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_DONE) {
		return -1;
	}
	osdp_mp_rx_commit(&rx);
	if (rx.offset != 8) {
		return -1;
	}
	return 0;
}

/* PROGRESS fires per committed mid-transfer fragment and stays silent on the
 * final one — the terminal is DONE, delivered via osdp_mp_finish. */
static int test_mp_tx_progress_counts(void)
{
	struct osdp_multipart tx;
	struct mp_event_probe probe = { 0 };
	uint8_t src[10] = { 0 };
	uint8_t out[128];

	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_set_event_cb(&tx, mp_event_probe_fn, &probe);
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	/* 3 frames (4 + 4 + 2): PROGRESS only for the first two commits. */
	if (drive_tx_to_buffer(&tx, out, 6 + 4) < 0) {
		return -1;
	}
	if (probe.progress != 2 || probe.done != 0) {
		printf(SUB_1 "progress=%d done=%d (want 2, 0)\n",
		       probe.progress, probe.done);
		return -1;
	}
	return 0;
}

/* GENAUTH-style consumer prefix: two bytes between header and data on the
 * offset-0 fragment only, excluded from TOTAL/DATA_LEN. */
static int test_mp_prefix_roundtrip(void)
{
	struct osdp_multipart tx, rx;
	uint8_t src[10], dst[10], frame[32];
	uint8_t pfx_in[2] = { 0xA7, 0x03 }, pfx_out[2];
	enum osdp_mp_rc rc = OSDP_MP_RC_MORE;
	int i, n, frames = 0;

	for (i = 0; i < 10; i++) {
		src[i] = (uint8_t)(0x30 + i);
	}
	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	memset(dst, 0, sizeof(dst));
	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 0);

	while (tx.offset < tx.total) {
		bool first = (tx.offset == 0);

		/* hdr(6) + pfx(2, first frame only) + up to 4 data bytes */
		n = osdp_mp_tx_build_ex(&tx, frame, 6 + 2 + 4, pfx_in,
					sizeof(pfx_in));
		if (n < 0) {
			return -1;
		}
		if (first &&
		    (n != 6 + 2 + 4 || frame[6] != 0xA7 || frame[7] != 0x03)) {
			printf(SUB_1 "pfx: first frame n=%d\n", n);
			return -1;
		}
		osdp_mp_tx_commit(&tx);
		memset(pfx_out, 0, sizeof(pfx_out));
		rc = osdp_mp_rx_consume_ex(&rx, frame, n, pfx_out,
					   sizeof(pfx_out));
		if (rc == OSDP_MP_RC_ERR) {
			printf(SUB_1 "pfx: frame %d rejected\n", frames);
			return -1;
		}
		if (first && (pfx_out[0] != 0xA7 || pfx_out[1] != 0x03)) {
			printf(SUB_1 "pfx: prefix not recovered\n");
			return -1;
		}
		osdp_mp_rx_commit(&rx);
		frames++;
	}
	if (rc != OSDP_MP_RC_DONE || frames < 2 ||
	    memcmp(src, dst, sizeof(src)) != 0) {
		printf(SUB_1 "pfx: rc=%d frames=%d\n", rc, frames);
		return -1;
	}
	return 0;
}

static int test_mp_rx_init_with_total_hint(void)
{
	struct osdp_multipart rx;
	struct mp_event_probe probe = { 0 };
	uint8_t dst[8], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_set_event_cb(&rx, mp_event_probe_fn, &probe);
	/* Consumer peeked the size (8) before rx_init: START must carry it. */
	osdp_mp_rx_init(&rx, OSDP_MP_W16, 8);
	osdp_mp_emit_start(&rx);
	if (probe.start != 1 || probe.start_total != 8) {
		printf(SUB_1 "hint: START total=%u (want 8)\n",
		       probe.start_total);
		return -1;
	}
	/* A frame disagreeing with the hint is rejected outright. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 9, 0, 4, frame);
	memset(frame + flen, 0xAA, 4);
	if (osdp_mp_rx_consume(&rx, frame, flen + 4) != OSDP_MP_RC_ERR) {
		return -1;
	}
	/* A matching frame proceeds normally. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 8, 0, 4, frame);
	memset(frame + flen, 0xAA, 4);
	if (osdp_mp_rx_consume(&rx, frame, flen + 4) != OSDP_MP_RC_MORE) {
		return -1;
	}
	osdp_mp_rx_commit(&rx);
	if (rx.offset != 4 || rx.total != 8) {
		return -1;
	}
	return 0;
}

static int test_mp_tx_next_action(void)
{
	struct osdp_multipart tx;
	uint8_t src[10] = { 0 };

	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	if (osdp_mp_tx_next(&tx) != OSDP_MP_ACT_DATA) {
		return -1;
	}
	osdp_mp_set_wait(&tx, 100000); /* 100s window; will not elapse here */
	if (osdp_mp_tx_next(&tx) != OSDP_MP_ACT_WAIT) {
		return -1;
	}
	osdp_mp_finish(&tx, -2);
	if (osdp_mp_tx_next(&tx) != OSDP_MP_ACT_DONE) {
		return -1;
	}
	return 0;
}

void run_multipart_tests(struct test *t)
{
	printf("\nBegin Multipart Engine Tests\n");

	TEST_CASE(t, "mp_hdr_w32", test_mp_hdr_w32() == 0);
	TEST_CASE(t, "mp_tx_single_frame", test_mp_tx_single_frame() == 0);
	TEST_CASE(t, "mp_tx_multi_frame", test_mp_tx_multi_frame() == 0);
	TEST_CASE(t, "mp_rx_roundtrip", test_mp_rx_roundtrip() == 0);
	TEST_CASE(t, "mp_rx_reject_gap", test_mp_rx_reject_gap() == 0);
	TEST_CASE(t, "mp_rx_early_terminate", test_mp_rx_early_terminate() == 0);
	TEST_CASE(t, "mp_rx_reject_differing_total",
		  test_mp_rx_reject_differing_total() == 0);
	TEST_CASE(t, "mp_rx_first_frame_error_keeps_total_clean",
		  test_mp_rx_first_frame_error_keeps_total_clean() == 0);
	TEST_CASE(t, "mp_rx_init_with_total_hint",
		  test_mp_rx_init_with_total_hint() == 0);
	TEST_CASE(t, "mp_rx_reject_truncated", test_mp_rx_reject_truncated() == 0);
	TEST_CASE(t, "mp_rx_retransmit_tolerated",
		  test_mp_rx_retransmit_tolerated() == 0);
	TEST_CASE(t, "mp_tx_progress_counts", test_mp_tx_progress_counts() == 0);
	TEST_CASE(t, "mp_prefix_roundtrip", test_mp_prefix_roundtrip() == 0);
	TEST_CASE(t, "mp_stream_roundtrip", test_mp_stream_roundtrip() == 0);
	TEST_CASE(t, "mp_rx_retry", test_mp_rx_retry() == 0);
	TEST_CASE(t, "mp_tx_keepalive_on_busy", test_mp_tx_keepalive_on_busy() == 0);
	TEST_CASE(t, "mp_start_latch", test_mp_start_latch() == 0);
	TEST_CASE(t, "mp_finish_outcome", test_mp_finish_outcome() == 0);
	TEST_CASE(t, "mp_tx_next_action", test_mp_tx_next_action() == 0);
}
