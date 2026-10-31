// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// A command a control panel sends to a peripheral device. Values are
/// validated when the command is submitted.
/// </summary>
public abstract record OsdpCommand
{
    private protected OsdpCommand() { }
}

/// <summary>Drive an output (osdp_OUT).</summary>
/// <param name="OutputNo">Output number, from 0.</param>
/// <param name="ControlCode">What to do.</param>
/// <param name="TimerCount">Duration of a temporary state, in units of 100 ms.</param>
public sealed record OutputCommand(byte OutputNo, OutputControlCode ControlCode, ushort TimerCount = 0) : OsdpCommand;

/// <summary>A temporary LED state that runs for a while and then reverts.</summary>
/// <param name="ControlCode">Set or cancel the temporary state.</param>
/// <param name="OnCount">On time, in units of 100 ms.</param>
/// <param name="OffCount">Off time, in units of 100 ms.</param>
/// <param name="OnColor">Colour during the on time.</param>
/// <param name="OffColor">Colour during the off time.</param>
/// <param name="TimerCount">How long the temporary state lasts, in units of 100 ms.</param>
public sealed record TemporaryLedParams(TemporaryLedControlCode ControlCode, byte OnCount, byte OffCount,
                                        LedColor OnColor, LedColor OffColor, ushort TimerCount);

/// <summary>A permanent LED state that holds until changed.</summary>
/// <param name="ControlCode">Set the permanent state.</param>
/// <param name="OnCount">On time, in units of 100 ms.</param>
/// <param name="OffCount">Off time, in units of 100 ms.</param>
/// <param name="OnColor">Colour during the on time.</param>
/// <param name="OffColor">Colour during the off time.</param>
public sealed record PermanentLedParams(PermanentLedControlCode ControlCode, byte OnCount, byte OffCount,
                                        LedColor OnColor, LedColor OffColor);

/// <summary>Drive a reader LED (osdp_LED). A null block leaves that state unchanged.</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="LedNumber">LED number on that reader.</param>
/// <param name="Temporary">Temporary state, or null for no change.</param>
/// <param name="Permanent">Permanent state, or null for no change.</param>
public sealed record LedCommand(byte Reader, byte LedNumber, TemporaryLedParams? Temporary = null,
                                PermanentLedParams? Permanent = null) : OsdpCommand;

/// <summary>Drive a reader buzzer (osdp_BUZ).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="ControlCode">Tone.</param>
/// <param name="OnCount">On time, in units of 100 ms.</param>
/// <param name="OffCount">Off time, in units of 100 ms.</param>
/// <param name="RepCount">Repetitions; 0 repeats until changed.</param>
public sealed record BuzzerCommand(byte Reader, BuzzerControlCode ControlCode, byte OnCount, byte OffCount,
                                   byte RepCount) : OsdpCommand;

/// <summary>Show text on a reader display (osdp_TEXT).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="ControlCode">Permanent or temporary, wrapped or not.</param>
/// <param name="TempTime">Duration of temporary text, in seconds.</param>
/// <param name="OffsetRow">Starting row, from 1.</param>
/// <param name="OffsetCol">Starting column, from 1.</param>
/// <param name="Data">ASCII text, at most 32 characters.</param>
public sealed record TextCommand(byte Reader, TextControlCode ControlCode, byte TempTime, byte OffsetRow,
                                 byte OffsetCol, string Data) : OsdpCommand;

/// <summary>Set a new secure channel base key (osdp_KEYSET). Requires an active secure channel.</summary>
/// <param name="Type">Key type; 1 is the SCBK.</param>
/// <param name="Data">The key, at most 32 bytes (16 for an SCBK).</param>
public sealed record KeysetCommand(byte Type, byte[] Data) : OsdpCommand;

/// <summary>Change the PD's address and baud rate (osdp_COMSET).</summary>
/// <param name="Address">New address.</param>
/// <param name="BaudRate">New baud rate.</param>
public sealed record ComsetCommand(byte Address, uint BaudRate) : OsdpCommand;

