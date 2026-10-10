/*
 * Copyright (c) 2022-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "osdp_common.h"
#include "osdp_trs.h"
#include "test.h"

#ifdef OPT_BUILD_OSDP_TRS

/*
 * Drives a full library-managed TRS session over the mock channel:
 *   CP submits OSDP_CMD_XWR(send-apdu) -> library runs mode-set handshake ->
 *   PD receives the APDU and answers with an OSDP_EVENT_TRS(card-data) ->
 *   CP delivers OSDP_EVENT_TRS to its app -> CP asks for an orderly stop.
 */

/* APDU the CP sends and the PD echoes back inside the card-data reply */
static const uint8_t g_req_apdu[] = { 0x00, 0xA4, 0x04, 0x00, 0x0E };
static const uint8_t g_rsp_apdu[] = { 0x6F, 0x1A, 0x84, 0x0E, 0x90, 0x00 };

struct test_trs_ctx {
	osdp_t *cp_ctx;
	osdp_t *pd_ctx;
	int cp_runner;
	int pd_runner;

	/* CP side: last OSDP_EVENT_TRS seen */
	bool event_seen;
	struct osdp_trs_reply last_reply;

	/* PD side: last CMD_XWR delivered to the app */
	bool cmd_seen;
	struct osdp_trs_cmd last_cmd;

	/* When set, the PD app does NOT answer the send-APDU in the callback;
	 * the test submits the R-APDU later to exercise the deferred path. */
	bool defer;

	/* When set, the PD app declines the send-APDU with a NAK */
	bool nak_apdu;

	/* When set, the PD app answers the send-APDU with an error report
	 * instead of card data */
	bool error_reply;

	/* PD reply event, built here and copied into the pool on submit. */
	struct osdp_event resp_event;

	/* CP side: last OSDP_NOTIFICATION_TRS_STATUS seen */
	bool status_seen;
	enum osdp_trs_session_status_e last_status;

	/* CP side: last command completion seen */
	/* Written by the completion callback on the refresh thread. */
	struct test_completion completion;

	/*
	 * How the PD app answers a card scan. OSDP_TRS_CARD_STATUS_UNKNOWN
	 * means "say nothing", which the CP reads as the reader declining the
	 * command -- the RPK40's behaviour, and the default here.
	 */
	enum osdp_trs_card_status_e card_status;
	int card_scan_count; /* card scans the PD app has been handed */

	/* CP side: card-present events counted by status */
	int present_events;
	int absent_events;
};

static struct test_trs_ctx g_trs = { 0 };

/* Build the card-data R-APDU reply into the app-owned resp_event */
static void trs_fill_response(struct test_trs_ctx *ctx)
{
	struct osdp_event *resp = &ctx->resp_event;

	memset(resp, 0, sizeof(*resp));
	resp->type = OSDP_EVENT_TRS;
	resp->trs.reply = OSDP_TRS_REPLY_CARD_DATA;
	resp->trs.card_data.reader = 0;
	resp->trs.card_data.status = 0;
	resp->trs.card_data.apdu.length = sizeof(g_rsp_apdu);
	memcpy(resp->trs.card_data.apdu.data, g_rsp_apdu, sizeof(g_rsp_apdu));
}

/* Build a transparent-mode error report into the app-owned resp_event */
static void trs_fill_error(struct test_trs_ctx *ctx)
{
	struct osdp_event *resp = &ctx->resp_event;

	memset(resp, 0, sizeof(*resp));
	resp->type = OSDP_EVENT_TRS;
	resp->trs.reply = OSDP_TRS_REPLY_ERROR;
	resp->trs.error.code = 0x6A;
}

static int trs_cp_event_callback(void *arg, int pd, struct osdp_event *ev)
{
	ARG_UNUSED(pd);
	struct test_trs_ctx *ctx = arg;

	if (ev->type == OSDP_EVENT_TRS) {
		ctx->event_seen = true;
		memcpy(&ctx->last_reply, &ev->trs, sizeof(ctx->last_reply));
		if (ev->trs.reply == OSDP_TRS_REPLY_CARD_PRESENT) {
			if (ev->trs.card_present.status ==
			    OSDP_TRS_CARD_NOT_PRESENT) {
				ctx->absent_events++;
			} else {
				ctx->present_events++;
			}
		}
	}
	if (ev->type == OSDP_EVENT_NOTIFICATION &&
	    ev->notif.type == OSDP_NOTIFICATION_TRS_STATUS) {
		ctx->status_seen = true;
		ctx->last_status = ev->notif.trs_status.status;
	}
	return 0;
}

static void trs_cp_completion_callback(void *arg, int pd,
				       struct osdp_cmd *cmd,
				       enum osdp_completion_status status)
{
	struct test_trs_ctx *ctx = arg;

	ARG_UNUSED(pd);
	if (cmd->id == OSDP_CMD_XWR) {
		test_completion_record(&ctx->completion, cmd->id, (int)status);
	}

	test_cmd_free(cmd);
}

/*
 * Completion callbacks can only be set before the first refresh, so the CP
 * gets one that forwards to whatever the running case swaps in.
 */
static cp_command_completion_callback_t g_trs_completion =
	trs_cp_completion_callback;
static void *g_trs_completion_arg = &g_trs;

static void trs_completion_dispatch(void *arg, int pd, struct osdp_cmd *cmd,
				    enum osdp_completion_status status)
{
	ARG_UNUSED(arg);
	g_trs_completion(g_trs_completion_arg, pd, cmd, status);
}

static void trs_use_completion(cp_command_completion_callback_t cb, void *arg)
{
	test_api_lock();
	g_trs_completion = cb;
	g_trs_completion_arg = arg;
	test_api_unlock();
}

/*
 * Submit a bare TRS marker/command (START, STOP, ...) to the CP. The command is
 * queued by reference, so @a cmd is the caller's to keep alive until the command
 * completes -- callers here block on the session notification that follows it.
 */
static int submit_trs_cmd(struct osdp_cmd *cmd, enum osdp_trs_cmd_e command)
{
	*cmd = (struct osdp_cmd){
		.id = OSDP_CMD_XWR,
		.trs = { .command = command },
	};

	return test_submit_command(g_trs.cp_ctx, 0, cmd) ? 0 : -1;
}

static bool wait_for_trs_status(enum osdp_trs_session_status_e want,
				int timeout_sec)
{
	int rc = 0;

	while (rc++ < timeout_sec) {
		if (g_trs.status_seen && g_trs.last_status == want) {
			return true;
		}
		usleep(1000 * 1000);
	}
	return false;
}

static int trs_pd_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_trs_ctx *ctx = arg;

	if (cmd->id != OSDP_CMD_XWR) {
		return 0;
	}

	ctx->cmd_seen = true;
	memcpy(&ctx->last_cmd, &cmd->trs, sizeof(ctx->last_cmd));

	/* The app cannot act on this APDU; decline it */
	if (ctx->nak_apdu) {
		return -OSDP_PD_NAK_RECORD;
	}

	if (cmd->trs.command == OSDP_TRS_CMD_CARD_SCAN) {
		ctx->card_scan_count++;
		if (ctx->card_status == OSDP_TRS_CARD_STATUS_UNKNOWN) {
			/* Reader has no answer for this command */
			return 0;
		}
		memset(&ctx->resp_event, 0, sizeof(ctx->resp_event));
		ctx->resp_event.type = OSDP_EVENT_TRS;
		ctx->resp_event.trs.reply = OSDP_TRS_REPLY_CARD_PRESENT;
		ctx->resp_event.trs.card_present.reader = 0;
		ctx->resp_event.trs.card_present.status = ctx->card_status;
		test_submit_event(ctx->pd_ctx, &ctx->resp_event);
		return 0;
	}

	/* Fast card: answer the send-APDU synchronously so the R-APDU rides
	 * back in the immediate REPLY_XRD. In deferred mode the app submits the
	 * response later (see test_trs_deferred_apdu), so the PD replies ACK
	 * ("working") and the R-APDU is delivered on a subsequent poll. */
	if (cmd->trs.command == OSDP_TRS_CMD_SEND_APDU && !ctx->defer) {
		if (ctx->error_reply) {
			trs_fill_error(ctx);
		} else {
			trs_fill_response(ctx);
		}
		test_submit_event(ctx->pd_ctx, &ctx->resp_event);
	}
	return 0;
}

static int setup_test_environment_ex(struct test *t, bool plaintext)
{
	int rc;

	printf(SUB_1 "setting up OSDP devices%s\n",
	       plaintext ? " (plaintext link)" : "");

	if (plaintext) {
		rc = test_setup_devices_plain(t, &g_trs.cp_ctx, &g_trs.pd_ctx,
					      OSDP_FLAG_ENABLE_NOTIFICATION, 0);
	} else {
		rc = test_setup_devices_ext(t, &g_trs.cp_ctx, &g_trs.pd_ctx,
					    OSDP_FLAG_ENABLE_NOTIFICATION, 0);
	}
	if (rc) {
		printf(SUB_1 "Failed to setup devices!\n");
		return -1;
	}

	osdp_cp_set_event_callback(g_trs.cp_ctx, trs_cp_event_callback, &g_trs);
	osdp_cp_set_command_completion_callback(g_trs.cp_ctx,
						trs_completion_dispatch, NULL);
	trs_use_completion(trs_cp_completion_callback, &g_trs);
	osdp_pd_set_command_callback(g_trs.pd_ctx, trs_pd_command_callback,
				     &g_trs);

	printf(SUB_1 "starting async runners\n");
	g_trs.cp_runner = async_runner_start(g_trs.cp_ctx, osdp_cp_refresh);
	g_trs.pd_runner = async_runner_start(g_trs.pd_ctx, osdp_pd_refresh);
	if (g_trs.cp_runner < 0 || g_trs.pd_runner < 0) {
		printf(SUB_1 "Failed to create CP/PD runners\n");
		return -1;
	}

	if (!test_wait_for_online(g_trs.cp_ctx, 0, 10)) {
		printf(SUB_1 "PD failed to come online\n");
		return -1;
	}
	return 0;
}

static int setup_test_environment(struct test *t)
{
	return setup_test_environment_ex(t, false);
}

static void teardown_test_environment()
{
	printf(SUB_1 "tearing down test environment\n");
	async_runner_stop(g_trs.cp_runner);
	async_runner_stop(g_trs.pd_runner);
	osdp_cp_teardown(g_trs.cp_ctx);
	osdp_pd_teardown(g_trs.pd_ctx);
	memset(&g_trs, 0, sizeof(g_trs));
}

static bool wait_for_trs_event(int timeout_sec)
{
	int rc = 0;
	while (rc++ < timeout_sec) {
		if (g_trs.event_seen) {
			return true;
		}
		usleep(1000 * 1000);
	}
	return false;
}

