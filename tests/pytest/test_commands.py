#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import threading
import time
import pytest

from osdp import *
from osdp import commands, events
from conftest import (
    MultidropBus,
    assert_command_received,
    wait_for_non_notification_event,
)

secure_pd_addr = 101
insecure_pd_addr = 102

bus = MultidropBus(2)
chn = bus.multidrop_channel()

key = KeyStore.gen_key()

pd_cap_list = [
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 1, 1),
    (Capability.AudibleControl, 1, 1),
    (Capability.TextOutput, 1, 1),
    (Capability.Biometrics, 1, 1),
]
pd_cap = PDCapabilities(pd_cap_list)
tk_pd_cap = PDCapabilities(pd_cap_list + [(Capability.TimeKeeping, 1, 0)])

pd_info_list = [
    PDInfo(secure_pd_addr, chn, scbk=key, flags=[ LibFlag.EnforceSecure, LibFlag.EnableNotification ]),
    PDInfo(insecure_pd_addr, chn, flags=[ LibFlag.EnableNotification ])
]

secure_pd = PeripheralDevice(
    PDInfo(secure_pd_addr, bus.pd_channel(0), scbk=key, flags=[ LibFlag.EnforceSecure ]),
    tk_pd_cap,
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

completion_lock = threading.Lock()
completion_records = []

def on_command_complete(pd_address, command, status):
    with completion_lock:
        completion_records.append((pd_address, command.ID, status))

cp.set_command_completion_handler(on_command_complete)

@pytest.fixture(autouse=True)
def fresh_completion_records():
    """Start every test with an empty ledger.

    Not every test consumes the completions it provokes, so a leftover record
    from an earlier test could otherwise satisfy a later assertion without the
    command under test having completed at all.
    """
    with completion_lock:
        completion_records.clear()
    yield

def cp_check_command_status(cmd_id, expected_outcome=True, address=None,
                            timeout=5):
    """Wait for (and consume) a completion record for cmd_id.

    Consuming the match keeps repeated checks of the same command honest:
    each one needs a fresh completion, as the old per-event queue did.
    """
    status = CompletionStatus.Ok if expected_outcome else \
        CompletionStatus.Failed
    record = (secure_pd.address if address is None else address,
              cmd_id, status)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with completion_lock:
            if record in completion_records:
                completion_records.remove(record)
                return
        time.sleep(0.02)
    pytest.fail(f"no {status.name} completion for {cmd_id.name}")

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
    test_cmd = commands.Output(
        output_no=0,
        control_code=OutputControlCode.PermanentOff,
        timer_count=0,
    )
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(CommandId.Output)

def test_command_output_ostatr():
    """OSDP v2.2 6.9: the PD may answer osdp_OUT with an osdp_OSTATR reporting
    the new output state, by submitting the status from within its callback."""
    reply = events.Status(type=StatusReportType.Output, report=bytes([1]))

    def cmd_handler(command):
        assert isinstance(command, commands.Output)
        secure_pd.submit_event(reply)
        return None

    assert cp.is_online(secure_pd_addr)
    original = secure_pd.user_command_handler
    try:
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = commands.Output(
            output_no=0,
            control_code=OutputControlCode.PermanentOff,
            timer_count=0,
        )
        assert cp.submit_command(secure_pd_addr, test_cmd)
        assert_command_received(secure_pd, test_cmd)

        wait_for_non_notification_event(cp, secure_pd_addr, reply)
        assert cp.is_online(secure_pd_addr)
    finally:
        secure_pd.set_command_handler(original)

def test_command_buzzer():
    test_cmd = commands.Buzzer(
        control_code=BuzzerControlCode.Off,
        on_count=10,
        off_count=10,
        rep_count=10,
    )
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(CommandId.Buzzer)

def test_command_rejects_unadvertised_reader():
    """This PD advertises no attached readers (0 is itself), so a command that
    targets reader 1 is refused at submission and never reaches the wire."""
    test_cmd = commands.Buzzer(
        reader=1,
        control_code=BuzzerControlCode.Off,
        on_count=10,
        off_count=10,
        rep_count=10,
    )
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd) is False

def test_command_text():
    test_cmd = commands.Text(
        control_code=TextControlCode.PermanentNoWrap,
        temp_time=20,
        offset_row=1,
        offset_col=1,
        data='PYOSDP',
    )
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(CommandId.Text)

def test_command_tdset():
    test_cmd = commands.TDSet(
        year=2026, month=7, day=17, hour=12, minute=30, second=45,
    )
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(CommandId.TDSet)

def test_command_led_permanent():
    """A permanent state persists until something else changes it."""
    test_cmd = commands.LED(
        led_number=0,
        permanent=commands.PermanentLEDParams(
            control_code=PermanentLEDControlCode.Set,
            on_count=10,
            off_count=10,
            on_color=LEDColor.Red,
            off_color=LEDColor.Black,
        ),
    )
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(CommandId.LED)

