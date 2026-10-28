/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "osdp_piv.h"

/* Per-family wiring of the §5.10 smartcard/PIV multi-part consumers. */
struct piv_family {
	int mp_msg;
	uint8_t app_cmd; /* OSDP_CMD_* */
	uint8_t wire_cmd; /* CMD_* */
	uint8_t wire_reply; /* REPLY_* */
	uint8_t event_type; /* OSDP_EVENT_* */
};

/* GENAUTH's Algorithm/Key prefix bytes (Table 32) ride outside the multipart
 * payload, on the first fragment only; TOTAL/DATA_LEN count the challenge
 * alone. */
#define PIV_AUTH_PFX_LEN 2

static const struct piv_family piv_families[] = {
	{
		.mp_msg = OSDP_MP_MSG_PIV,
		.app_cmd = OSDP_CMD_PIVDATA,
		.wire_cmd = CMD_PIVDATA,
		.wire_reply = REPLY_PIVDATAR,
		.event_type = OSDP_EVENT_PIVDATAR,
	},
	{
		.mp_msg = OSDP_MP_MSG_GENAUTH,
		.app_cmd = OSDP_CMD_GENAUTH,
		.wire_cmd = CMD_GENAUTH,
		.wire_reply = REPLY_GENAUTHR,
		.event_type = OSDP_EVENT_GENAUTHR,
	},
	{
		.mp_msg = OSDP_MP_MSG_CRAUTH,
		.app_cmd = OSDP_CMD_CRAUTH,
		.wire_cmd = CMD_CRAUTH,
		.wire_reply = REPLY_CRAUTHR,
		.event_type = OSDP_EVENT_CRAUTHR,
	},
};

/* Do GENAUTH/CRAUTH command fragments carry the Algorithm/Key prefix? */
static inline int piv_cmd_pfx_len(const struct osdp_piv *p)
{
	return (p->mp_msg == OSDP_MP_MSG_PIV) ? 0 : PIV_AUTH_PFX_LEN;
}

static const struct piv_family *family_by_app_cmd(int app_cmd)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZEOF(piv_families); i++) {
		if (piv_families[i].app_cmd == app_cmd) {
			return &piv_families[i];
		}
	}
	return NULL;
}

static const struct piv_family *family_by_wire_cmd(uint8_t wire_cmd)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZEOF(piv_families); i++) {
		if (piv_families[i].wire_cmd == wire_cmd) {
			return &piv_families[i];
		}
	}
	return NULL;
}

#ifdef OPT_OSDP_STATIC
/* CP-mode PDs that can run PIV; the PD context's slot is extra. */
#ifndef OSDP_PIV_STATIC_SLOTS
#define OSDP_PIV_STATIC_SLOTS OSDP_CP_MAX_PDS
#endif
static inline struct osdp_piv *piv_static_slot_get(struct osdp_pd *pd)
{
	static struct osdp_piv g_osdp_piv_slots[OSDP_PIV_STATIC_SLOTS +
						OSDP_STATIC_PD_MODE_SLOTS];
	int i = osdp_static_slot_idx(pd, OSDP_PIV_STATIC_SLOTS);

	return i < 0 ? NULL : &g_osdp_piv_slots[i];
}
#endif /* OPT_OSDP_STATIC */

static struct osdp_piv *piv_get(struct osdp_pd *pd)
{
	if (!pd->piv) {
#ifdef OPT_OSDP_STATIC
		pd->piv = piv_static_slot_get(pd);
		if (pd->piv) {
			memset(pd->piv, 0, sizeof(struct osdp_piv));
		}
#else
		pd->piv = osdp_calloc(1, sizeof(struct osdp_piv));
#endif
		if (pd->piv == NULL) {
			LOG_ERR("PIV: failed to allocate context");
			return NULL;
		}
	}
	return pd->piv;
}

/* End the active op; the context (and its allocation) survives for reuse. */
static void piv_op_reset(struct osdp_piv *p)
{
	p->phase = OSDP_PIV_IDLE;
	p->mp_msg = 0;
	osdp_mp_reset(&p->mp);
}

