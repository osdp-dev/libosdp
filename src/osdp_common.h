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

#define OSDP_CTX_MAGIC 0xDEADBEAF

#define ARG_UNUSED(x) (void)(x)

#define LOG_EM(...)    __logger_log(&pd->logger, LOG_EMERG,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ALERT(...) __logger_log(&pd->logger, LOG_ALERT,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_CRIT(...)  __logger_log(&pd->logger, LOG_CRIT,   __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERR(...)   __logger_log(&pd->logger, LOG_ERR,    __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INF(...)   __logger_log(&pd->logger, LOG_INFO,   __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WRN(...)   __logger_log(&pd->logger, LOG_WARNING,__FILE__, __LINE__, __VA_ARGS__)
#define LOG_WRN_ONCE(...) \
do {\
  static int warned = 0; \
  if(!warned) { \
    __logger_log(&pd->logger, LOG_WARNING,__FILE__, __LINE__, __VA_ARGS__);\
    warned = 1;\
  }\
}while(0)
#define LOG_NOT(...)   __logger_log(&pd->logger, LOG_NOTICE, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DBG(...)   __logger_log(&pd->logger, LOG_DEBUG,  __FILE__, __LINE__, __VA_ARGS__)

#define ISSET_FLAG(p, f)       (((p)->flags & (f)) == (f))
#define SET_FLAG(p, f)          ((p)->flags |= (f))
#define CLEAR_FLAG(p, f)        ((p)->flags &= ~(f))
#define SET_FLAG_V(p, f, v)     if ((v)) SET_FLAG(p, f); else CLEAR_FLAG(p, f);

#define BYTE_0(x) (uint8_t)(((x) >> 0) & 0xFF)
#define BYTE_1(x) (uint8_t)(((x) >> 8) & 0xFF)
#define BYTE_2(x) (uint8_t)(((x) >> 16) & 0xFF)
#define BYTE_3(x) (uint8_t)(((x) >> 24) & 0xFF)

/* casting helpers */
#define TO_OSDP(ctx)  ((struct osdp *)ctx)

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
	exit(EXIT_FAILURE);
	__unreachable();
}

#define BUG() \
	do { \
		printf("BUG at %s:%d %s(). Please report this issue!", \
		       __FILE__, __LINE__, __func__); \
		die(); \
	} while (0)

#define BUG_ON(pred) \
	do { \
		if (unlikely(pred)) { \
			BUG(); \
		} \
	} while (0)

/* Unused type only to estimate ephemeral_data size */
union osdp_ephemeral_data {
	struct osdp_cmd cmd;
	struct osdp_event event;
};
#define OSDP_EPHEMERAL_DATA_MAX_LEN sizeof(union osdp_ephemeral_data)

/**
 * OSDP application exposed method arg checker.
 *
 * Usage:
 *    input_check(ctx);
 *    input_check(ctx, pd);
 */
#define input_check_init(_ctx) do { \
		struct osdp *__ctx = (struct osdp *)_ctx; \
		assert(__ctx); \
		__ctx->_magic = OSDP_CTX_MAGIC; \
	} while (0)
#define input_check_osdp_ctx(_ctx) do { \
		struct osdp *__ctx = (struct osdp *)_ctx; \
		BUG_ON(__ctx  == NULL); \
		BUG_ON(__ctx->_magic != OSDP_CTX_MAGIC); \
	} while (0)
#define input_check_pd_offset(_ctx, _pd) do { \
		struct osdp *__ctx = (struct osdp *)_ctx; \
		int __pd = _pd; \
		if (__pd < 0 || __pd >= __ctx->_num_pd) { \
			LOG_PRINT("Invalid PD number %d", __pd); \
			return -1; \
		} \
	} while (0)
#define input_check2(_1, _2) \
	input_check_osdp_ctx(_1); \
	input_check_pd_offset(_1, _2);
#define input_check1(_1) \
	input_check_osdp_ctx(_1);
