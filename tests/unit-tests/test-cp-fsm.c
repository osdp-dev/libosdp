/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>

#include <osdp.h>
#include "test.h"

extern void test_state_update(struct osdp_pd *);
extern bool test_cp_cmd_failure_is_soft(struct osdp_pd *);
extern int test_cp_decode_response(struct osdp_pd *, uint8_t *, int);
extern enum osdp_cp_state_e test_get_next_ok_state(struct osdp_pd *);
extern enum osdp_cp_state_e test_get_next_err_state(struct osdp_pd *);

int test_fsm_resp = 0;
/* When set, every reply is a NAK(SEQ_NUM) regardless of the command sent. */
static bool s_fsm_force_seq_nak;
/* When set, osdp_ACURXSIZE is NAK'd, as by a PD that does not implement it. */
static bool s_fsm_nak_rx_size;
static int s_fsm_rx_size_count;
#ifdef OPT_OSDP_RX_ZERO_COPY
/* One canned reply is served per command; release_pkt marks it consumed so a
 * poll before the next command does not re-deliver a stale frame. */
static bool s_fsm_served;
#endif

int test_cp_fsm_send(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);

#ifndef OPT_OSDP_SKIP_MARK_BYTE
	int cmd_id_offset = OSDP_CMD_ID_OFFSET + 1;
#else
	int cmd_id_offset = OSDP_CMD_ID_OFFSET;
#endif

	switch (buf[cmd_id_offset]) {
	case 0x60:
		test_fsm_resp = 1;
		break;
	case 0x61:
		test_fsm_resp = 2;
		break;
	case 0x62:
		test_fsm_resp = 3;
		break;
	case CMD_ACURXSIZE:
		s_fsm_rx_size_count++;
		test_fsm_resp = s_fsm_nak_rx_size ? 4 : 1;
		break;
	default:
		printf(SUB_1 "invalid ID:0x%02x\n", buf[cmd_id_offset

		]);
	}
#ifdef OPT_OSDP_RX_ZERO_COPY
	s_fsm_served = false;
#endif
	return len;
}

/* Copy the canned reply selected by the last command into `buf`; returns its
 * length or -1 when no reply is pending. Shared by the byte-stream recv and the
 * zero-copy recv_pkt paths. */
static int fsm_fill_response(uint8_t *buf)
{
	uint8_t resp_id[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x14, 0x00, 0x04, 0x45, 0xa1, 0xa2, 0xa3, 0xb1,
		0xc1, 0xd1, 0xd2, 0xd3, 0xd4, 0xe1, 0xe2, 0xe3, 0xf8, 0xd9
	};
	uint8_t resp_cap[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x0b, 0x00, 0x05, 0x46, 0x04, 0x04, 0x01, 0xb3, 0xec
	};
	uint8_t resp_ack[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x08, 0x00, 0x06, 0x40, 0xb0, 0xf0
	};
	uint8_t resp_nak[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x09, 0x00, 0x06, 0x41, 0x00 /* code */,
		0x00, 0x00 /* crc filled below */
	};
	int n = sizeof(resp_nak);
	uint16_t crc;

	if (s_fsm_force_seq_nak || test_fsm_resp == 4) {
		resp_nak[n - 3] = s_fsm_force_seq_nak ? OSDP_PD_NAK_SEQ_NUM :
							OSDP_PD_NAK_CMD_UNKNOWN;
		crc = osdp_compute_crc16(resp_nak + (n - 9), 7);
		resp_nak[n - 2] = crc & 0xff;
		resp_nak[n - 1] = crc >> 8;
		memcpy(buf, resp_nak, n);
		return n;
	}

	switch (test_fsm_resp) {
	case 1:
		memcpy(buf, resp_ack, sizeof(resp_ack));
		return sizeof(resp_ack);
	case 2:
		memcpy(buf, resp_id, sizeof(resp_id));
		return sizeof(resp_id);
	case 3:
		memcpy(buf, resp_cap, sizeof(resp_cap));
		return sizeof(resp_cap);
	}
	return -1;
}

int test_cp_fsm_receive(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return fsm_fill_response(buf);
}

