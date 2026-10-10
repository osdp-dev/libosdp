/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "osdp_file.h"

#define FILE_TRANSFER_STAT_SIZE 7

/* Wire-protocol status codes carried in struct osdp_cmd_file_stat::status */
#define OSDP_FILE_TX_STATUS_ACK		       0
#define OSDP_FILE_TX_STATUS_CONTENTS_PROCESSED 1
#define OSDP_FILE_TX_STATUS_PD_RESET	       2
#define OSDP_FILE_TX_STATUS_KEEP_ALIVE	       3
#define OSDP_FILE_TX_STATUS_ERR_ABORT	       -1
#define OSDP_FILE_TX_STATUS_ERR_UNKNOWN	       -2
#define OSDP_FILE_TX_STATUS_ERR_INVALID	       -3

#define OSDP_FILE_TX_FLAG_EXCLUSIVE  0x01000000
#define OSDP_FILE_TX_FLAG_PLAIN_TEXT 0x02000000
#define OSDP_FILE_TX_FLAG_POLL_RESP  0x04000000

/* The engine shares one arg across read+write. Route both through the file so
 * write can map the app's byte-return to the engine's status convention while
 * read passes the byte count through unchanged. */
static int file_mp_read(void *arg, void *buf, uint32_t size, uint32_t offset)
{
	struct osdp_file *f = arg;

	return f->ops.read(f->ops.arg, buf, size, offset);
}

static int file_mp_write(void *arg, const void *buf, uint32_t size,
			 uint32_t offset)
{
	struct osdp_file *f = arg;
	int n = f->ops.write(f->ops.arg, buf, size, offset);

	/* A short write is fatal; file transfer does not use RETRY. */
	return (n == (int)size) ? OSDP_MP_RX_OK : OSDP_MP_RX_ABORT;
}

static inline void file_state_reset(struct osdp_pd *pd)
{
	struct osdp_file *f = TO_FILE(pd);
	struct osdp_mp_ops ops = { .read = file_mp_read,
				   .write = file_mp_write,
				   .arg = f };
	f->flags = 0;
	f->errors = 0;
	f->is_open = false;
	f->keep_alive_pending = false;
	f->finalize_tstamp = 0;
	f->file_id = 0;
	f->in_terminal = false;
	osdp_mp_reset(&f->mp);
	osdp_mp_bind_ops(&f->mp, &ops);
	osdp_mp_set_event_cb(&f->mp, osdp_mp_pd_notify, pd);
}

static inline void file_close_if_open(struct osdp_pd *pd)
{
	struct osdp_file *f = TO_FILE(pd);

	if (f->is_open) {
		if (f->ops.close(f->ops.arg) < 0) {
			LOG_ERR("File close failed; continuing");
		}
		f->is_open = false;
	}
}

/* Converge every terminal path here: fire MP_DONE, close the file,
 * reset to IDLE. */
static void file_transition_done(struct osdp_pd *pd,
				 enum osdp_mp_outcome outcome)
{
	struct osdp_file *f = TO_FILE(pd);

	/* Held for the whole terminal, callback included; the trailing
	 * file_state_reset() is what clears it, and it is the only way out of
	 * this function. */
	f->in_terminal = true;

	/* If a terminal is reached before the first osdp_file_tx_get_command()
	 * tick emitted START (e.g. an abort on the CP going offline), emit it
	 * now so DONE never precedes START. No-op otherwise. */
	osdp_mp_emit_start(&f->mp);

	/* Fire MP_DONE(outcome) once (both roles), while the engine still has
	 * state, then tear down. */
	osdp_mp_finish(&f->mp, (int)outcome);
	file_close_if_open(pd);
	if (is_cp_mode(pd) && outcome == OSDP_MP_OUTCOME_OK_REBOOTING) {
		make_request(pd, CP_REQ_OFFLINE);
	}
	file_state_reset(pd);
}

static enum osdp_mp_outcome file_outcome_from_wire_status(int16_t status)
{
	switch (status) {
	case OSDP_FILE_TX_STATUS_CONTENTS_PROCESSED:
		return OSDP_MP_OUTCOME_OK;
	case OSDP_FILE_TX_STATUS_PD_RESET:
		return OSDP_MP_OUTCOME_OK_REBOOTING;
	case OSDP_FILE_TX_STATUS_ERR_UNKNOWN:
		return OSDP_MP_OUTCOME_UNRECOGNIZED;
	case OSDP_FILE_TX_STATUS_ERR_INVALID:
		return OSDP_MP_OUTCOME_INVALID;
	case OSDP_FILE_TX_STATUS_ERR_ABORT:
	default:
		return OSDP_MP_OUTCOME_ABORTED;
	}
}

