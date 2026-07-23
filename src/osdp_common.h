/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_COMMON_H_
#define _OSDP_COMMON_H_

#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <utils/utils.h>
#include <utils/queue.h>
#include <utils/slab.h>
#include <utils/assert.h>

#define USE_CUSTOM_LOGGER
#include <utils/logger.h>

#include <osdp.h>

#ifndef CONFIG_NO_GENERATED_HEADERS
#include "osdp_config.h" /* generated */
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#define OSDP_CTX_MAGIC 0xDEADBEEF

#define ARG_UNUSED(x) (void)(x)

/*
 * OSDP_TEST_ALIAS(fn) exposes an internal function to an out-of-module driver
 * without touching its definition. Under UNIT_TESTING (the in-tree tests) or
 * OSDP_UNIT_TESTABLE (an embedder opting in to drive libosdp internals) it
 * emits a test_<fn> alias of fn -- which may be static -- so a caller in
 * another translation unit reaches it by forward-declaring test_<fn>. It must
 * appear after fn's definition and in the same translation unit. In a normal
 * build it expands to a harmless forward declaration: no test_* symbols reach
 * shipping images and every function keeps its original linkage.
 *
 * This replaces the older idiom of a hand-written test_<fn> function pointer:
 * the alias binds to fn by name (no signature can drift), costs no .data slot,
 * and adds no indirection at the call site.
 */
#if defined(UNIT_TESTING) || defined(OSDP_UNIT_TESTABLE)
#define OSDP_TEST_ALIAS(fn)                                                    \
	extern __typeof__(fn) test_##fn __attribute__((alias(#fn)))
#else
#define OSDP_TEST_ALIAS(fn) struct osdp_test_alias_##fn /* swallow the ';' */
#endif

#ifdef OPT_OSDP_LOG_MINIMAL

__format_printf(4, 5)
int osdp_log_emit_sys(int log_level, const char *file, unsigned long line,
		      const char *fmt, ...);
__format_printf(6, 7)
int osdp_log_emit(bool is_cp, int pd_address, int log_level,
		  const char *file, unsigned long line,
		  const char *fmt, ...);

#define OSDP_PD_LOG(_level, ...)                                               \
	do {                                                                   \
		osdp_log_emit(is_cp_mode(pd), pd->address, _level,             \
			      __FILE__, __LINE__, __VA_ARGS__);                \
	} while (0)

#undef LOG_PRINT
#define LOG_PRINT(...)                                                         \
	do {                                                                   \
		osdp_log_emit(false, -1, LOG_INFO, __FILE__,                   \
			      __LINE__, __VA_ARGS__);                          \
	} while (0)

#else

__format_printf(6, 7)
int osdp_log_cb_emit(bool is_cp, int pd_address, int log_level,
		     const char *file, unsigned long line,
		     const char *fmt, ...);

#define OSDP_PD_LOG(_level, ...)                                               \
	do {                                                                   \
		struct osdp *__ctx = pd_to_osdp(pd);                           \
		if (!__ctx->logger.cb &&                                       \
		    ((_level) < LOG_EMERG || (_level) >= LOG_MAX_LEVEL ||      \
		     (_level) > __ctx->logger.log_level)) {                    \
			break;                                                 \
		}                                                              \
		logger_t __log_ctx = __ctx->logger;                            \
		char __name[LOGGER_NAME_MAXLEN];                               \
		if (is_cp_mode(pd)) {                                          \
			snprintf(__name, sizeof(__name), "OSDP: CP: PD-%d",    \
				 pd->address);                                 \
		} else {                                                       \
			snprintf(__name, sizeof(__name), "OSDP: PD-%d",        \
				 pd->address);                                 \
		}                                                              \
		logger_set_name(&__log_ctx, __name);                           \
		if (__ctx->logger.cb) {                                        \
			osdp_log_cb_emit(is_cp_mode(pd), pd->address, _level,  \
					 __FILE__, __LINE__, __VA_ARGS__);     \
			break;                                                 \
		}                                                              \
		__logger_log(&__log_ctx, _level, __FILE__, __LINE__,           \
			     __VA_ARGS__);                                     \
	} while (0)

#endif /* OPT_OSDP_LOG_MINIMAL */

#define LOG_EM(...)    OSDP_PD_LOG(LOG_EMERG, __VA_ARGS__)
#define LOG_ALERT(...) OSDP_PD_LOG(LOG_ALERT, __VA_ARGS__)
#define LOG_CRIT(...)  OSDP_PD_LOG(LOG_CRIT, __VA_ARGS__)
#define LOG_ERR(...)   OSDP_PD_LOG(LOG_ERR, __VA_ARGS__)
#define LOG_INF(...)   OSDP_PD_LOG(LOG_INFO, __VA_ARGS__)
#define LOG_WRN(...)   OSDP_PD_LOG(LOG_WARNING, __VA_ARGS__)
#define LOG_WRN_ONCE(...)                                                      \
	do {                                                                   \
		static int warned = 0;                                         \
		if (!warned) {                                                 \
			OSDP_PD_LOG(LOG_WARNING, __VA_ARGS__);                 \
			warned = 1;                                            \
		}                                                              \
	} while (0)
#define LOG_NOT(...) OSDP_PD_LOG(LOG_NOTICE, __VA_ARGS__)
#define LOG_DBG(...) OSDP_PD_LOG(LOG_DEBUG, __VA_ARGS__)

#define ISSET_FLAG(p, f) (((p)->flags & (f)) == (f))
#define SET_FLAG(p, f)	 ((p)->flags |= (f))
#define CLEAR_FLAG(p, f) ((p)->flags &= ~(f))
#define SET_FLAG_V(p, f, v)                                                    \
	if ((v))                                                               \
		SET_FLAG(p, f);                                                \
	else                                                                   \
		CLEAR_FLAG(p, f);

#define BYTE_0(x) (uint8_t)(((x) >> 0) & 0xFF)
#define BYTE_1(x) (uint8_t)(((x) >> 8) & 0xFF)
#define BYTE_2(x) (uint8_t)(((x) >> 16) & 0xFF)
#define BYTE_3(x) (uint8_t)(((x) >> 24) & 0xFF)

/*
 * Round a bit count up to whole bytes. Widened to uint32_t because the
 * operand is a uint16_t wire value: it would otherwise promote to int, and
 * 0xFFFF + 7 overflows an int that is only 16 bits wide on some targets.
 */
