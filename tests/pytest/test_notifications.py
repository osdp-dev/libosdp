#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time
import threading
import pytest

from osdp import *
from osdp import commands, events
from conftest import make_fifo_pair, cleanup_fifo_pair


PD_ADDR = 101


def _fresh_pair(name, enable_notification_on_pd=True,
                enable_notification_on_cp=True):
    key = KeyStore.gen_key()
    f1, f2 = make_fifo_pair(name)

    pd_flags = [LibFlag.EnforceSecure]
    if enable_notification_on_pd:
        pd_flags.append(LibFlag.EnableNotification)
    cp_flags = [LibFlag.EnforceSecure]
    if enable_notification_on_cp:
        cp_flags.append(LibFlag.EnableNotification)

    pd = PeripheralDevice(
        PDInfo(PD_ADDR, f1, scbk=key, flags=pd_flags),
        PDCapabilities([]),
        log_level=LogLevel.Debug,
    )
    cp = ControlPanel(
        [PDInfo(PD_ADDR, f2, scbk=key, flags=cp_flags)],
        log_level=LogLevel.Debug,
    )
    return cp, pd


def _teardown_pair(cp, pd, name):
    try:
        cp.teardown()
    except RuntimeError:
        pass
    try:
        pd.teardown()
    except RuntimeError:
        pass
    cleanup_fifo_pair(name)


