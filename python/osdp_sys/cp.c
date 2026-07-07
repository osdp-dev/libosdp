/*
 * Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "module.h"

#define TAG "pyosdp_cp"

static void pyosdp_cp_free_pending_commands(pyosdp_cp_t *self)
{
	struct pyosdp_pending_cmd *node, *next;

	node = self->pending_cmd_head;
	while (node) {
		next = node->next;
		free(node);
		node = next;
	}
	self->pending_cmd_head = NULL;
}

static struct pyosdp_pending_cmd *
pyosdp_cp_take_pending_command(pyosdp_cp_t *self, int pd, const struct osdp_cmd *cmd)
{
	struct pyosdp_pending_cmd *cur, *prev = NULL;

	cur = self->pending_cmd_head;
	while (cur) {
		if (cur->pd == pd && &cur->cmd == cmd) {
			if (prev) {
				prev->next = cur->next;
			} else {
				self->pending_cmd_head = cur->next;
			}
			cur->next = NULL;
			return cur;
		}
		prev = cur;
		cur = cur->next;
	}
	return NULL;
}

#define pyosdp_cp_pd_status_doc                                                \
	"Get PD status, (online/offline) as a bitmask for all connected PDs\n" \
	"\n"                                                                   \
	"@return PD status bitmask"
static PyObject *pyosdp_cp_pd_status(pyosdp_cp_t *self, PyObject *args)
{
	uint32_t bitmask = 0;

	osdp_get_status_mask(self->ctx, (uint8_t *)&bitmask);

	return Py_BuildValue("I", bitmask);
}

#define pyosdp_cp_sc_status_doc                                                \
	"Get PD Secure Channel status bitmask of all connected PDs\n"          \
	"\n"                                                                   \
	"@return Secure Channel Status bitmask"
static PyObject *pyosdp_cp_sc_status(pyosdp_cp_t *self, PyObject *args)
{
	uint32_t bitmask = 0;

	osdp_get_sc_status_mask(self->ctx, (uint8_t *)&bitmask);

	return Py_BuildValue("I", bitmask);
}

int pyosdp_cp_event_cb(void *data, int address, struct osdp_event *event)
{
	pyosdp_cp_t *self = data;
	PyObject *arglist, *result, *event_dict;

	if (!self->event_cb)
		return 0;

	if (pyosdp_make_dict_event(&event_dict, event))
		return -1;

	arglist = Py_BuildValue("(IO)", address, event_dict);

	result = PyObject_CallObject(self->event_cb, arglist);

	Py_XDECREF(result);
	Py_DECREF(arglist);
	return 0;
}

static void pyosdp_cp_command_completion_cb(void *data, int pd,
					    const struct osdp_cmd *cmd,
					    enum osdp_completion_status status)
{
	pyosdp_cp_t *self = data;
	PyObject *arglist = NULL, *result = NULL, *cmd_dict = NULL;
	struct pyosdp_pending_cmd *pending;

	pending = pyosdp_cp_take_pending_command(self, pd, cmd);
	if (pending == NULL) {
		return;
	}

	if (self->command_completion_cb &&
	    pyosdp_make_dict_cmd(&cmd_dict, &pending->cmd) == 0) {
		arglist = Py_BuildValue("(IOI)", pd, cmd_dict, status);
		if (arglist) {
			result = PyObject_CallObject(self->command_completion_cb,
						     arglist);
		}
		if (result == NULL) {
			PyErr_Print();
		}
	}

	Py_XDECREF(result);
	Py_XDECREF(arglist);
	Py_XDECREF(cmd_dict);
	free(pending);
}

#define pyosdp_cp_set_command_completion_callback_doc                           \
	"Set OSDP command completion callback handler\n"                        \
	"\n"                                                                   \
	"@param callback Function called with (pd, command, status)\n"         \
	"\n"                                                                   \
	"@return None"
static PyObject *pyosdp_cp_set_command_completion_callback(pyosdp_cp_t *self,
							    PyObject *args)
{
	PyObject *callback = NULL;

	if (!PyArg_ParseTuple(args, "O", &callback))
		return NULL;

	if (callback == NULL || !PyCallable_Check(callback)) {
		PyErr_SetString(PyExc_TypeError, "Need a callable object!");
		return NULL;
	}

	Py_XDECREF(self->command_completion_cb);
	self->command_completion_cb = callback;
	Py_INCREF(self->command_completion_cb);
	Py_RETURN_NONE;
}

#define pyosdp_cp_set_event_callback_doc                                       \
	"Set OSDP event callback handler\n"                                    \
	"\n"                                                                   \
	"@param callback A function to call when a PD reports an event\n"      \
	"\n"                                                                   \
	"@return None"
static PyObject *pyosdp_cp_set_event_callback(pyosdp_cp_t *self, PyObject *args)
{
	PyObject *event_cb = NULL;

	if (!PyArg_ParseTuple(args, "O", &event_cb))
		return NULL;

	if (!event_cb || !PyCallable_Check(event_cb)) {
		PyErr_SetString(PyExc_TypeError, "Need a callable object!");
		return NULL;
	}

	Py_XDECREF(self->event_cb); /* if set_callback was called earlier */
	self->event_cb = event_cb;
	Py_INCREF(self->event_cb);
	Py_RETURN_NONE;
}

