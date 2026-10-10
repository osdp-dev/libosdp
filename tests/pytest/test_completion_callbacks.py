#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import gc
import threading
import time

import pytest

from osdp import *
from osdp import commands, events
from conftest import make_fifo_pair, cleanup_fifo_pair


PD_ADDR = 101

# The CP command pool holds OSDP_CP_CMD_POOL_SIZE (4) entries per PD; the PD
# event pool is sized the same way. Filling it is what lets a test observe the
# Flushed and Aborted completions, which only happen to commands that never
# reached the wire.
POOL_SIZE = 4

# FILE_TX is engine-owned: it never rides the command queue, so it needs its
# own file ops rather than the pool-filling Output command used elsewhere in
# this file.
FILE_ID = 7
FILE_SIZE = 64


class SenderFileOps:
    """The CP side of a transfer: reads out of a fixed buffer, never writes."""

    def __init__(self):
        self.data = bytes(range(FILE_SIZE))

    def open(self, file_id: int, size: int) -> int:
        assert file_id == FILE_ID
        assert size == 0
        return FILE_SIZE

    def read(self, size: int, offset: int) -> bytes:
        assert offset < FILE_SIZE
        if offset + size > FILE_SIZE:
            size = FILE_SIZE - offset
        return self.data[offset:offset + size]

    def write(self, data: bytes, offset: int) -> int:
        assert False

    def close(self, file_id: int) -> int:
        assert file_id == FILE_ID
        return 0


class ReceiverFileOps:
    """The PD side of a transfer: writes into a fixed buffer, never reads."""

    def __init__(self):
        self.data = bytearray(FILE_SIZE)

    def open(self, file_id: int, size: int) -> int:
        assert file_id == FILE_ID
        assert size == FILE_SIZE
        return 0

    def read(self, size: int, offset: int) -> bytes:
        assert False

    def write(self, data: bytes, offset: int) -> int:
        assert offset + len(data) <= FILE_SIZE
        self.data[offset:offset + len(data)] = data
        return len(data)

    def close(self, file_id: int) -> int:
        assert file_id == FILE_ID
        return 0


