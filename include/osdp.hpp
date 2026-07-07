/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIBOSDP_OSDP_HPP_
#define LIBOSDP_OSDP_HPP_

#include <osdp.h>

/**
 * @file osdp.hpp
 * @brief LibOSDP classical wrapper. See osdp.h for documentation.
 */

namespace OSDP {

class OSDP_EXPORT Common {
public:
	Common() : _ctx(nullptr) {}

#ifndef OPT_OSDP_LOG_MINIMAL
	void logger_init(const char *name, int log_level,
			 osdp_log_puts_fn_t puts_fn)
	{
		osdp_logger_init(name, log_level, puts_fn);
	}
#endif

	void set_log_callback(osdp_log_callback_fn_t cb)
	{
		osdp_set_log_callback(cb);
	}

	const char *get_version()
	{
		return osdp_get_version();
	}

	const char *get_source_info()
	{
		return osdp_get_source_info();
	}

	void get_status_mask(uint8_t *bitmask)
	{
		osdp_get_status_mask(_ctx, bitmask);
	}

	void get_sc_status_mask(uint8_t *bitmask)
	{
		osdp_get_sc_status_mask(_ctx, bitmask);
	}

	int file_register_ops(int pd, struct osdp_file_ops *ops)
	{
		return osdp_file_register_ops(_ctx, pd, ops);
	}

	int file_tx_get_status(int pd, int *size, int *offset)
	{
		return osdp_get_file_tx_status(_ctx, pd, size, offset);
	}

	int get_metrics(int pd, struct osdp_metrics *metrics)
	{
		return osdp_get_metrics(_ctx, pd, metrics);
	}

protected:
	osdp_t *_ctx;
};

class OSDP_EXPORT ControlPanel : public Common {
public:
	ControlPanel() {}

	~ControlPanel()
	{
		if (_ctx) {
			osdp_cp_teardown(_ctx);
		}
	}

	void teardown()
	{
		if (_ctx) {
			osdp_cp_teardown(_ctx);
			_ctx = nullptr;
		}
	}

	bool setup(const struct osdp_channel *channel, int num_pd, const osdp_pd_info_t *info)
	{
		_ctx = osdp_cp_setup(channel, num_pd, info);
		return _ctx != nullptr;
	}

	bool setup()
	{
		return false;
	}

	int add_pd(int num_pd, const osdp_pd_info_t *info)
	{
		return osdp_cp_add_pd(_ctx, num_pd, info);
	}

	void refresh()
	{
		osdp_cp_refresh(_ctx);
	}

	[[deprecated]]
	int send_command(int pd, const struct osdp_cmd *cmd)
	{
		return osdp_cp_submit_command(_ctx, pd, cmd);
	}

	int submit_command(int pd, const struct osdp_cmd *cmd)
	{
		return osdp_cp_submit_command(_ctx, pd, cmd);
	}

	int flush_commands(int pd)
	{
		return osdp_cp_flush_commands(_ctx, pd);
	}

	void set_event_callback(cp_event_callback_t cb, void *arg)
	{
		osdp_cp_set_event_callback(_ctx, cb, arg);
	}

	void set_command_completion_callback(cp_command_completion_callback_t cb,
					     void *arg)
	{
		osdp_cp_set_command_completion_callback(_ctx, cb, arg);
	}

	int get_pd_id(int pd, struct osdp_pd_id *id)
	{
		return osdp_cp_get_pd_id(_ctx, pd, id);
	}

	int get_capability(int pd, struct osdp_pd_cap *cap)
	{
		return osdp_cp_get_capability(_ctx, pd, cap);
	}

	int modify_flag(int pd, uint32_t flags, bool do_set)
	{
		return osdp_cp_modify_flag(_ctx, pd, flags, do_set);
	}

	int disable_pd(int pd)
	{
		return osdp_cp_disable_pd(_ctx, pd);
	}

	int enable_pd(int pd)
	{
		return osdp_cp_enable_pd(_ctx, pd);
	}

	bool is_pd_enabled(int pd)
	{
		return osdp_cp_is_pd_enabled(_ctx, pd);
	}

};

class OSDP_EXPORT PeripheralDevice : public Common {
public:
	PeripheralDevice() {}

	~PeripheralDevice()
	{
		if (_ctx) {
			osdp_pd_teardown(_ctx);
		}
	}

	void teardown()
	{
		if (_ctx) {
			osdp_pd_teardown(_ctx);
			_ctx = nullptr;
		}
	}

	bool setup(struct osdp_channel *channel, const osdp_pd_info_t *info)
	{
		_ctx = osdp_pd_setup(channel, info);
		return _ctx != nullptr;
	}

	void refresh()
	{
		osdp_pd_refresh(_ctx);
	}

	void set_capabilities(const struct osdp_pd_cap *cap)
	{
		osdp_pd_set_capabilities(_ctx, cap);
	}

	void set_command_callback(pd_command_callback_t cb, void* args)
	{
		osdp_pd_set_command_callback(_ctx, cb, args);
	}

	void set_event_completion_callback(pd_event_completion_callback_t cb,
					   void *arg)
	{
		osdp_pd_set_event_completion_callback(_ctx, cb, arg);
	}

	[[deprecated]]
	int notify_event(struct osdp_event *event)
	{
		return osdp_pd_submit_event(_ctx, event);
	}

	int submit_event(struct osdp_event *event)
	{
		return osdp_pd_submit_event(_ctx, event);
	}

	int flush_events()
	{
		return osdp_pd_flush_events(_ctx);
	}
};

}; /* namespace OSDP */

#endif // LIBOSDP_OSDP_HPP_
