#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
# What the binding layer does with bad input.
#
# These drive osdp_sys/{module,cp,pd,base,data,utils}.c straight through their
# error paths. submit_command() and submit_event() marshal synchronously, at
# call time, before anything reaches a queue -- so a device that is constructed
# but never start()ed is enough. No threads, no bus, no waiting.

import pytest

from osdp import Channel, _sys


class SilentChannel(Channel):
    def read(self, max_bytes: int) -> bytes:
        return b""

    def write(self, buf: bytes) -> int:
        return len(buf)

    def flush(self) -> None:
        pass


def cp_info(**kwargs):
    return {"address": 101, "flags": 0, "channel": SilentChannel(), **kwargs}


def pd_info(**kwargs):
    return {
        "address": 101,
        "flags": 0,
        "version": 1,
        "model": 1,
        "vendor_code": 0,
        "firmware_version": 0,
        "serial_number": 0,
        "channel": SilentChannel(),
        **kwargs,
    }


@pytest.fixture
def cp():
    return _sys.ControlPanel([cp_info()])


@pytest.fixture
def pd():
    return _sys.PeripheralDevice(pd_info())


# -- module.c ---------------------------------------------------------------


@pytest.mark.parametrize("level", ["bogus", 99, -1, None])
def test_set_loglevel_rejects_a_bad_level(level):
    # Pins current behaviour: this is a KeyError, not the ValueError you would
    # expect. Changing it would break callers, so it is only recorded here.
    with pytest.raises(KeyError):
        _sys.set_loglevel(level)


def test_set_loglevel_accepts_the_documented_range():
    for level in range(_sys.LOG_EMERG, _sys.LOG_DEBUG + 1):
        _sys.set_loglevel(level)


# -- cp.c: construction -----------------------------------------------------


def test_cp_needs_at_least_one_pd():
    with pytest.raises(ValueError, match="Invalid num_pd"):
        _sys.ControlPanel([])


def test_cp_refuses_more_pds_than_the_bus_allows():
    with pytest.raises(ValueError, match="Invalid num_pd"):
        _sys.ControlPanel([cp_info() for _ in range(128)])


def test_cp_rejects_a_pd_info_that_is_not_a_dict():
    with pytest.raises(ValueError, match="Invalid pd_info at index 0"):
        _sys.ControlPanel(["not a dict"])


@pytest.mark.parametrize("key", ["address", "flags"])
def test_cp_rejects_a_pd_info_missing_a_required_key(key):
    info = cp_info()
    del info[key]
    with pytest.raises(ValueError, match="Invalid pd_info at index 0"):
        _sys.ControlPanel([info])


def test_cp_rejects_a_zero_baud_rate():
    with pytest.raises(ValueError, match="Invalid baud_rate"):
        _sys.ControlPanel([cp_info(baud_rate=0)])


def test_cp_lets_a_negative_baud_rate_through():
    # Pins a known wart: baud_rate is parsed unsigned, so a negative wraps to a
    # large positive and slips past the "<= 0" check. Harmless (the value is
    # informational) but it is not the rejection you would expect.
    assert _sys.ControlPanel([cp_info(baud_rate=-1)]) is not None


def test_cp_needs_a_channel():
    info = cp_info()
    del info["channel"]
    with pytest.raises(KeyError, match="channel object missing"):
        _sys.ControlPanel([info])


@pytest.mark.parametrize("scbk", [b"x" * 15, b"x" * 17, b"x"])
def test_cp_rejects_a_malformed_scbk(scbk):
    with pytest.raises(TypeError, match="scbk must be exactly 16 bytes"):
        _sys.ControlPanel([cp_info(scbk=scbk)])


def test_cp_treats_an_empty_scbk_as_no_scbk():
    # An empty bytes reads as "the key is not there", which leaves that PD
    # with no secure channel.
    assert _sys.ControlPanel([cp_info(scbk=b"")]) is not None


# -- pd.c: construction -----------------------------------------------------


@pytest.mark.parametrize(
    "key",
    [
        "address",
        "flags",
        "version",
        "model",
        "vendor_code",
        "firmware_version",
        "serial_number",
    ],
)
def test_pd_rejects_a_pd_info_missing_a_required_key(key):
    info = pd_info()
    del info[key]
    with pytest.raises(KeyError, match=key):
        _sys.PeripheralDevice(info)


def test_pd_needs_a_channel():
    info = pd_info()
    del info["channel"]
    with pytest.raises(KeyError, match="channel object missing"):
        _sys.PeripheralDevice(info)


@pytest.mark.parametrize("scbk", [b"x" * 15, b"x" * 17, b"x"])
def test_pd_rejects_a_malformed_scbk(scbk):
    # A truncated key used to be dropped on the floor, quietly leaving the PD
    # keyless.
    with pytest.raises(TypeError, match="scbk must be exactly 16 bytes"):
        _sys.PeripheralDevice(pd_info(scbk=scbk))


def test_pd_treats_an_empty_scbk_as_no_scbk():
    assert _sys.PeripheralDevice(pd_info(scbk=b"")) is not None


