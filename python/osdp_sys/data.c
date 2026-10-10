/*
 * Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "module.h"

/*
 * Commands and events are described by a table of fields rather than by a pair
 * of hand written translators each. A field says where it lives in the C struct
 * and what it is called in the dict, and one engine walks the table in either
 * direction. Adding a command means adding a table; the conversion, the bounds
 * checks and the error paths come with it.
 */

enum pyosdp_field_kind {
	FIELD_U8,
	FIELD_U16,
	FIELD_U32,
	FIELD_INT,
	FIELD_BOOL,
	FIELD_BYTES, /* a buffer, with its length held in another field */
	FIELD_STR, /* UTF-8 into a buffer, with its length held likewise */
	FIELD_SUBDICT, /* a nested struct, marshalled as a nested dict */
};

struct pyosdp_field {
	const char *key;
	enum pyosdp_field_kind kind;
	size_t offset;

	/* FIELD_BYTES and FIELD_STR */
	size_t len_offset;
	enum pyosdp_field_kind len_kind;
	size_t max_len;
	bool allow_empty;
	bool optional;

	/* FIELD_SUBDICT */
	const struct pyosdp_field *fields;
	size_t present_offset; /* nested block is absent when this byte is 0 */
	bool required; /* nested block is never absent; present_offset unused */
};

#define INT_FIELD(st, member, int_kind)                                        \
	{                                                                      \
		.key = #member,                                                \
		.kind = int_kind,                                              \
		.offset = offsetof(st, member),                                \
	}

#define U8_FIELD(st, member)   INT_FIELD(st, member, FIELD_U8)
#define U16_FIELD(st, member)  INT_FIELD(st, member, FIELD_U16)
#define U32_FIELD(st, member)  INT_FIELD(st, member, FIELD_U32)
#define INT_FIELD_(st, member) INT_FIELD(st, member, FIELD_INT)

#define END_OF_FIELDS { .key = NULL }

/*
 * FIELD_INT reaches the member through an `int *`, so the enum-typed control
 * codes marshal correctly only while an enum is int-sized. Under
 * -fshort-enums (the AAPCS default on bare metal ARM) they shrink to a byte
 * and the write runs past the member into whatever follows it. Say so at
 * compile time rather than corrupting a struct at runtime.
 */