static bool wait_for_trs_completion(int timeout_sec)
{
	int rc = 0;
	while (rc++ < timeout_sec) {
		if (test_completion_count(&g_trs.completion)) {
			return true;
		}
		usleep(1000 * 1000);
	}
	return false;
}

static uint8_t pd_trs_mode(void)
{
	return osdp_to_pd(g_trs.pd_ctx, 0)->trs.mode;
}

/* Sample the PD's reader mode every 10ms until it reads @want */
static bool wait_for_pd_mode(uint8_t want, int timeout_ms)
{
	int waited = 0;

	while (waited < timeout_ms) {
		if (pd_trs_mode() == want) {
			return true;
		}
		usleep(10 * 1000);
		waited += 10;
	}
	return false;
}

/*
 * An APDU that cannot fit the negotiated packet size must be turned away at
 * submit time, not fail mysteriously somewhere mid-band.
 */
static bool test_trs_apdu_too_big_for_packet(void)
{
	/* Static: were a defective admission check to accept it, the queue
	 * would keep a reference past this function's frame. */
	static struct osdp_cmd cmd;

	printf(SUB_2 "testing TRS APDU beyond packet capacity\n");

	if (OSDP_TRS_APDU_MAX_LEN + 64 <= OSDP_PACKET_BUF_SIZE) {
		/* Carrier fits the packet with room to spare on this build
		 * config; nothing to reject. */
		return true;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.id = OSDP_CMD_XWR;
	cmd.trs.command = OSDP_TRS_CMD_SEND_APDU;
	cmd.trs.apdu.length = OSDP_TRS_APDU_MAX_LEN;

	if (test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "packet-overflowing APDU must be rejected at submit\n");
		return false;
	}
	return true;
}

/*
 * The reported capacity is the real admission boundary: exactly that many bytes
 * go through, one more does not. An app that sizes payloads against
 * OSDP_TRS_APDU_MAX_LEN instead is sizing against the buffer, not the wire.
 */
static bool test_trs_max_apdu_len_accessor(void)
{
	static struct osdp_cmd cmd;
	int max_len;

	printf(SUB_2 "testing TRS max-APDU accessor\n");

	max_len = osdp_cp_trs_get_max_apdu_len(g_trs.cp_ctx, 0);
	if (max_len <= 0 || max_len > OSDP_TRS_APDU_MAX_LEN) {
		printf(SUB_2 "implausible max APDU length %d\n", max_len);
		return false;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.id = OSDP_CMD_XWR;
	cmd.trs.command = OSDP_TRS_CMD_SEND_APDU;
	cmd.trs.apdu.length = (uint16_t)(max_len + 1);
	if (test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "APDU of max+1 bytes must be rejected\n");
		return false;
	}
	return true;
}

/*
 * A PIN entry carries its APDU behind the PIN parameter block, so it has less
 * room than a send-APDU -- and must be turned away at submit time too, not part
 * way through a band where the app can no longer act on it.
 */
static bool test_trs_pin_entry_too_big_for_packet(void)
{
	static struct osdp_cmd cmd;
	int max_len;

	printf(SUB_2 "testing TRS PIN-entry APDU beyond packet capacity\n");

	max_len = osdp_cp_trs_get_max_apdu_len(g_trs.cp_ctx, 0);
	if (max_len <= 0) {
		printf(SUB_2 "no max APDU length to size against\n");
		return false;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.id = OSDP_CMD_XWR;
	cmd.trs.command = OSDP_TRS_CMD_ENTER_PIN;
	cmd.trs.pin_entry.pin_block.format = OSDP_TRS_PIN_FORMAT_BINARY;
	/* Fits a send-APDU exactly, so only the PIN block's own overhead can
	 * be what rejects it. */
	cmd.trs.pin_entry.apdu.length = (uint16_t)max_len;

	if (test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "PIN APDU with no room for its block must be "
		       "rejected at submit\n");
		return false;
	}
	return true;
}

/*
 * A PIN-entry layout the wire cannot carry must be turned away at submit too;
 * the frame builder would otherwise refuse it only once the band is running.
 */
static bool test_trs_pin_entry_layout_unrepresentable(void)
{
	static struct osdp_cmd cmd;
	struct osdp_trs_pin_entry *pe = &cmd.trs.pin_entry;
	int i;

	printf(SUB_2 "testing TRS PIN-entry layout the wire cannot carry\n");

	for (i = 0; i < 5; i++) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.id = OSDP_CMD_XWR;
		cmd.trs.command = OSDP_TRS_CMD_ENTER_PIN;
		pe->pin_block.format = OSDP_TRS_PIN_FORMAT_BINARY;
		pe->pin_block.size_bytes = 8;
		pe->apdu.length = 16;
		switch (i) {
		case 0:
			pe->pin_block.offset_bits = 17;
			break;
		case 1:
			pe->pin_length_field.offset_bits = 128;
			break;
		case 2:
			pe->pin_length_field.size_bits = 16;
			break;
		case 3:
			pe->pin_block.size_bytes = 16;
			break;
		case 4:
			pe->pin_block.format = 0;
			break;
		}
		if (test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
			printf(SUB_2 "layout %d must be rejected at submit\n",
			       i);
			return false;
		}
	}
	return true;
}

/* A card command with no START must be turned away at submit, not queued */
static bool test_trs_apdu_outside_band()
{
	printf(SUB_2 "testing TRS APDU outside a session\n");

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_XWR,
		.trs = {
			.command = OSDP_TRS_CMD_SEND_APDU,
			.apdu = { .length = sizeof(g_req_apdu) },
		},
	};
	memcpy(cmd.trs.apdu.data, g_req_apdu, sizeof(g_req_apdu));

	if (test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "sessionless APDU must be rejected\n");
		return false;
	}
	/* So must a STOP that closes nothing */
	struct osdp_cmd stop;

	if (submit_trs_cmd(&stop, OSDP_TRS_CMD_STOP) == 0) {
		printf(SUB_2 "sessionless TRS stop must be rejected\n");
		return false;
	}
	return true;
}

/* Inside a band, a non-TRS command is refused: it would cut the card session */
static bool test_trs_non_trs_cmd_in_band()
{
	printf(SUB_2 "testing non-TRS command inside a session\n");

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_BUZZER,
		.buzzer = { .reader = 0, .control_code = 1, .on_count = 10,
			    .off_count = 10, .rep_count = 1 },
	};

	if (test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "in-band buzzer command must be rejected\n");
		return false;
	}
	/* A nested START is refused too */
	struct osdp_cmd start;

	if (submit_trs_cmd(&start, OSDP_TRS_CMD_START) == 0) {
		printf(SUB_2 "nested TRS start must be rejected\n");
		return false;
	}
	return true;
}

/* START opens the band: the library negotiates transparent mode and says so */
static bool test_trs_session_start()
{
	printf(SUB_2 "testing TRS session start\n");

	struct osdp_cmd cmd;

	g_trs.status_seen = false;

	if (submit_trs_cmd(&cmd, OSDP_TRS_CMD_START)) {
		printf(SUB_2 "Failed to submit TRS start\n");
		return false;
	}
	if (!wait_for_trs_status(OSDP_TRS_SESSION_OPENED, 10)) {
		printf(SUB_2 "TRS session-opened notification not received\n");
		return false;
	}
	return true;
}

static bool test_trs_apdu_exchange()
{
	printf(SUB_2 "testing TRS APDU exchange (synchronous)\n");

	g_trs.event_seen = false;
	g_trs.cmd_seen = false;
	g_trs.defer = false;

	/* Stack storage must outlive the submission; wait_for_trs_event()
	 * below blocks here until the whole exchange completes. */
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_XWR,
		.trs = {
			.command = OSDP_TRS_CMD_SEND_APDU,
			.apdu = {
				.length = sizeof(g_req_apdu),
			},
		},
	};
	memcpy(cmd.trs.apdu.data, g_req_apdu, sizeof(g_req_apdu));

	if (!test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to submit TRS command\n");
		return false;
	}

	if (!wait_for_trs_event(10)) {
		printf(SUB_2 "TRS event not received\n");
		return false;
	}

	/* PD must have received the exact APDU the CP sent */
	if (!g_trs.cmd_seen ||
	    g_trs.last_cmd.command != OSDP_TRS_CMD_SEND_APDU ||
	    g_trs.last_cmd.apdu.length != (int)sizeof(g_req_apdu) ||
	    memcmp(g_trs.last_cmd.apdu.data, g_req_apdu,
		   sizeof(g_req_apdu)) != 0) {
		printf(SUB_2 "PD did not receive the request APDU intact\n");
		return false;
	}

	/* CP must have received the card-data reply the PD sent */
	if (g_trs.last_reply.reply != OSDP_TRS_REPLY_CARD_DATA ||
	    g_trs.last_reply.card_data.apdu.length != (int)sizeof(g_rsp_apdu) ||
	    memcmp(g_trs.last_reply.card_data.apdu.data, g_rsp_apdu,
		   sizeof(g_rsp_apdu)) != 0) {
		printf(SUB_2 "CP did not receive the response APDU intact\n");
		return false;
	}

	return true;
}

static bool test_trs_deferred_apdu()
{
	int rc;

	printf(SUB_2 "testing TRS APDU exchange (deferred / slow card)\n");

	/* The session is already active (TRS_RUN) from the synchronous test;
	 * submit a second C-APDU that the PD app answers late. */
	g_trs.event_seen = false;
	g_trs.cmd_seen = false;
	g_trs.defer = true;

	struct osdp_cmd cmd = {
		.id = OSDP_CMD_XWR,
		.trs = {
			.command = OSDP_TRS_CMD_SEND_APDU,
			.apdu = {
				.length = sizeof(g_req_apdu),
			},
		},
	};
	memcpy(cmd.trs.apdu.data, g_req_apdu, sizeof(g_req_apdu));

	if (!test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to submit deferred TRS command\n");
		return false;
	}

	/* Wait until the PD has received the C-APDU and replied ACK ("working").
	 * The PD app deliberately did not answer, so no event must arrive yet. */
	rc = 0;
	while (rc++ < 8 && !g_trs.cmd_seen) {
		usleep(1000 * 1000);
	}
	if (!g_trs.cmd_seen) {
		printf(SUB_2 "PD never received the deferred C-APDU\n");
		g_trs.defer = false;
		return false;
	}
	if (g_trs.event_seen) {
		printf(SUB_2 "R-APDU arrived before the deferred submit\n");
		g_trs.defer = false;
		return false;
	}

	/* Simulate the card finally responding: the app submits the R-APDU now.
	 * The CP is still polling, so it rides out on the next poll as XRD. */
	trs_fill_response(&g_trs);
	if (!test_submit_event(g_trs.pd_ctx, &g_trs.resp_event)) {
		printf(SUB_2 "Failed to submit deferred R-APDU\n");
		g_trs.defer = false;
		return false;
	}

	if (!wait_for_trs_event(10)) {
		printf(SUB_2 "Deferred TRS event not received\n");
		g_trs.defer = false;
		return false;
	}

	g_trs.defer = false;

	/* The deferred R-APDU must round-trip intact */
	if (g_trs.last_reply.reply != OSDP_TRS_REPLY_CARD_DATA ||
	    g_trs.last_reply.card_data.apdu.length != (int)sizeof(g_rsp_apdu) ||
	    memcmp(g_trs.last_reply.card_data.apdu.data, g_rsp_apdu,
		   sizeof(g_rsp_apdu)) != 0) {
		printf(SUB_2 "Deferred response APDU mismatch\n");
		return false;
	}

	return true;
}

