#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Commands a CP sends to a PD.

Each class here mirrors one `struct osdp_cmd_*` from `osdp.h`. A CP submits
them with `ControlPanel.submit_command()`; a PD receives them in its command
handler, where `match` is the natural way to tell them apart:

    >>> def handler(cmd: Command) -> None:
    ...     match cmd:
    ...         case Buzzer(rep_count=n):
    ...             beep(n)
    ...         case LED(permanent=params) if params is not None:
    ...             set_led(params.on_color)

Every field is validated on construction, so an out-of-range value or an
oversized payload raises ValueError here rather than being truncated on its way
into a packet.
"""

from dataclasses import dataclass
from typing import ClassVar, TypeAlias

from . import _sys
from ._validate import (
    UINT8_MAX,
    UINT16_MAX,
    UINT32_MAX,
    check_blink,
    check_length,
    check_range,
)
from .enums import (
    BioFormat,
    BioType,
    BuzzerControlCode,
    CommandId,
    LEDColor,
    NotificationType,
    OutputControlCode,
    PermanentLEDControlCode,
    StatusReportType,
    TemporaryLEDControlCode,
    TextControlCode,
    TrsPinComplete,
    TrsPinFormat,
    TrsSessionStatus,
)

__all__ = [
    "BioMatch",
    "BioRead",
    "Buzzer",
    "Command",
    "Comset",
    "ComsetDone",
    "FileTransfer",
    "Keyset",
    "LED",
    "Manufacturer",
    "Notification",
    "Output",
    "PermanentLEDParams",
    "Status",
    "TDSet",
    "TemporaryLEDParams",
    "Text",
    "TrsCardScan",
    "TrsEnterPin",
    "TrsPinBlock",
    "TrsPinLengthField",
    "TrsSendApdu",
    "TrsStart",
    "TrsStop",
]

MAX_TEXT_LEN = _sys.CMD_TEXT_MAX_LEN
"""Longest message a Text command can carry, in UTF-8 bytes."""

MAX_KEYSET_LEN = _sys.CMD_KEYSET_KEY_MAX_LEN
"""Longest key a Keyset command can carry."""

MAX_MFG_DATA_LEN = _sys.CMD_MFG_MAX_DATALEN

#: Max smartcard/PIV payload a command or reply can carry, in bytes.
MAX_PIV_DATA_LEN = _sys.PIV_DATA_MAX_LEN
"""Longest payload a Manufacturer command can carry."""

MAX_BIO_TEMPLATE_LEN = _sys.CMD_BIOMATCH_MAX_TEMPLATE_LEN
"""Longest biometric template a BioMatch command can carry."""

MAX_STATUS_REPORT_LEN = _sys.STATUS_REPORT_MAX_LEN
"""Most entries a status report can carry."""

MAX_TRS_APDU_LEN = _sys.TRS_APDU_MAX_LEN
"""Size of the APDU buffer in a TRS command. What fits on the wire is less;
ask `ControlPanel.trs_get_max_apdu_len()`."""


@dataclass(frozen=True, slots=True, kw_only=True)
class Output:
    """Set the state of an output line.

    @see osdp_cmd_output

    Example:
        >>> cmd = Output(output_no=0, control_code=OutputControlCode.TemporaryOn,
        ...              timer_count=30)
        >>> cmd.timer_count
        30
    """

    ID: ClassVar[CommandId] = CommandId.Output

    output_no: int = 0
    """Zero-based output line number."""

    control_code: OutputControlCode = OutputControlCode.Nop
    """What to do to the output line."""

    timer_count: int = 0
    """How long a temporary state lasts, in units of 100ms."""

    def __post_init__(self) -> None:
        check_range("output_no", self.output_no, UINT8_MAX)
        check_range("timer_count", self.timer_count, UINT16_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class TemporaryLEDParams:
    """The temporary half of an LED command.

    A temporary state runs for `timer_count` and then gives way to whatever
    permanent state the LED was in.

    @see osdp_cmd_led_params

    Example:
        >>> params = TemporaryLEDParams(on_color=LEDColor.Red, on_count=10,
        ...                             off_count=10, timer_count=20)
        >>> params.on_color.name
        'Red'
    """

    control_code: TemporaryLEDControlCode = TemporaryLEDControlCode.Set
    """Whether to set the temporary state or cancel a running one."""

    on_count: int = 0
    """Duration of the ON phase of a blink, in units of 100ms."""

    off_count: int = 0
    """Duration of the OFF phase of a blink, in units of 100ms."""

    on_color: LEDColor = LEDColor.Black
    """Color shown during the ON phase."""

    off_color: LEDColor = LEDColor.Black
    """Color shown during the OFF phase."""

    timer_count: int = 0
    """How long the temporary state lasts, in units of 100ms."""

    def __post_init__(self) -> None:
        check_range("on_count", self.on_count, UINT8_MAX)
        check_range("off_count", self.off_count, UINT8_MAX)
        check_range("timer_count", self.timer_count, UINT16_MAX)
        if self.control_code == TemporaryLEDControlCode.Set:
            check_blink("TemporaryLEDParams", self.on_count, self.off_count)


@dataclass(frozen=True, slots=True, kw_only=True)
class PermanentLEDParams:
    """The permanent half of an LED command.

    A permanent state persists until it is changed. It has no timer; that is
    what makes it permanent.

    @see osdp_cmd_led_params

    Example:
        A steady green: light up, and never blink off.

        >>> params = PermanentLEDParams(on_color=LEDColor.Green, on_count=1)
        >>> params.control_code.name
        'Set'

        Both durations zero is rejected; the PD would refuse such a command.

        >>> PermanentLEDParams(on_color=LEDColor.Green)
        Traceback (most recent call last):
        ValueError: PermanentLEDParams: on_count and off_count cannot both be zero when the control code is Set. For a steady color use on_count=1, off_count=0
    """

    control_code: PermanentLEDControlCode = PermanentLEDControlCode.Set
    """Whether to apply the permanent state."""

    on_count: int = 0
    """Duration of the ON phase of a blink, in units of 100ms."""

    off_count: int = 0
    """Duration of the OFF phase of a blink, in units of 100ms."""

    on_color: LEDColor = LEDColor.Black
    """Color shown during the ON phase."""

    off_color: LEDColor = LEDColor.Black
    """Color shown during the OFF phase."""

    def __post_init__(self) -> None:
        check_range("on_count", self.on_count, UINT8_MAX)
        check_range("off_count", self.off_count, UINT8_MAX)
        if self.control_code == PermanentLEDControlCode.Set:
            check_blink("PermanentLEDParams", self.on_count, self.off_count)


@dataclass(frozen=True, slots=True, kw_only=True)
class LED:
    """Control a reader LED.

    An LED carries two independent parameter blocks. Setting only `permanent`
    changes the LED until something else changes it; setting only `temporary`
    overrides it for a while. Setting both at once is how a temporary state is
    cancelled and a new permanent state applied in the same command.

    A block whose control code is Nop does nothing, so it is normalised to
    None; passing neither block is an error.

    @see osdp_cmd_led

    Example:
        Flash red for two seconds, then go back to whatever it was:

        >>> cmd = LED(temporary=TemporaryLEDParams(on_color=LEDColor.Red,
        ...                                        on_count=5, off_count=5,
        ...                                        timer_count=20))
        >>> cmd.permanent is None
        True

        Cancel a running temporary state and go steady green, in one command:

        >>> cmd = LED(
        ...     temporary=TemporaryLEDParams(
        ...         control_code=TemporaryLEDControlCode.Cancel),
        ...     permanent=PermanentLEDParams(on_color=LEDColor.Green,
        ...                                  on_count=1),
        ... )
        >>> cmd.temporary.control_code.name
        'Cancel'
    """

    ID: ClassVar[CommandId] = CommandId.LED

    reader: int = 0
    """Zero-based reader number."""

    led_number: int = 0
    """Zero-based LED number on that reader."""

    temporary: TemporaryLEDParams | None = None
    """Temporary state to apply, or None to leave it alone."""

    permanent: PermanentLEDParams | None = None
    """Permanent state to apply, or None to leave it alone."""

    def __post_init__(self) -> None:
        check_range("reader", self.reader, UINT8_MAX)
        check_range("led_number", self.led_number, UINT8_MAX)

        # A no-op block and an absent block mean the same thing to the PD;
        # collapsing them here keeps a command equal to itself after a round
        # trip through the C layer, which cannot represent the difference.
        if (
            self.temporary is not None
            and self.temporary.control_code == TemporaryLEDControlCode.Nop
        ):
            object.__setattr__(self, "temporary", None)
        if (
            self.permanent is not None
            and self.permanent.control_code == PermanentLEDControlCode.Nop
        ):
            object.__setattr__(self, "permanent", None)

        if self.temporary is None and self.permanent is None:
            raise ValueError("LED needs a temporary or a permanent block")


@dataclass(frozen=True, slots=True, kw_only=True)
class Buzzer:
    """Control a reader's audible output.

    @see osdp_cmd_buzzer

    Example:
        >>> cmd = Buzzer(control_code=BuzzerControlCode.DefaultTone,
        ...              on_count=2, off_count=2, rep_count=3)
        >>> cmd.rep_count
        3
    """

    ID: ClassVar[CommandId] = CommandId.Buzzer

    reader: int = 0
    """Zero-based reader number."""

    control_code: BuzzerControlCode = BuzzerControlCode.DefaultTone
    """Whether to sound the tone or silence it."""

    on_count: int = 0
    """Duration of the ON phase, in units of 100ms."""

    off_count: int = 0
    """Duration of the OFF phase, in units of 100ms."""

    rep_count: int = 0
    """Number of ON/OFF cycles to play. Zero repeats forever."""

    def __post_init__(self) -> None:
        check_range("reader", self.reader, UINT8_MAX)
        check_range("on_count", self.on_count, UINT8_MAX)
        check_range("off_count", self.off_count, UINT8_MAX)
        check_range("rep_count", self.rep_count, UINT8_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class Text:
    """Show a message on a reader's display.

    @see osdp_cmd_text

    Example:
        >>> cmd = Text(control_code=TextControlCode.PermanentNoWrap,
        ...            data="PLEASE WAIT")
        >>> cmd.offset_row
        1
    """

    ID: ClassVar[CommandId] = CommandId.Text

    reader: int = 0
    """Zero-based reader number."""

    control_code: TextControlCode = TextControlCode.PermanentNoWrap
    """How long the message stays up, and whether it wraps."""

    temp_time: int = 0
    """How long a temporary message stays up, in seconds."""

    offset_row: int = 1
    """Row to start at. Rows are numbered from 1."""

    offset_col: int = 1
    """Column to start at. Columns are numbered from 1."""

    data: str = ""
    """The message. At most 32 bytes once encoded as UTF-8, not 32 characters."""

    def __post_init__(self) -> None:
        check_range("reader", self.reader, UINT8_MAX)
        check_range("temp_time", self.temp_time, UINT8_MAX)
        check_range("offset_row", self.offset_row, UINT8_MAX)
        check_range("offset_col", self.offset_col, UINT8_MAX)
        check_length("data", self.data.encode(), MAX_TEXT_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class TDSet:
    """Set the PD's local time and date.

    Obsolete in current editions of the OSDP specification; supported for
    interop with legacy devices. The PD must declare Capability.TimeKeeping.

    @see osdp_cmd_tdset

    Example:
        >>> cmd = TDSet(year=2026, month=7, day=17,
        ...             hour=12, minute=30, second=45)
        >>> cmd.month
        7
    """

    ID: ClassVar[CommandId] = CommandId.TDSet

    year: int
    """Full year in local time, e.g., 2026."""

    month: int
    """Month of year; 1 - 12."""

    day: int
    """Day of month; 1 - 31."""

    hour: int
    """Hours since midnight; 0 - 23."""

    minute: int
    """Minutes past the hour; 0 - 59."""

    second: int
    """Seconds past the minute; 0 - 59."""

    def __post_init__(self) -> None:
        check_range("year", self.year, UINT16_MAX)
        check_range("month", self.month, 12)
        check_range("day", self.day, 31)
        check_range("hour", self.hour, 23)
        check_range("minute", self.minute, 59)
        check_range("second", self.second, 59)


@dataclass(frozen=True, slots=True, kw_only=True)
class Keyset:
    """Install a new secure channel base key on the PD.

    The PD stores the key and uses it for every subsequent secure channel
    handshake, so losing it means losing the device.

    @see osdp_cmd_keyset

    Example:
        >>> cmd = Keyset(data=bytes(range(16)))
        >>> cmd.type
        1
    """

    ID: ClassVar[CommandId] = CommandId.Keyset

    type: int = 1
    """Key type. 1 is the secure channel base key, the only one defined."""

    data: bytes = b""
    """The key itself."""

    def __post_init__(self) -> None:
        check_range("type", self.type, UINT8_MAX)
        if not self.data:
            raise ValueError("Keyset needs a key")
        check_length("data", self.data, MAX_KEYSET_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class Comset:
    """Change the PD's address and baud rate.

    The PD applies the change after replying, so the CP must reconfigure its
    own channel to match or it will lose the device.

    @see osdp_cmd_comset

    Example:
        >>> cmd = Comset(address=42, baud_rate=115200)
        >>> cmd.address
        42
    """

    ID: ClassVar[CommandId] = CommandId.Comset

    address: int = 0
    """The PD's new address."""

    baud_rate: int = 9600
    """The new baud rate: 9600, 19200, 38400, 115200 or 230400."""

    def __post_init__(self) -> None:
        check_range("address", self.address, UINT8_MAX)
        check_range("baud_rate", self.baud_rate, UINT32_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class ComsetDone:
    """Tells a PD app that a Comset it received has taken effect.

    Delivered to a PD's command handler by the library. Apps never submit this.

    @see osdp_cmd_e

    Example:
        >>> cmd = ComsetDone(address=42, baud_rate=115200)
        >>> cmd.baud_rate
        115200
    """

    ID: ClassVar[CommandId] = CommandId.ComsetDone

    address: int = 0
    """The address the PD now answers on."""

    baud_rate: int = 9600
    """The baud rate the PD now runs at."""

    def __post_init__(self) -> None:
        check_range("address", self.address, UINT8_MAX)
        check_range("baud_rate", self.baud_rate, UINT32_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class Manufacturer:
    """Send a vendor-specific command.

    A PD may answer inline by returning a `events.ManufacturerReply`,
    `events.ManufacturerStatus` or `events.ManufacturerError` from its command
    handler.

    @see osdp_cmd_mfg

    Example:
        >>> cmd = Manufacturer(vendor_code=0x00030201, data=b"\\x01\\x02")
        >>> cmd.data
        b'\\x01\\x02'
    """

    ID: ClassVar[CommandId] = CommandId.Manufacturer

    vendor_code: int = 0
    """The vendor's 3-byte IEEE OUI."""

    data: bytes = b""
    """Vendor-defined payload, at most 64 bytes."""

    def __post_init__(self) -> None:
        check_range("vendor_code", self.vendor_code, UINT32_MAX)
        check_length("data", self.data, MAX_MFG_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class BioRead:
    """Ask a reader to capture a biometric and return the template.

    The PD answers with an `events.BioRead`.

    @see osdp_cmd_bioread

    Example:
        >>> cmd = BioRead(type=BioType.RightThumbPrint,
        ...               format=BioFormat.AnsiIncits378, quality=200)
        >>> cmd.type.name
        'RightThumbPrint'
    """

    ID: ClassVar[CommandId] = CommandId.BioRead

    reader: int = 0
    """Zero-based reader number."""

    type: BioType = BioType.NotSpecified
    """Which biometric to capture."""

    format: BioFormat = BioFormat.NotSpecified
    """The template encoding to return."""

    quality: int = 0
    """Required scan quality, from 0x00 (worst) to 0xFF (best)."""

    def __post_init__(self) -> None:
        check_range("reader", self.reader, UINT8_MAX)
        check_range("quality", self.quality, UINT8_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class BioMatch:
    """Ask a reader to scan a biometric and match it against a template.

    The PD answers with an `events.BioMatch` carrying the match score.

    @see osdp_cmd_biomatch

    Example:
        >>> cmd = BioMatch(type=BioType.RightThumbPrint,
        ...                format=BioFormat.AnsiIncits378, quality=200,
        ...                data=b"template")
        >>> len(cmd.data)
        8
    """

    ID: ClassVar[CommandId] = CommandId.BioMatch

    reader: int = 0
    """Zero-based reader number."""

    type: BioType = BioType.NotSpecified
    """Which biometric to scan."""

    format: BioFormat = BioFormat.NotSpecified
    """The encoding of the template in `data`."""

    quality: int = 0
    """Match threshold, from 0x00 (loosest) to 0xFF (strictest)."""

    data: bytes = b""
    """The template to match against, at most 128 bytes."""

    def __post_init__(self) -> None:
        check_range("reader", self.reader, UINT8_MAX)
        check_range("quality", self.quality, UINT8_MAX)
        check_length("data", self.data, MAX_BIO_TEMPLATE_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class FileTransfer:
    """Start, or cancel, a file transfer to the PD.

    The file's contents come from the file ops registered with
    `register_file_ops()`; this command only starts the process.

    @see osdp_cmd_file_tx

    To stop a transfer that is already running, call
    `ControlPanel.cancel()` rather than submitting another command.

    Example:
        >>> cmd = FileTransfer(id=1)
        >>> cmd.id
        1
    """

    ID: ClassVar[CommandId] = CommandId.FileTransfer

    id: int = 0
    """The file id, agreed out of band between the CP and the PD."""

    flags: int = 0
    """Reserved by the OSDP spec; set to zero."""


@dataclass(frozen=True, slots=True, kw_only=True)
class Status:
    """Query the PD's status, or a PD's answer to that query.

    A CP submits this with an empty `report` to ask. A PD answers by returning
    a Status with one report byte per entry from its command handler.

    @see osdp_status_report

    Example:
        >>> query = Status(type=StatusReportType.Input)
        >>> query.report
        b''

        A PD answering with three input contacts, the middle one active:

        >>> reply = Status(type=StatusReportType.Input, report=bytes([0, 1, 0]))
        >>> len(reply.report)
        3
    """

    ID: ClassVar[CommandId] = CommandId.Status

    type: StatusReportType = StatusReportType.Input
    """Which set of status bits this concerns."""

    report: bytes = b""
    """One byte per entry. Empty in a query."""

    def __post_init__(self) -> None:
        check_length("report", self.report, MAX_STATUS_REPORT_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class Notification:
    """A library notification delivered to a PD app.

    Produced by the library, not by a peer, when `LibFlag.EnableNotification`
    is set. Apps never submit this. The CP-side equivalent is
    `events.Notification`, which has the same shape.

    @see osdp_notification

    Example:
        >>> note = Notification(type=NotificationType.PeripheralDeviceStatus,
        ...                     online=True)
        >>> note.type.name
        'PeripheralDeviceStatus'
    """

    ID: ClassVar[CommandId] = CommandId.Notification

    type: NotificationType = NotificationType.SecureChannelStatus
    """What happened. Selects which fields below are meaningful."""

    active: bool = False
    """Whether the secure channel is up.
    Only for NotificationType.SecureChannelStatus."""

    scbk_d: bool = False
    """True when the SC uses SCBK-D (install key) rather than SCBK.
    Only for NotificationType.SecureChannelStatus."""

    online: bool = False
    """Whether the PD is reachable.
    Only for NotificationType.PeripheralDeviceStatus."""

    mp_type: int = 0
    """Multipart family (C enum osdp_mp_msg_type; 1 = file transfer). Only
    for NotificationType.Multipart*."""

    object_id: int = 0
    """File id for a multipart transfer. Only for NotificationType.Multipart*."""

    total: int = 0
    """Full payload length. Only for NotificationType.Multipart*."""

    offset: int = 0
    """Bytes transferred so far. Only for NotificationType.Multipart*."""

    outcome: int = 0
    """Terminal outcome. Only meaningful at NotificationType.MultipartDone."""

    status: int = 0
    """TRS session state (C enum osdp_trs_session_status_e). Only for
    NotificationType.TrsStatus."""

    @property
    def trs_status(self) -> TrsSessionStatus:
        """How the reader's TRS mode changed.

        Only for NotificationType.TrsStatus (reads status).
        """
        return TrsSessionStatus(self.status)



@dataclass(frozen=True, slots=True, kw_only=True)
class PivData:
    """Retrieve the contents of a PIV object from the PD's smartcard.

    The PD answers with an `events.PivData` carrying the reassembled object
    data; replies larger than one OSDP packet travel as multi-part messages
    transparently.

    @see osdp_cmd_pivdata

    Example:
        >>> cmd = PivData(oid=b"\\x5f\\xc1\\x02", element=7)
        >>> cmd.oid
        b'_\\xc1\\x02'
    """

    ID: ClassVar[CommandId] = CommandId.PivData

    oid: bytes = b"\x00\x00\x00"
    """3-byte PIV Object ID per NIST SP 800-73-4 Part 1."""

    element: int = 0
    """PIV element ID within the object."""

    offset: int = 0
    """Offset within the requested element."""

    def __post_init__(self) -> None:
        if len(self.oid) != 3:
            raise ValueError("oid must be exactly 3 bytes")
        check_range("element", self.element, UINT8_MAX)
        check_range("offset", self.offset, UINT8_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class GenAuth:
    """Direct a general-authenticate challenge to the PD's smartcard.

    The challenge travels as a multi-part message; the PD answers with an
    `events.GenAuth` carrying the reassembled response.

    @see osdp_cmd_auth

    Example:
        >>> cmd = GenAuth(algorithm=0xA7, key=0x9E, data=b"challenge")
        >>> len(cmd.data)
        9
    """

    ID: ClassVar[CommandId] = CommandId.GenAuth

    algorithm: int = 0
    """Selected algorithm per ISO 7816-4:2005 7.5.5."""

    key: int = 0
    """Key reference; see `algorithm`."""

    data: bytes = b""
    """Cryptographic challenge payload; non-empty."""

    def __post_init__(self) -> None:
        check_range("algorithm", self.algorithm, UINT8_MAX)
        check_range("key", self.key, UINT8_MAX)
        if not self.data:
            raise ValueError("data must not be empty")
        check_length("data", self.data, MAX_PIV_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class CrAuth:
    """Direct a challenge/response sequence to the PD's smartcard.

    Identical shape to `GenAuth`; the PD answers with an `events.CrAuth`.

    @see osdp_cmd_auth
    """

    ID: ClassVar[CommandId] = CommandId.CrAuth

    algorithm: int = 0
    """Selected algorithm per ISO 7816-4:2005 7.5.5."""

    key: int = 0
    """Key reference; see `algorithm`."""

    data: bytes = b""
    """Cryptographic challenge payload; non-empty."""

    def __post_init__(self) -> None:
        check_range("algorithm", self.algorithm, UINT8_MAX)
        check_range("key", self.key, UINT8_MAX)
        if not self.data:
            raise ValueError("data must not be empty")
        check_length("data", self.data, MAX_PIV_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class TrsStart:
    """Open a Transparent Reader Support card session (a band).

    A band is the run of commands between a TrsStart and the TrsStop that
    closes it, in submission order. The library puts the reader in
    transparent mode and reports progress as `NotificationType.TrsStatus`
    notifications. Inside a band only TRS commands are accepted, and a card
    command outside one is refused.

    @see osdp_trs_cmd_e

    Example:
        >>> TrsStart().ID.name
        'Trs'
    """

    ID: ClassVar[CommandId] = CommandId.Trs


@dataclass(frozen=True, slots=True, kw_only=True)
class TrsStop:
    """Close the card session opened by `TrsStart`.

    The library releases the card and restores the reader. To abandon a band
    and the APDUs still queued in it, flush the command queue first.

    @see osdp_trs_cmd_e
    """

    ID: ClassVar[CommandId] = CommandId.Trs


@dataclass(frozen=True, slots=True, kw_only=True)
class TrsSendApdu:
    """Send a C-APDU to the card, inside a band.

    The PD answers with an `events.TrsCardData` carrying the R-APDU, or an
    `events.TrsError`. A PD app submits that answer from within its command
    handler to reply at once, or later for it to ride out on a poll.

    @see osdp_trs_cmd
    @see osdp_trs_apdu

    Example:
        >>> cmd = TrsSendApdu(apdu=bytes([0x00, 0xA4, 0x04, 0x00]))
        >>> len(cmd.apdu)
        4
    """

    ID: ClassVar[CommandId] = CommandId.Trs

    apdu: bytes = b""
    """The C-APDU; non-empty."""

    def __post_init__(self) -> None:
        if not self.apdu:
            raise ValueError("apdu must not be empty")
        check_length("apdu", self.apdu, MAX_TRS_APDU_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class TrsPinBlock:
    """Where, and how, a reader writes the entered PIN into the C-APDU.

    @see osdp_trs_pin_entry
    """

    format: TrsPinFormat
    """How the PIN digits are encoded."""

    right_justify: bool = False
    """Right-justify the PIN within the block rather than left-justify it."""

    offset_bits: int = 0
    """Position of the block in the APDU payload, in bits."""

    size_bytes: int
    """Block size in bytes, as defined by the card scheme (8 for EMV)."""

    def __post_init__(self) -> None:
        check_range("offset_bits", self.offset_bits, UINT16_MAX)
        check_range("size_bytes", self.size_bytes, UINT8_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class TrsPinLengthField:
    """Where a reader records how many digits were entered, if anywhere.

    @see osdp_trs_pin_entry
    """

    size_bits: int = 0
    """Size of the field in bits; zero when the APDU has no such field."""

    offset_bits: int = 0
    """Position of the field in the APDU payload, in bits."""

    def __post_init__(self) -> None:
        check_range("size_bits", self.size_bits, UINT8_MAX)
        check_range("offset_bits", self.offset_bits, UINT16_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class TrsEnterPin:
    """Have the reader prompt for a PIN, insert it into `apdu` and send that
    to the card, inside a band.

    The PD answers with an `events.TrsPinComplete`. The CP waits as long as
    the person takes. Positions the wire cannot express (not byte-aligned
    past 15 bits, or a block or length field over 15) make the library refuse
    the submission.

    @see osdp_trs_pin_entry

    Example:
        >>> cmd = TrsEnterPin(
        ...     pin_block=TrsPinBlock(format=TrsPinFormat.BCD, size_bytes=8,
        ...                           offset_bits=40),
        ...     min_digits=4, max_digits=8,
        ...     complete_on=TrsPinComplete.Key | TrsPinComplete.MaxDigits,
        ...     apdu=bytes([0x00, 0x20, 0x00, 0x80, 0x08]) + bytes(8))
        >>> cmd.pin_length_field.size_bits
        0
    """

    ID: ClassVar[CommandId] = CommandId.Trs

    timeout_initial: int = 0
    """Seconds to wait for the first digit; zero for the reader's default."""

    timeout_digit: int = 0
    """Seconds to wait for each further digit."""

    pin_block: TrsPinBlock
    """Where and how the PIN goes into `apdu`."""

    pin_length_field: TrsPinLengthField = TrsPinLengthField()
    """Where the number of digits entered goes into `apdu`, if anywhere."""

    min_digits: int = 0
    """Shortest PIN accepted."""

    max_digits: int = 0
    """Longest PIN accepted."""

    complete_on: TrsPinComplete = TrsPinComplete(0)
    """What ends the entry."""

    num_messages: int = 0
    """Number of display messages."""

    language_id: int = 0
    """Display language."""

    msg_index: int = 0
    """Index of the message to display."""

    teo_prologue: bytes = b"\x00\x00\x00"
    """T=1 protocol prologue; exactly 3 bytes."""

    apdu: bytes = b""
    """The C-APDU the PIN is written into; non-empty."""

    def __post_init__(self) -> None:
        check_range("timeout_initial", self.timeout_initial, UINT8_MAX)
        check_range("timeout_digit", self.timeout_digit, UINT8_MAX)
        check_range("min_digits", self.min_digits, UINT8_MAX)
        check_range("max_digits", self.max_digits, UINT8_MAX)
        check_range("complete_on", self.complete_on, UINT32_MAX)
        check_range("num_messages", self.num_messages, UINT8_MAX)
        check_range("language_id", self.language_id, UINT16_MAX)
        check_range("msg_index", self.msg_index, UINT8_MAX)
        if len(self.teo_prologue) != 3:
            raise ValueError("teo_prologue must be exactly 3 bytes")
        if not self.apdu:
            raise ValueError("apdu must not be empty")
        check_length("apdu", self.apdu, MAX_TRS_APDU_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class TrsCardScan:
    """Ask the reader whether a card is in the field, inside a band.

    The PD answers with an `events.TrsCardPresent`, or says nothing if the
    reader cannot tell. For a scan that runs on its own between bands, see
    `ControlPanel.trs_scan_enable()`.

    @see osdp_trs_cmd_e
    """

    ID: ClassVar[CommandId] = CommandId.Trs


Command: TypeAlias = (
    Output
    | LED
    | Buzzer
    | Text
    | TDSet
    | Keyset
    | Comset
    | ComsetDone
    | Manufacturer
    | BioRead
    | BioMatch
    | PivData
    | GenAuth
    | CrAuth
    | FileTransfer
    | Status
    | Notification
    | TrsStart
    | TrsStop
    | TrsSendApdu
    | TrsEnterPin
    | TrsCardScan
)
"""Any command. Match on it to tell the members apart."""
