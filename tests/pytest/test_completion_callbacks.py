#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import gc
import time

import osdp_sys
import pytest

from osdp import *
from conftest import make_fifo_pair, cleanup_fifo_pair


def _bring_online(cp, pd, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        cp.refresh()
        pd.refresh()
        if cp.status() & 0x1:
            return
        time.sleep(0.01)
    pytest.fail("Timed out waiting for CP/PD to come online")


def _pump_until(predicate, cp, pd, timeout=2.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        cp.refresh()
        pd.refresh()
        if predicate():
            return
        time.sleep(0.01)
    pytest.fail("Timed out waiting for completion callback")


def _make_low_level_pair(name):
    cp_channel, pd_channel = make_fifo_pair(name)
    pd_info_cp = PDInfo(101, cp_channel, flags=[]).get()
    pd_info_pd = PDInfo(101, pd_channel, flags=[]).get()
    pd_cap = PDCapabilities([(Capability.OutputControl, 1, 1)]).get()
    cp = osdp_sys.ControlPanel([pd_info_cp])
    pd = osdp_sys.PeripheralDevice(pd_info_pd, capabilities=pd_cap)
    return cp, pd


def test_cp_command_completion_statuses():
    cp = None
    pd = None
    records = []
    cmd = {
        'command': Command.Output,
        'output_no': 0,
        'control_code': 1,
        'timer_count': 0,
    }

    def on_command(command):
        return 0, None

    def on_complete(pd_idx, command_dict, status):
        records.append((pd_idx, command_dict, status))

    try:
        cp, pd = _make_low_level_pair("completion-cp")
        pd.set_command_callback(on_command)
        cp.set_command_completion_callback(on_complete)
        _bring_online(cp, pd)

        assert cp.submit_command(0, cmd)
        _pump_until(lambda: any(r[2] == CompletionStatus.Ok for r in records), cp, pd)

        assert cp.submit_command(0, cmd)
        assert cp.flush_commands(0) == 1
        assert any(r[2] == CompletionStatus.Flushed for r in records)

        assert cp.submit_command(0, cmd)
        cp = None
        gc.collect()
        assert any(r[2] == CompletionStatus.Aborted for r in records)
    finally:
        cp = None
        pd = None
        gc.collect()
        cleanup_fifo_pair("completion-cp")


def test_pd_event_completion_statuses():
    cp = None
    pd = None
    records = []
    event = {
        'event': Event.Status,
        'type': StatusReportType.Input,
        'report': bytes([1, 0, 1, 0]),
    }

    def on_command(command):
        return 0, None

    def on_complete(event_dict, status):
        records.append((event_dict, status))

    try:
        cp, pd = _make_low_level_pair("completion-pd")
        pd.set_command_callback(on_command)
        pd.set_event_completion_callback(on_complete)
        _bring_online(cp, pd)

        assert pd.submit_event(event)
        _pump_until(lambda: any(r[1] == CompletionStatus.Ok for r in records), cp, pd)

        assert pd.submit_event(event)
        assert pd.flush_events() == 1
        assert any(r[1] == CompletionStatus.Flushed for r in records)

        assert pd.submit_event(event)
        pd = None
        gc.collect()
        assert any(r[1] == CompletionStatus.Aborted for r in records)
    finally:
        cp = None
        pd = None
        gc.collect()
        cleanup_fifo_pair("completion-pd")