/*
 * Submit one send-APDU and wait for the CP to complete it. The command is queued
 * by reference, so it must outlive the exchange: this blocks until it does.
 */
static bool submit_apdu_and_wait(enum osdp_completion_status *status)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_XWR,
		.trs = {
			.command = OSDP_TRS_CMD_SEND_APDU,
			.apdu = { .length = sizeof(g_req_apdu) },
		},
	};
	memcpy(cmd.trs.apdu.data, g_req_apdu, sizeof(g_req_apdu));

	test_completion_reset(&g_trs.completion);
	if (!test_submit_command(g_trs.cp_ctx, 0, &cmd)) {
		printf(SUB_2 "Failed to submit TRS command\n");
		return false;
	}
	if (!wait_for_trs_completion(10)) {
		printf(SUB_2 "TRS command never completed\n");
		return false;
	}
	*status = (enum osdp_completion_status)
		 test_completion_status(&g_trs.completion);
	return true;
}

/*
 * A PD that declines one APDU has not broken the card session. The command must
 * fail, no session-failure must be reported, and the next APDU must still go
 * through on the same session.
 */
static bool test_trs_apdu_nak_keeps_session()
{
	enum osdp_completion_status status;
	bool ok;

	printf(SUB_2 "testing TRS APDU declined by the PD\n");

	g_trs.event_seen = false;
	g_trs.cmd_seen = false;
	g_trs.status_seen = false;
	g_trs.nak_apdu = true;

	ok = submit_apdu_and_wait(&status);
	g_trs.nak_apdu = false;
	if (!ok) {
		return false;
	}

	if (!g_trs.cmd_seen) {
		printf(SUB_2 "PD never received the C-APDU\n");
		return false;
	}
	if (status != OSDP_COMPLETION_FAILED) {
		printf(SUB_2 "NAK'd APDU must complete as failed\n");
		return false;
	}
	if (g_trs.event_seen) {
		printf(SUB_2 "NAK'd APDU must not produce a card-data event\n");
		return false;
	}
	/* Any session status here can only be OPENED->FAILED/CLOSED: the
	 * session was already open, so a notification means it ended. */
	if (g_trs.status_seen) {
		printf(SUB_2 "NAK'd APDU must not end the session (status %d)\n",
		       g_trs.last_status);
		return false;
	}

	/* The session survived: the next APDU must round-trip as usual */
	return test_trs_apdu_exchange();
}

/*
 * The reader took the APDU but reports that the card transaction failed. The app
 * gets the error event and a failed command; the session stays open.
 */
static bool test_trs_apdu_error_reply_fails_cmd()
{
	enum osdp_completion_status status;
	bool ok;

	printf(SUB_2 "testing TRS APDU answered with an error report\n");

	g_trs.event_seen = false;
	g_trs.status_seen = false;
	g_trs.error_reply = true;

	ok = submit_apdu_and_wait(&status);
	g_trs.error_reply = false;
	if (!ok) {
		return false;
	}

	if (!g_trs.event_seen ||
	    g_trs.last_reply.reply != OSDP_TRS_REPLY_ERROR ||
	    g_trs.last_reply.error.code != 0x6A) {
		printf(SUB_2 "CP did not receive the error report\n");
		return false;
	}
	if (status != OSDP_COMPLETION_FAILED) {
		printf(SUB_2 "APDU answered with an error must complete as failed\n");
		return false;
	}
	if (g_trs.status_seen) {
		printf(SUB_2 "error report must not end the session (status %d)\n",
		       g_trs.last_status);
		return false;
	}

	return test_trs_apdu_exchange();
}

extern uint16_t test_osdp_compute_crc16(const uint8_t *buf, size_t len);

struct trs_wire_nak_hook {
	int xwr_tx_count;
	bool nak_next_reply;
	/* Refuse only the mode-1 card scan, leaving every other XWR alone --
	 * a reader that implements transparent mode but not that one command */
	bool card_scan_only;
	/* Swallow the answer instead of refusing it: silence, not a verdict */
	bool drop_reply;
	int card_scan_tx_count;
};

/*
 * Model the RPK40 declining a CARD_SCAN it does not implement: the command
 * is delivered (so the PD's sequence tracking stays honest), but the reply
 * going back is rewritten into a plaintext NAK(MSG_CHK) at the reply's own
 * sequence number.
 */
static enum test_channel_hook_verdict trs_wire_nak_hook(void *arg,
		bool cp_to_pd, const uint8_t *frame, int len,
		uint8_t *out, int *out_len, int out_max)
{
	struct trs_wire_nak_hook *s = arg;
	int base, data_off, n = 0, start, pkt_len;
	uint8_t ctrl;
	uint16_t crc;

	/* The leading mark byte depends on build options and direction;
	 * detect it instead of assuming (a frame proper starts at SOM) */
	base = (len > 0 && frame[0] == 0xff) ? 1 : 0;
	if (len < base + 6)
		return TEST_HOOK_PASS;
	ctrl = frame[base + 4];

	if (cp_to_pd) {
		data_off = base + 5;
		if (ctrl & 0x08) /* security control block */
			data_off += frame[base + 5];
		if (data_off < len && frame[data_off] == CMD_XWR) {
			bool is_scan = data_off + 2 < len &&
				       frame[data_off + 1] == 0x01 &&
				       frame[data_off + 2] == 0x04;

			s->xwr_tx_count++;
			if (is_scan) {
				s->card_scan_tx_count++;
			}
			if (!s->card_scan_only || is_scan) {
				s->nak_next_reply = true;
			}
		}
		return TEST_HOOK_PASS;
	}

	if (!s->nak_next_reply)
		return TEST_HOOK_PASS;
	if (s->drop_reply) {
		s->nak_next_reply = false;
		return TEST_HOOK_DROP;
	}
	if (out_max < 11)
		return TEST_HOOK_PASS;
	s->nak_next_reply = false;

	n = 0;
#ifndef OPT_OSDP_SKIP_MARK_BYTE
	out[n++] = 0xff;
#endif
	start = n;
	out[n++] = 0x53;
	out[n++] = 0x65 | 0x80;	/* PD address 101, reply direction */
	pkt_len = 5 + 2 + 2;	/* header + NAK/code + CRC */
	out[n++] = pkt_len & 0xff;
	out[n++] = (pkt_len >> 8) & 0xff;
	out[n++] = 0x04 | (ctrl & 0x03); /* CRC, reply's sequence */
	out[n++] = REPLY_NAK;
	out[n++] = OSDP_PD_NAK_MSG_CHK;
	crc = test_osdp_compute_crc16(out + start, n - start);
	out[n++] = crc & 0xff;
	out[n++] = (crc >> 8) & 0xff;
	*out_len = n;
	return TEST_HOOK_REPLACE;
}

/*
 * Not every reader implements CARD_SCAN: the RPK40 answers it with a wire
 * level NAK(MSG_CHK). An APDU-bearing command must never be blind
 * retransmitted -- replaying card traffic can have card-side effects -- so
 * the NAK costs the app exactly one failed command: one transmission, no
 * resends, session intact.
 */
static bool test_trs_card_scan_wire_nak(void)
{
	struct trs_wire_nak_hook s = { 0 };
	struct osdp_cmd cmd;
	int rc = 0;

	printf(SUB_2 "testing TRS CARD_SCAN declined on the wire\n");

	test_completion_reset(&g_trs.completion);
	g_trs.status_seen = false;
	g_trs.event_seen = false;

	test_set_channel_hook(trs_wire_nak_hook, &s);
	if (submit_trs_cmd(&cmd, OSDP_TRS_CMD_CARD_SCAN)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "failed to submit CARD_SCAN\n");
		return false;
	}
	while (rc++ < 120 && !test_completion_count(&g_trs.completion)) {
		usleep(100 * 1000);
	}
	test_set_channel_hook(NULL, NULL);

	if (!test_completion_count(&g_trs.completion)) {
		printf(SUB_2 "CARD_SCAN never completed\n");
		return false;
	}
	if (test_completion_status(&g_trs.completion) !=
		    OSDP_COMPLETION_FAILED) {
		printf(SUB_2 "NAK'd CARD_SCAN must complete as failed\n");
		return false;
	}
	if (s.xwr_tx_count != 1) {
		printf(SUB_2 "NAK'd CARD_SCAN must not be retransmitted; "
		       "saw %d transmissions\n", s.xwr_tx_count);
		return false;
	}
	if (g_trs.status_seen) {
		printf(SUB_2 "NAK'd CARD_SCAN must not end the session "
		       "(status %d)\n", g_trs.last_status);
		return false;
	}

	return test_trs_apdu_exchange();
}

/* Count mode-set commands on the wire (plaintext link only: the payload of a
 * secure frame is encrypted, so there is nothing to parse there) */
struct trs_mode_set_counter {
	int set_mode1;
	int set_mode0;
};

static enum test_channel_hook_verdict trs_mode_set_count_hook(void *arg,
		bool cp_to_pd, const uint8_t *frame, int len,
		uint8_t *out, int *out_len, int out_max)
{
	struct trs_mode_set_counter *s = arg;
	int base, data_off;
	uint8_t ctrl;

	ARG_UNUSED(out);
	ARG_UNUSED(out_len);
	ARG_UNUSED(out_max);

	if (!cp_to_pd)
		return TEST_HOOK_PASS;
	base = (len > 0 && frame[0] == 0xff) ? 1 : 0;
	if (len < base + 6)
		return TEST_HOOK_PASS;
	ctrl = frame[base + 4];
	data_off = base + 5;
	if (ctrl & 0x08) /* security control block */
		data_off += frame[base + 5];
	/* CMD_XWR carrying [XRW_MODE=00, PCMND=02 (mode-set), target mode] */
	if (data_off + 3 >= len || frame[data_off] != CMD_XWR ||
	    frame[data_off + 1] != 0x00 || frame[data_off + 2] != 0x02)
		return TEST_HOOK_PASS;
	if (frame[data_off + 3] == TRS_MODE_01)
		s->set_mode1++;
	else
		s->set_mode0++;
	return TEST_HOOK_PASS;
}

