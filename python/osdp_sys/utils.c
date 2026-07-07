/*
 * Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include "module.h"

#if PY_VERSION_HEX < 0x030c0000
static PyObject *PyErr_GetRaisedException(void)
{
	PyObject *type, *value, *tb;
	PyErr_Fetch(&type, &value, &tb);
	PyErr_NormalizeException(&type, &value, &tb);
	if (tb != NULL) {
		PyException_SetTraceback(value, tb);
		Py_DECREF(tb);
	}
	Py_XDECREF(type);
	return value;
}

static void PyErr_SetRaisedException(PyObject *exc)
{
	PyObject *type = (PyObject *)Py_TYPE(exc);
	Py_INCREF(type);
	PyErr_Restore(type, exc, NULL);
}
#endif

int pyosdp_dict_add_bool(PyObject *dict, const char *key, bool val)
{
	int ret;
	PyObject *val_obj;

	val_obj = val ? Py_True : Py_False;

	Py_INCREF(val_obj);
	ret = PyDict_SetItemString(dict, key, val_obj);
	Py_DECREF(val_obj);
	return ret;
}

int pyosdp_dict_add_int(PyObject *dict, const char *key, int val)
{
	int ret;
	PyObject *val_obj;

	val_obj = PyLong_FromLong((long)val);
	if (val_obj == NULL)
		return -1;
	ret = PyDict_SetItemString(dict, key, val_obj);
	Py_DECREF(val_obj);
	return ret;
}

int pyosdp_dict_add_str(PyObject *dict, const char *key, const char *val)
{
	int ret;
	PyObject *val_obj;

	val_obj = PyUnicode_FromString(val);
	if (val_obj == NULL)
		return -1;
	ret = PyDict_SetItemString(dict, key, val_obj);
	Py_DECREF(val_obj);
	return ret;
}

int pyosdp_dict_add_bytes(PyObject *dict, const char *key, const uint8_t *data,
			  int len)
{
	int ret;
	PyObject *obj;

	obj = Py_BuildValue("y#", data, len);
	if (obj == NULL)
		return -1;
	ret = PyDict_SetItemString(dict, key, obj);
	Py_DECREF(obj);
	return ret;
}

int pyosdp_module_add_type(PyObject *module, const char *name,
			   PyTypeObject *type)
{
	if (PyType_Ready(type)) {
		return -1;
	}
	Py_INCREF(type);
	if (PyModule_AddObject(module, name, (PyObject *)type)) {
		Py_DECREF(type);
		return -1;
	}
	return 0;
}

int pyosdp_parse_int(PyObject *obj, int *res)
{
	PyObject *tmp;

	/* Check if obj is numeric */
	if (PyNumber_Check(obj) != 1) {
		PyErr_SetString(PyExc_TypeError, "Expected number");
		return -1;
	}

	tmp = PyNumber_Long(obj);
	*res = (int)PyLong_AsUnsignedLong(tmp);
	Py_DECREF(tmp);
	return 0;
}

int pyosdp_parse_bool(PyObject *obj, bool *res)
{
	/* Check if obj is boolean */
	if (PyBool_Check(obj) != 1) {
		PyErr_SetString(PyExc_TypeError, "Expected boolean");
		return -1;
	}

	*res = PyObject_IsTrue(obj);
	return 0;
}

/* NOTE: Caller must free the returned string with free() */
int pyosdp_parse_str(PyObject *obj, char **str)
{
	char *s;
	PyObject *str_ref;

	str_ref = PyUnicode_AsEncodedString(obj, "UTF-8", "strict");
	if (str_ref == NULL) {
		PyErr_SetString(PyExc_TypeError, "Expected string");
		return -1;
	}

	s = PyBytes_AsString(str_ref);
	if (s == NULL) {
		PyErr_SetString(PyExc_TypeError, "Expected string");
		Py_DECREF(str_ref);
		return -1;
	}
	*str = strdup(s);
	if (*str == NULL) {
		PyErr_SetString(PyExc_MemoryError, "String allocation failed");
		Py_DECREF(str_ref);
		return -1;
	}
	Py_DECREF(str_ref);
	return 0;
}

int pyosdp_parse_bytes(PyObject *obj, uint8_t **data, int *length, bool allow_empty)
{
	Py_ssize_t len;
	uint8_t *buf;

	if (!obj || !PyArg_Parse(obj, "y#", &buf, &len))
		return -1;

	if (buf == NULL || (!allow_empty && len == 0)) {
		PyErr_Format(PyExc_ValueError, "Unable to extact data bytes");
		return -1;
	}
	*data = buf;
	*length = len;
	return 0;
}

int pyosdp_dict_get_str(PyObject *dict, const char *key, char **str)
{
	PyObject *tmp;

	if (!PyDict_Check(dict)) {
		PyErr_SetString(PyExc_TypeError, "arg is not a dict");
		return -1;
	}

	tmp = PyDict_GetItemString(dict, key);
	if (tmp == NULL) {
		PyErr_Format(PyExc_KeyError,
			     "Key: '%s' of type: string expected", key);
		return -1;
	}

	return pyosdp_parse_str(tmp, str);
}

