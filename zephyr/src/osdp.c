/*
 * Copyright (c) 2020-2026 Siddharth Chandrasekaran <siddharth@embedjournal.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include <utils/utils.h>

tick_t osdp_millis_now(void)
{
	return (tick_t)k_uptime_get();
}
