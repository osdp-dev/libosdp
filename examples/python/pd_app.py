#!/usr/bin/env python3
#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import signal
import argparse
import serial
from osdp import *

exit_event = 0
def signal_handler(sig, frame):
    global exit_event
    print('Received SIGINT, quitting...')
    exit_event = 1

signal.signal(signal.SIGINT, signal_handler)

class SerialChannel(Channel):
    def __init__(self, device: str, speed: int):
        self.dev = serial.Serial(device, speed, timeout=0)

    def read(self, max: int):
        return self.dev.read(max)

    def write(self, data: bytes):
        return self.dev.write(data)

    def flush(self):
        self.dev.flush()

    def __del__(self):
        self.dev.close()

parser = argparse.ArgumentParser(prog = 'pd_app', description = "LibOSDP PD APP Example")
parser.add_argument("device", type = str, metavar = "PATH", help = "Path to serial device")
parser.add_argument("--baudrate", type = int, metavar = "N", default = 115200, help = "Serial port's baud rate (default: 115200)")
parser.add_argument("--log-level", type = int, metavar = "N", default = 6, help = "LibOSDP log level; can be 0-7 (default: 6)")
args = parser.parse_args()

## Describe the PD (setting scbk=None puts the PD in install mode)
channel = SerialChannel(args.device, args.baudrate)
pd_info = PDInfo(101, channel, scbk=None)

## Indicate the PD's capabilities to LibOSDP.
pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 2, 1),
    (Capability.AudibleControl, 1, 1),
    (Capability.TextOutput, 1, 1),
])

## Create a PD device and kick-off the handler thread
pd = PeripheralDevice(pd_info, pd_cap, log_level=args.log_level)
pd.start()
pd.sc_wait(timeout=-1)

## create a card read event to be used later
card_event = {
    'event': Event.CardRead,
    'reader_no': 1,
    'direction': 1,
    'format': CardFormat.ASCII,
    'data': bytes([9,1,9,2,6,3,1,7,7,0]),
}

while not exit_event:
    ## Check if we have any commands from the CP
    cmd = pd.get_command(timeout=5)
    if cmd:
        print(f"PD: Received command: {cmd}")

        ## Send a card read event to CP
        pd.submit_event(card_event)

pd.teardown()
