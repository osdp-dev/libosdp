#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import pytest

from osdp import *
from osdp import commands, events
from conftest import (
    MultidropBus,
    assert_command_received,
    wait_for_notification_event,
    wait_for_non_notification_event,
)

pd_addr = 101

bus = MultidropBus(1)
chn = bus.multidrop_channel()

key = KeyStore.gen_key()

# Multipart notification families (C enum osdp_mp_msg_type)
MP_MSG_PIV = 2
MP_MSG_GENAUTH = 3
MP_MSG_CRAUTH = 4

pd_cap = PDCapabilities([
    (Capability.SmartCard, 1, 1),
])

pd_info_list = [
    PDInfo(pd_addr, chn, scbk=key,
           flags=[LibFlag.EnforceSecure, LibFlag.EnableNotification]),
]

pd = PeripheralDevice(
    PDInfo(pd_addr, bus.pd_channel(0), scbk=key,
           flags=[LibFlag.EnforceSecure]),
    pd_cap,
    log_level=LogLevel.Debug,
)

pd_list = [pd]

cp = ControlPanel(pd_info_list, log_level=LogLevel.Debug)

# Fill the whole payload buffer so both the command and the reply legs span
# multiple OSDP packets (multi-part fragmentation) on the wire.
CHALLENGE = bytes((0x10 + i) & 0xFF for i in range(commands.MAX_PIV_DATA_LEN))
REPLY = bytes((0xC0 + i) & 0xFF for i in range(commands.MAX_PIV_DATA_LEN))


@pytest.fixture(scope='module', autouse=True)
def setup_test():
    for dev in pd_list:
        dev.start()
    cp.start()
    if not cp.online_wait_all(timeout=10):
        teardown_test()
        pytest.fail("Failed to bring all PDs online within timeout")
    yield
    teardown_test()


def teardown_test():
    cp.teardown()
    for dev in pd_list:
        dev.teardown()


def drain_events(address):
    while cp.get_event(address, timeout=0.1) is not None:
        pass


def run_smartcard_op(cmd, reply_event, mp_msg, object_id):
    """Round-trip one smartcard op and pin its terminal notification.

    The reassembled reply event is dispatched before the engine finishes the
    op (MultipartDone), so consume the reply first, then the notification.
    """
    drain_events(pd_addr)

    def cmd_handler(command):
        if isinstance(command, type(cmd)):
            pd.submit_event(reply_event)
        return None

    pd.set_command_handler(cmd_handler)
    try:
        assert cp.submit_command(pd_addr, cmd)
        assert_command_received(pd, cmd)

        done = events.Notification(
            type=NotificationType.MultipartDone,
            mp_type=mp_msg,
            object_id=object_id,
            total=len(reply_event.data),
            offset=len(reply_event.data),
            outcome=int(MpOutcome.Ok),
        )
        wait_for_non_notification_event(cp, pd_addr, reply_event)
        wait_for_notification_event(cp, pd_addr, done)
    finally:
        pd.set_command_handler(None)


def test_pivdata_roundtrip():
    run_smartcard_op(
        commands.PivData(oid=b"\x5f\xc1\x02", element=7, offset=0),
        events.PivData(data=REPLY),
        MP_MSG_PIV, object_id=7,
    )


def test_genauth_roundtrip():
    run_smartcard_op(
        commands.GenAuth(algorithm=0xA7, key=0x9E, data=CHALLENGE),
        events.GenAuth(data=REPLY),
        MP_MSG_GENAUTH, object_id=0x9E,
    )


def test_crauth_roundtrip():
    run_smartcard_op(
        commands.CrAuth(algorithm=0x11, key=0x22, data=CHALLENGE),
        events.CrAuth(data=REPLY),
        MP_MSG_CRAUTH, object_id=0x22,
    )
