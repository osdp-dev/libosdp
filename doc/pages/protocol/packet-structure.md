---
lastUpdated: 2026-11-26
description: "The OSDP packet structure on the wire — header, security block, command/reply data, and CRC/checksum framing for the Open Supervised Device Protocol."
---
# Packet Structure

This document describes how OSDP packets are built and decoded.

## Header

All OSDP packets must have the following 5 bytes. The transmitter usually
sends a mark byte (`0xFF`) ahead of them, to drive the line to the marking
state before the message starts; it is not part of the packet.
(`OPT_OSDP_SKIP_MARK_BYTE` makes LibOSDP leave it out.)

| Byte | Name       | Meaning                                | Value       |
|------|------------|----------------------------------------|-------------|
| 0    | SOM        | Start of Message                       | `0x53`      |
| 1    | ADDR       | Physical Address of the PD             | `0x00–0x7E`; `0x7F` broadcast. Bit 7 is set in a PD's reply |
| 2    | LEN\_LSB   | Packet Length Least Significant Byte   | Any         |
| 3    | LEN\_MSB   | Packet Length Most Significant Byte    | Any         |
| 4    | CTRL       | Message Control Information            | See below   |

### Message Control Information

The CTRL byte at offset 4 is a bit mask:

| Bit   | Mask   | Name        | Meaning                                     |
|-------|--------|-------------|---------------------------------------------|
| 0–1   | `0x03` | SQN         | Packet sequence number                      |
| 2     | `0x04` | CKSUM / CRC | Set: 16-bit CRC; Clear: 8-bit CHECKSUM      |
| 3     | `0x08` | SCB         | Set: SCB is present; Clear: SCB is absent   |

SCB is the Security Control Block presence indicator. It is an optional block
present in [secure channel](/libosdp/secure-channel) messages.

## Security Control Block (Optional)

| Byte      | Name           | Meaning                            | Value          |
|-----------|----------------|------------------------------------|----------------|
| 5         | SEC\_BLK\_LEN  | Length of Security Control Block   | Any            |
| 6         | SEC\_BLK\_TYPE | Security Block Type                | See below      |
| 7 – m-1   | SEC\_BLK\_DATA | Security Block Data                | Based on type  |

## Command/Reply Structure

The Command/Reply block is mandatory. If no SCB is present, it follows directly
after the fixed header; otherwise it is offset by the SCB length.

| Byte   | Name         | Meaning                 | Value              |
|--------|--------------|-------------------------|--------------------|
| 5      | CMND/REPLY   | Command or Reply Code   | See [CMD/REPLY](/protocol/commands-and-replies) |
| 6 – n  | DATA         | (optional) Data Block   | Based on CMD/REPLY |

### Message Authentication Code (Optional)

Present in secure channel messages. These 4 bytes appear just before the
CRC/CKSUM bytes; the offsets below assume CRC mode (with a 1-byte checksum
they move one byte later).

| Byte | Name     | Meaning                              | Value |
|------|----------|--------------------------------------|-------|
| n-6  | MAC[0]   | Message Authentication Code Byte 0   | Any   |
| n-5  | MAC[1]   | Message Authentication Code Byte 1   | Any   |
| n-4  | MAC[2]   | Message Authentication Code Byte 2   | Any   |
| n-3  | MAC[3]   | Message Authentication Code Byte 3   | Any   |

## Packet Validation

The last 2 bytes (or 1 byte if CKSUM mode is selected in the CTRL bit mask):

| Byte | Name           | Meaning                                       | Value      |
|------|----------------|-----------------------------------------------|------------|
| n-2  | CKSUM/CRC\_LSB | Checksum, or CRC-16 Least Significant Byte    | CKSUM/CRC  |
| n-1  | CRC\_MSB       | (optional) CRC-16 Most Significant Byte       | CKSUM/CRC  |
