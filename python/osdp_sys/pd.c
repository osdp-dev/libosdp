/*
 * Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "module.h"

#define TAG "pyosdp_pd"

static void pyosdp_pd_free_pending_events(pyosdp_pd_t *self)
{
	struct pyosdp_pending_event *node, *next;

	node = self->pending_event_head;
	while (node) {
		next = node->next;
		free(node);
		node = next;
	}
	self->pending_event_head = NULL;
}

static struct pyosdp_pending_event *
pyosdp_pd_take_pending_event(pyosdp_pd_t *self, const struct osdp_event *event)
{
	struct pyosdp_pending_event *cur, *prev = NULL;

	cur = self->pending_event_head;
	while (cur) {
		if (&cur->event == event) {
			if (prev) {
				prev->next = cur->next;
			} else {
				self->pending_event_head = cur->next;
			}
			cur->next = NULL;
			return cur;
		}
		prev = cur;
		cur = cur->next;
	}
	return NULL;
}

#define pyosdp_pd_is_online_doc                                                \
	"Get PD status, (online/offline)\n"                                    \
	"\n"                                                                   \
	"@return PD online status (Bool)"
static PyObject *pyosdp_pd_is_online(pyosdp_pd_t *self, PyObject *args)
{
	uint64_t mask;

	osdp_get_status_mask(self->ctx, (uint8_t *)&mask);

	if (mask & 1)
		Py_RETURN_TRUE;
	else
		Py_RETURN_FALSE;
}

#define pyosdp_pd_is_sc_active_doc                                             \
	"Get Secure Channel status, (active/inactive)\n"                       \
	"\n"                                                                   \
	"@return Secure Channel Status (Bool)"
static PyObject *pyosdp_pd_is_sc_active(pyosdp_pd_t *self, PyObject *args)
{
	uint64_t mask;

	osdp_get_sc_status_mask(self->ctx, (uint8_t *)&mask);

	if (mask & 1)
		Py_RETURN_TRUE;
	else
		Py_RETURN_FALSE;
}

#define pyosdp_pd_submit_event_doc                                               \
	"Notify the CP of an OSDP event\n"                                       \
	"\n"                                                                     \
	"@param event A dict of event keys and values. See osdp.h for details\n" \
	"\n"                                                                     \
	"@return None\n"
static PyObject *pyosdp_pd_submit_event(pyosdp_pd_t *self, PyObject *args)
{
	PyObject *event_dict;
	struct pyosdp_pending_event *pending = NULL;

	if (!PyArg_ParseTuple(args, "O", &event_dict)) {
		PyErr_SetString(PyExc_TypeError, "Failed to parse event dict!");
		return NULL;
	}

	pending = calloc(1, sizeof(*pending));
	if (pending == NULL) {
		PyErr_SetString(PyExc_MemoryError, "event allocation failed");
		return NULL;
	}
	if (pyosdp_make_struct_event(&pending->event, event_dict)) {
		free(pending);
		pyosdp_add_error_context(PyExc_TypeError,
			"Unable to convert event dict to OSDP event structure");
		return NULL;
	}
	if (osdp_pd_submit_event(self->ctx, &pending->event)) {
		free(pending);
		Py_RETURN_FALSE;
	}
	pending->next = self->pending_event_head;
	self->pending_event_head = pending;
	Py_RETURN_TRUE;
}

#define pyosdp_pd_flush_events_doc                                               \
	"Deletes all events from the PD's event queue.\n"                        \
	"\n"                                                                     \
	"@return int Count of events dequeued.\n"
static PyObject *pyosdp_pd_flush_events(pyosdp_pd_t *self)
{
	int ret;

	ret = osdp_pd_flush_events(self->ctx);

	return Py_BuildValue("I", ret);
}

static int pd_command_cb(void *arg, struct osdp_cmd *cmd)
{
	int ret_val = -1;
	pyosdp_pd_t *self = arg;
	PyObject *dict, *arglist, *result;

	if (pyosdp_make_dict_cmd(&dict, cmd))
		return -1;

	arglist = Py_BuildValue("(O)", dict);
	result = PyObject_CallObject(self->command_cb, arglist);
	PyArg_ParseTuple(result, "IO", &ret_val, &result);

	if (ret_val == 0 && result && PyDict_Check(result)) {
		memset(cmd, 0, sizeof(struct osdp_cmd));
		if (pyosdp_make_struct_cmd(cmd, result) < 0)
			ret_val = -1;
	}

	Py_XDECREF(dict);
	Py_XDECREF(result);
	Py_DECREF(arglist);
	return ret_val;
}

static void pyosdp_pd_event_completion_cb(void *arg, const struct osdp_event *event,
					  enum osdp_completion_status status)
{
	pyosdp_pd_t *self = arg;
	PyObject *arglist = NULL, *result = NULL, *event_dict = NULL;
	struct pyosdp_pending_event *pending;

	pending = pyosdp_pd_take_pending_event(self, event);
	if (pending == NULL) {
		return;
	}

	if (self->event_completion_cb &&
	    pyosdp_make_dict_event(&event_dict, &pending->event) == 0) {
		arglist = Py_BuildValue("(OI)", event_dict, status);
		if (arglist) {
			result = PyObject_CallObject(self->event_completion_cb,
						     arglist);
		}
		if (result == NULL) {
			PyErr_Print();
		}
	}

	Py_XDECREF(result);
	Py_XDECREF(arglist);
	Py_XDECREF(event_dict);
	free(pending);
}

#define pyosdp_pd_set_event_completion_callback_doc                             \
	"Set OSDP event completion callback handler\n"                          \
	"\n"                                                                   \
	"@param callback Function called with (event, status)\n"               \
	"\n"                                                                   \
	"@return None"
static PyObject *pyosdp_pd_set_event_completion_callback(pyosdp_pd_t *self,
							  PyObject *args)
{
	PyObject *callable = NULL;

	if (!PyArg_ParseTuple(args, "O", &callable))
		return NULL;

	if (callable == NULL || !PyCallable_Check(callable)) {
		PyErr_SetString(PyExc_TypeError, "Need a callable object!");
		return NULL;
	}

	Py_XDECREF(self->event_completion_cb);
	self->event_completion_cb = callable;
	Py_INCREF(self->event_completion_cb);
	Py_RETURN_NONE;
}

#define pyosdp_pd_set_command_callback_doc                                     \
	"Set OSDP command callback handler\n"                                  \
	"\n"                                                                   \
	"@param callback A function to call when a CP sends a command\n"       \
	"\n"                                                                   \
	"@return None"
static PyObject *pyosdp_pd_set_command_callback(pyosdp_pd_t *self, PyObject *args)
{
	PyObject *callable = NULL;

	if (!PyArg_ParseTuple(args, "O", &callable))
		return NULL;

	if (callable == NULL || !PyCallable_Check(callable)) {
		PyErr_SetString(PyExc_TypeError, "Need a callable object!");
		return NULL;
	}

	Py_XDECREF(self->command_cb); /* release previous callback if any */
	self->command_cb = callable;
	Py_INCREF(self->command_cb);
	osdp_pd_set_command_callback(self->ctx, pd_command_cb, (void *)self);
	Py_RETURN_NONE;
}

