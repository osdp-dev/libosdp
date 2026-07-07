/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <osdp.hpp>

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
		.name = "pd[101]",
		.baud_rate = 115200,
		.address = 101,
		.flags = 0,
		.id = {},
		.cap = nullptr,
		.scbk = nullptr,
	}
};

static struct osdp_channel cp_channel = {
	.data = nullptr,
	.recv = sample_cp_recv_func,
	.send = sample_cp_send_func,
	.flush = nullptr,
	.close = nullptr,
};

int event_handler(void *data, int pd, struct osdp_event *event) {
	(void)(data);

	std::cout << "PD" << pd << " EVENT: " << event->type << std::endl;
	return 0;
}

int main()
{
	OSDP::ControlPanel cp;

	cp.logger_init("osdp::cp", OSDP_LOG_DEBUG, NULL);

	cp.setup(&cp_channel, 1, pd_info);

	cp.set_event_callback(event_handler, nullptr);

	while (1) {
		// your application code.

		cp.refresh();
		std::this_thread::sleep_for(std::chrono::microseconds(10 * 1000));
	}

	return 0;
}
