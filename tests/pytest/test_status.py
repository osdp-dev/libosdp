#
#  Copyright (c) 2023-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time
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

def test_cp_online_status():
    """Test CP's ability to detect PD online status"""
    # Verify PD is online and CP can detect it
    assert cp.is_online(pd_addr), "PD should be online"

    # Test online wait functionality
    assert cp.online_wait(pd_addr, timeout=1), "online_wait should return True for already online PD"

    # Test status bitmask includes this PD
    online_mask = cp.status()
    assert online_mask & (1 << 0), "PD should be set in online bitmask"

def test_cp_sc_status():
    """Test CP's ability to detect PD secure channel status"""
    # Verify SC is active and CP can detect it
    assert cp.is_sc_active(pd_addr), "Secure channel should be active"

    # Test SC wait functionality
    assert cp.sc_wait(pd_addr, timeout=1), "sc_wait should return True for already active SC"

    # Test SC status bitmask includes this PD
    sc_mask = cp.sc_status()
    assert sc_mask & (1 << 0), "PD should be set in SC bitmask"

def test_pd_online_status():
    """Test PD's ability to detect CP online status"""
    # Verify PD can detect that CP is online
    assert pd.is_online(), "PD should be online"

def test_pd_sc_status():
    """Test PD's ability to detect secure channel status"""
    # Verify PD can detect that SC is active
    assert pd.is_sc_active(), "Secure channel should be active"

def test_status_bitmasks():
    """Test status bitmask methods and counting functions"""
    # Test online status bitmask
    online_mask = cp.status()
    assert online_mask & (1 << 0), "PD should be set in online bitmask"
    assert cp.get_num_online() == 1, "Should have 1 PD online"

    # Test SC status bitmask
    sc_mask = cp.sc_status()
    assert sc_mask & (1 << 0), "PD should be set in SC bitmask"
    assert cp.get_num_sc_active() == 1, "Should have 1 PD with active SC"

def test_status_wait_methods():
    """Test various wait methods for status monitoring"""
    # Test waiting for already online PDs
    assert cp.online_wait_all(timeout=1), "online_wait_all should return True for already online PDs"
    assert cp.sc_wait_all(timeout=1), "sc_wait_all should return True for already active SCs"

    # Test individual wait methods
    assert cp.online_wait(pd_addr, timeout=1), "online_wait should return True for already online PD"
    assert cp.sc_wait(pd_addr, timeout=1), "sc_wait should return True for already active SC"

def test_metrics_access_and_reset():
    """Test metrics access from both CP and PD wrappers."""
    deadline = time.time() + 5
    cp_metrics = None
    pd_metrics = None

    while time.time() < deadline:
        cp_metrics = cp.get_metrics(pd_addr)
        pd_metrics = pd.get_metrics()
        if cp_metrics["packets_sent"] or cp_metrics["packets_received"]:
            break
        time.sleep(0.1)

    assert cp_metrics is not None
    assert pd_metrics is not None
    assert set(cp_metrics.keys()) == {
        "packets_sent",
        "packets_received",
        "packet_check_errors",
        "nak_count",
        "sc_handshake_count",
        "sc_failure_count",
        "command_count",
        "event_count",
    }
    assert set(pd_metrics.keys()) == set(cp_metrics.keys())

    # The API provides interval metrics, so a second read after the first
    # snapshot should reset counters back toward zero.
    next_cp_metrics = cp.get_metrics(pd_addr)
    next_pd_metrics = pd.get_metrics()
    assert sum(next_cp_metrics.values()) <= sum(cp_metrics.values())
    assert sum(next_pd_metrics.values()) <= sum(pd_metrics.values())

def test_pd_info_default_baud_rate():
    info = PDInfo(101, f1, scbk=key).get()
    assert info["baud_rate"] == 9600