static int16_t file_wire_status_from_outcome(enum osdp_mp_outcome outcome)
{
	switch (outcome) {
	case OSDP_MP_OUTCOME_OK:
		return OSDP_FILE_TX_STATUS_CONTENTS_PROCESSED;
	case OSDP_MP_OUTCOME_OK_REBOOTING:
		return OSDP_FILE_TX_STATUS_PD_RESET;
	case OSDP_MP_OUTCOME_UNRECOGNIZED:
		return OSDP_FILE_TX_STATUS_ERR_UNKNOWN;
	case OSDP_MP_OUTCOME_INVALID:
		return OSDP_FILE_TX_STATUS_ERR_INVALID;
	case OSDP_MP_OUTCOME_ABORTED:
	default:
		return OSDP_FILE_TX_STATUS_ERR_ABORT;
	}
}

/* --- Sender CMD/RESP Handers --- */

int osdp_file_cmd_tx_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	struct osdp_file *f = TO_FILE(pd);
	int hdr =
		1 + OSDP_MP_HDR_SIZE(OSDP_MP_W32); /* type + engine hdr = 11 */
	int engine_max, n;

	/* Reached only once a transfer is live; osdp_file_tx_get_command()
	 * has already gated on pd->file and state. */
	assert(f != NULL);
	assert(osdp_mp_is_active(&f->mp));

	if (max_len <= hdr) {
		LOG_ERR("TX_Build: insufficient space; need:%d have:%d", hdr,
			max_len);
		goto reply_abort;
	}

	if (ISSET_FLAG(f, OSDP_FILE_TX_FLAG_PLAIN_TEXT)) {
		LOG_WRN("TX_Build: Ignoring plaintext file transfer request");
	}

	/* PD-requested post-completion keep-alive (FTSTAT status 3): file is
	 * fully sent, PD just wants the channel kept warm. An idle frame
	 * makes no read attempt and no errors++, so a PD-driven keep-alive
	 * can ping indefinitely. */
	if (f->mp.state == OSDP_MP_WAIT && f->mp.offset == f->mp.total) {
		LOG_DBG("TX_Build: keep-alive (PD requested)");
		buf[0] = (uint8_t)f->file_id;
		return 1 + osdp_mp_tx_build_idle(&f->mp, buf + 1);
	}

	/**
	 * OSDP File module is a bit different than the rest of LibOSDP: it
	 * tries to greedily consume all available packet space, so the SC
	 * headroom must be carved out up front. The engine writes its header
	 * at buf+1; the type byte occupies buf[0].
	 *
	 * TODO: Try to add smarts here later.
	 */
	engine_max = (max_len - 1) - OSDP_MP_SC_RESERVE;
	if (engine_max <= OSDP_MP_HDR_SIZE(OSDP_MP_W32)) {
		LOG_ERR("TX_Build: insufficient space; need:%d have:%d",
			hdr + OSDP_MP_SC_RESERVE, max_len);
		goto reply_abort;
	}

	n = osdp_mp_tx_build(&f->mp, buf + 1, engine_max);
	if (n < 0) {
		LOG_ERR("TX_Build: engine build failed off:%" PRIu32,
			f->mp.offset);
		goto reply_abort;
	}
	buf[0] =
		(uint8_t)f->file_id; /* type byte prefix, ahead of engine hdr */

	if (f->mp.last_len == 0) {
		/* App has no data ready right now (read returned 0): the engine
		 * parked in WAIT and emitted a header-only keep-alive. The
		 * empty-read counter bounds the retry budget via
		 * osdp_file_tx_get_command()'s OSDP_FILE_ERROR_RETRY_MAX check,
		 * so a permanently-stuck app eventually still aborts cleanly. */
		if (f->mp.state == OSDP_MP_WAIT) {
			f->errors++;
			LOG_DBG("TX_Build: app busy; keep-alive (errors=%d)",
				f->errors);
		}
		return 1 + n; /* type + header-only frame */
	}
	f->errors = 0;
	return 1 + n;

