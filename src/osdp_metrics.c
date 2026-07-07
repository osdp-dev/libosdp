/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "osdp_common.h"
#include "osdp_metrics.h"

static inline void sat_inc(uint32_t *c)
{
	if (*c != UINT32_MAX) {
		(*c)++;
	}
}

void osdp_metrics_report(struct osdp_pd *pd, enum osdp_metric_event ev)
{
	struct osdp_metrics *m = &pd->metrics;

	switch (ev) {
	case OSDP_METRIC_PACKET_SENT:
		sat_inc(&m->packets_sent);
		break;
	case OSDP_METRIC_PACKET_RECEIVED:
		sat_inc(&m->packets_received);
		break;
	case OSDP_METRIC_PACKET_CHECK_ERROR:
		sat_inc(&m->packet_check_errors);
		break;
	case OSDP_METRIC_NAK:
		sat_inc(&m->nak_count);
		break;
	case OSDP_METRIC_SC_HANDSHAKE:
		sat_inc(&m->sc_handshake_count);
		break;
	case OSDP_METRIC_SC_FAILURE:
		sat_inc(&m->sc_failure_count);
		break;
	case OSDP_METRIC_COMMAND:
		sat_inc(&m->command_count);
		break;
	case OSDP_METRIC_EVENT:
		sat_inc(&m->event_count);
		break;
	}
}

/* --- Exported Methods --- */

int osdp_get_metrics(osdp_t *ctx, int pd_idx, struct osdp_metrics *out)
{
	struct osdp *osdp;
	struct osdp_pd *pd;

	if (ctx == NULL || out == NULL) {
		return -1;
	}
	osdp = TO_OSDP(ctx);
	if (pd_idx < 0 || pd_idx >= osdp->_num_pd) {
		return -1;
	}
	pd = osdp_to_pd(ctx, pd_idx);

	*out = pd->metrics;
	memset(&pd->metrics, 0, sizeof(pd->metrics));
	return 0;
}