/// <summary>Delivered to a PD after a <see cref="ComsetCommand"/> has been answered.</summary>
/// <param name="Address">Address now in use.</param>
/// <param name="BaudRate">Baud rate now in use.</param>
public sealed record ComsetDoneCommand(byte Address, uint BaudRate) : OsdpCommand;

/// <summary>Manufacturer-specific command (osdp_MFG).</summary>
/// <param name="VendorCode">IEEE OUI of the vendor (low 24 bits).</param>
/// <param name="Data">Payload, at most 64 bytes.</param>
public sealed record ManufacturerCommand(uint VendorCode, byte[] Data) : OsdpCommand;

/// <summary>
/// Send a file to the PD (osdp_FILETRANSFER). Both sides need file operations
/// registered first; the command completes when the transfer ends.
/// </summary>
/// <param name="FileId">Pre-agreed file identifier.</param>
/// <param name="Flags">Transfer flags; 0 for none.</param>
public sealed record FileTransferCommand(int FileId, uint Flags = 0) : OsdpCommand;

/// <summary>
/// Ask the PD for a status report (osdp_ISTAT, OSTAT, LSTAT, RSTAT). A PD answers
/// with <see cref="CommandReply.WithAnswer"/> carrying the filled report.
/// </summary>
/// <param name="Type">Which report.</param>
/// <param name="Report">Empty in a query; one byte per entry in an answer, at most 64.</param>
public sealed record StatusCommand(StatusReportType Type, byte[] Report) : OsdpCommand;

/// <summary>A library notification delivered to a PD application. Cannot be submitted.</summary>
/// <param name="Notification">What happened.</param>
public sealed record NotificationCommand(OsdpNotification Notification) : OsdpCommand;

/// <summary>Request a biometric read (osdp_BIOREAD).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Type">Biometric type.</param>
/// <param name="Format">Template format.</param>
/// <param name="Quality">Minimum quality, 0-100.</param>
public sealed record BioReadCommand(byte Reader, BioType Type, BioFormat Format, byte Quality) : OsdpCommand;

/// <summary>Request a biometric match against a template (osdp_BIOMATCH).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Type">Biometric type.</param>
/// <param name="Format">Template format.</param>
/// <param name="Quality">Minimum quality, 0-100.</param>
/// <param name="Template">Template to match, at most 128 bytes.</param>
public sealed record BioMatchCommand(byte Reader, BioType Type, BioFormat Format, byte Quality,
                                     byte[] Template) : OsdpCommand;

/// <summary>Set the PD's clock (osdp_TDSET).</summary>
/// <param name="Year">Year.</param>
/// <param name="Month">Month, 1-12.</param>
/// <param name="Day">Day, 1-31.</param>
/// <param name="Hour">Hour, 0-23.</param>
/// <param name="Minute">Minute, 0-59.</param>
/// <param name="Second">Second, 0-59.</param>
public sealed record TdSetCommand(ushort Year, byte Month, byte Day, byte Hour, byte Minute,
                                  byte Second) : OsdpCommand;

/// <summary>Read a PIV data object from a card (osdp_PIVDATA).</summary>
/// <param name="ObjectId">Three-byte PIV object identifier.</param>
/// <param name="Element">Element within the object.</param>
/// <param name="Offset">Data offset within the element.</param>
public sealed record PivDataCommand(byte[] ObjectId, byte Element, byte Offset) : OsdpCommand;

/// <summary>PIV general authenticate (osdp_GENAUTH).</summary>
/// <param name="Algorithm">Algorithm reference.</param>
/// <param name="Key">Key reference.</param>
/// <param name="Data">Authentication template, at most 256 bytes.</param>
public sealed record GenAuthCommand(byte Algorithm, byte Key, byte[] Data) : OsdpCommand;

/// <summary>PIV challenge/response authenticate (osdp_CRAUTH).</summary>
/// <param name="Algorithm">Algorithm reference.</param>
/// <param name="Key">Key reference.</param>
/// <param name="Data">Challenge, at most 256 bytes.</param>
public sealed record CrAuthCommand(byte Algorithm, byte Key, byte[] Data) : OsdpCommand;

