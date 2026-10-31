#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""The Control Panel side of an OSDP bus."""

import queue
import threading
import time
from typing import Any, Callable

from . import _sys, commands, events
from ._marshal import decode_command, decode_event, encode_command
from .commands import Command
from .enums import (
    Capability,
    CompletionStatus,
    LibFlag,
    LogLevel,
    MpMsgType,
    NotificationType,
    TrsFailReason,
    TrsSessionStatus,
)
from .errors import TrsSessionError
from .events import Event, Notification
from .types import FileOps, FileTxStatus, Metrics, PdId, PDInfo

__all__ = [
    "CommandCompletionHandler",
    "ControlPanel",
    "EventHandler",
    "TrsSession",
]

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

# The events that answer each card command a TrsSession sends; a reader error
# answers any of them.
_TRS_ANSWERS: dict[type, tuple[type, ...]] = {
    commands.TrsStart: (),
    commands.TrsStop: (),
    commands.TrsSendApdu: (events.TrsCardData, events.TrsError),
    commands.TrsEnterPin: (events.TrsPinComplete, events.TrsError),
    commands.TrsCardScan: (events.TrsCardPresent, events.TrsError),
}


class _TrsExchange:
    """One card command a TrsSession is waiting on.

    The library delivers a card command's answer before the command
    completes, and completes every command exactly once, so the completion
    is when the exchange is decided and the answer is already in hand.
    """

    def __init__(self, command: Command) -> None:
        self.command = command
        self.answer: Event | None = None
        self.failure: TrsFailReason | None = None
        self.status: CompletionStatus | None = None
        self.done = threading.Event()

    def takes(self, event: Event) -> bool:
        return self.answer is None and isinstance(
            event, _TRS_ANSWERS[type(self.command)]
        )


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
        self._trs_exchange: list[_TrsExchange | None] = [
            None for _ in self.pd_addr
        ]

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
            exchange = self._trs_exchange[pd]
            if (
                exchange is not None
                and decoded.type is NotificationType.TrsStatus
                and decoded.trs_status is TrsSessionStatus.Failed
            ):
                # Sent ahead of the completions it settles
                exchange.failure = decoded.trs_fail_reason
            if not self._forward_notifications[pd]:
                # This one was only ever ours; the caller did not ask to see
                # library notifications.
                return 0
        exchange = self._trs_exchange[pd]
        if exchange is not None and exchange.takes(decoded):
            # The TrsSession waiting on it returns it; get_event() would
            # otherwise hand the same answer out a second time.
            exchange.answer = decoded
        else:
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
        decoded = decode_command(command)
        exchange = self._trs_exchange[pd]
        if exchange is not None and decoded == exchange.command:
            self._trs_exchange[pd] = None
            exchange.status = CompletionStatus(status)
            exchange.done.set()
        if self.user_command_completion_handler:
            self.user_command_completion_handler(
                self.pd_addr[pd], decoded, CompletionStatus(status)
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

    def set_scbk(self, address: int, scbk: bytes) -> bool:
        """Replace a disabled PD's secure channel base key (16 bytes).

        How a CP tries another key after a key rotation that never settled:
        disable the PD, set the key, enable it again. Returns False if the
        PD is not disabled.

        @see osdp_cp_set_scbk
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(self.ctx.set_scbk(pd, scbk))

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

    # -- transparent reader support -----------------------------------------

    def trs_scan_enable(
        self,
        address: int,
        mode0_dwell_ms: int = 0,
        mode1_dwell_ms: int = 0,
        hold_ms: int = 0,
        reader: int = 0,
    ) -> bool:
        """Scan a TRS-capable PD for smart cards between card sessions.

        For readers that do not announce cards in their default mode. The
        library alternates the reader between its default mode, for
        `mode0_dwell_ms`, and transparent mode, for `mode1_dwell_ms`. A
        sighting arrives as an `events.TrsCardPresent` or
        `events.TrsCardInfo`, and transparent mode is then held for `hold_ms`
        so that a `commands.TrsStart` submitted in response adopts it as is.
        A zero selects that parameter's default. `reader` is the zero-based
        reader the scan asks.

        Returns False if the library refused it.

        @see osdp_cp_trs_scan_enable
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(
                self.ctx.trs_scan_enable(
                    pd, mode0_dwell_ms, mode1_dwell_ms, hold_ms, reader
                )
            )

    def trs_scan_disable(self, address: int) -> bool:
        """Stop the smart-card presence scan on a PD.

        @see osdp_cp_trs_scan_disable
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return bool(self.ctx.trs_scan_disable(pd))

    def trs_get_max_apdu_len(self, address: int) -> int:
        """Largest C-APDU a `commands.TrsSendApdu` can carry to a PD.

        Bounded by the packet size in use, so well below
        `commands.MAX_TRS_APDU_LEN` by default, and final only once the PD
        has been online.

        @see osdp_cp_trs_get_max_apdu_len
        """
        pd = self.pd_addr.index(address)
        with self.lock:
            return self.ctx.trs_get_max_apdu_len(pd)

    def trs_session(self, address: int, reader: int = 0) -> "TrsSession":
        """Open a card session on a PD's reader, for use in a `with` block.

        Entering submits a `commands.TrsStart` and returns once the reader is
        in transparent mode; leaving submits the `commands.TrsStop` that
        closes it and returns once the reader is back out. In between, the
        session's methods send one card command at a time and return the
        card's answer.

        Example:
            with cp.trs_session(101) as card:
                answer = card.transceive(bytes.fromhex("00A4040007D2760000850101"))

        @see osdp_trs_cmd_e
        """
        return TrsSession(self, address, reader)

    def _trs_settle(
        self, address: int, command: Command, timeout: float | None
    ) -> _TrsExchange:
        """Submit a TRS command and wait for the library to complete it."""
        pd = self.pd_addr.index(address)
        exchange = _TrsExchange(command)
        with self.lock:
            if self._trs_exchange[pd] is not None:
                raise TrsSessionError("a card exchange is already running")
            # Registered before the submit, under the lock the refresh loop
            # holds, so the answer cannot arrive ahead of it.
            self._trs_exchange[pd] = exchange
            if not self.ctx.submit_command(pd, encode_command(command)):
                self._trs_exchange[pd] = None
                raise TrsSessionError("card command refused")
        if not exchange.done.wait(timeout):
            with self.lock:
                if self._trs_exchange[pd] is exchange:
                    self._trs_exchange[pd] = None
            raise TimeoutError("no verdict on the card command in time")
        return exchange

    def _trs_exchange_run(
        self, address: int, command: Command, timeout: float | None
    ) -> Event:
        exchange = self._trs_settle(address, command, timeout)
        answer = exchange.answer
        if exchange.status is CompletionStatus.Ok and answer is not None:
            return answer
        code = answer.code if isinstance(answer, events.TrsError) else None
        raise TrsSessionError(
            "card exchange failed", exchange.status, code, exchange.failure
        )

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
        while not predicate():
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.02)
        return True

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


class TrsSession:
    """A card session on one reader of one PD; see
    `ControlPanel.trs_session()`.

    Each method sends one card command and blocks until the library has
    settled it, returning the card's answer. The answer is returned here
    rather than queued for `ControlPanel.get_event()`; an event handler
    still sees it.

    `timeout` defaults to None, which waits for the library's own verdict:
    it fails a card command the reader leaves unanswered (after
    OSDP_TRS_ANSWER_TOUT_MS, 5 s by default) and fails or flushes every
    command of a session that ends early, so the wait always ends. A PIN
    entry waits as long as the person takes; give it a timeout to bound
    that. A TimeoutError leaves the command running; leave the session to
    close it.

    Raises TrsSessionError when the session cannot open or a card exchange
    did not happen.
    """

    def __init__(self, cp: ControlPanel, address: int, reader: int) -> None:
        self.cp = cp
        self.address = address
        self.reader = reader

    def __enter__(self) -> "TrsSession":
        start = self.cp._trs_settle(
            self.address, commands.TrsStart(reader=self.reader), None
        )
        if start.status is not CompletionStatus.Ok:
            raise TrsSessionError(
                "card session did not open", start.status, None, start.failure
            )
        return self

    def __exit__(self, *exc_info: object) -> None:
        # Returns once the reader is back out of transparent mode. A STOP is
        # refused only when the session already ended on its own, which
        # leaves nothing to close.
        try:
            self.cp._trs_settle(
                self.address, commands.TrsStop(reader=self.reader), None
            )
        except TrsSessionError:
            pass

    def transceive(self, apdu: bytes, timeout: float | None = None) -> bytes:
        """Send a C-APDU to the card and return its R-APDU, status word
        included."""
        answer = self.cp._trs_exchange_run(
            self.address,
            commands.TrsSendApdu(reader=self.reader, apdu=apdu),
            timeout,
        )
        assert isinstance(answer, events.TrsCardData)
        return answer.apdu

    def enter_pin(
        self, command: commands.TrsEnterPin, timeout: float | None = None
    ) -> events.TrsPinComplete:
        """Run a secure PIN entry on the reader. `command.reader` must be
        this session's reader."""
        answer = self.cp._trs_exchange_run(self.address, command, timeout)
        assert isinstance(answer, events.TrsPinComplete)
        return answer

    def card_scan(
        self, timeout: float | None = None
    ) -> events.TrsCardPresent:
        """Ask the reader whether a card is in its field."""
        answer = self.cp._trs_exchange_run(
            self.address, commands.TrsCardScan(reader=self.reader), timeout
        )
        assert isinstance(answer, events.TrsCardPresent)
        return answer
