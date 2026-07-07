/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_CONFIG_H_
#define _OSDP_CONFIG_H_

/**
 * @brief The following macros are defined defined from the variable in cmake
 * files. All @XXX@ are replaced by the value of XXX as resolved by cmake.
 */
#define PROJECT_VERSION                "3.2.0"
#define PROJECT_NAME                   "libosdp"
#define GIT_BRANCH                     "platformio"
#define GIT_REV                        ""
#define GIT_TAG                        ""
#define GIT_DIFF                       ""
#define REPO_ROOT                      ""

/**
 * @brief Other OSDP constants
 */
#define OSDP_PD_SC_RETRY_MS                     (600 * 1000u)
#define OSDP_PD_POLL_TIMEOUT_MS                 (50)
#define OSDP_PD_SC_TIMEOUT_MS                   (8 * 1000u)
#define OSDP_PD_ONLINE_TOUT_MS                  (8 * 1000u)
#define OSDP_RESP_TOUT_MS                       (200)
#define OSDP_CMD_MAX_RETRIES                    (8)
#define OSDP_ONLINE_RETRY_WAIT_MAX_MS           (300 * 1000u)
#define OSDP_CMD_RETRY_WAIT_MS                  (800)
#define OSDP_PACKET_BUF_SIZE                    (256)
#define OSDP_RX_RB_SIZE                         (512)
#define OSDP_CP_CMD_POOL_SIZE                   (4)
#define OSDP_FILE_ERROR_RETRY_MAX               (10)
#define OSDP_PD_MAX                             (126)
#define OSDP_CMD_ID_OFFSET                      (5)
#define OSDP_PCAP_LINK_TYPE                     (162)
#define OSDP_PD_NAME_MAXLEN                     (16)
#define OSDP_MINIMUM_PACKET_SIZE		(128)

#endif /* _OSDP_CONFIG_H_ */
