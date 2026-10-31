#!/usr/bin/env python3
#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
#  A minimal OSDP Control Panel. See the Python docs for a full walkthrough:
#  https://doc.osdp.dev/python/control-panel
#

import argparse

from osdp import (
    Command,
    CompletionStatus,
    ControlPanel,
    KeyStore,
    LEDColor,
    PDInfo,
    SerialChannel,
    commands,
    events,
)


parser = argparse.ArgumentParser(prog='cp_app', description="LibOSDP CP APP Example")
parser.add_argument("device", type=str, metavar="PATH", help="Path to serial device")
parser.add_argument("--baudrate", type=int, metavar="N", default=115200, help="Serial port's baud rate (default: 115200)")
parser.add_argument("--log-level", type=int, metavar="LEVEL", default=6, help="LibOSDP log level; can be 0-7 (default: 6)")
args = parser.parse_args()

## Describe the PD. The CP owns the key; pd_app runs in install mode so
## this key gets provisioned into it over SCBK-D on first contact.
channel = SerialChannel(args.device, args.baudrate)
pd_info = [
    PDInfo(101, channel, scbk=KeyStore.gen_key()),
]


def command_completion_handler(
    address: int, command: Command, status: CompletionStatus
) -> None:
    """Called once for every command that submit_command() accepted.

    This is how a command's outcome comes back: Ok once the PD answered it,
    Failed if it could not be delivered, Flushed if flush_commands() dropped
    it, or Aborted if teardown() ran while it was still queued.
    """
    if status is not CompletionStatus.Ok:
        print(f"PD-{address}: {type(command).__name__} -> {status.name}")


## Create a CP device and kick-off the handler thread
cp = ControlPanel(
    pd_info,
    log_level=args.log_level,
    command_completion_handler=command_completion_handler,
)
cp.start()
cp.sc_wait_all()

## Flash the reader LED red for one second. Durations are in units of 100ms;
## on_count and off_count cannot both be zero. Every field has a default, so
## only what differs from it needs spelling out.
led_cmd = commands.LED(
    temporary=commands.TemporaryLEDParams(
        on_color=LEDColor.Red,
        on_count=10,
        off_count=10,
        timer_count=10,
    ),
)

count = 0  # loop counter
while True:
    ## Send LED command to PD-0. A False return means it was never queued (the
    ## PD is offline, say) and no completion will be reported for it.
    if not cp.submit_command(pd_info[0].address, led_cmd):
        print("PD-0 is not accepting commands")

    ## Check if we have an event from PD. Each event is a distinct type, so
    ## match tells them apart and gives us the right fields for free.
    event = cp.get_event(pd_info[0].address, timeout=2)
    match event:
        case events.CardRead(data=data, format=fmt):
            print(f"PD-0 read a {fmt.name} card: {data.hex()}")
        case events.KeyPress(data=keys):
            print(f"PD-0 keypad: {keys!r}")
        case events.Status(type=report_type, report=report):
            print(f"PD-0 {report_type.name} status: {list(report)}")
        case None:
            pass
        case _:
            print(f"PD-0 sent event {event}")

    if count >= 5:
        break
    count += 1

cp.teardown()
