---
lastUpdated: 2026-11-26
description: "An introduction to OSDP (Open Supervised Device Protocol) — the control panel and peripheral device model, the RS-485 bus, and secure access control."
---
# Introduction

OSDP describes the communication protocol for interfacing one or more Peripheral
Devices (PD) to a Control Panel (CP). The OSDP specification describes the
protocol over a two-wire RS-485 multi-drop serial communication channel.
Nevertheless, this protocol can be used to transfer secure data over any
physical channel.

LibOSDP complies with v2.2 of the OSDP specification. This page pulls excerpts
from the specification that are crucial to understanding the protocol.

## Physical Interface

Half-duplex RS-485 — one twisted pair, shield/signal ground.

## Signaling

Half-duplex asynchronous serial: 8 data bits, 1 stop bit, no parity bits, with
any one of the following baud rates: 9600, 19200, 38400, 57600, 115200, or
230400.

## Character Encoding

Text data is UTF-8 encoded (ISO/IEC 10646). Binary fields use the complete
8-bit character, so any bit pattern may appear within a message.

## Channel Access

The communication channel is used in "interrogation/reply" mode. Only the CP
may spontaneously send a message. Each message sent by the CP is addressed to
one and only one PD.

## Timing

The transmitting device must guarantee a gap of at least two character times
before accessing the communication channel. This idle-line delay is required to
allow signal converters and/or multiplexers to sense that the line has become
idle.

The PD must send a single reply to each addressed message within 200 ms. If the
PD is temporarily unable to process a command, it must send `osdp_BUSY`. When
the CP receives `osdp_BUSY`, it may re-send the same command as if it had timed
out.

The typical `REPLY_DELAY` should be less than 3 milliseconds. If a device is
overwhelmed it can send a BUSY message.

## Message Synchronization

A PD obtains message synchronization by waiting for an inter-character timeout
and then looking for a Start-Of-Message (SOM) code. The device receives and
stores at least the header fields while computing the checksum/CRC. If the
checksum is valid, only the PD matching the address field processes the message;
all other PDs monitor the packet by counting the remaining bytes to anticipate
the start of the next packet.

An inter-character timeout while receiving aborts the receive sequence, after
which the PD re-syncs using the method above. The nominal inter-character
timeout is 20 milliseconds.

## Packet Structure

See the [packet structure documentation](/protocol/packet-structure).

## Peripheral Device Capabilities

OSDP PDs must advertise a list of predefined capabilities to the CP in response
to the `osdp_PDCAP` command. See the
[comprehensive list of PD capabilities](/protocol/pd-capabilities).

## Commands

See the [list of commands supported by LibOSDP](/protocol/commands-and-replies).

## Secure Channel

OSDP can run an encrypted, authenticated session over the bus. See
[Secure Channel](/libosdp/secure-channel) for the handshake, SCBK handling, and
install mode.

## Frequently Asked Questions

Common protocol questions are answered in the [FAQ](/protocol/faq).