def test_command_led_temporary_and_permanent():
    """Both blocks in one command: cancel the running temporary state and put
    the LED into a new permanent state. The old flat command could not say
    this; it carried a single control code and a `temporary` boolean."""
    flash = commands.LED(
        led_number=0,
        temporary=commands.TemporaryLEDParams(
            on_count=5,
            off_count=5,
            on_color=LEDColor.Red,
            off_color=LEDColor.Black,
            timer_count=100,
        ),
    )
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, flash)
    assert_command_received(secure_pd, flash)
    cp_check_command_status(CommandId.LED)

    cancel_and_set = commands.LED(
        led_number=0,
        temporary=commands.TemporaryLEDParams(
            control_code=TemporaryLEDControlCode.Cancel,
        ),
        permanent=commands.PermanentLEDParams(
            on_color=LEDColor.Green,
            on_count=1,
        ),
    )
    assert cp.submit_command(secure_pd_addr, cancel_and_set)
    received = assert_command_received(secure_pd, cancel_and_set)
    assert received.temporary is not None
    assert received.permanent is not None
    cp_check_command_status(CommandId.LED)

def test_command_comset():
    test_cmd = commands.Comset(address=secure_pd_addr, baud_rate=9600)
    test_cmd_done = commands.ComsetDone(address=secure_pd_addr, baud_rate=9600)
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    assert_command_received(secure_pd, test_cmd_done)
    cp_check_command_status(CommandId.Comset)

def test_command_mfg_with_reply():
    """Test manufacturer command with asynchronous manufacturer reply event"""
    payload = bytes([9,1,9,2,6,3,1,7,7,0])
    seen_events = []
    seen_commands = []

    def evt_handler(address, event):
        seen_events.append((address, event))
        return 0

    def cmd_handler(command):
        seen_commands.append(command)
        return None

    assert cp.is_online(secure_pd_addr)

    original_event_handler = cp.user_event_handler
    original_command_handler = secure_pd.user_command_handler

    try:
        cp.set_event_handler(evt_handler)
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = commands.Manufacturer(vendor_code=0x00030201, data=payload)

        assert cp.submit_command(secure_pd_addr, test_cmd)
        assert_command_received(secure_pd, test_cmd)
        assert test_cmd in seen_commands

        cp_check_command_status(CommandId.Manufacturer)

        # Emit manufacturer reply event asynchronously from the PD app
        reply = events.ManufacturerReply(vendor_code=0x00030201, data=payload)
        secure_pd.submit_event(reply)

        # The manufacturer reply event should be received by the CP
        wait_for_non_notification_event(cp, secure_pd_addr, reply)
        assert (secure_pd_addr, reply) in seen_events
    finally:
        cp.set_event_handler(original_event_handler)
        secure_pd.set_command_handler(original_command_handler)

def test_command_mfg_inline_error_reply():
    """PD answers from within its command handler; the reply must ride out on
    the MFG command itself rather than on a later poll."""
    payload = bytes([0xC0, 0xFF, 0xEE])
    reply = events.ManufacturerError(data=payload)

    def cmd_handler(command):
        assert isinstance(command, commands.Manufacturer)
        secure_pd.submit_event(reply)
        return None

    assert cp.is_online(secure_pd_addr)

    original_command_handler = secure_pd.user_command_handler
    try:
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = commands.Manufacturer(vendor_code=0x00030201, data=payload)
        assert cp.submit_command(secure_pd_addr, test_cmd)

        # Also drains the PD's command queue so it does not leak to other tests
        assert_command_received(secure_pd, test_cmd)

        wait_for_non_notification_event(cp, secure_pd_addr, reply)

        # An error reply fails the command but must not take the PD offline
        assert cp.is_online(secure_pd_addr)
    finally:
        secure_pd.set_command_handler(original_command_handler)

BIO_TEMPLATE = bytes([0xB1, 0x0C, 0x0D, 0xA7, 0xA0])

def test_command_bioread():
    """BIOREAD: the PD scans and returns a template from within its callback."""
    reply = events.BioRead(
        status=BioStatus.Success,
        type=BioType.RightIndexFingerPrint,
        quality=0xC0,
        data=BIO_TEMPLATE,
    )

    def cmd_handler(command):
        assert isinstance(command, commands.BioRead)
        assert command.type == BioType.RightIndexFingerPrint
        assert command.format == BioFormat.AnsiIncits378
        secure_pd.submit_event(reply)
        return None

    assert cp.is_online(secure_pd_addr)
    original = secure_pd.user_command_handler
    try:
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = commands.BioRead(
            type=BioType.RightIndexFingerPrint,
            format=BioFormat.AnsiIncits378,
            quality=0x80,
        )
        assert cp.submit_command(secure_pd_addr, test_cmd)
        assert_command_received(secure_pd, test_cmd)

        wait_for_non_notification_event(cp, secure_pd_addr, reply)
    finally:
        secure_pd.set_command_handler(original)