#define pyosdp_cp_refresh_doc                                                   \
	"OSDP periodic refresh hook. Must be called at least once every 50ms\n" \
	"\n"                                                                    \
	"@return None\n"
static PyObject *pyosdp_cp_refresh(pyosdp_cp_t *self, pyosdp_cp_t *args)
{
	osdp_cp_refresh(self->ctx);

	Py_RETURN_NONE;
}

#define pyosdp_cp_get_pd_id_doc                                                \
	"Get PD_ID info as reported by the PD\n"                               \
	"\n"                                                                   \
	"@param pd PD offset number\n"                                         \
	"\n"                                                                   \
	"@return dict with PD_ID info\n"
static PyObject *pyosdp_cp_get_pd_id(pyosdp_cp_t *self, PyObject *args)
{
	int pd;
	struct osdp_pd_id pd_id = {0};

	if (!PyArg_ParseTuple(args, "I", &pd)) {
		PyErr_SetString(PyExc_ValueError, "Invalid arguments");
		return NULL;
	}

	if (osdp_cp_get_pd_id(self->ctx, pd, &pd_id)) {
		PyErr_SetString(PyExc_ValueError, "invalid PD offset");
		return NULL;
	}

	return pyosdp_make_dict_pd_id(&pd_id);
}

#define pyosdp_cp_check_capability_doc                                         \
	"Get capability associated to a function_code as reported by the PD\n" \
	"\n"                                                                   \
	"@param pd PD offset number\n"                                         \
	"@param capability function code\n"                                    \
	"\n"                                                                   \
	"@return (compliance_level, num_items)\n"
static PyObject *pyosdp_cp_check_capability(pyosdp_cp_t *self, PyObject *args)
{
	int pd, function_code;
	struct osdp_pd_cap cap = {0};

	if (!PyArg_ParseTuple(args, "II", &pd, &function_code)) {
		PyErr_SetString(PyExc_ValueError, "Invalid arguments");
		return NULL;
	}

	cap.function_code = function_code;
	if (osdp_cp_get_capability(self->ctx, pd, &cap)) {
		PyErr_SetString(PyExc_ValueError, "invalid PD offset or function code");
		return NULL;
	}

	return Py_BuildValue("(II)", cap.compliance_level, cap.num_items);
}

#define pyosdp_cp_submit_command_doc                                                 \
	"Send an OSDP command to a PD\n"                                             \
	"\n"                                                                         \
	"@param pd PD offset number\n"                                               \
	"@param command A dict of command keys and values. See osdp.h for details\n" \
	"\n"                                                                         \
	"@return boolean status of command submission\n"