/// <summary>
/// Open a transparent-reader (TRS) card session, a band: the commands up to
/// the matching <see cref="TrsStopCommand"/>. LibOSDP puts the reader in
/// transparent mode; inside a band only TRS commands are accepted, and every
/// one must name this reader.
/// </summary>
/// <param name="Reader">Reader the session talks to, from 0.</param>
public sealed record TrsStartCommand(byte Reader = 0) : OsdpCommand;

/// <summary>Close the card session; LibOSDP releases the card and restores the reader.</summary>
/// <param name="Reader">The reader the band's <see cref="TrsStartCommand"/> named.</param>
public sealed record TrsStopCommand(byte Reader = 0) : OsdpCommand;

/// <summary>
/// Send a C-APDU to the card, inside a band. The card's answer arrives as a
/// <see cref="TrsCardDataEvent"/> before this command completes.
/// </summary>
/// <param name="Reader">The reader the band's <see cref="TrsStartCommand"/> named.</param>
/// <param name="Apdu">The C-APDU; see <see cref="ControlPanel.GetTrsMaxApduLength"/> for how much fits.</param>
public sealed record TrsSendApduCommand(byte Reader, byte[] Apdu) : OsdpCommand;

/// <summary>Where, and how, a reader writes the entered PIN into the C-APDU.</summary>
/// <param name="Format">Digit encoding.</param>
/// <param name="RightJustify">Right-justify the PIN within the block.</param>
/// <param name="OffsetBits">Block position in the APDU payload, in bits.</param>
/// <param name="SizeBytes">Block size in bytes, as defined by the card scheme (8 for EMV).</param>
public sealed record TrsPinBlock(TrsPinFormat Format, bool RightJustify, ushort OffsetBits, byte SizeBytes);

/// <summary>Where a reader records how many digits were entered.</summary>
/// <param name="SizeBits">Field size in bits; 0 when the APDU has no such field.</param>
/// <param name="OffsetBits">Field position in the APDU payload, in bits.</param>
public sealed record TrsPinLengthField(byte SizeBits, ushort OffsetBits);

/// <summary>
/// Have the reader prompt for a PIN, insert it into <paramref name="Apdu"/>
/// and send that to the card, inside a band. The answer arrives as a
/// <see cref="TrsPinCompleteEvent"/>. Positions the wire cannot express are
/// refused at submission.
/// </summary>
/// <param name="Reader">The reader the band's <see cref="TrsStartCommand"/> named.</param>
/// <param name="TimeoutInitial">Seconds to wait for the first digit; 0 for the reader's default.</param>
/// <param name="TimeoutDigit">Seconds to wait for each further digit.</param>
/// <param name="PinBlock">Where and how the PIN goes into the APDU.</param>
/// <param name="PinLengthField">Where the number of digits goes into the APDU.</param>
/// <param name="MinDigits">Shortest PIN accepted.</param>
/// <param name="MaxDigits">Longest PIN accepted.</param>
/// <param name="CompleteOn">What ends the entry.</param>
/// <param name="NumMessages">Number of display messages.</param>
/// <param name="LanguageId">Display language.</param>
/// <param name="MsgIndex">Index of the message to display.</param>
/// <param name="TeoPrologue">T=1 protocol prologue; exactly 3 bytes.</param>
/// <param name="Apdu">The C-APDU the PIN is written into.</param>
public sealed record TrsEnterPinCommand(byte Reader, byte TimeoutInitial, byte TimeoutDigit, TrsPinBlock PinBlock,
                                        TrsPinLengthField PinLengthField, byte MinDigits, byte MaxDigits,
                                        TrsPinComplete CompleteOn, byte NumMessages, ushort LanguageId,
                                        byte MsgIndex, byte[] TeoPrologue, byte[] Apdu) : OsdpCommand;

/// <summary>
/// Ask the reader whether a card is in its field, inside a band. The answer
/// arrives as a <see cref="TrsCardPresentEvent"/>. For a scan that runs on
/// its own between bands, see <see cref="ControlPanel.TrsScanEnable"/>.
/// </summary>
/// <param name="Reader">The reader the band's <see cref="TrsStartCommand"/> named.</param>
public sealed record TrsCardScanCommand(byte Reader = 0) : OsdpCommand;
