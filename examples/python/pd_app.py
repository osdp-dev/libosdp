#!/usr/bin/env python3
#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
#  A minimal OSDP Peripheral Device. See the Python docs for a full walkthrough:
#  https://doc.osdp.dev/python/peripheral-device
#

import argparse
import signal

from osdp import (
    Capability,
    Command,
    CompletionStatus,
    Event,
    LibFlag,
    NakCode,
    NakError,
    PDCapabilities,
    PDInfo,
    PeripheralDevice,
    SerialChannel,
    StatusReportType,
    commands,
    events,
)

exit_event = 0


def signal_handler(sig, frame):
    global exit_event
    print('Received SIGINT, quitting...')
    exit_event = 1


signal.signal(signal.SIGINT, signal_handler)


parser = argparse.ArgumentParser(prog='pd_app', description="LibOSDP PD APP Example")
parser.add_argument("device", type=str, metavar="PATH", help="Path to serial device")
parser.add_argument("--baudrate", type=int, metavar="N", default=115200, help="Serial port's baud rate (default: 115200)")
parser.add_argument("--log-level", type=int, metavar="N", default=6, help="LibOSDP log level; can be 0-7 (default: 6)")
args = parser.parse_args()

## Describe the PD. Without an scbk the secure channel is disabled, so this
## example asks for install mode to let cp_app provision the real key.
channel = SerialChannel(args.device, args.baudrate)
pd_info = PDInfo(101, channel, scbk=None, flags=[LibFlag.InstallMode])

## Indicate the PD's capabilities to LibOSDP.
pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 2, 1),
    (Capability.AudibleControl, 1, 1),
    (Capability.TextOutput, 1, 1),
    (Capability.ContactStatusMonitoring, 1, 4),
])

## Pretend these are the PD's input contacts.
INPUT_CONTACTS = bytes([0, 1, 0, 1])


def command_handler(cmd: Command) -> Command | None:
    """Handle a command from the CP.

    Return None to accept it, return a command to answer it inline, or raise
    NakError to decline it.
    """
    print(f"PD: Received command: {cmd}")

    match cmd:
        case commands.Status(type=StatusReportType.Input):
            ## Answer the query inline with our contact states.
            return commands.Status(
                type=StatusReportType.Input, report=INPUT_CONTACTS
            )
        case commands.LED(permanent=permanent) if permanent is not None:
            print(f"PD: LED set to {permanent.on_color.name}")
        case commands.Buzzer(rep_count=rep_count):
            print(f"PD: buzz {rep_count} times")
        case commands.BioRead() | commands.BioMatch():
            ## We have no biometric reader; decline rather than pretend.
            raise NakError(NakCode.BioType)

    return None


def event_completion_handler(event: Event, status: CompletionStatus) -> None:
    """Called once for every event that submit_event() accepted.

    Ok means the reply carrying the event was handed to the transport -- OSDP
    gives the PD no delivery receipt from the CP. Failed means it never got
    out; Flushed and Aborted mean flush_events() or teardown() dropped it
    while it was still queued.
    """
    if status is not CompletionStatus.Ok:
        print(f"PD: {type(event).__name__} -> {status.name}")


## Create a PD device and kick-off the handler thread
pd = PeripheralDevice(
    pd_info,
    pd_cap,
    log_level=args.log_level,
    command_handler=command_handler,
    event_completion_handler=event_completion_handler,
)
pd.start()
pd.sc_wait(timeout=-1)

## A card read to report to the CP. Every field has a default, so only spell
## out what differs. For the raw formats the card is a bit string, so its
## length is in bits: this is a 26-bit Wiegand card, which needs four bytes to
## carry its 26 bits.
card_event = events.CardRead(
    data=bytes([0x01, 0x02, 0x03, 0x40]),
    bits=26,
)

while not exit_event:
    ## Commands are dispatched to command_handler above; this loop just reports
    ## a card read every few seconds.
    if pd.get_command(timeout=5) is not None:
        ## A False return means it was never queued and no completion will be
        ## reported for it.
        if not pd.submit_event(card_event):
            print("PD: could not queue the card read")

pd.teardown()
