#
#  Copyright (c) 2023-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time
from dataclasses import astuple, fields

import pytest

from osdp import *
from conftest import make_fifo_pair, cleanup_fifo_pair

pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 1, 1),
    (Capability.AudibleControl, 1, 1),
    (Capability.TextOutput, 1, 1),
])

key = KeyStore.gen_key()
f1, f2 = make_fifo_pair("status")

pd_addr = 101
pd_info_list = [PDInfo(pd_addr, f2, scbk=key, flags=[LibFlag.EnforceSecure, LibFlag.EnableNotification])]

pd = PeripheralDevice(
    PDInfo(pd_addr, f1, scbk=key, flags=[LibFlag.EnforceSecure]),
    pd_cap,
    log_level=LogLevel.Debug
)
cp = ControlPanel(pd_info_list, log_level=LogLevel.Debug)

@pytest.fixture(scope='module', autouse=True)
def setup_test():
    pd.start()
    cp.start()
    if not cp.sc_wait_all(timeout=10):
        teardown_test()
        pytest.fail("Failed to establish secure channel within timeout")
    yield
    teardown_test()

def teardown_test():
    try:
        if cp.thread:
            cp.teardown()
    except RuntimeError:
        pass  # Already stopped
    try:
        if pd.thread:
            pd.teardown()
    except RuntimeError:
        pass  # Already stopped
    cleanup_fifo_pair("status")

def test_status_of_a_secured_pd():
    """Every status accessor on both sides agrees the PD is online with SC."""
    assert cp.is_online(pd_addr)
    assert cp.is_sc_active(pd_addr)
    assert cp.status() & (1 << 0), "PD missing from the online bitmask"
    assert cp.sc_status() & (1 << 0), "PD missing from the SC bitmask"
    assert cp.get_num_online() == 1
    assert cp.get_num_sc_active() == 1

    # Already satisfied, so each returns at once
    assert cp.online_wait(pd_addr, timeout=1)
    assert cp.sc_wait(pd_addr, timeout=1)
    assert cp.online_wait_all(timeout=1)
    assert cp.sc_wait_all(timeout=1)

    assert pd.is_online()
    assert pd.is_sc_active()

def test_metrics_access_and_reset():
    """Test metrics access from both CP and PD wrappers."""
    deadline = time.time() + 5
    cp_metrics = None
    pd_metrics = None

    while time.time() < deadline:
        cp_metrics = cp.get_metrics(pd_addr)
        pd_metrics = pd.get_metrics()
        if cp_metrics and (cp_metrics.packets_sent or cp_metrics.packets_received):
            break
        time.sleep(0.1)

    assert cp_metrics is not None
    assert pd_metrics is not None
    assert isinstance(cp_metrics, Metrics)
    assert isinstance(pd_metrics, Metrics)
    assert {f.name for f in fields(Metrics)} == {
        "packets_sent",
        "packets_received",
        "packet_check_errors",
        "nak_count",
        "sc_handshake_count",
        "sc_failure_count",
        "command_count",
        "event_count",
    }

    # The API provides interval metrics, so a second read after the first
    # snapshot should reset counters back toward zero.
    next_cp_metrics = cp.get_metrics(pd_addr)
    next_pd_metrics = pd.get_metrics()
    assert sum(astuple(next_cp_metrics)) <= sum(astuple(cp_metrics))
    assert sum(astuple(next_pd_metrics)) <= sum(astuple(pd_metrics))