class NotifRecorder:
    """Mirrors pd_cmd_cb in tests/unit-tests/test-notifications.c"""

    def __init__(self):
        self.lock = threading.Lock()
        self.pd_status_count = 0
        self.pd_status_last = None
        self.sc_status_count = 0
        self.sc_status_last = None

    def handler(self, cmd):
        if not isinstance(cmd, commands.Notification):
            return None
        with self.lock:
            if cmd.type == NotificationType.PeripheralDeviceStatus:
                self.pd_status_count += 1
                self.pd_status_last = cmd
            elif cmd.type == NotificationType.SecureChannelStatus:
                self.sc_status_count += 1
                self.sc_status_last = cmd
        return None

    def wait_pd_status(self, timeout):
        return self._wait("pd_status_count", timeout)

    def wait_sc_status(self, timeout):
        return self._wait("sc_status_count", timeout)

    def _wait(self, attr, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if getattr(self, attr) > 0:
                    return True
            time.sleep(0.05)
        return False


def test_pd_receives_pd_status_and_sc_status_notifications():
    """On SC handshake, PD sees PD_STATUS(online=1) and SC_STATUS(active=1)
    via its command callback."""
    cp, pd = _fresh_pair("notif-pd")
    rec = NotifRecorder()
    pd.set_command_handler(rec.handler)
    try:
        pd.start()
        cp.start()
        assert cp.sc_wait_all(timeout=10), "SC handshake did not complete"

        assert rec.wait_pd_status(timeout=5), \
            "PD command callback never saw a PD_STATUS notification"
        assert rec.wait_sc_status(timeout=5), \
            "PD command callback never saw an SC_STATUS notification"

        with rec.lock:
            assert rec.pd_status_last.type == \
                NotificationType.PeripheralDeviceStatus
            assert rec.pd_status_last.online, \
                f"PD_STATUS online={rec.pd_status_last.online}, want True"
            assert rec.sc_status_last.type == \
                NotificationType.SecureChannelStatus
            assert rec.sc_status_last.active, \
                f"SC_STATUS active={rec.sc_status_last.active}, want True"
    finally:
        _teardown_pair(cp, pd, "notif-pd")


def test_state_is_tracked_without_forwarding_notifications():
    """Notifications feed the bookkeeping behind is_online()/is_sc_active()
    whether or not the caller asked to see them -- and a caller who did not
    ask must not find them turning up in its own event/command stream.
    """
    cp, pd = _fresh_pair("notif-optout", enable_notification_on_pd=False,
                         enable_notification_on_cp=False)
    try:
        pd.start()
        cp.start()

        ## These are answered purely from notification-fed state.
        assert cp.online_wait(PD_ADDR, timeout=10), "CP never saw PD online"
        assert cp.sc_wait(PD_ADDR, timeout=10), "CP never saw SC come up"
        assert cp.is_online(PD_ADDR)
        assert cp.is_sc_active(PD_ADDR)
        assert pd.is_online()
        assert pd.is_sc_active()

        ## ...without any of them leaking to a caller who opted out.
        while (event := cp.get_event(PD_ADDR, timeout=0.2)) is not None:
            assert not isinstance(event, events.Notification), \
                f"notification leaked into the CP event stream: {event}"
        while (cmd := pd.get_command(timeout=0.2)) is not None:
            assert not isinstance(cmd, commands.Notification), \
                f"notification leaked into the PD command stream: {cmd}"
    finally:
        _teardown_pair(cp, pd, "notif-optout")


def test_cp_reports_offline_while_re_running_sc_handshake():
    """A rekey drops the PD out of ONLINE to redo the SC handshake.

    Commands are refused for that whole window, so the CP must report the PD
    offline going in and online coming back out -- otherwise is_online() would
    disagree with what submit_command() accepts.
    """
    cp, pd = _fresh_pair("notif-rekey")
    seen = []
    lock = threading.Lock()

    def on_event(address, event):
        if isinstance(event, events.Notification) and \
                event.type == NotificationType.PeripheralDeviceStatus:
            with lock:
                seen.append(event.online)
        return 0

    try:
        pd.start()
        cp.start()
        assert cp.sc_wait_all(timeout=10), "SC handshake did not complete"

        cp.set_event_handler(on_event)
        with lock:
            seen.clear()

        ## Rekeying restarts the secure channel, taking the PD out of ONLINE.
        assert cp.submit_command(
            PD_ADDR, commands.Keyset(type=1, data=KeyStore.gen_key())
        )

        deadline = time.time() + 10
        while time.time() < deadline:
            with lock:
                if seen[:2] == [False, True]:
                    break
            time.sleep(0.05)

        with lock:
            assert seen[:2] == [False, True], \
                f"want offline then online across the rekey, got {seen}"
    finally:
        _teardown_pair(cp, pd, "notif-rekey")


# -----------------------------------------------------------------------------
# PD id notification
# -----------------------------------------------------------------------------

def _wait_pd_id(cp, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = deadline - time.time()
        e = cp.get_event(PD_ADDR, timeout=max(0.05, remaining))
        if e is None:
            continue
        if isinstance(e, events.Notification) and \
                e.type == NotificationType.PdId:
            return e
    pytest.fail("PdId notification not received")


def test_cp_receives_pd_id_notification():
    """CP side: on connect the CP collects the PD's identity during the INIT
    handshake and delivers it to the app as a PdId notification carrying the
    reported fields."""
    key = KeyStore.gen_key()
    f1, f2 = make_fifo_pair("notif-pdid")
    want = PdId(version=2, model=42, vendor_code=0x00CAFE,
                serial_number=0x01020304, firmware_version=0x0A0B0C)
    pd = PeripheralDevice(
        PDInfo(PD_ADDR, f1, scbk=key, id=want, flags=[LibFlag.EnforceSecure]),
        PDCapabilities([]),
        log_level=LogLevel.Debug,
    )
    cp = ControlPanel(
        [PDInfo(PD_ADDR, f2, scbk=key,
                flags=[LibFlag.EnforceSecure, LibFlag.EnableNotification])],
        log_level=LogLevel.Debug,
    )
    try:
        pd.start()
        cp.start()
        note = _wait_pd_id(cp, timeout=5.0)
        assert note.type == NotificationType.PdId
        got = note.pd_id
        assert got.version == want.version
        assert got.model == want.model
        assert got.vendor_code == want.vendor_code
        assert got.serial_number == want.serial_number
        assert got.firmware_version == want.firmware_version
    finally:
        _teardown_pair(cp, pd, "notif-pdid")