reply_abort:
	LOG_ERR("TX_Build: Aborting file transfer due to unrecoverable error!");
	file_transition_done(pd, OSDP_MP_OUTCOME_ABORTED);
	return -1;
}

int osdp_file_cmd_stat_decode(struct osdp_pd *pd, uint8_t *buf, int len)
{
	int pos = 0;
	struct osdp_file *f = TO_FILE(pd);
	struct osdp_cmd_file_stat stat;

	if (f == NULL) {
		LOG_ERR("Stat_Decode: File ops not registered!");
		return -1;
	}

	if (!osdp_mp_is_active(&f->mp)) {
		LOG_ERR("Stat_Decode: File transfer is not in progress!");
		return -1;
	}

	if ((size_t)len < sizeof(struct osdp_cmd_file_stat)) {
		LOG_ERR("Stat_Decode: invalid decode len:%d exp:%zu", len,
			sizeof(struct osdp_cmd_file_stat));
		return -1;
	}

	/* Collect struct osdp_cmd_file_stat */
	stat.control = buf[pos++];
	stat.delay = bread_u16_le(buf, &pos);
	stat.status = bread_u16_le(buf, &pos);
	stat.rx_size = bread_u16_le(buf, &pos);
	assert(pos == len);

	/* Collect control flags */
	SET_FLAG_V(f, OSDP_FILE_TX_FLAG_EXCLUSIVE, !(stat.control & 0x01))
	SET_FLAG_V(f, OSDP_FILE_TX_FLAG_PLAIN_TEXT, stat.control & 0x02)
	SET_FLAG_V(f, OSDP_FILE_TX_FLAG_POLL_RESP, stat.control & 0x04)

	/* Commit the just-acked chunk (advances mp.offset by last_len). A
	 * prior host-busy keep-alive had last_len == 0, so this is a no-op
	 * there and the empty-read counter stays intact — a permanently-busy
	 * app still hits OSDP_FILE_ERROR_RETRY_MAX. Successful data chunks
	 * clear the counter in tx_build. */
	osdp_mp_tx_commit(&f->mp);
	osdp_mp_set_wait(&f->mp, stat.delay);

	if (stat.status < 0) {
		LOG_ERR("Stat_Decode: File transfer error; "
			"status:%d offset:%" PRIu32,
			stat.status, f->mp.offset);
		file_transition_done(
			pd, file_outcome_from_wire_status(stat.status));
		return -1;
	}

	if (f->mp.offset != f->mp.total) {
		/* Transfer still in progress. */
		return 0;
	}

	if (stat.status == OSDP_FILE_TX_STATUS_KEEP_ALIVE) {
		osdp_mp_park(&f->mp);
		LOG_INF("Stat_Decode: File transfer done; keep alive");
		return 0;
	}

	LOG_INF("Stat_Decode: File transfer complete");
	file_transition_done(pd, file_outcome_from_wire_status(stat.status));
	return 0;
}

/* --- Receiver CMD/RESP Handler --- */