#ifdef OPT_OSDP_RX_ZERO_COPY
int test_cp_fsm_recv_pkt(void *data, const uint8_t **buf, int *max_len)
{
	static uint8_t frame[32];
	int n;

	ARG_UNUSED(data);
	if (s_fsm_served) {
		return -1;
	}
	n = fsm_fill_response(frame);
	if (n <= 0) {
		return -1;
	}
	*buf = frame;
	*max_len = n;
	return 0;
}

void test_cp_fsm_release_pkt(void *data, const uint8_t *buf)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	s_fsm_served = true;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

int test_cp_fsm_setup(struct test *t)
{
	/* mock application data */
	struct osdp_channel channel = {
		.data = NULL,
		.send = test_cp_fsm_send,
#ifndef OPT_OSDP_RX_ZERO_COPY
		.recv = test_cp_fsm_receive,
#else
		.recv_pkt = test_cp_fsm_recv_pkt,
		.release_pkt = test_cp_fsm_release_pkt,
#endif
		.flush = NULL,
	};
	osdp_pd_info_t info = {
		.address = 101,
		.baud_rate = 9600,
		.flags = 0,
		.scbk = NULL,
	};
#ifndef OPT_OSDP_LOG_MINIMAL
	osdp_logger_init("osdp::cp", t->loglevel, NULL);
#endif /* OPT_OSDP_LOG_MINIMAL */
	struct osdp *ctx = (struct osdp *)osdp_cp_setup(&channel, 1, &info);
	if (ctx == NULL) {
		printf("   init failed!\n");
		return -1;
	}
	SET_CURRENT_PD(ctx, 0);
	SET_FLAG(GET_CURRENT_PD(ctx), PD_FLAG_SKIP_SEQ_CHECK);
	t->mock_data = (void *)ctx;
	return 0;
}

void test_cp_fsm_teardown(struct test *t)
{
	osdp_cp_teardown(t->mock_data);
}

/**
 * A command failure is "soft" when the PD answered us and merely declined the
 * command: the app is told, but the link stays up. Everything else -- a silent
 * or garbled PD, a NAK that describes the link rather than the command, or a
 * command LibOSDP itself depends on -- must still take the PD offline.
 */
struct soft_fail_case {
	const char *name;
	enum osdp_cp_state_e state;
	enum osdp_cp_phy_state_e phy_state;
	int cmd_id;
	int reply_id;
	uint8_t nak_code;
	bool is_soft;
};

static const struct soft_fail_case soft_fail_cases[] = {
	/* The PD declined an app command; it is still very much alive. */
	{ "LED NAK'd with RECORD", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, true },
	{ "LED the PD does not implement", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_CMD_UNKNOWN, true },
	{ "file transfer refused", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_FILETRANSFER, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, true },
	{ "vendor command reported an error", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_MFG, REPLY_MFGERRR,
	  OSDP_PD_NAK_NONE, true },
	{ "app keyset NAK'd while online", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_KEYSET, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, true },

	/* The NAK code describes the link, not the command. */
	{ "NAK'd for want of secure channel", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_SC_COND, false },
	{ "NAK'd with a bad check character", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_MSG_CHK, false },
	{ "NAK'd with a sequence error", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_SEQ_NUM, false },

	/* The PD never answered, or answered something else entirely. */
	{ "no reply to an LED command", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_ERR, CMD_LED, REPLY_INVALID,
	  OSDP_PD_NAK_NONE, false },
	/* The phy rejected the frame; nak_code is whatever the last exchange
	 * left behind and must not be read as the PD's answer to this one. */
	{ "phy error carrying a stale NAK code", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_ERR, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, false },
	{ "LED answered with a status report", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_LSTATR,
	  OSDP_PD_NAK_NONE, false },

	/* Commands LibOSDP issues for itself; if these fail, the link is not
	 * usable no matter what the PD says. */
	{ "poll NAK'd", OSDP_CP_STATE_ONLINE, OSDP_CP_PHY_STATE_DONE,
	  CMD_POLL, REPLY_NAK, OSDP_PD_NAK_RECORD, false },
	{ "ID request NAK'd", OSDP_CP_STATE_INIT, OSDP_CP_PHY_STATE_DONE,
	  CMD_ID, REPLY_NAK, OSDP_PD_NAK_RECORD, false },
	{ "SCBK install NAK'd", OSDP_CP_STATE_SET_SCBK,
	  OSDP_CP_PHY_STATE_DONE, CMD_KEYSET, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, false },
};

