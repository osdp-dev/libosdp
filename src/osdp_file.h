/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_FILE_H_
#define _OSDP_FILE_H_

#include "osdp_common.h"

#define TO_FILE(pd) (pd)->file

/**
 * @brief OSDP specified file transfer command payload.
 */
PACK(struct osdp_cmd_file_xfer {
	uint8_t type;   /**< File transfer type: 1=opaque, 2..127 reserved, 128..255 private use. */
	uint32_t size;  /**< File size in little-endian format. */
	uint32_t offset; /**< Offset in the file for the current message. */
	uint16_t length; /**< Length of the data section in this command. */
	uint8_t data[]; /**< Variable-length file contents. */
});

/**
 * @brief OSDP specified file transfer status payload.
 */
PACK(struct osdp_cmd_file_stat {
	uint8_t control; /**< Control flags: bit-0 interleave, bit-1 leave secure channel, bit-2 separate poll response available. */
	uint16_t delay; /**< Requested delay in milliseconds before the next CMD_FILETRANSFER message. */
	int16_t status; /**< Transfer status: 0 proceed, 1 processed, 2 rebooting, 3 finishing, -1 abort, -2 unrecognized, -3 malformed. */
	uint16_t rx_size; /**< Alternate maximum CMD_FILETRANSFER size, or 0 to keep the current value. */
});

enum osdp_file_tx_state {
	OSDP_FILE_TX_STATE_IDLE,   /* no active transfer */
	OSDP_FILE_TX_STATE_INPROG, /* data being exchanged */
	OSDP_FILE_TX_STATE_WAIT,   /* CP only: app or PD requested time */
	OSDP_FILE_TX_STATE_DONE,   /* terminal; outcome captured */
};

struct osdp_file {
	uint32_t flags;
	int file_id;
	enum osdp_file_tx_state state;
	enum osdp_file_tx_outcome outcome;
	bool is_open;
	bool keep_alive_pending; /* PD: last frame was a zero-length ping */
	int length;
	uint32_t size;
	uint32_t offset;
	int errors;
	bool cancel_req;
	tick_t tstamp;
	uint32_t wait_time_ms;
	struct osdp_file_ops ops;
};

static inline bool osdp_file_tx_is_active(struct osdp_pd *pd)
{
	struct osdp_file *f = TO_FILE(pd);

	return f && (f->state == OSDP_FILE_TX_STATE_INPROG ||
		     f->state == OSDP_FILE_TX_STATE_WAIT);
}

int osdp_file_cmd_tx_build(struct osdp_pd *pd, uint8_t *buf, int max_len);
int osdp_file_cmd_tx_decode(struct osdp_pd *pd, uint8_t *buf, int len);
int osdp_file_cmd_stat_decode(struct osdp_pd *pd, uint8_t *buf, int len);
int osdp_file_cmd_stat_build(struct osdp_pd *pd, uint8_t *buf, int max_len);
int osdp_file_tx_command(struct osdp_pd *pd, int file_id, uint32_t flags);
int osdp_file_tx_get_command(struct osdp_pd *pd);
void osdp_file_tx_abort(struct osdp_pd *pd);

/* Implemented in osdp_cp.c; called by osdp_file.c only on CP-mode PDs. */
void osdp_file_tx_notify_done(struct osdp_pd *pd, int file_id,
			      enum osdp_file_tx_outcome outcome);

#endif /* _OSDP_FILE_H_ */