def test_pd_builds_without_the_capabilities_keyword():
    # capabilities is optional, and PyArg_ParseTupleAndKeywords leaves optional
    # arguments untouched when omitted. This used to read uninitialized stack.
    assert _sys.PeripheralDevice(pd_info()) is not None


def test_pd_rejects_too_many_capabilities():
    cap = {"function_code": 1, "compliance_level": 1, "num_items": 1}
    with pytest.raises(ValueError, match="Invalid cap list size"):
        _sys.PeripheralDevice(pd_info(), capabilities=[cap] * 64)


@pytest.mark.parametrize(
    "cap", [{}, {"function_code": 1}, {"compliance_level": 1, "num_items": 1}]
)
def test_pd_rejects_an_incomplete_capability(cap):
    with pytest.raises(ValueError, match="Invalid capability at index 0"):
        _sys.PeripheralDevice(pd_info(), capabilities=[cap])


# -- cp.c: bad PD offsets ---------------------------------------------------


@pytest.mark.parametrize(
    "method, args",
    [
        ("set_flag", (99, _sys.FLAG_ENFORCE_SECURE)),
        ("clear_flag", (99, _sys.FLAG_ENFORCE_SECURE)),
        ("get_pd_id", (99,)),
        ("check_capability", (99, _sys.CAP_OUTPUT_CONTROL)),
        ("enable_pd", (99,)),
        ("disable_pd", (99,)),
        ("is_pd_enabled", (99,)),
        ("register_file_ops", (99, {})),
    ],
)
def test_cp_rejects_an_out_of_range_pd_offset(cp, method, args):
    with pytest.raises(ValueError, match="(?i)invalid pd offset|function code"):
        getattr(cp, method)(*args)


def test_cp_flag_methods_raise_on_a_non_integer(cp):
    # These used to hand back False with the parse error still set, which
    # CPython surfaced as an unhelpful SystemError.
    with pytest.raises(TypeError):
        cp.set_flag("nope", "nope")
    with pytest.raises(TypeError):
        cp.clear_flag("nope", "nope")


def test_cp_flag_methods_report_a_refused_change():
    # Security posture is fixed at setup; the library refuses to reopen it
    # and the binding has to surface that as False, not raise or succeed.
    keyed = _sys.ControlPanel(
        [cp_info(scbk=bytes(range(1, 17)), flags=_sys.FLAG_ENFORCE_SECURE)]
    )
    assert keyed.set_flag(0, _sys.FLAG_INSTALL_MODE) is False
    assert keyed.clear_flag(0, _sys.FLAG_INSTALL_MODE) is False
    assert keyed.clear_flag(0, _sys.FLAG_ENFORCE_SECURE) is False
    assert keyed.set_flag(0, _sys.FLAG_ENFORCE_SECURE) is True

    keyless = _sys.ControlPanel([cp_info()])
    assert keyless.set_flag(0, _sys.FLAG_ENFORCE_SECURE) is False


# -- base.c: metrics, file tx status, file ops ------------------------------


@pytest.mark.parametrize("method", ["get_metrics", "get_file_tx_status"])
def test_base_query_raises_on_a_non_integer(cp, method):
    with pytest.raises(TypeError):
        getattr(cp, method)("nope")


@pytest.mark.parametrize("method", ["get_metrics", "get_file_tx_status"])
def test_base_query_returns_none_for_an_unknown_pd(cp, method):
    # An unknown PD is not an error here, it is simply "nothing to report".
    assert getattr(cp, method)(99) is None


def test_file_tx_status_is_none_when_no_transfer_is_running(cp):
    assert cp.get_file_tx_status(0) is None


@pytest.mark.parametrize("missing", ["open", "read", "write", "close"])
def test_register_file_ops_needs_every_callback(cp, missing):
    fops = {
        name: (lambda *args: 0)
        for name in ("open", "read", "write", "close")
        if name != missing
    }
    with pytest.raises(ValueError, match=f"Missing '{missing}' callback"):
        cp.register_file_ops(0, fops)


def test_register_file_ops_needs_a_dict(cp):
    with pytest.raises(TypeError):
        cp.register_file_ops(0, "not a dict")


# -- callback registration --------------------------------------------------


@pytest.mark.parametrize("not_callable", [42, "nope", None, b"x"])
def test_cp_callbacks_must_be_callable(cp, not_callable):
    with pytest.raises(TypeError, match="Need a callable object"):
        cp.set_event_callback(not_callable)
    with pytest.raises(TypeError, match="Need a callable object"):
        cp.set_command_completion_callback(not_callable)


@pytest.mark.parametrize("not_callable", [42, "nope", None, b"x"])
def test_pd_callbacks_must_be_callable(pd, not_callable):
    with pytest.raises(TypeError, match="Need a callable object"):
        pd.set_command_callback(not_callable)
    with pytest.raises(TypeError, match="Need a callable object"):
        pd.set_event_completion_callback(not_callable)