int osdp_file_cmd_tx_decode(struct osdp_pd *pd, uint8_t *buf, int len)
{
	struct osdp_file *f = TO_FILE(pd);
	uint8_t type;
	enum osdp_mp_rc mrc;
	struct osdp_cmd cmd;
	bool opened_now = false;

	if (f == NULL) {
		LOG_ERR("TX_Decode: File ops not registered!");
		return -1;
	}

	/* A header-only frame (zero-length data) is the CP keep-alive
	 * ping. Accept it; reject only frames that are short of the
	 * fixed header (type + engine W32 header). */
	if (len < 1 + OSDP_MP_HDR_SIZE(OSDP_MP_W32)) {
		LOG_ERR("TX_Decode: invalid decode len:%d exp>=%d", len,
			1 + OSDP_MP_HDR_SIZE(OSDP_MP_W32));
		return -1;
	}
	type = buf[0];

	if (!osdp_mp_is_active(&f->mp)) {
		/* Peek the engine header to learn the declared file size so
		 * the app's open() sees it. */
		uint32_t total = 0, off = 0;
		uint16_t dlen = 0;

		if (osdp_mp_hdr_read(OSDP_MP_W32, buf + 1, len - 1, &total,
				     &off, &dlen) < 0) {
			LOG_ERR("TX_Decode: invalid header");
			return -1;
		}

		/* Reject a lying declared length BEFORE open(): a receiver
		 * open() is typically create/truncate, so opening on a
		 * malformed first frame would clobber the destination. */
		if ((int)dlen > len - 1 - OSDP_MP_HDR_SIZE(OSDP_MP_W32)) {
			LOG_ERR("TX_Decode: declared length %d exceeds %d bytes present",
				dlen, len - 1 - OSDP_MP_HDR_SIZE(OSDP_MP_W32));
			return -1;
		}

		if (pd->command_callback) {
			/* Notify app of this command and make sure we can
			 * proceed */
			cmd.id = OSDP_CMD_FILE_TX;
			cmd.file_tx.flags = f->flags;
			cmd.file_tx.id = type;
			if (pd->command_callback(pd->command_callback_arg,
						 &cmd) < 0) {
				return -1;
			}
		}

		/* new file write request */
		uint32_t size = total;
		if (f->ops.open(f->ops.arg, type, &size) < 0) {
			LOG_ERR("TX_Decode: Open failed! fd:%d", type);
			return -1;
		}

		LOG_INF("TX_Decode: Starting file transfer of size: %" PRIu32,
			total);
		file_state_reset(pd);
		f->file_id = type;
		f->is_open = true;
		osdp_mp_set_identity(&f->mp, OSDP_MP_MSG_FILE_TRANSFER, type);
		/* Pass the peeked size so MP_START reports the real total. */
		osdp_mp_rx_init(&f->mp, OSDP_MP_W32, total);
		osdp_mp_emit_start(&f->mp);
		opened_now = true;
	}

	/* Hand the frame to the engine: it writes the chunk via f->ops.write
	 * at the frame's declared offset and enforces the offset/length
	 * bounds against the declared total. */
	mrc = osdp_mp_rx_consume(&f->mp, buf + 1, len - 1);
	if (mrc == OSDP_MP_RC_ERR) {
		LOG_ERR("TX_Decode: engine rejected frame at off:%" PRIu32,
			f->mp.offset);
		f->errors++;
		/* If this same call opened the transfer, a malformed first
		 * frame must not leave a dangling open/INPROG transfer
		 * behind: tear it back down. A mid-transfer rejection
		 * legitimately stays open so the CP can retry. */
		if (opened_now) {
			file_transition_done(pd, OSDP_MP_OUTCOME_ABORTED);
		}
		return -1;
	}

	/* Idle frame (offset >= total, zero length): for FILETRANSFER this is
	 * NOT a termination — it is the keep-alive the CP sends after an
	 * FTSTAT status 3 ("finishing", OSDP 2.2 §7.25) while the PD is still
	 * processing. ACK it without advancing. */
	if (mrc == OSDP_MP_RC_EARLY_TERM) {
		LOG_DBG("TX_Decode: idle frame (keep-alive at EOF)");
		f->keep_alive_pending = true;
		return 0;
	}

	/* CP keep-alive ping (zero-length frame mid-transfer): the engine
	 * left offset unchanged. stat_build will reply ACK without
	 * ERR_INVALID once it sees the flag. */
	if (f->mp.last_len == 0) {
		LOG_DBG("TX_Decode: keep-alive ping");
		f->keep_alive_pending = true;
	}

	return 0;
}

