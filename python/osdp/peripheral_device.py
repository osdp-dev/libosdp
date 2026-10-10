#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""The Peripheral Device side of an OSDP bus."""

import queue
import threading
import time
from typing import Any, Callable

from . import _sys
from ._marshal import decode_command, decode_event, encode_command, encode_event
from .commands import Command, Notification
from .enums import CompletionStatus, LibFlag, LogLevel, NotificationType
from .errors import NakError
from .events import Event
from .types import FileOps, FileTxStatus, Metrics, PDCapabilities, PDInfo

__all__ = ["CommandHandler", "EventCompletionHandler", "PeripheralDevice"]

CommandHandler = Callable[[Command], "Command | None"]
"""Called with each command the CP sends. Runs on the refresh thread.

Return None to acknowledge the command.

Return a command to acknowledge it and send that as an inline reply. In
practice that means answering a `commands.Status` query with a `commands.Status`
carrying the report.

Raise `NakError` to reject the command. Any other exception propagates.
"""

EventCompletionHandler = Callable[[Event, CompletionStatus], None]
"""Called with (event, status) when a submitted event settles.

Every accepted event is reported exactly once, including ones that were flushed
or torn down before reaching the wire.
"""

REFRESH_INTERVAL = 0.020
"""How often to service the bus. OSDP requires at least once every 50ms."""


