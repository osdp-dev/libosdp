#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Enumerations mirroring the constants in `osdp.h`.

Every member takes its value from the C extension, so these can never drift
from the library they describe. All of them derive from `int`, which means they
can be used anywhere an `int` is expected.
"""

from enum import IntEnum, IntFlag

from . import _sys

__all__ = [
    "BioFormat",
    "BioStatus",
    "BioType",
    "BuzzerControlCode",
    "Capability",
    "CardFormat",
    "CommandId",
    "CompletionStatus",
    "EventId",
    "MpMsgType",
    "MpOutcome",
    "LEDColor",
    "LibFlag",
    "LogLevel",
    "NakCode",
    "NotificationType",
    "OutputControlCode",
    "PermanentLEDControlCode",
    "StatusReportType",
    "TemporaryLEDControlCode",
    "TextControlCode",
    "TrsCardProtocol",
    "TrsCardStatus",
    "TrsCommand",
    "TrsPinComplete",
    "TrsPinFormat",
    "TrsReply",
    "TrsSessionStatus",
]


class _WireEnum(IntEnum):
    """An IntEnum that tolerates values it does not name.

    Some of these enumerations describe bytes that arrive from a peer device,
    and real devices ship values outside the spec. A strict IntEnum would raise
    ValueError while decoding, deep inside a callback invoked from C, where the
    exception surfaces as an opaque failure. Unknown values instead become
    unnamed pseudo-members that still compare equal to the underlying int.
    """

    @classmethod
    def _missing_(cls, value: object) -> "_WireEnum | None":
        if not isinstance(value, int):
            return None
        pseudo = int.__new__(cls, value)
        pseudo._name_ = f"UNKNOWN_{value:#04x}"
        pseudo._value_ = value
        return pseudo


class LibFlag(IntFlag):
    """Per-PD flags passed to `PDInfo`. @see osdp_pd_info_t"""

    EnforceSecure = _sys.FLAG_ENFORCE_SECURE
    """Only allow communication in secure channel."""

    InstallMode = _sys.FLAG_INSTALL_MODE
    """Allow a keyless PD to bring up the secure channel on SCBK-D so the CP
    can provision it. Without this flag a keyless PD refuses OSDP-SC."""

    IgnoreUnsolicited = _sys.FLAG_IGN_UNSOLICITED
    """Discard unsolicited replies from the PD."""

    EnableNotification = _sys.FLAG_ENABLE_NOTIFICATION
    """Deliver library notifications as events (CP) or commands (PD)."""

    CapturePackets = _sys.FLAG_CAPTURE_PACKETS
    """Capture packets to a PCAP file for offline analysis."""

    AllowEmptyEncryptedDataBlock = _sys.FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK
    """Tolerate peers that send an encrypted data block with no payload."""

    BioReadrMultipart = _sys.FLAG_BIOREADR_MULTIPART
    """Transfer biometric read replies larger than one packet in multiple parts."""


class LogLevel(IntEnum):
    """Verbosity of the library's logger. @see osdp_log_level_e"""

    Emergency = _sys.LOG_EMERG
    Alert = _sys.LOG_ALERT
    Critical = _sys.LOG_CRIT
    Error = _sys.LOG_ERROR
    Warning = _sys.LOG_WARNING
    Notice = _sys.LOG_NOTICE
    Info = _sys.LOG_INFO
    Debug = _sys.LOG_DEBUG


class CommandId(IntEnum):
    """Discriminator for the command types in `osdp.commands`.

    @see osdp_cmd_e
    """

    Output = _sys.CMD_OUTPUT
    LED = _sys.CMD_LED
    Buzzer = _sys.CMD_BUZZER
    Text = _sys.CMD_TEXT
    Keyset = _sys.CMD_KEYSET
    Comset = _sys.CMD_COMSET
    Manufacturer = _sys.CMD_MFG
    FileTransfer = _sys.CMD_FILE_TX
    Status = _sys.CMD_STATUS
    ComsetDone = _sys.CMD_COMSET_DONE
    Notification = _sys.CMD_NOTIFICATION
    BioRead = _sys.CMD_BIOREAD
    BioMatch = _sys.CMD_BIOMATCH
    TDSet = _sys.CMD_TDSET
    PivData = _sys.CMD_PIVDATA
    GenAuth = _sys.CMD_GENAUTH
    CrAuth = _sys.CMD_CRAUTH
    Trs = _sys.CMD_XWR
    """Transparent Reader Support; every `commands.Trs*` class carries it."""