#define BITS_TO_BYTES(n) ((int)(((uint32_t)(n) + 7) / 8))

/* casting helpers */
#define TO_OSDP(ctx) ((struct osdp *)ctx)

#define GET_CURRENT_PD(ctx) (TO_OSDP(ctx)->_current_pd)
#define SET_CURRENT_PD(ctx, i)                                                 \
	do {                                                                   \
		TO_OSDP(ctx)->_current_pd = osdp_to_pd(ctx, i);                \
	} while (0)
#define AES_PAD_LEN(x) ((x + 16 - 1) & (~(16 - 1)))
#define NUM_PD(ctx)    (TO_OSDP(ctx)->_num_pd)
#define PD_MASK(ctx)   (uint32_t)(BIT(NUM_PD(ctx)) - 1)

#define safe_free(p)                                                           \
	if (p)                                                                 \
	free(p)

#define osdp_dump hexdump // for zephyr compatibility.

static inline __noreturn void die()
{
#ifdef __BARE_METAL__
	for (;;) {
	}
#else
	exit(EXIT_FAILURE);
	__unreachable();
#endif
}

#define BUG()                                                                  \
	do {                                                                   \
		printf("BUG at %s:%d %s(). Please report this issue!",         \
		       __FILE__, __LINE__, __func__);                          \
		die();                                                         \
	} while (0)

#define BUG_ON(pred)                                                           \
	do {                                                                   \
		if (unlikely(pred)) {                                          \
			BUG();                                                 \
		}                                                              \
	} while (0)

/**
 * OSDP application exposed method arg checker.
 *
 * Usage:
 *    input_check(ctx);
 *    input_check(ctx, pd);
 */
#define input_check_init(_ctx)                                                 \
	do {                                                                   \
		struct osdp *__ctx = (struct osdp *)_ctx;                      \
		assert(__ctx);                                                 \
		__ctx->_magic = OSDP_CTX_MAGIC;                                \
	} while (0)
#define input_check_osdp_ctx(_ctx)                                             \
	do {                                                                   \
		struct osdp *__ctx = (struct osdp *)_ctx;                      \
		BUG_ON(__ctx == NULL);                                         \
		BUG_ON(__ctx->_magic != OSDP_CTX_MAGIC);                       \
	} while (0)
#define input_check_pd_offset(_ctx, _pd)                                       \
	do {                                                                   \
		struct osdp *__ctx = (struct osdp *)_ctx;                      \
		int __pd = _pd;                                                \
		if (__pd < 0 || __pd >= __ctx->_num_pd) {                      \
			LOG_PRINT("Invalid PD number %d", __pd);               \
			return -1;                                             \
		}                                                              \
	} while (0)
#define input_check2(_1, _2)                                                   \
	input_check_osdp_ctx(_1);                                              \
	input_check_pd_offset(_1, _2);
#define input_check1(_1)	      input_check_osdp_ctx(_1);
#define get_macro(_1, _2, macro, ...) macro
#define input_check(...)                                                       \
	get_macro(__VA_ARGS__, input_check2, input_check1)(__VA_ARGS__)

/**
 * @brief OSDP reserved commands
 */
#define CMD_INVALID	 0x00
#define CMD_POLL	 0x60
#define CMD_ID		 0x61
#define CMD_CAP		 0x62
#define CMD_LSTAT	 0x64
#define CMD_ISTAT	 0x65
#define CMD_OSTAT	 0x66
#define CMD_RSTAT	 0x67
#define CMD_OUT		 0x68
#define CMD_LED		 0x69
#define CMD_BUZ		 0x6A
#define CMD_TEXT	 0x6B
#define CMD_RMODE	 0x6C /* deprecated: removed from the spec */
#define CMD_TDSET	 0x6D /* obsolete in spec; kept for legacy interop */
#define CMD_COMSET	 0x6E
#define CMD_BIOREAD	 0x73
#define CMD_BIOMATCH	 0x74
#define CMD_KEYSET	 0x75
#define CMD_CHLNG	 0x76
#define CMD_SCRYPT	 0x77
#define CMD_ACURXSIZE	 0x7B
#define CMD_FILETRANSFER 0x7C
#define CMD_MFG		 0x80
#define CMD_XWR		 0xA1
#define CMD_ABORT	 0xA2
#define CMD_PIVDATA	 0xA3
#define CMD_GENAUTH	 0xA4
#define CMD_CRAUTH	 0xA5
#define CMD_KEEPACTIVE	 0xA7

/**
 * @brief OSDP reserved responses
 */
#define REPLY_INVALID	0x00
#define REPLY_ACK	0x40
#define REPLY_NAK	0x41
#define REPLY_PDID	0x45
#define REPLY_PDCAP	0x46
#define REPLY_LSTATR	0x48
#define REPLY_ISTATR	0x49
#define REPLY_OSTATR	0x4A
#define REPLY_RSTATR	0x4B
#define REPLY_RAW	0x50
#define REPLY_FMT	0x51 /* deprecated */
#define REPLY_KEYPAD	0x53
#define REPLY_COM	0x54
#define REPLY_BIOREADR	0x57
#define REPLY_BIOMATCHR 0x58
#define REPLY_CCRYPT	0x76
#define REPLY_RMAC_I	0x78
#define REPLY_BUSY	0x79
#define REPLY_FTSTAT	0x7A
#define REPLY_PIVDATAR	0x80
#define REPLY_GENAUTHR	0x81
#define REPLY_CRAUTHR	0x82
#define REPLY_MFGSTATR	0x83
#define REPLY_MFGERRR	0x84
#define REPLY_MFGREP	0x90
#define REPLY_XRD	0xB1

/**
 * @brief Number of data bytes that follow the command/reply ID byte.
 *
 * The encoders (cp_build_command(), pd_build_reply()) write the ID byte and
 * the decoders (pd_decode_command(), cp_decode_response()) consume it, both
 * outside of their per-message switch. These lengths therefore describe the
 * data block alone and are shared by both sides of the wire. For variable
 * length messages, they cover only the fixed part that precedes the payload.
 * Messages that carry no data at all are simply absent from this list.
 */