def test_command_biomatch():
    """BIOMATCH: the CP sends a template, the PD scans and returns a score."""
    reply = events.BioMatch(status=BioStatus.Success, score=0xFF)

    def cmd_handler(command):
        assert isinstance(command, commands.BioMatch)
        assert command.data == BIO_TEMPLATE
        secure_pd.submit_event(reply)
        return None

    assert cp.is_online(secure_pd_addr)
    original = secure_pd.user_command_handler
    try:
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = commands.BioMatch(
            type=BioType.RightIndexFingerPrint,
            format=BioFormat.AnsiIncits378,
            quality=0x80,
            data=BIO_TEMPLATE,
        )
        assert cp.submit_command(secure_pd_addr, test_cmd)
        assert_command_received(secure_pd, test_cmd)

        wait_for_non_notification_event(cp, secure_pd_addr, reply)
    finally:
        secure_pd.set_command_handler(original)

def test_command_bioread_nak_unsupported_type():
    """A PD that cannot scan the requested body part NAKs with BIO_TYPE. The
    NAK is a command failure but must not take the PD offline."""
    def cmd_handler(command):
        assert isinstance(command, commands.BioRead)
        raise NakError(NakCode.BioType)

    assert cp.is_online(secure_pd_addr)
    original = secure_pd.user_command_handler
    try:
        secure_pd.set_command_handler(cmd_handler)

        test_cmd = commands.BioRead(
            type=BioType.LeftRetinaScan,
            format=BioFormat.AnsiIncits378,
            quality=0x80,
        )
        assert cp.submit_command(secure_pd_addr, test_cmd)
        assert_command_received(secure_pd, test_cmd)

        cp_check_command_status(CommandId.BioRead, expected_outcome=False)
        assert cp.is_online(secure_pd_addr)
    finally:
        secure_pd.set_command_handler(original)

def test_command_keyset():
    test_cmd = commands.Keyset(type=1, data=KeyStore.gen_key())
    assert cp.is_online(secure_pd_addr)
    assert cp.submit_command(secure_pd_addr, test_cmd)
    assert_command_received(secure_pd, test_cmd)
    cp_check_command_status(CommandId.Keyset)

    # The CP drops SC on the ACK and re-runs the handshake on the new key
    assert cp.sc_wait(secure_pd_addr, timeout=5)
    assert cp.is_online(secure_pd_addr)

    # When not in SC, KEYSET command should not be accepted.
    assert cp.is_sc_active(insecure_pd_addr) == False
    assert cp.submit_command(insecure_pd_addr, test_cmd) == False

def test_command_status():
    """A PD answers a status query by returning a Status from its command
    handler; the report it puts there must reach the CP intact."""
    report = bytes([0, 1])

    def cmd_handler(command):
        if isinstance(command, commands.Status):
            return commands.Status(type=command.type, report=report)
        return None

    assert cp.is_online(secure_pd_addr)

    original_command_handler = secure_pd.user_command_handler
    try:
        secure_pd.set_command_handler(cmd_handler)

        query = commands.Status(type=StatusReportType.Local)
        assert cp.submit_command(secure_pd_addr, query)

        # The PD sees the query with an empty report ...
        received = assert_command_received(secure_pd, query)
        assert received.report == b''

        # ... and the CP gets the report the PD answered with.
        event = wait_for_non_notification_event(
            cp, secure_pd_addr,
            events.Status(type=StatusReportType.Local, report=report),
        )
        assert isinstance(event, events.Status)
        assert event.type == StatusReportType.Local
        assert event.report == report

        cp_check_command_status(CommandId.Status)
    finally:
        secure_pd.set_command_handler(original_command_handler)

def test_command_nak_keeps_pd_online():
    """A PD that declines a command is not a broken link. The app must be told
    the command failed, but the PD has to stay online."""
    declined = []

    def cmd_handler(command):
        declined.append(command.ID)
        raise NakError(NakCode.Record)

    test_cmd = commands.Output(
        output_no=0,
        control_code=OutputControlCode.PermanentOff,
        timer_count=0,
    )
    assert cp.is_online(secure_pd_addr)
    original = secure_pd.user_command_handler
    try:
        secure_pd.set_command_handler(cmd_handler)

        assert cp.submit_command(secure_pd_addr, test_cmd)

        cp_check_command_status(CommandId.Output, expected_outcome=False)
        assert declined == [CommandId.Output]
        assert cp.is_online(secure_pd_addr)
    finally:
        secure_pd.set_command_handler(original)
