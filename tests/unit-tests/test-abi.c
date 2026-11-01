/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <osdp.h>
#include "test.h"

static uint32_t options_this_build_was_compiled_with(void)
{
	uint32_t options = 0;

#ifdef OPT_OSDP_RX_ZERO_COPY
	options |= OSDP_ABI_OPT_RX_ZERO_COPY;
#endif
#ifdef OPT_OSDP_LOG_MINIMAL
	options |= OSDP_ABI_OPT_LOG_MINIMAL;
#endif
	return options;
}

static bool abi_info_reports_struct_sizes(void)
{
	struct osdp_abi_info info;

	memset(&info, 0xA5, sizeof(info));
	info.size = sizeof(info);
	osdp_get_abi_info(&info);
	return info.size == sizeof(info) &&
	       info.sizeof_cmd == sizeof(struct osdp_cmd) &&
	       info.sizeof_event == sizeof(struct osdp_event) &&
	       info.sizeof_channel == sizeof(struct osdp_channel) &&
	       info.sizeof_pd_info == sizeof(osdp_pd_info_t);
}

/* A caller compiled against an older, shorter header must not be overrun. */
static bool abi_info_respects_caller_size(void)
{
	struct osdp_abi_info info;
	const uint32_t short_size = offsetof(struct osdp_abi_info, sizeof_cmd);

	memset(&info, 0xA5, sizeof(info));
	info.size = short_size;
	osdp_get_abi_info(&info);
	return info.size == short_size &&
	       info.options == options_this_build_was_compiled_with() &&
	       info.sizeof_cmd == 0xA5A5A5A5 &&
	       info.sizeof_pd_info == 0xA5A5A5A5;
}

/* A caller compiled against a newer, longer header must keep its tail. */
static bool abi_info_leaves_caller_tail_untouched(void)
{
	struct {
		struct osdp_abi_info info;
		uint32_t tail;
	} wrapper;

	memset(&wrapper, 0xA5, sizeof(wrapper));
	wrapper.info.size = sizeof(wrapper);
	osdp_get_abi_info(&wrapper.info);
	return wrapper.info.size == sizeof(struct osdp_abi_info) &&
	       wrapper.tail == 0xA5A5A5A5;
}

void run_abi_tests(struct test *t)
{
	printf("\nBegin ABI info tests\n");
	TEST_CASE(t, "abi_info_reports_struct_sizes",
		  abi_info_reports_struct_sizes());
	TEST_CASE(t, "abi_info_respects_caller_size",
		  abi_info_respects_caller_size());
	TEST_CASE(t, "abi_info_leaves_caller_tail_untouched",
		  abi_info_leaves_caller_tail_untouched());
}