class EventId(IntEnum):
    """Discriminator for the event types in `osdp.events`.

    @see osdp_event_type
    """

    CardRead = _sys.EVENT_CARDREAD
    KeyPress = _sys.EVENT_KEYPRESS
    ManufacturerReply = _sys.EVENT_MFGREP
    Status = _sys.EVENT_STATUS
    Notification = _sys.EVENT_NOTIFICATION
    ManufacturerStatus = _sys.EVENT_MFGSTATR
    ManufacturerError = _sys.EVENT_MFGERRR
    BioRead = _sys.EVENT_BIOREADR
    BioMatch = _sys.EVENT_BIOMATCHR
    PivData = _sys.EVENT_PIVDATAR
    GenAuth = _sys.EVENT_GENAUTHR
    CrAuth = _sys.EVENT_CRAUTHR
    Trs = _sys.EVENT_TRS
    """Transparent Reader Support; every `events.Trs*` class carries it."""


class LEDColor(_WireEnum):
    """Colors a reader LED can be set to. @see osdp_led_color_e"""

    Black = _sys.LED_COLOR_NONE
    """No color; the LED is off."""

    Red = _sys.LED_COLOR_RED
    Green = _sys.LED_COLOR_GREEN
    Amber = _sys.LED_COLOR_AMBER
    Blue = _sys.LED_COLOR_BLUE
    Magenta = _sys.LED_COLOR_MAGENTA
    Cyan = _sys.LED_COLOR_CYAN
    White = _sys.LED_COLOR_WHITE


class OutputControlCode(_WireEnum):
    """What an `Output` command does to the output line.

    @see osdp_cmd_output_control_code_e
    """

    Nop = _sys.CMD_OUTPUT_CC_NOP
    """Do nothing."""

    PermanentOff = _sys.CMD_OUTPUT_CC_PERMANENT_OFF
    """Drive the output to its inactive state permanently."""

    PermanentOn = _sys.CMD_OUTPUT_CC_PERMANENT_ON
    """Drive the output to its active state permanently."""

    PermanentOffAllowTimed = _sys.CMD_OUTPUT_CC_PERMANENT_OFF_ALLOW_TIMED
    """Go permanently inactive, but let a running timer finish first."""

    PermanentOnAllowTimed = _sys.CMD_OUTPUT_CC_PERMANENT_ON_ALLOW_TIMED
    """Go permanently active, but let a running timer finish first."""

    TemporaryOn = _sys.CMD_OUTPUT_CC_TEMPORARY_ON
    """Go active for `timer_count`, then revert."""

    TemporaryOff = _sys.CMD_OUTPUT_CC_TEMPORARY_OFF
    """Go inactive for `timer_count`, then revert."""


class TemporaryLEDControlCode(_WireEnum):
    """What the temporary block of an `LED` command does.

    @see osdp_cmd_led_temporary_control_code_e
    """

    Nop = _sys.CMD_LED_TEMPORARY_CC_NOP
    """Do nothing; equivalent to omitting the temporary block."""

    Cancel = _sys.CMD_LED_TEMPORARY_CC_CANCEL
    """Cancel any running temporary state and show the permanent state now."""

    Set = _sys.CMD_LED_TEMPORARY_CC_SET
    """Apply the temporary state and start its timer."""


class PermanentLEDControlCode(_WireEnum):
    """What the permanent block of an `LED` command does.

    @see osdp_cmd_led_permanent_control_code_e
    """

    Nop = _sys.CMD_LED_PERMANENT_CC_NOP
    """Do nothing; equivalent to omitting the permanent block."""

    Set = _sys.CMD_LED_PERMANENT_CC_SET
    """Apply the permanent state."""


class BuzzerControlCode(_WireEnum):
    """What a `Buzzer` command does. @see osdp_cmd_buzzer_control_code_e"""

    NoTone = _sys.CMD_BUZZER_CC_NO_TONE
    """Do nothing."""

    Off = _sys.CMD_BUZZER_CC_OFF
    """Silence the buzzer."""

    DefaultTone = _sys.CMD_BUZZER_CC_DEFAULT_TONE
    """Sound the reader's default tone."""


class TextControlCode(_WireEnum):
    """How a `Text` command displays its message.

    @see osdp_cmd_text_control_code_e
    """

    PermanentNoWrap = _sys.CMD_TEXT_CC_PERMANENT_NO_WRAP
    PermanentWrap = _sys.CMD_TEXT_CC_PERMANENT_WRAP
    TemporaryNoWrap = _sys.CMD_TEXT_CC_TEMPORARY_NO_WRAP
    """Display for `temp_time` seconds, then revert."""

    TemporaryWrap = _sys.CMD_TEXT_CC_TEMPORARY_WRAP
    """Display for `temp_time` seconds, then revert."""


