/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file Secure channel policy: which key a device is willing to use, and what
 * it tells the world about it. These are setup-time and first-packet
 * decisions, so each case builds its own context rather than sharing one.
 */

#include "test.h"

extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);
extern enum osdp_cp_state_e test_get_next_err_state(struct osdp_pd *);

#define SCP_ADDR 101

/* osdp_phy.c keeps PKT_CONTROL_* private; mirror the two bits we frame with. */
#define SCP_CTRL_CRC 0x04
#define SCP_CTRL_SCB 0x08

static const uint8_t scp_scbk[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};

/* --- one-shot mock channel, both RX shapes --- */

#define SCP_BUF_MAX 256

static uint8_t scp_rx[SCP_BUF_MAX];
static int scp_rx_len;
static uint8_t scp_tx[SCP_BUF_MAX];
static int scp_tx_len;

static void scp_channel_reset(void)
{
	scp_rx_len = 0;
	scp_tx_len = 0;
	memset(scp_rx, 0, sizeof(scp_rx));
	memset(scp_tx, 0, sizeof(scp_tx));
}

static int scp_send(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);
	if (len > SCP_BUF_MAX) {
		len = SCP_BUF_MAX;
	}
	memcpy(scp_tx, buf, len);
	scp_tx_len = len;
	return len;
}

#ifndef OPT_OSDP_RX_ZERO_COPY
static int scp_recv(void *data, uint8_t *buf, int max_len)
{
	int len = scp_rx_len;

	ARG_UNUSED(data);
	if (len == 0) {
		return 0;
	}
	if (len > max_len) {
		len = max_len;
	}
	memcpy(buf, scp_rx, len);
	scp_rx_len = 0;
	return len;
}
#else
static int scp_recv_pkt(void *data, const uint8_t **buf, int *max_len)
{
	ARG_UNUSED(data);
	if (scp_rx_len == 0) {
		return -1;
	}
	*buf = scp_rx;
	*max_len = scp_rx_len;
	return 0;
}

static void scp_release_pkt(void *data, const uint8_t *buf)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	scp_rx_len = 0;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

static void scp_flush(void *data)
{
	ARG_UNUSED(data);
}

static void scp_fill_channel(struct osdp_channel *chn)
{
	memset(chn, 0, sizeof(*chn));
	chn->send = scp_send;
	chn->flush = scp_flush;
#ifndef OPT_OSDP_RX_ZERO_COPY
	chn->recv = scp_recv;
#else
	chn->recv_pkt = scp_recv_pkt;
	chn->release_pkt = scp_release_pkt;
#endif
}

/* --- device builders --- */

static struct osdp_pd_cap scp_cap[] = {
	{ OSDP_PD_CAP_READER_LED_CONTROL, 1, 1 },
	{ -1, -1, -1 }
};

static void scp_fill_pd_info(osdp_pd_info_t *info, const uint8_t *scbk,
			     uint32_t flags)
{
	memset(info, 0, sizeof(*info));
	info->address = SCP_ADDR;
	info->baud_rate = 9600;
	info->flags = flags;
	info->id.version = 1;
	info->id.model = 153;
	info->id.vendor_code = 31337;
	info->id.serial_number = 0x01020304;
	info->id.firmware_version = 0x0A0B0C0D;
	info->cap = scp_cap;
	info->scbk = scbk;
}

static struct osdp *scp_pd_setup(const uint8_t *scbk, uint32_t flags)
{
	struct osdp_channel chn;
	osdp_pd_info_t info;

	scp_channel_reset();
	scp_fill_channel(&chn);
	scp_fill_pd_info(&info, scbk, flags);
	return (struct osdp *)osdp_pd_setup(&chn, &info);
}

/* --- wire helpers --- */

/*
 * Frame a CP->PD command by hand so the security block and sequence number
 * are ours to choose. Layout per osdp_phy.c: [MARK] SOM ADDR LEN_LSB LEN_MSB
 * CTRL [SB] CMD [DATA] CRC.
 */
