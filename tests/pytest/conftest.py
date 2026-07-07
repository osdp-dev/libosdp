#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import os
import socket
import pytest
from osdp import *

import fcntl
import errno
import threading

class FIFOChannel(Channel):
    def __init__(self, fifo_read_path: str, fifo_write_path: str) -> None:
        super().__init__()
        self.fifo_read_path = fifo_read_path
        self.fifo_write_path = fifo_write_path
        self.fifo_read_fd = -1
        self.fifo_write_fd = -1

        # Open read FIFO immediately so other side can open write FIFO
        if not os.path.exists(self.fifo_read_path):
            os.mkfifo(self.fifo_read_path)
        self.fifo_read_fd = os.open(self.fifo_read_path, os.O_RDONLY | os.O_NONBLOCK)

    def _open_fifo(self) -> None:
        import time
        if not os.path.exists(self.fifo_read_path):
            os.mkfifo(self.fifo_read_path)
        if not os.path.exists(self.fifo_write_path):
            os.mkfifo(self.fifo_write_path)

        # Open read side in non-blocking mode
        if self.fifo_read_fd < 0:
            self.fifo_read_fd = os.open(self.fifo_read_path, os.O_RDONLY | os.O_NONBLOCK)

        # Open write side: use non-blocking for the open() call to avoid deadlock,
        # then make it blocking for actual I/O operations
        if self.fifo_write_fd < 0:
            # Retry opening write FIFO with timeout
            for _ in range(100):  # 10 second timeout
                try:
                    self.fifo_write_fd = os.open(self.fifo_write_path, os.O_WRONLY | os.O_NONBLOCK)
                    # Successfully opened, now make it blocking for writes
                    flags = fcntl.fcntl(self.fifo_write_fd, fcntl.F_GETFL)
                    fcntl.fcntl(self.fifo_write_fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
                    break
                except OSError as e:
                    if e.errno == errno.ENXIO:  # No reader yet
                        time.sleep(0.1)
                        continue
                    raise
            else:
                raise TimeoutError(f"Failed to open write FIFO {self.fifo_write_path} - no reader")

    def read(self, max_bytes: int) -> bytes:
        if self.fifo_read_fd < 0:
            self._open_fifo()
        try:
            return os.read(self.fifo_read_fd, max_bytes)
        except OSError as e:
            if e.errno == errno.EAGAIN or e.errno == errno.EWOULDBLOCK:
                return b''  # Non-blocking read would block
            raise

    def write(self, buf: bytes) -> int:
        if self.fifo_write_fd < 0:
            self._open_fifo()
        return os.write(self.fifo_write_fd, buf)

    def flush(self) -> None:
        # Only flush if FIFO is already open
        if self.fifo_read_fd >= 0:
            try:
                # Drain the read buffer
                while True:
                    data = os.read(self.fifo_read_fd, 1024)
                    if not data:
                        break
            except OSError as e:
                if e.errno != errno.EAGAIN and e.errno != errno.EWOULDBLOCK:
                    raise

    def close(self) -> None:
        if self.fifo_read_fd >= 0:
            os.close(self.fifo_read_fd)
            self.fifo_read_fd = -1
        if self.fifo_write_fd >= 0:
            os.close(self.fifo_write_fd)
            self.fifo_write_fd = -1
        if os.path.exists(self.fifo_read_path):
            os.remove(self.fifo_read_path)
        if os.path.exists(self.fifo_write_path):
            os.remove(self.fifo_write_path)

class _MultidropCPChannel(Channel):
    def __init__(self, bus: 'MultidropBus') -> None:
        super().__init__()
        self._bus = bus
        self._idx = 0

    def read(self, max_bytes: int) -> bytes:
        return self._bus._read(self._idx, max_bytes)

    def write(self, buf: bytes) -> int:
        return self._bus._write(self._idx, buf)

    def flush(self) -> None:
        self._bus._flush(self._idx)


class _MultidropPDChannel(Channel):
    def __init__(self, bus: 'MultidropBus', pd_idx: int) -> None:
        super().__init__()
        self._bus = bus
        self._idx = pd_idx + 1

    def read(self, max_bytes: int) -> bytes:
        return self._bus._read(self._idx, max_bytes)

    def write(self, buf: bytes) -> int:
        return self._bus._write(self._idx, buf)

    def flush(self) -> None:
        self._bus._flush(self._idx)


class MultidropBus:
    """
    Two-stream RS-485 bus simulation:

    Stream A (CP → PDs): CP's writes land here; each PD has its own read
    position and advances independently.  PDs see CP's commands in temporal
    order, including commands addressed to other PDs (which they discard by
    address — exactly as on a real RS-485 wire).

    Stream B (PDs → CP): every PD reply is appended here in write order; CP
    reads through them sequentially.  Because the stream is ordered, CP
    processes PD-101's reply and then PD-102's reply without either starving
    the other.

    Echo suppression is built-in: CP never reads stream A (its own writes)
    and PDs never read stream B (other PDs' writes).
    """

    def __init__(self, num_pds: int) -> None:
        self._lock = threading.Lock()
        self._num_pds = num_pds

        # Stream A: CP writes, each PD reads independently
        self._a_buf = bytearray()
        self._a_pos = [0] * num_pds   # _a_pos[i] = bytes consumed by PD (i+1)

        # Stream B: PDs write in temporal order, CP reads
        self._b_segs = []             # [(pd_writer_idx, bytearray), ...]
        self._b_base = 0              # absolute index of _b_segs[0]
        self._b_cursor = [0, 0]       # [abs_seg_idx, byte_offset]

    def multidrop_channel(self) -> _MultidropCPChannel:
        return _MultidropCPChannel(self)

    def pd_channel(self, idx: int) -> _MultidropPDChannel:
        return _MultidropPDChannel(self, idx)

    def _write(self, writer_idx: int, data: bytes) -> int:
        with self._lock:
            if writer_idx == 0:           # CP → stream A
                self._a_buf.extend(data)
            else:                         # PD → stream B
                self._b_segs.append((writer_idx, bytearray(data)))
        return len(data)

    def _read(self, reader_idx: int, max_bytes: int) -> bytes:
        with self._lock:
            if reader_idx == 0:           # CP reads from stream B
                result = bytearray()
                ci, off = self._b_cursor
                while len(result) < max_bytes:
                    rel = ci - self._b_base
                    if rel >= len(self._b_segs):
                        break
                    _, data = self._b_segs[rel]
                    chunk = data[off:off + (max_bytes - len(result))]
                    if not chunk:
                        ci += 1
                        off = 0
                        continue
                    result.extend(chunk)
                    off += len(chunk)
                    if off >= len(data):
                        ci += 1
                        off = 0
                self._b_cursor = [ci, off]
                if ci > self._b_base:
                    del self._b_segs[:ci - self._b_base]
                    self._b_base = ci
                return bytes(result)
            else:                         # PD reads from stream A
                i = reader_idx - 1
                pos = self._a_pos[i]
                chunk = bytes(self._a_buf[pos:pos + max_bytes])
                self._a_pos[i] += len(chunk)
                min_pos = min(self._a_pos)
                if min_pos > 0:
                    del self._a_buf[:min_pos]
                    for j in range(self._num_pds):
                        self._a_pos[j] -= min_pos
                return chunk

    def _flush(self, reader_idx: int) -> None:
        with self._lock:
            if reader_idx == 0:           # CP
                abs_end = self._b_base + len(self._b_segs)
                self._b_cursor = [abs_end, 0]
            else:                         # PD
                i = reader_idx - 1
                self._a_pos[i] = len(self._a_buf)

# ============================================================================
# Test Helper Functions for OSDP Communication Assertions
# ============================================================================
# These helpers abstract away timeout handling and provide consistent error
# messages when OSDP communication fails during tests.

def assert_command_received(pd, expected_cmd, timeout=2):
    """Helper to assert that a PD receives the expected command within timeout

    Args:
        pd: PeripheralDevice instance
        expected_cmd: Expected command dict to match
        timeout: Timeout in seconds (default: 2)

    Returns:
        The received command dict

    Raises:
        pytest.fail if timeout occurs or command doesn't match
    """
    cmd = pd.get_command(timeout=timeout)
    if cmd is None:
        pytest.fail(f"Timeout waiting for command after {timeout}s")
    assert cmd == expected_cmd
    return cmd

def assert_event_received(cp, pd_address, expected_event, timeout=2):
    """Helper to assert that a CP receives the expected event within timeout"""
    event = cp.get_event(pd_address, timeout=timeout)
    if event is None:
        pytest.fail(f"Timeout waiting for event after {timeout}s")
    assert event == expected_event
    return event

def wait_for_notification_event(cp, pd_address, expected_event, timeout=5):
    """Helper to wait for a specific notification event, filtering out other events"""
    timeout_count = 0
    max_attempts = int(timeout * 2)  # Check every 0.5s

    while timeout_count < max_attempts:
        e = cp.get_event(pd_address, timeout=0.5)
        if e and e['event'] == Event.Notification and e['type'] == expected_event['type']:
            if e == expected_event:
                return e
        timeout_count += 1

    pytest.fail(f"Timeout waiting for notification event after {timeout}s")

def wait_for_non_notification_event(cp, pd_address, expected_event, timeout=5):
    """Helper to wait for a non-notification event, filtering out notification events"""
    timeout_count = 0
    max_attempts = int(timeout * 2)  # Check every 0.5s

    while timeout_count < max_attempts:
        e = cp.get_event(pd_address, timeout=0.5)
        if e and e['event'] != Event.Notification:
            if e == expected_event:
                return e
        timeout_count += 1

    pytest.fail(f"Timeout waiting for non-notification event after {timeout}s")

class TestUtils:
    # Prevent pytest from collecting this helper as a test class.
    __test__ = False
    def __init__(self):
        self.ks = KeyStore()

    def create_cp(self, pd_info_list, sc_wait=False, online_wait=False):
        cp = ControlPanel(pd_info_list, log_level=LogLevel.Debug)
        cp.start()
        if sc_wait:
            if not cp.sc_wait_all(timeout=10):
                cp.teardown()
                pytest.fail("Failed to establish secure channel within timeout")
        elif online_wait:
            if not cp.online_wait_all(timeout=10):
                cp.teardown()
                pytest.fail("Failed to bring all PDs online within timeout")
        return cp

    def create_pd(self, pd_info, capabilities=None):
        if capabilities is None:
            capabilities = PDCapabilities()
        pd = PeripheralDevice(pd_info, capabilities, log_level=LogLevel.Debug)
        pd.start()
        return pd


_FIFO_REGISTRY = {}

def make_fifo_pair(name):
    one = f'/tmp/fifo-{name}.one'
    two = f'/tmp/fifo-{name}.two'
    pair = (FIFOChannel(one, two), FIFOChannel(two, one))
    _FIFO_REGISTRY[name] = pair
    return pair

def cleanup_fifo_pair(name):
    pair = _FIFO_REGISTRY.pop(name, None)
    if pair:
        for chan in pair:
            close_fn = getattr(chan, "close", None)
            if callable(close_fn):
                close_fn()
    one = f'/tmp/fifo-{name}.one'
    two = f'/tmp/fifo-{name}.two'
    if os.path.exists(one):
        os.remove(one)
    if os.path.exists(two):
        os.remove(two)

@pytest.fixture(scope='session')
def utils():
    return TestUtils()