static void piv_op_setup(struct osdp_pd *pd, struct osdp_piv *p,
			 const struct piv_family *f, int object_id)
{
	p->mp_msg = f->mp_msg;
	p->app_cmd = f->app_cmd;
	p->wire_cmd = f->wire_cmd;
	p->wire_reply = f->wire_reply;
	p->event_type = f->event_type;
	p->start_emitted = false;
	p->tstamp = osdp_millis_now();
	osdp_mp_reset(&p->mp);
	osdp_mp_set_event_cb(&p->mp, osdp_mp_pd_notify, pd);
	osdp_mp_set_identity(&p->mp, f->mp_msg, object_id);
	/* Arm the engine right away (a leg re-inits it with real parameters)
	 * so an op that dies before its first leg — e.g. the PD NAKs the
	 * command — still reports MP_DONE; osdp_mp_finish() is a no-op on an
	 * idle engine. */
	osdp_mp_rx_init(&p->mp, OSDP_MP_W16, 0);
	p->phase = OSDP_PIV_CMD;
}

/* One MP_START per op: the engine re-latches at each leg's init, but both
 * legs belong to the same observable transfer. */
static void piv_emit_start(struct osdp_piv *p)
{
	if (!p->start_emitted) {
		p->start_emitted = true;
		osdp_mp_emit_start(&p->mp);
	}
}

bool osdp_piv_is_active(struct osdp_pd *pd)
{
	return TO_PIV(pd) && TO_PIV(pd)->phase != OSDP_PIV_IDLE;
}

bool osdp_piv_owns_cmd(struct osdp_pd *pd, int cmd_id)
{
	return osdp_piv_is_active(pd) && TO_PIV(pd)->wire_cmd == cmd_id;
}

int osdp_piv_request_cancel(struct osdp_pd *pd, int mp_msg)
{
	struct osdp_piv *p = TO_PIV(pd);

	if (!p || p->phase == OSDP_PIV_IDLE || p->mp_msg != mp_msg) {
		return -1;
	}
	/* Consumed by osdp_piv_cp_get_command(), which also sends CMD_ABORT. */
	osdp_mp_request_cancel(&p->mp);
	return 0;
}

void osdp_piv_abort(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	if (!p || p->phase == OSDP_PIV_IDLE) {
		return;
	}
	/* DONE never precedes START, even for an op that dies before its
	 * first fragment. */
	piv_emit_start(p);
	osdp_mp_finish(&p->mp, OSDP_MP_OUTCOME_ABORTED);
	piv_op_reset(p);
}

/* --- CP role --- */

int osdp_piv_cp_submit(struct osdp_pd *pd, const struct osdp_cmd *cmd)
{
	const struct piv_family *f = family_by_app_cmd(cmd->id);
	struct osdp_piv *p = piv_get(pd);

	if (!p || !f) {
		return -1;
	}
	if (p->phase != OSDP_PIV_IDLE) {
		LOG_ERR("PIV: an operation is already in progress");
		return -1;
	}
	/* §5.10.2: a multi-part transfer may not interleave with another. */
	if (osdp_mp_engine_busy(pd)) {
		LOG_ERR("PIV: another multi-part transfer in progress");
		return -1;
	}

	switch (cmd->id) {
	case OSDP_CMD_PIVDATA:
		p->req = cmd->pivdata;
		piv_op_setup(pd, p, f, cmd->pivdata.element);
		break;
	case OSDP_CMD_GENAUTH:
	case OSDP_CMD_CRAUTH:
		if (cmd->auth.length == 0 ||
		    cmd->auth.length > sizeof(p->data)) {
			LOG_ERR("PIV: invalid challenge length %u",
				cmd->auth.length);
			return -1;
		}
		piv_op_setup(pd, p, f, cmd->auth.key);
		p->algorithm = cmd->auth.algorithm;
		p->key = cmd->auth.key;
		memcpy(p->data, cmd->auth.data, cmd->auth.length);
		osdp_mp_bind_buffer(&p->mp, p->data, cmd->auth.length);
		osdp_mp_tx_init(&p->mp, cmd->auth.length, OSDP_MP_W16);
		break;
	default:
		return -1;
	}
	return 0;
}