#define ASSERT_INT_SIZED(st, member)                                           \
	_Static_assert(sizeof(((st *)0)->member) == sizeof(int),               \
		       #st "::" #member " must be int-sized for FIELD_INT")

ASSERT_INT_SIZED(struct osdp_cmd_output, control_code);
ASSERT_INT_SIZED(struct osdp_cmd_buzzer, control_code);
ASSERT_INT_SIZED(struct osdp_cmd_text, control_code);
ASSERT_INT_SIZED(struct osdp_status_report, type);
ASSERT_INT_SIZED(struct osdp_notification, type);
ASSERT_INT_SIZED(struct osdp_cmd_led_params, on_color);
ASSERT_INT_SIZED(struct osdp_cmd_led_params, off_color);
ASSERT_INT_SIZED(struct osdp_cmd_bioread, type);
ASSERT_INT_SIZED(struct osdp_cmd_bioread, format);
ASSERT_INT_SIZED(struct osdp_cmd_biomatch, type);
ASSERT_INT_SIZED(struct osdp_cmd_biomatch, format);
ASSERT_INT_SIZED(struct osdp_event_bioreadr, status);
ASSERT_INT_SIZED(struct osdp_event_bioreadr, type);
ASSERT_INT_SIZED(struct osdp_event_biomatchr, status);
ASSERT_INT_SIZED(struct osdp_trs_pin_entry, pin_block.format);
ASSERT_INT_SIZED(struct osdp_trs_card_info, protocol);
ASSERT_INT_SIZED(struct osdp_trs_card_present, status);

/* --- Field access --- */

static int field_get_int(const void *base, size_t offset,
			 enum pyosdp_field_kind kind)
{
	const void *p = (const uint8_t *)base + offset;

	switch (kind) {
	case FIELD_U8:
		return *(const uint8_t *)p;
	case FIELD_U16:
		return *(const uint16_t *)p;
	case FIELD_U32:
		return (int)*(const uint32_t *)p;
	case FIELD_INT:
		return *(const int *)p;
	case FIELD_BOOL:
		return *(const bool *)p;
	default:
		return -1;
	}
}

static void field_set_int(void *base, size_t offset,
			  enum pyosdp_field_kind kind, int val)
{
	void *p = (uint8_t *)base + offset;

	switch (kind) {
	case FIELD_U8:
		*(uint8_t *)p = (uint8_t)val;
		break;
	case FIELD_U16:
		*(uint16_t *)p = (uint16_t)val;
		break;
	case FIELD_U32:
		*(uint32_t *)p = (uint32_t)val;
		break;
	case FIELD_INT:
		*(int *)p = val;
		break;
	case FIELD_BOOL:
		*(bool *)p = val != 0;
		break;
	default:
		break;
	}
}

/* --- The engine --- */

static int pyosdp_fields_to_dict(PyObject *obj, const void *base,
				 const struct pyosdp_field *fields);

static int field_to_dict(PyObject *obj, const void *base,
			 const struct pyosdp_field *f)
{
	const uint8_t *data = (const uint8_t *)base + f->offset;
	PyObject *child;
	char str[64];
	int len;

	switch (f->kind) {
	case FIELD_U8:
	case FIELD_U16:
	case FIELD_U32:
	case FIELD_INT:
	case FIELD_BOOL:
		return pyosdp_dict_add_int(
			obj, f->key, field_get_int(base, f->offset, f->kind));
	case FIELD_BYTES:
		len = field_get_int(base, f->len_offset, f->len_kind);
		if (len < 0 || (size_t)len > f->max_len) {
			PyErr_Format(PyExc_ValueError, "'%s' has a bad length",
				     f->key);
			return -1;
		}
		return pyosdp_dict_add_bytes(obj, f->key, data, len);
	case FIELD_STR:
		len = field_get_int(base, f->len_offset, f->len_kind);
		if (len < 0 || (size_t)len > f->max_len ||
		    (size_t)len >= sizeof(str)) {
			PyErr_Format(PyExc_ValueError, "'%s' has a bad length",
				     f->key);
			return -1;
		}
		memcpy(str, data, len);
		str[len] = '\0';
		return pyosdp_dict_add_str(obj, f->key, str);
	case FIELD_SUBDICT:
		if (!f->required &&
		    field_get_int(data, f->present_offset, FIELD_U8) == 0)
			return 0; /* this block does nothing; leave it out */
		child = PyDict_New();
		if (child == NULL)
			return -1;
		if (pyosdp_fields_to_dict(child, data, f->fields) ||
		    PyDict_SetItemString(obj, f->key, child)) {
			Py_DECREF(child);
			return -1;
		}
		Py_DECREF(child);
		return 0;
	}
	return -1;
}

static int pyosdp_fields_to_dict(PyObject *obj, const void *base,
				 const struct pyosdp_field *fields)
{
	const struct pyosdp_field *f;

	for (f = fields; f->key != NULL; f++) {
		if (field_to_dict(obj, base, f))
			return -1;
	}
	return 0;
}

static int pyosdp_fields_to_struct(void *base, PyObject *dict,
				   const struct pyosdp_field *fields);

static int field_to_struct(void *base, PyObject *dict,
			   const struct pyosdp_field *f)
{
	uint8_t *data = (uint8_t *)base + f->offset;
	PyObject *item;
	uint8_t *buf;
	char *str;
	size_t slen;
	int val, len;

	switch (f->kind) {
	case FIELD_U8:
	case FIELD_U16:
	case FIELD_U32:
	case FIELD_INT:
	case FIELD_BOOL:
		if (pyosdp_dict_get_int(dict, f->key, &val))
			return -1;
		field_set_int(base, f->offset, f->kind, val);
		return 0;
	case FIELD_BYTES:
		item = PyDict_GetItemString(dict, f->key);
		if (item == NULL) {
			if (!f->optional) {
				PyErr_Format(
					PyExc_KeyError,
					"Key: '%s' of type: bytes expected",
					f->key);
				return -1;
			}
			field_set_int(base, f->len_offset, f->len_kind, 0);
			return 0;
		}
		if (pyosdp_parse_bytes(item, &buf, &len, f->allow_empty))
			return -1;
		if ((size_t)len > f->max_len) {
			PyErr_Format(PyExc_ValueError,
				     "'%s' is too long: %d bytes", f->key, len);
			return -1;
		}
		memcpy(data, buf, len);
		field_set_int(base, f->len_offset, f->len_kind, len);
		return 0;
	case FIELD_STR:
		if (pyosdp_dict_get_str(dict, f->key, &str))
			return -1;
		slen = strlen(str);
		if (slen > f->max_len) {
			PyErr_Format(PyExc_ValueError,
				     "'%s' is too long: %zu bytes", f->key,
				     slen);
			free(str);
			return -1;
		}
		memcpy(data, str, slen);
		free(str);
		field_set_int(base, f->len_offset, f->len_kind, (int)slen);
		return 0;
	case FIELD_SUBDICT:
		item = PyDict_GetItemString(dict, f->key);
		if (item == NULL && f->required) {
			PyErr_Format(PyExc_KeyError,
				     "Key: '%s' of type: dict expected", f->key);
			return -1;
		}
		if (item == NULL)
			return 0; /* an absent block stays zeroed: no action */
		if (!PyDict_Check(item)) {
			PyErr_Format(PyExc_TypeError, "'%s' must be a dict",
				     f->key);
			return -1;
		}
		return pyosdp_fields_to_struct(data, item, f->fields);
	}
	return -1;
}

static int pyosdp_fields_to_struct(void *base, PyObject *dict,
				   const struct pyosdp_field *fields)
{
	const struct pyosdp_field *f;

	for (f = fields; f->key != NULL; f++) {
		if (field_to_struct(base, dict, f))
			return -1;
	}
	return 0;
}

/* ------------------------------- */
/*            COMMANDS             */
/* ------------------------------- */

static const struct pyosdp_field cmd_output_fields[] = {
	U8_FIELD(struct osdp_cmd_output, output_no),
	INT_FIELD_(struct osdp_cmd_output, control_code),
	U16_FIELD(struct osdp_cmd_output, timer_count),
	END_OF_FIELDS,
};

/*
 * A LED command carries two independent parameter blocks. A control code of
 * zero means "do nothing" for that block, which is what an absent nested dict
 * marshals to. Only the temporary block has a timer.
 */
static const struct pyosdp_field led_temporary_fields[] = {
	U8_FIELD(struct osdp_cmd_led_params, control_code),
	U8_FIELD(struct osdp_cmd_led_params, on_count),
	U8_FIELD(struct osdp_cmd_led_params, off_count),
	INT_FIELD_(struct osdp_cmd_led_params, on_color),
	INT_FIELD_(struct osdp_cmd_led_params, off_color),
	U16_FIELD(struct osdp_cmd_led_params, timer_count),
	END_OF_FIELDS,
};

static const struct pyosdp_field led_permanent_fields[] = {
	U8_FIELD(struct osdp_cmd_led_params, control_code),
	U8_FIELD(struct osdp_cmd_led_params, on_count),
	U8_FIELD(struct osdp_cmd_led_params, off_count),
	INT_FIELD_(struct osdp_cmd_led_params, on_color),
	INT_FIELD_(struct osdp_cmd_led_params, off_color),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_led_fields[] = {
	U8_FIELD(struct osdp_cmd_led, reader),
	U8_FIELD(struct osdp_cmd_led, led_number),
	{
		.key = "temporary",
		.kind = FIELD_SUBDICT,
		.offset = offsetof(struct osdp_cmd_led, temporary),
		.fields = led_temporary_fields,
		.present_offset =
			offsetof(struct osdp_cmd_led_params, control_code),
	},
	{
		.key = "permanent",
		.kind = FIELD_SUBDICT,
		.offset = offsetof(struct osdp_cmd_led, permanent),
		.fields = led_permanent_fields,
		.present_offset =
			offsetof(struct osdp_cmd_led_params, control_code),
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_buzzer_fields[] = {
	U8_FIELD(struct osdp_cmd_buzzer, reader),
	INT_FIELD_(struct osdp_cmd_buzzer, control_code),
	U8_FIELD(struct osdp_cmd_buzzer, on_count),
	U8_FIELD(struct osdp_cmd_buzzer, off_count),
	U8_FIELD(struct osdp_cmd_buzzer, rep_count),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_text_fields[] = {
	U8_FIELD(struct osdp_cmd_text, reader),
	INT_FIELD_(struct osdp_cmd_text, control_code),
	U8_FIELD(struct osdp_cmd_text, temp_time),
	U8_FIELD(struct osdp_cmd_text, offset_row),
	U8_FIELD(struct osdp_cmd_text, offset_col),
	{
		.key = "data",
		.kind = FIELD_STR,
		.offset = offsetof(struct osdp_cmd_text, data),
		.len_offset = offsetof(struct osdp_cmd_text, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_CMD_TEXT_MAX_LEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_tdset_fields[] = {
	U16_FIELD(struct osdp_cmd_tdset, year),
	U8_FIELD(struct osdp_cmd_tdset, month),
	U8_FIELD(struct osdp_cmd_tdset, day),
	U8_FIELD(struct osdp_cmd_tdset, hour),
	U8_FIELD(struct osdp_cmd_tdset, minute),
	U8_FIELD(struct osdp_cmd_tdset, second),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_keyset_fields[] = {
	U8_FIELD(struct osdp_cmd_keyset, type),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_cmd_keyset, data),
		.len_offset = offsetof(struct osdp_cmd_keyset, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_CMD_KEYSET_KEY_MAX_LEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_comset_fields[] = {
	U8_FIELD(struct osdp_cmd_comset, address),
	U32_FIELD(struct osdp_cmd_comset, baud_rate),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_mfg_fields[] = {
	U32_FIELD(struct osdp_cmd_mfg, vendor_code),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_cmd_mfg, data),
		.len_offset = offsetof(struct osdp_cmd_mfg, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_CMD_MFG_MAX_DATALEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_bioread_fields[] = {
	U8_FIELD(struct osdp_cmd_bioread, reader),
	INT_FIELD_(struct osdp_cmd_bioread, type),
	INT_FIELD_(struct osdp_cmd_bioread, format),
	U8_FIELD(struct osdp_cmd_bioread, quality),
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_biomatch_fields[] = {
	U8_FIELD(struct osdp_cmd_biomatch, reader),
	INT_FIELD_(struct osdp_cmd_biomatch, type),
	INT_FIELD_(struct osdp_cmd_biomatch, format),
	U8_FIELD(struct osdp_cmd_biomatch, quality),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_cmd_biomatch, data),
		.len_offset = offsetof(struct osdp_cmd_biomatch, length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_CMD_BIOMATCH_MAX_TEMPLATE_LEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field cmd_file_tx_fields[] = {
	INT_FIELD_(struct osdp_cmd_file_tx, id),
	U32_FIELD(struct osdp_cmd_file_tx, flags),
	END_OF_FIELDS,
};

/*
 * A CP sends a status command as a query and carries no report; a PD answers by
 * returning one with an entry per input. Hence the report is optional and may
 * legitimately be empty.
 */
static const struct pyosdp_field cmd_status_fields[] = {
	INT_FIELD_(struct osdp_status_report, type),
	{
		.key = "report",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_status_report, report),
		.len_offset = offsetof(struct osdp_status_report, nr_entries),
		.len_kind = FIELD_INT,
		.max_len = OSDP_STATUS_REPORT_MAX_LEN,
		.allow_empty = true,
		.optional = true,
	},
	END_OF_FIELDS,
};

/* GENAUTH/CRAUTH challenge: two u8 fields plus a length-carrying buffer. */
static const struct pyosdp_field cmd_auth_fields[] = {
	U8_FIELD(struct osdp_cmd_auth, algorithm),
	U8_FIELD(struct osdp_cmd_auth, key),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_cmd_auth, data),
		.len_offset = offsetof(struct osdp_cmd_auth, length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_PIV_DATA_MAX_LEN,
	},
	END_OF_FIELDS,
};

/* Reassembled smartcard/PIV reply payload (PIVDATAR/GENAUTHR/CRAUTHR). */
static const struct pyosdp_field event_piv_reply_fields[] = {
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_piv_reply, data),
		.len_offset = offsetof(struct osdp_event_piv_reply, length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_PIV_DATA_MAX_LEN,
	},
	END_OF_FIELDS,
};

/*
 * The PIV object id is a fixed 3-byte array with no length field, which the
 * declarative field kinds cannot express; marshal it by hand as `bytes`.
 */
static int pyosdp_make_dict_cmd_pivdata(PyObject *obj, const void *payload)
{
	const struct osdp_cmd_pivdata *p = payload;

	if (pyosdp_dict_add_bytes(obj, "oid", p->oid, sizeof(p->oid)) ||
	    pyosdp_dict_add_int(obj, "element", p->element) ||
	    pyosdp_dict_add_int(obj, "offset", p->offset))
		return -1;
	return 0;
}

static int pyosdp_make_struct_cmd_pivdata(void *payload, PyObject *dict)
{
	struct osdp_cmd_pivdata *p = payload;
	uint8_t *oid;
	int len, val;

	if (pyosdp_dict_get_bytes(dict, "oid", &oid, &len))
		return -1;
	if (len != (int)sizeof(p->oid)) {
		PyErr_Format(PyExc_ValueError, "oid must be %zu bytes",
			     sizeof(p->oid));
		return -1;
	}
	memcpy(p->oid, oid, sizeof(p->oid));
	if (pyosdp_dict_get_int(dict, "element", &val))
		return -1;
	p->element = (uint8_t)val;
	if (pyosdp_dict_get_int(dict, "offset", &val))
		return -1;
	p->offset = (uint8_t)val;
	return 0;
}

static const struct pyosdp_field trs_send_apdu_fields[] = {
	{
		.key = "apdu",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_trs_cmd, apdu.data),
		.len_offset = offsetof(struct osdp_trs_cmd, apdu.length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_TRS_APDU_MAX_LEN,
	},
	END_OF_FIELDS,
};

/* The PIN block and PIN length field are anonymous structs, so their members
 * are placed relative to the start of the struct that encloses them. */
#define PIN_ENTRY_FIELD(block, member, field_kind)                             \
	{                                                                      \
		.key = #member,                                                \
		.kind = field_kind,                                            \
		.offset = offsetof(struct osdp_trs_pin_entry, block.member) -  \
			  offsetof(struct osdp_trs_pin_entry, block),          \
	}

static const struct pyosdp_field trs_pin_block_fields[] = {
	PIN_ENTRY_FIELD(pin_block, format, FIELD_INT),
	PIN_ENTRY_FIELD(pin_block, right_justify, FIELD_BOOL),
	PIN_ENTRY_FIELD(pin_block, offset_bits, FIELD_U16),
	PIN_ENTRY_FIELD(pin_block, size_bytes, FIELD_U8),
	END_OF_FIELDS,
};

static const struct pyosdp_field trs_pin_length_field_fields[] = {
	PIN_ENTRY_FIELD(pin_length_field, size_bits, FIELD_U8),
	PIN_ENTRY_FIELD(pin_length_field, offset_bits, FIELD_U16),
	END_OF_FIELDS,
};

static const struct pyosdp_field trs_pin_entry_fields[] = {
	U8_FIELD(struct osdp_trs_pin_entry, timeout_initial),
	U8_FIELD(struct osdp_trs_pin_entry, timeout_digit),
	{
		.key = "pin_block",
		.kind = FIELD_SUBDICT,
		.offset = offsetof(struct osdp_trs_pin_entry, pin_block),
		.fields = trs_pin_block_fields,
		.required = true,
	},
	{
		.key = "pin_length_field",
		.kind = FIELD_SUBDICT,
		.offset = offsetof(struct osdp_trs_pin_entry, pin_length_field),
		.fields = trs_pin_length_field_fields,
		.required = true,
	},
	U8_FIELD(struct osdp_trs_pin_entry, min_digits),
	U8_FIELD(struct osdp_trs_pin_entry, max_digits),
	U32_FIELD(struct osdp_trs_pin_entry, complete_on),
	U8_FIELD(struct osdp_trs_pin_entry, num_messages),
	U16_FIELD(struct osdp_trs_pin_entry, language_id),
	U8_FIELD(struct osdp_trs_pin_entry, msg_index),
	{
		.key = "apdu",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_trs_pin_entry, apdu.data),
		.len_offset = offsetof(struct osdp_trs_pin_entry, apdu.length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_TRS_APDU_MAX_LEN,
	},
	END_OF_FIELDS,
};

/*
 * A TRS command is a union keyed by its own `command`. That name is taken in
 * the dict by the OSDP command id, so the TRS one travels as "trs_command".
 * The members of the union a command carries sit beside it, as do those of the
 * PIN entry; its T=1 prologue is a fixed array with no length field, which
 * the field kinds cannot express, so it is marshalled by hand.
 */
static int pyosdp_make_dict_cmd_trs(PyObject *obj, const void *payload)
{
	const struct osdp_trs_cmd *t = payload;
	const struct osdp_trs_pin_entry *pe = &t->pin_entry;

	if (pyosdp_dict_add_int(obj, "trs_command", t->command))
		return -1;
	switch (t->command) {
	case OSDP_TRS_CMD_START:
	case OSDP_TRS_CMD_STOP:
	case OSDP_TRS_CMD_CARD_SCAN:
		return 0;
	case OSDP_TRS_CMD_SEND_APDU:
		return pyosdp_fields_to_dict(obj, t, trs_send_apdu_fields);
	case OSDP_TRS_CMD_ENTER_PIN:
		if (pyosdp_fields_to_dict(obj, pe, trs_pin_entry_fields))
			return -1;
		return pyosdp_dict_add_bytes(obj, "teo_prologue",
					     pe->teo_prologue,
					     sizeof(pe->teo_prologue));
	}
	PyErr_Format(PyExc_ValueError, "Unknown TRS command: %d", t->command);
	return -1;
}

static int pyosdp_make_struct_cmd_trs(void *payload, PyObject *dict)
{
	struct osdp_trs_cmd *t = payload;
	struct osdp_trs_pin_entry *pe = &t->pin_entry;
	uint8_t *teo_prologue;
	int command, len;

	if (pyosdp_dict_get_int(dict, "trs_command", &command))
		return -1;
	switch (command) {
	case OSDP_TRS_CMD_START:
	case OSDP_TRS_CMD_STOP:
	case OSDP_TRS_CMD_CARD_SCAN:
		break;
	case OSDP_TRS_CMD_SEND_APDU:
		if (pyosdp_fields_to_struct(t, dict, trs_send_apdu_fields))
			return -1;
		break;
	case OSDP_TRS_CMD_ENTER_PIN:
		if (pyosdp_fields_to_struct(pe, dict, trs_pin_entry_fields) ||
		    pyosdp_dict_get_bytes(dict, "teo_prologue", &teo_prologue,
					  &len))
			return -1;
		if (len != (int)sizeof(pe->teo_prologue)) {
			PyErr_Format(PyExc_ValueError,
				     "teo_prologue must be %zu bytes",
				     sizeof(pe->teo_prologue));
			return -1;
		}
		memcpy(pe->teo_prologue, teo_prologue, sizeof(pe->teo_prologue));
		break;
	default:
		PyErr_Format(PyExc_ValueError, "Unknown TRS command: %d",
			     command);
		return -1;
	}
	t->command = (enum osdp_trs_cmd_e)command;
	return 0;
}

/*
 * A notification's payload is a union keyed by `type`: each kind carries its
 * own typed struct (sc_status/pd_status/mp/pd_id/trs_status). Emit/consume
 * the dict keys the type selects so every field survives the round trip.
 */
static int pyosdp_make_dict_notification(PyObject *obj, const void *payload)
{
	const struct osdp_notification *n = payload;

	if (pyosdp_dict_add_int(obj, "type", n->type))
		return -1;
	switch (n->type) {
	case OSDP_NOTIFICATION_SC_STATUS:
		if (pyosdp_dict_add_int(obj, "active", n->sc_status.active) ||
		    pyosdp_dict_add_int(obj, "scbk_d", n->sc_status.scbk_d))
			return -1;
		break;
	case OSDP_NOTIFICATION_PD_STATUS:
		if (pyosdp_dict_add_int(obj, "online", n->pd_status.online))
			return -1;
		break;
	case OSDP_NOTIFICATION_MP_START:
	case OSDP_NOTIFICATION_MP_PROGRESS:
	case OSDP_NOTIFICATION_MP_DONE:
		if (pyosdp_dict_add_int(obj, "mp_type", n->mp.mp_type) ||
		    pyosdp_dict_add_int(obj, "object_id", n->mp.object_id) ||
		    pyosdp_dict_add_int(obj, "total", (int)n->mp.total) ||
		    pyosdp_dict_add_int(obj, "offset", (int)n->mp.offset) ||
		    pyosdp_dict_add_int(obj, "outcome", n->mp.outcome))
			return -1;
		break;
	case OSDP_NOTIFICATION_PD_ID:
		if (pyosdp_dict_add_int(obj, "version", n->pd_id.version) ||
		    pyosdp_dict_add_int(obj, "model", n->pd_id.model) ||
		    pyosdp_dict_add_int(obj, "vendor_code",
					n->pd_id.vendor_code) ||
		    pyosdp_dict_add_int(obj, "serial_number",
					n->pd_id.serial_number) ||
		    pyosdp_dict_add_int(obj, "firmware_version",
					n->pd_id.firmware_version))
			return -1;
		break;
	case OSDP_NOTIFICATION_TRS_STATUS:
		if (pyosdp_dict_add_int(obj, "status", n->trs_status.status))
			return -1;
		break;
	default:
		break;
	}
	return 0;
}

static int pyosdp_make_struct_notification(void *payload, PyObject *dict)
{
	struct osdp_notification *n = payload;
	int type, val;

	if (pyosdp_dict_get_int(dict, "type", &type))
		return -1;
	n->type = (enum osdp_notification_type)type;
	switch (n->type) {
	case OSDP_NOTIFICATION_SC_STATUS:
		if (pyosdp_dict_get_int(dict, "active", &val))
			return -1;
		n->sc_status.active = (bool)val;
		if (pyosdp_dict_get_int(dict, "scbk_d", &val))
			return -1;
		n->sc_status.scbk_d = (bool)val;
		break;
	case OSDP_NOTIFICATION_PD_STATUS:
		if (pyosdp_dict_get_int(dict, "online", &val))
			return -1;
		n->pd_status.online = (bool)val;
		break;
	case OSDP_NOTIFICATION_MP_START:
	case OSDP_NOTIFICATION_MP_PROGRESS:
	case OSDP_NOTIFICATION_MP_DONE:
		if (pyosdp_dict_get_int(dict, "mp_type", &val))
			return -1;
		n->mp.mp_type = (enum osdp_mp_msg_type)val;
		if (pyosdp_dict_get_int(dict, "object_id", &val))
			return -1;
		n->mp.object_id = val;
		if (pyosdp_dict_get_int(dict, "total", &val))
			return -1;
		n->mp.total = (uint32_t)val;
		if (pyosdp_dict_get_int(dict, "offset", &val))
			return -1;
		n->mp.offset = (uint32_t)val;
		if (pyosdp_dict_get_int(dict, "outcome", &val))
			return -1;
		n->mp.outcome = val;
		break;
	case OSDP_NOTIFICATION_PD_ID:
		if (pyosdp_dict_get_int(dict, "version", &val))
			return -1;
		n->pd_id.version = val;
		if (pyosdp_dict_get_int(dict, "model", &val))
			return -1;
		n->pd_id.model = val;
		if (pyosdp_dict_get_int(dict, "vendor_code", &val))
			return -1;
		n->pd_id.vendor_code = (uint32_t)val;
		if (pyosdp_dict_get_int(dict, "serial_number", &val))
			return -1;
		n->pd_id.serial_number = (uint32_t)val;
		if (pyosdp_dict_get_int(dict, "firmware_version", &val))
			return -1;
		n->pd_id.firmware_version = (uint32_t)val;
		break;
	case OSDP_NOTIFICATION_TRS_STATUS:
		if (pyosdp_dict_get_int(dict, "status", &val))
			return -1;
		n->trs_status.status = (enum osdp_trs_session_status_e)val;
		break;
	default:
		break;
	}
	return 0;
}

/* ------------------------------- */
/*             EVENTS              */
/* ------------------------------- */

/*
 * Card data is a bit string for the raw formats, so its length is a bit count
 * and the bytes that carry it are a whole number of bytes wide. Every other
 * event says what it means in its field table; this one cannot.
 */
static int pyosdp_make_dict_event_cardread(PyObject *obj, const void *payload)
{
	const struct osdp_event_cardread *ev = payload;
	int nr_bytes = ev->length;

	if (pyosdp_dict_add_int(obj, "reader_no", ev->reader_no) ||
	    pyosdp_dict_add_int(obj, "format", ev->format) ||
	    pyosdp_dict_add_int(obj, "direction", ev->direction))
		return -1;

	if (ev->format == OSDP_CARD_FMT_RAW_UNSPECIFIED ||
	    ev->format == OSDP_CARD_FMT_RAW_WIEGAND) {
		if (pyosdp_dict_add_int(obj, "length", ev->length))
			return -1;
		nr_bytes = (ev->length + 7) / 8;
	}

	if (nr_bytes < 0 || nr_bytes > OSDP_EVENT_CARDREAD_MAX_DATALEN) {
		PyErr_SetString(PyExc_ValueError, "Card data has a bad length");
		return -1;
	}

	return pyosdp_dict_add_bytes(obj, "data", ev->data, nr_bytes);
}

static int pyosdp_make_struct_event_cardread(void *payload, PyObject *dict)
{
	struct osdp_event_cardread *ev = payload;
	int reader_no, format, direction, length, nr_bytes;
	uint8_t *data;

	if (pyosdp_dict_get_int(dict, "reader_no", &reader_no) ||
	    pyosdp_dict_get_int(dict, "format", &format) ||
	    pyosdp_dict_get_int(dict, "direction", &direction) ||
	    pyosdp_dict_get_bytes(dict, "data", &data, &nr_bytes))
		return -1;

	length = nr_bytes;
	if (format == OSDP_CARD_FMT_RAW_UNSPECIFIED ||
	    format == OSDP_CARD_FMT_RAW_WIEGAND) {
		if (pyosdp_dict_get_int(dict, "length", &length))
			return -1;
		if (length < 0 || (length + 7) / 8 > nr_bytes) {
			PyErr_Format(PyExc_ValueError,
				     "Card of %d bits needs more than %d bytes",
				     length, nr_bytes);
			return -1;
		}
		nr_bytes = (length + 7) / 8;
	}

	if (nr_bytes > OSDP_EVENT_CARDREAD_MAX_DATALEN) {
		PyErr_SetString(PyExc_ValueError, "Card data is too long");
		return -1;
	}

	ev->reader_no = reader_no;
	ev->format = format;
	ev->direction = direction;
	ev->length = (uint16_t)length;
	memcpy(ev->data, data, nr_bytes);
	return 0;
}

static const struct pyosdp_field event_keypress_fields[] = {
	INT_FIELD_(struct osdp_event_keypress, reader_no),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_keypress, data),
		.len_offset = offsetof(struct osdp_event_keypress, length),
		.len_kind = FIELD_INT,
		.max_len = OSDP_EVENT_KEYPRESS_MAX_DATALEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field event_mfgrep_fields[] = {
	U32_FIELD(struct osdp_event_mfgrep, vendor_code),
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_mfgrep, data),
		.len_offset = offsetof(struct osdp_event_mfgrep, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_EVENT_MFGREP_MAX_DATALEN,
	},
	END_OF_FIELDS,
};

/* Backs both MFGSTATR and MFGERRR; either may carry an empty payload */
static const struct pyosdp_field event_mfgstat_fields[] = {
	{
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_mfgstat, data),
		.len_offset = offsetof(struct osdp_event_mfgstat, length),
		.len_kind = FIELD_U8,
		.max_len = OSDP_EVENT_MFGSTAT_MAX_DATALEN,
		.allow_empty = true,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field event_bioreadr_fields[] = {
	U8_FIELD(struct osdp_event_bioreadr, reader),
	INT_FIELD_(struct osdp_event_bioreadr, status),
	INT_FIELD_(struct osdp_event_bioreadr, type),
	U8_FIELD(struct osdp_event_bioreadr, quality),
	{
		/* A failed scan carries no template */
		.key = "data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_event_bioreadr, data),
		.len_offset = offsetof(struct osdp_event_bioreadr, length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN,
		.allow_empty = true,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field event_biomatchr_fields[] = {
	U8_FIELD(struct osdp_event_biomatchr, reader),
	INT_FIELD_(struct osdp_event_biomatchr, status),
	U8_FIELD(struct osdp_event_biomatchr, score),
	END_OF_FIELDS,
};

static const struct pyosdp_field event_status_fields[] = {
	INT_FIELD_(struct osdp_status_report, type),
	{
		.key = "report",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_status_report, report),
		.len_offset = offsetof(struct osdp_status_report, nr_entries),
		.len_kind = FIELD_INT,
		.max_len = OSDP_STATUS_REPORT_MAX_LEN,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field trs_card_info_fields[] = {
	U8_FIELD(struct osdp_trs_card_info, reader),
	INT_FIELD_(struct osdp_trs_card_info, protocol),
	{
		.key = "csn",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_trs_card_info, csn),
		.len_offset = offsetof(struct osdp_trs_card_info, csn_len),
		.len_kind = FIELD_U8,
		.max_len = OSDP_TRS_CSN_MAX_LEN,
		.allow_empty = true,
	},
	{
		.key = "protocol_data",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_trs_card_info, protocol_data),
		.len_offset =
			offsetof(struct osdp_trs_card_info, protocol_data_len),
		.len_kind = FIELD_U8,
		.max_len = OSDP_TRS_PROTOCOL_DATA_MAX_LEN,
		.allow_empty = true,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field trs_card_present_fields[] = {
	U8_FIELD(struct osdp_trs_card_present, reader),
	INT_FIELD_(struct osdp_trs_card_present, status),
	END_OF_FIELDS,
};

static const struct pyosdp_field trs_card_data_fields[] = {
	U8_FIELD(struct osdp_trs_card_data, reader),
	U8_FIELD(struct osdp_trs_card_data, status),
	{
		/* What the card returned, if anything, is the card's business */
		.key = "apdu",
		.kind = FIELD_BYTES,
		.offset = offsetof(struct osdp_trs_card_data, apdu.data),
		.len_offset = offsetof(struct osdp_trs_card_data, apdu.length),
		.len_kind = FIELD_U16,
		.max_len = OSDP_TRS_APDU_MAX_LEN,
		.allow_empty = true,
	},
	END_OF_FIELDS,
};

static const struct pyosdp_field trs_pin_complete_fields[] = {
	U8_FIELD(struct osdp_trs_pin_complete, reader),
	U8_FIELD(struct osdp_trs_pin_complete, status),
	U8_FIELD(struct osdp_trs_pin_complete, tries),
	END_OF_FIELDS,
};

static const struct pyosdp_field trs_error_fields[] = {
	U8_FIELD(struct osdp_trs_error, code),
	END_OF_FIELDS,
};

static const struct pyosdp_field *trs_reply_fields(int reply)
{
	switch (reply) {
	case OSDP_TRS_REPLY_CARD_INFO:
		return trs_card_info_fields;
	case OSDP_TRS_REPLY_CARD_PRESENT:
		return trs_card_present_fields;
	case OSDP_TRS_REPLY_CARD_DATA:
		return trs_card_data_fields;
	case OSDP_TRS_REPLY_PIN_COMPLETE:
		return trs_pin_complete_fields;
	case OSDP_TRS_REPLY_ERROR:
		return trs_error_fields;
	}
	PyErr_Format(PyExc_ValueError, "Unknown TRS reply: %d", reply);
	return NULL;
}

/*
 * A TRS reply is a union keyed by `reply`; the members of the one it carries
 * sit beside it in the dict. Every member begins at the same address.
 */
static int pyosdp_make_dict_event_trs(PyObject *obj, const void *payload)
{
	const struct osdp_trs_reply *r = payload;
	const struct pyosdp_field *fields = trs_reply_fields(r->reply);

	if (fields == NULL || pyosdp_dict_add_int(obj, "reply", r->reply))
		return -1;
	return pyosdp_fields_to_dict(obj, &r->card_info, fields);
}

static int pyosdp_make_struct_event_trs(void *payload, PyObject *dict)
{
	struct osdp_trs_reply *r = payload;
	const struct pyosdp_field *fields;
	int reply;

	if (pyosdp_dict_get_int(dict, "reply", &reply))
		return -1;
	fields = trs_reply_fields(reply);
	if (fields == NULL ||
	    pyosdp_fields_to_struct(&r->card_info, dict, fields))
		return -1;
	r->reply = (enum osdp_trs_reply_e)reply;
	return 0;
}

/* --- Translators --- */

struct pyosdp_translator {
	const struct pyosdp_field *fields;
	/* Set only when a payload's meaning cannot be stated as a field table */
	int (*to_dict)(PyObject *obj, const void *payload);
	int (*to_struct)(void *payload, PyObject *dict);
};

static const struct pyosdp_translator command_translator[OSDP_CMD_SENTINEL] = {
	[OSDP_CMD_OUTPUT] = { .fields = cmd_output_fields },
	[OSDP_CMD_LED] = { .fields = cmd_led_fields },
	[OSDP_CMD_BUZZER] = { .fields = cmd_buzzer_fields },
	[OSDP_CMD_TEXT] = { .fields = cmd_text_fields },
	[OSDP_CMD_TDSET] = { .fields = cmd_tdset_fields },
	[OSDP_CMD_KEYSET] = { .fields = cmd_keyset_fields },
	[OSDP_CMD_COMSET] = { .fields = cmd_comset_fields },
	[OSDP_CMD_COMSET_DONE] = { .fields = cmd_comset_fields },
	[OSDP_CMD_MFG] = { .fields = cmd_mfg_fields },
	[OSDP_CMD_BIOREAD] = { .fields = cmd_bioread_fields },
	[OSDP_CMD_BIOMATCH] = { .fields = cmd_biomatch_fields },
	[OSDP_CMD_PIVDATA] = {
		.to_dict = pyosdp_make_dict_cmd_pivdata,
		.to_struct = pyosdp_make_struct_cmd_pivdata,
	},
	[OSDP_CMD_GENAUTH] = { .fields = cmd_auth_fields },
	[OSDP_CMD_CRAUTH] = { .fields = cmd_auth_fields },
	[OSDP_CMD_FILE_TX] = { .fields = cmd_file_tx_fields },
	[OSDP_CMD_STATUS] = { .fields = cmd_status_fields },
	[OSDP_CMD_NOTIFICATION] = {
		.to_dict = pyosdp_make_dict_notification,
		.to_struct = pyosdp_make_struct_notification,
	},
	[OSDP_CMD_XWR] = {
		.to_dict = pyosdp_make_dict_cmd_trs,
		.to_struct = pyosdp_make_struct_cmd_trs,
	},
};

static const struct pyosdp_translator event_translator[OSDP_EVENT_SENTINEL] = {
	[OSDP_EVENT_CARDREAD] = {
		.to_dict = pyosdp_make_dict_event_cardread,
		.to_struct = pyosdp_make_struct_event_cardread,
	},
	[OSDP_EVENT_KEYPRESS] = { .fields = event_keypress_fields },
	[OSDP_EVENT_MFGREP] = { .fields = event_mfgrep_fields },
	[OSDP_EVENT_MFGSTATR] = { .fields = event_mfgstat_fields },
	[OSDP_EVENT_MFGERRR] = { .fields = event_mfgstat_fields },
	[OSDP_EVENT_BIOREADR] = { .fields = event_bioreadr_fields },
	[OSDP_EVENT_BIOMATCHR] = { .fields = event_biomatchr_fields },
	[OSDP_EVENT_PIVDATAR] = { .fields = event_piv_reply_fields },
	[OSDP_EVENT_GENAUTHR] = { .fields = event_piv_reply_fields },
	[OSDP_EVENT_CRAUTHR] = { .fields = event_piv_reply_fields },
	[OSDP_EVENT_STATUS] = { .fields = event_status_fields },
	[OSDP_EVENT_NOTIFICATION] = {
		.to_dict = pyosdp_make_dict_notification,
		.to_struct = pyosdp_make_struct_notification,
	},
	[OSDP_EVENT_TRS] = {
		.to_dict = pyosdp_make_dict_event_trs,
		.to_struct = pyosdp_make_struct_event_trs,
	},
};

static int translate_to_dict(const struct pyosdp_translator *t, PyObject *obj,
			     const void *payload)
{
	if (t->fields)
		return pyosdp_fields_to_dict(obj, payload, t->fields);
	return t->to_dict(obj, payload);
}

static int translate_to_struct(const struct pyosdp_translator *t, void *payload,
			       PyObject *dict)
{
	if (t->fields)
		return pyosdp_fields_to_struct(payload, dict, t->fields);
	return t->to_struct(payload, dict);
}

/*
 * Every payload lives in the same anonymous union, so they all begin at the
 * same address and one pointer serves whichever the id selects.
 */
static void *cmd_payload(struct osdp_cmd *cmd)
{
	return &cmd->led;
}

static void *event_payload(struct osdp_event *event)
{
	return &event->cardread;
}

/* --- Exposed Methods --- */

int pyosdp_make_struct_cmd(struct osdp_cmd *cmd, PyObject *dict)
{
	int cmd_id;

	if (pyosdp_dict_get_int(dict, "command", &cmd_id))
		return -1;
	if (cmd_id <= 0 || cmd_id >= OSDP_CMD_SENTINEL) {
		PyErr_Format(PyExc_ValueError, "Unknown command: %d", cmd_id);
		return -1;
	}
	if (translate_to_struct(&command_translator[cmd_id], cmd_payload(cmd),
				dict))
		return -1;

	cmd->id = cmd_id;
	return 0;
}

int pyosdp_make_dict_cmd(PyObject **dict, struct osdp_cmd *cmd)
{
	PyObject *obj;

	if (cmd->id <= 0 || cmd->id >= OSDP_CMD_SENTINEL) {
		PyErr_Format(PyExc_ValueError, "Unknown command: %d", cmd->id);
		return -1;
	}

	obj = PyDict_New();
	if (obj == NULL)
		return -1;

	if (pyosdp_dict_add_int(obj, "command", cmd->id) ||
	    translate_to_dict(&command_translator[cmd->id], obj,
			      cmd_payload(cmd))) {
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
	if (event_type <= 0 || event_type >= OSDP_EVENT_SENTINEL) {
		PyErr_Format(PyExc_ValueError, "Unknown event: %d", event_type);
		return -1;
	}
	if (translate_to_struct(&event_translator[event_type],
				event_payload(event), dict))
		return -1;

	event->type = event_type;
	return 0;
}

int pyosdp_make_dict_event(PyObject **dict, struct osdp_event *event)
{
	PyObject *obj;

	if (event->type <= 0 || event->type >= OSDP_EVENT_SENTINEL) {
		PyErr_Format(PyExc_ValueError, "Unknown event: %d",
			     event->type);
		return -1;
	}

	obj = PyDict_New();
	if (obj == NULL)
		return -1;

	if (pyosdp_dict_add_int(obj, "event", event->type) ||
	    translate_to_dict(&event_translator[event->type], obj,
			      event_payload(event))) {
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
	    pyosdp_dict_add_int(obj, "firmware_version",
				pd_id->firmware_version)) {
		Py_DECREF(obj);
		return NULL;
	}

	return obj;
}
