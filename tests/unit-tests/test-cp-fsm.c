/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>

#include <osdp.h>
#include "test.h"

extern int test_state_update(struct osdp_pd *);

int test_fsm_resp = 0;
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

void run_cp_fsm_tests(struct test *t)
{
	int result = true;
	uint32_t count = 0;
	struct osdp *ctx;

	printf("\nStarting CP Phy state tests\n");

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

	test_cp_fsm_teardown(t);
}

// unnecessary