int osdp_piv_cp_get_command(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	if (!p || p->phase == OSDP_PIV_IDLE) {
		return 0;
	}
	if (osdp_mp_cancel_requested(&p->mp) ||
	    osdp_millis_since(p->tstamp) > OSDP_PIV_OP_TIMEOUT_MS) {
		LOG_ERR("PIV: operation %s; aborting",
			osdp_mp_cancel_requested(&p->mp) ? "cancelled" :
							   "timed out");
		osdp_piv_abort(pd);
		return CMD_ABORT;
	}
	switch (p->phase) {
	case OSDP_PIV_CMD:
		return p->wire_cmd;
	case OSDP_PIV_REPLY:
		/* Reply fragments are pulled with polls (§5.10.2: the ACU is
		 * in control of the retrieval of replies). */
		return CMD_POLL;
	default:
		return 0;
	}
}

int osdp_piv_cp_cmd_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	struct osdp_piv *p = TO_PIV(pd);
	int len = 0;

	if (!p || p->phase != OSDP_PIV_CMD || pd->cmd_id != p->wire_cmd) {
		LOG_ERR("PIV: no command staged for build");
		return -1;
	}

	switch (pd->cmd_id) {
	case CMD_PIVDATA:
		if (max_len < CMD_PIVDATA_DATA_LEN) {
			return -1;
		}
		buf[len++] = p->req.oid[0];
		buf[len++] = p->req.oid[1];
		buf[len++] = p->req.oid[2];
		buf[len++] = p->req.element;
		buf[len++] = p->req.offset;
		break;
	case CMD_GENAUTH:
	case CMD_CRAUTH: {
		uint8_t pfx[PIV_AUTH_PFX_LEN] = { p->algorithm, p->key };

		/* Reserve for the command id byte plus the SC headroom the
		 * phy adds when finalizing; the multipart build greedily
		 * fills what remains. */
		len = osdp_mp_tx_build_ex(&p->mp, buf,
					  max_len - 1 - OSDP_MP_SC_RESERVE,
					  pfx, sizeof(pfx));
		if (len <= 0) {
			return -1;
		}
		piv_emit_start(p);
		break;
	}
	default:
		return -1;
	}

	/* Pure serialization: the phy layer re-invokes this on retries. The
	 * fragment is committed (and CMD -> REPLY transitioned) when a valid
	 * reply arrives. */
	return len;
}

/* The PD answered the command leg; the reply leg begins. */
static void piv_cp_reply_begin(struct osdp_pd *pd, struct osdp_piv *p)
{
	ARG_UNUSED(pd);

	osdp_mp_bind_buffer(&p->mp, p->data, sizeof(p->data));
	osdp_mp_rx_init(&p->mp, OSDP_MP_W16, 0);
	piv_emit_start(p);
	p->tstamp = osdp_millis_now();
	p->phase = OSDP_PIV_REPLY;
}

/* A valid reply to the command leg acknowledges the fragment that carried
 * it. Commits the fragment; returns true once the whole command is out. */
static bool piv_cp_cmd_leg_done(struct osdp_piv *p)
{
	if (p->mp.total == 0 || !osdp_mp_is_active(&p->mp)) {
		/* Single-exchange command (PIVDATA): the engine only holds
		 * the placeholder arming, there is no tx leg to commit. */
		return true;
	}
	osdp_mp_tx_commit(&p->mp);
	p->tstamp = osdp_millis_now();
	return p->mp.offset >= p->mp.total;
}

void osdp_piv_cp_cmd_acked(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	if (!p || p->phase != OSDP_PIV_CMD || pd->cmd_id != p->wire_cmd) {
		return;
	}
	if (piv_cp_cmd_leg_done(p)) {
		/* All fragments ACKed; the app on the PD now has the whole
		 * command and may take its time — poll for the reply. */
		piv_cp_reply_begin(pd, p);
	}
}