/*
 * The point of the hold: a sighting during a probe keeps the reader in
 * transparent mode long enough for the app's START to take the session over
 * as-is. The wire must show exactly one mode-set(01) -- the probe's -- from
 * enable to band close: adoption renegotiates nothing.
 */
static bool test_trs_scan_hold_and_adopt(void)
{
	struct trs_mode_set_counter s = { 0 };
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 80,
		.mode1_dwell_ms = 300,
		.hold_ms = 3000,
	};
	struct osdp_cmd start, stop;

	printf(SUB_2 "testing TRS sighting hold and band adoption\n");

	g_trs.event_seen = false;
	g_trs.status_seen = false;

	test_set_channel_hook(trs_mode_set_count_hook, &s);
	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}
	if (!wait_for_pd_mode(TRS_MODE_01, 3000)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "no probe to sight a card in\n");
		return false;
	}

	/* The reader sights a card during the probe */
	memset(&g_trs.resp_event, 0, sizeof(g_trs.resp_event));
	g_trs.resp_event.type = OSDP_EVENT_TRS;
	g_trs.resp_event.trs.reply = OSDP_TRS_REPLY_CARD_PRESENT;
	g_trs.resp_event.trs.card_present.reader = 0;
	g_trs.resp_event.trs.card_present.status =
		OSDP_TRS_CARD_PRESENT_CONTACTLESS;
	if (!test_submit_event(g_trs.pd_ctx, &g_trs.resp_event)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "failed to submit sighting\n");
		return false;
	}
	if (!wait_for_trs_event(10) ||
	    g_trs.last_reply.reply != OSDP_TRS_REPLY_CARD_PRESENT) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "sighting not delivered to the app\n");
		return false;
	}

	/* The app answers with a band */
	if (submit_trs_cmd(&start, OSDP_TRS_CMD_START) ||
	    !wait_for_trs_status(OSDP_TRS_SESSION_OPENED, 10)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "band did not open on the sighting\n");
		return false;
	}
	if (!test_trs_apdu_exchange()) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "adopted session unusable\n");
		return false;
	}
	/* Stop scanning before the band closes so no fresh probe muddies the
	 * mode-set count between the CLOSED notification and the checks */
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
	g_trs.status_seen = false;
	if (submit_trs_cmd(&stop, OSDP_TRS_CMD_STOP) ||
	    !wait_for_trs_status(OSDP_TRS_SESSION_CLOSED, 10)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "band did not close\n");
		return false;
	}
	test_set_channel_hook(NULL, NULL);

	if (s.set_mode1 != 1) {
		printf(SUB_2 "adoption must not renegotiate the mode; "
		       "saw %d mode-set(01)\n", s.set_mode1);
		return false;
	}
	if (s.set_mode0 < 1) {
		printf(SUB_2 "band close must restore the default mode\n");
		return false;
	}
	return true;
}

/*
 * A reader that refuses probes (NAK to every XWR) must cost the app one
 * SCAN_SUSPENDED notification per attempt and progressively fewer attempts
 * (exponential backoff) -- never the link. Removing the fault heals the scan.
 */
static bool test_trs_scan_probe_failure_backoff(void)
{
	struct trs_wire_nak_hook s = { 0 };
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 80,
		.mode1_dwell_ms = 100,
	};
	uint8_t status = 0;
	int early_count, late_count;

	printf(SUB_2 "testing TRS scan probe failure backoff\n");

	g_trs.status_seen = false;

	test_set_channel_hook(trs_wire_nak_hook, &s);
	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}

	/* Give the first refused probes time to happen and back off */
	usleep(2000 * 1000);
	early_count = s.xwr_tx_count;
	if (early_count < 2) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "refused probes never retried (%d)\n", early_count);
		return false;
	}
	if (!g_trs.status_seen ||
	    g_trs.last_status != OSDP_TRS_SCAN_SUSPENDED) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "refused probe must raise SCAN_SUSPENDED\n");
		return false;
	}

	/* Backed off: the next window must see far fewer transmissions than
	 * the ~25 probes an 80ms cadence would fire. A refused probe costs two
	 * XWRs -- the mode-set and the just-in-case teardown behind it. */
	usleep(3000 * 1000);
	late_count = s.xwr_tx_count - early_count;
	if (late_count > 4) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "no backoff: %d transmissions in 3s\n", late_count);
		return false;
	}
	osdp_get_status_mask(g_trs.cp_ctx, &status);
	if (!(status & 1)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "refused probes must not cost the link\n");
		return false;
	}

	/* Fault gone: probing must recover on its own */
	test_set_channel_hook(NULL, NULL);
	if (!wait_for_pd_mode(TRS_MODE_01, 8000)) {
		printf(SUB_2 "scan did not recover after the fault cleared\n");
		return false;
	}
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
	if (!wait_for_pd_mode(TRS_MODE_00, 3000)) {
		printf(SUB_2 "reader stuck in mode 01 after recovery\n");
		return false;
	}
	return true;
}

/*
 * A reader that refuses the card scan but honours everything else is a verdict
 * about one command, not a broken link: ask once, take the answer, tell the app
 * that removal will not be reported, and carry on probing without it.
 */
static bool test_trs_scan_card_scan_unsupported_latches(void)
{
	struct trs_wire_nak_hook s = { .card_scan_only = true };
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 80,
		.mode1_dwell_ms = 150,
	};
	int waited = 0, asked_at_verdict;
	uint8_t status = 0;

	printf(SUB_2 "testing TRS card-scan refusal latches\n");

	g_trs.status_seen = false;
	test_completion_reset(&g_trs.completion);
	g_trs.card_status = OSDP_TRS_CARD_STATUS_UNKNOWN;

	test_set_channel_hook(trs_wire_nak_hook, &s);
	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}
	while (waited < 5000 &&
	       !(g_trs.status_seen &&
		 g_trs.last_status == OSDP_TRS_SCAN_NO_DEPARTURE)) {
		usleep(50 * 1000);
		waited += 50;
	}
	if (!g_trs.status_seen ||
	    g_trs.last_status != OSDP_TRS_SCAN_NO_DEPARTURE) {
		test_set_channel_hook(NULL, NULL);
		osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
		printf(SUB_2 "refusal never reported to the app\n");
		return false;
	}
	asked_at_verdict = s.card_scan_tx_count;

	/* Having heard the answer, the scan must stop asking */
	g_trs.status_seen = false;
	usleep(2000 * 1000);
	test_set_channel_hook(NULL, NULL);
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);

	if (s.card_scan_tx_count != asked_at_verdict) {
		printf(SUB_2 "kept asking after the refusal (%d -> %d)\n",
		       asked_at_verdict, s.card_scan_tx_count);
		return false;
	}
	if (g_trs.status_seen &&
	    g_trs.last_status == OSDP_TRS_SCAN_NO_DEPARTURE) {
		printf(SUB_2 "refusal reported more than once\n");
		return false;
	}
	if (g_trs.status_seen &&
	    g_trs.last_status == OSDP_TRS_SCAN_SUSPENDED) {
		printf(SUB_2 "a refused card scan must not suspend the scan\n");
		return false;
	}
	if (test_completion_count(&g_trs.completion)) {
		printf(SUB_2 "library's own card scan reported a completion\n");
		return false;
	}
	osdp_get_status_mask(g_trs.cp_ctx, &status);
	if (!(status & 1)) {
		printf(SUB_2 "PD dropped offline over a refused card scan\n");
		return false;
	}
	return true;
}

/*
 * Silence is not a refusal. A card scan that never comes back says nothing
 * about whether the reader implements it -- that is the link failing, and it
 * must take the ordinary session-error path rather than latching a verdict
 * that would outlive the fault.
 */
static bool test_trs_scan_card_scan_timeout_is_not_unsupported(void)
{
	struct trs_wire_nak_hook s = { .card_scan_only = true,
				       .drop_reply = true };
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 80,
		.mode1_dwell_ms = 150,
	};
	int waited = 0, asked_at_settle;

	printf(SUB_2 "testing TRS card-scan silence is not a refusal\n");

	g_trs.status_seen = false;
	g_trs.card_status = OSDP_TRS_CARD_STATUS_UNKNOWN;

	test_set_channel_hook(trs_wire_nak_hook, &s);
	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		test_set_channel_hook(NULL, NULL);
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}
	while (waited < 6000 && s.card_scan_tx_count < 2) {
		usleep(50 * 1000);
		waited += 50;
	}
	asked_at_settle = s.card_scan_tx_count;
	test_set_channel_hook(NULL, NULL);
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);

	if (asked_at_settle < 2) {
		printf(SUB_2 "stopped asking after silence (%d attempts)\n",
		       asked_at_settle);
		return false;
	}
	if (g_trs.status_seen &&
	    g_trs.last_status == OSDP_TRS_SCAN_NO_DEPARTURE) {
		printf(SUB_2 "silence was mistaken for a refusal\n");
		return false;
	}
	if (!wait_for_pd_mode(TRS_MODE_00, 5000)) {
		printf(SUB_2 "reader left in transparent mode after the fault\n");
		return false;
	}
	return true;
}

/* --- wire-level unit tests: assert exact on-wire bytes vs SIA OSDP 2.2 --- */

/*
 * A bare PD for codec tests. The codec logs through pd_to_osdp(), so it needs
 * an osdp context even when no device is set up; `mode` seeds the transparent
 * mode the PD believes it is in.
 */
static void trs_wire_pd_init(struct osdp *ctx, struct osdp_pd *pd, uint8_t mode)
{
	memset(ctx, 0, sizeof(*ctx));
	memset(pd, 0, sizeof(*pd));
	pd->osdp_ctx = ctx;
	pd->trs.mode = mode;
}

