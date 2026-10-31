---
lastUpdated: 2026-10-29
description: "LibOSDP Python API reference — PDInfo, the PD capabilities model, the Channel transport, KeyStore, file transfer, logging, errors, and the enumerations shared across the bindings."
---
# API Reference

Everything documented here is importable from the top-level `osdp` package. This
page covers the supporting types shared by both modes — how you describe a PD,
carry bytes over the wire, key a secure channel, transfer files, and handle
errors — plus the enumerations used throughout. The two device classes and the
message payloads have their own pages:

- [`ControlPanel`](/python/control-panel) — the CP mode class.
- [`PeripheralDevice`](/python/peripheral-device) — the PD mode class.
- [Commands](/python/commands) and [Events](/python/events) — the message dataclasses.

## Describing a PD

### PDInfo

`PDInfo` is the descriptor for one peripheral device — its bus address, the
[`Channel`](#channel) it is reached over, its secure-channel key, and its
identity. A CP is constructed from a list of these; a PD from a single one.
Constructing it with `scbk=None` disables the secure channel; add
`flags=[LibFlag.InstallMode]` to let a CP provision a key instead.

<!-- pydoc:class:types.PDInfo:fields -->

### PdId

The PD's identity block — vendor, model, and firmware — reported by a PD and
read back on a CP with [`get_pd_id`](/python/control-panel#capabilities-ids-and-flags).

<!-- pydoc:class:types.PdId:fields -->

## The capabilities model

A PD advertises what it can do as a set of **capabilities**. Each capability
pairs a [`Capability`](#enumerations) function code with a compliance level and a
number of items (for example, how many LEDs or output lines it has). You declare
them when constructing a `PeripheralDevice`; a CP reads them back with
[`check_capability`](/python/control-panel#capabilities-ids-and-flags).

`PDCapabilities` is the collection you pass in. It accepts either
`PDCapability` instances or plain `(Capability, compliance_level, num_items)`
tuples:

```python
from osdp import Capability, PDCapabilities

pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 2, 1),
    (Capability.AudibleControl, 1, 1),
])
```

<!-- pydoc:class:types.PDCapability:fields -->

See [PD Capabilities](/protocol/pd-capabilities) for the protocol-level meaning
of each function code and its compliance levels.

## Channel

LibOSDP does not own the transport. A `Channel` moves bytes over your wire and
is handed to a `PDInfo`. Serial ports are covered by `SerialChannel`; for
anything else, subclass `Channel` and implement its three methods.

<!-- pydoc:class:channel.Channel -->

<!-- pydoc:class:channel.SerialChannel -->

## KeyStore

`KeyStore` is a helper for generating and persisting Secure Channel Base Keys
during development and testing. `gen_key()` returns a fresh random 16-byte key
suitable for a `PDInfo`.

<!-- pydoc:class:KeyStore -->

## File transfer

To transfer a file to a PD, implement the `FileOps` protocol and register it
with [`register_file_ops`](/python/control-panel#file-transfer), then submit a
[`commands.FileTransfer`](/python/commands#file-transfer). `FileTxStatus`
reports progress.

<!-- pydoc:class:types.FileOps -->

<!-- pydoc:class:types.FileTxStatus:fields -->

## Metrics

`Metrics` is a snapshot of a link's counters — packets, errors, secure-channel
handshakes, and command/event totals — read with `get_metrics`.

<!-- pydoc:class:types.Metrics:fields -->

## Logging

The library logs to stderr, filtered by the `log_level` a `ControlPanel` or
`PeripheralDevice` is created with. `set_log_handler(handler)` routes those
messages to `handler(pd_address, level, message, file, line)` instead —
`pd_address` is -1 for messages not about a PD — and `set_log_handler(None)`
goes back to stderr. The setting is process-wide.

```python
import logging
from osdp import LogLevel, set_log_handler

log = logging.getLogger("osdp")
LEVELS = {LogLevel.Debug: logging.DEBUG, LogLevel.Info: logging.INFO,
          LogLevel.Notice: logging.INFO, LogLevel.Warning: logging.WARNING}

set_log_handler(lambda pd, level, message, file, line:
                log.log(LEVELS.get(level, logging.ERROR), "%s", message))
```

## Errors

The bindings raise a small exception hierarchy, all subclasses of `OSDPError`.
Raise `NakError` from a PD [command handler](/python/peripheral-device#the-command-handler-contract)
to decline a command with a [`NakCode`](#enumerations); `MarshalError` signals a
command or event that could not cross the C boundary.

<!-- pydoc:class:errors.OSDPError -->

<!-- pydoc:class:errors.NakError -->

<!-- pydoc:class:errors.MarshalError -->

<!-- pydoc:class:errors.TrsSessionError -->

## Enumerations

These enums are shared across the API. The command- and event-specific enums
(control codes, colors, card and biometric formats) are documented alongside the
command or event that uses them on the [Commands](/python/commands) and
[Events](/python/events) pages.

Enum members take their values from the C library, so they cannot drift from it.
The wire-facing ones tolerate values a peer sends that the spec does not name,
surfacing them as `UNKNOWN_0x..` members rather than raising mid-callback.

### Capability

<!-- pydoc:class:enums.Capability -->

### LogLevel

<!-- pydoc:class:enums.LogLevel -->

### LibFlag

<!-- pydoc:class:enums.LibFlag -->

### CompletionStatus

<!-- pydoc:class:enums.CompletionStatus -->

### NakCode

<!-- pydoc:class:enums.NakCode -->

## See also

- [Getting Started](/python/getting-started) — install and a first app.
- [Control Panel](/python/control-panel) / [Peripheral Device](/python/peripheral-device) — the mode classes.
- [Commands](/python/commands) / [Events](/python/events) — the message payloads.