static PyObject *pyosdp_cp_submit_command(pyosdp_cp_t *self, PyObject *args)
{
	int pd, ret;
	PyObject *cmd_dict;
	struct pyosdp_pending_cmd *pending = NULL;

	if (!PyArg_ParseTuple(args, "IO!", &pd, &PyDict_Type, &cmd_dict)) {
		PyErr_SetString(PyExc_ValueError, "Invalid arguments");
		return NULL;
	}

	if (pd < 0 || pd >= self->num_pd) {
		PyErr_SetString(PyExc_ValueError, "Invalid PD offset");
		return NULL;
	}

	pending = calloc(1, sizeof(*pending));
	if (pending == NULL) {
		PyErr_SetString(PyExc_MemoryError, "command allocation failed");
		return NULL;
	}
	pending->pd = pd;

	if (pyosdp_make_struct_cmd(&pending->cmd, cmd_dict)) {
		free(pending);
		pyosdp_add_error_context(PyExc_ValueError,
			"Unable to convert command dict to OSDP command structure");
		Py_RETURN_FALSE;
	}

	ret = osdp_cp_submit_command(self->ctx, pd, &pending->cmd);
	if (ret == 0) {
		pending->next = self->pending_cmd_head;
		self->pending_cmd_head = pending;
		Py_RETURN_TRUE;
	}

	free(pending);

	Py_RETURN_FALSE;
}

#define pyosdp_cp_flush_commands_doc                                            \
	"Deletes all queued commands for a given PD.\n"                         \
	"\n"                                                                   \
	"@param pd PD offset number\n"                                          \
	"\n"                                                                   \
	"@return int Count of commands dequeued.\n"
static PyObject *pyosdp_cp_flush_commands(pyosdp_cp_t *self, PyObject *args)
{
	int pd, ret;

	if (!PyArg_ParseTuple(args, "I", &pd)) {
		PyErr_SetString(PyExc_ValueError, "Invalid arguments");
		return NULL;
	}
	if (pd < 0 || pd >= self->num_pd) {
		PyErr_SetString(PyExc_ValueError, "Invalid PD offset");
		return NULL;
	}

	ret = osdp_cp_flush_commands(self->ctx, pd);
	return Py_BuildValue("I", ret);
}

#define pyosdp_cp_disable_pd_doc                                             \
	"Disable a PD (simulate hot-plug removal)\n"                         \
	"\n"                                                                 \
	"@param pd PD offset number\n"                                       \
	"\n"                                                                 \
	"@return boolean status of disable request\n"
static PyObject *pyosdp_cp_disable_pd(pyosdp_cp_t *self, PyObject *args)
{
	int pd;

	if (!PyArg_ParseTuple(args, "I", &pd)) {
		PyErr_SetString(PyExc_ValueError, "Invalid arguments");
		return NULL;
	}

	if (pd < 0 || pd >= self->num_pd) {
		PyErr_SetString(PyExc_ValueError, "Invalid PD offset");
		return NULL;
	}

	if (osdp_cp_disable_pd(self->ctx, pd))
		Py_RETURN_FALSE;

	Py_RETURN_TRUE;
}

#define pyosdp_cp_enable_pd_doc                                              \
	"Enable a PD (simulate hot-plug insertion)\n"                        \
	"\n"                                                                 \
	"@param pd PD offset number\n"                                       \
	"\n"                                                                 \
	"@return boolean status of enable request\n"
static PyObject *pyosdp_cp_enable_pd(pyosdp_cp_t *self, PyObject *args)
{
	int ret, pd;

	if (!PyArg_ParseTuple(args, "I", &pd)) {
		PyErr_SetString(PyExc_ValueError, "Invalid arguments");
		return NULL;
	}

	if (pd < 0 || pd >= self->num_pd) {
		PyErr_SetString(PyExc_ValueError, "Invalid PD offset");
		return NULL;
	}

	if (osdp_cp_enable_pd(self->ctx, pd))
		Py_RETURN_FALSE;

	Py_RETURN_TRUE;
}