#define get_macro(_1, _2, macro, ...) macro
#define input_check(...) \
	get_macro(__VA_ARGS__, input_check2, input_check1)(__VA_ARGS__)

/**
 * @brief OSDP reserved commands
 */
#define CMD_INVALID      0x00
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
#define CMD_RMODE	 0x6C
#define CMD_TDSET	 0x6D
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
#define REPLY_INVALID   0x00
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
#define PD_FLAG_SC_CAPABLE     BIT(0)  /* PD secure channel capable */
#define PD_FLAG_TAMPER         BIT(1)  /* local tamper status */
#define PD_FLAG_POWER          BIT(2)  /* local power status */
#define PD_FLAG_R_TAMPER       BIT(3)  /* remote tamper status */
#define PD_FLAG_SKIP_SEQ_CHECK BIT(5)  /* disable seq checks (debug) */
#define PD_FLAG_SC_USE_SCBKD   BIT(6)  /* in this SC attempt, use SCBKD */
#define PD_FLAG_SC_ACTIVE      BIT(7)  /* secure channel is active */
#define PD_FLAG_PD_MODE        BIT(8)  /* device is setup as PD */
#define PD_FLAG_CHN_SHARED     BIT(9)  /* PD's channel is shared */
#define PD_FLAG_PKT_SKIP_MARK  BIT(10) /* OPT_OSDP_SKIP_MARK_BYTE */
#define PD_FLAG_PKT_HAS_MARK   BIT(11) /* Packet has mark byte */
#define PD_FLAG_SC_DISABLED    BIT(12) /* master_key=NULL && scbk=NULL */
#define PD_FLAG_PKT_BROADCAST  BIT(13) /* this packet was addressed to 0x7F */
#define PD_FLAG_CP_USE_CRC     BIT(14) /* CP uses CRC-16 instead of checksum */
#define PD_FLAG_SC_NO_KEY      BIT(15) /* PD: this SC attempt has no key */

/* PD Init flags */
#define PD_FLAG_ENFORCE_SECURE  BIT(24) /* See: OSDP_FLAG_ENFORCE_SECURE */
#define PD_FLAG_INSTALL_MODE    BIT(25) /* See: OSDP_FLAG_INSTALL_MODE */
#define PD_FLAG_IGNORE_USR      BIT(26) /* See: OSDP_FLAG_IGNORE_USR */
#define PD_FLAG_ENABLE_NOTIF    BIT(27) /* See: OSDP_FLAG_ENABLE_NOTIFICATION */
#define PD_FLAG_CAPTURE_PKT     BIT(28) /* See: OSDP_FLAG_CAPTURE_PACKETS */
#define PD_FLAG_ALLOW_EMPTY_EDB BIT(29) /* See: OSDP_FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK */

/* CP event requests; used with make_request() and check_request() */
#define CP_REQ_RESTART_SC              0x00000001
#define CP_REQ_EVENT_SEND              0x00000002
#define CP_REQ_OFFLINE                 0x00000004
#define CP_REQ_DISABLE                 0x00000008
#define CP_REQ_ENABLE                  0x00000010

enum osdp_cp_phy_state_e {
	OSDP_CP_PHY_STATE_IDLE,
	OSDP_CP_PHY_STATE_SEND_CMD,
	OSDP_CP_PHY_STATE_REPLY_WAIT,
	OSDP_CP_PHY_STATE_WAIT,
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
	OSDP_CP_STATE_PROBE,
	OSDP_CP_STATE_OFFLINE,
	OSDP_CP_STATE_DISABLED,
	OSDP_CP_STATE_SENTINEL
};

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

#define OSDP_APP_DATA_QUEUE_SIZE \
	(OSDP_CP_CMD_POOL_SIZE * sizeof(union osdp_ephemeral_data))

struct osdp_app_data_pool {
	slab_t slab;
	uint8_t slab_blob[OSDP_APP_DATA_QUEUE_SIZE];
};


