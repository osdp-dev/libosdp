#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""The transport a CP and PD talk over."""

import abc
from typing import Any

__all__ = ["Channel", "SerialChannel"]


class Channel(abc.ABC):
    """A byte stream between a CP and one or more PDs.

    Subclass this to talk over a serial port, a socket, or anything else. On an
    RS-485 bus every PD shares one channel, so reads may return bytes meant for
    another device; the library sorts that out.

    The library calls these from its refresh thread, so they must not block for
    long: a PD has to be serviced at least every 50ms to stay in spec.
    """

    @abc.abstractmethod
    def read(self, max_bytes: int) -> bytes:
        """Return up to `max_bytes` bytes, or b"" if none are waiting.

        Must not block waiting for data to arrive.
        """

    @abc.abstractmethod
    def write(self, buf: bytes) -> int:
        """Send as much of `buf` as possible; return how much was sent."""

    @abc.abstractmethod
    def flush(self) -> None:
        """Discard anything buffered. May do nothing if that has no meaning."""


class SerialChannel(Channel):
    """A Channel over a serial port: RS-485 adapters, USB CDC ACM devices,
    or anything else the OS presents as one.

    Needs pyserial (``pip install libosdp[serial]``). Reads never block;
    a write blocks until the frame is handed to the driver, which at OSDP
    frame sizes is a few milliseconds. Close the port with `close()`, or use
    the channel as a context manager.

    Example:
        with SerialChannel("/dev/ttyUSB0", 115200) as channel:
            cp = ControlPanel([PDInfo(101, channel)])
    """

    def __init__(self, device: str, baudrate: int = 115200) -> None:
        try:
            import serial
        except ImportError as e:
            raise ImportError(
                "SerialChannel needs pyserial: pip install libosdp[serial]"
            ) from e
        self.port: Any = serial.Serial(device, baudrate, timeout=0)
        """The underlying `serial.Serial`."""

    def read(self, max_bytes: int) -> bytes:
        return bytes(self.port.read(max_bytes))

    def write(self, buf: bytes) -> int:
        return int(self.port.write(buf))

    def flush(self) -> None:
        # What a peer sent before the library chose to resynchronize; what
        # we have queued to send is a frame in flight, and must go out whole.
        self.port.reset_input_buffer()

    def close(self) -> None:
        """Close the serial port."""
        self.port.close()

    def __enter__(self) -> "SerialChannel":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()