int osdp_file_cmd_stat_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int len = 0;
	struct osdp_file *f = TO_FILE(pd);
	struct osdp_cmd_file_stat stat = {
		.status = OSDP_FILE_TX_STATUS_ACK,
		.control = 0x01, /* interleaving, secure channel, no activity */
	};

	if (f == NULL) {
		LOG_ERR("Stat_Build: File ops not registered!");
		return -1;
	}

	if (!osdp_mp_is_active(&f->mp)) {
		LOG_ERR("Stat_Build: File transfer is not in progress!");
		return -1;
	}

	if ((size_t)max_len < sizeof(struct osdp_cmd_file_stat)) {
		LOG_ERR("Stat_Build: insufficient space; need:%zu have:%d",
			sizeof(struct osdp_cmd_file_stat), max_len);
		return -1;
	}

	if (f->keep_alive_pending) {
		/* CP-side keep-alive ping: ACK without advancing offset so
		 * the CP can retry the same chunk once its app recovers. */
		f->keep_alive_pending = false;
	} else if (f->mp.last_len > 0) {
		osdp_mp_rx_commit(&f->mp); /* advance by the acked chunk */
	} else {
		stat.status = OSDP_FILE_TX_STATUS_ERR_INVALID;
	}
	LOG_DBG("offset: %" PRIu32 " size: %" PRIu32, f->mp.offset,
		f->mp.total);

	/*
	 * EOF is decided on offset/total, NOT on mp.state: only osdp_mp_finish
	 * (via file_transition_done) sets state=DONE now, and that is the only
	 * place that tears down + resets. Testing state here would skip
	 * completion. A legitimately retransmitted chunk (lost FTSTAT)
	 * re-drives the running offset without overshooting; the write-side
	 * bound in the engine is the real guard.
	 */
	if (f->mp.offset == f->mp.total && f->mp.total != 0) { /* EOF */
		enum osdp_mp_outcome outcome = OSDP_MP_OUTCOME_OK;
		uint16_t delay = 0;

		if (f->ops.finalize &&
		    !f->ops.finalize(f->ops.arg, &outcome, &delay)) {
			/* App is still working on what it received. Hold the
			 * transfer open: the CP parks on this status and pings
			 * us, which lands back here for another ask. */
			if (f->finalize_tstamp == 0) {
				f->finalize_tstamp = osdp_millis_now();
			}
			if (osdp_millis_since(f->finalize_tstamp) <
			    OSDP_FILE_FINALIZE_TIMEOUT_MS) {
				stat.status = OSDP_FILE_TX_STATUS_KEEP_ALIVE;
				stat.delay = delay;
				goto serialize;
			}
			LOG_ERR("Stat_Build: finalize timed out after %d ms",
				OSDP_FILE_FINALIZE_TIMEOUT_MS);
			outcome = OSDP_MP_OUTCOME_ABORTED;
		}
		stat.status = file_wire_status_from_outcome(outcome);
		LOG_INF("Stat_Build: File receive complete; outcome:%d",
			outcome);
		file_transition_done(pd, outcome);
	}

serialize:
	/* fill the packet buffer (layout: struct osdp_cmd_file_stat) */

	bwrite_u8(stat.control, buf, &len);
	bwrite_u16_le(stat.delay, buf, &len);
	bwrite_u16_le(stat.status, buf, &len);
	bwrite_u16_le(stat.rx_size, buf, &len);
	assert(len == FILE_TRANSFER_STAT_SIZE);
	return len;
}

/* --- State Management --- */

int osdp_file_tx_request_cancel(struct osdp_pd *pd)
{
	struct osdp_file *f = TO_FILE(pd);

	if (!osdp_file_tx_is_active(pd)) {
		return -1;
	}
	/*
	 * Cooperative, not immediate: the transfer ends on the next refresh
	 * tick, which is also where CMD_ABORT goes out to the PD. Tearing it
	 * down here would skip that and leave the PD waiting.
	 */
	osdp_mp_request_cancel(&f->mp);
	return 0;
}

void osdp_file_tx_abort(struct osdp_pd *pd)
{
	if (osdp_file_tx_is_active(pd)) {
		file_transition_done(pd, OSDP_MP_OUTCOME_ABORTED);
	}
}

/**
 * @brief Return the next command that the CP should send to the PD.
 *
 * @param pd PD context
 * @retval +ve - Send this OSDP command
 * @retval  -1 - don't send any command, wait for me
 * @retval   0 - nothing to send; let some other module decide
 */
int osdp_file_tx_get_command(struct osdp_pd *pd)
{
	struct osdp_file *f = TO_FILE(pd);

	if (!osdp_file_tx_is_active(pd)) {
		return 0;
	}

	/* Flush a START deferred by osdp_file_tx_command(): we are now on the
	 * refresh context, before any fragment (PROGRESS) or terminal (DONE). */
	osdp_mp_emit_start(&f->mp);

	if (f->errors > OSDP_FILE_ERROR_RETRY_MAX ||
	    osdp_mp_cancel_requested(&f->mp)) {
		LOG_ERR("Aborting transfer of file fd:%d", f->file_id);
		file_transition_done(pd, OSDP_MP_OUTCOME_ABORTED);
		return CMD_ABORT;
	}

	switch (osdp_mp_tx_next(&f->mp)) {
	case OSDP_MP_ACT_WAIT:
		return ISSET_FLAG(f, OSDP_FILE_TX_FLAG_EXCLUSIVE) ? -1 : 0;
	case OSDP_MP_ACT_DATA:
		return ISSET_FLAG(f, OSDP_FILE_TX_FLAG_POLL_RESP) ?
			       CMD_POLL :
			       CMD_FILETRANSFER;
	default:
		return 0;
	}
}