#define pyosdp_cp_is_pd_enabled_doc                                          \
	"Check if a PD is currently enabled\n"                               \
	"\n"                                                                 \
	"@param pd PD offset number\n"                                       \
	"\n"                                                                 \
	"@return boolean enabled state of the PD\n"
static PyObject *pyosdp_cp_is_pd_enabled(pyosdp_cp_t *self, PyObject *args)
{
	int pd;

	if (!PyArg_ParseTuple(args, "I", &pd)) {
		PyErr_SetString(PyExc_ValueError, "Invalid arguments");
		return NULL;
	}

	if (pd < 0 || pd >= self->num_pd) {
		PyErr_SetString(PyExc_ValueError, "Invalid PD offset");
		return NULL;
	}

	if (osdp_cp_is_pd_enabled(self->ctx, pd))
		Py_RETURN_TRUE;

	Py_RETURN_FALSE;
}

static PyObject *pyosdp_cp_modify_flag(pyosdp_cp_t *self, PyObject *args, bool do_set)
{
	int ret, pd, flags;

	if (!PyArg_ParseTuple(args, "II", &pd, &flags))
		Py_RETURN_FALSE;

	if (pd < 0 || pd >= self->num_pd) {
		PyErr_SetString(PyExc_ValueError, "Invalid PD offset");
		return NULL;
	}

	if (osdp_cp_modify_flag(self->ctx, pd, (uint32_t)flags, do_set))
		Py_RETURN_FALSE;

	Py_RETURN_TRUE;
}

#define pyosdp_cp_set_flag_doc \
	"Set PD flag\n" \
	"\n" \
	"@param pd_idx PD offset number\n" \
	"@param flag One of the OSDP public flags" \
	"\n" \
	"@return boolean status"
static PyObject *pyosdp_cp_set_flag(pyosdp_cp_t *self, PyObject *args)
{
	return pyosdp_cp_modify_flag(self, args, true);
}

#define pyosdp_cp_clear_flag_doc \
	"Clear PD flag\n" \
	"\n" \
	"@param pd_idx PD offset number\n" \
	"@param flag One of the OSDP public flags" \
	"\n" \
	"@return boolean status"
static PyObject *pyosdp_cp_clear_flag(pyosdp_cp_t *self, PyObject *args)
{
	return pyosdp_cp_modify_flag(self, args, false);
}

static int pyosdp_cp_tp_clear(pyosdp_cp_t *self)
{
	Py_XDECREF(self->event_cb);
	self->event_cb = NULL;
	Py_XDECREF(self->command_completion_cb);
	self->command_completion_cb = NULL;
	pyosdp_cp_free_pending_commands(self);
	return 0;
}

static PyObject *pyosdp_cp_tp_new(PyTypeObject *type, PyObject *args,
				  PyObject *kwargs)
{
	pyosdp_cp_t *self = NULL;

	self = (pyosdp_cp_t *)type->tp_alloc(type, 0);
	if (self == NULL) {
		return NULL;
	}
	self->ctx = NULL;
	self->event_cb = NULL;
	self->command_completion_cb = NULL;
	self->pending_cmd_head = NULL;
	self->num_pd = 0;
	return (PyObject *)self;
}