int osdp_piv_cp_reply_consume(struct osdp_pd *pd, const uint8_t *buf, int len,
			      struct osdp_event *event)
{
	struct osdp_piv *p = TO_PIV(pd);
	enum osdp_mp_rc rc;

	if (!p || pd->reply_id != p->wire_reply) {
		LOG_ERR("PIV: unexpected reply %02x", pd->reply_id);
		return -1;
	}
	if (p->phase == OSDP_PIV_CMD && pd->cmd_id == p->wire_cmd) {
		/* First reply fragment arrived inline: it also acknowledges
		 * the (final) command fragment it answers. */
		(void)piv_cp_cmd_leg_done(p);
		piv_cp_reply_begin(pd, p);
	} else if (p->phase != OSDP_PIV_REPLY) {
		LOG_ERR("PIV: reply %02x with no op in progress", pd->reply_id);
		return -1;
	}

	rc = osdp_mp_rx_consume(&p->mp, buf, len);
	switch (rc) {
	case OSDP_MP_RC_MORE:
		osdp_mp_rx_commit(&p->mp);
		p->tstamp = osdp_millis_now();
		return 0;
	case OSDP_MP_RC_DONE:
		osdp_mp_rx_commit(&p->mp);
		memset(event, 0, sizeof(*event));
		event->type = p->event_type;
		event->piv_reply.length = (uint16_t)p->mp.total;
		memcpy(event->piv_reply.data, p->data, p->mp.total);
		return 1;
	case OSDP_MP_RC_EARLY_TERM:
		/* §5.10.2: the sender terminated the transfer early. */
		LOG_ERR("PIV: PD terminated the reply early; aborting");
		osdp_piv_abort(pd);
		return 0;
	default:
		LOG_ERR("PIV: bad reply fragment at off:%" PRIu32 "; aborting",
			p->mp.offset);
		osdp_piv_abort(pd);
		return 0;
	}
}

void osdp_piv_cp_reply_finish(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	osdp_mp_finish(&p->mp, OSDP_MP_OUTCOME_OK);
	piv_op_reset(p);
}

/* --- PD role --- */

int osdp_piv_pd_open(struct osdp_pd *pd, uint8_t wire_cmd, int object_id)
{
	const struct piv_family *f = family_by_wire_cmd(wire_cmd);
	struct osdp_piv *p = piv_get(pd);

	if (!p || !f) {
		return -1;
	}
	/* A new command supersedes whatever the previous op left behind. */
	osdp_piv_abort(pd);
	piv_op_setup(pd, p, f, object_id);
	return 0;
}