struct osdp_pd {
	char name[OSDP_PD_NAME_MAXLEN];
	struct osdp *osdp_ctx; /* Ref to osdp * to access shared resources */
	int idx;               /* Offset into osdp->pd[] for this PD */
	uint32_t flags;        /* Used with: ISSET_FLAG, SET_FLAG, CLEAR_FLAG */

	uint32_t baud_rate;    /* Serial baud/bit rate */
	int address;           /* PD address */
	int seq_number;        /* Current packet sequence number */
	struct osdp_pd_id id;  /* PD ID information (as received from app) */

	/* PD Capability; Those received from app + implicit capabilities */
	struct osdp_pd_cap cap[OSDP_PD_CAP_SENTINEL];

	int state;             /* FSM state (CP mode only) */
	int phy_state;         /* phy layer FSM state (CP mode only) */
	int phy_retry_count;   /* command retry counter */
	int phy_tx_seq;        /* seq number embedded in last TX packet */
	uint32_t wait_ms;      /* wait time in MS to retry communication */
	int64_t tstamp;        /* Last POLL command issued time in ticks */
	int64_t sc_tstamp;     /* Last received secure reply time in ticks */
	int64_t phy_tstamp;    /* Time in ticks since command was sent */
	uint32_t request;      /* Event loop requests */

	uint16_t peer_rx_size; /* Receive buffer size of the peer PD/CP */

	/* Raw bytes received from the serial line for this PD */
	union {
		struct osdp_rb *rb;
		struct osdp_rx_pkt *pkt;
	} rx;

	uint8_t *packet_buf;
	uint8_t packet_buf_store[OSDP_PACKET_BUF_SIZE];
	unsigned long packet_len;
	unsigned long packet_buf_len;
	uint32_t packet_scan_skip;

	int cmd_id;            /* Currently processing command ID */
	int reply_id;          /* Currently processing reply ID */

	/* Data bytes of the current command/reply ID */
	uint8_t ephemeral_data[OSDP_EPHEMERAL_DATA_MAX_LEN];

	union {
		queue_t cmd_queue;
		queue_t event_queue;
	};
	const struct osdp_cmd *active_cmd;      /* in-flight cmd (app-owned mode) */
	const struct osdp_event *active_event;  /* in-flight event (app-owned mode) */
#ifndef OPT_OSDP_APP_OWNED_QUEUE_DATA
	struct osdp_app_data_pool app_data;  /* alloc osdp_event / osdp_cmd */
#endif

	struct osdp_channel channel;     /* PD's serial channel */
	struct osdp_secure_channel sc;   /* Secure Channel session context */
	struct osdp_file *file;          /* File transfer context */

	/* PD command callback to app with opaque arg pointer as passed by app */
	void *command_callback_arg;
	pd_command_callback_t command_callback;
	void *event_completion_callback_arg;
	pd_event_completion_callback_t event_completion_callback;

	/* logger context (from utils/logger.h) */
	logger_t logger;

	/* Opaque packet capture pointer (see osdp_pcap.c) */
	void *packet_capture_ctx;
};

struct osdp {
	uint32_t _magic;       /* Canary to be used in input_check() */
	int _num_pd;           /* Number of PDs attached to this context */
	struct osdp_pd *_current_pd; /* current operational pd's pointer */
	struct osdp_pd *pd;    /* base of PD list (must be at lest one) */
	int num_channels;      /* Number of distinct channels */
	int *channel_lock;     /* array of length NUM_PD() to lock a channel */

	/* CP event callback to app with opaque arg pointer as passed by app */
	void *event_callback_arg;
	cp_event_callback_t event_callback;
	void *command_completion_callback_arg;
	cp_command_completion_callback_t command_completion_callback;
};

void osdp_keyset_complete(struct osdp_pd *pd);

/* --- from osdp_phy.c --- */
int osdp_phy_packet_init(struct osdp_pd *p, uint8_t *buf, int max_len);
int osdp_phy_check_packet(struct osdp_pd *pd);
int osdp_phy_decode_packet(struct osdp_pd *p, uint8_t **pkt_start);
void osdp_phy_state_reset(struct osdp_pd *pd, bool is_error);
int osdp_phy_packet_get_data_offset(struct osdp_pd *p, const uint8_t *buf);
uint8_t *osdp_phy_packet_get_smb(struct osdp_pd *p, const uint8_t *buf);
int osdp_phy_send_packet(struct osdp_pd *pd, uint8_t *buf,
                         int len, int max_len);
