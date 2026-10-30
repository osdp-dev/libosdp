---
lastUpdated: 2026-10-29
description: "LibOSDP Python events reference — every event dataclass a peripheral device reports to a control panel (card read, key press, biometrics, status, manufacturer, notification) and its fields."
---
# Events

Events travel from a PD to a Control Panel. In Python each event is a frozen
dataclass in the `osdp.events` module. A PD reports one with
[`PeripheralDevice.submit_event`](/python/peripheral-device#reporting-events);
a CP receives it from [`ControlPanel.get_event`](/python/control-panel#events)
or a registered event handler.

Because each event is its own type, a `match` statement gives you exactly the
right fields with no dictionary lookups:

```python
match cp.get_event(101):
    case events.CardRead(data=data, format=fmt):
        print(f"card {data.hex()} ({fmt.name})")
    case events.KeyPress(data=keys):
        print(f"keys {keys!r}")
    case events.Status(report=report):
        print(f"status {report!r}")
```

## Card and keypad

### CardRead

A card was presented to a reader. For the raw formats, `bits` is the exact bit
count and `data` is the packed payload; for ASCII, `data` is the character
bytes. `format` is a `CardFormat`:

<!-- pydoc:class:events.CardRead -->

<!-- pydoc:class:enums.CardFormat -->

### KeyPress

Keys were entered on a reader's keypad.

<!-- pydoc:class:events.KeyPress -->

## Biometrics

### BioRead

A PD's answer to a `commands.BioRead` — the captured template and its quality.
`status` is a `BioStatus`:

<!-- pydoc:class:events.BioRead -->

A template larger than one packet is delivered only when both roles enable the
opt-in multi-part flag; see
[Multi-part Messages](/protocol/multipart#the-special-case-biometric-read-replies).

<!-- pydoc:class:enums.BioStatus -->

### BioMatch

A PD's answer to a `commands.BioMatch` — whether the scan matched and the score.

<!-- pydoc:class:events.BioMatch -->

## Smartcard

A PD's answers to the smartcard commands, carrying the payload reassembled
from the multi-part reply. On the PD side, submit these to answer the
corresponding command — from inside the command handler for an inline reply,
or later for delivery on a subsequent poll.

<!-- pydoc:class:events.PivData -->

<!-- pydoc:class:events.GenAuth -->

<!-- pydoc:class:events.CrAuth -->

### Transparent Reader (TRS)

What a reader reports about a smart card: one arriving (`TrsCardInfo`), its
presence (`TrsCardPresent`), the card's answer to an APDU (`TrsCardData`), a PIN
entry finishing (`TrsPinComplete`), or an exchange the reader could not do
(`TrsError`). On the PD side, submit these to answer a TRS command. See
[Transparent Reader Support](/libosdp/smartcard#transparent-reader-support-trs).

<!-- pydoc:class:events.TrsCardInfo -->

<!-- pydoc:class:events.TrsCardPresent -->

<!-- pydoc:class:events.TrsCardData -->

<!-- pydoc:class:events.TrsPinComplete -->

<!-- pydoc:class:events.TrsError -->

## Status and manufacturer

### Status

A status report from the PD (inputs, outputs, tamper, power). `type` is a
[`StatusReportType`](/python/commands#status).

<!-- pydoc:class:events.Status -->

### Manufacturer

Vendor-specific replies and status/error notifications.

<!-- pydoc:class:events.ManufacturerReply -->

<!-- pydoc:class:events.ManufacturerStatus -->

<!-- pydoc:class:events.ManufacturerError -->

## Notification

A library-generated notification, delivered as an event when notifications are
enabled. Beyond the raw `type`/`arg0`/`arg1` fields, `Notification` exposes
computed properties (`succeeded`, `sc_active`, `pd_online`, `file_id`,
`file_tx_outcome`) that decode the arguments for the notification kind. `type`
is a [`NotificationType`](/python/commands#notification).

<!-- pydoc:class:events.Notification -->

## See also

- [Peripheral Device](/python/peripheral-device) — how events are submitted and completed.
- [Commands](/python/commands) — the payloads a CP sends.
- [API Reference](/python/api) — the remaining enums and supporting types.
