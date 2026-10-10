#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Type stubs for the private C extension backing this package.

Not a public API: the dicts these methods exchange are an implementation detail
shared with osdp._marshal, which is the only module that should speak to them.
"""

from typing import Any, Callable, Final, Protocol

class _Channel(Protocol):
    def read(self, max_bytes: int, /) -> bytes: ...
    def write(self, buf: bytes, /) -> int: ...
    def flush(self) -> None: ...

def set_loglevel(log_level: int, /) -> None: ...

class _OSDPBaseClass:
    def get_version(self) -> str: ...
    def get_source_info(self) -> str: ...
    def register_file_ops(
        self, pd: int, fops: dict[str, Callable[..., Any]], /
    ) -> bool: ...
    def get_file_tx_status(self, pd: int, /) -> dict[str, int] | None: ...
    def get_metrics(self, pd: int, /) -> dict[str, int] | None: ...

class ControlPanel(_OSDPBaseClass):
    def __init__(self, pd_info: list[dict[str, Any]], /) -> None: ...
    def refresh(self) -> None: ...
    def set_event_callback(
        self, cb: Callable[[int, dict[str, Any]], int], /
    ) -> None: ...
    def set_command_completion_callback(
        self, cb: Callable[[int, dict[str, Any], int], None], /
    ) -> None: ...
    def submit_command(self, pd: int, command: dict[str, Any], /) -> bool: ...
    def flush_commands(self, pd: int, /) -> int: ...
    def cancel(self, pd: int, what: int, /) -> bool: ...
    def status(self) -> int: ...
    def sc_status(self) -> int: ...
    def set_flag(self, pd: int, flag: int, /) -> bool: ...
    def clear_flag(self, pd: int, flag: int, /) -> bool: ...
    def get_pd_id(self, pd: int, /) -> dict[str, int]: ...
    def check_capability(
        self, pd: int, function_code: int, /
    ) -> tuple[int, int]: ...
    def enable_pd(self, pd: int, /) -> bool: ...
    def disable_pd(self, pd: int, /) -> bool: ...
    def is_pd_enabled(self, pd: int, /) -> bool: ...
    def trs_scan_enable(
        self,
        pd: int,
        mode0_dwell_ms: int = ...,
        mode1_dwell_ms: int = ...,
        hold_ms: int = ...,
        /,
    ) -> bool: ...
    def trs_scan_disable(self, pd: int, /) -> bool: ...
    def trs_get_max_apdu_len(self, pd: int, /) -> int: ...

class PeripheralDevice(_OSDPBaseClass):
    def __init__(
        self,
        pd_info: dict[str, Any],
        /,
        *,
        capabilities: list[dict[str, int]] = ...,
    ) -> None: ...
    def refresh(self) -> None: ...
    def set_command_callback(
        self,
        cb: Callable[[dict[str, Any]], tuple[int, dict[str, Any] | None]],
        /,
    ) -> None: ...
    def set_event_completion_callback(
        self, cb: Callable[[dict[str, Any], int], None], /
    ) -> None: ...
    def submit_event(self, event: dict[str, Any], /) -> bool: ...
    def flush_events(self) -> int: ...
    def is_online(self) -> bool: ...
    def is_sc_active(self) -> bool: ...

# Constants, mirroring pyosdp_add_module_constants() in module.c.
BIO_FMT_ANSI_INCITS_378: Final[int]
BIO_FMT_NOT_SPECIFIED: Final[int]
BIO_FMT_RAW_PGM: Final[int]
BIO_STATUS_SUCCESS: Final[int]
BIO_STATUS_TIMEOUT: Final[int]
BIO_STATUS_UNKNOWN_ERROR: Final[int]
BIO_TYPE_FULL_FACE_IMAGE: Final[int]
BIO_TYPE_LEFT_HAND_GEOMETRY: Final[int]
BIO_TYPE_LEFT_INDEX_FINGER_PRINT: Final[int]
BIO_TYPE_LEFT_IRIS_SCAN: Final[int]
BIO_TYPE_LEFT_LITTLE_FINGER_PRINT: Final[int]
BIO_TYPE_LEFT_MIDDLE_FINGER_PRINT: Final[int]
BIO_TYPE_LEFT_RETINA_SCAN: Final[int]
BIO_TYPE_LEFT_RING_FINGER_PRINT: Final[int]
BIO_TYPE_LEFT_THUMB_PRINT: Final[int]
BIO_TYPE_NOT_SPECIFIED: Final[int]
BIO_TYPE_RIGHT_HAND_GEOMETRY: Final[int]
BIO_TYPE_RIGHT_INDEX_FINGER_PRINT: Final[int]
BIO_TYPE_RIGHT_IRIS_SCAN: Final[int]
BIO_TYPE_RIGHT_LITTLE_FINGER_PRINT: Final[int]
BIO_TYPE_RIGHT_MIDDLE_FINGER_PRINT: Final[int]
BIO_TYPE_RIGHT_RETINA_SCAN: Final[int]
BIO_TYPE_RIGHT_RING_FINGER_PRINT: Final[int]
BIO_TYPE_RIGHT_THUMB_PRINT: Final[int]
CAP_BIOMETRICS: Final[int]
CAP_CARD_DATA_FORMAT: Final[int]
CAP_CHECK_CHARACTER_SUPPORT: Final[int]
CAP_COMMUNICATION_SECURITY: Final[int]
CAP_CONTACT_STATUS_MONITORING: Final[int]
CAP_LARGEST_COMBINED_MESSAGE_SIZE: Final[int]
CAP_OSDP_VERSION: Final[int]
CAP_OUTPUT_CONTROL: Final[int]
CAP_READER_AUDIBLE_OUTPUT: Final[int]
CAP_READER_LED_CONTROL: Final[int]
CAP_READERS: Final[int]
CAP_READER_TEXT_OUTPUT: Final[int]
CAP_RECEIVE_BUFFERSIZE: Final[int]
CAP_SECURE_PIN_ENTRY: Final[int]
CAP_SMART_CARD_SUPPORT: Final[int]
CAP_TIME_KEEPING: Final[int]
CAP_UNUSED: Final[int]
CARD_FMT_ASCII: Final[int]
CARD_FMT_RAW_UNSPECIFIED: Final[int]
CARD_FMT_RAW_WIEGAND: Final[int]
CMD_BIOMATCH: Final[int]
CMD_PIVDATA: Final[int]
CMD_GENAUTH: Final[int]
CMD_CRAUTH: Final[int]
CMD_BIOMATCH_MAX_TEMPLATE_LEN: Final[int]
PIV_DATA_MAX_LEN: Final[int]
CMD_BIOREAD: Final[int]
CMD_BUZZER: Final[int]
CMD_BUZZER_CC_DEFAULT_TONE: Final[int]
CMD_BUZZER_CC_NO_TONE: Final[int]
CMD_BUZZER_CC_OFF: Final[int]
CMD_COMSET: Final[int]
CMD_COMSET_DONE: Final[int]
CMD_FILE_TX: Final[int]
MP_MSG_FILE_TRANSFER: Final[int]
MP_MSG_PIV: Final[int]
MP_MSG_GENAUTH: Final[int]
MP_MSG_CRAUTH: Final[int]
MP_MSG_BIOREAD: Final[int]
CMD_KEYSET: Final[int]
CMD_KEYSET_KEY_MAX_LEN: Final[int]
CMD_LED: Final[int]
CMD_LED_PERMANENT_CC_NOP: Final[int]
CMD_LED_PERMANENT_CC_SET: Final[int]
CMD_LED_TEMPORARY_CC_CANCEL: Final[int]
CMD_LED_TEMPORARY_CC_NOP: Final[int]
CMD_LED_TEMPORARY_CC_SET: Final[int]
CMD_MFG: Final[int]
CMD_MFG_MAX_DATALEN: Final[int]
CMD_NOTIFICATION: Final[int]
CMD_OUTPUT: Final[int]
CMD_OUTPUT_CC_NOP: Final[int]
CMD_OUTPUT_CC_PERMANENT_OFF: Final[int]
CMD_OUTPUT_CC_PERMANENT_OFF_ALLOW_TIMED: Final[int]
CMD_OUTPUT_CC_PERMANENT_ON: Final[int]
CMD_OUTPUT_CC_PERMANENT_ON_ALLOW_TIMED: Final[int]
CMD_OUTPUT_CC_TEMPORARY_OFF: Final[int]
CMD_OUTPUT_CC_TEMPORARY_ON: Final[int]
CMD_STATUS: Final[int]
CMD_TDSET: Final[int]
CMD_TEXT: Final[int]
CMD_TEXT_CC_PERMANENT_NO_WRAP: Final[int]
CMD_TEXT_CC_PERMANENT_WRAP: Final[int]
CMD_TEXT_CC_TEMPORARY_NO_WRAP: Final[int]
CMD_TEXT_CC_TEMPORARY_WRAP: Final[int]
CMD_TEXT_MAX_LEN: Final[int]
CMD_XWR: Final[int]
COMPLETION_ABORTED: Final[int]
COMPLETION_FAILED: Final[int]
COMPLETION_FLUSHED: Final[int]
COMPLETION_OK: Final[int]
EVENT_BIOMATCHR: Final[int]
EVENT_PIVDATAR: Final[int]
EVENT_GENAUTHR: Final[int]
EVENT_CRAUTHR: Final[int]
EVENT_BIOREADR: Final[int]
EVENT_BIOREADR_MAX_TEMPLATE_LEN: Final[int]
EVENT_CARDREAD: Final[int]
EVENT_CARDREAD_MAX_DATALEN: Final[int]
EVENT_KEYPRESS: Final[int]
EVENT_KEYPRESS_MAX_DATALEN: Final[int]
EVENT_MFGERRR: Final[int]
EVENT_MFGREP: Final[int]
EVENT_MFGREP_MAX_DATALEN: Final[int]
EVENT_MFGSTAT_MAX_DATALEN: Final[int]
EVENT_MFGSTATR: Final[int]
EVENT_NOTIFICATION: Final[int]
EVENT_STATUS: Final[int]
EVENT_TRS: Final[int]
MP_OUTCOME_ABORTED: Final[int]
MP_OUTCOME_INVALID: Final[int]
MP_OUTCOME_OK: Final[int]
MP_OUTCOME_OK_REBOOTING: Final[int]
MP_OUTCOME_UNRECOGNIZED: Final[int]
FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK: Final[int]
FLAG_BIOREADR_MULTIPART: Final[int]
FLAG_CAPTURE_PACKETS: Final[int]
FLAG_ENABLE_NOTIFICATION: Final[int]
FLAG_ENFORCE_SECURE: Final[int]
FLAG_IGN_UNSOLICITED: Final[int]
FLAG_INSTALL_MODE: Final[int]
LED_COLOR_AMBER: Final[int]
LED_COLOR_BLUE: Final[int]
LED_COLOR_CYAN: Final[int]
LED_COLOR_GREEN: Final[int]
LED_COLOR_MAGENTA: Final[int]
LED_COLOR_NONE: Final[int]
LED_COLOR_RED: Final[int]
LED_COLOR_WHITE: Final[int]
LOG_ALERT: Final[int]
LOG_CRIT: Final[int]
LOG_DEBUG: Final[int]
LOG_EMERG: Final[int]
LOG_ERROR: Final[int]
LOG_INFO: Final[int]
LOG_MAX_LEVEL: Final[int]
LOG_NOTICE: Final[int]
LOG_WARNING: Final[int]
NOTIFICATION_MP_START: Final[int]
NOTIFICATION_MP_PROGRESS: Final[int]
NOTIFICATION_MP_DONE: Final[int]
NOTIFICATION_PD_ID: Final[int]
NOTIFICATION_PD_STATUS: Final[int]
NOTIFICATION_SC_STATUS: Final[int]
NOTIFICATION_TRS_STATUS: Final[int]
PD_NAK_BIO_FMT: Final[int]
PD_NAK_BIO_TYPE: Final[int]
PD_NAK_RECORD: Final[int]
STATUS_REPORT_INPUT: Final[int]
STATUS_REPORT_LOCAL: Final[int]
STATUS_REPORT_MAX_LEN: Final[int]
STATUS_REPORT_OUTPUT: Final[int]
STATUS_REPORT_READER: Final[int]
TRS_APDU_MAX_LEN: Final[int]
TRS_CARD_NOT_PRESENT: Final[int]
TRS_CARD_PRESENT: Final[int]
TRS_CARD_PRESENT_CONTACT: Final[int]
TRS_CARD_PRESENT_CONTACTLESS: Final[int]
TRS_CARD_PROTOCOL_CONTACT: Final[int]
TRS_CARD_PROTOCOL_CONTACTLESS: Final[int]
TRS_CARD_RELEASED: Final[int]
TRS_CARD_STATUS_UNKNOWN: Final[int]
TRS_CMD_CARD_SCAN: Final[int]
TRS_CMD_ENTER_PIN: Final[int]
TRS_CMD_SEND_APDU: Final[int]
TRS_CMD_START: Final[int]
TRS_CMD_STOP: Final[int]
TRS_CSN_MAX_LEN: Final[int]
TRS_PIN_COMPLETE_ON_KEY: Final[int]
TRS_PIN_COMPLETE_ON_MAX_DIGITS: Final[int]
TRS_PIN_COMPLETE_ON_TIMEOUT: Final[int]
TRS_PIN_FORMAT_ASCII: Final[int]
TRS_PIN_FORMAT_BCD: Final[int]
TRS_PIN_FORMAT_BINARY: Final[int]
TRS_PROTOCOL_DATA_MAX_LEN: Final[int]
TRS_REPLY_CARD_DATA: Final[int]
TRS_REPLY_CARD_INFO: Final[int]
TRS_REPLY_CARD_PRESENT: Final[int]
TRS_REPLY_ERROR: Final[int]
TRS_REPLY_PIN_COMPLETE: Final[int]
TRS_SCAN_NO_DEPARTURE: Final[int]
TRS_SCAN_SUSPENDED: Final[int]
TRS_SESSION_CLOSED: Final[int]
TRS_SESSION_FAILED: Final[int]
TRS_SESSION_OPENED: Final[int]
