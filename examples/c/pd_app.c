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

enum osdp_pd_e {
	OSDP_PD_1,
	OSDP_PD_2,
	OSDP_PD_SENTINEL,
};

int sample_pd_send_func(void *data, uint8_t *buf, int len)
{
	(void)(data);
	(void)(buf);

	// TODO (user): send buf of len bytes, over the UART channel.

	return len;
}

int sample_pd_recv_func(void *data, uint8_t *buf, int len)
{
	(void)(data);
	(void)(buf);
	(void)(len);

	// TODO (user): read from UART channel into buf, for upto len bytes.

	return 0;
}

int pd_command_handler(void *arg, struct osdp_cmd *cmd)
{
	(void)(arg);

	printf("PD: CMD: %d\n", cmd->id);
	return 0;
}

osdp_pd_info_t info_pd = {
	.address = 101,
	.baud_rate = 9600,
	.flags = 0,
	.id = {
		.version = 1,
		.model = 153,
		.vendor_code = 31337,
		.serial_number = 0x01020304,
		.firmware_version = 0x0A0B0C0D,
	},
	.cap = (struct osdp_pd_cap []) {
		{
			.function_code = OSDP_PD_CAP_READER_LED_CONTROL,
			.compliance_level = 1,
			.num_items = 1
		},
		{
			.function_code = OSDP_PD_CAP_READER_AUDIBLE_OUTPUT,
			.compliance_level = 1,
			.num_items = 1
		},
		{ (uint8_t)-1, 0, 0 } /* Sentinel */
	},
	.scbk = NULL,
};

static struct osdp_channel pd_channel = {
	.data = NULL,
	.send = sample_pd_send_func,
	.recv = sample_pd_recv_func,
};

int main()
{
	osdp_t *ctx;

	osdp_logger_init("osdp::pd", OSDP_LOG_DEBUG, NULL);

	ctx = osdp_pd_setup(&pd_channel, &info_pd);
	if (ctx == NULL) {
		printf("pd init failed!\n");
		return -1;
	}

	osdp_pd_set_command_callback(ctx, pd_command_handler, NULL);

	while (1) {
		osdp_pd_refresh(ctx);

		// your application code.
		// delay();
	}

	return 0;
}
