---
lastUpdated: 2026-10-29
description: "LibOSDP Python peripheral device (PD) reference — the PeripheralDevice class, its lifecycle, the command-handler contract for accepting, answering, or NAKing commands, and reporting events."
---
# Peripheral Device

A **Peripheral Device** (PD) is the follower on the bus: it answers a Control
Panel's commands and reports events such as card reads and key presses. In
Python that role is the [`PeripheralDevice`](#peripheraldevice) class. A PD is
described by one [`PDInfo`](/python/api#pdinfo) and a set of
[capabilities](/python/api#the-capabilities-model) that tell the CP what it can
do.

## Lifecycle

1. **Construct** — `PeripheralDevice(pd_info, pd_cap, ...)`, optionally passing a
   command handler and an event-completion handler.
2. **Register handlers** (optional) — or set them later with
   `set_command_handler` / `set_event_completion_handler`.
3. **Start** — `start()` spawns the background thread that services the bus and
   keeps the library refreshed within OSDP's 50ms window.
4. **Wait for the link** — `sc_wait()` blocks until the secure channel is up (or
   returns once online for an unkeyed link).
5. **Run** — answer commands in the handler and report events with
   `submit_event()`.
6. **Tear down** — `teardown()`.

```python
pd = PeripheralDevice(pd_info, pd_cap, command_handler=command_handler)
pd.start()
pd.sc_wait()
...
pd.teardown()
```

::: warning Callbacks run on the library thread
The command handler and event-completion handler are invoked from the
background refresh thread. Keep them short — blocking in the command handler
delays the reply to the CP — and guard any state shared with your application.
:::

## The command-handler contract

The command handler is the heart of a PD. LibOSDP calls it for each command the
CP sends, and your return value decides the reply:

- **Return `None`** — accept the command (the library ACKs it).
- **Return a command** — answer inline. A `commands.Status` query, for example,
  is answered by returning a `commands.Status` carrying the report.
- **Raise [`NakError`](/python/api#errors)** — decline the command with a
  [`NakCode`](/python/api#enumerations).

```python
from osdp import Command, NakCode, NakError, StatusReportType, commands

def command_handler(cmd: Command) -> Command | None:
    match cmd:
        case commands.Status(type=StatusReportType.Input):
            return commands.Status(type=StatusReportType.Input, report=read_inputs())
        case commands.LED():
            drive_led(cmd)
        case commands.BioRead():
            raise NakError(NakCode.BioType)   # not supported on this PD
    return None
```

You can also pull commands off a queue instead of (or alongside) the handler
with `get_command(timeout=...)`, which returns the next [command](/python/commands)
or `None`.

## PeripheralDevice

<!-- pydoc:class:PeripheralDevice:methods -->

## Reporting events

A PD reports asynchronous events — card reads, key presses, biometric results —
by submitting an [event](/python/events) dataclass:

```python
pd.submit_event(events.CardRead(reader_no=0, format=CardFormat.Wiegand, data=card, bits=26))
```

Each submitted event settles once. Register an event-completion handler to learn
its outcome as a [`CompletionStatus`](/python/api#enumerations) — for instance
`Flushed` if `flush_events()` dropped it before it reached the wire:

```python
def on_event_complete(event, status):
    if status == CompletionStatus.Flushed:
        print("event removed by flush")

pd.set_event_completion_handler(on_event_complete)
```

A PD that bridges a smart-card reader advertises `Capability.SmartCard` and
answers each `commands.TrsSendApdu` by submitting an `events.TrsCardData` (or
`events.TrsError`) — from inside the command handler to reply at once,
returning `None`, or later for the answer to ride out on a poll. A card
arriving in the field is reported with `events.TrsCardInfo`. See
[Transparent Reader Support](/libosdp/smartcard#transparent-reader-support-trs).

## Install mode and secure channel

Constructing `PDInfo` with `scbk=None` **disables the secure channel**: the PD
advertises no communication security and the link runs in plain text. Pass a
16-byte `scbk` to fix the key ahead of time.

To let a CP provision the key instead, pass `flags=[LibFlag.InstallMode]`
alongside `scbk=None`. The PD then brings the channel up on the well-known
SCBK-Default just long enough to receive a real key, and leaves install mode
once it has one. This is a provisioning-time mode — see
[Secure Channel](/libosdp/secure-channel) for the full keying model.

## See also

- [Control Panel](/python/control-panel) — the other end of the link.
- [Commands](/python/commands) — what a PD receives.
- [Events](/python/events) — what a PD reports.
- [API Reference](/python/api) — `PDInfo`, the capabilities model, and errors.
