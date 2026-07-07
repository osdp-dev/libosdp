/*
 * Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "module.h"

/* ------------------------------- */
/*            COMMANDS             */
/* ------------------------------- */

static int pyosdp_make_dict_cmd_output(PyObject *obj, struct osdp_cmd *cmd)
{
	if (pyosdp_dict_add_int(obj, "control_code", cmd->output.control_code))
		return -1;
	if (pyosdp_dict_add_int(obj, "output_no", cmd->output.output_no))
		return -1;
	if (pyosdp_dict_add_int(obj, "timer_count", cmd->output.timer_count))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_output(struct osdp_cmd *p, PyObject *dict)
{
	struct osdp_cmd_output *cmd = &p->output;
	int output_no, control_code, timer_count;

	if (pyosdp_dict_get_int(dict, "output_no", &output_no))
		return -1;

	if (pyosdp_dict_get_int(dict, "control_code", &control_code))
		return -1;

	if (pyosdp_dict_get_int(dict, "timer_count", &timer_count))
		return -1;

	cmd->output_no = (uint8_t)output_no;
	cmd->control_code = (uint8_t)control_code;
	cmd->timer_count = (uint16_t)timer_count;
	return 0;
}

static int pyosdp_make_dict_cmd_led(PyObject *obj, struct osdp_cmd *cmd)
{
	bool is_temporary = false;
	bool cancel_temporary = false;
	struct osdp_cmd_led_params *p = &cmd->led.permanent;

	if (cmd->led.temporary.control_code == 1 &&
	    cmd->led.permanent.control_code != 0) {
		cancel_temporary = true;
	} else if (cmd->led.temporary.control_code) {
		p = &cmd->led.temporary;
		is_temporary = true;
	}
	if (pyosdp_dict_add_bool(obj, "temporary", is_temporary))
		return -1;
	if (pyosdp_dict_add_int(obj, "led_number", cmd->led.led_number))
		return -1;
	if (pyosdp_dict_add_int(obj, "reader", cmd->led.reader))
		return -1;
	if (pyosdp_dict_add_int(obj, "control_code", p->control_code))
		return -1;
	if (pyosdp_dict_add_int(obj, "off_color", p->off_color))
		return -1;
	if (pyosdp_dict_add_int(obj, "on_color", p->on_color))
		return -1;
	if (pyosdp_dict_add_int(obj, "on_count", p->on_count))
		return -1;
	if (pyosdp_dict_add_int(obj, "off_count", p->off_count))
		return -1;
	if (is_temporary) {
		if (pyosdp_dict_add_int(obj, "timer_count", p->timer_count))
			return -1;
	}
	if (cancel_temporary) {
		if (pyosdp_dict_add_bool(obj, "cancel_temporary", cancel_temporary))
			return -1;
	}
	return 0;
}

static int pyosdp_make_struct_cmd_led(struct osdp_cmd *p, PyObject *dict)
{
	int led_number, reader, off_color, on_color, off_count, on_count,
		timer_count, control_code;
	bool is_temporary = false;
	struct osdp_cmd_led *cmd = &p->led;
	struct osdp_cmd_led_params *params = &cmd->permanent;

	if (pyosdp_dict_get_int(dict, "led_number", &led_number))
		return -1;

	if (pyosdp_dict_get_int(dict, "reader", &reader))
		return -1;

	if (pyosdp_dict_get_bool(dict, "temporary", &is_temporary) < 0)
		return -1;

	if (pyosdp_dict_get_int(dict, "control_code", &control_code))
		return -1;

	if (pyosdp_dict_get_int(dict, "off_color", &off_color))
		return -1;

	if (pyosdp_dict_get_int(dict, "on_color", &on_color))
		return -1;

	if (pyosdp_dict_get_int(dict, "off_count", &off_count))
		return -1;

	if (pyosdp_dict_get_int(dict, "on_count", &on_count))
		return -1;

	if (is_temporary) {
		params = &cmd->temporary;
		if (pyosdp_dict_get_int(dict, "timer_count", &timer_count))
			return -1;
		params->timer_count = (uint8_t)timer_count;
	}

	cmd->led_number = (uint8_t)led_number;
	cmd->reader = (uint8_t)reader;
	params->control_code = (uint8_t)control_code;
	params->off_color = (uint8_t)off_color;
	params->on_color = (uint8_t)on_color;
	params->on_count = (uint8_t)on_count;
	params->off_count = (uint8_t)off_count;
	return 0;
}

static int pyosdp_make_dict_cmd_buzzer(PyObject *obj, struct osdp_cmd *cmd)
{
	if (pyosdp_dict_add_int(obj, "control_code", cmd->buzzer.control_code))
		return -1;
	if (pyosdp_dict_add_int(obj, "on_count", cmd->buzzer.on_count))
		return -1;
	if (pyosdp_dict_add_int(obj, "off_count", cmd->buzzer.off_count))
		return -1;
	if (pyosdp_dict_add_int(obj, "reader", cmd->buzzer.reader))
		return -1;
	if (pyosdp_dict_add_int(obj, "rep_count", cmd->buzzer.rep_count))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_buzzer(struct osdp_cmd *p, PyObject *dict)
{
	struct osdp_cmd_buzzer *cmd = &p->buzzer;
	int reader, on_count, off_count, rep_count, control_code;

	if (pyosdp_dict_get_int(dict, "reader", &reader))
		return -1;

	if (pyosdp_dict_get_int(dict, "on_count", &on_count))
		return -1;

	if (pyosdp_dict_get_int(dict, "off_count", &off_count))
		return -1;

	if (pyosdp_dict_get_int(dict, "rep_count", &rep_count))
		return -1;

	if (pyosdp_dict_get_int(dict, "control_code", &control_code))
		return -1;

	cmd->reader = (uint8_t)reader;
	cmd->on_count = (uint8_t)on_count;
	cmd->off_count = (uint8_t)off_count;
	cmd->rep_count = (uint8_t)rep_count;
	cmd->control_code = (uint8_t)control_code;
	return 0;
}

static int pyosdp_make_dict_cmd_text(PyObject *obj, struct osdp_cmd *cmd)
{
	char buf[64];

	if (pyosdp_dict_add_int(obj, "control_code", cmd->text.control_code))
		return -1;
	if (pyosdp_dict_add_int(obj, "temp_time", cmd->text.temp_time))
		return -1;
	if (pyosdp_dict_add_int(obj, "offset_col", cmd->text.offset_col))
		return -1;
	if (pyosdp_dict_add_int(obj, "offset_row", cmd->text.offset_row))
		return -1;
	if (pyosdp_dict_add_int(obj, "reader", cmd->text.reader))
		return -1;
	if (cmd->text.length > (sizeof(buf) - 1))
		return -1;
	memcpy(buf, cmd->text.data, cmd->text.length);
	buf[cmd->text.length] = '\0';
	if (pyosdp_dict_add_str(obj, "data", buf))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_text(struct osdp_cmd *p, PyObject *dict)
{
	int ret = -1;
	char *data = NULL;
	struct osdp_cmd_text *cmd = &p->text;
	int length, reader, control_code, offset_row, offset_col, temp_time;

	if (pyosdp_dict_get_str(dict, "data", &data))
		goto exit;

	length = strlen(data);
	if (length > OSDP_CMD_TEXT_MAX_LEN)
		goto exit;

	if (pyosdp_dict_get_int(dict, "reader", &reader))
		goto exit;

	if (pyosdp_dict_get_int(dict, "control_code", &control_code))
		goto exit;

	if (pyosdp_dict_get_int(dict, "offset_col", &offset_col))
		goto exit;

	if (pyosdp_dict_get_int(dict, "offset_row", &offset_row))
		goto exit;

	if (pyosdp_dict_get_int(dict, "temp_time", &temp_time))
		goto exit;

	cmd->reader = (uint8_t)reader;
	cmd->control_code = (uint8_t)control_code;
	cmd->length = (uint8_t)length;
	cmd->offset_col = (uint8_t)offset_col;
	cmd->offset_row = (uint8_t)offset_row;
	cmd->temp_time = (uint8_t)temp_time;
	memcpy(cmd->data, data, length);
	ret = 0;
exit:
	free(data);
	return ret;
}

static int pyosdp_make_dict_cmd_keyset(PyObject *obj, struct osdp_cmd *cmd)
{
	if (pyosdp_dict_add_int(obj, "type", cmd->keyset.type))
		return -1;
	if (cmd->keyset.length > 16)
		return -1;
	if (pyosdp_dict_add_bytes(obj, "data", cmd->keyset.data, cmd->keyset.length))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_keyset(struct osdp_cmd *p, PyObject *dict)
{
	int type, len;
	struct osdp_cmd_keyset *cmd = &p->keyset;
	uint8_t *buf;

	if (pyosdp_dict_get_int(dict, "type", &type))
		return -1;

	if (pyosdp_dict_get_bytes(dict, "data", &buf, &len))
		return -1;

	cmd->type = (uint8_t)type;
	if (len > OSDP_CMD_KEYSET_KEY_MAX_LEN)
		return -1;
	cmd->length = len;
	memcpy(cmd->data, buf, len);
	return 0;
}

static int pyosdp_make_dict_cmd_comset(PyObject *obj, struct osdp_cmd *cmd)
{
	if (pyosdp_dict_add_int(obj, "address", cmd->comset.address))
		return -1;
	if (pyosdp_dict_add_int(obj, "baud_rate", cmd->comset.baud_rate))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_comset(struct osdp_cmd *p, PyObject *dict)
{
	struct osdp_cmd_comset *cmd = &p->comset;
	int address, baud_rate;

	if (pyosdp_dict_get_int(dict, "address", &address))
		return -1;

	if (pyosdp_dict_get_int(dict, "baud_rate", &baud_rate))
		return -1;

	cmd->address = (uint8_t)address;
	cmd->baud_rate = (uint32_t)baud_rate;
	return 0;
}

static int pyosdp_make_dict_cmd_mfg(PyObject *obj, struct osdp_cmd *cmd)
{
	if (pyosdp_dict_add_int(obj, "vendor_code", cmd->mfg.vendor_code))
		return -1;
	if (pyosdp_dict_add_bytes(obj, "data", cmd->mfg.data, cmd->mfg.length))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_mfg(struct osdp_cmd *p, PyObject *dict)
{
	int i, data_length;
	struct osdp_cmd_mfg *cmd = &p->mfg;
	uint8_t *data_bytes;
	int vendor_code;

	if (pyosdp_dict_get_int(dict, "vendor_code", &vendor_code))
		return -1;

	if (pyosdp_dict_get_bytes(dict, "data", &data_bytes, &data_length))
		return -1;

	cmd->vendor_code = (uint32_t)vendor_code;
	cmd->length = data_length;
	for (i = 0; i < cmd->length; i++)
		cmd->data[i] = data_bytes[i];
	return 0;
}

static int pyosdp_make_dict_cmd_file_tx(PyObject *obj, struct osdp_cmd *cmd)
{
	if (pyosdp_dict_add_int(obj, "flags", cmd->file_tx.flags))
		return -1;
	if (pyosdp_dict_add_int(obj, "id", cmd->file_tx.id))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_file_tx(struct osdp_cmd *p, PyObject *dict)
{
	int id, flags;
	struct osdp_cmd_file_tx *cmd = &p->file_tx;

	if (pyosdp_dict_get_int(dict, "id", &id))
		return -1;

	if (pyosdp_dict_get_int(dict, "flags", &flags))
		return -1;

	cmd->id = id;
	cmd->flags = flags;
	return 0;
}

static int pyosdp_make_dict_cmd_status(PyObject *obj, struct osdp_cmd *cmd)
{
	if (pyosdp_dict_add_int(obj, "type", cmd->status.type))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_status(struct osdp_cmd *p, PyObject *dict)
{
	int type;
	struct osdp_status_report *cmd = &p->status;

	if (pyosdp_dict_get_int(dict, "type", &type))
		return -1;

	cmd->type = type;
	return 0;
}

static int pyosdp_make_dict_cmd_notif(PyObject *obj, struct osdp_cmd *cmd)
{
	if (pyosdp_dict_add_int(obj, "type", cmd->notif.type))
		return -1;
	if (pyosdp_dict_add_int(obj, "arg0", cmd->notif.arg0))
		return -1;
	if (pyosdp_dict_add_int(obj, "arg1", cmd->notif.arg1))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_notif(struct osdp_cmd *p, PyObject *dict)
{
	int type, arg0, arg1;
	struct osdp_notification *cmd = &p->notif;

	if (pyosdp_dict_get_int(dict, "type", &type))
		return -1;
	if (pyosdp_dict_get_int(dict, "arg0", &arg0))
		return -1;
	if (pyosdp_dict_get_int(dict, "arg1", &arg1))
		return -1;

	cmd->type = type;
	cmd->arg0 = arg0;
	cmd->arg1 = arg1;
	return 0;
}

/* ------------------------------- */
/*             EVENTS              */
/* ------------------------------- */

static int pyosdp_make_dict_event_cardread(PyObject *obj, struct osdp_event *event)
{
	int tmp;

	if (pyosdp_dict_add_int(obj, "reader_no",
				event->cardread.reader_no))
		return -1;
	if (pyosdp_dict_add_int(obj, "format", event->cardread.format))
		return -1;
	if (pyosdp_dict_add_int(obj, "direction",
				event->cardread.direction))
		return -1;
	tmp = event->cardread.length;
	if (event->cardread.format == OSDP_CARD_FMT_RAW_UNSPECIFIED ||
		event->cardread.format == OSDP_CARD_FMT_RAW_WIEGAND) {
		if (pyosdp_dict_add_int(obj, "length", tmp))
			return -1;
		tmp = (tmp + 7) / 8; // bits to bytes
	}
	if (pyosdp_dict_add_bytes(obj, "data", event->cardread.data, tmp))
		return -1;
	return 0;
}

static int pyosdp_make_struct_event_cardread(struct osdp_event *p,
					     PyObject *dict)
{
	int i, data_length, reader_no, format, direction, len_bytes;
	struct osdp_event_cardread *ev = &p->cardread;
	uint8_t *data_bytes;

	if (pyosdp_dict_get_int(dict, "reader_no", &reader_no))
		return -1;

	if (pyosdp_dict_get_int(dict, "format", &format))
		return -1;

	if (pyosdp_dict_get_int(dict, "direction", &direction))
		return -1;

	if (pyosdp_dict_get_bytes(dict, "data", &data_bytes, &data_length))
		return -1;

	if (format == OSDP_CARD_FMT_RAW_WIEGAND ||
	    format == OSDP_CARD_FMT_RAW_UNSPECIFIED) {
		if (pyosdp_dict_get_int(dict, "length", &data_length))
			return -1;
		len_bytes = (data_length + 7) / 8; // bits to bytes
	} else {
		len_bytes = data_length;
	}

	if (len_bytes > OSDP_EVENT_CARDREAD_MAX_DATALEN) {
		PyErr_Format(PyExc_ValueError, "Data bytes too long");
		return -1;
	}

	ev->reader_no = (uint8_t)reader_no;
	ev->format = (uint8_t)format;
	ev->direction = (uint8_t)direction;
	ev->length = data_length;
	for (i = 0; i < len_bytes; i++)
		ev->data[i] = data_bytes[i];
	return 0;
}

static int pyosdp_make_dict_event_keypress(PyObject *obj, struct osdp_event *event)
{
	if (pyosdp_dict_add_int(obj, "reader_no",
				event->keypress.reader_no))
		return -1;
	if (pyosdp_dict_add_bytes(obj, "data", event->keypress.data,
					event->keypress.length))
		return -1;
	return 0;
}

static int pyosdp_make_struct_event_keypress(struct osdp_event *p, PyObject *dict)
{
	int i, data_length, reader_no;
	struct osdp_event_keypress *ev = &p->keypress;
	uint8_t *data_bytes;

	if (pyosdp_dict_get_int(dict, "reader_no", &reader_no))
		return -1;

	if (pyosdp_dict_get_bytes(dict, "data", &data_bytes, &data_length))
		return -1;

	ev->reader_no = (uint8_t)reader_no;
	ev->length = data_length;
	for (i = 0; i < ev->length; i++)
		ev->data[i] = data_bytes[i];
	return 0;
}

static int pyosdp_make_dict_event_mfg_reply(PyObject *obj, struct osdp_event *event)
{
	if (pyosdp_dict_add_int(obj, "vendor_code", event->mfgrep.vendor_code))
		return -1;
	if (pyosdp_dict_add_bytes(obj, "data", event->mfgrep.data, event->mfgrep.length))
		return -1;
	return 0;
}

static int pyosdp_make_struct_event_mfg_reply(struct osdp_event *p,
					      PyObject *dict)
{
	int i, data_length, vendor_code, command;
	struct osdp_event_mfgrep *ev = &p->mfgrep;
	uint8_t *data_bytes;

	if (pyosdp_dict_get_int(dict, "vendor_code", &vendor_code))
		return -1;

	if (pyosdp_dict_get_bytes(dict, "data", &data_bytes, &data_length))
		return -1;

	ev->vendor_code = (uint32_t)vendor_code;
	ev->length = data_length;
	for (i = 0; i < ev->length; i++)
		ev->data[i] = data_bytes[i];
	return 0;
}

static int pyosdp_make_dict_event_status(PyObject *obj, struct osdp_event *event)
{
	if (pyosdp_dict_add_int(obj, "type", event->status.type))
		return -1;
	if (pyosdp_dict_add_bytes(obj, "report", event->status.report, event->status.nr_entries))
		return -1;
	return 0;
}

static int pyosdp_make_struct_event_status(struct osdp_event *p, PyObject *dict)
{
	int type, nr_entries;
	uint8_t *report;
	struct osdp_status_report *ev = &p->status;

	if (pyosdp_dict_get_int(dict, "type", &type))
		return -1;

	if (pyosdp_dict_get_bytes(dict, "report", &report, &nr_entries))
		return -1;

	if (nr_entries > OSDP_STATUS_REPORT_MAX_LEN)
		return -1;

	ev->type = type;
	ev->nr_entries = nr_entries;
	memcpy(ev->report, report, nr_entries);
	return 0;
}

static int pyosdp_make_dict_event_notif(PyObject *obj, struct osdp_event *event)
{
	if (pyosdp_dict_add_int(obj, "type", event->notif.type))
		return -1;
	if (pyosdp_dict_add_int(obj, "arg0", event->notif.arg0))
		return -1;
	if (pyosdp_dict_add_int(obj, "arg1", event->notif.arg1))
		return -1;
	return 0;
}

static int pyosdp_make_struct_event_notif(struct osdp_event *p, PyObject *dict)
{
	int type, arg0, arg1;
	struct osdp_notification *ev = &p->notif;

	if (pyosdp_dict_get_int(dict, "type", &type))
		return -1;

	if (pyosdp_dict_get_int(dict, "arg0", &arg0))
		return -1;

	if (pyosdp_dict_get_int(dict, "arg1", &arg1))
		return -1;

	ev->type = type;
	ev->arg0 = arg0;
	ev->arg1 = arg1;
	return 0;
}

static struct {
	int (*dict_to_struct)(struct osdp_cmd *, PyObject *);
	int (*struct_to_dict)(PyObject *, struct osdp_cmd *);
} command_translator[OSDP_CMD_SENTINEL] = {
	[OSDP_CMD_OUTPUT] = {
		.dict_to_struct = pyosdp_make_struct_cmd_output,
		.struct_to_dict = pyosdp_make_dict_cmd_output,
	},
	[OSDP_CMD_LED] = {
		.dict_to_struct = pyosdp_make_struct_cmd_led,
		.struct_to_dict = pyosdp_make_dict_cmd_led,
	},
	[OSDP_CMD_BUZZER] = {
		.dict_to_struct = pyosdp_make_struct_cmd_buzzer,
		.struct_to_dict = pyosdp_make_dict_cmd_buzzer,
	},
	[OSDP_CMD_TEXT] = {
		.dict_to_struct = pyosdp_make_struct_cmd_text,
		.struct_to_dict = pyosdp_make_dict_cmd_text,
	},
	[OSDP_CMD_KEYSET] = {
		.dict_to_struct = pyosdp_make_struct_cmd_keyset,
		.struct_to_dict = pyosdp_make_dict_cmd_keyset,
	},
	[OSDP_CMD_COMSET] = {
		.dict_to_struct = pyosdp_make_struct_cmd_comset,
		.struct_to_dict = pyosdp_make_dict_cmd_comset,
	},
	[OSDP_CMD_COMSET_DONE] = {
		.dict_to_struct = pyosdp_make_struct_cmd_comset,
		.struct_to_dict = pyosdp_make_dict_cmd_comset,
	},
	[OSDP_CMD_MFG] = {
		.dict_to_struct = pyosdp_make_struct_cmd_mfg,
		.struct_to_dict = pyosdp_make_dict_cmd_mfg,
	},
	[OSDP_CMD_FILE_TX] = {
		.dict_to_struct = pyosdp_make_struct_cmd_file_tx,
		.struct_to_dict = pyosdp_make_dict_cmd_file_tx,
	},
	[OSDP_CMD_STATUS] = {
		.dict_to_struct = pyosdp_make_struct_cmd_status,
		.struct_to_dict = pyosdp_make_dict_cmd_status,
	},
	[OSDP_CMD_NOTIFICATION] = {
		.dict_to_struct = pyosdp_make_struct_cmd_notif,
		.struct_to_dict = pyosdp_make_dict_cmd_notif,
	},
};

static struct {
	int (*struct_to_dict)(PyObject *, struct osdp_event *);
	int (*dict_to_struct)(struct osdp_event *, PyObject *);
} event_translator[OSDP_EVENT_SENTINEL] = {
	[OSDP_EVENT_CARDREAD] = {
		.struct_to_dict = pyosdp_make_dict_event_cardread,
		.dict_to_struct = pyosdp_make_struct_event_cardread,
	},
	[OSDP_EVENT_KEYPRESS] = {
		.struct_to_dict = pyosdp_make_dict_event_keypress,
		.dict_to_struct = pyosdp_make_struct_event_keypress,
	},
	[OSDP_EVENT_MFGREP] = {
		.struct_to_dict = pyosdp_make_dict_event_mfg_reply,
		.dict_to_struct = pyosdp_make_struct_event_mfg_reply,
	},
	[OSDP_EVENT_STATUS] = {
		.struct_to_dict = pyosdp_make_dict_event_status,
		.dict_to_struct = pyosdp_make_struct_event_status,
	},
	[OSDP_EVENT_NOTIFICATION] = {
		.struct_to_dict = pyosdp_make_dict_event_notif,
		.dict_to_struct = pyosdp_make_struct_event_notif,
	},
};

/* --- Exposed Methods --- */

int pyosdp_make_struct_cmd(struct osdp_cmd *cmd, PyObject *dict)
{
	int cmd_id;

	if (pyosdp_dict_get_int(dict, "command", &cmd_id))
		return -1;
	if (cmd_id <= 0 || cmd_id >= OSDP_CMD_SENTINEL)
		return -1;
	if (command_translator[cmd_id].dict_to_struct(cmd, dict))
		return -1;

	cmd->id = cmd_id;
	return 0;
}

int pyosdp_make_dict_cmd(PyObject **dict, struct osdp_cmd *cmd)
{
	PyObject *obj;

	if (cmd->id <= 0 || cmd->id >= OSDP_CMD_SENTINEL)
		return -1;

	obj = PyDict_New();
	if (obj == NULL)
		return -1;

	if (pyosdp_dict_add_int(obj, "command", cmd->id)) {
		Py_DECREF(obj);
		return -1;
	}

	if (command_translator[cmd->id].struct_to_dict(obj, cmd)) {
		Py_DECREF(obj);
		return -1;
	}

	*dict = obj;
	return 0;
}

int pyosdp_make_struct_event(struct osdp_event *event, PyObject *dict)
{
	int event_type;

	if (pyosdp_dict_get_int(dict, "event", &event_type))
		return -1;

	if (event_type <= 0 || event_type >= OSDP_EVENT_SENTINEL)
		return -1;

	event->type = event_type;
	return event_translator[event_type].dict_to_struct(event, dict);
}

int pyosdp_make_dict_event(PyObject **dict, struct osdp_event *event)
{
	PyObject *obj;

	if (event->type < OSDP_EVENT_CARDREAD || event->type >= OSDP_EVENT_SENTINEL)
		return -1;

	obj = PyDict_New();
	if (obj == NULL)
		return -1;

	if (pyosdp_dict_add_int(obj, "event", event->type)) {
		Py_DECREF(obj);
		return -1;
	}

	if (event_translator[event->type].struct_to_dict(obj, event)) {
		Py_DECREF(obj);
		return -1;
	}

	*dict = obj;
	return 0;
}

PyObject *pyosdp_make_dict_pd_id(struct osdp_pd_id *pd_id)
{
	PyObject *obj = PyDict_New();
	if (obj == NULL)
		return NULL;

	if (pyosdp_dict_add_int(obj, "version", pd_id->version) ||
	    pyosdp_dict_add_int(obj, "model", pd_id->model) ||
	    pyosdp_dict_add_int(obj, "vendor_code", pd_id->vendor_code) ||
	    pyosdp_dict_add_int(obj, "serial_number", pd_id->serial_number) ||
	    pyosdp_dict_add_int(obj, "firmware_version", pd_id->firmware_version)) {
		Py_DECREF(obj);
		return NULL;
	}

	return obj;
}