class StatusReportType(_WireEnum):
    """Which set of status bits a status report carries.

    @see osdp_status_report_type
    """

    Input = _sys.STATUS_REPORT_INPUT
    """One entry per input (contact) line."""

    Output = _sys.STATUS_REPORT_OUTPUT
    """One entry per output line."""

    Local = _sys.STATUS_REPORT_LOCAL
    """Local status: two entries, report[0] tamper and report[1] power."""

    Reader = _sys.STATUS_REPORT_READER
    """Reader tamper status: one entry per reader (0 normal, 1 not connected, 2 tamper)."""


class NotificationType(IntEnum):
    """Kind of library notification. @see osdp_notification_type

    Each type carries its own typed fields on `osdp.events.Notification` /
    `osdp.commands.Notification`; this discriminator selects which are valid.
    """

    SecureChannelStatus = _sys.NOTIFICATION_SC_STATUS
    """Secure channel changed. Carries `active` and `scbk_d`."""

    PeripheralDeviceStatus = _sys.NOTIFICATION_PD_STATUS
    """PD reachability changed. Carries `online`."""

    MultipartStart = _sys.NOTIFICATION_MP_START
    MultipartProgress = _sys.NOTIFICATION_MP_PROGRESS
    MultipartDone = _sys.NOTIFICATION_MP_DONE

    PdId = _sys.NOTIFICATION_PD_ID
    """PD identity collected (CP mode). Carries the PD id fields (vendor_code,
    model, version, serial_number, firmware_version)."""

    TrsStatus = _sys.NOTIFICATION_TRS_STATUS
    """A TRS card session changed state. Carries `status`, a
    `TrsSessionStatus`."""


class MpMsgType(_WireEnum):
    """Which multi-part operation a notification or cancel refers to.

    Reported as ``MultipartNotification.mp_type`` and passed back to
    `ControlPanel.cancel()`. @see osdp_mp_msg_type
    """

    FileTransfer = _sys.MP_MSG_FILE_TRANSFER
    PivData = _sys.MP_MSG_PIV
    GenAuth = _sys.MP_MSG_GENAUTH
    CrAuth = _sys.MP_MSG_CRAUTH
    BioRead = _sys.MP_MSG_BIOREAD


class MpOutcome(_WireEnum):
    """How a multipart transfer ended, as reported by MultipartDone.

    ``Ok`` and ``Aborted`` apply to any transfer; the remaining values are
    file-transfer specific. @see osdp_mp_outcome
    """

    Ok = _sys.MP_OUTCOME_OK
    OkRebooting = _sys.MP_OUTCOME_OK_REBOOTING
    """Transfer succeeded and the PD is rebooting to apply it."""

    Aborted = _sys.MP_OUTCOME_ABORTED
    Unrecognized = _sys.MP_OUTCOME_UNRECOGNIZED
    Invalid = _sys.MP_OUTCOME_INVALID


class CompletionStatus(IntEnum):
    """Fate of a submitted command or event. @see osdp_completion_status

    Every accepted command or event is reported exactly once, including those
    that were flushed or torn down before they reached the wire.
    """

    Ok = _sys.COMPLETION_OK
    """Delivered to the peer and acknowledged."""

    Failed = _sys.COMPLETION_FAILED
    """Sent but the peer rejected it, or it could not be sent."""

    Flushed = _sys.COMPLETION_FLUSHED
    """Dropped from the queue before being sent."""

    Aborted = _sys.COMPLETION_ABORTED
    """The context was torn down while it was still queued."""