#define CMD_ID_DATA_LEN		1
#define CMD_CAP_DATA_LEN	1
#define CMD_OUT_DATA_LEN	4
#define CMD_LED_DATA_LEN	14
#define CMD_BUZ_DATA_LEN	5
#define CMD_TEXT_DATA_LEN	6 /* variable length message */
#define CMD_TDSET_DATA_LEN	7
#define CMD_COMSET_DATA_LEN	5
#define CMD_BIOREAD_DATA_LEN	4
#define CMD_BIOMATCH_DATA_LEN	6 /* variable length message */
#define CMD_KEYSET_DATA_LEN	18
#define CMD_CHLNG_DATA_LEN	8
#define CMD_SCRYPT_DATA_LEN	16
#define CMD_ACURXSIZE_DATA_LEN	2
#define CMD_KEEPACTIVE_DATA_LEN 2
#define CMD_MFG_DATA_LEN	3 /* variable length message */
#define CMD_PIVDATA_DATA_LEN	5

#define REPLY_NAK_DATA_LEN	 1
#define REPLY_PDID_DATA_LEN	 12
#define REPLY_PDCAP_ENTITY_LEN	 3 /* variable count of these entities */
#define REPLY_LSTATR_DATA_LEN	 2
#define REPLY_COM_DATA_LEN	 5
#define REPLY_KEYPAD_DATA_LEN	 2 /* variable length message */
#define REPLY_RAW_DATA_LEN	 4 /* variable length message */
#define REPLY_MFGREP_DATA_LEN	 3 /* variable length message */
#define REPLY_BIOREADR_DATA_LEN	 6 /* variable length message */
#define REPLY_BIOMATCHR_DATA_LEN 3
#define REPLY_CCRYPT_DATA_LEN	 32
#define REPLY_RMAC_I_DATA_LEN	 16

/**
 * @brief secure block types
 */
#define SCS_11 0x11 /* CP -> PD -- CMD_CHLNG */
#define SCS_12 0x12 /* PD -> CP -- REPLY_CCRYPT */
#define SCS_13 0x13 /* CP -> PD -- CMD_SCRYPT */
#define SCS_14 0x14 /* PD -> CP -- REPLY_RMAC_I */

#define SCS_15 0x15 /* CP -> PD -- packets w MAC w/o ENC */
#define SCS_16 0x16 /* PD -> CP -- packets w MAC w/o ENC */
#define SCS_17 0x17 /* CP -> PD -- packets w MAC w ENC*/
#define SCS_18 0x18 /* PD -> CP -- packets w MAC w ENC*/

/* PD State Flags */
#define PD_FLAG_SC_CAPABLE     BIT(0) /* PD secure channel capable */
#define PD_FLAG_TAMPER	       BIT(1) /* local tamper status */
#define PD_FLAG_POWER	       BIT(2) /* local power status */
#define PD_FLAG_R_TAMPER       BIT(3) /* reader tamper status */
#define PD_FLAG_SKIP_SEQ_CHECK BIT(5) /* disable seq checks (debug) */
#define PD_FLAG_SC_USE_SCBKD   BIT(6) /* in this SC attempt, use SCBKD */
#define PD_FLAG_SC_ACTIVE      BIT(7) /* secure channel is active */
#define PD_FLAG_PD_MODE	       BIT(8) /* device is setup as PD */
#define PD_FLAG_PKT_SKIP_MARK  BIT(10) /* OPT_OSDP_SKIP_MARK_BYTE */
#define PD_FLAG_PKT_HAS_MARK   BIT(11) /* Packet has mark byte */
#define PD_FLAG_SC_DISABLED    BIT(12) /* master_key=NULL && scbk=NULL */
#define PD_FLAG_PKT_BROADCAST  BIT(13) /* this packet was addressed to 0x7F */
#define PD_FLAG_CP_USE_CRC     BIT(14) /* CP uses CRC-16 instead of checksum */
#define PD_FLAG_ONLINE	       BIT(15) /* PD mode: CP link is active */
#define PD_FLAG_TRS_CAPABLE    BIT(16) /* PD reported a smart card reader */
#define PD_FLAG_TRS_ACTIVE     BIT(17) /* a TRS card session is in progress */

/* PD Init flags */
#define PD_FLAG_ENFORCE_SECURE  BIT(24) /* See: OSDP_FLAG_ENFORCE_SECURE */
#define PD_FLAG_INSTALL_MODE    BIT(25) /* See: OSDP_FLAG_INSTALL_MODE */
#define PD_FLAG_IGNORE_USR      BIT(26) /* See: OSDP_FLAG_IGNORE_USR */
#define PD_FLAG_ENABLE_NOTIF    BIT(27) /* See: OSDP_FLAG_ENABLE_NOTIFICATION */
#define PD_FLAG_CAPTURE_PKT     BIT(28) /* See: OSDP_FLAG_CAPTURE_PACKETS */
#define PD_FLAG_ALLOW_EMPTY_EDB BIT(29) /* See: OSDP_FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK */
#define PD_FLAG_BIOREADR_MP     BIT(30) /* See: OSDP_FLAG_BIOREADR_MULTIPART */

/* CP event requests; used with make_request() and check_request() */
#define CP_REQ_RESTART_SC 0x00000001
/* PD NAK'd our sequence number: its link state and ours have diverged.
 * Tear the session down and reconnect from scratch. */
#define CP_REQ_LINK_RESET 0x00000002
#define CP_REQ_OFFLINE	  0x00000004
#define CP_REQ_DISABLE	  0x00000008
#define CP_REQ_ENABLE	  0x00000010
#ifdef OPT_BUILD_OSDP_TRS
/* Open a TRS session: OPEN for an app band (a START was dequeued), SCAN for a
 * library-initiated presence probe. Posted by the online command selector and
 * consumed like every other transition request in state_update(). */
#define CP_REQ_TRS_OPEN 0x00000020
#define CP_REQ_TRS_SCAN 0x00000040
#endif

#define TRS_MODE_00 0x00
#define TRS_MODE_01 0x01

#define TRS_DISABLE_CARD_INFO_REPORT 0x00

enum osdp_cp_phy_state_e {
	OSDP_CP_PHY_STATE_IDLE,
	OSDP_CP_PHY_STATE_SEND_CMD,
	OSDP_CP_PHY_STATE_SEND_CMD_WAIT,
	OSDP_CP_PHY_STATE_REPLY_WAIT,
	OSDP_CP_PHY_STATE_WAIT,
	OSDP_CP_PHY_STATE_RETRY_CMD,
	OSDP_CP_PHY_STATE_DONE,
	OSDP_CP_PHY_STATE_ERR,
};

