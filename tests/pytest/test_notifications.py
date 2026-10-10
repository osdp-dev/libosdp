#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import os
import time
import threading
import random
import pytest

from osdp import *
from osdp import commands, events
from conftest import make_fifo_pair, cleanup_fifo_pair


PD_ADDR = 101
FILE_ID = 13
FILE_SIZE = 4096


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

    def reset_pd_status(self):
        with self.lock:
            self.pd_status_count = 0
            self.pd_status_last = None

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


def test_pd_receives_offline_notification_on_cp_silence():
    """When CP stops refreshing, PD's online-timeout trips and PD_STATUS
    offline fires on the command callback."""
    cp, pd = _fresh_pair("notif-off")
    rec = NotifRecorder()
    pd.set_command_handler(rec.handler)
    try:
        pd.start()
        cp.start()
        assert cp.sc_wait_all(timeout=10), "SC handshake did not complete"
        assert rec.wait_pd_status(timeout=5), \
            "PD never saw initial online notification"

        rec.reset_pd_status()

        # Silence the CP side. OSDP_PD_ONLINE_TOUT_MS defaults to 8s in
        # shipped builds; give a generous margin for workqueue scheduling.
        cp.stop()

        assert rec.wait_pd_status(timeout=12), \
            "PD never saw offline notification after CP went silent"
        with rec.lock:
            assert not rec.pd_status_last.online, \
                f"PD_STATUS online={rec.pd_status_last.online}, want False"
    finally:
        _teardown_pair(cp, pd, "notif-off")


# -----------------------------------------------------------------------------
# File transfer abort tests
# -----------------------------------------------------------------------------

class _SenderFops:
    def __init__(self, data, tally):
        self._data = data
        self._tally = tally

    def open(self, file_id, file_size):
        assert file_id == FILE_ID
        assert file_size == 0
        return FILE_SIZE

    def read(self, size, offset):
        assert offset < FILE_SIZE
        end = min(offset + size, FILE_SIZE)
        return self._data[offset:end]

    def write(self, data, offset):
        assert False, "sender should not be asked to write"

    def close(self, file_id):
        self._tally["sender_closed"] += 1
        return 0


class _ReceiverFops:
    def __init__(self, received, tally):
        self._received = received
        self._tally = tally

    def open(self, file_id, file_size):
        assert file_id == FILE_ID
        assert file_size == FILE_SIZE
        return 0

    def read(self, size, offset):
        assert False, "receiver should not be asked to read"

    def write(self, data, offset):
        self._received[offset:offset + len(data)] = data
        return len(data)

    def close(self, file_id):
        self._tally["receiver_closed"] += 1
        return 0


def _make_file_ops():
    """Return (sender_fops, receiver_fops, tally) where tally tracks close
    calls so tests can assert the abort path actually closed the backing
    resource.
    """
    data = bytes(random.randint(0, 255) for _ in range(FILE_SIZE))
    received = bytearray(FILE_SIZE)
    tally = {"sender_closed": 0, "receiver_closed": 0}

    return _SenderFops(data, tally), _ReceiverFops(received, tally), tally


def _wait_tx_in_progress(getter, timeout=2.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = getter()
        if status is not None and status.offset > 0:
            return True
        time.sleep(0.02)
    return False


def _wait_tx_cleared(getter, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if getter() is None:
            return True
        time.sleep(0.05)
    return False


def _wait_file_tx_done(cp, expected_outcome, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = deadline - time.time()
        e = cp.get_event(PD_ADDR, timeout=max(0.05, remaining))
        if e is None:
            continue
        if not isinstance(e, events.Notification):
            continue
        if e.type != NotificationType.MultipartDone:
            continue
        assert e.file_id == FILE_ID
        assert e.mp_outcome == expected_outcome, \
            f"outcome={e.mp_outcome}, want {expected_outcome}"
        return e
    pytest.fail(f"MultipartDone({expected_outcome}) not received")


def test_cp_file_tx_aborts_on_disable_pd():
    """CP side: disabling a PD mid-transfer must abort the file_tx and
    fire MP_DONE/ABORTED, matching test_cp_file_tx_abort_on_disable
    in the C suite."""
    cp, pd = _fresh_pair("notif-fabort-cp")
    try:
        sender_fops, receiver_fops, _ = _make_file_ops()
        assert cp.register_file_ops(PD_ADDR, sender_fops)
        assert pd.register_file_ops(receiver_fops)

        pd.start()
        cp.start()
        assert cp.sc_wait_all(timeout=10), "SC handshake did not complete"

        # Drain any setup-time notifications.
        while cp.get_event(PD_ADDR, timeout=0) is not None:
            pass

        assert cp.submit_command(PD_ADDR, commands.FileTransfer(id=FILE_ID))

        assert _wait_tx_in_progress(lambda: cp.get_file_tx_status(PD_ADDR)), \
            "file_tx never reached 'in progress' on CP side"

        # Disabling the PD drives OSDP_CP_STATE_DISABLED, which must
        # call osdp_file_tx_abort() and fire MP_DONE/ABORTED.
        assert cp.disable_pd(PD_ADDR), "disable_pd failed"

        _wait_file_tx_done(cp, MpOutcome.Aborted, timeout=5.0)
        assert cp.get_file_tx_status(PD_ADDR) is None, \
            "CP still reports transfer active after abort"
    finally:
        _teardown_pair(cp, pd, "notif-fabort-cp")


def test_pd_file_tx_aborts_on_cp_silence():
    """PD side: when the CP goes silent mid-transfer, the PD's online
    timeout must call osdp_file_tx_abort() on its own transfer."""
    cp, pd = _fresh_pair("notif-fabort-pd")
    try:
        sender_fops, receiver_fops, _ = _make_file_ops()
        assert cp.register_file_ops(PD_ADDR, sender_fops)
        assert pd.register_file_ops(receiver_fops)

        pd.start()
        cp.start()
        assert cp.sc_wait_all(timeout=10), "SC handshake did not complete"

        assert cp.submit_command(PD_ADDR, commands.FileTransfer(id=FILE_ID))

        assert _wait_tx_in_progress(lambda: pd.get_file_tx_status()), \
            "file_tx never reached 'in progress' on PD side"

        # Silence the CP — PD should trip OSDP_PD_ONLINE_TOUT_MS (8s in
        # the shipped build) and abort its in-flight rx.
        cp.stop()

        assert _wait_tx_cleared(lambda: pd.get_file_tx_status(), timeout=15.0), \
            "PD still reports file_tx active after CP went silent"
    finally:
        _teardown_pair(cp, pd, "notif-fabort-pd")


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