class BioType(_WireEnum):
    """Which biometric a reader should capture or match.

    @see osdp_biometric_type_e
    """

    NotSpecified = _sys.BIO_TYPE_NOT_SPECIFIED
    RightThumbPrint = _sys.BIO_TYPE_RIGHT_THUMB_PRINT
    RightIndexFingerPrint = _sys.BIO_TYPE_RIGHT_INDEX_FINGER_PRINT
    RightMiddleFingerPrint = _sys.BIO_TYPE_RIGHT_MIDDLE_FINGER_PRINT
    RightRingFingerPrint = _sys.BIO_TYPE_RIGHT_RING_FINGER_PRINT
    RightLittleFingerPrint = _sys.BIO_TYPE_RIGHT_LITTLE_FINGER_PRINT
    LeftThumbPrint = _sys.BIO_TYPE_LEFT_THUMB_PRINT
    LeftIndexFingerPrint = _sys.BIO_TYPE_LEFT_INDEX_FINGER_PRINT
    LeftMiddleFingerPrint = _sys.BIO_TYPE_LEFT_MIDDLE_FINGER_PRINT
    LeftRingFingerPrint = _sys.BIO_TYPE_LEFT_RING_FINGER_PRINT
    LeftLittleFingerPrint = _sys.BIO_TYPE_LEFT_LITTLE_FINGER_PRINT
    RightIrisScan = _sys.BIO_TYPE_RIGHT_IRIS_SCAN
    RightRetinaScan = _sys.BIO_TYPE_RIGHT_RETINA_SCAN
    LeftIrisScan = _sys.BIO_TYPE_LEFT_IRIS_SCAN
    LeftRetinaScan = _sys.BIO_TYPE_LEFT_RETINA_SCAN
    FullFaceImage = _sys.BIO_TYPE_FULL_FACE_IMAGE
    RightHandGeometry = _sys.BIO_TYPE_RIGHT_HAND_GEOMETRY
    LeftHandGeometry = _sys.BIO_TYPE_LEFT_HAND_GEOMETRY


class BioFormat(_WireEnum):
    """Encoding of a biometric template. @see osdp_biometric_format_e"""

    NotSpecified = _sys.BIO_FMT_NOT_SPECIFIED
    RawPGM = _sys.BIO_FMT_RAW_PGM
    AnsiIncits378 = _sys.BIO_FMT_ANSI_INCITS_378


class BioStatus(_WireEnum):
    """Outcome of a biometric read or match. @see osdp_biometric_status_e

    The values are sparse; UnknownError is 0xFF.
    """

    Success = _sys.BIO_STATUS_SUCCESS
    """The scan or match completed; the reply's other fields are valid."""

    Timeout = _sys.BIO_STATUS_TIMEOUT
    """The reader gave up waiting for the subject."""

    UnknownError = _sys.BIO_STATUS_UNKNOWN_ERROR


class NakCode(IntEnum):
    """Reasons a PD command handler may reject a command.

    Raise `osdp.NakError` with one of these from a command handler; the other
    NAK codes in the spec are produced by the library itself.

    @see osdp_pd_nak_code_e
    """

    BioType = _sys.PD_NAK_BIO_TYPE
    """The requested biometric type is not supported."""

    BioFormat = _sys.PD_NAK_BIO_FMT
    """The requested biometric format is not supported."""

    Record = _sys.PD_NAK_RECORD
    """The command could not be processed. The catch-all rejection."""


class CardFormat(_WireEnum):
    """Encoding of the data in a `CardRead` event.

    @see osdp_event_cardread_format_e
    """

    Unspecified = _sys.CARD_FMT_RAW_UNSPECIFIED
    """Raw bits in an unspecified format; `bits` is meaningful."""

    Wiegand = _sys.CARD_FMT_RAW_WIEGAND
    """Raw Wiegand bits; `bits` is meaningful."""

    ASCII = _sys.CARD_FMT_ASCII
    """Card data as ASCII bytes (osdp_FMT). Not supported: a PD refuses to
    submit it and a CP ignores osdp_FMT replies."""


class Capability(IntEnum):
    """Things a PD can tell a CP it supports.

    @see osdp_pd_cap_function_code_e
    """

    Unused = _sys.CAP_UNUSED
    ContactStatusMonitoring = _sys.CAP_CONTACT_STATUS_MONITORING
    OutputControl = _sys.CAP_OUTPUT_CONTROL
    CardDataFormat = _sys.CAP_CARD_DATA_FORMAT
    LEDControl = _sys.CAP_READER_LED_CONTROL
    AudibleControl = _sys.CAP_READER_AUDIBLE_OUTPUT
    TextOutput = _sys.CAP_READER_TEXT_OUTPUT
    TimeKeeping = _sys.CAP_TIME_KEEPING
    CheckCharacter = _sys.CAP_CHECK_CHARACTER_SUPPORT
    CommunicationSecurity = _sys.CAP_COMMUNICATION_SECURITY
    ReceiveBufferSize = _sys.CAP_RECEIVE_BUFFERSIZE
    CombinedMessageSize = _sys.CAP_LARGEST_COMBINED_MESSAGE_SIZE
    SmartCard = _sys.CAP_SMART_CARD_SUPPORT
    Readers = _sys.CAP_READERS
    Biometrics = _sys.CAP_BIOMETRICS
    SecurePinEntry = _sys.CAP_SECURE_PIN_ENTRY
    OSDPVersion = _sys.CAP_OSDP_VERSION