enum osdp_cp_state_e {
	OSDP_CP_STATE_INIT,
	OSDP_CP_STATE_CAPDET,
	OSDP_CP_STATE_SC_CHLNG,
	OSDP_CP_STATE_SC_SCRYPT,
	OSDP_CP_STATE_SET_SCBK,
	OSDP_CP_STATE_ONLINE,
	OSDP_CP_STATE_OFFLINE,
	OSDP_CP_STATE_DISABLED,
	OSDP_CP_STATE_TRS_SETUP,
	OSDP_CP_STATE_TRS_RUN,
	OSDP_CP_STATE_SENTINEL
};

#ifdef OPT_BUILD_OSDP_TRS
enum trs_state_e {
	TRS_STATE_SET_MODE, /* CP negotiating TRS mode 01 with PD */
	TRS_STATE_XMIT, /* session up; draining app APDU commands */
	TRS_STATE_DISCONNECT_CARD, /* terminating the current card session */
	TRS_STATE_TEARDOWN, /* CP restoring TRS mode 00 (transparent off) */
	TRS_STATE_DONE, /* session finished; return to ONLINE */
};

struct osdp_trs {
	enum trs_state_e state; /* current TRS session sub-state */
	uint8_t mode; /* negotiated TRS mode (TRS_MODE_00/01) */
	bool failed; /* session did not open, or was cut short by an error */
	/*
	 * Tracks the tail of cmd_queue, not the session: set when a START is
	 * queued and cleared when its STOP is. A command is inside the band iff
	 * this is set when it is submitted; pd->state cannot answer that, as it
	 * trails the tail by everything still queued behind it.
	 */
	bool band_open;
	bool stop_pending; /* this session's STOP has not been dequeued yet */
	/*
	 * The last REPLY_XRD was an error report: the reader took the command
	 * but the card transaction failed. Written on every REPLY_XRD, and read
	 * only for the CMD_XWR it answered.
	 */
	bool reply_error;
	/*
	 * The TRS session in progress is a presence-scan probe the library
	 * opened on its own -- no app band, no session notifications; it winds
	 * down by mode-set alone (there is no card transaction to terminate).
	 */
	bool probe;
	/* Background presence scan (osdp_cp_trs_scan_enable) */
	struct {
		bool enabled;
		bool holding; /* card sighted; hold mode-1 for the app's START */
		uint16_t mode0_dwell_ms;
		uint16_t mode1_dwell_ms;
		uint16_t hold_ms;
		uint32_t backoff_ms; /* probe refused; 0 = no backoff pending */
		tick_t tstamp; /* mode-0 dwell / backoff anchor */
		tick_t probe_tstamp; /* when mode-1 was actually entered */
		tick_t hold_tstamp; /* last card sighting */
	} scan;
};
#endif

enum osdp_pkt_errors_e {
	OSDP_ERR_PKT_NONE = 0,
	/**
	 * Fatal packet formatting issues. The phy layer was unable to find a
	 * valid OSDP packet or the length of the packet was too long/incorrect.
	 */
	OSDP_ERR_PKT_FMT = -1,
	/**
	 * Not enough data in buffer (but we have some); wait for more.
	 */
	OSDP_ERR_PKT_WAIT = -2,
	/**
	 * Message to/from an foreign device that can be safely ignored
	 * without altering the state of this PD.
	 */
	OSDP_ERR_PKT_SKIP = -3,
	/**
	 * Packet was valid but does not match some conditions. ie., only this
	 * packet is faulty, rest of the buffer may still be intact.
	 */
	OSDP_ERR_PKT_CHECK = -4,
	/**
	 * Discovered a busy packet. In CP mode, it should retry this command
	 * after some time.
	 */
	OSDP_ERR_PKT_BUSY = -5,
	/**
	 * Phy layer found a reason to send NACK to the CP that produced
	 * this packet; pd->reply_id is set REPLY_NAK and the reason code is
	 * also filled.
	 */
	OSDP_ERR_PKT_NACK = -6,
	/**
	 * Packet build errors
	 */
	OSDP_ERR_PKT_BUILD = -7,
	/**
	 * No data received (do not confuse with OSDP_ERR_PKT_WAIT)
	 */
	OSDP_ERR_PKT_NO_DATA = -8,
	/**
	 * Channel send returned 0 (EAGAIN): transport is momentarily not
	 * ready to queue the finalized bytes (half-duplex turnaround,
	 * previous TX draining, etc.). The caller must yield and re-send
	 * the same finalized buffer on the next refresh — no rebuild, no
	 * SC/seq state advance.
	 */
	OSDP_ERR_PKT_WAIT_TX = -9,
};

struct osdp_slab {
	int block_size;
	int num_blocks;
	int free_blocks;
	uint8_t *blob;
};

struct osdp_secure_channel {
	uint8_t scbk[16];
	uint8_t s_enc[16];
	uint8_t s_mac1[16];
	uint8_t s_mac2[16];
	uint8_t r_mac[16];
	uint8_t c_mac[16];
	uint8_t cp_random[8];
	uint8_t pd_random[8];
	uint8_t pd_client_uid[8];
	uint8_t cp_cryptogram[16];
	uint8_t pd_cryptogram[16];
};

struct osdp_rb {
	size_t head;
	size_t tail;
	uint8_t buffer[OSDP_RX_RB_SIZE];
};

struct osdp_rx_pkt {
	const uint8_t *buf;
	unsigned long len;
	unsigned long max_len;
};

struct osdp_pd {
	char name[OSDP_PD_NAME_MAXLEN];
	struct osdp *osdp_ctx; /* Ref to osdp * to access shared resources */
	int idx; /* Offset into osdp->pd[] for this PD */
	uint32_t flags; /* Used with: ISSET_FLAG, SET_FLAG, CLEAR_FLAG */

	uint32_t baud_rate; /* Serial baud/bit rate */
	int address; /* PD address */
	int seq_number; /* Current packet sequence number */
	struct osdp_pd_id id; /* PD ID information (as received from app) */

	/* PD Capability; Those received from app + implicit capabilities */
	struct osdp_pd_cap cap[OSDP_PD_CAP_SENTINEL];

	int state; /* FSM state (CP mode only) */
	int phy_state; /* phy layer FSM state (CP mode only) */
	int phy_retry_count; /* command retry counter */
	int phy_tx_seq; /* seq number embedded in last TX packet */
	uint32_t wait_ms; /* wait time in MS to retry communication */
	tick_t tstamp; /* Last POLL command issued time in ticks */
	tick_t sc_tstamp; /* Last received secure reply time in ticks */
	tick_t phy_tstamp; /* Time in ticks since command was sent */
	uint32_t resp_timeout_ms; /* Wait time in MS before a response is due */
	uint32_t request; /* Event loop requests */