#define pyosdp_pd_refresh_doc                                                   \
	"OSDP periodic refresh hook. Must be called at least once every 50ms\n" \
	"\n"                                                                    \
	"@return None\n"
static PyObject *pyosdp_pd_refresh(pyosdp_pd_t *self, PyObject *args)
{
	osdp_pd_refresh(self->ctx);

	Py_RETURN_NONE;
}

static int pyosdp_pd_tp_clear(pyosdp_pd_t *self)
{
	Py_XDECREF(self->command_cb);
	self->command_cb = NULL;
	Py_XDECREF(self->event_completion_cb);
	self->event_completion_cb = NULL;
	pyosdp_pd_free_pending_events(self);
	return 0;
}

static PyObject *pyosdp_pd_tp_new(PyTypeObject *type, PyObject *args,
				  PyObject *kwargs)
{
	pyosdp_pd_t *self = NULL;

	self = (pyosdp_pd_t *)type->tp_alloc(type, 0);
	if (self == NULL) {
		return NULL;
	}
	self->ctx = NULL;
	self->command_cb = NULL;
	self->event_completion_cb = NULL;
	self->pending_event_head = NULL;
	return (PyObject *)self;
}

static void pyosdp_pd_tp_dealloc(pyosdp_pd_t *self)
{
	if (self->ctx)
		osdp_pd_teardown(self->ctx);

	/* Free allocated name string */
	free(self->name);

	/* call base class destructor */
	OSDPBaseType.tp_dealloc((PyObject *)self);

	pyosdp_pd_tp_clear(self);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static int pyosdp_add_pd_cap(PyObject *obj, osdp_pd_info_t *info)
{
	PyObject *py_pd_cap;
	int i, cap_list_size, function_code, compliance_level, num_items;
	struct osdp_pd_cap *cap;

	cap_list_size = (int)PyList_Size(obj);
	if (cap_list_size == 0)
		return 0;
	if (cap_list_size >= OSDP_PD_CAP_SENTINEL) {
		PyErr_SetString(PyExc_ValueError, "Invalid cap list size");
		return -1;
	}

	cap = calloc(cap_list_size + 1, sizeof(struct osdp_pd_cap));
	if (cap == NULL) {
		PyErr_SetString(PyExc_MemoryError, "pd cap alloc error");
		return -1;
	}

	for (i = 0; i < cap_list_size; i++) {
		py_pd_cap = PyList_GetItem(obj, i);

		if (pyosdp_dict_get_int(py_pd_cap, "function_code",
					&function_code)) {
			pyosdp_add_error_context(PyExc_ValueError,
				"Invalid capability at index %d", i);
			goto error;
		}

		if (pyosdp_dict_get_int(py_pd_cap, "compliance_level",
					&compliance_level)) {
			pyosdp_add_error_context(PyExc_ValueError,
				"Invalid capability at index %d", i);
			goto error;
		}

		if (pyosdp_dict_get_int(py_pd_cap, "num_items", &num_items)) {
			pyosdp_add_error_context(PyExc_ValueError,
				"Invalid capability at index %d", i);
			goto error;
		}

		cap[i].function_code = (uint8_t)function_code;
		cap[i].compliance_level = (uint8_t)compliance_level;
		cap[i].num_items = (uint8_t)num_items;
	}
	info->cap = cap;
	return 0;
error:
	free(cap);
	return -1;
}

#define pyosdp_pd_tp_init_doc                                                                         \
	"OSDP Peripheral Device Class\n"                                                              \
	"\n"                                                                                          \
	"@param pd_info A dict with osdp_pd_info_t keys and values. See osdp.h for more info.\n"      \
	"@param capabilities A list of osdp_pd_cap_t keys and values. See osdp.h for more details.\n" \
	"@param scbk A hexadecimal string representation of the PD secure channel base key\n"         \
	"\n"                                                                                          \
	"@return None"
static int pyosdp_pd_tp_init(pyosdp_pd_t *self, PyObject *args, PyObject *kwargs)
{
	int scbk_length, baud_rate;
	osdp_t *ctx;
	osdp_pd_info_t info = { 0 };
	struct osdp_channel osdp_channel = { 0 };
	static char *kwlist[] = { "", "capabilities", NULL };
	PyObject *py_info, *py_pd_cap_list, *channel;
	uint8_t *scbk = NULL;

	/* call base class constructor */
	if (OSDPBaseType.tp_init((PyObject *)self, NULL, NULL) < 0)
		return -1;

	self->base.is_cp = false;

	/* the string after the : is used as the function name in error messages */
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O!|$O!:pyosdp_pd_init", kwlist,
					 &PyDict_Type, &py_info, &PyList_Type,
					 &py_pd_cap_list))
		goto error;

	if (pyosdp_dict_get_str(py_info, "name", &self->name) == 0)
		info.name = self->name;
	else
		info.name = NULL;

	if (py_pd_cap_list && pyosdp_add_pd_cap(py_pd_cap_list, &info))
		goto error;

	if (pyosdp_dict_get_int(py_info, "address", &info.address))
		goto error;

	if (pyosdp_dict_get_int(py_info, "flags", &info.flags))
		goto error;

	if (pyosdp_dict_get_int(py_info, "baud_rate", &baud_rate)) {
		PyErr_Clear();
		baud_rate = 9600;
	}
	if (baud_rate <= 0) {
		PyErr_SetString(PyExc_ValueError, "Invalid baud_rate");
		goto error;
	}
	info.baud_rate = baud_rate;

	channel = PyDict_GetItemString(py_info, "channel");
	if (channel == NULL) {
		PyErr_Format(PyExc_KeyError, "channel object missing");
		return -1;
	}
	pyosdp_get_channel(channel, &osdp_channel);

	if (pyosdp_dict_get_int(py_info, "version", &info.id.version))
		goto error;

	if (pyosdp_dict_get_int(py_info, "model", &info.id.model))
		goto error;

	if (pyosdp_dict_get_int(py_info, "vendor_code",
				(int *)&info.id.vendor_code))
		goto error;

	if (pyosdp_dict_get_int(py_info, "firmware_version",
				(int *)&info.id.firmware_version))
		goto error;

	if (pyosdp_dict_get_int(py_info, "serial_number",
				(int *)&info.id.serial_number))
		goto error;

	if (pyosdp_dict_get_bytes(py_info, "scbk", &scbk, &scbk_length) == 0) {
		if (scbk && scbk_length == 16)
			info.scbk = scbk;
	} else {
		PyErr_Clear();
	}

	ctx = osdp_pd_setup(&osdp_channel, &info);
	if (ctx == NULL) {
		pyosdp_add_error_context(PyExc_Exception,
			"Failed to setup PD (check pd_info configuration)");
		goto error;
	}

	osdp_pd_set_event_completion_callback(ctx, pyosdp_pd_event_completion_cb,
					      (void *)self);
	self->ctx = ctx;
	free((void *)info.cap);
	return 0;