# -- data.c: command marshalling --------------------------------------------
#
# data.c wraps whatever went wrong into a generic "Unable to convert ..." and
# chains the real diagnosis onto __cause__. Assert on the cause: that is the
# part a caller can act on.


def output_cmd(**kwargs):
    return {
        "command": _sys.CMD_OUTPUT,
        "output_no": 0,
        "control_code": 1,
        "timer_count": 0,
        **kwargs,
    }


def submit_command_cause(cp, command):
    with pytest.raises(ValueError) as excinfo:
        cp.submit_command(0, command)
    assert excinfo.value.__cause__ is not None, "the real error was not chained"
    return excinfo.value.__cause__


@pytest.mark.parametrize("command_id", [0, 99, -1])
def test_submit_command_rejects_an_unknown_command_id(cp, command_id):
    cause = submit_command_cause(cp, {"command": command_id})
    assert isinstance(cause, ValueError)
    assert "Unknown command" in str(cause)


def test_submit_command_needs_a_command_id(cp):
    assert isinstance(submit_command_cause(cp, {}), KeyError)


def test_submit_command_reports_the_missing_key(cp):
    command = output_cmd()
    del command["output_no"]
    cause = submit_command_cause(cp, command)
    assert isinstance(cause, KeyError)
    assert "output_no" in str(cause)


def test_submit_command_rejects_a_non_number(cp):
    cause = submit_command_cause(cp, output_cmd(output_no="nope"))
    assert isinstance(cause, TypeError)
    assert "Expected number" in str(cause)


def test_submit_command_rejects_a_value_too_big_for_32_bits(cp):
    cause = submit_command_cause(cp, output_cmd(output_no=2**33))
    assert isinstance(cause, OverflowError)
    assert "32 bits" in str(cause)


@pytest.mark.parametrize(
    "command, field",
    [
        (
            {
                "command": _sys.CMD_TEXT,
                "reader": 0,
                "control_code": 1,
                "temp_time": 0,
                "offset_row": 1,
                "offset_col": 1,
                "data": "x" * 99,
            },
            "data",
        ),
        (
            {"command": _sys.CMD_MFG, "vendor_code": 1, "data": b"x" * 99},
            "data",
        ),
    ],
)
def test_submit_command_rejects_an_over_long_payload(cp, command, field):
    cause = submit_command_cause(cp, command)
    assert isinstance(cause, ValueError)
    assert f"'{field}' is too long" in str(cause)


def test_submit_command_needs_a_dict(cp):
    with pytest.raises(ValueError, match="Invalid arguments"):
        cp.submit_command(0, "not a dict")


def test_submit_command_rejects_an_unknown_pd(cp):
    with pytest.raises(ValueError, match="Invalid PD offset"):
        cp.submit_command(99, output_cmd())


# -- data.c: event marshalling ----------------------------------------------


def submit_event_cause(pd, event):
    with pytest.raises(TypeError) as excinfo:
        pd.submit_event(event)
    assert excinfo.value.__cause__ is not None, "the real error was not chained"
    return excinfo.value.__cause__


@pytest.mark.parametrize("event_id", [0, 99, -1])
def test_submit_event_rejects_an_unknown_event_id(pd, event_id):
    cause = submit_event_cause(pd, {"event": event_id})
    assert isinstance(cause, ValueError)
    assert "Unknown event" in str(cause)


def test_submit_event_needs_an_event_id(pd):
    assert isinstance(submit_event_cause(pd, {}), KeyError)


def test_submit_event_reports_the_missing_key(pd):
    cause = submit_event_cause(
        pd, {"event": _sys.EVENT_KEYPRESS, "reader_no": 0}
    )
    assert isinstance(cause, KeyError)
    assert "data" in str(cause)


def test_submit_event_rejects_a_card_read_whose_bits_exceed_its_data(pd):
    cause = submit_event_cause(
        pd,
        {
            "event": _sys.EVENT_CARDREAD,
            "reader_no": 0,
            "format": _sys.CARD_FMT_RAW_WIEGAND,
            "direction": 0,
            "length": 200,
            "data": b"\x01",
        },
    )
    assert isinstance(cause, ValueError)
    assert "needs more than" in str(cause)


def test_submit_event_needs_a_dict(pd):
    with pytest.raises(TypeError, match="Unable to convert event dict"):
        pd.submit_event("not a dict")


def test_pd_rejects_enforce_secure_with_install_mode():
    # Install mode exists to allow SCBK-D; EnforceSecure exists to forbid it.
    # Asking for both is a configuration error, not a preference to resolve.
    info = pd_info(scbk=b"\x00" * 16,
                   flags=_sys.FLAG_ENFORCE_SECURE | _sys.FLAG_INSTALL_MODE)
    with pytest.raises(Exception, match="Failed to setup PD"):
        _sys.PeripheralDevice(info)


def test_cp_rejects_enforce_secure_with_install_mode():
    info = cp_info(scbk=b"\x00" * 16,
                   flags=_sys.FLAG_ENFORCE_SECURE | _sys.FLAG_INSTALL_MODE)
    with pytest.raises(Exception, match="Failed to setup CP"):
        _sys.ControlPanel([info])