/* Table 71: Mode Setting Report must carry both Mode code and Mode config */
static bool test_trs_wire_mode_report(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	uint8_t buf[8];
	int len;

	printf(SUB_2 "testing TRS wire: mode-setting-report layout\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_01);
	pd.active_event = NULL; /* handshake reply, no app payload */

	len = osdp_trs_reply_build(&pd, buf, sizeof(buf));
	if (len != 4 || buf[0] != 0x00 || buf[1] != 0x01 ||
	    buf[2] != TRS_MODE_01 || buf[3] != 0x00) {
		printf(SUB_2 "want [00 01 mode 00] len 4; got len %d\n", len);
		return false;
	}
	return true;
}

/*
 * Table 44: the presence scan's own card scan is selected by the scan's
 * in-flight marker, not by the session sub-state -- so it must be emitted while
 * the marker is set, and must not shadow a lifecycle step when it is not.
 */
static bool test_trs_wire_probe_card_scan_build(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	uint8_t buf[8];
	int len;

	printf(SUB_2 "testing TRS wire: probe card-scan layout\n");

	trs_wire_pd_init(&ctx, &pd, TRS_MODE_01);
	pd.trs.state = TRS_STATE_XMIT;
	pd.trs.scan.asking = true;
	len = osdp_trs_cmd_build(&pd, NULL, buf, sizeof(buf));
	if (len != 3 || buf[0] != 0x01 || buf[1] != 0x04 || buf[2] != 0x00) {
		printf(SUB_2 "want [01 04 00] len 3; got len %d\n", len);
		return false;
	}

	/* Not asking: a lifecycle step must still win */
	pd.trs.scan.asking = false;
	pd.trs.state = TRS_STATE_TEARDOWN;
	len = osdp_trs_cmd_build(&pd, NULL, buf, sizeof(buf));
	if (len != 4 || buf[0] != 0x00 || buf[1] != 0x02 ||
	    buf[2] != TRS_MODE_00) {
		printf(SUB_2 "card-scan marker shadowed the mode-set\n");
		return false;
	}
	return true;
}

/*
 * The no-smart-card warning is raised once per enable, from wherever the
 * capability first becomes known -- not once per capability report, which a
 * reconnecting PD sends afresh every time.
 */
static bool test_trs_wire_capability_warning_is_once(void)
{
	struct osdp ctx;
	struct osdp_pd pd;

	printf(SUB_2 "testing TRS wire: capability warning fires once\n");

	trs_wire_pd_init(&ctx, &pd, TRS_MODE_00);
	CLEAR_FLAG(&pd, PD_FLAG_TRS_CAPABLE);
	pd.trs.scan.enabled = true;
	pd.trs.scan.warned = false;

	osdp_trs_scan_note_capability(&pd);
	if (!pd.trs.scan.warned) {
		printf(SUB_2 "no warning for a PD with no smart-card reader\n");
		return false;
	}
	pd.trs.scan.warned = false;
	SET_FLAG(&pd, PD_FLAG_TRS_CAPABLE);
	osdp_trs_scan_note_capability(&pd);
	if (pd.trs.scan.warned) {
		printf(SUB_2 "warned about a PD that has a reader\n");
		return false;
	}
	return true;
}

/* Table 75: Card Present reply must carry both Reader and Status */
static bool test_trs_wire_card_present_build(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	struct osdp_event ev;
	uint8_t buf[8];
	int len;

	printf(SUB_2 "testing TRS wire: card-present layout\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_01);
	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_TRS;
	ev.trs.reply = OSDP_TRS_REPLY_CARD_PRESENT;
	ev.trs.card_present.reader = 0;
	ev.trs.card_present.status = OSDP_TRS_CARD_PRESENT_CONTACT;
	pd.active_event = &ev;

	len = osdp_trs_reply_build(&pd, buf, sizeof(buf));
	if (len != 4 || buf[0] != 0x01 || buf[1] != 0x01 || buf[2] != 0x00 ||
	    buf[3] != 0x03) {
		printf(SUB_2 "card-present want len 4; got %d\n", len);
		return false;
	}
	return true;
}

/*
 * The Card Present status byte is optional per v2.2 section 7.26.8; readers
 * like the HID RPK40 send the reader byte alone. Take the reply at face
 * value -- a card is there -- and report the status as unspecified.
 */
static bool test_trs_wire_card_present_decode(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	struct osdp_event ev;
	uint8_t reader_only[] = { 0x01, 0x01, 0x00 };
	uint8_t with_status[] = { 0x01, 0x01, 0x00, 0x03 };
	int r;

	printf(SUB_2 "testing TRS wire: card-present decode\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_01);

	r = osdp_trs_reply_decode(&pd, reader_only, sizeof(reader_only), &ev);
	if (r < 0 || ev.trs.reply != OSDP_TRS_REPLY_CARD_PRESENT ||
	    ev.trs.card_present.reader != 0 ||
	    ev.trs.card_present.status != OSDP_TRS_CARD_PRESENT) {
		printf(SUB_2 "status-less card-present must decode as present\n");
		return false;
	}

	r = osdp_trs_reply_decode(&pd, with_status, sizeof(with_status), &ev);
	if (r < 0 ||
	    ev.trs.card_present.status != OSDP_TRS_CARD_PRESENT_CONTACT) {
		printf(SUB_2 "card-present status byte must still be honoured\n");
		return false;
	}
	return true;
}

/* Table 74: Mode-1 Error reply (osdp_PR01ERROR) = [01 00 error-code] */
static bool test_trs_wire_error_build(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	struct osdp_event ev;
	uint8_t buf[8];
	int len;

	printf(SUB_2 "testing TRS wire: error-reply layout\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_01);
	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_TRS;
	ev.trs.reply = OSDP_TRS_REPLY_ERROR;
	ev.trs.error.code = 0x6A;
	pd.active_event = &ev;

	len = osdp_trs_reply_build(&pd, buf, sizeof(buf));
	if (len != 3 || buf[0] != 0x01 || buf[1] != 0x00 || buf[2] != 0x6A) {
		printf(SUB_2 "want [01 00 6A] len 3; got len %d\n", len);
		return false;
	}
	return true;
}

/*
 * Table 36/39/42: Mode-Set and Terminate are ACK'd; Mode-Read (Table 39) is
 * answered with an XRD mode report; card-session commands go to the app.
 */
static bool test_trs_wire_cmd_decode_actions(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	struct osdp_cmd cmd;
	uint8_t set_mode[] = { 0x00, 0x02, TRS_MODE_01, 0x00 };
	uint8_t set_mode_no_cfg[] = { 0x00, 0x02, TRS_MODE_01 };
	uint8_t read_mode[] = { 0x00, 0x01 };
	uint8_t terminate[] = { 0x01, 0x02, 0x00 };
	uint8_t send_apdu[] = { 0x01, 0x01, 0x00, 0x00, 0xA4 };
	int r;

	printf(SUB_2 "testing TRS wire: command decode reply-actions\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_00);

	r = osdp_trs_cmd_decode(&pd, &cmd, set_mode, sizeof(set_mode));
	if (r != OSDP_TRS_DECODE_ACK) {
		printf(SUB_2 "mode-set must be ACK'd; got %d\n", r);
		return false;
	}
	/* Table 36: the config byte is optional; in its absence 0x00 is used */
	r = osdp_trs_cmd_decode(&pd, &cmd, set_mode_no_cfg,
				sizeof(set_mode_no_cfg));
	if (r != OSDP_TRS_DECODE_ACK || pd.trs.mode != TRS_MODE_01) {
		printf(SUB_2 "config-less mode-set must be ACK'd; got %d\n", r);
		return false;
	}
	r = osdp_trs_cmd_decode(&pd, &cmd, read_mode, sizeof(read_mode));
	if (r != OSDP_TRS_DECODE_MODE_REPORT) {
		printf(SUB_2 "mode-read want mode report; got %d\n", r);
		return false;
	}
	r = osdp_trs_cmd_decode(&pd, &cmd, terminate, sizeof(terminate));
	if (r != OSDP_TRS_DECODE_ACK) {
		printf(SUB_2 "terminate must be ACK'd; got %d\n", r);
		return false;
	}
	r = osdp_trs_cmd_decode(&pd, &cmd, send_apdu, sizeof(send_apdu));
	if (r != OSDP_TRS_DECODE_TO_APP) {
		printf(SUB_2 "send-apdu must be delivered to app; got %d\n", r);
		return false;
	}
	return true;
}

/*
 * The PD must refuse a mode it cannot honour, and must not act on card-session
 * commands until the CP has actually put it in transparent mode.
 */
static bool test_trs_wire_mode_gating(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	struct osdp_cmd cmd;
	uint8_t bad_mode[] = { 0x00, 0x02, 0x05, 0x00 };
	uint8_t send_apdu[] = { 0x01, 0x01, 0x00, 0x00, 0xA4 };
	uint8_t set_mode[] = { 0x00, 0x02, TRS_MODE_01, 0x00 };
	int r;

	printf(SUB_2 "testing TRS wire: transparent-mode gating\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_00);

	/* An unsupported mode must be NAK'd, not stored and ACK'd */
	r = osdp_trs_cmd_decode(&pd, &cmd, bad_mode, sizeof(bad_mode));
	if (r >= 0 || pd.trs.mode != TRS_MODE_00) {
		printf(SUB_2 "mode 0x05 must be refused; got %d mode %02x\n", r,
		       pd.trs.mode);
		return false;
	}

	/* Card-session commands are invalid while transparent mode is off */
	r = osdp_trs_cmd_decode(&pd, &cmd, send_apdu, sizeof(send_apdu));
	if (r >= 0) {
		printf(SUB_2 "send-apdu in mode 00 must be refused; got %d\n", r);
		return false;
	}

	/* ... and valid once the CP negotiates mode 01 */
	r = osdp_trs_cmd_decode(&pd, &cmd, set_mode, sizeof(set_mode));
	if (r != OSDP_TRS_DECODE_ACK || pd.trs.mode != TRS_MODE_01) {
		printf(SUB_2 "mode-set 01 must be ACK'd; got %d\n", r);
		return false;
	}
	r = osdp_trs_cmd_decode(&pd, &cmd, send_apdu, sizeof(send_apdu));
	if (r != OSDP_TRS_DECODE_TO_APP) {
		printf(SUB_2 "send-apdu in mode 01 must reach app; got %d\n", r);
		return false;
	}
	return true;
}

/* Oversized APDUs must be rejected on decode, never silently truncated */
static bool test_trs_wire_oversized_apdu_reject(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	struct osdp_cmd cmd;
	struct osdp_event ev;
	uint8_t buf[OSDP_TRS_APDU_MAX_LEN + 16];
	int r;

	printf(SUB_2 "testing TRS wire: oversized APDU rejection\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_01);
	memset(buf, 0xA5, sizeof(buf));

	/* CP side: REPLY_XRD card-data carrying an R-APDU 6 bytes too long */
	buf[0] = 0x01; /* mode 1 */
	buf[1] = 0x02; /* card data */
	buf[2] = 0x00; /* reader */
	buf[3] = 0x00; /* status */
	r = osdp_trs_reply_decode(&pd, buf, 4 + OSDP_TRS_APDU_MAX_LEN + 6, &ev);
	if (r >= 0) {
		printf(SUB_2 "oversized R-APDU must be rejected; got %d\n", r);
		return false;
	}

	/* PD side: CMD_XWR send-APDU carrying a C-APDU 6 bytes too long */
	buf[1] = 0x01; /* send-apdu */
	r = osdp_trs_cmd_decode(&pd, &cmd, buf, 3 + OSDP_TRS_APDU_MAX_LEN + 6);
	if (r >= 0) {
		printf(SUB_2 "oversized C-APDU must be NAK'd; got %d\n", r);
		return false;
	}

	/* ... while an exactly max-size C-APDU still goes through */
	r = osdp_trs_cmd_decode(&pd, &cmd, buf, 3 + OSDP_TRS_APDU_MAX_LEN);
	if (r != OSDP_TRS_DECODE_TO_APP ||
	    cmd.trs.apdu.length != OSDP_TRS_APDU_MAX_LEN) {
		printf(SUB_2 "max-size C-APDU must be accepted; got %d\n", r);
		return false;
	}
	return true;
}

/*
 * PIV certificate reads move R-APDU chunks up to a short-APDU maximum of
 * 258 bytes (255 data + SW1SW2 + margin); the carrier must hold them, and
 * a chunk-sized APDU must survive the CP-build -> PD-decode round trip.
 */
static bool test_trs_wire_piv_class_apdu_roundtrip(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	struct osdp_cmd in, out;
	uint8_t buf[512];
	int len, i, r;
	const int apdu_len = 250;

	printf(SUB_2 "testing TRS wire: PIV-class APDU round-trip\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_01);

	if (OSDP_TRS_APDU_MAX_LEN < 258) {
		printf(SUB_2 "APDU carrier too small for PIV chunks: %d\n",
		       OSDP_TRS_APDU_MAX_LEN);
		return false;
	}

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	in.id = OSDP_CMD_XWR;
	in.trs.command = OSDP_TRS_CMD_SEND_APDU;
	in.trs.apdu.length = apdu_len;
	for (i = 0; i < apdu_len; i++) {
		in.trs.apdu.data[i] = (uint8_t)i;
	}

	len = osdp_trs_cmd_build(&pd, &in, buf, sizeof(buf));
	if (len != 3 + apdu_len) {
		printf(SUB_2 "large APDU build: want len %d; got %d\n",
		       3 + apdu_len, len);
		return false;
	}
	r = osdp_trs_cmd_decode(&pd, &out, buf, len);
	if (r != OSDP_TRS_DECODE_TO_APP ||
	    out.trs.apdu.length != apdu_len ||
	    memcmp(out.trs.apdu.data, in.trs.apdu.data, apdu_len) != 0) {
		printf(SUB_2 "large APDU did not round-trip intact: %d\n", r);
		return false;
	}
	return true;
}

/* Table 43: PIN-entry packed bytes must survive a CP-build -> PD-decode trip */
static bool test_trs_wire_pin_entry_roundtrip(void)
{
	struct osdp ctx;
	struct osdp_pd pd;
	struct osdp_cmd in, out;
	struct osdp_trs_pin_entry *pe = &in.trs.pin_entry;
	uint8_t buf[64];
	int len, r;

	printf(SUB_2 "testing TRS wire: PIN-entry round-trip\n");
	trs_wire_pd_init(&ctx, &pd, TRS_MODE_01);
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	in.id = OSDP_CMD_XWR;
	in.trs.command = OSDP_TRS_CMD_ENTER_PIN;
	pe->timeout_initial = 30;
	pe->timeout_digit = 10;
	pe->pin_block.format = OSDP_TRS_PIN_FORMAT_ASCII;
	pe->pin_block.right_justify = true;
	pe->pin_block.offset_bits = 40; /* byte-aligned: goes out in byte units */
	pe->pin_block.size_bytes = 8;
	pe->pin_length_field.size_bits = 4;
	pe->pin_length_field.offset_bits = 3; /* sub-byte: goes out in bit units */
	pe->min_digits = 4;
	pe->max_digits = 12;
	pe->complete_on = OSDP_TRS_PIN_COMPLETE_ON_MAX_DIGITS |
			  OSDP_TRS_PIN_COMPLETE_ON_KEY;
	pe->num_messages = 1;
	pe->language_id = 0x0409;
	pe->msg_index = 0;
	pe->apdu.length = sizeof(g_req_apdu);
	memcpy(pe->apdu.data, g_req_apdu, sizeof(g_req_apdu));

	/* header(2) + reader(1) + fixed fields(15) + apdu len(2) + apdu(5) */
	len = osdp_trs_cmd_build(&pd, &in, buf, sizeof(buf));
	if (len != 25) {
		printf(SUB_2 "PIN-entry build: want len 25; got %d\n", len);
		return false;
	}
	r = osdp_trs_cmd_decode(&pd, &out, buf, len);
	if (r != OSDP_TRS_DECODE_TO_APP ||
	    out.trs.command != OSDP_TRS_CMD_ENTER_PIN) {
		printf(SUB_2 "PIN-entry decode failed: %d\n", r);
		return false;
	}
	if (memcmp(pe, &out.trs.pin_entry, sizeof(*pe)) != 0) {
		printf(SUB_2 "PIN-entry did not round-trip intact\n");
		return false;
	}
	return true;
}

/* End-to-end: PD reports a card-present status; CP app must receive it */
static bool test_trs_card_present()
{
	printf(SUB_2 "testing TRS card-present notification\n");

	g_trs.event_seen = false;
	memset(&g_trs.resp_event, 0, sizeof(g_trs.resp_event));
	g_trs.resp_event.type = OSDP_EVENT_TRS;
	g_trs.resp_event.trs.reply = OSDP_TRS_REPLY_CARD_PRESENT;
	g_trs.resp_event.trs.card_present.reader = 0;
	g_trs.resp_event.trs.card_present.status = OSDP_TRS_CARD_PRESENT_CONTACT;

	if (!test_submit_event(g_trs.pd_ctx, &g_trs.resp_event)) {
		printf(SUB_2 "Failed to submit card-present event\n");
		return false;
	}
	if (!wait_for_trs_event(10)) {
		printf(SUB_2 "card-present event not received\n");
		return false;
	}
	if (g_trs.last_reply.reply != OSDP_TRS_REPLY_CARD_PRESENT ||
	    g_trs.last_reply.card_present.reader != 0 ||
	    g_trs.last_reply.card_present.status !=
		    OSDP_TRS_CARD_PRESENT_CONTACT) {
		printf(SUB_2 "card-present reader/status not intact\n");
		return false;
	}
	return true;
}

/* End-to-end: PD reports a transparent-mode error; CP app must receive it */
static bool test_trs_error_reply()
{
	printf(SUB_2 "testing TRS error reply\n");

	g_trs.event_seen = false;
	trs_fill_error(&g_trs);

	if (!test_submit_event(g_trs.pd_ctx, &g_trs.resp_event)) {
		printf(SUB_2 "Failed to submit error event\n");
		return false;
	}
	if (!wait_for_trs_event(10)) {
		printf(SUB_2 "error event not received\n");
		return false;
	}
	if (g_trs.last_reply.reply != OSDP_TRS_REPLY_ERROR ||
	    g_trs.last_reply.error.code != 0x6A) {
		printf(SUB_2 "error code not intact\n");
		return false;
	}
	return true;
}

/* Event submitted by reference; must outlive the callback that ships it */
static struct osdp_event g_unsolicited_ev;

/*
 * With no session open, a reader may still announce a card in its field as an
 * XRD poll response (v2.2 sections 5.11, 7.26.8); the CP must forward that
 * sighting to the app. Any other out-of-session XRD (a reader left in
 * transparent mode by a failed teardown or a CP restart) must be shrugged
 * off -- not surfaced, and never a reason to cycle the PD offline.
 */
static bool test_trs_unsolicited_xrd_out_of_session(void)
{
	uint8_t status = 0;
	int rc = 0;

	printf(SUB_2 "testing unsolicited XRD outside a session\n");

	g_trs.event_seen = false;

	g_unsolicited_ev = (struct osdp_event){
		.type = OSDP_EVENT_TRS,
		.trs = {
			.reply = OSDP_TRS_REPLY_CARD_PRESENT,
			.card_present = {
				.reader = 0,
				.status = OSDP_TRS_CARD_PRESENT,
			},
		},
	};
	if (!test_submit_event(g_trs.pd_ctx, &g_unsolicited_ev)) {
		printf(SUB_2 "failed to submit unsolicited TRS event\n");
		return false;
	}

	if (!wait_for_trs_event(10)) {
		printf(SUB_2 "out-of-session card sighting must surface\n");
		return false;
	}
	if (g_trs.last_reply.reply != OSDP_TRS_REPLY_CARD_PRESENT ||
	    g_trs.last_reply.card_present.status != OSDP_TRS_CARD_PRESENT) {
		printf(SUB_2 "card sighting not intact\n");
		return false;
	}

	/* Card data with no session to route it to stays ignored */
	g_trs.event_seen = false;
	g_unsolicited_ev = (struct osdp_event){
		.type = OSDP_EVENT_TRS,
		.trs = {
			.reply = OSDP_TRS_REPLY_CARD_DATA,
			.card_data = {
				.reader = 0,
				.status = 0,
				.apdu = { .length = 2, .data = { 0x90, 0x00 } },
			},
		},
	};
	if (!test_submit_event(g_trs.pd_ctx, &g_unsolicited_ev)) {
		printf(SUB_2 "failed to submit unsolicited card data\n");
		return false;
	}

	/* Long enough for the poll loop to carry it across and, were the CP
	 * still intolerant, for the link to collapse. */
	while (rc++ < 30) {
		usleep(100 * 1000);
	}

	osdp_get_status_mask(g_trs.cp_ctx, &status);
	if (!(status & 1)) {
		printf(SUB_2 "a stray XRD must not take the PD offline\n");
		return false;
	}
	if (g_trs.event_seen) {
		printf(SUB_2 "stray card data must not surface as an event\n");
		return false;
	}
	return true;
}

/* --- presence scan (osdp_cp_trs_scan_enable) --- */

/*
 * With the scan on and nothing else going on, the reader must keep flipping
 * default mode -> transparent -> default, silently: probes are not sessions
 * (no OPENED/CLOSED notifications) and not app commands (no completions).
 */
static bool test_trs_scan_cadence(void)
{
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 80,
		.mode1_dwell_ms = 150,
	};
	uint8_t status = 0;
	int flips;

	printf(SUB_2 "testing TRS presence-scan cadence\n");

	g_trs.status_seen = false;
	test_completion_reset(&g_trs.completion);

	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}
	for (flips = 0; flips < 3; flips++) {
		if (!wait_for_pd_mode(TRS_MODE_01, 3000)) {
			printf(SUB_2 "probe %d never reached the reader\n",
			       flips);
			return false;
		}
		if (!wait_for_pd_mode(TRS_MODE_00, 3000)) {
			printf(SUB_2 "probe %d never wound down\n", flips);
			return false;
		}
	}
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);

	if (g_trs.status_seen) {
		printf(SUB_2 "probes must not raise session notifications\n");
		return false;
	}
	if (test_completion_count(&g_trs.completion)) {
		printf(SUB_2 "probes must not complete app commands\n");
		return false;
	}
	osdp_get_status_mask(g_trs.cp_ctx, &status);
	if (!(status & 1)) {
		printf(SUB_2 "scanning must not cost the link\n");
		return false;
	}
	return true;
}

/* Event submitted by reference; must outlive the callback that ships it */
/*
 * A reader that answers the card scan is asked, on a cadence, and the answer
 * is the app's first news of a card -- with no session, no APDU and no command
 * completion of its own, because nobody submitted it.
 */
static bool test_trs_scan_probe_asks_card_scan(void)
{
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 80,
		.mode1_dwell_ms = 150,
	};
	int waited = 0;

	printf(SUB_2 "testing TRS scan asks the reader for a card scan\n");

	g_trs.card_scan_count = 0;
	g_trs.present_events = 0;
	g_trs.absent_events = 0;
	test_completion_reset(&g_trs.completion);
	g_trs.status_seen = false;
	g_trs.card_status = OSDP_TRS_CARD_PRESENT_CONTACTLESS;

	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}
	while (waited < 4000 && g_trs.present_events < 1) {
		usleep(50 * 1000);
		waited += 50;
	}
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
	g_trs.card_status = OSDP_TRS_CARD_STATUS_UNKNOWN;

	if (g_trs.card_scan_count < 1) {
		printf(SUB_2 "the probe never asked\n");
		return false;
	}
	if (g_trs.present_events < 1) {
		printf(SUB_2 "sighting never reached the app\n");
		return false;
	}
	/* One question per probe cycle, not one per refresh tick */
	if (g_trs.card_scan_count > waited / 100) {
		printf(SUB_2 "asked %d times in %d ms; too chatty\n",
		       g_trs.card_scan_count, waited);
		return false;
	}
	if (test_completion_count(&g_trs.completion)) {
		printf(SUB_2 "library's own card scan reported a completion\n");
		return false;
	}
	if (g_trs.status_seen) {
		printf(SUB_2 "probe raised a session notification (%d)\n",
		       g_trs.last_status);
		return false;
	}
	return true;
}

/*
 * The point of asking: a card lifted out of the field is reported gone, with
 * the app having sent nothing. Presence is reported as it changes, so a card
 * left sitting there is announced once, not once per probe.
 */
static bool test_trs_scan_card_departure_event(void)
{
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 80,
		.mode1_dwell_ms = 150,
	};
	int waited = 0, present_after_settle;

	printf(SUB_2 "testing TRS scan reports card departure\n");

	g_trs.present_events = 0;
	g_trs.absent_events = 0;
	g_trs.card_status = OSDP_TRS_CARD_PRESENT_CONTACTLESS;

	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}
	while (waited < 4000 && g_trs.present_events < 1) {
		usleep(50 * 1000);
		waited += 50;
	}
	if (g_trs.present_events < 1) {
		osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
		printf(SUB_2 "card never sighted\n");
		return false;
	}

	/* The card stays put across several probes: still one sighting */
	usleep(1500 * 1000);
	present_after_settle = g_trs.present_events;
	if (present_after_settle != 1) {
		osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
		printf(SUB_2 "a parked card was announced %d times\n",
		       present_after_settle);
		return false;
	}

	/* Now it is lifted away */
	g_trs.card_status = OSDP_TRS_CARD_NOT_PRESENT;
	waited = 0;
	while (waited < 4000 && g_trs.absent_events < 1) {
		usleep(50 * 1000);
		waited += 50;
	}
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
	g_trs.card_status = OSDP_TRS_CARD_STATUS_UNKNOWN;

	if (g_trs.absent_events < 1) {
		printf(SUB_2 "card removal was never reported\n");
		return false;
	}
	if (!wait_for_pd_mode(TRS_MODE_00, 3000)) {
		printf(SUB_2 "reader left in transparent mode\n");
		return false;
	}
	return true;
}

static struct osdp_event g_activity_ev;

/*
 * Ordinary keypad/card traffic restarts the default-mode dwell: as long as
 * the user is interacting with the reader, no probe may cut in.
 */
static bool test_trs_scan_mode0_activity_defers_probe(void)
{
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 500,
		.mode1_dwell_ms = 100,
	};
	int i;

	printf(SUB_2 "testing TRS scan defers to mode-0 activity\n");

	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}
	for (i = 0; i < 15; i++) {
		g_activity_ev = (struct osdp_event){
			.type = OSDP_EVENT_KEYPRESS,
			.keypress = {
				.reader_no = 0,
				.length = 1,
				.data = { 0x30 },
			},
		};
		if (!test_submit_event(g_trs.pd_ctx, &g_activity_ev)) {
			printf(SUB_2 "failed to submit keypress %d\n", i);
			return false;
		}
		if (pd_trs_mode() != TRS_MODE_00) {
			printf(SUB_2 "probe sliced into keypad activity\n");
			return false;
		}
		usleep(100 * 1000);
	}
	/* activity stops; the next dwell must end in a probe */
	if (!wait_for_pd_mode(TRS_MODE_01, 3000)) {
		printf(SUB_2 "scan never resumed after activity\n");
		return false;
	}
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
	if (!wait_for_pd_mode(TRS_MODE_00, 3000)) {
		printf(SUB_2 "reader stuck in mode 01 after disable\n");
		return false;
	}
	return true;
}