	uint16_t peer_rx_size; /* Receive buffer size of the peer PD/CP */

	/* Raw bytes received from the serial line for this PD */
#ifdef OPT_OSDP_RX_ZERO_COPY
	struct osdp_rx_pkt *rx_pkt;
#else /* OPT_OSDP_RX_ZERO_COPY */
	struct osdp_rb *rx_rb;
#endif /* OPT_OSDP_RX_ZERO_COPY */

	uint8_t *packet_buf;
	unsigned long packet_len;
	unsigned long packet_buf_len;
	uint32_t packet_scan_skip;
	/* A finalized packet is parked in packet_buf awaiting channel-send.
	 * Set by the build step (fresh reply, prebuilt status reply, or
	 * EAGAIN retry promotion); cleared after the send completes. While
	 * set, the update loop skips RX and only retries the send — a new
	 * RX would advance seq and stale the cached bytes. */
	bool reply_prebuilt;

	/* Retransmit cache: last successfully-sent reply bytes live in the
	 * shared tx_buf as long as nothing overwrites them. On a sequence
	 * repeat we re-emit those bytes verbatim (see phy_check_packet). */
	uint16_t last_tx_len; /* 0 = cache empty */
	uint8_t last_cmd_id;

	int cmd_id; /* Currently processing command ID */
	int reply_id; /* Currently processing reply ID */
	union {
		uint8_t nak_code;
		uint8_t keyset_pending[16];
		struct {
			uint8_t address;
			uint32_t baud_rate;
		} comset_pending;
	};

	union {
		queue_t cmd_queue;
		queue_t event_queue;
	};
	const struct osdp_cmd *active_cmd;      /* in-flight cmd (app-owned mode) */
	const struct osdp_event *active_event;  /* in-flight event (app-owned mode) */

	struct osdp_secure_channel sc;   /* Secure Channel session context */
	struct osdp_file *file;          /* File transfer context */
	struct osdp_piv *piv;            /* Smartcard/PIV multipart context */
	struct osdp_bio *bio;            /* Biometric read multipart context */
	struct osdp_metrics metrics;     /* link/protocol health counters */
#ifdef OPT_BUILD_OSDP_TRS
	struct osdp_trs trs; /* TRS mode context (embedded) */
#endif

	/* PD command callback to app with opaque arg pointer as passed by app */
	void *command_callback_arg;
	pd_command_callback_t command_callback;
	void *event_completion_callback_arg;
	pd_event_completion_callback_t event_completion_callback;

#ifndef __BARE_METAL__
	/* Opaque packet capture pointer (see osdp_pcap.c) */
	void *packet_capture_ctx;
#endif
};

struct osdp {
	uint32_t _magic; /* Canary to be used in input_check() */
	int _num_pd; /* Number of PDs attached to this context */
	struct osdp_pd *_current_pd; /* current operational pd's pointer */
	struct osdp_pd *pd; /* base of PD list (must be at lest one) */
	struct osdp_channel channel; /* OSDP channel */
	uint8_t tx_buf[OSDP_PACKET_BUF_SIZE];
	uint8_t *rx_buf; /* RX landing buffer: aliased to tx_buf in CP; distinct in PD */

	/* CP event callback to app with opaque arg pointer as passed by app */
	void *event_callback_arg;
	cp_event_callback_t event_callback;
	void *command_completion_callback_arg;
	cp_command_completion_callback_t command_completion_callback;

#ifndef OPT_OSDP_LOG_MINIMAL
	logger_t logger; /* logger context (from utils/logger.h) */
#endif
};

void osdp_keyset_complete(struct osdp_pd *pd);

#ifdef OPT_BUILD_OSDP_TRS
/* --- from osdp_trs.c --- */
enum osdp_cp_state_e osdp_trs_state_update(struct osdp_pd *pd);
enum osdp_cp_state_e osdp_trs_state_update_err(struct osdp_pd *pd);
#endif

/* --- from osdp_phy.c --- */
int osdp_phy_packet_init(struct osdp_pd *p, uint8_t *buf, int max_len);
int osdp_phy_check_packet(struct osdp_pd *pd);
int osdp_phy_decode_packet(struct osdp_pd *p, uint8_t **pkt_start);
void osdp_phy_state_reset(struct osdp_pd *pd, bool is_error);
int osdp_phy_packet_get_data_offset(struct osdp_pd *p, const uint8_t *buf);
uint8_t *osdp_phy_packet_get_smb(struct osdp_pd *p, const uint8_t *buf);
int osdp_phy_finalize_packet(struct osdp_pd *pd, uint8_t *buf, int len,
			     int max_len);
int osdp_phy_send_packet(struct osdp_pd *pd, uint8_t *buf, int len);
void osdp_phy_progress_sequence(struct osdp_pd *pd);
void osdp_phy_release_packet(struct osdp_pd *pd);

/* --- from osdp_common.c --- */
#ifdef UTILS_UNKNOWN_TARGET
/* Strong declaration: a target utils cannot identify must supply the tick
 * source itself, and a missing one must be a link error. A __weak undefined
 * reference would quietly resolve to NULL and be called. */
tick_t osdp_millis_now(void);
#else
__weak tick_t osdp_millis_now(void);
#endif
tick_t osdp_millis_since(tick_t last);
uint16_t osdp_compute_crc16(const uint8_t *buf, size_t len);

const char *osdp_cmd_name(int cmd_id);
const char *osdp_reply_name(int reply_id);

/* Abort every feature engine (file/piv/bio) on this PD. The single owner of
 * "full engine teardown" -- call it from every link-down path (CP going
 * OFFLINE/DISABLED, PD losing the CP, CMD_ABORT) instead of listing engines
 * at the call site. Each engine's abort is idempotent. */
void osdp_engines_abort(struct osdp_pd *pd);

/* Any multi-part engine mid-transfer on this PD? §5.10.2 forbids interleaving
 * multi-part transfers; every engine's submit path must check this. */
bool osdp_mp_engine_busy(struct osdp_pd *pd);

/* Targeted reader number for commands/events that carry one, else -1. */
int osdp_cmd_reader_no(const struct osdp_cmd *cmd);
int osdp_event_reader_no(const struct osdp_event *event);