int osdp_piv_pd_cmd_frag(struct osdp_pd *pd, uint8_t wire_cmd,
			 const uint8_t *buf, int len, struct osdp_cmd *cmd)
{
	struct osdp_piv *p = TO_PIV(pd);
	uint8_t pfx[PIV_AUTH_PFX_LEN] = { 0, 0 };
	uint32_t total = 0, off = 0;
	uint16_t dlen = 0;
	enum osdp_mp_rc rc;

	if (osdp_mp_hdr_read(OSDP_MP_W16, buf, len, &total, &off, &dlen) < 0) {
		return -1;
	}
	if (off == 0) {
		/* First fragment opens (or supersedes) the op. */
		if (total == 0 || total > OSDP_PIV_DATA_MAX_LEN) {
			LOG_ERR("PIV: bad declared total %" PRIu32, total);
			return -1;
		}
		if (osdp_piv_pd_open(pd, wire_cmd, 0)) {
			return -1;
		}
		p = TO_PIV(pd);
		osdp_mp_bind_buffer(&p->mp, p->data, sizeof(p->data));
		osdp_mp_rx_init(&p->mp, OSDP_MP_W16, total);
		piv_emit_start(p);
	} else if (!p || p->phase != OSDP_PIV_CMD || p->wire_cmd != wire_cmd ||
		   !osdp_mp_is_active(&p->mp)) {
		LOG_ERR("PIV: continuation fragment with no op in progress");
		return -1;
	}

	rc = osdp_mp_rx_consume_ex(&p->mp, buf, len, pfx, piv_cmd_pfx_len(p));
	switch (rc) {
	case OSDP_MP_RC_MORE:
	case OSDP_MP_RC_DONE:
		if (off == 0) {
			p->algorithm = pfx[0];
			p->key = pfx[1];
			osdp_mp_set_identity(&p->mp, p->mp_msg, p->key);
		}
		osdp_mp_rx_commit(&p->mp);
		p->tstamp = osdp_millis_now();
		if (rc == OSDP_MP_RC_MORE) {
			return 0;
		}
		memset(cmd, 0, sizeof(*cmd));
		cmd->id = p->app_cmd;
		cmd->auth.algorithm = p->algorithm;
		cmd->auth.key = p->key;
		cmd->auth.length = (uint16_t)p->mp.total;
		memcpy(cmd->auth.data, p->data, p->mp.total);
		return 1;
	case OSDP_MP_RC_EARLY_TERM:
		/* §5.10.2: the sender terminated the transfer early. */
		LOG_INF("PIV: CP terminated the command leg");
		osdp_piv_abort(pd);
		return 0; /* nothing left to process; ACK the frame */
	default:
		osdp_piv_abort(pd);
		return -1;
	}
}

bool osdp_piv_pd_reply_pending(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	return p && p->phase == OSDP_PIV_REPLY && osdp_mp_is_active(&p->mp) &&
	       p->mp.offset < p->mp.total;
}

int osdp_piv_pd_reply_id(struct osdp_pd *pd)
{
	return TO_PIV(pd)->wire_reply;
}

int osdp_piv_pd_reply_build(struct osdp_pd *pd, const struct osdp_event *event,
			    uint8_t *buf, int max_len)
{
	struct osdp_piv *p = TO_PIV(pd);
	int n;

	if (!p) {
		LOG_ERR("PIV: no context to build reply from");
		return -1;
	}
	if (event) { /* first fragment: seed the reply leg from the app */
		if (p->phase != OSDP_PIV_CMD) {
			/* No command is awaiting this reply (e.g. the op was
			 * aborted while the app's event sat in the queue). */
			LOG_ERR("PIV: reply event with no command pending");
			return -1;
		}
		if (event->piv_reply.length == 0 ||
		    event->piv_reply.length > sizeof(p->data)) {
			LOG_ERR("PIV: invalid reply length %u",
				event->piv_reply.length);
			return -1;
		}
		memcpy(p->data, event->piv_reply.data, event->piv_reply.length);
		osdp_mp_bind_buffer(&p->mp, p->data, event->piv_reply.length);
		osdp_mp_tx_init(&p->mp, event->piv_reply.length, OSDP_MP_W16);
		osdp_mp_emit_start(&p->mp);
		p->phase = OSDP_PIV_REPLY;
	} else if (!osdp_piv_pd_reply_pending(pd)) {
		LOG_ERR("PIV: no reply in progress");
		return -1;
	}

	/* The multipart build greedily consumes all offered space, but the
	 * phy layer still adds SC padding + MAC when finalizing the packet.
	 * Reserve for it, as the file transfer build does. */
	max_len -= 16;
	n = osdp_mp_tx_build(&p->mp, buf, max_len);
	if (n < 0) {
		LOG_ERR("PIV: reply fragment build failed; aborting");
		osdp_piv_abort(pd);
		return -1;
	}
	/* Commit immediately: a lost reply is replayed verbatim by the phy
	 * layer's sequence-repeat handling, never re-built. */
	osdp_mp_tx_commit(&p->mp);
	p->tstamp = osdp_millis_now();
	if (p->mp.offset >= p->mp.total) {
		osdp_mp_finish(&p->mp, OSDP_MP_OUTCOME_OK);
		piv_op_reset(p);
	}
	return n;
}