/*
 * Disabling the scan mid-probe must restore the reader's default mode, and
 * flushing the (empty) queue mid-probe must leave both the scan and a
 * subsequent band working.
 */
static bool test_trs_scan_disable_and_flush_mid_probe(void)
{
	struct osdp_trs_scan_params p = {
		.mode0_dwell_ms = 80,
		.mode1_dwell_ms = 3000, /* long probe: room to interrupt it */
	};
	struct osdp_cmd start, stop;

	printf(SUB_2 "testing TRS scan disable/flush mid-probe\n");

	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		printf(SUB_2 "failed to enable scan\n");
		return false;
	}
	if (!wait_for_pd_mode(TRS_MODE_01, 3000)) {
		printf(SUB_2 "no probe to disable\n");
		return false;
	}
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
	if (!wait_for_pd_mode(TRS_MODE_00, 2000)) {
		printf(SUB_2 "disable must restore the default mode\n");
		return false;
	}
	usleep(500 * 1000);
	if (pd_trs_mode() != TRS_MODE_00) {
		printf(SUB_2 "a disabled scan kept probing\n");
		return false;
	}

	if (osdp_cp_trs_scan_enable(g_trs.cp_ctx, 0, &p)) {
		printf(SUB_2 "failed to re-enable scan\n");
		return false;
	}
	if (!wait_for_pd_mode(TRS_MODE_01, 3000)) {
		printf(SUB_2 "scan did not resume\n");
		return false;
	}
	osdp_cp_flush_commands(g_trs.cp_ctx, 0);

	/* A band right through the probe: adoption, exchange, orderly close */
	g_trs.status_seen = false;
	if (submit_trs_cmd(&start, OSDP_TRS_CMD_START)) {
		printf(SUB_2 "failed to submit START\n");
		return false;
	}
	if (!wait_for_trs_status(OSDP_TRS_SESSION_OPENED, 10)) {
		printf(SUB_2 "band did not open after flush\n");
		return false;
	}
	if (!test_trs_apdu_exchange()) {
		printf(SUB_2 "band unusable after flush\n");
		return false;
	}
	g_trs.status_seen = false;
	if (submit_trs_cmd(&stop, OSDP_TRS_CMD_STOP)) {
		printf(SUB_2 "failed to submit STOP\n");
		return false;
	}
	if (!wait_for_trs_status(OSDP_TRS_SESSION_CLOSED, 10)) {
		printf(SUB_2 "band did not close\n");
		return false;
	}
	osdp_cp_trs_scan_disable(g_trs.cp_ctx, 0);
	if (!wait_for_pd_mode(TRS_MODE_00, 3000)) {
		printf(SUB_2 "reader stuck in mode 01 after band close\n");
		return false;
	}
	return true;
}

