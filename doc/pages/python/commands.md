---
lastUpdated: 2026-10-29
description: "LibOSDP Python commands reference — every command dataclass a control panel sends to a PD (LED, buzzer, output, text, keyset, comset, manufacturer, file transfer, status, biometrics) and its fields."
---
# Commands

Commands travel from a Control Panel to a PD. In Python each command is a frozen
dataclass in the `osdp.commands` module. You construct one with keyword
arguments and hand it to [`ControlPanel.submit_command`](/python/control-panel#commands):

```python
from osdp import commands, BuzzerControlCode

cp.submit_command(101, commands.Buzzer(
    reader=0,
    control_code=BuzzerControlCode.DefaultTone,
    on_count=10,
    off_count=10,
    rep_count=3,
))
```

Every field is validated in the constructor, so an out-of-range value or an
over-long buffer raises immediately, at the line where you built the command,
rather than being truncated on its way into a packet. Byte and string fields
have documented maximum lengths (`commands.MAX_TEXT_LEN`, `MAX_MFG_DATA_LEN`,
and so on) sourced from the C library.

A PD may also **return** a command from its
[command handler](/python/peripheral-device#the-command-handler-contract) to
answer a query inline — most commonly a `Status` reply to a status request.

## Reader output

### Output

Drive a digital output line. Its `control_code` is an `OutputControlCode`:

<!-- pydoc:class:commands.Output -->

<!-- pydoc:class:enums.OutputControlCode -->

### LED

Control a reader LED. An LED command carries a **temporary** block, a
**permanent** block, or both; each is described by the helper dataclasses below.
Counts are in units of 100ms, and within a block `on_count` and `off_count`
cannot both be zero.

<!-- pydoc:class:commands.LED -->

<!-- pydoc:class:commands.TemporaryLEDParams -->

<!-- pydoc:class:commands.PermanentLEDParams -->

Each block's control code selects whether it takes effect; colors are `LEDColor`
values:

<!-- pydoc:class:enums.TemporaryLEDControlCode -->

<!-- pydoc:class:enums.PermanentLEDControlCode -->

<!-- pydoc:class:enums.LEDColor -->

### Buzzer

Sound the reader's buzzer. Its `control_code` is a `BuzzerControlCode`:

<!-- pydoc:class:commands.Buzzer -->

<!-- pydoc:class:enums.BuzzerControlCode -->

### Text

Show text on a reader's display. Its `control_code` is a `TextControlCode`:

<!-- pydoc:class:commands.Text -->

<!-- pydoc:class:enums.TextControlCode -->

## Configuration

### Comset / ComsetDone

Change a PD's address and baud rate. A PD acknowledges by returning a
`ComsetDone` from its command handler.

<!-- pydoc:class:commands.Comset -->

<!-- pydoc:class:commands.ComsetDone -->

### Keyset

Set the Secure Channel Base Key (SCBK) on a PD. See
[Secure Channel](/libosdp/secure-channel) for how keying and install mode work.

<!-- pydoc:class:commands.Keyset -->

## Data and status

### Manufacturer

Send a vendor-specific payload.

<!-- pydoc:class:commands.Manufacturer -->

### File Transfer

Begin a file transfer to the PD. Register a
[`FileOps`](/python/api#file-transfer) first so the library can read the file.
To abort a transfer in progress, call `ControlPanel.cancel()`.

<!-- pydoc:class:commands.FileTransfer -->

### Smartcard

Talk to a smartcard attached to the PD. `PivData` retrieves a PIV object;
`GenAuth` and `CrAuth` run a cryptographic challenge against the card. The
payloads travel as multi-part messages and the PD answers with the matching
`events.PivData` / `events.GenAuth` / `events.CrAuth` carrying the reassembled
reply. See [Smartcard Commands](/libosdp/smartcard) for the full flow.

<!-- pydoc:class:commands.PivData -->

<!-- pydoc:class:commands.GenAuth -->

<!-- pydoc:class:commands.CrAuth -->

### Transparent Reader (TRS)

Drive a smart card directly: a `TrsStart` opens a card session, `TrsSendApdu`,
`TrsEnterPin` and `TrsCardScan` work inside it, and `TrsStop` closes it. See
[Transparent Reader Support](/libosdp/smartcard#transparent-reader-support-trs).

<!-- pydoc:class:commands.TrsStart -->

<!-- pydoc:class:commands.TrsSendApdu -->

<!-- pydoc:class:commands.TrsEnterPin -->

<!-- pydoc:class:commands.TrsPinBlock -->

<!-- pydoc:class:commands.TrsPinLengthField -->

<!-- pydoc:class:commands.TrsCardScan -->

<!-- pydoc:class:commands.TrsStop -->

### Status

Request a status report, or (from a PD) carry one back. The `type` is a
`StatusReportType`:

<!-- pydoc:class:commands.Status -->

<!-- pydoc:class:enums.StatusReportType -->

### Notification

Library-generated notification delivered as a command on the PD side when
notifications are enabled. The `type` is a `NotificationType`:

<!-- pydoc:class:commands.Notification -->

<!-- pydoc:class:enums.NotificationType -->

## Biometrics

### BioRead / BioMatch

Ask a reader to capture a biometric (`BioRead`) or match one against a supplied
template (`BioMatch`). `type` is a `BioType` and `format` a `BioFormat`. A
captured template larger than one packet is only carried when both roles opt in
to [multi-part biometric replies](/protocol/multipart#the-special-case-biometric-read-replies):

<!-- pydoc:class:commands.BioRead -->

<!-- pydoc:class:commands.BioMatch -->

<!-- pydoc:class:enums.BioType -->

<!-- pydoc:class:enums.BioFormat -->

## See also

- [Control Panel](/python/control-panel) — how commands are submitted and completed.
- [Events](/python/events) — the payloads a PD sends back.
- [API Reference](/python/api) — the remaining enums and supporting types.
