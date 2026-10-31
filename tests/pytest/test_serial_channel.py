#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import os
import sys
import time

import pytest

from osdp import *

pytestmark = pytest.mark.skipif(
    not sys.platform.startswith("linux"), reason="needs a Linux pseudo-terminal"
)


class PtyMasterChannel(Channel):
    """The far end of a pseudo-terminal, standing in for the wire."""

    def __init__(self, fd: int):
        self.fd = fd
        os.set_blocking(fd, False)

    def read(self, max_bytes: int) -> bytes:
        try:
            return os.read(self.fd, max_bytes)
        except BlockingIOError:
            return b""

    def write(self, buf: bytes) -> int:
        return os.write(self.fd, buf)

    def flush(self) -> None:
        pass


@pytest.fixture
def pty_pair():
    master, slave = os.openpty()
    yield master, os.ttyname(slave)
    os.close(slave)
    os.close(master)


def test_serial_channel_carries_a_secure_session(pty_pair):
    master, device = pty_pair
    key = KeyStore.gen_key()

    with SerialChannel(device) as channel:
        pd = PeripheralDevice(
            PDInfo(101, PtyMasterChannel(master), scbk=key),
            PDCapabilities(),
            log_level=LogLevel.Error,
        )
        cp = ControlPanel([PDInfo(101, channel, scbk=key)],
                          log_level=LogLevel.Error)
        pd.start()
        cp.start()
        try:
            assert cp.sc_wait(101, timeout=10)
        finally:
            cp.teardown()
            pd.teardown()


def test_serial_channel_flush_discards_what_was_received(pty_pair):
    master, device = pty_pair
    with SerialChannel(device) as channel:
        os.write(master, b"stale")
        time.sleep(0.1)
        channel.flush()
        assert channel.read(16) == b""
        os.write(master, b"fresh")
        time.sleep(0.1)
        assert channel.read(16) == b"fresh"


def test_serial_channel_closes_its_port(pty_pair):
    _, device = pty_pair
    channel = SerialChannel(device)
    channel.close()
    assert not channel.port.is_open