class PeripheralDevice:
    """Answers a CP on one address.

    Runs a background thread that services the bus; nothing happens until
    `start()` is called.

    Example:
        def on_command(cmd: Command) -> Command | None:
            match cmd:
                case commands.Status(type=t):
                    return commands.Status(type=t, report=read_inputs())
                case commands.BioRead():
                    raise NakError(NakCode.BioType)
            return None

        pd = PeripheralDevice(info, caps, command_handler=on_command)
        pd.start()
    """

    def __init__(
        self,
        pd_info: PDInfo,
        pd_cap: PDCapabilities,
        log_level: LogLevel = LogLevel.Info,
        command_handler: CommandHandler | None = None,
        event_completion_handler: EventCompletionHandler | None = None,
    ) -> None:
        self.address = pd_info.address
        self.command_queue: queue.Queue[Command] = queue.Queue()
        self.user_command_handler = command_handler
        self.user_event_completion_handler = event_completion_handler

        # State kept up to date from the library's notifications so that
        # reading it costs nothing and never crosses into C. The library
        # reports every transition, so this cannot drift.
        self._online = False
        self._sc_active = False

        # Notifications are how the above is fed, so they are turned on at the
        # library level whatever the caller asked for. Whether the caller also
        # gets to see them stays their own flag's decision.
        self._forward_notifications = (
            LibFlag.EnableNotification in pd_info.flags
        )
        info = pd_info.to_dict()
        info["flags"] |= int(LibFlag.EnableNotification)

        _sys.set_loglevel(int(log_level))
        self._ctx: _sys.PeripheralDevice | None = _sys.PeripheralDevice(
            info, capabilities=pd_cap.to_dict_list()
        )
        # Our handlers always run, so that get_command() works whether or not
        # the caller registered one of their own.
        self.ctx.set_command_callback(self._internal_command_handler)
        self.ctx.set_event_completion_callback(
            self._internal_event_completion_handler
        )

        self.event: threading.Event | None = None
        self.lock = threading.RLock()
        self.thread: threading.Thread | None = None

    @property
    def ctx(self) -> "_sys.PeripheralDevice":
        """The library context. Raises once this PD has been torn down."""
        if self._ctx is None:
            raise RuntimeError("This PeripheralDevice has been torn down")
        return self._ctx

    # -- callbacks from the library -----------------------------------------

    def _internal_command_handler(
        self, command: dict[str, Any]
    ) -> tuple[int, dict[str, Any] | None]:
        decoded = decode_command(command)

        if isinstance(decoded, Notification):
            self._track_notification(decoded)
            if not self._forward_notifications:
                # This one was only ever ours; the caller did not ask to see
                # library notifications.
                return 0, None

        self.command_queue.put(decoded)

        if self.user_command_handler is None:
            return 0, None

        try:
            reply = self.user_command_handler(decoded)
        except NakError as nak:
            return -int(nak.code), None

        if reply is None:
            return 0, None
        return 0, encode_command(reply)

    def _track_notification(self, note: Notification) -> None:
        """Fold a notification into the state this object reports.

        Runs on the refresh thread, before anything else sees the command.
        """
        if note.type is NotificationType.PeripheralDeviceStatus:
            self._online = note.online
            if not note.online:
                # With no CP reaching us there is no secure channel either;
                # the library says so separately, but not always first.
                self._sc_active = False
        elif note.type is NotificationType.SecureChannelStatus:
            self._sc_active = note.active

    def _internal_event_completion_handler(
        self, event: dict[str, Any], status: int
    ) -> None:
        if self.user_event_completion_handler:
            self.user_event_completion_handler(
                decode_event(event), CompletionStatus(status)
            )

    def set_command_handler(self, handler: CommandHandler | None) -> None:
        """Install the handler called for every command from the CP."""
        self.user_command_handler = handler

    def set_event_completion_handler(
        self, handler: EventCompletionHandler | None
    ) -> None:
        """Install the handler called when a submitted event settles."""
        self.user_event_completion_handler = handler

    # -- commands and events ------------------------------------------------

    def submit_event(self, event: Event) -> bool:
        """Queue an event for the CP. Returns False if it could not be queued.

        Submitting from inside the command handler sends the event as an inline
        reply to that command. Submitting later sends it as a poll response, and
        the command is acknowledged on its own.
        """
        with self.lock:
            return bool(self.ctx.submit_event(encode_event(event)))

    def flush_events(self) -> int:
        """Drop every queued event. Returns how many were dropped.

        Each one is still reported to the completion handler, as Flushed.
        """
        with self.lock:
            return self.ctx.flush_events()

    def get_command(self, timeout: float = 5) -> Command | None:
        """Pop the next command from the CP, or None if none arrives in time.

        A negative timeout does not block.
        """
        try:
            return self.command_queue.get(timeout >= 0, timeout=timeout)
        except queue.Empty:
            return None

    # -- state --------------------------------------------------------------

    def is_online(self) -> bool:
        """Whether the CP is polling us.

        Answered from state this object keeps up to date from the library's
        notifications, so it costs a single attribute read.
        """
        self.ctx  # refuse to answer for a context that has been released
        return self._online

    def is_sc_active(self) -> bool:
        """Whether the secure channel is up.

        Answered from tracked state, for the same reason as is_online().
        """
        self.ctx  # refuse to answer for a context that has been released
        return self._sc_active

    def sc_wait(self, timeout: float = 8) -> bool:
        """Block until the secure channel comes up, or the timeout elapses."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            time.sleep(0.5)
            if self.is_sc_active():
                return True
        return False

    # -- file transfer ------------------------------------------------------

    def register_file_ops(self, fops: FileOps) -> bool:
        """Supply the sink a file transfer from the CP will write into.

        Call it before start(); once the PD is running, LibOSDP refuses it
        and this raises ValueError.
        """
        with self.lock:
            return bool(
                self.ctx.register_file_ops(
                    0,
                    {
                        "open": fops.open,
                        "read": fops.read,
                        "write": fops.write,
                        "close": fops.close,
                    },
                )
            )

    def get_file_tx_status(self) -> FileTxStatus | None:
        """How far the running transfer has got, or None if none is running."""
        with self.lock:
            status = self.ctx.get_file_tx_status(0)
        if status is None:
            return None
        return FileTxStatus(size=status["size"], offset=status["offset"])

    def get_metrics(self) -> Metrics | None:
        """Packet and command counters."""
        with self.lock:
            metrics = self.ctx.get_metrics(0)
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
            name="pd",
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

        The PD cannot be used afterwards.
        """
        self.stop()
        self._ctx = None
