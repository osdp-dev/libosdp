// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// Something a peripheral device reports to its control panel. Values are
/// validated when the event is submitted.
/// </summary>
public abstract record OsdpEvent
{
    private protected OsdpEvent() { }
}

/// <summary>A card was read (osdp_RAW).</summary>
/// <param name="ReaderNo">Reader number, from 0.</param>
/// <param name="Format">
/// Raw bits. LibOSDP does not support <see cref="CardFormat.Ascii"/> (osdp_FMT);
/// submitting it throws <see cref="ArgumentException"/>.
/// </param>
/// <param name="Data">Card data, at most 64 bytes. Raw formats pad the last byte.</param>
/// <param name="Bits">
/// Length in bits for raw formats; null means every bit of <paramref name="Data"/>.
/// Null on received <see cref="CardFormat.Ascii"/> events; always set on received
/// events with a raw format.
/// </param>
public sealed record CardReadEvent(int ReaderNo, CardFormat Format, byte[] Data, int? Bits = null) : OsdpEvent;

/// <summary>Keys were pressed on a reader keypad (osdp_KEYPAD).</summary>
/// <param name="ReaderNo">Reader number, from 0.</param>
/// <param name="Data">Key codes, at most 64.</param>
public sealed record KeyPressEvent(int ReaderNo, byte[] Data) : OsdpEvent;

/// <summary>Manufacturer-specific reply (osdp_MFGREP).</summary>
/// <param name="VendorCode">IEEE OUI of the vendor (low 24 bits).</param>
/// <param name="Data">Payload, at most 128 bytes.</param>
public sealed record ManufacturerReplyEvent(uint VendorCode, byte[] Data) : OsdpEvent;

/// <summary>Manufacturer-specific status (osdp_MFGSTATR).</summary>
/// <param name="Data">Payload, at most 128 bytes.</param>
public sealed record ManufacturerStatusEvent(byte[] Data) : OsdpEvent;

/// <summary>Manufacturer-specific error (osdp_MFGERRR).</summary>
/// <param name="Data">Payload, at most 128 bytes.</param>
public sealed record ManufacturerErrorEvent(byte[] Data) : OsdpEvent;

/// <summary>Result of a biometric read (osdp_BIOREADR).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Status">Outcome.</param>
/// <param name="Type">Biometric type.</param>
/// <param name="Quality">Quality, 0-100.</param>
/// <param name="Template">Template, at most 256 bytes.</param>
public sealed record BioReadEvent(byte Reader, BioStatus Status, BioType Type, byte Quality,
                                  byte[] Template) : OsdpEvent;

/// <summary>Result of a biometric match (osdp_BIOMATCHR).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Status">Outcome.</param>
/// <param name="Score">Match score, 0-100.</param>
public sealed record BioMatchEvent(byte Reader, BioStatus Status, byte Score) : OsdpEvent;

/// <summary>A status report (osdp_ISTATR, OSTATR, LSTATR, RSTATR).</summary>
/// <param name="Type">Which report.</param>
/// <param name="Report">One byte per entry, at most 64.</param>
public sealed record StatusEvent(StatusReportType Type, byte[] Report) : OsdpEvent;

/// <summary>A library notification delivered to a CP application. Cannot be submitted.</summary>
/// <param name="Notification">What happened.</param>
public sealed record NotificationEvent(OsdpNotification Notification) : OsdpEvent;

/// <summary>PIV data read from a card (osdp_PIVDATAR).</summary>
/// <param name="Data">Data, at most 256 bytes.</param>
public sealed record PivDataEvent(byte[] Data) : OsdpEvent;

/// <summary>PIV general authenticate response (osdp_GENAUTHR).</summary>
/// <param name="Data">Response, at most 256 bytes.</param>
public sealed record GenAuthEvent(byte[] Data) : OsdpEvent;

/// <summary>PIV challenge/response result (osdp_CRAUTHR).</summary>
/// <param name="Data">Response, at most 256 bytes.</param>
public sealed record CrAuthEvent(byte[] Data) : OsdpEvent;

/// <summary>A smart card entered a reader's field (transparent reader, osdp_XRD).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Protocol">How the card talks to the reader.</param>
/// <param name="Csn">Card serial number (UID), at most 32 bytes.</param>
/// <param name="ProtocolData">ATR (contact) or ATS/ATQB (contactless), at most 64 bytes.</param>
public sealed record TrsCardInfoEvent(byte Reader, TrsCardProtocol Protocol, byte[] Csn,
                                      byte[] ProtocolData) : OsdpEvent;

/// <summary>Whether a smart card is in a reader's field (transparent reader, osdp_XRD).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Status">Presence, and the interface when known.</param>
public sealed record TrsCardPresentEvent(byte Reader, TrsCardStatus Status) : OsdpEvent;

/// <summary>A card's answer to a <see cref="TrsSendApduCommand"/> (osdp_XRD).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="ReaderStatus">Result of the exchange as the reader reports it; reader-defined.</param>
/// <param name="Apdu">The R-APDU, status word included.</param>
public sealed record TrsCardDataEvent(byte Reader, byte ReaderStatus, byte[] Apdu) : OsdpEvent;

/// <summary>A secure PIN entry finished (osdp_XRD).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="ReaderStatus">Result as the reader reports it; reader-defined.</param>
/// <param name="Tries">Number of PIN entry attempts.</param>
public sealed record TrsPinCompleteEvent(byte Reader, byte ReaderStatus, byte Tries) : OsdpEvent;

/// <summary>
/// The reader could not carry out a card command (osdp_XRD). The command it
/// answered completes as <see cref="CompletionStatus.Failed"/>; the session
/// stays open.
/// </summary>
/// <param name="Code">
/// Error condition. LibOSDP recommends the <see cref="TrsErrorCode"/> values,
/// but a reader may send any byte.
/// </param>
public sealed record TrsErrorEvent(byte Code) : OsdpEvent
{
    /// <summary>Reports one of LibOSDP's recommended codes.</summary>
    public TrsErrorEvent(TrsErrorCode code) : this((byte)code) { }

    /// <summary><see cref="Code"/> as a <see cref="TrsErrorCode"/>, or null for a reader's own code.</summary>
    public TrsErrorCode? KnownCode => Enum.IsDefined((TrsErrorCode)Code) ? (TrsErrorCode)Code : null;
}
