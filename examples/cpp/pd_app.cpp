/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <osdp.hpp>

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

osdp_pd_info_t info_pd = {
	.name = "pd[101]",
	.baud_rate = 9600,
	.address = 101,
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
			{ static_cast<uint8_t>(-1), 0, 0 } /* Sentinel */
		},
		.scbk = nullptr,
};

static struct osdp_channel pd_channel = {
	.data = nullptr,
	.recv = sample_pd_recv_func,
	.send = sample_pd_send_func,
	.flush = nullptr,
	.close = nullptr,
};

int pd_command_handler(void *data, struct osdp_cmd *cmd)
{
	(void)(data);

	std::cout << "PD: CMD: " << cmd->id << std::endl;
	return 0;
}

int main()
{
	OSDP::PeripheralDevice pd;

	pd.logger_init("osdp::pd", OSDP_LOG_DEBUG, NULL);

	pd.setup(&pd_channel, &info_pd);

	pd.set_command_callback(pd_command_handler, nullptr);

	while (1) {
		pd.refresh();

		// your application code.
		std::this_thread::sleep_for(std::chrono::microseconds(10 * 1000));
	}

	return 0;
}