int osdp_rb_push(struct osdp_rb *p, uint8_t data);
int osdp_rb_push_buf(struct osdp_rb *p, uint8_t *buf, int len);
int osdp_rb_pop(struct osdp_rb *p, uint8_t *data);
int osdp_rb_pop_buf(struct osdp_rb *p, uint8_t *buf, int max_len);
void osdp_rb_reset(struct osdp_rb *p);

void osdp_crypt_setup();
void osdp_encrypt(uint8_t *key, uint8_t *iv, uint8_t *data, int len);
void osdp_decrypt(uint8_t *key, uint8_t *iv, uint8_t *data, int len);
void osdp_fill_random(uint8_t *buf, int len);
void osdp_fill_zeros(void *buf, int len);
void osdp_crypt_teardown();

/* --- from osdp_sc.c --- */
void osdp_compute_scbk(struct osdp_pd *pd, uint8_t *master_key, uint8_t *scbk);
void osdp_compute_session_keys(struct osdp_pd *pd);
void osdp_compute_cp_cryptogram(struct osdp_pd *pd);
int osdp_verify_cp_cryptogram(struct osdp_pd *pd);
void osdp_compute_pd_cryptogram(struct osdp_pd *pd);
int osdp_verify_pd_cryptogram(struct osdp_pd *pd);
void osdp_compute_rmac_i(struct osdp_pd *pd);
int osdp_decrypt_data(struct osdp_pd *pd, int is_cmd, uint8_t *data, int len);
int osdp_encrypt_data(struct osdp_pd *pd, int is_cmd, uint8_t *data, int len);
int osdp_compute_mac(struct osdp_pd *pd, int is_cmd, const uint8_t *data,
		     int len);
void osdp_sc_setup(struct osdp_pd *pd);
void osdp_sc_teardown(struct osdp_pd *pd);

/* --- Little-endian readers --- */

static inline uint16_t bread_u16_le(const uint8_t *buf, int *pos)
{
	uint32_t v = buf[(*pos)++];
	v |= (uint32_t)buf[(*pos)++] << 8;
	return (uint16_t)v;
}

static inline uint32_t bread_u24_le(const uint8_t *buf, int *pos)
{
	uint32_t v = buf[(*pos)++];
	v |= (uint32_t)buf[(*pos)++] << 8;
	v |= (uint32_t)buf[(*pos)++] << 16;
	return v;
}

static inline uint32_t bread_u32_le(const uint8_t *buf, int *pos)
{
	uint32_t v = buf[(*pos)++];
	v |= (uint32_t)buf[(*pos)++] << 8;
	v |= (uint32_t)buf[(*pos)++] << 16;
	v |= (uint32_t)buf[(*pos)++] << 24;
	return v;
}

/* --- Little-endian writers --- */

static inline void bwrite_u8(uint8_t val, uint8_t *buf, int *len)
{
	buf[(*len)++] = val;
}

static inline void bwrite_u16_le(uint16_t val, uint8_t *buf, int *len)
{
	buf[(*len)++] = BYTE_0(val);
	buf[(*len)++] = BYTE_1(val);
}

static inline void bwrite_u24_le(uint32_t val, uint8_t *buf, int *len)
{
	buf[(*len)++] = BYTE_0(val);
	buf[(*len)++] = BYTE_1(val);
	buf[(*len)++] = BYTE_2(val);
}

static inline void bwrite_u32_le(uint32_t val, uint8_t *buf, int *len)
{
	buf[(*len)++] = BYTE_0(val);
	buf[(*len)++] = BYTE_1(val);
	buf[(*len)++] = BYTE_2(val);
	buf[(*len)++] = BYTE_3(val);
}

/* --- Big-endian readers --- */

static inline uint16_t bread_u16_be(const uint8_t *buf, int *pos)
{
	uint32_t v = (uint32_t)buf[(*pos)++] << 8;
	v |= buf[(*pos)++];
	return (uint16_t)v;
}

static inline uint32_t bread_u24_be(const uint8_t *buf, int *pos)
{
	uint32_t v = (uint32_t)buf[(*pos)++] << 16;
	v |= (uint32_t)buf[(*pos)++] << 8;
	v |= buf[(*pos)++];
	return v;
}

static inline uint32_t bread_u32_be(const uint8_t *buf, int *pos)
{
	uint32_t v = (uint32_t)buf[(*pos)++] << 24;
	v |= (uint32_t)buf[(*pos)++] << 16;
	v |= (uint32_t)buf[(*pos)++] << 8;
	v |= buf[(*pos)++];
	return v;
}

/* --- Big-endian writers --- */

static inline void bwrite_u16_be(uint16_t val, uint8_t *buf, int *len)
{
	buf[(*len)++] = BYTE_1(val);
	buf[(*len)++] = BYTE_0(val);
}

static inline void bwrite_u24_be(uint32_t val, uint8_t *buf, int *len)
{
	buf[(*len)++] = BYTE_2(val);
	buf[(*len)++] = BYTE_1(val);
	buf[(*len)++] = BYTE_0(val);
}

static inline void bwrite_u32_be(uint32_t val, uint8_t *buf, int *len)
{
	buf[(*len)++] = BYTE_3(val);
	buf[(*len)++] = BYTE_2(val);
	buf[(*len)++] = BYTE_1(val);
	buf[(*len)++] = BYTE_0(val);
}

/* --- Other Helpers --- */

static inline int get_tx_buf_size(struct osdp_pd *pd)
{
	int packet_buf_size = OSDP_PACKET_BUF_SIZE;

	if (pd->peer_rx_size) {
		if (packet_buf_size > (int)pd->peer_rx_size)
			packet_buf_size = (int)pd->peer_rx_size;
	}
	return packet_buf_size;
}

static inline struct osdp *pd_to_osdp(struct osdp_pd *pd)
{
	return pd->osdp_ctx;
}

static inline struct osdp_pd *osdp_to_pd(const struct osdp *ctx, int pd_idx)
{
	return ctx->pd + pd_idx;
}

static inline bool is_pd_mode(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_PD_MODE);
}

static inline bool is_cp_mode(struct osdp_pd *pd)
{
	return !ISSET_FLAG(pd, PD_FLAG_PD_MODE);
}

static inline uint8_t *osdp_tx_staging_buf(struct osdp_pd *pd)
{
	return pd_to_osdp(pd)->tx_buf;
}

static inline bool sc_use_scbkd(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
}

static inline bool sc_is_capable(struct osdp_pd *pd)
{
	return (ISSET_FLAG(pd, PD_FLAG_SC_CAPABLE) &&
		!ISSET_FLAG(pd, PD_FLAG_SC_DISABLED));
}