static int scp_build_cmd(uint8_t seq, const uint8_t *sb, int sb_len,
			 uint8_t cmd_id, const uint8_t *data, int data_len,
			 uint8_t *out)
{
	int len = 0, pkt_len, body;
	bool use_mark = !IS_ENABLED(OPT_OSDP_SKIP_MARK_BYTE);
	uint16_t crc16;

	pkt_len = 5 + sb_len + 1 + data_len + 2;
	if (use_mark) {
		out[len++] = 0xff;
	}
	body = len;
	out[len++] = 0x53;
	out[len++] = SCP_ADDR;
	out[len++] = pkt_len & 0xff;
	out[len++] = (pkt_len >> 8) & 0xff;
	out[len++] = (seq & 0x03) | SCP_CTRL_CRC | (sb_len ? SCP_CTRL_SCB : 0);
	if (sb_len) {
		memcpy(out + len, sb, sb_len);
		len += sb_len;
	}
	out[len++] = cmd_id;
	if (data_len) {
		memcpy(out + len, data, data_len);
		len += data_len;
	}
	crc16 = test_osdp_compute_crc16(out + body, len - body);
	out[len++] = crc16 & 0xff;
	out[len++] = (crc16 >> 8) & 0xff;
	return len;
}

static void scp_exchange(struct osdp *ctx, const uint8_t *pkt, int len)
{
	int i;

	memcpy(scp_rx, pkt, len);
	scp_rx_len = len;
	scp_tx_len = 0;
	for (i = 0; i < 4 && scp_tx_len == 0; i++) {
		osdp_pd_refresh((osdp_t *)ctx);
	}
}

/*
 * Offset of the reply ID byte in a captured TX frame. The mark byte and the
 * security block are both conditional, so ask phy rather than guessing -- the
 * zero-copy lane frames without a mark.
 */
static int scp_reply_off(struct osdp_pd *pd)
{
	return osdp_phy_packet_get_data_offset(pd, scp_tx);
}

static uint8_t scp_reply_byte(struct osdp_pd *pd, int n)
{
	int off = scp_reply_off(pd) + n;

	return (scp_tx_len > off) ? scp_tx[off] : 0;
}

/* --- cases --- */

static bool keyless_pd_disables_sc(void)
{
	struct osdp *ctx = scp_pd_setup(NULL, 0);
	struct osdp_pd *pd;
	bool ok;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	ok = !sc_is_capable(pd) && !is_install_mode(pd) &&
	     ISSET_FLAG(pd, PD_FLAG_SC_DISABLED);
	osdp_pd_teardown((osdp_t *)ctx);
	return ok;
}

static bool install_mode_pd_keeps_sc(void)
{
	struct osdp *ctx = scp_pd_setup(NULL, OSDP_FLAG_INSTALL_MODE);
	struct osdp_pd *pd;
	bool ok;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	ok = sc_is_capable(pd) && is_install_mode(pd);
	osdp_pd_teardown((osdp_t *)ctx);
	return ok;
}

/* A key alone must not imply install mode -- that was the old behaviour. */
static bool keyed_pd_is_not_in_install_mode(void)
{
	struct osdp *ctx = scp_pd_setup(scp_scbk, 0);
	struct osdp_pd *pd;
	bool ok;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	ok = sc_is_capable(pd) && !is_install_mode(pd);
	osdp_pd_teardown((osdp_t *)ctx);
	return ok;
}

static bool cap_level_for(const uint8_t *scbk, uint32_t flags, int *level)
{
	struct osdp *ctx = scp_pd_setup(scbk, flags);
	struct osdp_pd *pd;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	*level = pd->cap[OSDP_PD_CAP_COMMUNICATION_SECURITY].compliance_level;
	osdp_pd_teardown((osdp_t *)ctx);
	return true;
}

static bool keyless_pd_advertises_no_comsec(void)
{
	int level = -1;

	return cap_level_for(NULL, 0, &level) && level == 0;
}

static bool install_mode_pd_advertises_comsec(void)
{
	int level = -1;

	return cap_level_for(NULL, OSDP_FLAG_INSTALL_MODE, &level) &&
	       level == 1;
}

/* The PDCAP reply must carry the level-0 entity, not omit it. */
static bool keyless_pd_pdcap_reports_no_comsec(void)
{
	struct osdp *ctx = scp_pd_setup(NULL, 0);
	const uint8_t reply_type = 0;
	struct osdp_pd *pd;
	uint8_t pkt[SCP_BUF_MAX];
	int len, i, off;
	bool seen = false, ok = false;

	if (!ctx) {
		return false;
	}
	/* CMD_CAP carries one reply-type byte */
	len = scp_build_cmd(0, NULL, 0, CMD_CAP, &reply_type, 1, pkt);
	scp_exchange(ctx, pkt, len);
	pd = osdp_to_pd(ctx, 0);
	if (scp_reply_byte(pd, 0) != REPLY_PDCAP) {
		goto out;
	}
	off = scp_reply_off(pd) + 1;
	for (i = off; i + 2 < scp_tx_len - 2; i += 3) {
		if (scp_tx[i] == OSDP_PD_CAP_COMMUNICATION_SECURITY) {
			seen = true;
			ok = (scp_tx[i + 1] == 0);
			break;
		}
	}
	ok = ok && seen;
out:
	osdp_pd_teardown((osdp_t *)ctx);
	return ok;
}

