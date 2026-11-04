#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
# LibFlags whose effect is only visible on a live link. How modify_flag
# refuses a change is checked without threads in test_api_errors.py.

import pytest

from osdp import (
    Capability,
    ControlPanel,
    KeyStore,
    LibFlag,
    LogLevel,
    PDCapabilities,
    PDInfo,
    PeripheralDevice,
    commands,
)

CAPABILITIES = PDCapabilities([(Capability.OutputControl, 1, 8)])


@pytest.fixture
def pair(fifo_pair):
    devices = []

    def make(cp_flags=None, pd_flags=None):
        cp_chan, pd_chan = fifo_pair()
        key = KeyStore.gen_key()
        pd = PeripheralDevice(
            PDInfo(101, pd_chan, scbk=key, flags=pd_flags or []),
            CAPABILITIES,
            log_level=LogLevel.Error,
        )
        pd.start()
        cp = ControlPanel(
            [PDInfo(101, cp_chan, scbk=key, flags=cp_flags or [])],
            log_level=LogLevel.Error,
        )
        cp.start()
        devices.extend([cp, pd])
        assert cp.online_wait(101, timeout=10), "PD never came online"
        return cp, pd

    yield make

    for device in reversed(devices):
        if device.thread:
            device.teardown()


def test_capture_packets_writes_a_pcap(pair, tmp_path, monkeypatch):
    # osdp_diag.c names the file osdp-trace-<...>.pcap and drops it in the
    # working directory, so give it one of its own.
    monkeypatch.chdir(tmp_path)

    cp, pd = pair(cp_flags=[LibFlag.CapturePackets])
    command = commands.Output(output_no=3, control_code=1, timer_count=10)
    assert cp.submit_command(101, command)
    assert pd.get_command(timeout=5) == command

    # The capture is finalised when the context goes away.
    cp.teardown()

    traces = list(tmp_path.glob("osdp-trace-*.pcap"))
    assert traces, f"no pcap written into {tmp_path}"
    assert traces[0].stat().st_size > 0, "the pcap is empty"


def test_a_flag_can_be_set_and_cleared_at_runtime(pair):
    cp, pd = pair()
    flag = LibFlag.IgnoreUnsolicited

    assert cp.set_flag(101, flag)
    assert cp.clear_flag(101, flag)
    assert cp.set_flag(101, flag)

    # Toggling a flag on a live link must not disturb it.
    assert cp.is_online(101)
    command = commands.Output(output_no=4, control_code=1, timer_count=10)
    assert cp.submit_command(101, command)
    assert pd.get_command(timeout=5) == command


def test_enforce_secure_keeps_an_unkeyed_pd_offline(fifo_pair):
    # An unkeyed PD reports communication security at compliance level 0, so
    # the CP has to refuse it at capability detection. Without the flag the
    # same pair is online in plaintext well inside the wait below.
    cp_chan, pd_chan = fifo_pair()
    pd = PeripheralDevice(
        PDInfo(101, pd_chan), CAPABILITIES, log_level=LogLevel.Error,
    )
    pd.start()
    cp = ControlPanel(
        [PDInfo(101, cp_chan, scbk=KeyStore.gen_key(),
                flags=[LibFlag.EnforceSecure])],
        log_level=LogLevel.Error,
    )
    cp.start()
    try:
        assert not cp.online_wait(101, timeout=2), "an unkeyed PD came online"
    finally:
        cp.teardown()
        pd.teardown()
