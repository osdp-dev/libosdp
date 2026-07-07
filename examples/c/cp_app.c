/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <osdp.h>
#include <stdint.h>

/**
 * This method overrides the one provided by libosdp. It should return
 * a millisecond reference point from some tick source.
 */
int64_t osdp_millis_now()
{
	return 0;
}

int sample_cp_send_func(void *data, uint8_t *buf, int len)
{
	(void)(data);
	(void)(buf);

	// TODO (user): send buf of len bytes, over the UART channel.

	return len;
}

int sample_cp_recv_func(void *data, uint8_t *buf, int len)
{
	(void)(data);
	(void)(buf);
	(void)(len);

	// TODO (user): read from UART channel into buf, for upto len bytes.

	return 0;
}

osdp_pd_info_t pd_info[] = {
	{
		.address = 101,
		.baud_rate = 115200,
		.flags = 0,
		.scbk = NULL,
	},
};

static struct osdp_channel cp_channel = {
	.data = NULL,
	.send = sample_cp_send_func,
	.recv = sample_cp_recv_func,
};

int main()
{
	osdp_t *ctx;

	osdp_logger_init("osdp::cp", OSDP_LOG_DEBUG, NULL);

	ctx = osdp_cp_setup(&cp_channel, 1, pd_info);
	if (ctx == NULL) {
		printf("cp init failed!\n");
		return -1;
	}

	while (1) {
		// your application code.

		osdp_cp_refresh(ctx);
		// delay();
	}
	return 0;
}
