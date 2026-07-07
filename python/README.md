# LibOSDP for Python

This package exposes the C/C++ library for OSDP devices to python to enable rapid
prototyping of these devices. There are two modules exposed by this package:

- `osdp_sys`: A thin wrapper around the C/C++ API; this is a low level API and
  is no longer recommended to use this directly.

- `osdp`: A wrapper over the `osdp_sys` to provide python friendly API; this
  implementation which is now powering the integration testing suit used to test
  all changes made to this project.

## Documentation

Full documentation is hosted at [doc.osdp.dev](https://doc.osdp.dev/), including
a Python-specific section:

- [Getting Started](https://doc.osdp.dev/python/getting-started)
- [API Reference](https://doc.osdp.dev/python/api)
- [Commands](https://doc.osdp.dev/python/commands) — the command dicts a CP sends
- [Events](https://doc.osdp.dev/python/events) — the event dicts a PD sends

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

LibOSDP does not own the transport; you implement a `Channel` over your wire.
Both snippets below use this `pyserial`-backed channel (`pip install pyserial`):

```python
import serial
from osdp import Channel

class SerialChannel(Channel):
    def __init__(self, device: str, speed: int = 115200):
        self.dev = serial.Serial(device, speed, timeout=0)

    def read(self, max_read: int) -> bytes:
        return self.dev.read(max_read)

    def write(self, data: bytes) -> int:
        return self.dev.write(data)

    def flush(self):
        self.dev.flush()

    def __del__(self):
        self.dev.close()
```

### Control Panel Mode

```python
from osdp import ControlPanel, PDInfo, KeyStore, LogLevel, Command, CommandLEDColor

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

# See https://doc.osdp.dev/python/commands for all command dicts
led_cmd = {
    'command': Command.LED,
    'reader': 1,
    'led_number': 0,
    'control_code': 1,
    'on_count': 10,
    'off_count': 10,
    'on_color': CommandLEDColor.Red,
    'off_color': CommandLEDColor.Black,
    'temporary': False,
}

while True:
    ## Check if we have an event from PD
    event = cp.get_event(pd_info[0].address)
    if event:
        print(f"CP: Received event {event}")

    # Send LED command to PD-0
    cp.send_command(pd_info[0].address, led_cmd)
```

see [examples/cp_app.py][2] for more details.

Optional command completion callback:

```python
from osdp import CompletionStatus

def on_command_complete(address, command, status):
    if status == CompletionStatus.Ok:
        print("command completed")

cp.set_command_completion_handler(on_command_complete)
```

### Peripheral Device mode:

```python
from osdp import PeripheralDevice, PDInfo, PDCapabilities, Capability, Event, CardFormat

# Create a communication channel
channel = SerialChannel("/dev/ttyUSB0")

# Describe the PD (setting scbk=None puts the PD in install mode)
pd_info = PDInfo(101, channel, scbk=None)

# Indicate the PD's capabilities to LibOSDP.
pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 2, 1),
])

# Create a PD device and kick-off the handler thread and wait till a secure
# channel is established.
pd = PeripheralDevice(pd_info, pd_cap)
pd.start()
pd.sc_wait()

# See https://doc.osdp.dev/python/events for all event dicts
card_event = {
    'event': Event.CardRead,
    'reader_no': 1,
    'direction': 1,
    'format': CardFormat.ASCII,
    'data': bytes([9, 1, 9, 2, 6, 3, 1, 7, 7, 0]),
}

while True:
    # Send a card read event to CP
    pd.notify_event(card_event)

    # Check if we have any commands from the CP
    cmd = pd.get_command()
    if cmd:
        print(f"PD: Received command: {cmd}")
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

[2]: https://github.com/osdp-dev/libosdp/blob/master/examples/python/cp_app.py
[3]: https://github.com/osdp-dev/libosdp/blob/master/examples/python/pd_app.py
