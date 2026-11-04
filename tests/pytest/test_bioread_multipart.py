#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import pytest

from osdp import *
from osdp import commands, events
from osdp.events import MAX_BIO_TEMPLATE_LEN
from conftest import (
    MultidropBus,
    assert_command_received,
    wait_for_notification_event,
    wait_for_non_notification_event,
)

pd_addr = 101

# Multipart notification family (C enum osdp_mp_msg_type).
MP_MSG_BIOREAD = 5

bus = MultidropBus(1)
chn = bus.multidrop_channel()

key = KeyStore.gen_key()

pd_cap = PDCapabilities([
    (Capability.Biometrics, 1, 1),
])

pd_info_list = [
    PDInfo(pd_addr, chn, scbk=key,
           flags=[LibFlag.EnforceSecure, LibFlag.EnableNotification,
                  LibFlag.BioReadrMultipart]),
]

pd = PeripheralDevice(
    PDInfo(pd_addr, bus.pd_channel(0), scbk=key,
           flags=[LibFlag.EnforceSecure, LibFlag.BioReadrMultipart]),
    pd_cap,
    log_level=LogLevel.Debug,
)

cp = ControlPanel(pd_info_list, log_level=LogLevel.Debug)

# A template that fills the whole staging buffer forces the reply to span
# multiple OSDP packets (fragment 0 in the standard BIOREADR envelope, the rest
# pulled by polls as W16 fragments).
BIG_TEMPLATE = bytes((0xC0 + i) & 0xFF for i in range(MAX_BIO_TEMPLATE_LEN))


@pytest.fixture(scope='module', autouse=True)
def setup_test():
    pd.start()
    cp.start()
    if not cp.online_wait_all(timeout=10):
        teardown_test()
        pytest.fail("Failed to bring the PD online within timeout")
    yield
    teardown_test()


def teardown_test():
    cp.teardown()
    pd.teardown()


def drain_events(address):
    while cp.get_event(address, timeout=0.1) is not None:
        pass


def test_bioread_multipart_roundtrip():
    """A template larger than one packet is fragmented and reassembled.

    The reassembled reply event is dispatched before the engine finishes the
    transfer (MultipartDone), so the reply is consumed first.
    """
    reply_event = events.BioRead(status=BioStatus.Success,
                                 type=BioType.RightThumbPrint, quality=200,
                                 data=BIG_TEMPLATE)
    drain_events(pd_addr)
    cmd = commands.BioRead(reader=reply_event.reader,
                           type=reply_event.type, quality=reply_event.quality)

    def cmd_handler(command):
        if isinstance(command, commands.BioRead):
            pd.submit_event(reply_event)
        return None

    pd.set_command_handler(cmd_handler)
    try:
        assert cp.submit_command(pd_addr, cmd)
        assert_command_received(pd, cmd)

        wait_for_non_notification_event(cp, pd_addr, reply_event)
        done = events.Notification(
            type=NotificationType.MultipartDone,
            mp_type=MP_MSG_BIOREAD,
            object_id=reply_event.reader,
            total=len(reply_event.data),
            offset=len(reply_event.data),
            outcome=int(MpOutcome.Ok),
        )
        wait_for_notification_event(cp, pd_addr, done)
    finally:
        pd.set_command_handler(None)