static inline bool sc_is_active(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_SC_ACTIVE);
}

static inline void sc_activate(struct osdp_pd *pd)
{
	SET_FLAG(pd, PD_FLAG_SC_ACTIVE);
}

static inline void sc_deactivate(struct osdp_pd *pd)
{
	if (sc_is_active(pd)) {
		osdp_sc_teardown(pd);
	}
	CLEAR_FLAG(pd, PD_FLAG_SC_ACTIVE);
	/* Cached retransmit reply is no longer meaningful without SC. */
	pd->last_tx_len = 0;
}

static inline bool is_pd_online(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_ONLINE);
}

static inline void pd_set_online(struct osdp_pd *pd)
{
	SET_FLAG(pd, PD_FLAG_ONLINE);
}

static inline void pd_set_offline(struct osdp_pd *pd)
{
	CLEAR_FLAG(pd, PD_FLAG_ONLINE);
}

static inline void make_request(struct osdp_pd *pd, uint32_t req)
{
	pd->request |= req;
}
static inline bool trs_capable(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_TRS_CAPABLE);
}

static inline bool trs_active(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_TRS_ACTIVE);
}

static inline void trs_set_reply_error(struct osdp_pd *pd, bool error)
{
#ifdef OPT_BUILD_OSDP_TRS
	pd->trs.reply_error = error;
#else
	ARG_UNUSED(pd);
	ARG_UNUSED(error);
#endif
}

static inline bool trs_reply_error(struct osdp_pd *pd)
{
#ifdef OPT_BUILD_OSDP_TRS
	return pd->trs.reply_error;
#else
	ARG_UNUSED(pd);
	return false;
#endif
}

/*
 * A TRS session runs on top of an established link: the PD is still online,
 * just busy with a card. Use this over a bare `state == ONLINE` test wherever
 * the question is "is this PD up and serving the app?".
 */
static inline bool cp_is_online(struct osdp_pd *pd)
{
#ifdef OPT_BUILD_OSDP_TRS
	if (pd->state == OSDP_CP_STATE_TRS_SETUP ||
	    pd->state == OSDP_CP_STATE_TRS_RUN) {
		return true;
	}
#endif
	return pd->state == OSDP_CP_STATE_ONLINE;
}

static inline bool check_request(struct osdp_pd *pd, uint32_t req)
{
	if (pd->request & req) {
		pd->request &= ~req;
		return true;
	}
	return false;
}

static inline bool test_request(struct osdp_pd *pd, uint32_t req)
{
	return pd->request & req;
}

static inline bool is_capture_enabled(struct osdp_pd *pd)
{
	return (ISSET_FLAG(pd, PD_FLAG_CAPTURE_PKT) &&
		(IS_ENABLED(OPT_OSDP_PACKET_TRACE) ||
		 IS_ENABLED(OPT_OSDP_DATA_TRACE)));
}

static inline bool is_data_trace_enabled(struct osdp_pd *pd)
{
	return (ISSET_FLAG(pd, PD_FLAG_CAPTURE_PKT) &&
		IS_ENABLED(OPT_OSDP_DATA_TRACE));
}

static inline bool is_packet_trace_enabled(struct osdp_pd *pd)
{
	return (ISSET_FLAG(pd, PD_FLAG_CAPTURE_PKT) &&
		IS_ENABLED(OPT_OSDP_PACKET_TRACE));
}

static inline bool sc_allow_empty_encrypted_data_block(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_ALLOW_EMPTY_EDB);
}

static inline bool is_bioreadr_multipart(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_BIOREADR_MP);
}

static inline bool is_enforce_secure(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_ENFORCE_SECURE);
}

static inline bool is_notifications_enabled(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_ENABLE_NOTIF);
}

static inline bool is_ignore_unsolicited_messages(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_IGNORE_USR);
}

static inline bool is_install_mode(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_INSTALL_MODE);
}

/* --- CP Alloc Helpers --- */

#ifdef OPT_OSDP_STATIC

#ifdef OPT_OSDP_EXCLUSIVE_ROLE
/*
 * Exclusive-role arena: a context is either CP or PD. When a build only ever runs
 * one role at a time, the inactive role's pool sits idle for the lifetime of
 * the active one, so overlaying the CP and PD static pools in a union reclaims
 * the smaller (PD) pool's worth of BSS.
 *
 * Opt-in only: leaving OPT_OSDP_EXCLUSIVE_ROLE undefined keeps the CP and PD
 * pools as independent storage, which is required when a CP and a PD context
 * are instantiated at the same time. One external instance (defined in
 * osdp_common.c) is shared across the osdp_cp.c / osdp_pd.c TUs.
 */
union osdp_role_arena {
	struct {
		struct osdp ctx;
		struct osdp_pd pd[OSDP_CP_MAX_PDS];
#ifdef OPT_OSDP_RX_ZERO_COPY
		struct osdp_rx_pkt rx_pkt[OSDP_CP_MAX_PDS];
#else
		struct osdp_rb rb[OSDP_CP_MAX_PDS];
#endif
	} cp;
	struct {
		struct osdp ctx;
		struct osdp_pd pd;
		uint8_t rx_buf[OSDP_PACKET_BUF_SIZE];
#ifdef OPT_OSDP_RX_ZERO_COPY
		struct osdp_rx_pkt rx_pkt;
#else
		struct osdp_rb rb;
#endif
	} pd;
};
extern union osdp_role_arena g_osdp_role_arena;
#endif /* OPT_OSDP_EXCLUSIVE_ROLE */

static inline struct osdp *cp_static_ctx_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return &g_osdp_role_arena.cp.ctx;
#else
	static struct osdp g_osdp_ctx;
	return &g_osdp_ctx;
#endif
}

static inline struct osdp_pd *cp_static_pd_array_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return g_osdp_role_arena.cp.pd;
#else
	static struct osdp_pd g_cp_pd_ctx[OSDP_CP_MAX_PDS];
	return g_cp_pd_ctx;
#endif
}

#ifdef OPT_OSDP_RX_ZERO_COPY
static inline struct osdp_rx_pkt *cp_static_rx_pkt_array_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return g_osdp_role_arena.cp.rx_pkt;
#else
	static struct osdp_rx_pkt g_cp_rx_pkt[OSDP_CP_MAX_PDS];
	return g_cp_rx_pkt;
