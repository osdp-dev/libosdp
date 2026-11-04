#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
# What the binding layer does with a channel that misbehaves.
#
# osdp_sys/utils.c wraps a Python Channel in C callbacks that the library calls
# from inside refresh(). These tests drive refresh() directly rather than
# through a refresh thread, so a failure is deterministic and lands on the
# assertion instead of in a thread nobody is watching.

import time

import pytest

from osdp import Channel, PDCapabilities, _sys


class WellBehavedChannel(Channel):
    """Idle, but honest: the contract every other channel here breaks."""

    def read(self, max_bytes: int) -> bytes:
        return b""

    def write(self, buf: bytes) -> int:
        return len(buf)

    def flush(self) -> None:
        pass


class RecordingChannel(WellBehavedChannel):
    def __init__(self) -> None:
        self.reads = 0
        self.writes = 0
        self.flushes = 0

    def read(self, max_bytes: int) -> bytes:
        self.reads += 1
        return b""

    def write(self, buf: bytes) -> int:
        self.writes += 1
        return len(buf)

    def flush(self) -> None:
        self.flushes += 1


class ReadReturnsNonBytes(WellBehavedChannel):
    def read(self, max_bytes: int) -> bytes:
        return "not bytes"  # type: ignore[return-value]


class ReadIgnoresMaxBytes(WellBehavedChannel):
    def read(self, max_bytes: int) -> bytes:
        return b"\x00" * (max_bytes + 64)


class ReadRaises(WellBehavedChannel):
    def read(self, max_bytes: int) -> bytes:
        raise RuntimeError("read blew up")


class WriteReturnsNonInt(WellBehavedChannel):
    def write(self, buf: bytes) -> int:
        return None  # type: ignore[return-value]


class WriteRaises(WellBehavedChannel):
    def write(self, buf: bytes) -> int:
        raise RuntimeError("write blew up")


def make_cp(channel):
    return _sys.ControlPanel([{"address": 101, "flags": 0, "channel": channel}])


def make_pd_info(channel):
    return {
        "address": 101,
        "flags": 0,
        "version": 1,
        "model": 1,
        "vendor_code": 0,
        "firmware_version": 0,
        "serial_number": 0,
        "channel": channel,
    }


def make_pd(channel):
    return _sys.PeripheralDevice(make_pd_info(channel))


def test_channel_is_driven_from_c():
    channel = RecordingChannel()
    cp = make_cp(channel)
    for _ in range(20):
        cp.refresh()

    # The CP has a PD to poll, so it must have written; it always drains the
    # read side; and flush() -- which nothing else in the suite asserts -- is
    # called by the library, not by us.
    assert channel.writes > 0
    assert channel.reads > 0
    assert channel.flushes > 0


@pytest.mark.parametrize("channel", [ReadReturnsNonBytes(), WriteReturnsNonInt()])
def test_a_channel_of_the_wrong_type_is_a_failed_transfer_not_a_crash(channel):
    # The C side cannot do anything with a str from read() or a None from
    # write(), so it reports -1 to the library and the link simply never comes
    # up. No exception, and above all no crash.
    cp = make_cp(channel)
    for _ in range(10):
        cp.refresh()
    assert cp.status() == 0


def test_a_read_longer_than_requested_is_refused():
    cp = make_cp(ReadIgnoresMaxBytes())
    with pytest.raises(TypeError, match="read callback maxlen not respected"):
        for _ in range(10):
            cp.refresh()


@pytest.mark.parametrize(
    "channel, message",
    [
        (ReadRaises(), "read blew up"),
        (WriteRaises(), "write blew up"),
    ],
)
def test_an_exception_from_a_channel_reaches_the_caller(channel, message):
    # The callbacks run inside refresh() and can only tell the library "-1".
    # refresh() used to return None with the exception still pending, which
    # CPython reported as a SystemError naming some unrelated object. The
    # caller should get their own exception back instead.
    cp = make_cp(channel)
    with pytest.raises(RuntimeError, match=message):
        for _ in range(10):
            cp.refresh()


def test_a_pd_also_surfaces_a_channel_exception():
    # Only the read path: a PD writes when it answers a command, and nothing
    # ever arrives over an idle channel, so its write() is never reached.
    pd = make_pd(ReadRaises())
    with pytest.raises(RuntimeError, match="read blew up"):
        for _ in range(10):
            pd.refresh()


class LoopbackEnd(WellBehavedChannel):
    """One end of an in-memory wire; refresh() on both ends moves the bytes."""

    def __init__(self, inbox: bytearray, outbox: bytearray) -> None:
        self.inbox = inbox
        self.outbox = outbox

    def read(self, max_bytes: int) -> bytes:
        data = bytes(self.inbox[:max_bytes])
        del self.inbox[:max_bytes]
        return data

    def write(self, buf: bytes) -> int:
        self.outbox.extend(buf)
        return len(buf)


def refresh_until(predicate, *devices, timeout=3):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            return False
        for device in devices:
            device.refresh()
        time.sleep(0.001)
    return True


def test_a_command_callback_returning_the_wrong_shape_does_not_kill_the_pd():
    # pd.c wants a (status, reply) tuple back. Anything else is reported to the
    # library as -1: the PD refuses that one command and stays on the link.
    to_pd, to_cp = bytearray(), bytearray()
    cp = make_cp(LoopbackEnd(to_cp, to_pd))
    pd = _sys.PeripheralDevice(
        make_pd_info(LoopbackEnd(to_pd, to_cp)),
        capabilities=[{
            "function_code": _sys.CAP_OUTPUT_CONTROL,
            "compliance_level": 1,
            "num_items": 1,
        }],
    )
    seen, completions = [], []

    def wrong_shape(command):
        seen.append(command["command"])
        return "not a tuple"

    pd.set_command_callback(wrong_shape)
    cp.set_event_callback(lambda address, event: 0)
    cp.set_command_completion_callback(
        lambda pd_offset, command, status: completions.append(status)
    )
    assert refresh_until(lambda: cp.status() & 1, cp, pd), "never came online"

    assert cp.submit_command(0, {
        "command": _sys.CMD_OUTPUT,
        "output_no": 0,
        "control_code": 1,
        "timer_count": 0,
    })
    assert refresh_until(lambda: completions, cp, pd), "command never completed"

    assert _sys.CMD_OUTPUT in seen
    assert completions == [_sys.COMPLETION_FAILED]
    assert cp.status() & 1, "a refused command took the PD offline"
