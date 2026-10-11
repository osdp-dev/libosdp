#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import threading
import time
import pytest

from osdp import *
from osdp import commands, events
from conftest import make_fifo_pair, cleanup_fifo_pair

pd_addr = 101

key = KeyStore.gen_key()
f1, f2 = make_fifo_pair("trs")

# Bit 0 of the smart-card compliance level is what tells a CP the PD speaks TRS
pd_cap = PDCapabilities([
    (Capability.SmartCard, 1, 1),
])

pd = PeripheralDevice(
    PDInfo(pd_addr, f1, scbk=key,
           flags=[LibFlag.EnforceSecure, LibFlag.EnableNotification]),
    pd_cap,
    log_level=LogLevel.Debug,
)
cp = ControlPanel(
    [PDInfo(pd_addr, f2, scbk=key,
            flags=[LibFlag.EnforceSecure, LibFlag.EnableNotification])],
    log_level=LogLevel.Debug,
)

C_APDU = bytes([0x00, 0xA4, 0x04, 0x00, 0x0E])
R_APDU = bytes([0x6F, 0x1A, 0x84, 0x0E, 0x90, 0x00])


class PDApp:
    """What the PD app is told, and how it answers card commands."""

    def __init__(self):
        self.lock = threading.Lock()
        self.trs_statuses = []
        self.trs_commands = []
        self.card_status = TrsCardStatus.Present
        self.defer = False

    def reset(self):
        with self.lock:
            self.trs_statuses = []
            self.trs_commands = []
        self.defer = False

    def statuses(self):
        with self.lock:
            return list(self.trs_statuses)

    def handler(self, command):
        match command:
            case commands.Notification(type=NotificationType.TrsStatus):
                with self.lock:
                    self.trs_statuses.append(command.trs_status)
            case commands.TrsSendApdu():
                self._record(command)
                if not self.defer:
                    pd.submit_event(events.TrsCardData(apdu=R_APDU))
            case commands.TrsEnterPin():
                self._record(command)
                pd.submit_event(events.TrsPinComplete(reader_status=0, tries=1))
            case commands.TrsCardScan():
                self._record(command)
                pd.submit_event(
                    events.TrsCardPresent(status=self.card_status)
                )
        return None

    def _record(self, command):
        with self.lock:
            self.trs_commands.append(command)


pd_app = PDApp()
completions = []


def on_completion(address, command, status):
    if command.ID == CommandId.Trs:
        completions.append((command, status))


@pytest.fixture(scope='module', autouse=True)
def setup_test():
    pd.set_command_handler(pd_app.handler)
    cp.set_command_completion_handler(on_completion)
    pd.start()
    cp.start()
    if not cp.sc_wait_all():
        teardown_test()
        pytest.fail("Failed to establish secure channel within timeout")
    yield
    teardown_test()


def teardown_test():
    cp.teardown()
    pd.teardown()
    cleanup_fifo_pair("trs")


@pytest.fixture(autouse=True)
def fresh_records():
    while cp.get_event(pd_addr, timeout=0.1) is not None:
        pass
    pd_app.reset()
    completions.clear()
    yield
    cp.trs_scan_disable(pd_addr)


def collect_events(until, timeout=5):
    """Gather CP events until `until(collected)` holds, or fail."""
    collected = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        event = cp.get_event(pd_addr, timeout=0.1)
        if event is not None:
            collected.append(event)
            if until(collected):
                return collected
    pytest.fail(f"Timed out; CP saw {collected}")


def trs_statuses(collected):
    return [
        e.trs_status for e in collected
        if isinstance(e, events.Notification)
        and e.type == NotificationType.TrsStatus
    ]


def trs_replies(collected):
    return [e for e in collected if type(e).ID == EventId.Trs]