static bool test_trs_session_stop()
{
	printf(SUB_2 "testing TRS session stop\n");

	struct osdp_cmd cmd;

	g_trs.status_seen = false;

	if (submit_trs_cmd(&cmd, OSDP_TRS_CMD_STOP)) {
		printf(SUB_2 "Failed to submit TRS stop\n");
		return false;
	}
	if (!wait_for_trs_status(OSDP_TRS_SESSION_CLOSED, 10)) {
		printf(SUB_2 "TRS session-closed notification not received\n");
		return false;
	}

	/* After teardown the PD is back to ordinary online operation */
	if (!test_wait_for_online(g_trs.cp_ctx, 0, 10)) {
		printf(SUB_2 "PD did not return online after TRS stop\n");
		return false;
	}

	return true;
}

/* A send-APDU with a real payload, built fresh for each of the two in-flight
 * commands the failure test needs (the queue node lives inside the struct,
 * so the two cannot share one instance). */
static struct osdp_cmd trs_band_flush_apdu_cmd(void)
{
	struct osdp_cmd cmd = {
		.id = OSDP_CMD_XWR,
		.trs = {
			.command = OSDP_TRS_CMD_SEND_APDU,
			.apdu = { .length = sizeof(g_req_apdu) },
		},
	};

	memcpy(cmd.trs.apdu.data, g_req_apdu, sizeof(g_req_apdu));
	return cmd;
}

static struct osdp_cmd trs_band_flush_led_cmd(void)
{
	struct osdp_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.id = OSDP_CMD_LED;
	cmd.led.reader = 0;
	cmd.led.led_number = 0;
	cmd.led.temporary.control_code = OSDP_CMD_LED_TEMPORARY_CC_SET;
	cmd.led.temporary.on_count = 2;
	cmd.led.temporary.off_count = 2;
	cmd.led.temporary.on_color = OSDP_LED_COLOR_RED;
	cmd.led.temporary.off_color = OSDP_LED_COLOR_NONE;
	cmd.led.temporary.timer_count = 2;
	cmd.led.permanent.control_code = OSDP_CMD_LED_PERMANENT_CC_NOP;
	return cmd;
}

static struct osdp_cmd trs_band_flush_buzzer_cmd(void)
{
	struct osdp_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.id = OSDP_CMD_BUZZER;
	cmd.buzzer.reader = 0;
	cmd.buzzer.control_code = OSDP_CMD_BUZZER_CC_DEFAULT_TONE;
	cmd.buzzer.on_count = 2;
	cmd.buzzer.off_count = 2;
	cmd.buzzer.rep_count = 1;
	return cmd;
}

