/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_TUNABLES_H_
#define _OSDP_TUNABLES_H_

/**
 * @brief OSDP configuration parameters
 *
 * These values can be overridden by build system (Kconfig for Zephyr,
 * CMake options for other platforms). The values below are defaults
 * used when not overridden.
 */

/* Protocol Timings */
#ifndef OSDP_PD_SC_RETRY_MS
#define OSDP_PD_SC_RETRY_MS                     (600 * 1000u)
#endif

#ifndef OSDP_PD_POLL_TIMEOUT_MS
#define OSDP_PD_POLL_TIMEOUT_MS                 (50)
#endif

#ifndef OSDP_PD_SC_TIMEOUT_MS
#define OSDP_PD_SC_TIMEOUT_MS                   (8 * 1000u)
#endif

#ifndef OSDP_PD_ONLINE_TOUT_MS
#define OSDP_PD_ONLINE_TOUT_MS                  (8 * 1000u)
#endif

#ifndef OSDP_RESP_TOUT_MS
#define OSDP_RESP_TOUT_MS                       (200)
#endif

#ifndef OSDP_CMD_RETRY_WAIT_MS
#define OSDP_CMD_RETRY_WAIT_MS                  (800)
#endif

#ifndef OSDP_ONLINE_RETRY_WAIT_MAX_MS
#define OSDP_ONLINE_RETRY_WAIT_MAX_MS           (300 * 1000u)
#endif

/* TRS presence-scan cadence defaults (see osdp_cp_trs_scan_enable) */
#ifndef OSDP_TRS_SCAN_MODE0_DWELL_MS
#define OSDP_TRS_SCAN_MODE0_DWELL_MS            (200)
#endif

#ifndef OSDP_TRS_SCAN_MODE1_DWELL_MS
#define OSDP_TRS_SCAN_MODE1_DWELL_MS            (200)
#endif

#ifndef OSDP_TRS_SCAN_HOLD_MS
#define OSDP_TRS_SCAN_HOLD_MS                   (500)
#endif

#ifndef OSDP_TRS_SCAN_BACKOFF_MAX_MS
#define OSDP_TRS_SCAN_BACKOFF_MAX_MS            (5 * 1000u)
#endif

/* How long the CP holds a TRS band for the answer to a card command the PD
 * acknowledged ("working") instead of answering at once. */
#ifndef OSDP_TRS_ANSWER_TOUT_MS
#define OSDP_TRS_ANSWER_TOUT_MS                 (5 * 1000u)
#endif

/* Protocol Limits */
#ifndef OSDP_CMD_MAX_RETRIES
#define OSDP_CMD_MAX_RETRIES                    (8)
#endif

#ifndef OSDP_FILE_ERROR_RETRY_MAX
#define OSDP_FILE_ERROR_RETRY_MAX               (10)
#endif

#ifndef OSDP_FILE_FINALIZE_TIMEOUT_MS
#define OSDP_FILE_FINALIZE_TIMEOUT_MS           (30000)
#endif

/* A smartcard/PIV multi-part operation that makes no forward progress for
 * this long is aborted (covers a PD app that never supplies its reply). */
#ifndef OSDP_PIV_OP_TIMEOUT_MS
#define OSDP_PIV_OP_TIMEOUT_MS                  (10 * 1000)
#endif

/* A multi-part biometric read reply that makes no forward progress for this
 * long is aborted (covers a PD app that never supplies its template). */
#ifndef OSDP_BIO_OP_TIMEOUT_MS
#define OSDP_BIO_OP_TIMEOUT_MS                  (10 * 1000)
#endif

#ifndef OSDP_PD_MAX
#define OSDP_PD_MAX                             (126)
#endif

#ifndef OSDP_PD_NAME_MAXLEN
#define OSDP_PD_NAME_MAXLEN                     (16)
#endif

/* Memory Configuration */
#ifndef OSDP_PACKET_BUF_SIZE
#define OSDP_PACKET_BUF_SIZE                    (256)
#endif

#ifndef OSDP_MINIMUM_PACKET_SIZE
#define OSDP_MINIMUM_PACKET_SIZE                (128)
#endif

#ifndef OSDP_RX_RB_SIZE
#define OSDP_RX_RB_SIZE                         (512)
#endif

#ifndef OSDP_CP_CMD_POOL_SIZE
#define OSDP_CP_CMD_POOL_SIZE                   (4)
#endif

#ifndef OSDP_CP_MAX_PDS
#define OSDP_CP_MAX_PDS                         (8)
#endif

/* Internal Constants */
#ifndef OSDP_CMD_ID_OFFSET
#define OSDP_CMD_ID_OFFSET                      (5)
#endif

#ifndef OSDP_PCAP_LINK_TYPE
#define OSDP_PCAP_LINK_TYPE                     (162)
#endif


#endif /* _OSDP_TUNABLES_H_ */