class TrsCommand(IntEnum):
    """Which Transparent Reader Support command a `commands.Trs*` class is.

    @see osdp_trs_cmd_e
    """

    Start = _sys.TRS_CMD_START
    """Open a card session."""

    Stop = _sys.TRS_CMD_STOP
    """Close the card session opened by Start."""

    SendApdu = _sys.TRS_CMD_SEND_APDU
    """Send a C-APDU to the card."""

    EnterPin = _sys.TRS_CMD_ENTER_PIN
    """Have the reader collect a PIN and send it to the card."""

    CardScan = _sys.TRS_CMD_CARD_SCAN
    """Ask the reader whether a card is in the field."""


class TrsReply(IntEnum):
    """Which Transparent Reader Support reply an `events.Trs*` class is.

    @see osdp_trs_reply_e
    """

    CardInfo = _sys.TRS_REPLY_CARD_INFO
    CardPresent = _sys.TRS_REPLY_CARD_PRESENT
    CardData = _sys.TRS_REPLY_CARD_DATA
    PinComplete = _sys.TRS_REPLY_PIN_COMPLETE
    Error = _sys.TRS_REPLY_ERROR


class TrsSessionStatus(IntEnum):
    """Life-cycle of a TRS card session, carried by a TrsStatus notification.

    A PD is told only Opened, Closed and CardReleased; the rest are the CP's.

    @see osdp_trs_session_status_e
    """

    Opened = _sys.TRS_SESSION_OPENED
    """The reader is in transparent mode; APDUs flow."""

    Closed = _sys.TRS_SESSION_CLOSED
    """The session ended, or (on a PD) the CP took the reader out of
    transparent mode or restarted the link."""

    Failed = _sys.TRS_SESSION_FAILED
    """The PD refused transparent mode, or a link error cut the session short."""

    ScanSuspended = _sys.TRS_SCAN_SUSPENDED
    """The reader refused a presence-scan probe; probing backs off and goes on."""

    ScanNoDeparture = _sys.TRS_SCAN_NO_DEPARTURE
    """The reader cannot answer a card scan, so card removal will not be
    reported."""

    CardReleased = _sys.TRS_CARD_RELEASED
    """PD only: the CP ended its transaction with the card. Disconnect it."""


class TrsPinFormat(_WireEnum):
    """Encoding of the PIN digits a reader inserts into the C-APDU.

    @see osdp_trs_pin_format_e
    """

    Binary = _sys.TRS_PIN_FORMAT_BINARY
    BCD = _sys.TRS_PIN_FORMAT_BCD
    ASCII = _sys.TRS_PIN_FORMAT_ASCII


class TrsPinComplete(IntFlag):
    """Conditions that end PIN entry. @see osdp_trs_pin_complete_e"""

    MaxDigits = _sys.TRS_PIN_COMPLETE_ON_MAX_DIGITS
    """The maximum number of digits has been entered."""

    Key = _sys.TRS_PIN_COMPLETE_ON_KEY
    """The validation (enter) key was pressed."""

    Timeout = _sys.TRS_PIN_COMPLETE_ON_TIMEOUT
    """Entry timed out."""


class TrsCardProtocol(_WireEnum):
    """How a card in the field talks. @see osdp_trs_card_protocol_e"""

    Contact = _sys.TRS_CARD_PROTOCOL_CONTACT
    """ISO 7816 contact (T=0/T=1)."""

    Contactless = _sys.TRS_CARD_PROTOCOL_CONTACTLESS
    """ISO 14443 A/B contactless."""


class TrsCardStatus(_WireEnum):
    """Smart-card presence reported by a reader. @see osdp_trs_card_status_e

    Test for a card with the named values, not for "not NotPresent": Unknown
    is neither.
    """

    Unknown = _sys.TRS_CARD_STATUS_UNKNOWN
    """No presence has been reported."""

    NotPresent = _sys.TRS_CARD_NOT_PRESENT
    Present = _sys.TRS_CARD_PRESENT
    """A card is present; the reader does not say on which interface."""

    PresentContactless = _sys.TRS_CARD_PRESENT_CONTACTLESS
    PresentContact = _sys.TRS_CARD_PRESENT_CONTACT