static void pyosdp_cp_tp_dealloc(pyosdp_cp_t *self)
{
	if (self->ctx)
		osdp_cp_teardown(self->ctx);

	/* Free allocated name string */
	free(self->name);

	/* call base class destructor */
	OSDPBaseType.tp_dealloc((PyObject *)self);

	pyosdp_cp_tp_clear(self);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

#define pyosdp_cp_tp_init_doc                                                                \
	"OSDP Control Panel Class\n"                                                         \
	"\n"                                                                                 \
	"@param pd_info List of PD info dicts. See osdp_pd_info_t in osdp.h for more info\n" \
	"\n"                                                                                 \
	"@return None"
static int pyosdp_cp_tp_init(pyosdp_cp_t *self, PyObject *args, PyObject *kwargs)
{
	int i, len, baud_rate;
	uint8_t *scbk = NULL;
	PyObject *py_info_list, *py_info, *channel;
	static char *kwlist[] = { "", NULL };
	osdp_t *ctx;
	osdp_pd_info_t *info, *info_list = NULL;
	struct osdp_channel cp_channel = { 0 };

	/* call base class constructor */
	if (OSDPBaseType.tp_init((PyObject *)self, args, kwargs) < 0)
		return -1;

	self->base.is_cp = true;

	/* the string after the : is used as the function name in error messages */
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O!:pyosdp_cp_init",
					 kwlist, &PyList_Type, &py_info_list))
		goto error;

	self->num_pd = (int)PyList_Size(py_info_list);
	if (self->num_pd == 0 || self->num_pd > 127) {
		PyErr_SetString(PyExc_ValueError, "Invalid num_pd");
		goto error;
	}

	info_list = calloc(self->num_pd, sizeof(osdp_pd_info_t));
	if (info_list == NULL) {
		PyErr_SetString(PyExc_MemoryError, "pd_info alloc error");
		goto error;
	}

	for (i = 0; i < self->num_pd; i++) {
		info = info_list + i;
		py_info = PyList_GetItem(py_info_list, i);
		if (py_info == NULL) {
			PyErr_SetString(PyExc_ValueError,
					"py_info_list extract error");
			goto error;
		}

		if (pyosdp_dict_get_str(py_info, "name", &self->name) == 0)
			info->name = self->name;
		else
			info->name = NULL;

		if (pyosdp_dict_get_int(py_info, "address", &info->address)) {
			pyosdp_add_error_context(PyExc_ValueError,
				"Invalid pd_info at index %d", i);
			goto error;
		}

		if (pyosdp_dict_get_int(py_info, "flags", &info->flags)) {
			pyosdp_add_error_context(PyExc_ValueError,
				"Invalid pd_info at index %d", i);
			goto error;
		}

		if (pyosdp_dict_get_int(py_info, "baud_rate", &baud_rate)) {
			PyErr_Clear();
			baud_rate = 9600;
		}
		if (baud_rate <= 0) {
			PyErr_Format(PyExc_ValueError,
				     "Invalid baud_rate in pd_info at index %d", i);
			goto error;
		}
		info->baud_rate = baud_rate;

			if (i == 0) {
				channel = PyDict_GetItemString(py_info, "channel");
				if (channel == NULL) {
					PyErr_SetString(PyExc_KeyError,
							"channel object missing in pd_info[0]");
					goto error;
				}
				pyosdp_get_channel(channel, &cp_channel);
			}

		if (pyosdp_dict_get_bytes(py_info, "scbk", &scbk, &len) == 0) {
			if (scbk && len != 16) {
				PyErr_Format(PyExc_TypeError,
					"scbk must be exactly 16 bytes in pd_info at index %d", i);
				goto error;
			}
			info->scbk = scbk;
		} else {
			PyErr_Clear();
		}
	}

	ctx = osdp_cp_setup(&cp_channel, self->num_pd, info_list);
	if (ctx == NULL) {
		pyosdp_add_error_context(PyExc_Exception,
			"Failed to setup CP (check pd_info configuration)");
		goto error;
	}

	osdp_cp_set_event_callback(ctx, pyosdp_cp_event_cb, self);
	osdp_cp_set_command_completion_callback(ctx,
						pyosdp_cp_command_completion_cb,
						self);

	self->ctx = ctx;
	return 0;
error:
	free(info_list);
	return -1;
}

PyObject *pyosdp_cp_tp_repr(PyObject *self)
{
	PyObject *py_string;

	py_string = Py_BuildValue("s", "control panel object");

	return py_string;
}

static int pyosdp_cp_tp_traverse(pyosdp_cp_t *self, visitproc visit, void *arg)
{
	Py_VISIT(self->event_cb);
	Py_VISIT(self->command_completion_cb);
	return 0;
}