error:
	free((void *)info.cap);
	return -1;
}

PyObject *pyosdp_pd_tp_repr(PyObject *self)
{
	PyObject *py_string;

	py_string = Py_BuildValue("s", "peripheral device object");

	return py_string;
}

static int pyosdp_pd_tp_traverse(pyosdp_pd_t *self, visitproc visit, void *arg)
{
	Py_VISIT(self->command_cb);
	Py_VISIT(self->event_completion_cb);
	return 0;
}

PyObject *pyosdp_pd_tp_str(PyObject *self)
{
	return pyosdp_pd_tp_repr(self);
}

static PyGetSetDef pyosdp_pd_tp_getset[] = {
	{ NULL } /* Sentinel */
};

static PyMethodDef pyosdp_pd_tp_methods[] = {
	{ "refresh", (PyCFunction)pyosdp_pd_refresh,
	  METH_NOARGS, pyosdp_pd_refresh_doc },
	{ "set_command_callback", (PyCFunction)pyosdp_pd_set_command_callback,
	  METH_VARARGS, pyosdp_pd_set_command_callback_doc },
	{ "set_event_completion_callback", (PyCFunction)pyosdp_pd_set_event_completion_callback,
	  METH_VARARGS, pyosdp_pd_set_event_completion_callback_doc },
	{ "submit_event", (PyCFunction)pyosdp_pd_submit_event,
	  METH_VARARGS, pyosdp_pd_submit_event_doc },
	{ "is_sc_active", (PyCFunction)pyosdp_pd_is_sc_active,
	  METH_NOARGS, pyosdp_pd_is_sc_active_doc },
	{ "is_online", (PyCFunction)pyosdp_pd_is_online,
	  METH_NOARGS, pyosdp_pd_is_online_doc },
	{ "flush_events", (PyCFunction)pyosdp_pd_flush_events,
	  METH_NOARGS, pyosdp_pd_flush_events_doc },
	{ NULL, NULL, 0, NULL } /* Sentinel */
};

