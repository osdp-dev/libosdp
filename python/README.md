# LibOSDP for Python

This package exposes the C/C++ library for OSDP devices to python to enable rapid
prototyping of these devices.

Commands and events are typed dataclasses, so your editor completes the fields,
a type checker catches a wrong one before you run, and an out-of-range value
raises where you wrote it instead of being truncated on its way into a packet.

The package ships a `py.typed` marker; `mypy --strict` and `pyright` both pass
against it.

## Documentation

Full documentation is hosted at [doc.osdp.dev](https://doc.osdp.dev/), including
a Python-specific section:

- [Getting Started](https://doc.osdp.dev/python/getting-started) — install and a minimal app
- [Build and Install](https://doc.osdp.dev/python/build-and-install) — from source, per-OS
- [Control Panel](https://doc.osdp.dev/python/control-panel) — the CP mode class
- [Peripheral Device](https://doc.osdp.dev/python/peripheral-device) — the PD mode class
- [Commands](https://doc.osdp.dev/python/commands) — what a CP sends
- [Events](https://doc.osdp.dev/python/events) — what a PD sends
- [API Reference](https://doc.osdp.dev/python/api) — PDInfo, capabilities, channel, errors, enums

## Install

You can install LibOSDP from PyPI using,

```sh
pip install libosdp
```

Or, from github,

```sh
pip install -e "git+https://github.com/osdp-dev/libosdp#egg=libosdp&subdirectory=python"
```

Or, from source using,

```sh
git clone https://github.com/osdp-dev/libosdp --recurse-submodules
cd libosdp/python
pip install .
```

## Quick Start

LibOSDP does not own the transport; a `Channel` moves bytes over your wire.
For a serial port — an RS-485 adapter or a USB serial device — use the
built-in `SerialChannel` (`pip install libosdp[serial]`); for anything else,
subclass `Channel`. Both snippets below talk over a serial port.

### Control Panel Mode

```python
from osdp import (ControlPanel, PDInfo, KeyStore, LogLevel, LEDColor,
                  SerialChannel, commands, events)

# Create a communication channel
channel = SerialChannel("/dev/ttyUSB0")

# populate osdp_pd_info_t from python
pd_info = [
    PDInfo(101, channel, scbk=KeyStore.gen_key()),
]

# Create a CP device and kick-off the handler thread and wait till a secure
# channel is established.
cp = ControlPanel(pd_info, log_level=LogLevel.Debug)
cp.start()
cp.sc_wait_all()

# An LED command carries a temporary block, a permanent block, or both. Counts
# are in units of 100ms; on_count and off_count cannot both be zero.
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
    # Each event is its own type, so match gives you the right fields for free
    match cp.get_event(pd_info[0].address):
        case events.CardRead(data=data):
            print(f"CP: card {data.hex()}")
        case events.KeyPress(data=keys):
            print(f"CP: keypad {keys!r}")

    # Send LED command to PD-0
    cp.submit_command(pd_info[0].address, led_cmd)
```

see [examples/cp_app.py][2] for more details.

Optional command completion callback. Every submitted command is reported
exactly once, including ones flushed before they reached the wire:

```python
from osdp import CompletionStatus

def on_command_complete(address, command, status):
    if status == CompletionStatus.Ok:
        print(f"{type(command).__name__} completed")

cp.set_command_completion_handler(on_command_complete)
```

### Peripheral Device mode:

```python
from osdp import (
    Capability, CardFormat, Command, NakCode, NakError, PDCapabilities, PDInfo,
    PeripheralDevice, SerialChannel, StatusReportType, commands, events,
)

# Create a communication channel
channel = SerialChannel("/dev/ttyUSB0")

# Describe the PD. Without an scbk the secure channel is disabled; pass
# flags=[LibFlag.InstallMode] to let a CP provision one over SCBK-D.
pd_info = PDInfo(101, channel, scbk=None)

# Indicate the PD's capabilities to LibOSDP.
pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 2, 1),
])

# Return None to accept a command, return a command to answer it inline, or
# raise NakError to decline it.
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

# A 26-bit Wiegand card: the length is in BITS, padded out to whole bytes.
card_event = events.CardRead(
    reader_no=0,
    format=CardFormat.Wiegand,
    data=bytes([0x01, 0x02, 0x03, 0x40]),
    bits=26,
)

while True:
    # Send a card read event to CP
    pd.submit_event(card_event)
```

see [examples/pd_app.py][3] for more details.

Optional event completion callback:

```python
from osdp import CompletionStatus

def on_event_complete(event, status):
    if status == CompletionStatus.Flushed:
        print("event removed by flush")

pd.set_event_completion_handler(on_event_complete)
```

## Contributing

### The C extension is private

The compiled extension lives at `osdp._sys`. It speaks dicts, and the only
module allowed to talk to it is `osdp/_marshal.py`. Treat its dict schema as an
implementation detail shared between the two halves of this package, not an API:
it changes whenever that is convenient.

### How fields are documented

Every field carries a PEP 258 attribute docstring — a plain string *after* the
field, not a `#` comment above it:

```python
on_count: int = 0
"""Duration of the ON phase, in units of 100ms."""
```

This is deliberate and load-bearing. That one string does two jobs: editors
(pyright, Pylance) show it when you hover the field, and `doxy-pyfilter.py`
rewrites it into a Doxygen `##` block so it also reaches the documentation site.
Doxygen will not read the docstring itself, and editors will not read a `##`
comment, so writing it either other way silently loses half the benefit.

Do not "tidy" these into `#` comments. `tests/pytest/test_docs.py` asserts the
documentation actually lands in the generated XML, and will fail if you do.

To regenerate the XML the doc site consumes:

```sh
cd python && doxygen Doxyfile   # output in python/doxygen/xml
```

[2]: https://github.com/osdp-dev/libosdp/blob/master/examples/python/cp_app.py
[3]: https://github.com/osdp-dev/libosdp/blob/master/examples/python/pd_app.py