PyObject *pyosdp_cp_tp_str(PyObject *self)
{
	return pyosdp_cp_tp_repr(self);
}

static PyGetSetDef pyosdp_cp_tp_getset[] = {
	{ NULL } /* Sentinel */
};

static PyMethodDef pyosdp_cp_tp_methods[] = {
	{ "refresh", (PyCFunction)pyosdp_cp_refresh,
	  METH_NOARGS, pyosdp_cp_refresh_doc },
	{ "set_event_callback", (PyCFunction)pyosdp_cp_set_event_callback,
	  METH_VARARGS, pyosdp_cp_set_event_callback_doc },
	{ "set_command_completion_callback", (PyCFunction)pyosdp_cp_set_command_completion_callback,
	  METH_VARARGS, pyosdp_cp_set_command_completion_callback_doc },
	{ "submit_command", (PyCFunction)pyosdp_cp_submit_command,
	  METH_VARARGS, pyosdp_cp_submit_command_doc },
	{ "flush_commands", (PyCFunction)pyosdp_cp_flush_commands,
	  METH_VARARGS, pyosdp_cp_flush_commands_doc },
	{ "status", (PyCFunction)pyosdp_cp_pd_status,
	  METH_NOARGS, pyosdp_cp_pd_status_doc },
	{ "sc_status", (PyCFunction)pyosdp_cp_sc_status,
	  METH_NOARGS, pyosdp_cp_sc_status_doc },
	{ "set_flag", (PyCFunction)pyosdp_cp_set_flag,
	  METH_VARARGS, pyosdp_cp_set_flag_doc },
	{ "clear_flag", (PyCFunction)pyosdp_cp_clear_flag,
	  METH_VARARGS, pyosdp_cp_clear_flag_doc },
	{ "get_pd_id", (PyCFunction)pyosdp_cp_get_pd_id,
	  METH_VARARGS, pyosdp_cp_get_pd_id_doc },
	{ "check_capability", (PyCFunction)pyosdp_cp_check_capability,
	  METH_VARARGS, pyosdp_cp_check_capability_doc },
	{ "disable_pd", (PyCFunction)pyosdp_cp_disable_pd,
	  METH_VARARGS, pyosdp_cp_disable_pd_doc },
	{ "enable_pd", (PyCFunction)pyosdp_cp_enable_pd,
	  METH_VARARGS, pyosdp_cp_enable_pd_doc },
	{ "is_pd_enabled", (PyCFunction)pyosdp_cp_is_pd_enabled,
	  METH_VARARGS, pyosdp_cp_is_pd_enabled_doc },
	{ NULL, NULL, 0, NULL } /* Sentinel */
};

static PyMemberDef pyosdp_cp_tp_members[] = {
	{ NULL } /* Sentinel */
};

static PyTypeObject ControlPanelTypeObject = {
	PyVarObject_HEAD_INIT(NULL, 0).tp_name = "ControlPanel",
	.tp_doc = pyosdp_cp_tp_init_doc,
	.tp_basicsize = sizeof(pyosdp_cp_t),
	.tp_itemsize = 0,
	.tp_dealloc = (destructor)pyosdp_cp_tp_dealloc,
	.tp_repr = pyosdp_cp_tp_repr,
	.tp_str = pyosdp_cp_tp_str,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_traverse = (traverseproc)pyosdp_cp_tp_traverse,
	.tp_clear = (inquiry)pyosdp_cp_tp_clear,
	.tp_methods = pyosdp_cp_tp_methods,
	.tp_members = pyosdp_cp_tp_members,
	.tp_getset = pyosdp_cp_tp_getset,
	.tp_init = (initproc)pyosdp_cp_tp_init,
	.tp_new = pyosdp_cp_tp_new,
	.tp_base = &OSDPBaseType,
};

int pyosdp_add_type_cp(PyObject *module)
{
	return pyosdp_module_add_type(module, "ControlPanel",
				      &ControlPanelTypeObject);
}