def wait_for_pd_statuses(want, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pd_app.statuses() == want:
            return
        time.sleep(0.05)
    assert pd_app.statuses() == want


def run_band(*card_commands):
    """Open a band, run the card commands in it, close it; return CP events."""
    assert cp.submit_command(pd_addr, commands.TrsStart())
    for command in card_commands:
        assert cp.submit_command(pd_addr, command)
    collected = collect_events(
        lambda c: len(trs_replies(c)) == len(card_commands)
    )
    assert cp.submit_command(pd_addr, commands.TrsStop())
    collected += collect_events(
        lambda c: TrsSessionStatus.Closed in trs_statuses(c)
    )
    return collected


def test_trs_is_advertised():
    assert cp.check_capability(pd_addr, Capability.SmartCard) == (1, 1)
    assert 0 < cp.trs_get_max_apdu_len(pd_addr) <= commands.MAX_TRS_APDU_LEN


def test_trs_band_send_apdu():
    send = commands.TrsSendApdu(apdu=C_APDU)

    collected = run_band(send)

    assert trs_statuses(collected) == [
        TrsSessionStatus.Opened,
        TrsSessionStatus.Closed,
    ]
    assert trs_replies(collected) == [events.TrsCardData(apdu=R_APDU)]
    assert pd_app.trs_commands == [send]
    assert (send, CompletionStatus.Ok) in completions
    wait_for_pd_statuses([
        TrsSessionStatus.Opened,
        TrsSessionStatus.CardReleased,
        TrsSessionStatus.Closed,
    ])


def test_trs_band_send_apdu_deferred():
    # A slow card: the PD acknowledges the APDU and answers on a later poll,
    # and the CP holds the band until it does.
    pd_app.defer = True
    send = commands.TrsSendApdu(apdu=C_APDU)
    assert cp.submit_command(pd_addr, commands.TrsStart())
    assert cp.submit_command(pd_addr, send)

    deadline = time.monotonic() + 5
    while send not in pd_app.trs_commands and time.monotonic() < deadline:
        time.sleep(0.05)
    assert pd_app.trs_commands == [send]
    time.sleep(0.2)
    assert pd.submit_event(events.TrsCardData(reader_status=1, apdu=R_APDU))

    collected = collect_events(lambda c: len(trs_replies(c)) == 1)
    assert trs_replies(collected) == [
        events.TrsCardData(reader_status=1, apdu=R_APDU)
    ]

    assert cp.submit_command(pd_addr, commands.TrsStop())
    collect_events(lambda c: TrsSessionStatus.Closed in trs_statuses(c))
    assert (send, CompletionStatus.Ok) in completions


def test_trs_band_enter_pin():
    # Positions must be wire-representable: byte-aligned, or under 16 bits
    enter_pin = commands.TrsEnterPin(
        timeout_initial=30,
        timeout_digit=5,
        pin_block=commands.TrsPinBlock(
            format=TrsPinFormat.BCD, right_justify=True, offset_bits=40,
            size_bytes=8,
        ),
        pin_length_field=commands.TrsPinLengthField(
            size_bits=4, offset_bits=4
        ),
        min_digits=4,
        max_digits=12,
        complete_on=TrsPinComplete.Key | TrsPinComplete.MaxDigits,
        num_messages=1,
        language_id=0x0409,
        msg_index=2,
        teo_prologue=b"\x00\x40\x0d",
        apdu=bytes([0x00, 0x20, 0x00, 0x80, 0x08]) + bytes(8),
    )

    collected = run_band(enter_pin)

    assert trs_replies(collected) == [events.TrsPinComplete(tries=1)]
    assert pd_app.trs_commands == [enter_pin]
    assert (enter_pin, CompletionStatus.Ok) in completions


def test_trs_card_command_outside_a_band_is_refused():
    assert not cp.submit_command(pd_addr, commands.TrsSendApdu(apdu=C_APDU))


def test_trs_scan_reports_card_present():
    pd_app.card_status = TrsCardStatus.PresentContactless
    assert cp.trs_scan_enable(pd_addr, mode0_dwell_ms=100,
                              mode1_dwell_ms=200, hold_ms=500)

    collected = collect_events(
        lambda c: any(isinstance(e, events.TrsCardPresent) for e in c)
    )

    assert events.TrsCardPresent(
        status=TrsCardStatus.PresentContactless
    ) in collected
    assert commands.TrsCardScan() in pd_app.trs_commands
    assert cp.trs_scan_disable(pd_addr)


def test_trs_scan_rejects_out_of_range_times():
    with pytest.raises(ValueError):
        cp.trs_scan_enable(pd_addr, mode0_dwell_ms=0x10000)