#endif
}
#else /* OPT_OSDP_RX_ZERO_COPY */
static inline struct osdp_rb *cp_static_rx_rb_array_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return g_osdp_role_arena.cp.rb;
#else
	static struct osdp_rb g_cp_rb[OSDP_CP_MAX_PDS];
	return g_cp_rb;
#endif
}
#endif /* OPT_OSDP_RX_ZERO_COPY */
#endif /* OPT_OSDP_STATIC */

static inline struct osdp *cp_ctx_alloc(void)
{
	struct osdp *ctx;
#ifdef OPT_OSDP_STATIC
	ctx = cp_static_ctx_get();
	memset(ctx, 0, sizeof(struct osdp));
#else
	ctx = calloc(1, sizeof(struct osdp));
	if (!ctx) {
		return NULL;
	}
#endif /* OPT_OSDP_STATIC */
	/* CP mode does not cache retransmit replies; alias rx_buf to tx_buf
	 * to preserve the legacy shared-buffer behavior with zero cost. */
	ctx->rx_buf = ctx->tx_buf;
	return ctx;
}

static inline struct osdp_pd *cp_pd_array_alloc(int old_num_pd, int num_pd)
{
#ifdef OPT_OSDP_STATIC
	struct osdp_pd *pd = cp_static_pd_array_get();

	if (old_num_pd + num_pd > OSDP_CP_MAX_PDS) {
		return NULL;
	}
	memset(pd + old_num_pd, 0, sizeof(struct osdp_pd) * num_pd);
	return pd;
#else
	return calloc(old_num_pd + num_pd, sizeof(struct osdp_pd));
#endif
}

#ifdef OPT_OSDP_RX_ZERO_COPY

static inline struct osdp_rx_pkt *cp_rx_pkt_alloc(int pd_idx)
{
#ifdef OPT_OSDP_STATIC
	struct osdp_rx_pkt *rx_pkt = cp_static_rx_pkt_array_get();
	memset(&rx_pkt[pd_idx], 0, sizeof(struct osdp_rx_pkt));
	return &rx_pkt[pd_idx];
#else
	ARG_UNUSED(pd_idx);
	return calloc(1, sizeof(struct osdp_rx_pkt));
#endif
}

#else /* OPT_OSDP_RX_ZERO_COPY */

static inline struct osdp_rb *cp_rx_rb_alloc(int pd_idx)
{
#ifdef OPT_OSDP_STATIC
	struct osdp_rb *rx_rb = cp_static_rx_rb_array_get();
	memset(&rx_rb[pd_idx], 0, sizeof(struct osdp_rb));
	return &rx_rb[pd_idx];
#else
	ARG_UNUSED(pd_idx);
	return calloc(1, sizeof(struct osdp_rb));
#endif
}

#endif /* OPT_OSDP_RX_ZERO_COPY */

/* --- PD Alloc Helpers --- */

#ifdef OPT_OSDP_STATIC

static inline struct osdp *pd_static_ctx_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return &g_osdp_role_arena.pd.ctx;
#else
	static struct osdp g_osdp_ctx;
	return &g_osdp_ctx;
#endif
}

static inline struct osdp_pd *pd_static_ctx_pd_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return &g_osdp_role_arena.pd.pd;
#else
	static struct osdp_pd g_osdp_pd_ctx;
	return &g_osdp_pd_ctx;
#endif
}

static inline uint8_t *pd_static_rx_buf_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return g_osdp_role_arena.pd.rx_buf;
#else
	static uint8_t g_osdp_pd_rx_buf[OSDP_PACKET_BUF_SIZE];
	return g_osdp_pd_rx_buf;
#endif
}

#ifdef OPT_OSDP_RX_ZERO_COPY

static inline struct osdp_rx_pkt *pd_static_rx_pkt_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return &g_osdp_role_arena.pd.rx_pkt;
#else
	static struct osdp_rx_pkt g_osdp_rx_pkt;
	return &g_osdp_rx_pkt;
#endif
}

#else /* OPT_OSDP_RX_ZERO_COPY */

static inline struct osdp_rb *pd_static_rx_rb_get(void)
{
#ifdef OPT_OSDP_EXCLUSIVE_ROLE
	return &g_osdp_role_arena.pd.rb;
#else
	static struct osdp_rb g_osdp_rb;
	return &g_osdp_rb;
#endif
}

#endif /* OPT_OSDP_RX_ZERO_COPY */

#endif /* OPT_OSDP_STATIC */

static inline struct osdp *pd_ctx_alloc(void)
{
	struct osdp *ctx;
#ifdef OPT_OSDP_STATIC
	ctx = pd_static_ctx_get();
	memset(ctx, 0, sizeof(struct osdp));
	ctx->rx_buf = pd_static_rx_buf_get();
#else
	ctx = calloc(1, sizeof(struct osdp));
	if (!ctx) {
		return NULL;
	}
	ctx->rx_buf = calloc(1, OSDP_PACKET_BUF_SIZE);
	if (!ctx->rx_buf) {
		free(ctx);
		return NULL;
	}
#endif /* OPT_OSDP_STATIC */
	return ctx;
}

static inline struct osdp_pd *pd_instance_alloc(void)
{
#ifdef OPT_OSDP_STATIC
	struct osdp_pd *pd = pd_static_ctx_pd_get();
	memset(pd, 0, sizeof(struct osdp_pd));
	return pd;
#else
	return calloc(1, sizeof(struct osdp_pd));
#endif /* OPT_OSDP_STATIC */
}

#ifdef OPT_OSDP_RX_ZERO_COPY
static inline struct osdp_rx_pkt *pd_rx_pkt_alloc(void)
{
#ifdef OPT_OSDP_STATIC
	struct osdp_rx_pkt *rx_pkt = pd_static_rx_pkt_get();
	memset(rx_pkt, 0, sizeof(struct osdp_rx_pkt));
	return rx_pkt;
#else
	return calloc(1, sizeof(struct osdp_rx_pkt));
#endif /* OPT_OSDP_STATIC */
}
#else /* OPT_OSDP_RX_ZERO_COPY */
static inline struct osdp_rb *pd_rx_rb_alloc(void)
{
#ifdef OPT_OSDP_STATIC
	struct osdp_rb *rx_rb = pd_static_rx_rb_get();
	memset(rx_rb, 0, sizeof(struct osdp_rb));
	return rx_rb;
#else
	return calloc(1, sizeof(struct osdp_rb));
#endif /* OPT_OSDP_STATIC */
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

#endif /* _OSDP_COMMON_H_ */
