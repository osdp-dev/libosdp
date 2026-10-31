---
lastUpdated: 2026-06-24
description: "OSDP PD capabilities — how a peripheral device reports its supported functions so a control panel can discover features before driving the device."
---
# PD Capabilities

This section documents the structure of the OSDP capability response
(`REPLY_PDCAP`).

## Message Structure

The `REPLY_PDCAP` response consists of 3-byte tuples with the following
meaning:

| Byte | Name             | Meaning                                   | Value     |
|------|------------------|-------------------------------------------|-----------|
| 0    | Function Code    | Function/feature code                     | See below |
| 1    | Compliance Level | Level of compliance with above function   | See below |
| 2    | Number of Units  | Number of objects of this type            | See below |

## Function Code 1 — Contact Status Monitoring

Indicates the ability to monitor the status of a switch using a two-wire
electrical connection between the PD and the switch.

**Compliance Levels:**
- `01` — PD monitors and reports circuit state without supervision, encoding per default interpretation of contact state.
- `02` — Like 01, plus: PD accepts configuration of the encoding of open/closed circuit status to active/inactive status.
- `03` — Like 02, plus: PD supports supervised monitoring. Operating mode per circuit is determined by configuration settings.
- `04` — Like 03, plus: PD supports custom End-Of-Line settings within manufacturer guidelines.

**Number of Units:** Number of inputs.

## Function Code 2 — Output Control

Provides a switched output, typically a relay, with active/inactive states.
The CP can set the output state directly or specify a timed activation.

**Compliance Levels:**
- `01` — PD can activate and deactivate the output per CP command.
- `02` — Like 01, plus: PD accepts configuration of the output driver to set the inactive state (supports fail-safe/fail-secure modes).
- `03` — Like 01, plus: PD accepts timed commands specifying output state for a duration.
- `04` — Like 02 and 03 combined.

**Number of Units:** Number of outputs.

## Function Code 3 — Card Data Format

Indicates the form in which card data is presented to the CP.

**Compliance Levels:**
- `01` — PD sends card data as a raw bit array, up to 1024 bits.
- `02` — PD sends card data as a BCD character array, up to 256 characters.
- `03` — PD can send card data as either a bit array or BCD character array.

**Number of Units:** N/A, set to 0.

## Function Code 4 — Reader LED Control

Indicates the presence and type of LEDs.

**Compliance Levels:**
- `01` — PD supports on/off control only.
- `02` — PD supports timed commands.
- `03` — Like 02, plus bi-color LEDs.
- `04` — Like 02, plus tri-color LEDs.

**Number of Units:** Number of LEDs per reader.

## Function Code 5 — Reader Audible Output

Indicates the presence and type of an audible annunciator (buzzer or tone
generator).

**Compliance Levels:**
- `01` — PD supports on/off control only.
- `02` — PD supports timed commands.

**Number of Units:** Number of audible annunciators per reader.

## Function Code 6 — Reader Text Output

Indicates that the PD supports a character-based text display.

**Compliance Levels:**
- `00` — No text display support.
- `01` — PD supports 1 row of 16 characters.
- `02` — PD supports 2 rows of 16 characters.
- `03` — PD supports 4 rows of 16 characters.
- `04` — TBD.

**Number of Units:** Number of textual displays per reader.

## Function Code 7 — Time Keeping

Indicates date and time awareness or time-keeping ability.

**Compliance Levels:**
- `00` — PD does not support time/date functionality.
- `01` — PD understands time/date settings via `osdp_TDSET`.
- `02` — PD can locally update the time and date.

**Number of Units:** N/A, set to 0.

## Function Code 8 — Check Character Support

Indicates CRC-16 capability. All PDs must support checksum mode.

**Compliance Levels:**
- `00` — PD does not support CRC-16; checksum only.
- `01` — PD supports 16-bit CRC-16.

**Number of Units:** N/A, set to 0.

## Function Code 9 — Communication Security

Indicates the extent of [Secure Channel](/libosdp/secure-channel) communication
support.

**Compliance Levels** (bit map):
- `0x01` — (Bit 0) AES128 support.
- `0x02` — (Bit 1) to be defined.

**Number of Units** (bit map):
- `0x01` — (Bit 0) Default AES128 key, as defined in APPENDIX D.
- `0x02` — (Bit 1) to be defined.

## Function Code 10 — Receive Buffer Size

Indicates the maximum size of a single message the PD can receive.

**Compliance Level:** LSB of the buffer size.

**Number of Units:** MSB of the buffer size.

## Function Code 11 — Largest Combined Message Size

Indicates the maximum size of a multi-part message the PD can handle.

**Compliance Level:** LSB of the combined buffer size.

**Number of Units:** MSB of the combined buffer size.

## Function Code 12 — Smart Card Support

Indicates whether the PD supports transparent mode for communicating directly
with a smart card.

**Compliance Levels:**
- `0` — PD does not support transparent reader mode.
- `1` — PD supports transparent reader mode.

**Number of Units:** Unused, send `0x00`.

## Function Code 13 — Readers

Indicates the number of credential reader devices present.

**Compliance Levels** (bit fields): `0x01` (Bit 0), `0x02` (Bit 1).

**Number of Units:** Number of attached downstream readers.

### Reader numbering in LibOSDP

Commands and replies that address a reader — `osdp_LED`, `osdp_BUZ`,
`osdp_TEXT`, `osdp_BIOREAD`, `osdp_BIOMATCH` and the transparent-mode card
commands, and the card, keypad, biometric and transparent-mode replies —
carry a reader number. The specification numbers it only as "0 = First
Reader, 1 = Second Reader", and does not say whether a PD's own reader is
one of them: whether the first reader is the PD itself or the first reader
attached to it.

LibOSDP reads it this way:

- Reader 0 is always the PD itself, whether or not anything is attached to
  it.
- The readers this capability counts are the ones attached downstream,
  numbered from 1. A PD that advertises N readers takes reader numbers 0 to N.
- A reader number above N is refused: the CP does not accept such a command,
  a PD NAKs one it receives with `0x09` (unable to process command record),
  and a PD does not accept an event for such a reader.

The reader tamper status report (`osdp_RSTATR`) follows from this. The
specification has it carry one status per reader, and says reader tamper
applies only to external readers attached to the PD. So the report has N
entries, the first being reader 1, and the PD's own reader 0 has none.

## Function Code 14 — Biometrics

Indicates the ability of the reader to handle biometric input.

**Compliance Levels:**
- `0` — No biometric support.
- `1` — Fingerprint, Template 1.
- `2` — Fingerprint, Template 2.
- `3` — Iris, Template 1.

**Number of Units:** Number of readers.
