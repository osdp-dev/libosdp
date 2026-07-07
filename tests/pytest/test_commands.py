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

def cp_check_command_status(cmd, expected_outcome=True):
    event = {
        'event': Event.Notification,
        'type': Notification.Command,
        'arg0': cmd,
        'arg1': 0 if expected_outcome else -1,
    }
    wait_for_notification_event(cp, secure_pd.address, event)

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

def test_command_output():
    test_cmd = {
        'command': Command.Output,
        'output_no': 0,
        'control_code': 1,
        'timer_count': 0
    }
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(Command.Output)

def test_command_buzzer():
    test_cmd = {
        'command': Command.Buzzer,
        'reader': 0,
        'control_code': 1,
        'on_count': 10,
        'off_count': 10,
        'rep_count': 10
    }
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(Command.Buzzer)

def test_command_text():
    test_cmd = {
        'command': Command.Text,
        'reader': 0,
        'control_code': 1,
        'temp_time': 20,
        'offset_row': 1,
        'offset_col': 1,
        'data': 'PYOSDP'
    }
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(Command.Text)

def test_command_led():
    test_cmd = {
        'command': Command.LED,
        'reader': 1,
        'led_number': 0,
        'control_code': 1,
        'on_count': 10,
        'off_count': 10,
        'on_color': CommandLEDColor.Red,
        'off_color': CommandLEDColor.Black,
        'timer_count': 10,
        'temporary': True
    }
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(Command.LED)

    test_cmd['temporary'] = False
    del test_cmd['timer_count']

    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(Command.LED)

def test_command_comset():
    test_cmd = {
        'command': Command.Comset,
        'address': secure_pd_addr,
        'baud_rate': 9600
    }
    test_cmd_done = {
        'command': Command.ComsetDone,
        'address': secure_pd_addr,
        'baud_rate': 9600
    }
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    assert_command_received(secure_pd, test_cmd_done)
    cp_check_command_status(Command.Comset)

def test_command_mfg():
    test_cmd = {
        'command': Command.Manufacturer,
        'vendor_code': 0x00030201,
        'data': bytes([9,1,9,2,6,3,1,7,7,0])
    }
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(Command.Manufacturer)

def test_command_mfg_with_reply():
    """Test manufacturer command with asynchronous manufacturer reply event"""
    def evt_handler(pd, event):
        print(f"DEBUG: Received event: {event}")
        assert event['event'] == Event.ManufacturerReply
        assert event['vendor_code'] == 0x00030201
        assert event['data'] == bytes([9,1,9,2,6,3,1,7,7,0])
        return 0

    def cmd_handler(command):
        print(f"DEBUG: Received command: {command}")
        assert command['command'] == Command.Manufacturer
        assert command['vendor_code'] == 0x00030201
        assert command['data'] == bytes([9,1,9,2,6,3,1,7,7,0])
        return 0, None

    assert cp.is_online(secure_pd_addr)

    # Backup original handlers
    original_event_handler = getattr(cp, '_event_handler', None)
    original_command_handler = getattr(secure_pd, 'user_command_handler', None)

    try:
        cp.set_event_handler(evt_handler)
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = {
            'command': Command.Manufacturer,
            'vendor_code': 0x00030201,
            'data': bytes([9,1,9,2,6,3,1,7,7,0])
        }

        assert cp.submit_command(secure_pd_addr, test_cmd)

        # The command should be received by the PD (without mfg_command field)
        expected_cmd_received = {
            'command': Command.Manufacturer,
            'vendor_code': 0x00030201,
            'data': bytes([9,1,9,2,6,3,1,7,7,0])
        }
        assert_command_received(secure_pd, expected_cmd_received)

        cp_check_command_status(Command.Manufacturer)

        # Emit manufacturer reply event asynchronously from the PD app
        secure_pd.submit_event({
            'event': Event.ManufacturerReply,
            'vendor_code': 0x00030201,
            'data': bytes([9,1,9,2,6,3,1,7,7,0]),
        })

        # The manufacturer reply event should be received by the CP
        expected_event = {
            'event': Event.ManufacturerReply,
            'vendor_code': 0x00030201,
            'data': bytes([9,1,9,2,6,3,1,7,7,0])
        }
        wait_for_non_notification_event(cp, secure_pd_addr, expected_event)

    finally:
        # Restore original handlers
        if original_event_handler is not None:
            cp.set_event_handler(original_event_handler)
        else:
            cp.set_event_handler(None)

        if original_command_handler is not None:
            secure_pd.set_command_handler(original_command_handler)
        else:
            secure_pd.set_command_handler(None)

def test_command_mfg_nack_soft_fail():
    def cmd_handler(command):
        assert command['command'] == Command.Manufacturer
        return -1, None

    assert cp.is_online(secure_pd_addr)

    original_command_handler = getattr(secure_pd, 'user_command_handler', None)
    try:
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = {
            'command': Command.Manufacturer,
            'vendor_code': 0x00030201,
            'data': bytes([1, 2, 3, 4]),
        }
        assert cp.submit_command(secure_pd_addr, test_cmd)
        assert_command_received(secure_pd, test_cmd)
        cp_check_command_status(Command.Manufacturer, expected_outcome=False)

        # MFG NAK is a soft failure and does not drop PD online state.
        time.sleep(0.2)
        assert cp.is_online(secure_pd_addr)
    finally:
        if original_command_handler is not None:
            secure_pd.set_command_handler(original_command_handler)
        else:
            secure_pd.set_command_handler(None)

def test_command_keyset():
    test_cmd = {
        'command': Command.Keyset,
        'type': 1,
        'data': KeyStore.gen_key()
    }
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(Command.Keyset)

    # PD must be online and SC active after a KEYSET command
    time.sleep(0.5)
    assert cp.is_online(secure_pd_addr)
    assert cp.is_sc_active(secure_pd_addr)

    # When not in SC, KEYSET command should not be accepted.
    assert cp.is_sc_active(insecure_pd_addr) == False
    assert cp.submit_command(insecure_pd_addr, test_cmd) == False

def test_command_status():
    def evt_handler(pd, event):
        assert event['event'] == Event.Status
        assert event['type'] == StatusReportType.Input
        assert event['report'] == bytes([ 0, 1, 0, 1, 0, 1, 0, 1 ])
        return 0

    def cmd_handler(command):
        assert command['command'] == Command.Status
        assert command['type'] == StatusReportType.Input
        cmd = {
            'command': Command.Status,
            'type': StatusReportType.Input,
            'report': bytes([ 0, 1, 0, 1, 0, 1, 0, 1 ])
        }
        return 0, cmd

    assert cp.is_online(secure_pd_addr)

    # Backup original handlers
    original_event_handler = getattr(cp, '_event_handler', None)
    original_command_handler = getattr(secure_pd, 'user_command_handler', None)

    try:
        cp.set_event_handler(evt_handler)
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = {
            'command': Command.Status,
            'type': StatusReportType.Input,
            'report': bytes([]),
        }
        cp.submit_command(secure_pd_addr, test_cmd)

    finally:
        # Restore original handlers
        if original_event_handler is not None:
            cp.set_event_handler(original_event_handler)
        else:
            cp.set_event_handler(None)

        if original_command_handler is not None:
            secure_pd.set_command_handler(original_command_handler)
        else:
            secure_pd.set_command_handler(None)