static void test_cp_soft_failure_matrix(struct test *t)
{
	bool result = true;
	struct osdp_pd pd;

	printf(SUB_1 "classifying command failures\n");

	for (size_t i = 0; i < ARRAY_SIZEOF(soft_fail_cases); i++) {
		const struct soft_fail_case *c = &soft_fail_cases[i];

		memset(&pd, 0, sizeof(pd));
		pd.state = c->state;
		pd.phy_state = c->phy_state;
		pd.cmd_id = c->cmd_id;
		pd.reply_id = c->reply_id;
		pd.nak_code = c->nak_code;

		if (test_cp_cmd_failure_is_soft(&pd) != c->is_soft) {
			printf(SUB_2 "%s: expected a %s failure\n", c->name,
			       c->is_soft ? "soft" : "hard");
			result = false;
		}
	}

	printf(SUB_1 "command failure classification %s\n",
	       result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

/**
 * NAK(MSG_CHK) means "your check character was wrong". From a PD that never
 * claimed CRC-16 that is how a checksum-only PD tells us so, and we downgrade
 * the link. From a PD that did claim CRC-16 it can only mean the frame was
 * corrupted in flight: downgrading there would silently reframe every packet
 * that follows, and the PD -- still speaking CRC -- would demand a sequence
 * reset and take the whole link offline. Resend instead.
 */
static int feed_msg_chk_nak(struct osdp_pd *pd, bool pd_declares_crc)
{
	struct osdp_pd_cap *cap =
		&pd->cap[OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT];
	uint8_t nak[] = { REPLY_NAK, OSDP_PD_NAK_MSG_CHK };

	pd->cmd_id = CMD_LED;
	pd->phy_retry_count = 0;
	SET_FLAG(pd, PD_FLAG_CP_USE_CRC);
	if (pd_declares_crc) {
		cap->function_code = OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT;
		cap->compliance_level = 1;
	} else {
		memset(cap, 0, sizeof(*cap));
	}
	return test_cp_decode_response(pd, nak, sizeof(nak));
}

static void test_cp_crc_nak_fallback(struct test *t)
{
	bool result = true;
	struct osdp_pd pd;
	struct osdp ctx;

	printf(SUB_1 "classifying a NAK(MSG_CHK)\n");

	memset(&ctx, 0, sizeof(ctx));
	memset(&pd, 0, sizeof(pd));
	pd.osdp_ctx = &ctx;

	feed_msg_chk_nak(&pd, false);
	if (ISSET_FLAG(&pd, PD_FLAG_CP_USE_CRC)) {
		printf(SUB_2 "a PD that never claimed CRC-16 must be downgraded\n");
		result = false;
	}

	/* A resend request is a non-zero rc; the retry funnel owns the
	 * budget accounting, so the decoder must leave the counter alone. */
	if (feed_msg_chk_nak(&pd, true) == 0 || pd.phy_retry_count != 0) {
		printf(SUB_2 "a corrupted frame must be resent; retries: %d\n",
		       pd.phy_retry_count);
		result = false;
	}
	if (!ISSET_FLAG(&pd, PD_FLAG_CP_USE_CRC)) {
		printf(SUB_2 "a PD that claimed CRC-16 must not be downgraded\n");
		result = false;
	}

	/* Once the resends are spent, it is an ordinary NAK again */
	pd.phy_retry_count = OSDP_CMD_MAX_RETRIES;
	if (test_cp_decode_response(&pd, (uint8_t[]){ REPLY_NAK,
						      OSDP_PD_NAK_MSG_CHK },
				    2) != 0 ||
	    !ISSET_FLAG(&pd, PD_FLAG_CP_USE_CRC)) {
		printf(SUB_2 "resends must be bounded, not endless\n");
		result = false;
	}

	printf(SUB_1 "NAK(MSG_CHK) classification %s\n",
	       result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

/**
 * The transition tables must be pure: same next state on re-evaluation and
 * not a single byte of pd modified. Effects belong to cp_transition_effects()
 * and cp_state_change(); a write sneaking back into a table would make
 * transitions depend on how often the tables happen to be consulted.
 */
static void test_cp_transition_table_purity(struct test *t)
{
	static const enum osdp_cp_state_e states[] = {
		OSDP_CP_STATE_INIT,    OSDP_CP_STATE_CAPDET,
		OSDP_CP_STATE_SC_CHLNG, OSDP_CP_STATE_SC_SCRYPT,
		OSDP_CP_STATE_SET_SCBK, OSDP_CP_STATE_ONLINE,
		OSDP_CP_STATE_OFFLINE,  OSDP_CP_STATE_DISABLED,
	};
	struct osdp_pd pd, snap;
	bool result = true;

	printf(SUB_1 "transition tables are pure\n");

	for (size_t i = 0; i < ARRAY_SIZEOF(states); i++) {
		memset(&pd, 0, sizeof(pd));
		pd.state = states[i];
		/* Park the OFFLINE re-INIT timer far from its boundary so the
		 * clock cannot flip the answer between two evaluations. */
		pd.tstamp = osdp_millis_now();
		pd.wait_ms = 300 * 1000;
		snap = pd;

		if (test_get_next_ok_state(&pd) != test_get_next_ok_state(&pd) ||
		    memcmp(&pd, &snap, sizeof(pd)) != 0) {
			printf(SUB_2 "ok-table impure in state %d\n", states[i]);
			result = false;
		}
		if (test_get_next_err_state(&pd) != test_get_next_err_state(&pd) ||
		    memcmp(&pd, &snap, sizeof(pd)) != 0) {
			printf(SUB_2 "err-table impure in state %d\n", states[i]);
			result = false;
		}
	}

	printf(SUB_1 "transition table purity %s\n",
	       result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

/**
 * NAK(SEQ_NUM) tells us the PD's link state and ours have diverged. It must
 * take the PD offline through the regular transition path (CP_REQ_LINK_RESET),
 * and any session-scoped request pending at that moment must not survive to
 * fire on the next ONLINE visit.
 */
static bool test_cp_seq_nak_link_reset(struct osdp *ctx)
{
	struct osdp_pd *pd = GET_CURRENT_PD(ctx);
	int count = 0;

	printf(SUB_1 "NAK(SEQ_NUM) resets the link\n");

	if (pd->state != OSDP_CP_STATE_ONLINE) {
		printf(SUB_2 "CP not online to begin with\n");
		return false;
	}

	/* Over the wire: every reply is now NAK(SEQ_NUM). */
	s_fsm_force_seq_nak = true;
	while (pd->state != OSDP_CP_STATE_OFFLINE && count++ < 300) {
		test_state_update(pd);
		usleep(1000);
	}
	s_fsm_force_seq_nak = false;
	if (pd->state != OSDP_CP_STATE_OFFLINE) {
		printf(SUB_2 "CP did not go offline on NAK(SEQ_NUM)\n");
		return false;
	}
	if (pd->request & CP_REQ_LINK_RESET) {
		printf(SUB_2 "LINK_RESET request left pending\n");
		return false;
	}

	/* White-box: LINK_RESET outranks a pending session-scoped request,
	 * and OFFLINE entry clears the stale bit instead of latching it. */
	pd->state = OSDP_CP_STATE_ONLINE;
	pd->request |= CP_REQ_LINK_RESET | CP_REQ_RESTART_SC;
	test_state_update(pd);
	if (pd->state != OSDP_CP_STATE_OFFLINE) {
		printf(SUB_2 "LINK_RESET did not win over RESTART_SC\n");
		return false;
	}
	if (pd->request & (CP_REQ_LINK_RESET | CP_REQ_RESTART_SC)) {
		printf(SUB_2 "stale request bits survived: 0x%x\n",
		       pd->request);
		return false;
	}
	return true;
}

/* Records OSDP_NOTIFICATION_PD_ID deliveries for the change-detection test. */
static struct {
	int count;
	struct osdp_pd_id id;
} g_pdid_note;

static int pdid_event_cb(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(pd);
	if (ev->type == OSDP_EVENT_NOTIFICATION &&
	    ev->notif.type == OSDP_NOTIFICATION_PD_ID) {
		g_pdid_note.count++;
		g_pdid_note.id = ev->notif.pd_id;
	}
	return 0;
}

/* Serialize a REPLY_PDID frame (reply id + 12-byte data block) for `id`. */
static int fill_pdid_reply(uint8_t *buf, const struct osdp_pd_id *id)
{
	int n = 0;

	buf[n++] = REPLY_PDID;
	buf[n++] = id->vendor_code & 0xff;
	buf[n++] = (id->vendor_code >> 8) & 0xff;
	buf[n++] = (id->vendor_code >> 16) & 0xff;
	buf[n++] = (uint8_t)id->model;
	buf[n++] = (uint8_t)id->version;
	buf[n++] = id->serial_number & 0xff;
	buf[n++] = (id->serial_number >> 8) & 0xff;
	buf[n++] = (id->serial_number >> 16) & 0xff;
	buf[n++] = (id->serial_number >> 24) & 0xff;
	buf[n++] = (id->firmware_version >> 16) & 0xff;
	buf[n++] = (id->firmware_version >> 8) & 0xff;
	buf[n++] = id->firmware_version & 0xff;
	return n;
}

static bool pdid_note_matches(const struct osdp_pd_id *want)
{
	return g_pdid_note.id.version == want->version &&
	       g_pdid_note.id.model == want->model &&
	       g_pdid_note.id.vendor_code == want->vendor_code &&
	       g_pdid_note.id.serial_number == want->serial_number &&
	       g_pdid_note.id.firmware_version == want->firmware_version;
}

/*
 * The CP reports a collected PD identity to the app once, and again only when
 * it changes: a same-id reconnect is silent, a different device answering the
 * address fires a fresh notification carrying the new identity.
 */
static bool test_cp_pd_id_notification(void)
{
	const struct osdp_pd_id id_a = {
		.version = 2, .model = 42, .vendor_code = 0x00CAFE,
		.serial_number = 0x01020304, .firmware_version = 0x0A0B0C,
	};
	const struct osdp_pd_id id_b = {
		.version = 2, .model = 42, .vendor_code = 0x00CAFE,
		.serial_number = 0x05060708, .firmware_version = 0x0A0B0C,
	};
	uint8_t buf[16];
	struct osdp_pd pd;
	struct osdp ctx;

	printf(SUB_1 "PD id notification fires on first sight and on change\n");

	memset(&pd, 0, sizeof(pd));
	memset(&ctx, 0, sizeof(ctx));
	memset(&g_pdid_note, 0, sizeof(g_pdid_note));
	pd.osdp_ctx = &ctx;
	ctx.event_callback = pdid_event_cb;
	SET_FLAG(&pd, PD_FLAG_ENABLE_NOTIF);

	/* First sight: fires with the reported identity. */
	test_cp_decode_response(&pd, buf, fill_pdid_reply(buf, &id_a));
	if (g_pdid_note.count != 1 || !pdid_note_matches(&id_a)) {
		printf(SUB_2 "first PD id was not reported (count=%d)\n",
		       g_pdid_note.count);
		return false;
	}

	/* Same identity again (a reconnect): stays silent. */
	test_cp_decode_response(&pd, buf, fill_pdid_reply(buf, &id_a));
	if (g_pdid_note.count != 1) {
		printf(SUB_2 "unchanged PD id must not re-notify (count=%d)\n",
		       g_pdid_note.count);
		return false;
	}

	/* Different identity: fires again with the new id. */
	test_cp_decode_response(&pd, buf, fill_pdid_reply(buf, &id_b));
	if (g_pdid_note.count != 2 || !pdid_note_matches(&id_b)) {
		printf(SUB_2 "changed PD id was not reported (count=%d)\n",
		       g_pdid_note.count);
		return false;
	}

	return true;
}

/*
 * A PD that does not implement osdp_ACURXSIZE NAKs it. That says nothing about
 * the link, so the PD stays online, and is not asked again on this link.
 */
static bool test_cp_rx_size_nak_keeps_pd_online(struct test *t)
{
	struct osdp_pd *pd;
	bool result = true;

	printf(SUB_1 "a NAK'd osdp_ACURXSIZE keeps the PD online\n");

	s_fsm_nak_rx_size = true;
	s_fsm_rx_size_count = 0;
	if (test_cp_fsm_setup(t)) {
		s_fsm_nak_rx_size = false;
		return false;
	}
	pd = GET_CURRENT_PD((struct osdp *)t->mock_data);

	for (int i = 0; i < 300; i++) {
		test_state_update(pd);
		if (pd->state == OSDP_CP_STATE_OFFLINE) {
			printf(SUB_2 "CP took the PD offline\n");
			result = false;
			break;
		}
		usleep(1000);
	}
	if (result && pd->state != OSDP_CP_STATE_ONLINE) {
		printf(SUB_2 "CP never got the PD online\n");
		result = false;
	}
	if (result && s_fsm_rx_size_count != 1) {
		printf(SUB_2 "osdp_ACURXSIZE sent %d times, want 1\n",
		       s_fsm_rx_size_count);
		result = false;
	}

	test_cp_fsm_teardown(t);
	s_fsm_nak_rx_size = false;
	return result;
}

/* Over a real link, the PD learns the CP's receive size, secure channel and all */
static bool test_cp_tells_pd_its_rx_size(struct test *t)
{
	osdp_t *cp_ctx, *pd_ctx;
	struct osdp_pd *pd;
	int cp_runner, pd_runner;
	bool result = false;

	printf(SUB_1 "the CP tells the PD its receive size\n");

	if (test_setup_devices(t, &cp_ctx, &pd_ctx)) {
		return false;
	}
	pd = GET_CURRENT_PD(TO_OSDP(pd_ctx));
	cp_runner = async_runner_start(cp_ctx, osdp_cp_refresh);
	pd_runner = async_runner_start(pd_ctx, osdp_pd_refresh);

	if (!test_wait_for_online(cp_ctx, 0, 10)) {
		printf(SUB_2 "PD never came online\n");
		goto out;
	}
	for (int i = 0; i < 200 && pd->peer_rx_size == 0; i++) {
		usleep(10 * 1000);
	}
	if (pd->peer_rx_size != OSDP_PACKET_BUF_SIZE) {
		printf(SUB_2 "PD holds receive size %u, want %d\n",
		       pd->peer_rx_size, OSDP_PACKET_BUF_SIZE);
		goto out;
	}
	result = true;
out:
	async_runner_stop(cp_runner);
	async_runner_stop(pd_runner);
	osdp_cp_teardown(cp_ctx);
	osdp_pd_teardown(pd_ctx);
	return result;
}

void run_cp_fsm_tests(struct test *t)
{
	int result = true;
	uint32_t count = 0;
	struct osdp *ctx;

	printf("\nStarting CP Phy state tests\n");

	test_cp_soft_failure_matrix(t);
	test_cp_crc_nak_fallback(t);
	test_cp_transition_table_purity(t);
	TEST_REPORT(t, test_cp_pd_id_notification());

	TEST_REPORT(t, test_cp_rx_size_nak_keeps_pd_online(t));
	TEST_REPORT(t, test_cp_tells_pd_its_rx_size(t));

	s_fsm_rx_size_count = 0;
	if (test_cp_fsm_setup(t))
		return;

	ctx = t->mock_data;

	printf(SUB_1 "executing state_update()\n");
	while (1) {
		test_state_update(GET_CURRENT_PD(ctx));

		if (GET_CURRENT_PD(ctx)->state == OSDP_CP_STATE_OFFLINE) {
			printf(SUB_2 "state_update() CP went offline\n");
			result = false;
			break;
		}
		if (count++ > 300)
			break;
		usleep(1000);
	}
	printf(SUB_1 "state_update test %s\n", result ? "succeeded" : "failed");

	TEST_REPORT(t, result);

	printf(SUB_1 "osdp_ACURXSIZE sent once per link\n");
	TEST_REPORT(t, s_fsm_rx_size_count == 1);

	TEST_REPORT(t, test_cp_seq_nak_link_reset(ctx));

	test_cp_fsm_teardown(t);
}

// unnecessary
