#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time
import pytest

from osdp import *
from conftest import (
    MultidropBus,
    assert_command_received,
    wait_for_notification_event,
    wait_for_non_notification_event,
)

secure_pd_addr = 101
insecure_pd_addr = 102

bus = MultidropBus(2)
chn = bus.multidrop_channel()

key = KeyStore.gen_key()

pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 1, 1),
    (Capability.AudibleControl, 1, 1),
    (Capability.TextOutput, 1, 1),
])

pd_info_list = [
    PDInfo(secure_pd_addr, chn, scbk=key, flags=[ LibFlag.EnforceSecure, LibFlag.EnableNotification ]),
    PDInfo(insecure_pd_addr, chn, flags=[ LibFlag.EnableNotification ])
]

secure_pd = PeripheralDevice(
    PDInfo(secure_pd_addr, bus.pd_channel(0), scbk=key, flags=[ LibFlag.EnforceSecure ]),
    pd_cap,
    log_level=LogLevel.Debug
)

insecure_pd = PeripheralDevice(
    PDInfo(insecure_pd_addr, bus.pd_channel(1)),
    pd_cap,
    log_level=LogLevel.Debug
)

pd_list = [
    secure_pd,
    insecure_pd,
]

cp = ControlPanel(pd_info_list, log_level=LogLevel.Debug)

@pytest.fixture(scope='module', autouse=True)
def setup_test():
    for pd in pd_list:
        pd.start()
    cp.start()
    if not cp.online_wait_all(timeout=10):
        teardown_test()
        pytest.fail("Failed to bring all PDs online within timeout")
    yield
    teardown_test()

def teardown_test():
    cp.teardown()
    for pd in pd_list:
        pd.teardown()

def test_hotplug_enable_already_enabled():
    assert cp.is_online(secure_pd_addr)
    assert cp.is_pd_enabled(secure_pd_addr) == True

    result = cp.enable_pd(secure_pd_addr)
    assert result == False, "enable_pd should return False when PD is already enabled"

def test_hotplug_disable_functionality():
    assert cp.is_online(secure_pd_addr)
    assert cp.is_pd_enabled(secure_pd_addr) == True

    result = cp.disable_pd(secure_pd_addr)
    assert result == True, "disable_pd should return True on success"

    # Allow time for state change processing
    time.sleep(0.5)

    assert cp.is_pd_enabled(secure_pd_addr) == False, "PD should be disabled after disable_pd"

def test_hotplug_enable_after_disable():
    cp.disable_pd(secure_pd_addr)
    time.sleep(0.5)
    assert cp.is_pd_enabled(secure_pd_addr) == False

    result = cp.enable_pd(secure_pd_addr)
    assert result == True, "enable_pd should return True on success"

    # Allow time for state change processing and re-initialization
    time.sleep(1)

    assert cp.is_pd_enabled(secure_pd_addr) == True, "PD should be enabled after enable_pd"

def test_hotplug_command_blocking():
    assert cp.is_pd_enabled(secure_pd_addr) == True

    cmd = {
        'command': Command.Buzzer,
        'reader': 0,
        'control_code': 1,
        'on_count': 1,
        'off_count': 1,
        'rep_count': 1
    }

    initial_result = cp.submit_command(secure_pd_addr, cmd)
    assert initial_result == True, "Commands should be accepted for enabled PDs"

    cp.disable_pd(secure_pd_addr)

    # Allow time for state change processing
    time.sleep(0.5)

    assert cp.is_pd_enabled(secure_pd_addr) == False

    disabled_result = cp.submit_command(secure_pd_addr, cmd)
    assert disabled_result == False, "Commands should be blocked for disabled PDs"
