#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""The Control Panel side of an OSDP bus."""

import queue
import threading
import time
from typing import Any, Callable

from . import _sys
from ._marshal import decode_command, decode_event, encode_command
from .commands import Command
from .enums import (
    Capability,
    CompletionStatus,
    LibFlag,
    LogLevel,
    MpMsgType,
    NotificationType,
)
from .events import Event, Notification
from .types import FileOps, FileTxStatus, Metrics, PdId, PDInfo

__all__ = ["CommandCompletionHandler", "ControlPanel", "EventHandler"]

EventHandler = Callable[[int, Event], int]
"""Called with (pd_address, event) when a PD reports something.

Runs on the refresh thread, inside the library. Return 0 to accept the event.
"""

CommandCompletionHandler = Callable[[int, Command, CompletionStatus], None]
"""Called with (pd_address, command, status) when a submitted command settles.

Every accepted command is reported exactly once, including ones that were
flushed or torn down before reaching the wire.

The command handed back is rebuilt from the library's own copy, so it compares
equal to the one that was submitted but is not the same object.
"""

REFRESH_INTERVAL = 0.020
"""How often to service the bus. OSDP requires at least once every 50ms."""

_UNKNOWN_PD_ID = PdId(
    version=0, model=0, vendor_code=0, serial_number=0, firmware_version=0
)
"""What a PD's identity reads as before it has reported one."""


