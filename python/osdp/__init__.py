#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Python bindings for LibOSDP.

Commands and events are typed dataclasses, grouped into two modules so that the
names which appear on both sides (Status, Notification, BioRead) stay distinct:

    from osdp import ControlPanel, PDInfo, commands, events

    cp = ControlPanel([PDInfo(address=101, channel=my_channel)])
    cp.start()
    cp.submit_command(101, commands.Buzzer(rep_count=3))

    match cp.get_event(101):
        case events.CardRead(data=data):
            admit(data)
        case events.KeyPress(data=keys):
            collect(keys)
"""

from . import commands, enums, events
from .channel import Channel
from .commands import Command
from .control_panel import CommandCompletionHandler, ControlPanel, EventHandler
from .enums import (
    BioFormat,
    BioStatus,
    BioType,
    BuzzerControlCode,
    Capability,
    CardFormat,
    CommandId,
    CompletionStatus,
    EventId,
    MpMsgType,
    MpOutcome,
    LEDColor,
    LibFlag,
    LogLevel,
    NakCode,
    NotificationType,
    OutputControlCode,
    PermanentLEDControlCode,
    StatusReportType,
    TemporaryLEDControlCode,
    TextControlCode,
    TrsCardProtocol,
    TrsCardStatus,
    TrsCommand,
    TrsPinComplete,
    TrsPinFormat,
    TrsReply,
    TrsSessionStatus,
)
from .errors import MarshalError, NakError, OSDPError
from .events import Event
from .key_store import KeyStore
from .peripheral_device import (
    CommandHandler,
    EventCompletionHandler,
    PeripheralDevice,
)
from .types import (
    FileOps,
    FileTxStatus,
    Metrics,
    PDCapabilities,
    PDCapability,
    PDInfo,
    PdId,
)

__author__ = "Siddharth Chandrasekaran <sidcha@osdp.dev>"
__copyright__ = "Copyright 2021-2026 Siddharth Chandrasekaran"
__license__ = "Apache License, Version 2.0 (Apache-2.0)"

__all__ = [
    # modules
    "commands",
    "enums",
    "events",
    # core
    "Channel",
    "ControlPanel",
    "PeripheralDevice",
    "KeyStore",
    # payload unions
    "Command",
    "Event",
    # handler types
    "CommandCompletionHandler",
    "CommandHandler",
    "EventCompletionHandler",
    "EventHandler",
    # descriptors
    "FileOps",
    "FileTxStatus",
    "Metrics",
    "PDCapabilities",
    "PDCapability",
    "PDInfo",
    "PdId",
    # errors
    "MarshalError",
    "NakError",
    "OSDPError",
    # enums
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