class Recorder:
    """Collects completion callbacks from the library's refresh thread."""

    def __init__(self):
        self.lock = threading.Lock()
        self.records = []

    def on_command_complete(self, pd_address, command, status):
        with self.lock:
            self.records.append((pd_address, command, status))

    def on_event_complete(self, event, status):
        with self.lock:
            self.records.append((event, status))

    def statuses(self):
        with self.lock:
            return [rec[-1] for rec in self.records]

    def first_with_status(self, status):
        with self.lock:
            for rec in self.records:
                if rec[-1] == status:
                    return rec
        return None

    def wait_for_status(self, status, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if status in self.statuses():
                return True
            time.sleep(0.02)
        return status in self.statuses()


def _make_pair(name, cp_flags=None):
    cp_channel, pd_channel = make_fifo_pair(name)
    pd_cap = PDCapabilities([(Capability.OutputControl, 1, 1)])
    cp = ControlPanel(
        [PDInfo(PD_ADDR, cp_channel, flags=cp_flags or [])],
        log_level=LogLevel.Debug,
    )
    pd = PeripheralDevice(
        PDInfo(PD_ADDR, pd_channel, flags=[]),
        pd_cap,
        log_level=LogLevel.Debug,
    )
    return cp, pd


def _teardown(cp, pd, name):
    for dev in (cp, pd):
        try:
            dev.teardown()
        except RuntimeError:
            pass  # already torn down by the test body
    gc.collect()
    cleanup_fifo_pair(name)


def _fill_queue(submit):
    """Submit until the pool is full. Returns how many were accepted."""
    queued = 0
    while queued < POOL_SIZE and submit():
        queued += 1
    return queued


def test_cp_command_completion_statuses():
    """Every command a CP accepts settles exactly once: Ok once it is
    acknowledged, Flushed if it is dropped from the queue, Aborted if the CP is
    torn down while it is still queued."""
    cp, pd = _make_pair("completion-cp")
    rec = Recorder()
    cmd = commands.Output(
        output_no=0,
        control_code=OutputControlCode.PermanentOff,
        timer_count=0,
    )

    cp.set_command_completion_handler(rec.on_command_complete)
    pd.set_command_handler(lambda command: None)

    try:
        pd.start()
        cp.start()
        assert cp.online_wait(PD_ADDR, timeout=10), "PD did not come online"

        # Ok: the command reaches the PD and is acknowledged.
        assert cp.submit_command(PD_ADDR, cmd)
        assert rec.wait_for_status(CompletionStatus.Ok), \
            "no Ok completion for an acknowledged command"

        pd_address, delivered, _ = rec.first_with_status(CompletionStatus.Ok)
        assert pd_address == PD_ADDR
        # The library hands back its own re-marshalled copy of the command.
        assert delivered == cmd
        assert delivered is not cmd

        # Flushed: queued commands dropped by flush_commands() still settle.
        queued = _fill_queue(lambda: cp.submit_command(PD_ADDR, cmd))
        assert queued > 0, "could not queue any command to flush"
        assert cp.flush_commands(PD_ADDR) > 0
        assert rec.wait_for_status(CompletionStatus.Flushed), \
            "no Flushed completion for a flushed command"

        # Aborted: commands still queued when the CP is torn down settle too.
        queued = _fill_queue(lambda: cp.submit_command(PD_ADDR, cmd))
        assert queued > 0, "could not queue any command to abort"
        cp.teardown()
        gc.collect()
        assert CompletionStatus.Aborted in rec.statuses(), \
            "no Aborted completion for a command queued at teardown"
    finally:
        _teardown(cp, pd, "completion-cp")


def test_pd_naks_an_out_of_spec_control_code():
    """A control code is a byte off the wire, so a peer can send one the spec
    does not define. The PD rejects it itself: the app never sees a code it
    cannot act on, and the CP is told the command failed."""
    cp, pd = _make_pair("nak-control-code")
    rec = Recorder()
    seen = []

    cp.set_command_completion_handler(rec.on_command_complete)
    pd.set_command_handler(lambda command: seen.append(command))

    try:
        pd.start()
        cp.start()
        assert cp.online_wait(PD_ADDR, timeout=10), "PD did not come online"

        # 200 is not an OutputControlCode; nothing downstream can honour it.
        bad = commands.Output(output_no=0, control_code=200, timer_count=0)
        assert cp.submit_command(PD_ADDR, bad)

        assert rec.wait_for_status(CompletionStatus.Failed), \
            "PD did not NAK a command carrying an undefined control code"
        assert not seen, \
            "PD handed the app a control code it cannot name"

        # A valid code on the same link still gets through, so the PD rejected
        # the command rather than wedging the connection.
        good = commands.Output(
            output_no=0,
            control_code=OutputControlCode.PermanentOff,
            timer_count=0,
        )
        assert cp.submit_command(PD_ADDR, good)
        assert rec.wait_for_status(CompletionStatus.Ok), \
            "PD did not accept a valid command after NAKing a bad one"
        assert seen == [good]
    finally:
        _teardown(cp, pd, "nak-control-code")


def test_pd_event_completion_statuses():
    """Every event a PD accepts settles exactly once: Ok once the CP takes it,
    Flushed if it is dropped from the queue, Aborted if the PD is torn down
    while it is still queued."""
    cp, pd = _make_pair("completion-pd")
    rec = Recorder()
    # The PD advertises OutputControl with one item, so a one-entry output
    # status is a valid event to exercise the completion machinery with.
    event = events.Status(
        type=StatusReportType.Output,
        report=bytes([1]),
    )

    pd.set_event_completion_handler(rec.on_event_complete)
    pd.set_command_handler(lambda command: None)

    try:
        pd.start()
        cp.start()
        assert cp.online_wait(PD_ADDR, timeout=10), "PD did not come online"

        # Ok: the event is picked up by the CP.
        assert pd.submit_event(event)
        assert rec.wait_for_status(CompletionStatus.Ok), \
            "no Ok completion for a delivered event"

        delivered, _ = rec.first_with_status(CompletionStatus.Ok)
        # The library hands back its own re-marshalled copy of the event.
        assert delivered == event
        assert delivered is not event

        # Flushed: queued events dropped by flush_events() still settle.
        queued = _fill_queue(lambda: pd.submit_event(event))
        assert queued > 0, "could not queue any event to flush"
        assert pd.flush_events() > 0
        assert rec.wait_for_status(CompletionStatus.Flushed), \
            "no Flushed completion for a flushed event"

        # Aborted: events still queued when the PD is torn down settle too.
        queued = _fill_queue(lambda: pd.submit_event(event))
        assert queued > 0, "could not queue any event to abort"
        pd.teardown()
        gc.collect()
        assert CompletionStatus.Aborted in rec.statuses(), \
            "no Aborted completion for an event queued at teardown"
    finally:
        _teardown(cp, pd, "completion-pd")


def test_file_tx_command_completes_at_multipart_done():
    """An engine-owned command settles when its operation ends, not when it is
    accepted: exactly one Ok completion, arriving no earlier than the
    MultipartDone that reports the transfer's outcome."""
    rec = Recorder()
    cp, pd = _make_pair("completion-mp-done",
                        cp_flags=[LibFlag.EnableNotification])
    mp_done = threading.Event()

    def on_event(address, event):
        if (isinstance(event, events.Notification)
                and event.type == NotificationType.MultipartDone):
            mp_done.set()
        return 0

    try:
        assert cp.register_file_ops(PD_ADDR, SenderFileOps())
        assert pd.register_file_ops(ReceiverFileOps())
        pd.start()
        cp.start()
        assert cp.online_wait(PD_ADDR, timeout=10), "PD did not come online"

        cp.set_event_handler(on_event)
        cp.set_command_completion_handler(rec.on_command_complete)

        assert cp.submit_command(PD_ADDR, commands.FileTransfer(id=FILE_ID))
        # The engine owns it until the transfer ends; nothing settles here.
        assert rec.statuses() == [], "file transfer completed at submit"

        assert rec.wait_for_status(CompletionStatus.Ok, timeout=10.0), (
            "no Ok completion for a finished file transfer"
        )
        assert mp_done.is_set(), "completion fired before MultipartDone"
        assert rec.statuses() == [CompletionStatus.Ok]
    finally:
        _teardown(cp, pd, "completion-mp-done")
