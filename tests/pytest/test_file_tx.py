#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time
import random
import pytest
from osdp import *
from conftest import make_fifo_pair, cleanup_fifo_pair

sender_data = [ random.randint(0, 255) for _ in range(4096) ]

def sender_open(file_id: int, file_size: int) -> int:
    assert file_id == 13
    assert file_size == 0 # sender has to return file_size so this must be 0
    return 4096 # we'll just send 4k of random data

def sender_read(size: int, offset: int) -> bytes:
    assert offset < 4096
    if offset + size > 4096:
        size = 4096 - offset
    return bytes(sender_data[offset:offset+size])

def sender_write(data: bytes, offset: int) -> int:
    # sender should not try to write anything!
    assert False

def sender_close(file_id: int):
    assert file_id == 13

sender_fops = {
    'open': sender_open,
    'read': sender_read,
    'write': sender_write,
    'close': sender_close
}

receiver_data = [0] * 4096

def receiver_open(file_id: int, file_size: int) -> int:
    assert file_id == 13
    assert file_size == 4096
    return 0 # indicates success. Both CP and PD have the file_size now.

def receiver_read(size: int, offset: int) -> bytes:
    # receiver should not read anything
    assert False

def receiver_write(data: bytes, offset: int) -> int:
    global receiver_data
    assert offset + len(data) <= 4096
    receiver_data[offset:offset + len(data)] = list(data)
    return len(data)

def receiver_close(file_id: int):
    assert file_id == 13

receiver_fops = {
    'open': receiver_open,
    'read': receiver_read,
    'write': receiver_write,
    'close': receiver_close
}

pd_cap = PDCapabilities([])

f1, f2 = make_fifo_pair("file")
key = KeyStore.gen_key()

pd = PeripheralDevice(
    PDInfo(101, f1, scbk=key, flags=[ LibFlag.EnforceSecure ]),
    pd_cap,
    log_level=LogLevel.Debug
)
cp = ControlPanel([
        PDInfo(101, f2, scbk=key,
               flags=[ LibFlag.EnforceSecure, LibFlag.EnableNotification ]),
    ],
    log_level=LogLevel.Debug
)

def drain_events(address):
    while cp.get_event(address, timeout=0) is not None:
        pass

def wait_for_file_tx_done(address, expected_outcome, timeout=10.0,
                          dupe_window=0.3):
    """Wait for a FileTransferDone notification, then briefly check for dupes.

    The short post-receipt window (`dupe_window`) pins the "notification fires
    exactly once" guarantee from the refactor without stretching the test's
    wall-clock by the full `timeout`.
    """
    deadline = time.monotonic() + timeout
    notif = None
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        e = cp.get_event(address, timeout=max(0.05, remaining))
        if e is None:
            continue
        if e.get('event') != Event.Notification:
            continue
        if e.get('type') != Notification.FileTransferDone:
            continue
        notif = e
        break
    assert notif is not None, "FileTransferDone notification not received"
    assert notif['arg0'] == 13, \
        f"unexpected file_id in notification: {notif['arg0']}"
    assert notif['arg1'] == expected_outcome, \
        f"unexpected outcome: got {notif['arg1']}, want {expected_outcome}"

    # Pin "fires exactly once" — drain for a short window and fail on any dupe
    dupe_deadline = time.monotonic() + dupe_window
    while time.monotonic() < dupe_deadline:
        e = cp.get_event(address, timeout=0.05)
        if e is None:
            continue
        if e.get('event') == Event.Notification and \
           e.get('type') == Notification.FileTransferDone:
            raise AssertionError("FileTransferDone fired more than once")
    return notif

@pytest.fixture(scope='module', autouse=True)
def setup_test():
    pd.start()
    cp.start()
    cp.sc_wait_all()
    yield
    teardown_test()

def teardown_test():
    cp.teardown()
    pd.teardown()
    cleanup_fifo_pair("file")

def test_file_transfer(utils):
    # Drain any notifications from prior tests / SC setup
    drain_events(101)

    # Register file OPs and kick off a transfer
    assert cp.register_file_ops(101, sender_fops)
    assert pd.register_file_ops(receiver_fops)
    file_tx_cmd = {
        'command': Command.FileTransfer,
        'id': 13,
        'flags': 0
    }
    assert cp.submit_command(101, file_tx_cmd)
    assert pd.get_command() == file_tx_cmd

    # Wait for the CP-side completion notification (the new canonical signal)
    wait_for_file_tx_done(101, FileTxOutcome.Ok)

    # Poll API must report "not in progress" after completion
    assert cp.get_file_tx_status(101) is None

    # Check if the data was sent properly
    assert sender_data == receiver_data

def test_file_tx_abort(utils):
    # Drain any notifications from prior tests / SC setup
    drain_events(101)

    # Register file OPs and kick off a transfer
    assert cp.register_file_ops(101, sender_fops)
    assert pd.register_file_ops(receiver_fops)
    file_tx_cmd = {
        'command': Command.FileTransfer,
        'id': 13,
        'flags': 0
    }
    assert cp.submit_command(101, file_tx_cmd)
    assert pd.get_command() == file_tx_cmd

    # Allow some number of transfers to go through
    time.sleep(0.5)

    file_tx_abort = {
        'command': Command.FileTransfer,
        'id': 13,
        'flags': CommandFileTxFlags.Cancel
    }
    assert cp.submit_command(101, file_tx_abort)

    # Wait for the abort completion notification
    wait_for_file_tx_done(101, FileTxOutcome.Aborted)

    assert cp.get_file_tx_status(101) is None
    assert pd.get_file_tx_status() is None