/**
 * Entry point based on command OSDP_CMD_FILE to kick off a new file transfer.
 */
int osdp_file_tx_command(struct osdp_pd *pd, int file_id, uint32_t flags)
{
	uint32_t size = 0;
	struct osdp_file *f = TO_FILE(pd);

	if (f == NULL) {
		LOG_ERR("TX_init: File ops not registered!");
		return -1;
	}

	if (f->in_terminal) {
		LOG_ERR("TX_init: cannot start a transfer from inside a "
			"file-transfer completion");
		return -1;
	}

	if (osdp_file_tx_is_active(pd)) {
		LOG_ERR("TX_init: A file tx is already in progress");
		return -1;
	}

	/* §5.10.2: a multi-part transfer may not interleave with another. */
	if (osdp_mp_engine_busy(pd)) {
		LOG_ERR("TX_init: another multi-part transfer in progress");
		return -1;
	}

	if (f->ops.open(f->ops.arg, file_id, &size) < 0) {
		LOG_ERR("TX_init: Open failed! fd:%d", file_id);
		return -1;
	}

	if (size == 0) {
		LOG_ERR("TX_init: Invalid file size %" PRIu32, size);
		if (f->ops.close(f->ops.arg) < 0) {
			LOG_ERR("TX_init: File close failed; continuing");
		}
		return -1;
	}

	LOG_INF("TX_init: Starting file transfer of size: %" PRIu32, size);

	file_state_reset(pd);
	f->flags = flags;
	f->file_id = file_id;
	f->is_open = true;
	osdp_mp_set_identity(&f->mp, OSDP_MP_MSG_FILE_TRANSFER, file_id);
	/* This runs inline on the osdp_cp_submit_command() stack. START is
	 * not emitted here; osdp_file_tx_get_command() emits it from the
	 * osdp_cp_refresh() context like every other phase. */
	osdp_mp_tx_init(&f->mp, size, OSDP_MP_W32);
	return 0;
}

/* --- Exported Methods --- */

#ifdef OPT_OSDP_STATIC
#ifndef OSDP_FILE_STATIC_SLOTS
#define OSDP_FILE_STATIC_SLOTS OSDP_CP_MAX_PDS
#endif
static inline struct osdp_file *file_static_slot_get(int pd_idx)
{
	static struct osdp_file g_osdp_file_slots[OSDP_FILE_STATIC_SLOTS];
	if (pd_idx < 0 || pd_idx >= OSDP_FILE_STATIC_SLOTS) {
		return NULL;
	}
	return &g_osdp_file_slots[pd_idx];
}
#endif /* OPT_OSDP_STATIC */

int osdp_file_register_ops(osdp_t *ctx, int pd_idx,
			   const struct osdp_file_ops *ops)
{
	input_check(ctx, pd_idx);
	input_check_not_tearing_down(ctx);
	input_check_not_running(ctx);
	input_check_not_sealed(ctx);
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	if (!pd->file) {
#ifdef OPT_OSDP_STATIC
		pd->file = file_static_slot_get(pd_idx);
		if (pd->file == NULL) {
			LOG_PRINT("No static osdp_file slot for pd_idx=%d",
				  pd_idx);
			return -1;
		}
		memset(pd->file, 0, sizeof(struct osdp_file));
#else
		pd->file = osdp_calloc(1, sizeof(struct osdp_file));
		if (pd->file == NULL) {
			LOG_PRINT("Failed to alloc struct osdp_file");
			return -1;
		}
#endif
	}

	memcpy(&pd->file->ops, ops, sizeof(struct osdp_file_ops));
	file_state_reset(pd);
	return 0;
}

int osdp_get_file_tx_status(const osdp_t *ctx, int pd_idx, uint32_t *size,
			    uint32_t *offset)
{
	input_check(ctx, pd_idx);
	struct osdp_file *f = TO_FILE(osdp_to_pd(ctx, pd_idx));

	if (!f || !osdp_mp_is_active(&f->mp)) {
		LOG_PRINT("File TX not in progress");
		return -1;
	}

	*size = f->mp.total;
	*offset = f->mp.offset;
	return 0;
}
