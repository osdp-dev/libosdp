/*
 * Copyright (c) 2022 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_TRS_H_
#define _OSDP_TRS_H_

#include "osdp_common.h"

/*
 * Reply action returned by osdp_trs_cmd_decode() (a negative return means NAK).
 * Tells osdp_pd.c how to answer a CMD_XWR the library handled internally.
 */
enum osdp_trs_decode_e {
	OSDP_TRS_DECODE_ACK = 0, /* library handled it; answer osdp_ACK */
	OSDP_TRS_DECODE_MODE_REPORT, /* answer osdp_XRD current-mode report */
	OSDP_TRS_DECODE_TO_APP, /* deliver the decoded command to the app */
};

/*
 * Action returned by osdp_trs_reply_decode() (a negative return means the reply
 * was malformed). Tells osdp_cp.c what to do with the decoded event.
 */
enum osdp_trs_reply_action_e {
	OSDP_TRS_REPLY_ACTION_NONE = 0, /* mode handshake; nothing for the app */
	OSDP_TRS_REPLY_ACTION_DISPATCH, /* deliver the decoded event to the app */
	/* Deliver the event, and fail the command it answered: the reader took
	 * the command but reported that the card transaction did not work. */
	OSDP_TRS_REPLY_ACTION_DISPATCH_ERROR,
};

#ifdef OPT_BUILD_OSDP_TRS

/* @cmd is the app's card command, or NULL for a library-driven session step */
int osdp_trs_cmd_build(struct osdp_pd *pd, const struct osdp_cmd *cmd,
		       uint8_t *buf, int max_len);
int osdp_trs_reply_decode(struct osdp_pd *pd, uint8_t *buf, int len,
			  struct osdp_event *event);
int osdp_trs_reply_build(struct osdp_pd *pd, uint8_t *buf, int max_len);
int osdp_trs_cmd_decode(struct osdp_pd *pd, struct osdp_cmd *cmd, uint8_t *buf,
			int len);
/* Largest C-APDU the negotiated packet size can carry for this PD */
int osdp_trs_max_apdu_len(struct osdp_pd *pd);

/*
 * Background presence scan (osdp_cp_trs_scan_enable): probe scheduling and
 * bookkeeping. The probe rides the ordinary TRS session machinery; these
 * helpers only decide when one opens/closes and what it must not do (raise
 * session notifications, flush the band).
 */
bool osdp_trs_scan_probe_due(struct osdp_pd *pd);
bool osdp_trs_probe_expired(struct osdp_pd *pd);
void osdp_trs_probe_note_open(struct osdp_pd *pd);
void osdp_trs_probe_note_run(struct osdp_pd *pd);
void osdp_trs_probe_adopted(struct osdp_pd *pd);
bool osdp_trs_probe_close(struct osdp_pd *pd);
void osdp_trs_probe_reset(struct osdp_pd *pd);
void osdp_trs_scan_note_activity(struct osdp_pd *pd);

#else /* OPT_BUILD_OSDP_TRS */

static inline int osdp_trs_cmd_build(struct osdp_pd *pd,
				     const struct osdp_cmd *cmd, uint8_t *buf,
				     int max_len)
{
	ARG_UNUSED(pd);
	ARG_UNUSED(cmd);
	ARG_UNUSED(buf);
	ARG_UNUSED(max_len);
	return -1;
}
static inline int osdp_trs_reply_decode(struct osdp_pd *pd, uint8_t *buf,
					int len, struct osdp_event *event)
{
	ARG_UNUSED(pd);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	ARG_UNUSED(event);
	return -1;
}
static inline int osdp_trs_reply_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	ARG_UNUSED(pd);
	ARG_UNUSED(buf);
	ARG_UNUSED(max_len);
	return -1;
}
static inline int osdp_trs_cmd_decode(struct osdp_pd *pd, struct osdp_cmd *cmd,
				      uint8_t *buf, int len)
{
	ARG_UNUSED(pd);
	ARG_UNUSED(cmd);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -1;
}
static inline bool osdp_trs_scan_probe_due(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
	return false;
}
static inline bool osdp_trs_probe_expired(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
	return true;
}
static inline void osdp_trs_probe_note_open(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
}
static inline void osdp_trs_probe_note_run(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
}
static inline void osdp_trs_probe_adopted(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
}
static inline bool osdp_trs_probe_close(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
	return false;
}
static inline void osdp_trs_probe_reset(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
}
static inline void osdp_trs_scan_note_activity(struct osdp_pd *pd)
{
	ARG_UNUSED(pd);
}

#endif /* OPT_BUILD_OSDP_TRS */

#endif /* _OSDP_TRS_H_ */