class ControlPanel:
    """Manages one or more PDs over a shared channel.

    Runs a background thread that services the bus; nothing happens until
    `start()` is called.

    Example:
        cp = ControlPanel([PDInfo(address=101, channel=my_channel)])
        cp.start()
        cp.online_wait(101)
        cp.submit_command(101, commands.Buzzer(rep_count=3))
    """

    def __init__(
        self,
        pd_info_list: list[PDInfo],
        log_level: LogLevel = LogLevel.Info,
        event_handler: EventHandler | None = None,
        command_completion_handler: CommandCompletionHandler | None = None,
    ) -> None:
        self.pd_addr = [pd_info.address for pd_info in pd_info_list]
        self.num_pds = len(pd_info_list)
        self.event_queue: list[queue.Queue[Event]] = [
            queue.Queue() for _ in self.pd_addr
        ]
        self.user_event_handler = event_handler
        self.user_command_completion_handler = command_completion_handler

        # Per-PD state, kept up to date from the library's notifications so
        # that reading it costs nothing and never crosses into C. The library
        # reports every transition, so this cannot drift; it starts out as
        # "nothing is up yet", which is what the library would report too.
        self._online = 0
        self._sc_active = 0
        self._pd_ids = [_UNKNOWN_PD_ID for _ in self.pd_addr]

        # Notifications are how the above is fed, so they are turned on at the
        # library level whatever the caller asked for. Whether the caller also
        # gets to see them stays their own flag's decision.
        self._forward_notifications = [
            LibFlag.EnableNotification in pd_info.flags
            for pd_info in pd_info_list
        ]
        info_list = []
        for pd_info in pd_info_list:
            info = pd_info.to_dict()
            info["flags"] |= int(LibFlag.EnableNotification)
            info_list.append(info)

        _sys.set_loglevel(int(log_level))
        self._ctx: _sys.ControlPanel | None = _sys.ControlPanel(info_list)
        # Our handlers always run, so that get_event() works whether or not the
        # caller registered one of their own.
        self.ctx.set_event_callback(self._internal_event_handler)
        self.ctx.set_command_completion_callback(
            self._internal_command_completion_handler
        )

        self.event: threading.Event | None = None
        self.lock = threading.RLock()
        self.thread: threading.Thread | None = None

    @property
    def ctx(self) -> "_sys.ControlPanel":
        """The library context. Raises once this CP has been torn down."""
        if self._ctx is None:
            raise RuntimeError("This ControlPanel has been torn down")
        return self._ctx

    # -- callbacks from the library -----------------------------------------

    def _internal_event_handler(self, pd: int, event: dict[str, Any]) -> int:
        decoded = decode_event(event)
        if isinstance(decoded, Notification):
            self._track_notification(pd, decoded)
            if not self._forward_notifications[pd]:
                # This one was only ever ours; the caller did not ask to see
                # library notifications.
                return 0
        self.event_queue[pd].put(decoded)
        if self.user_event_handler:
            return self.user_event_handler(self.pd_addr[pd], decoded)
        return 0

    def _track_notification(self, pd: int, note: Notification) -> None:
        """Fold a notification into the per-PD state this object reports.

        Runs on the refresh thread, before anything else sees the event.
        """
        bit = 1 << pd
        if note.type is NotificationType.PeripheralDeviceStatus:
            if note.online:
                self._online |= bit
            else:
                self._online &= ~bit
                # A PD that is not online has no secure channel either; the
                # library says so separately, but not always first.
                self._sc_active &= ~bit
        elif note.type is NotificationType.SecureChannelStatus:
            if note.active:
                self._sc_active |= bit
            else:
                self._sc_active &= ~bit
        elif note.type is NotificationType.PdId:
            self._pd_ids[pd] = PdId(
                version=note.version,
                model=note.model,
                vendor_code=note.vendor_code,
                serial_number=note.serial_number,
                firmware_version=note.firmware_version,
            )

    def _internal_command_completion_handler(
        self, pd: int, command: dict[str, Any], status: int
    ) -> None:
        if self.user_command_completion_handler:
            self.user_command_completion_handler(
                self.pd_addr[pd],
                decode_command(command),
                CompletionStatus(status),
            )

    def set_event_handler(self, handler: EventHandler | None) -> None:
        """Install the handler called for every event from every PD."""
        self.user_event_handler = handler

    def set_command_completion_handler(
        self, handler: CommandCompletionHandler | None
    ) -> None:
        """Install the handler called when a submitted command settles."""
        self.user_command_completion_handler = handler

    # -- commands and events ------------------------------------------------

    def submit_command(self, address: int, command: Command) -> bool:
        """Queue a command for a PD. Returns False if it could not be queued.

        The command is copied on the way in, so it may be discarded or reused
        as soon as this returns.
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(self.ctx.submit_command(pd, encode_command(command)))

    def flush_commands(self, address: int) -> int:
        """Drop every queued command for a PD. Returns how many were dropped.

        Each one is still reported to the completion handler, as Flushed.
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return self.ctx.flush_commands(pd)

    def cancel(self, address: int, what: MpMsgType) -> bool:
        """Ask a running multi-part operation on a PD to stop early.

        `what` is the value the operation reported as ``mp_type`` when it
        started. Returns False if no such operation is running.

        The request is cooperative: the operation winds down on a later
        refresh, and the command that started it then completes as Failed.
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return self.ctx.cancel(pd, int(what))

    def get_event(self, address: int, timeout: float = 5) -> Event | None:
        """Pop the next event from a PD, or None if none arrives in time.

        A negative timeout does not block.
        """
        pd = self.pd_addr.index(address)
        try:
            return self.event_queue[pd].get(timeout >= 0, timeout=timeout)
        except queue.Empty:
            return None

    # -- state --------------------------------------------------------------

    def status(self) -> int:
        """Bitmask of which PDs are online, indexed by their position.

        Answered from state this object keeps up to date from the library's
        notifications, so it costs a single attribute read: no lock, and no
        call into the library.
        """
        self.ctx  # refuse to answer for a context that has been released
        return self._online

    def is_online(self, address: int) -> bool:
        """Whether a PD is answering."""
        return bool(self.status() & (1 << self.pd_addr.index(address)))

    def get_num_online(self) -> int:
        """How many PDs are answering."""
        return self.status().bit_count()

    def sc_status(self) -> int:
        """Bitmask of which PDs have a secure channel up.

        Answered from tracked state, for the same reason as status().
        """
        self.ctx  # refuse to answer for a context that has been released
        return self._sc_active

    def is_sc_active(self, address: int) -> bool:
        """Whether a PD's secure channel is up."""
        return bool(self.sc_status() & (1 << self.pd_addr.index(address)))

    def get_num_sc_active(self) -> int:
        """How many PDs have a secure channel up."""
        return self.sc_status().bit_count()

    def get_pd_id(self, address: int) -> PdId:
        """The identity a PD reported during setup.

        Answered from tracked state: the library reports each PD's identity as
        it collects it. Ask before the PD has come online and the fields read
        as zero, as they always have.
        """
        self.ctx  # refuse to answer for a context that has been released
        return self._pd_ids[self.pd_addr.index(address)]

    def check_capability(
        self, address: int, cap: Capability
    ) -> tuple[int, int]:
        """A PD's (compliance_level, num_items) for one capability.

        Both are zero when the PD does not claim the capability.
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return self.ctx.check_capability(pd, int(cap))

    def set_flag(self, address: int, flag: LibFlag) -> bool:
        """Turn a per-PD flag on."""
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(self.ctx.set_flag(pd, int(flag)))

    def clear_flag(self, address: int, flag: LibFlag) -> bool:
        """Turn a per-PD flag off."""
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(self.ctx.clear_flag(pd, int(flag)))

    def enable_pd(self, address: int) -> bool:
        """Resume polling a PD."""
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(self.ctx.enable_pd(pd))

    def disable_pd(self, address: int) -> bool:
        """Stop polling a PD without tearing down the CP."""
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(self.ctx.disable_pd(pd))

    def is_pd_enabled(self, address: int) -> bool:
        """Whether a PD is being polled."""
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(self.ctx.is_pd_enabled(pd))

    # -- file transfer ------------------------------------------------------

    def register_file_ops(self, address: int, fops: FileOps) -> bool:
        """Supply the file a subsequent FileTransfer command will send.

        Call it before start(); once the CP is running, LibOSDP refuses it
        and this raises ValueError.
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(
                self.ctx.register_file_ops(
                    pd,
                    {
                        "open": fops.open,
                        "read": fops.read,
                        "write": fops.write,
                        "close": fops.close,
                    },
                )
            )

    def get_file_tx_status(self, address: int) -> FileTxStatus | None:
        """How far the running transfer has got, or None if none is running.

        Read without taking the refresh lock: progress is sampled, not
        latched, so it is a snapshot that the transfer has likely moved past
        by the time it is used -- which is what a progress reading is.
        """
        pd = self.pd_addr.index(address)
        status = self.ctx.get_file_tx_status(pd)
        if status is None:
            return None
        return FileTxStatus(size=status["size"], offset=status["offset"])

    def get_metrics(self, address: int) -> Metrics | None:
        """Packet and command counters for a PD."""
        pd = self.pd_addr.index(address)
        with self.lock:
            metrics = self.ctx.get_metrics(pd)
        if metrics is None:
            return None
        return Metrics(**metrics)

    # -- lifecycle ----------------------------------------------------------

    @staticmethod
    def _refresh_loop(
        stop: threading.Event, lock: threading.RLock, ctx: Any
    ) -> None:
        while not stop.is_set():
            with lock:
                ctx.refresh()
            time.sleep(REFRESH_INTERVAL)

    def start(self) -> None:
        """Start servicing the bus."""
        if self.thread:
            raise RuntimeError("Thread already running!")
        self.event = threading.Event()
        self.thread = threading.Thread(
            name="cp",
            target=self._refresh_loop,
            args=(self.event, self.lock, self.ctx),
        )
        self.thread.start()

    def stop(self) -> None:
        """Stop servicing the bus."""
        if not self.thread or self.event is None:
            raise RuntimeError("Thread not running!")
        while self.thread.is_alive():
            self.event.set()
            self.thread.join(2)
            if not self.thread.is_alive():
                self.thread = None
                break

    def teardown(self) -> None:
        """Stop the bus and release the library context.

        The CP cannot be used afterwards.
        """
        self.stop()
        self._ctx = None

    # -- waiting ------------------------------------------------------------

    def _wait_until(
        self, predicate: Callable[[], bool], timeout: float
    ) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            time.sleep(0.5)
            if predicate():
                return True
        return False

    def online_wait(self, address: int, timeout: float = 8) -> bool:
        """Block until a PD comes online, or the timeout elapses."""
        return self._wait_until(lambda: self.is_online(address), timeout)

    def online_wait_all(self, timeout: float = 10) -> bool:
        """Block until every PD is online, or the timeout elapses."""
        return self._wait_until(
            lambda: self.get_num_online() == self.num_pds, timeout
        )

    def offline_wait(self, address: int, timeout: float = 8) -> bool:
        """Block until a PD goes offline, or the timeout elapses."""
        return self._wait_until(lambda: not self.is_online(address), timeout)

    def sc_wait(self, address: int, timeout: float = 5) -> bool:
        """Block until a PD's secure channel comes up."""
        return self._wait_until(lambda: self.is_sc_active(address), timeout)

    def sc_wait_all(self, timeout: float = 5) -> bool:
        """Block until every PD's secure channel comes up."""
        return self._wait_until(
            lambda: self.get_num_sc_active() == self.num_pds, timeout
        )
