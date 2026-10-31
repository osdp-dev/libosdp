---
lastUpdated: 2026-10-29
description: "Get started with the LibOSDP Python bindings — install the package and build a minimal OSDP control panel or peripheral device in a few lines of Python."
---
# Python Getting Started

The `libosdp` Python package wraps the LibOSDP C library so you can build and
test OSDP devices without writing any C. Commands and events are typed
dataclasses, so your editor completes the fields, a type checker catches a wrong
one before you run, and an out-of-range value raises where you wrote it instead
of being silently truncated into a packet.

## Install

Pre-built wheels are published on PyPI for Linux, macOS, and Windows:

```sh
pip install libosdp
```

That is all most users need. If no wheel matches your platform, pip builds from
source, which needs a C compiler — see [Build and Install](/python/build-and-install)
for the per-OS toolchain requirements and other install methods.

## A channel to talk over

LibOSDP does not own the transport. A [`Channel`](/python/api#channel) moves
bytes over your wire and is handed to the device. For a serial port — an RS-485
adapter or a USB serial device — the bindings ship `SerialChannel`, which needs
pyserial:

```sh
pip install "libosdp[serial]"
```

```python
from osdp import SerialChannel

channel = SerialChannel("/dev/ttyUSB0", 115200)
```

For any other transport (TCP, a radio link, a test harness), subclass
`Channel`. Both snippets below talk over a `SerialChannel`.

## Control Panel mode

A Control Panel (CP) drives one or more Peripheral Devices (PDs). Each PD is
described by a [`PDInfo`](/python/api#pdinfo). `start()` spawns a background
thread that services the bus for you, so the application never has to poll on a
timer.

```python
from osdp import (ControlPanel, PDInfo, KeyStore, LogLevel, LEDColor,
                  SerialChannel, commands, events)

channel = SerialChannel("/dev/ttyUSB0")

# KeyStore.gen_key() provisions a random Secure Channel Base Key (SCBK).
pd_info = [
    PDInfo(101, channel, scbk=KeyStore.gen_key()),
]

cp = ControlPanel(pd_info, log_level=LogLevel.Debug)
cp.start()
cp.sc_wait_all()   # block until the secure channel is up

# An LED command carries a temporary block, a permanent block, or both.
# Counts are in units of 100ms; on_count and off_count cannot both be zero.
led_cmd = commands.LED(
    reader=1,
    led_number=0,
    permanent=commands.PermanentLEDParams(
        on_color=LEDColor.Red,
        off_color=LEDColor.Black,
        on_count=10,
        off_count=10,
    ),
)

while True:
    # Each event is its own type, so match gives you the right fields for free.
    match cp.get_event(pd_info[0].address):
        case events.CardRead(data=data):
            print(f"CP: card {data.hex()}")
        case events.KeyPress(data=keys):
            print(f"CP: keypad {keys!r}")

    cp.submit_command(pd_info[0].address, led_cmd)
```

The full CP surface — every method, the event queue, and completion callbacks —
is on the [Control Panel](/python/control-panel) page. The commands you submit
are documented in [Commands](/python/commands); the events you read in
[Events](/python/events).

## Peripheral Device mode

A Peripheral Device (PD) answers a CP. Describe it with `PDInfo`, declare its
[capabilities](/python/api#the-capabilities-model), and hand LibOSDP a command
handler. Return `None` to accept a command, return a command to answer it
inline, or raise [`NakError`](/python/api#errors) to decline it.

```python
from osdp import (
    Capability, Command, NakCode, NakError, PDCapabilities, PDInfo,
    PeripheralDevice, SerialChannel, StatusReportType, commands, events,
    CardFormat,
)

channel = SerialChannel("/dev/ttyUSB0")

# scbk=None runs the link in plain text. Pass a 16-byte key to secure it, or
# flags=[LibFlag.InstallMode] to let the CP provision one.
pd_info = PDInfo(101, channel, scbk=None)

pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 2, 1),
])

def command_handler(cmd: Command) -> Command | None:
    match cmd:
        case commands.Status(type=report_type):
            return commands.Status(type=report_type, report=read_inputs())
        case commands.BioRead():
            raise NakError(NakCode.BioType)
    return None

pd = PeripheralDevice(pd_info, pd_cap, command_handler=command_handler)
pd.start()
pd.sc_wait()

# For the raw card formats the length is in BITS; this one is ASCII, so bytes.
card_event = events.CardRead(
    reader_no=1,
    direction=1,
    format=CardFormat.ASCII,
    data=bytes([9, 1, 9, 2, 6, 3, 1, 7, 7, 0]),
)

while True:
    pd.submit_event(card_event)
```

The full PD surface and the command-handler contract are on the
[Peripheral Device](/python/peripheral-device) page.

## Full examples

Runnable command-line versions of both apps live in the LibOSDP repo:

- [`examples/python/cp_app.py`](https://github.com/osdp-dev/libosdp/blob/master/examples/python/cp_app.py)
- [`examples/python/pd_app.py`](https://github.com/osdp-dev/libosdp/blob/master/examples/python/pd_app.py)

## Where to next

- [Build and Install](/python/build-and-install) — from source, per-OS.
- [Control Panel](/python/control-panel) / [Peripheral Device](/python/peripheral-device) — the two modes in depth.
- [Commands](/python/commands) and [Events](/python/events) — the message payloads.
- [API Reference](/python/api) — `PDInfo`, capabilities, `Channel`, `KeyStore`, errors, and enums.
