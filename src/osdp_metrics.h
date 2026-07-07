/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_METRICS_H_
#define _OSDP_METRICS_H_

#include "osdp_common.h"

/**
 * Event ids reported from libosdp hot paths into the per-PD metrics
 * bank. Each event id maps 1:1 to one field in `struct osdp_metrics`.
 *
 * The enum is kept deliberately flat (no grouping, no per-direction
 * distinction) because the PD/CP role is implicit in the `pd` context
 * the call site passes in.
 */
enum osdp_metric_event {
	OSDP_METRIC_PACKET_SENT,
	OSDP_METRIC_PACKET_RECEIVED,
	OSDP_METRIC_PACKET_CHECK_ERROR,
	OSDP_METRIC_NAK,
	OSDP_METRIC_SC_HANDSHAKE,
	OSDP_METRIC_SC_FAILURE,
	OSDP_METRIC_COMMAND,
	OSDP_METRIC_EVENT,
};

/**
 * Report one metric event on this PD. Increments the matching counter
 * saturating at UINT32_MAX. Safe to call from any libosdp internal
 * hot path; the operation is a single u32 compare-and-increment.
 */
void osdp_metrics_report(struct osdp_pd *pd, enum osdp_metric_event ev);

#endif /* _OSDP_METRICS_H_ */