void osdp_phy_progress_sequence(struct osdp_pd *pd);
void osdp_phy_release_packet(struct osdp_pd *pd);

/* --- from osdp_common.c --- */
__weak int64_t osdp_millis_now(void);
int64_t osdp_millis_since(int64_t last);
uint16_t osdp_compute_crc16(const uint8_t *buf, size_t len);

const char *osdp_cmd_name(int cmd_id);
const char *osdp_reply_name(int reply_id);

int osdp_rb_push(struct osdp_rb *p, uint8_t data);
int osdp_rb_push_buf(struct osdp_rb *p, uint8_t *buf, int len);
int osdp_rb_pop(struct osdp_rb *p, uint8_t *data);
int osdp_rb_pop_buf(struct osdp_rb *p, uint8_t *buf, int max_len);
void osdp_rb_reset(struct osdp_rb *p);

void osdp_crypt_setup();
void osdp_encrypt(uint8_t *key, uint8_t *iv, uint8_t *data, int len);
void osdp_decrypt(uint8_t *key, uint8_t *iv, uint8_t *data, int len);
void osdp_fill_random(uint8_t *buf, int len);
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
int osdp_compute_mac(struct osdp_pd *pd, int is_cmd,
		     const uint8_t *data, int len);
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

static inline bool is_channel_shared(struct osdp_pd *pd) {
	return ISSET_FLAG(pd, PD_FLAG_CHN_SHARED);
}

static inline bool sc_use_scbkd(struct osdp_pd *pd) {
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
}

static inline void make_request(struct osdp_pd *pd, uint32_t req) {
	pd->request |= req;
}

static inline bool check_request(struct osdp_pd *pd, uint32_t req) {
	if (pd->request & req) {
		pd->request &= ~req;
		return true;
	}
	return false;
}

static inline bool test_request(struct osdp_pd *pd, uint32_t req) {
	return pd->request & req;
}

static inline bool is_capture_enabled(struct osdp_pd *pd) {
	return (ISSET_FLAG(pd, PD_FLAG_CAPTURE_PKT) &&
	        (IS_ENABLED(OPT_OSDP_PACKET_TRACE) ||
	         IS_ENABLED(OPT_OSDP_DATA_TRACE)));
}

static inline bool is_data_trace_enabled(struct osdp_pd *pd) {
	return (ISSET_FLAG(pd, PD_FLAG_CAPTURE_PKT) &&
	        IS_ENABLED(OPT_OSDP_DATA_TRACE));
}

static inline bool is_packet_trace_enabled(struct osdp_pd *pd) {
	return (ISSET_FLAG(pd, PD_FLAG_CAPTURE_PKT) &&
	        IS_ENABLED(OPT_OSDP_PACKET_TRACE));
}

static inline bool sc_allow_empty_encrypted_data_block(struct osdp_pd *pd) {
	return ISSET_FLAG(pd, PD_FLAG_ALLOW_EMPTY_EDB);
}

static inline bool is_enforce_secure(struct osdp_pd *pd)
{
	return ISSET_FLAG(pd, PD_FLAG_ENFORCE_SECURE);
}

static inline bool is_notifications_enabled(struct osdp_pd *pd) {
	return ISSET_FLAG(pd, PD_FLAG_ENABLE_NOTIF);
}

static inline bool is_ignore_unsolicited_messages(struct osdp_pd *pd) {
	return ISSET_FLAG(pd, PD_FLAG_IGNORE_USR);
}

static inline bool is_install_mode(struct osdp_pd *pd) {
	return ISSET_FLAG(pd, PD_FLAG_INSTALL_MODE);
}

#endif	/* _OSDP_COMMON_H_ */