/* What test_trs_band_flush_on_session_failure() below needs from a
 * completion: the order they arrived in (the ordering guarantee is the
 * point of the test) and a one-shot hook to resubmit from inside a FLUSHED
 * completion, the way test-completion.c's resubmit_completion_cb does for
 * the whole-queue flush. */
struct trs_band_flush_log {
	atomic_int count;
	int id[8];
	int status[8];
	atomic_bool resubmit_armed;
	atomic_int resubmit_rc;
};

static void trs_band_flush_completion_cb(void *arg, int pd,
					 struct osdp_cmd *cmd,
					 enum osdp_completion_status status)
{
	struct trs_band_flush_log *log = arg;
	int idx = atomic_fetch_add(&log->count, 1);

	ARG_UNUSED(pd);
	if (idx < (int)(sizeof(log->id) / sizeof(log->id[0]))) {
		log->id[idx] = cmd->id;
		log->status[idx] = (int)status;
	}
	/*
	 * Resubmit from inside the first FLUSHED completion. The new command
	 * must outlive this callback's stack frame, so it goes in through the
	 * pool (test_submit_command()) rather than a local passed straight to
	 * osdp_cp_submit_command() -- the queue keeps it by reference.
	 */
	if (status == OSDP_COMPLETION_FLUSHED &&
	    atomic_exchange(&log->resubmit_armed, false)) {
		struct osdp_cmd next = trs_band_flush_buzzer_cmd();

		atomic_store(&log->resubmit_rc,
			    test_submit_command(g_trs.cp_ctx, 0, &next) ?
				    1 : 0);
	}
	test_cmd_free(cmd);
}

static bool wait_for_log_count(struct trs_band_flush_log *log, int want,
			       int timeout_sec)
{
	int rc = 0;

	while (rc++ < timeout_sec) {
		if (atomic_load(&log->count) >= want) {
			return true;
		}
		usleep(1000 * 1000);
	}
	return false;
}

/*
 * A session that dies of a genuine link fault -- not an orderly STOP -- must
 * still flush its band cleanly. Silencing every reply once the app's first
 * send-APDU is in flight drives osdp_trs_state_update_err() through both its
 * retry stages (XMIT -> TEARDOWN -> DONE) without the STOP ever dequeuing the
 * ordinary way, so cp_flush_trs_band() is what closes the session out: the
 * second APDU and the STOP still sitting behind the failed one must complete
 * FLUSHED, in order, and whatever the app queued after the STOP must survive
 * and run -- ahead of anything a FLUSHED completion submits while the flush
 * is still draining.
 */
static bool test_trs_band_flush_on_session_failure(void)
{
	struct trs_wire_nak_hook hook = { .drop_reply = true };
	struct trs_band_flush_log log = { 0 };
	struct osdp_cmd start_cmd, stop_cmd;
	struct osdp_cmd cmd_a = trs_band_flush_apdu_cmd();
	struct osdp_cmd cmd_b = trs_band_flush_apdu_cmd();
	struct osdp_cmd tail_cmd = trs_band_flush_led_cmd();
	bool ok = true;

	printf(SUB_2 "testing TRS band flush on session failure\n");

	g_trs.status_seen = false;
	if (submit_trs_cmd(&start_cmd, OSDP_TRS_CMD_START)) {
		printf(SUB_2 "failed to submit TRS start\n");
		return false;
	}
	if (!wait_for_trs_status(OSDP_TRS_SESSION_OPENED, 10)) {
		printf(SUB_2 "TRS session never opened\n");
		return false;
	}

	atomic_store(&log.resubmit_armed, true);
	atomic_store(&log.resubmit_rc, -1);
	trs_use_completion(trs_band_flush_completion_cb, &log);
	g_trs.status_seen = false;
	test_set_channel_hook(trs_wire_nak_hook, &hook);

	if (!test_submit_command(g_trs.cp_ctx, 0, &cmd_a) ||
	    !test_submit_command(g_trs.cp_ctx, 0, &cmd_b)) {
		printf(SUB_2 "failed to submit the send-APDUs\n");
		ok = false;
		goto out;
	}
	if (submit_trs_cmd(&stop_cmd, OSDP_TRS_CMD_STOP)) {
		printf(SUB_2 "failed to submit TRS stop\n");
		ok = false;
		goto out;
	}
	if (!test_submit_command(g_trs.cp_ctx, 0, &tail_cmd)) {
		printf(SUB_2 "failed to submit the post-band command\n");
		ok = false;
		goto out;
	}

	/* Two retry-exhaustion stages (XMIT, then TEARDOWN's own restore
	 * attempt), each costing close to OSDP_CMD_MAX_RETRIES *
	 * OSDP_CMD_RETRY_WAIT_MS -- give it generous headroom. */
	if (!wait_for_trs_status(OSDP_TRS_SESSION_FAILED, 40)) {
		printf(SUB_2 "session-failed notification not received\n");
		ok = false;
		goto out;
	}
	test_set_channel_hook(NULL, NULL);

	if (!wait_for_log_count(&log, 5, 10) ||
	    atomic_load(&log.count) != 5) {
		printf(SUB_2 "want 5 completions, got %d\n",
		       atomic_load(&log.count));
		ok = false;
		goto out;
	}
	if (log.id[0] != OSDP_CMD_XWR ||
	    log.status[0] != OSDP_COMPLETION_FAILED) {
		printf(SUB_2 "1st APDU: want XWR/FAILED, got %d/%d\n",
		       log.id[0], log.status[0]);
		ok = false;
	}
	if (log.id[1] != OSDP_CMD_XWR ||
	    log.status[1] != OSDP_COMPLETION_FLUSHED) {
		printf(SUB_2 "2nd APDU: want XWR/FLUSHED, got %d/%d\n",
		       log.id[1], log.status[1]);
		ok = false;
	}
	if (log.id[2] != OSDP_CMD_XWR ||
	    log.status[2] != OSDP_COMPLETION_FLUSHED) {
		printf(SUB_2 "STOP: want XWR/FLUSHED, got %d/%d\n",
		       log.id[2], log.status[2]);
		ok = false;
	}
	if (log.id[3] != OSDP_CMD_LED || log.status[3] != OSDP_COMPLETION_OK) {
		printf(SUB_2 "post-band tail: want LED/OK, got %d/%d\n",
		       log.id[3], log.status[3]);
		ok = false;
	}
	if (log.id[4] != OSDP_CMD_BUZZER ||
	    log.status[4] != OSDP_COMPLETION_OK) {
		printf(SUB_2 "resubmitted cmd: want BUZZER/OK, got %d/%d\n",
		       log.id[4], log.status[4]);
		ok = false;
	}
	if (atomic_load(&log.resubmit_rc) != 1) {
		printf(SUB_2 "resubmit from a FLUSHED completion failed\n");
		ok = false;
	}

	/* After the flush the PD is back to ordinary online operation */
	if (!test_wait_for_online(g_trs.cp_ctx, 0, 10)) {
		printf(SUB_2 "PD did not return online after the failure\n");
		ok = false;
	}

out:
	test_set_channel_hook(NULL, NULL);
	trs_use_completion(trs_cp_completion_callback, &g_trs);
	return ok;
}

void run_trs_tests(struct test *t)
{
	bool result = true;

	printf("\nBegin TRS Tests\n");

	/* wire-level unit tests need no devices; run them first */
	printf(SUB_1 "running TRS wire-encoding tests\n");
	result &= test_trs_wire_mode_report();
	result &= test_trs_wire_card_present_build();
	result &= test_trs_wire_card_present_decode();
	result &= test_trs_wire_error_build();
	result &= test_trs_wire_cmd_decode_actions();
	result &= test_trs_wire_mode_gating();
	result &= test_trs_wire_oversized_apdu_reject();
	result &= test_trs_wire_piv_class_apdu_roundtrip();
	result &= test_trs_wire_pin_entry_roundtrip();
	result &= test_trs_wire_probe_card_scan_build();
	result &= test_trs_wire_capability_warning_is_once();

	if (setup_test_environment(t) != 0) {
		printf(SUB_1 "Failed to setup test environment\n");
		TEST_REPORT(t, false);
		return;
	}

	printf(SUB_1 "running TRS tests\n");
	result &= test_trs_apdu_outside_band();
	result &= test_trs_session_start();
	result &= test_trs_non_trs_cmd_in_band();
	result &= test_trs_apdu_too_big_for_packet();
	result &= test_trs_max_apdu_len_accessor();
	result &= test_trs_pin_entry_too_big_for_packet();
	result &= test_trs_pin_entry_layout_unrepresentable();
	result &= test_trs_apdu_exchange();
	result &= test_trs_deferred_apdu();
	result &= test_trs_apdu_nak_keeps_session();
	result &= test_trs_apdu_error_reply_fails_cmd();
	result &= test_trs_card_present();
	result &= test_trs_error_reply();
	result &= test_trs_session_stop();
	result &= test_trs_unsolicited_xrd_out_of_session();
	result &= test_trs_scan_cadence();
	result &= test_trs_scan_probe_asks_card_scan();
	result &= test_trs_scan_card_departure_event();
	result &= test_trs_scan_mode0_activity_defers_probe();
	result &= test_trs_scan_disable_and_flush_mid_probe();

	teardown_test_environment();

	/*
	 * Reader-quirk tests run on a plaintext link, matching the field
	 * traces they are modeled on: rewriting a reply on the wire would
	 * desynchronize the secure channel's MAC chain, which a real quirky
	 * reader never does.
	 */
	if (setup_test_environment_ex(t, true) != 0) {
		printf(SUB_1 "Failed to setup plaintext test environment\n");
		TEST_REPORT(t, false);
		return;
	}
	result &= test_trs_session_start();
	result &= test_trs_card_scan_wire_nak();
	result &= test_trs_session_stop();
	result &= test_trs_scan_hold_and_adopt();
	/*
	 * Order matters: the refusal latches "no card scan" for the rest of the
	 * connection, so the silence case -- which must leave the verdict open
	 * -- has to run while there is still a verdict to reach.
	 */
	result &= test_trs_scan_card_scan_timeout_is_not_unsupported();
	result &= test_trs_scan_card_scan_unsupported_latches();
	result &= test_trs_scan_probe_failure_backoff();
	/*
	 * Last on this link: two full retry-exhaustion stages leave the CP
	 * having just burned a long backlog of timeouts, which is not a
	 * clean starting point for a test tuned to a short scan cadence.
	 */
	result &= test_trs_band_flush_on_session_failure();

	teardown_test_environment();

	printf(SUB_1 "TRS tests %s\n", result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

#else /* OPT_BUILD_OSDP_TRS */

/* TRS-only suite: compiles to a no-op when TRS support is not built in, so the
 * file can sit in the shared source list without a link dependency. */
void run_trs_tests(struct test *t)
{
	ARG_UNUSED(t);
}

#endif /* OPT_BUILD_OSDP_TRS */