int pyosdp_dict_get_int(PyObject *dict, const char *key, int *res)
{
	PyObject *tmp;

	if (!PyDict_Check(dict)) {
		PyErr_SetString(PyExc_TypeError, "arg is not a dict");
		return -1;
	}

	tmp = PyDict_GetItemString(dict, key);
	if (tmp == NULL) {
		PyErr_Format(PyExc_KeyError, "Key: '%s' of type: int expected",
			     key);
		return -1;
	}

	return pyosdp_parse_int(tmp, res);
}

int pyosdp_dict_get_bool(PyObject *dict, const char *key, bool *res)
{
	PyObject *tmp;

	if (!PyDict_Check(dict)) {
		PyErr_SetString(PyExc_TypeError, "arg is not a dict");
		return -1;
	}

	tmp = PyDict_GetItemString(dict, key);
	if (tmp == NULL)
		return 1;

	return pyosdp_parse_bool(tmp, res);
}

int pyosdp_dict_get_bytes(PyObject *dict, const char *key, uint8_t **data,
			  int *length)
{
	PyObject *obj;

	if (!PyDict_Check(dict)) {
		PyErr_SetString(PyExc_TypeError, "arg is not a dict");
		return -1;
	}

	obj = PyDict_GetItemString(dict, key);
	if (obj == NULL) {
		PyErr_Format(PyExc_KeyError,
			     "Key: '%s' of type: bytes expected", key);
		return -1;
	}

	return pyosdp_parse_bytes(obj, data, length, false);
}

int pyosdp_dict_get_bytes_allow_empty(PyObject *dict, const char *key, uint8_t **data,
			  int *length)
{
	PyObject *obj;

	if (!PyDict_Check(dict)) {
		PyErr_SetString(PyExc_TypeError, "arg is not a dict");
		return -1;
	}

	obj = PyDict_GetItemString(dict, key);
	if (obj == NULL) {
		PyErr_Format(PyExc_KeyError,
			     "Key: '%s' of type: bytes expected", key);
		return -1;
	}

	return pyosdp_parse_bytes(obj, data, length, true);
}

int pyosdp_dict_get_object(PyObject *dict, const char *key, PyObject **obj)
{
	if (!PyDict_Check(dict)) {
		PyErr_SetString(PyExc_TypeError, "arg is not a dict");
		return -1;
	}

	*obj = PyDict_GetItemString(dict, key);
	if (*obj == NULL)
		return -1;

	return 0;
}

/* --- Channel --- */

static int channel_read_callback(void *data, uint8_t *buf, int maxlen)
{
	Py_ssize_t len;
	PyObject *channel = data;
	uint8_t *tmp;

	PyObject *result = PyObject_CallMethod(channel, "read", "I", maxlen);

	if (!result || !PyBytes_Check(result))
		return -1;

	PyArg_Parse(result, "y#", &tmp, &len);
	if (len <= maxlen) {
		memcpy(buf, tmp, len);
	} else {
		PyErr_SetString(PyExc_TypeError,
				"read callback maxlen not respected");
		len = -1;
	}
	Py_DECREF(result);
	return len;
}

static int channel_write_callback(void *data, uint8_t *buf, int len)
{
	PyObject *channel = data;
	PyObject *byte_array;

	byte_array = Py_BuildValue("y#", buf, len);
	if (byte_array == NULL)
		return -1;

	PyObject *result = PyObject_CallMethod(channel, "write", "O", byte_array);
	if (!result || !PyLong_Check(result)) {
		Py_DECREF(byte_array);
		Py_XDECREF(result);
		return -1;
	}

	len = (int)PyLong_AsLong(result);
	Py_DECREF(byte_array);
	Py_DECREF(result);
	return len;
}

static void channel_flush_callback(void *data)
{
	PyObject *channel = data;
	PyObject *result;

	result = PyObject_CallMethod(channel, "flush", NULL);
	Py_XDECREF(result);
}

void pyosdp_get_channel(PyObject *channel, struct osdp_channel *ops)
{
	ops->send = channel_write_callback;
	ops->flush = channel_flush_callback;
	ops->data = channel;
	ops->recv = channel_read_callback;

	Py_INCREF(channel);
}

void pyosdp_add_error_context(PyObject *exc_type, const char *format, ...)
{
	PyObject *cause_exc, *new_exc;
	va_list args;
	char message[512];

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	cause_exc = PyErr_GetRaisedException();
	if (cause_exc != NULL) {
		PyErr_SetString(exc_type, message);
		new_exc = PyErr_GetRaisedException();
		if (new_exc != NULL) {
			PyException_SetCause(new_exc, cause_exc);
			PyErr_SetRaisedException(new_exc);
		}
	} else {
		PyErr_SetString(exc_type, message);
	}
}