static PyMemberDef pyosdp_pd_tp_members[] = {
	{ NULL } /* Sentinel */
};

PyTypeObject PeripheralDeviceTypeObject = {
	PyVarObject_HEAD_INIT(NULL, 0).tp_name = "PeripheralDevice",
	.tp_basicsize = sizeof(pyosdp_pd_t),
	.tp_itemsize = 0,
	.tp_dealloc = (destructor)pyosdp_pd_tp_dealloc,
	.tp_repr = pyosdp_pd_tp_repr,
	.tp_str = pyosdp_pd_tp_str,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc = pyosdp_pd_tp_init_doc,
	.tp_traverse = (traverseproc)pyosdp_pd_tp_traverse,
	.tp_clear = (inquiry)pyosdp_pd_tp_clear,
	.tp_methods = pyosdp_pd_tp_methods,
	.tp_members = pyosdp_pd_tp_members,
	.tp_getset = pyosdp_pd_tp_getset,
	.tp_init = (initproc)pyosdp_pd_tp_init,
	.tp_new = pyosdp_pd_tp_new,
	.tp_base = &OSDPBaseType,
};

int pyosdp_add_type_pd(PyObject *module)
{
	return pyosdp_module_add_type(module, "PeripheralDevice",
				      &PeripheralDeviceTypeObject);
}
