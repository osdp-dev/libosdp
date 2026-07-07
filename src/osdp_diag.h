/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_PCAP_H_
#define _OSDP_PCAP_H_

#include "osdp_common.h"

#if defined(OPT_OSDP_PACKET_TRACE) || defined(OPT_OSDP_DATA_TRACE)

void osdp_packet_capture_init(struct osdp_pd *pd);
void osdp_packet_capture_finish(struct osdp_pd *pd);
void osdp_capture_packet(struct osdp_pd *pd, uint8_t *buf, int len);

#else

static inline void osdp_packet_capture_init(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
}

static inline void osdp_packet_capture_finish(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
}

static inline void osdp_capture_packet(struct osdp_pd *pd,
				       uint8_t *buf, int len)
{
	ARG_UNUSED(pd);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
}

#endif

#endif /* _OSDP_PCAP_H_ */