static bool chlng_reply_is(const uint8_t *scbk, uint32_t flags,
			   uint8_t want_reply, uint8_t want_nak)
{
	struct osdp *ctx = scp_pd_setup(scbk, flags);
	/* SCS_11 with SEC_BLK_DATA[0] = 0 asks for SCBK-D */
	const uint8_t sb[] = { 3, SCS_11, 0 };
	const uint8_t rnd[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	struct osdp_pd *pd;
	uint8_t pkt[SCP_BUF_MAX];
	int len;
	bool ok;

	if (!ctx) {
		return false;
	}
	len = scp_build_cmd(0, sb, sizeof(sb), CMD_CHLNG, rnd, sizeof(rnd),
			    pkt);
	scp_exchange(ctx, pkt, len);
	pd = osdp_to_pd(ctx, 0);
	ok = scp_reply_byte(pd, 0) == want_reply &&
	     (want_nak == 0 || scp_reply_byte(pd, 1) == want_nak);
	osdp_pd_teardown((osdp_t *)ctx);
	return ok;
}

static bool keyless_pd_naks_chlng(void)
{
	return chlng_reply_is(NULL, 0, REPLY_NAK, OSDP_PD_NAK_SC_UNSUP);
}

static bool install_mode_pd_answers_chlng(void)
{
	return chlng_reply_is(NULL, OSDP_FLAG_INSTALL_MODE, REPLY_CCRYPT, 0);
}

/* Clearing the capability after setup is the documented way to turn SC off;
 * it has to disable the channel too, not just change what we advertise. */
static bool set_capabilities_can_disable_sc(void)
{
	struct osdp_pd_cap off[] = {
		{ OSDP_PD_CAP_COMMUNICATION_SECURITY, 0, 0 },
		{ -1, -1, -1 }
	};
	struct osdp *ctx = scp_pd_setup(scp_scbk, 0);
	struct osdp_pd *pd;
	bool ok;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	ok = sc_is_capable(pd);
	ok = ok && osdp_pd_set_capabilities((osdp_t *)ctx, off) == 0;
	ok = ok && !sc_is_capable(pd);
	osdp_pd_teardown((osdp_t *)ctx);
	return ok;
}

/* A CP may have read the capabilities by the time the PD has been refreshed,
 * so they are fixed from then on. */
static bool set_capabilities_is_refused_after_refresh(void)
{
	struct osdp_pd_cap off[] = {
		{ OSDP_PD_CAP_COMMUNICATION_SECURITY, 0, 0 },
		{ -1, -1, -1 }
	};
	struct osdp *ctx = scp_pd_setup(scp_scbk, 0);
	struct osdp_pd *pd;
	bool ok;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	osdp_pd_refresh((osdp_t *)ctx);
	ok = osdp_pd_set_capabilities((osdp_t *)ctx, off) == -1;
	ok = ok && sc_is_capable(pd);
	osdp_pd_teardown((osdp_t *)ctx);
	return ok;
}

/* ENFORCE_SECURE undertakes never to use SCBK-D and INSTALL_MODE exists only
 * to permit it, so the pair contradicts itself. Pass a valid key too, so a
 * failure can only be the flag pair. */
static bool pd_setup_rejects_enforce_secure_with_install_mode(void)
{
	const uint32_t both = OSDP_FLAG_ENFORCE_SECURE | OSDP_FLAG_INSTALL_MODE;
	struct osdp *ctx = scp_pd_setup(scp_scbk, both);

	if (ctx) {
		osdp_pd_teardown((osdp_t *)ctx);
		return false;
	}
	return true;
}

static bool cp_setup_rejects_enforce_secure_with_install_mode(void)
{
	const uint32_t both = OSDP_FLAG_ENFORCE_SECURE | OSDP_FLAG_INSTALL_MODE;
	struct osdp_channel chn;
	osdp_pd_info_t info;
	osdp_t *ctx;

	scp_channel_reset();
	scp_fill_channel(&chn);
	scp_fill_pd_info(&info, scp_scbk, both);
	ctx = osdp_cp_setup(&chn, 1, &info);
	if (ctx) {
		osdp_cp_teardown(ctx);
		return false;
	}
	return true;
}

/* The pre-existing rule, kept honest now that a sibling check can shadow it. */
static bool cp_setup_rejects_enforce_secure_without_a_key(void)
{
	struct osdp_channel chn;
	osdp_pd_info_t info;
	osdp_t *ctx;

	scp_channel_reset();
	scp_fill_channel(&chn);
	scp_fill_pd_info(&info, NULL, OSDP_FLAG_ENFORCE_SECURE);
	ctx = osdp_cp_setup(&chn, 1, &info);
	if (ctx) {
		osdp_cp_teardown(ctx);
		return false;
	}
	return true;
}

static bool pd_setup_rejects_enforce_secure_without_a_key(void)
{
	struct osdp *ctx = scp_pd_setup(NULL, OSDP_FLAG_ENFORCE_SECURE);

	if (ctx) {
		osdp_pd_teardown((osdp_t *)ctx);
		return false;
	}
	return true;
}

/*
 * A failed challenge normally means "try SCBK-D, the PD may be in install
 * mode". Once a PD has answered on the configured key that inference is
 * wrong and dangerous: an attacker who fails one handshake would get a
 * session on a published key, and the CP would then hand it the real SCBK.
 */
static enum osdp_cp_state_e chlng_failure_state(bool established,
						bool enforce_secure)
{
	struct osdp_pd pd;

	memset(&pd, 0, sizeof(pd));
	pd.state = OSDP_CP_STATE_SC_CHLNG;
	if (established) {
		SET_FLAG(&pd, PD_FLAG_SC_ESTABLISHED);
	}
	if (enforce_secure) {
		SET_FLAG(&pd, PD_FLAG_ENFORCE_SECURE);
	}
	return test_get_next_err_state(&pd);
}

static bool first_chlng_failure_tries_scbkd(void)
{
	return chlng_failure_state(false, false) == OSDP_CP_STATE_SC_CHLNG;
}

static bool established_pd_never_falls_back_to_scbkd(void)
{
	return chlng_failure_state(true, false) == OSDP_CP_STATE_ONLINE;
}

static bool enforce_secure_still_wins_over_the_latch(void)
{
	return chlng_failure_state(true, true) == OSDP_CP_STATE_OFFLINE &&
	       chlng_failure_state(false, true) == OSDP_CP_STATE_OFFLINE;
}

static osdp_t *scp_cp_setup(const uint8_t *scbk, uint32_t flags)
{
	struct osdp_channel chn;
	osdp_pd_info_t info;

	scp_channel_reset();
	scp_fill_channel(&chn);
	scp_fill_pd_info(&info, scbk, flags);
	return osdp_cp_setup(&chn, 1, &info);
}

static int scp_modify(osdp_t *ctx, uint32_t flag, bool set)
{
	return osdp_cp_modify_flag(ctx, 0, flag, set);
}

/*
 * Every setup-time rule above can be undone from the app at any moment if the
 * runtime flag API will re-open it. Security posture is decided once.
 */
static bool modify_flag_refuses_install_mode(void)
{
	osdp_t *ctx = scp_cp_setup(scp_scbk, 0);
	bool ok;

	if (!ctx) {
		return false;
	}
	ok = scp_modify(ctx, OSDP_FLAG_INSTALL_MODE, true) == -1 &&
	     scp_modify(ctx, OSDP_FLAG_INSTALL_MODE, false) == -1;
	osdp_cp_teardown(ctx);
	return ok;
}

static bool modify_flag_refuses_clearing_enforce_secure(void)
{
	osdp_t *ctx = scp_cp_setup(scp_scbk, OSDP_FLAG_ENFORCE_SECURE);
	struct osdp_pd *pd;
	bool ok;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	/* Raising an already-raised guard is fine; lowering it is not. */
	ok = scp_modify(ctx, OSDP_FLAG_ENFORCE_SECURE, true) == 0 &&
	     scp_modify(ctx, OSDP_FLAG_ENFORCE_SECURE, false) == -1 &&
	     is_enforce_secure(pd);
	osdp_cp_teardown(ctx);
	return ok;
}

static bool modify_flag_refuses_enforce_secure_without_a_key(void)
{
	osdp_t *ctx = scp_cp_setup(NULL, 0);
	struct osdp_pd *pd;
	bool ok;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	ok = scp_modify(ctx, OSDP_FLAG_ENFORCE_SECURE, true) == -1 &&
	     !is_enforce_secure(pd);
	osdp_cp_teardown(ctx);
	return ok;
}

/* A refused flag must not let its companions through. */
static bool modify_flag_rejection_is_all_or_nothing(void)
{
	const uint32_t mixed = OSDP_FLAG_ENFORCE_SECURE |
			       OSDP_FLAG_IGN_UNSOLICITED;
	osdp_t *ctx = scp_cp_setup(scp_scbk, OSDP_FLAG_ENFORCE_SECURE |
					     OSDP_FLAG_IGN_UNSOLICITED);
	struct osdp_pd *pd;
	bool ok;

	if (!ctx) {
		return false;
	}
	pd = osdp_to_pd(ctx, 0);
	ok = scp_modify(ctx, mixed, false) == -1 &&
	     is_enforce_secure(pd) &&
	     ISSET_FLAG(pd, PD_FLAG_IGNORE_USR);
	osdp_cp_teardown(ctx);
	return ok;
}

void run_sc_policy_tests(struct test *t)
{
	printf("\nStarting sc_policy tests\n");

#ifndef OPT_OSDP_LOG_MINIMAL
	osdp_logger_init("osdp::pd", t->loglevel, NULL);
#else
	ARG_UNUSED(t);
#endif

	TEST_CASE(t, "keyless_pd_disables_sc", keyless_pd_disables_sc());
	TEST_CASE(t, "keyed_pd_is_not_in_install_mode",
		  keyed_pd_is_not_in_install_mode());
	TEST_CASE(t, "install_mode_pd_keeps_sc", install_mode_pd_keeps_sc());
	TEST_CASE(t, "keyless_pd_advertises_no_comsec",
		  keyless_pd_advertises_no_comsec());
	TEST_CASE(t, "install_mode_pd_advertises_comsec",
		  install_mode_pd_advertises_comsec());
	TEST_CASE(t, "keyless_pd_pdcap_reports_no_comsec",
		  keyless_pd_pdcap_reports_no_comsec());
	TEST_CASE(t, "keyless_pd_naks_chlng", keyless_pd_naks_chlng());
	TEST_CASE(t, "install_mode_pd_answers_chlng",
		  install_mode_pd_answers_chlng());
	TEST_CASE(t, "set_capabilities_can_disable_sc",
		  set_capabilities_can_disable_sc());
	TEST_CASE(t, "set_capabilities_is_refused_after_refresh",
		  set_capabilities_is_refused_after_refresh());
	TEST_CASE(t, "pd_setup_rejects_enforce_secure_with_install_mode",
		  pd_setup_rejects_enforce_secure_with_install_mode());
	TEST_CASE(t, "cp_setup_rejects_enforce_secure_with_install_mode",
		  cp_setup_rejects_enforce_secure_with_install_mode());
	TEST_CASE(t, "pd_setup_rejects_enforce_secure_without_a_key",
		  pd_setup_rejects_enforce_secure_without_a_key());
	TEST_CASE(t, "cp_setup_rejects_enforce_secure_without_a_key",
		  cp_setup_rejects_enforce_secure_without_a_key());
	TEST_CASE(t, "first_chlng_failure_tries_scbkd",
		  first_chlng_failure_tries_scbkd());
	TEST_CASE(t, "established_pd_never_falls_back_to_scbkd",
		  established_pd_never_falls_back_to_scbkd());
	TEST_CASE(t, "enforce_secure_still_wins_over_the_latch",
		  enforce_secure_still_wins_over_the_latch());
	TEST_CASE(t, "modify_flag_refuses_install_mode",
		  modify_flag_refuses_install_mode());
	TEST_CASE(t, "modify_flag_refuses_clearing_enforce_secure",
		  modify_flag_refuses_clearing_enforce_secure());
	TEST_CASE(t, "modify_flag_refuses_enforce_secure_without_a_key",
		  modify_flag_refuses_enforce_secure_without_a_key());
	TEST_CASE(t, "modify_flag_rejection_is_all_or_nothing",
		  modify_flag_rejection_is_all_or_nothing());
}
