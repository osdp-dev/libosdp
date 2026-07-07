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
from conftest import make_fifo_pair, cleanup_fifo_pair


PD_ADDR = 101
FILE_ID = 13
FILE_SIZE = 4096


def _fresh_pair(name, enable_notification_on_pd=True):
    key = KeyStore.gen_key()
    f1, f2 = make_fifo_pair(name)

    pd_flags = [LibFlag.EnforceSecure]
    if enable_notification_on_pd:
        pd_flags.append(LibFlag.EnableNotification)
    cp_flags = [LibFlag.EnforceSecure, LibFlag.EnableNotification]

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
        if cmd.get("command") != Command.Notification:
            return 0, None
        with self.lock:
            if cmd["type"] == Notification.PeripheralDeviceStatus:
                self.pd_status_count += 1
                self.pd_status_last = dict(cmd)
            elif cmd["type"] == Notification.SecureChannelStatus:
                self.sc_status_count += 1
                self.sc_status_last = dict(cmd)
        return 0, None

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
            assert rec.pd_status_last["type"] == \
                Notification.PeripheralDeviceStatus
            assert rec.pd_status_last["arg0"] == 1, \
                f"PD_STATUS arg0={rec.pd_status_last['arg0']}, want 1 (online)"
            assert rec.sc_status_last["type"] == \
                Notification.SecureChannelStatus
            assert rec.sc_status_last["arg0"] == 1, \
                f"SC_STATUS arg0={rec.sc_status_last['arg0']}, want 1 (active)"
    finally:
        _teardown_pair(cp, pd, "notif-pd")


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
            assert rec.pd_status_last["arg0"] == 0, \
                f"PD_STATUS arg0={rec.pd_status_last['arg0']}, want 0 (offline)"
    finally:
        _teardown_pair(cp, pd, "notif-off")


# -----------------------------------------------------------------------------
# File transfer abort tests
# -----------------------------------------------------------------------------

def _make_file_ops():
    """Return (sender_fops, receiver_fops, tally) where tally tracks close
    calls so tests can assert the abort path actually closed the backing
    resource.
    """
    data = bytes(random.randint(0, 255) for _ in range(FILE_SIZE))
    received = bytearray(FILE_SIZE)
    tally = {"sender_closed": 0, "receiver_closed": 0}

    def sender_open(file_id, file_size):
        assert file_id == FILE_ID
        assert file_size == 0
        return FILE_SIZE

    def sender_read(size, offset):
        assert offset < FILE_SIZE
        end = min(offset + size, FILE_SIZE)
        return data[offset:end]

    def sender_write(*_):
        assert False, "sender should not be asked to write"

    def sender_close(file_id):
        tally["sender_closed"] += 1

    def receiver_open(file_id, file_size):
        assert file_id == FILE_ID
        assert file_size == FILE_SIZE
        return 0

    def receiver_read(*_):
        assert False, "receiver should not be asked to read"

    def receiver_write(buf, offset):
        received[offset:offset + len(buf)] = buf
        return len(buf)

    def receiver_close(file_id):
        tally["receiver_closed"] += 1

    sender_fops = {
        "open": sender_open,
        "read": sender_read,
        "write": sender_write,
        "close": sender_close,
    }
    receiver_fops = {
        "open": receiver_open,
        "read": receiver_read,
        "write": receiver_write,
        "close": receiver_close,
    }
    return sender_fops, receiver_fops, tally


def _wait_tx_in_progress(getter, timeout=2.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = getter()
        if status is not None and status.get("offset", 0) > 0:
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
        if e.get("event") != Event.Notification:
            continue
        if e.get("type") != Notification.FileTransferDone:
            continue
        assert e["arg0"] == FILE_ID
        assert e["arg1"] == expected_outcome, \
            f"outcome={e['arg1']}, want {expected_outcome}"
        return e
    pytest.fail(f"FileTransferDone({expected_outcome}) not received")


def test_cp_file_tx_aborts_on_disable_pd():
    """CP side: disabling a PD mid-transfer must abort the file_tx and
    fire FILE_TX_DONE/ABORTED, matching test_cp_file_tx_abort_on_disable
    in the C suite."""
    cp, pd = _fresh_pair("notif-fabort-cp")
    try:
        pd.start()
        cp.start()
        assert cp.sc_wait_all(timeout=10), "SC handshake did not complete"

        sender_fops, receiver_fops, _ = _make_file_ops()
        assert cp.register_file_ops(PD_ADDR, sender_fops)
        assert pd.register_file_ops(receiver_fops)

        # Drain any setup-time notifications.
        while cp.get_event(PD_ADDR, timeout=0) is not None:
            pass

        assert cp.submit_command(PD_ADDR, {
            "command": Command.FileTransfer, "id": FILE_ID, "flags": 0,
        })

        assert _wait_tx_in_progress(lambda: cp.get_file_tx_status(PD_ADDR)), \
            "file_tx never reached 'in progress' on CP side"

        # Disabling the PD drives OSDP_CP_STATE_DISABLED, which must
        # call osdp_file_tx_abort() and fire FILE_TX_DONE/ABORTED.
        assert cp.disable_pd(PD_ADDR), "disable_pd failed"

        _wait_file_tx_done(cp, FileTxOutcome.Aborted, timeout=5.0)
        assert cp.get_file_tx_status(PD_ADDR) is None, \
            "CP still reports transfer active after abort"
    finally:
        _teardown_pair(cp, pd, "notif-fabort-cp")


def test_pd_file_tx_aborts_on_cp_silence():
    """PD side: when the CP goes silent mid-transfer, the PD's online
    timeout must call osdp_file_tx_abort() on its own transfer."""
    cp, pd = _fresh_pair("notif-fabort-pd")
    try:
        pd.start()
        cp.start()
        assert cp.sc_wait_all(timeout=10), "SC handshake did not complete"

        sender_fops, receiver_fops, _ = _make_file_ops()
        assert cp.register_file_ops(PD_ADDR, sender_fops)
        assert pd.register_file_ops(receiver_fops)

        assert cp.submit_command(PD_ADDR, {
            "command": Command.FileTransfer, "id": FILE_ID, "flags": 0,
        })

        assert _wait_tx_in_progress(lambda: pd.get_file_tx_status()), \
            "file_tx never reached 'in progress' on PD side"

        # Silence the CP — PD should trip OSDP_PD_ONLINE_TOUT_MS (8s in
        # the shipped build) and abort its in-flight rx.
        cp.stop()

        assert _wait_tx_cleared(lambda: pd.get_file_tx_status(), timeout=15.0), \
            "PD still reports file_tx active after CP went silent"
    finally:
        _teardown_pair(cp, pd, "notif-fabort-pd")
