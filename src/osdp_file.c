/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "osdp_file.h"

#define FILE_TRANSFER_HEADER_SIZE     11
#define FILE_TRANSFER_STAT_SIZE       7

/* Wire-protocol status codes carried in struct osdp_cmd_file_stat::status */
#define OSDP_FILE_TX_STATUS_ACK                0
#define OSDP_FILE_TX_STATUS_CONTENTS_PROCESSED 1
#define OSDP_FILE_TX_STATUS_PD_RESET           2
#define OSDP_FILE_TX_STATUS_KEEP_ALIVE         3
#define OSDP_FILE_TX_STATUS_ERR_ABORT         -1
#define OSDP_FILE_TX_STATUS_ERR_UNKNOWN       -2
#define OSDP_FILE_TX_STATUS_ERR_INVALID       -3

#define OSDP_FILE_TX_FLAG_EXCLUSIVE            0x01000000
#define OSDP_FILE_TX_FLAG_PLAIN_TEXT           0x02000000
#define OSDP_FILE_TX_FLAG_POLL_RESP            0x04000000

static inline void file_state_reset(struct osdp_file *f)
{
	f->flags = 0;
	f->offset = 0;
	f->length = 0;
	f->errors = 0;
	f->size = 0;
	f->state = OSDP_FILE_TX_STATE_IDLE;
	f->outcome = OSDP_FILE_TX_OUTCOME_OK;
	f->is_open = false;
	f->keep_alive_pending = false;
	f->file_id = 0;
	f->tstamp = 0;
	f->wait_time_ms = 0;
	f->cancel_req = false;
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

/* Converge every terminal path here: close the file, emit the
 * notification (CP only), reset to IDLE. */
static void file_transition_done(struct osdp_pd *pd,
				 enum osdp_file_tx_outcome outcome)
{
	struct osdp_file *f = TO_FILE(pd);
	int file_id = f->file_id;

	file_close_if_open(pd);
	f->outcome = outcome;
	f->state = OSDP_FILE_TX_STATE_DONE;

	if (is_cp_mode(pd)) {
		if (outcome == OSDP_FILE_TX_OUTCOME_OK_REBOOTING) {
			make_request(pd, CP_REQ_OFFLINE);
		}
		osdp_file_tx_notify_done(pd, file_id, outcome);
	}

	file_state_reset(f);
}

static enum osdp_file_tx_outcome file_outcome_from_wire_status(int16_t status)
{
	switch (status) {
	case OSDP_FILE_TX_STATUS_CONTENTS_PROCESSED:
		return OSDP_FILE_TX_OUTCOME_OK;
	case OSDP_FILE_TX_STATUS_PD_RESET:
		return OSDP_FILE_TX_OUTCOME_OK_REBOOTING;
	case OSDP_FILE_TX_STATUS_ERR_UNKNOWN:
		return OSDP_FILE_TX_OUTCOME_UNRECOGNIZED;
	case OSDP_FILE_TX_STATUS_ERR_INVALID:
		return OSDP_FILE_TX_OUTCOME_INVALID;
	case OSDP_FILE_TX_STATUS_ERR_ABORT:
	default:
		return OSDP_FILE_TX_OUTCOME_ABORTED;
	}
}

/* --- Sender CMD/RESP Handers --- */

static void write_file_tx_header(struct osdp_file *f, uint8_t *buf)
{
	int len = 0;

	bwrite_u8(f->file_id, buf, &len);
	bwrite_u32_le(f->size, buf, &len);
	bwrite_u32_le(f->offset, buf, &len);
	bwrite_u16_le(f->length, buf, &len);
	assert(len == FILE_TRANSFER_HEADER_SIZE);
}

int osdp_file_cmd_tx_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int buf_available;
	struct osdp_file *f = TO_FILE(pd);
	uint8_t *data = buf + FILE_TRANSFER_HEADER_SIZE;

	/* Reached only once a transfer is live; osdp_file_tx_get_command()
	 * has already gated on pd->file and state. */
	assert(f != NULL);
	assert(f->state == OSDP_FILE_TX_STATE_INPROG ||
	       f->state == OSDP_FILE_TX_STATE_WAIT);

	if ((size_t)max_len <= FILE_TRANSFER_HEADER_SIZE) {
		LOG_ERR("TX_Build: insufficient space; need:%d have:%d",
			FILE_TRANSFER_HEADER_SIZE, max_len);
		goto reply_abort;
	}

	if (ISSET_FLAG(f, OSDP_FILE_TX_FLAG_PLAIN_TEXT)) {
		LOG_WRN("TX_Build: Ignoring plaintext file transfer request");
	}

	/* PD-requested post-completion keep-alive: file is fully sent, PD
	 * just wants the channel kept warm. No read attempt; emit a
	 * header-only ping and let the PD drive completion via its next
	 * stat reply. */
	if (f->state == OSDP_FILE_TX_STATE_WAIT && f->offset == f->size) {
		LOG_DBG("TX_Build: keep-alive (PD requested)");
		f->length = 0;
		write_file_tx_header(f, buf);
		return FILE_TRANSFER_HEADER_SIZE;
	}

	/**
	 * OSDP File module is a bit different than the rest of LibOSDP: it
	 * tries to greedily consume all available packet space. We need to
	 * account for the bytes that phy layer would add and then account for
	 * the overhead due to encryption if a secure channel is active. For
	 * now 16 is choosen based on crude observation.
	 *
	 * TODO: Try to add smarts here later.
	 */
	buf_available = max_len - FILE_TRANSFER_HEADER_SIZE - 16;

	f->length = f->ops.read(f->ops.arg, data, buf_available, f->offset);
	if (f->length < 0) {
		LOG_ERR("TX_Build: user read failed! rc:%d len:%d off:%d",
			f->length, buf_available, f->offset);
		goto reply_abort;
	}
	if (f->length == 0) {
		/* App has no data ready right now. Park in WAIT and emit a
		 * header-only keep-alive instead of tearing the transfer
		 * down. The empty-read counter bounds the retry budget via
		 * osdp_file_tx_get_command()'s OSDP_FILE_ERROR_RETRY_MAX
		 * check, so a permanently-stuck app eventually still
		 * aborts cleanly. */
		f->errors++;
		f->state = OSDP_FILE_TX_STATE_WAIT;
		LOG_DBG("TX_Build: app busy; keep-alive (errors=%d)",
			f->errors);
		write_file_tx_header(f, buf);
		return FILE_TRANSFER_HEADER_SIZE;
	}

	/* Got data: resume INPROG if we were waiting and clear the
	 * empty-read retry budget. */
	if (f->state == OSDP_FILE_TX_STATE_WAIT) {
		LOG_DBG("TX_Build: app recovered; resuming");
		f->state = OSDP_FILE_TX_STATE_INPROG;
	}
	f->errors = 0;

	/* fill the packet buffer (layout: struct osdp_cmd_file_xfer) */
	write_file_tx_header(f, buf);

	return FILE_TRANSFER_HEADER_SIZE + f->length;

reply_abort:
	LOG_ERR("TX_Build: Aborting file transfer due to unrecoverable error!");
	file_transition_done(pd, OSDP_FILE_TX_OUTCOME_ABORTED);
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

	if (f->state != OSDP_FILE_TX_STATE_INPROG &&
	    f->state != OSDP_FILE_TX_STATE_WAIT) {
		LOG_ERR("Stat_Decode: File transfer is not in progress!");
		return -1;
	}

	if ((size_t)len < sizeof(struct osdp_cmd_file_stat)) {
		LOG_ERR("Stat_Decode: invalid decode len:%d exp:%zu",
			len, sizeof(struct osdp_cmd_file_stat));
		return -1;
	}

	/* Collect struct osdp_cmd_file_stat */
	stat.control = buf[pos++];
	stat.delay = bread_u16_le(buf, &pos);
	stat.status = bread_u16_le(buf, &pos);
	stat.rx_size = bread_u16_le(buf, &pos);
	assert(pos == len);
	assert(f->offset + f->length <= f->size);

	/* Collect control flags */
	SET_FLAG_V(f, OSDP_FILE_TX_FLAG_EXCLUSIVE, !(stat.control & 0x01))
	SET_FLAG_V(f, OSDP_FILE_TX_FLAG_PLAIN_TEXT, stat.control & 0x02)
	SET_FLAG_V(f, OSDP_FILE_TX_FLAG_POLL_RESP, stat.control & 0x04)

	/* If the prior tx was a host-busy keep-alive (length == 0), keep
	 * the empty-read counter intact so a permanently-busy app still
	 * hits OSDP_FILE_ERROR_RETRY_MAX. Successful data chunks clear it
	 * in tx_build. */
	f->offset += f->length;
	f->wait_time_ms = stat.delay;
	f->tstamp = osdp_millis_now();
	f->length = 0;

	if (stat.status < 0) {
		LOG_ERR("Stat_Decode: File transfer error; "
			"status:%d offset:%d", stat.status, f->offset);
		file_transition_done(pd,
				     file_outcome_from_wire_status(stat.status));
		return -1;
	}

	if (f->offset != f->size) {
		/* Transfer still in progress. */
		return 0;
	}

	if (stat.status == OSDP_FILE_TX_STATUS_KEEP_ALIVE) {
		f->state = OSDP_FILE_TX_STATE_WAIT;
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
	int rc;
	int pos = 0;
	struct osdp_file *f = TO_FILE(pd);
	struct osdp_cmd_file_xfer xfer;
	struct osdp_cmd cmd;
	uint8_t *data = buf + FILE_TRANSFER_HEADER_SIZE;

	if (f == NULL) {
		LOG_ERR("TX_Decode: File ops not registered!");
		return -1;
	}

	/* A header-only frame (zero-length data) is the CP keep-alive
	 * ping. Accept it; reject only frames that are short of the
	 * fixed header. */
	if ((size_t)len < sizeof(struct osdp_cmd_file_xfer)) {
		LOG_ERR("TX_Decode: invalid decode len:%d exp>=%zu",
			len, sizeof(struct osdp_cmd_file_xfer));
		return -1;
	}

	xfer.type = buf[pos++];
	xfer.size = bread_u32_le(buf, &pos);
	xfer.offset = bread_u32_le(buf, &pos);
	xfer.length = bread_u16_le(buf, &pos);
	assert(pos == sizeof(struct osdp_cmd_file_xfer));
	assert(xfer.length + pos == len);

	if (f->state == OSDP_FILE_TX_STATE_IDLE) {
		if (pd->command_callback) {
			/* Notify app of this command and make sure we can
			 * proceed */
			cmd.id = OSDP_CMD_FILE_TX;
			cmd.file_tx.flags = f->flags;
			cmd.file_tx.id = xfer.type;
			rc = pd->command_callback(pd->command_callback_arg, &cmd);
			if (rc < 0) {
				return -1;
			}
		}

		/* new file write request */
		int size = (int)xfer.size;
		if (f->ops.open(f->ops.arg, xfer.type, &size) < 0) {
			LOG_ERR("TX_Decode: Open failed! fd:%d", xfer.type);
			return -1;
		}

		LOG_INF("TX_Decode: Starting file transfer of size: %d", xfer.size);
		file_state_reset(f);
		f->file_id = xfer.type;
		f->size = xfer.size;
		f->is_open = true;
		f->state = OSDP_FILE_TX_STATE_INPROG;
	}

	if (f->state != OSDP_FILE_TX_STATE_INPROG) {
		LOG_ERR("TX_Decode: File transfer is not in progress!");
		return -1;
	}

	/* CP keep-alive ping (zero-length frame): skip the user write
	 * callback entirely and leave offset unchanged. stat_build will
	 * reply ACK without ERR_INVALID once it sees the flag. */
	if (xfer.length == 0) {
		LOG_DBG("TX_Decode: keep-alive ping at off:%d", xfer.offset);
		f->keep_alive_pending = true;
		f->length = 0;
		return 0;
	}

	f->length = f->ops.write(f->ops.arg, data, xfer.length, xfer.offset);
	if (f->length != xfer.length) {
		LOG_ERR("TX_Decode: user write failed! rc:%d len:%d off:%d",
			f->length, xfer.length, xfer.offset);
		f->errors++;
		return -1;
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

	if (f->state != OSDP_FILE_TX_STATE_INPROG) {
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
	} else if (f->length > 0) {
		f->offset += f->length;
	} else {
		stat.status = OSDP_FILE_TX_STATUS_ERR_INVALID;
	}
	LOG_DBG("length: %d offset: %d size: %d", f->length, f->offset, f->size);
	f->length = 0;
	assert(f->offset <= f->size);
	if (f->offset == f->size) { /* EOF */
		stat.status = OSDP_FILE_TX_STATUS_CONTENTS_PROCESSED;
		LOG_INF("TX_Decode: File receive complete");
		file_transition_done(pd, OSDP_FILE_TX_OUTCOME_OK);
	}

	/* fill the packet buffer (layout: struct osdp_cmd_file_stat) */

	bwrite_u8(stat.control, buf, &len);
	bwrite_u16_le(stat.delay, buf, &len);
	bwrite_u16_le(stat.status, buf, &len);
	bwrite_u16_le(stat.rx_size, buf, &len);
	assert(len == FILE_TRANSFER_STAT_SIZE);
	return len;
}

/* --- State Management --- */

void osdp_file_tx_abort(struct osdp_pd *pd)
{
	if (osdp_file_tx_is_active(pd)) {
		file_transition_done(pd, OSDP_FILE_TX_OUTCOME_ABORTED);
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

	if (f->errors > OSDP_FILE_ERROR_RETRY_MAX || f->cancel_req) {
		LOG_ERR("Aborting transfer of file fd:%d", f->file_id);
		file_transition_done(pd, OSDP_FILE_TX_OUTCOME_ABORTED);
		return CMD_ABORT;
	}

	if (f->wait_time_ms &&
	    osdp_millis_since(f->tstamp) < f->wait_time_ms) {
		return ISSET_FLAG(f, OSDP_FILE_TX_FLAG_EXCLUSIVE) ? -1 : 0;
	}

	if (ISSET_FLAG(f, OSDP_FILE_TX_FLAG_POLL_RESP)) {
		return CMD_POLL;
	}

	return CMD_FILETRANSFER;
}

/**
 * Entry point based on command OSDP_CMD_FILE to kick off a new file transfer.
 */
int osdp_file_tx_command(struct osdp_pd *pd, int file_id, uint32_t flags)
{
	int size = 0;
	struct osdp_file *f = TO_FILE(pd);

	if (f == NULL) {
		LOG_ERR("TX_init: File ops not registered!");
		return -1;
	}

	if (osdp_file_tx_is_active(pd)) {
		if (flags & OSDP_CMD_FILE_TX_FLAG_CANCEL) {
			if (file_id == f->file_id) {
				f->cancel_req = true;
				return 0;
			}
			LOG_ERR("TX_init: invalid cancel request; no such tx!");
			return -1;
		}
		LOG_ERR("TX_init: A file tx is already in progress");
		return -1;
	}

	if (flags & OSDP_CMD_FILE_TX_FLAG_CANCEL) {
		LOG_ERR("TX_init: invalid cancel request");
		return -1;
	}

	if (f->ops.open(f->ops.arg, file_id, &size) < 0) {
		LOG_ERR("TX_init: Open failed! fd:%d", file_id);
		return -1;
	}

	if (size <= 0) {
		LOG_ERR("TX_init: Invalid file size %d", size);
		return -1;
	}

	LOG_INF("TX_init: Starting file transfer of size: %d", size);

	file_state_reset(f);
	f->flags = flags;
	f->file_id = file_id;
	f->size = size;
	f->is_open = true;
	f->state = OSDP_FILE_TX_STATE_INPROG;
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
	struct osdp_pd *pd = osdp_to_pd(ctx, pd_idx);

	if (!pd->file) {
#ifdef OPT_OSDP_STATIC
		pd->file = file_static_slot_get(pd_idx);
		if (pd->file == NULL) {
			LOG_PRINT("No static osdp_file slot for pd_idx=%d", pd_idx);
			return -1;
		}
		memset(pd->file, 0, sizeof(struct osdp_file));
#else
		pd->file = calloc(1, sizeof(struct osdp_file));
		if (pd->file == NULL) {
			LOG_PRINT("Failed to alloc struct osdp_file");
			return -1;
		}
#endif
	}

	memcpy(&pd->file->ops, ops, sizeof(struct osdp_file_ops));
	file_state_reset(pd->file);
	return 0;
}

int osdp_get_file_tx_status(const osdp_t *ctx, int pd_idx,
			    int *size, int *offset)
{
	input_check(ctx, pd_idx);
	struct osdp_file *f = TO_FILE(osdp_to_pd(ctx, pd_idx));

	if (!f || (f->state != OSDP_FILE_TX_STATE_INPROG &&
		   f->state != OSDP_FILE_TX_STATE_WAIT)) {
		LOG_PRINT("File TX not in progress");
		return -1;
	}

	*size = f->size;
	*offset = f->offset;
	return 0;
}
