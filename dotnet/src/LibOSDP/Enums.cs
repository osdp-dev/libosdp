// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>Per-PD behaviour flags, set on <see cref="PdInfo.Flags"/>.</summary>
[Flags]
public enum OsdpFlags : uint
{
    /// <summary>No flags.</summary>
    None = 0,
    /// <summary>Only allow communication in a secure channel.</summary>
    EnforceSecure = 0x0001_0000,
    /// <summary>Let a keyless PD bring up the secure channel on SCBK-D so a CP can provision its key.</summary>
    InstallMode = 0x0002_0000,
    /// <summary>Discard unsolicited replies from the PD.</summary>
    IgnoreUnsolicited = 0x0004_0000,
    /// <summary>Deliver library notifications as events (CP) or commands (PD).</summary>
    EnableNotification = 0x0008_0000,
    /// <summary>Capture packets to a PCAP file for offline analysis.</summary>
    CapturePackets = 0x0010_0000,
    /// <summary>Tolerate peers that send an encrypted data block with no payload.</summary>
    AllowEmptyEncryptedDataBlock = 0x0020_0000,
    /// <summary>Deliver biometric read replies through the multipart engine.</summary>
    BioReadrMultipart = 0x0040_0000,
}

/// <summary>Reader LED colours.</summary>
public enum LedColor
{
    /// <summary>Off.</summary>
    Black = 0,
    /// <summary>Red.</summary>
    Red = 1,
    /// <summary>Green.</summary>
    Green = 2,
    /// <summary>Amber.</summary>
    Amber = 3,
    /// <summary>Blue.</summary>
    Blue = 4,
    /// <summary>Magenta.</summary>
    Magenta = 5,
    /// <summary>Cyan.</summary>
    Cyan = 6,
    /// <summary>White.</summary>
    White = 7,
}

/// <summary>What an <see cref="OutputCommand"/> does to the output.</summary>
public enum OutputControlCode
{
    /// <summary>Leave the output as it is.</summary>
    Nop = 0,
    /// <summary>Turn off and cancel any timed operation.</summary>
    PermanentOff = 1,
    /// <summary>Turn on and cancel any timed operation.</summary>
    PermanentOn = 2,
    /// <summary>Turn off once any timed operation completes.</summary>
    PermanentOffAllowTimed = 3,
    /// <summary>Turn on once any timed operation completes.</summary>
    PermanentOnAllowTimed = 4,
    /// <summary>Turn on for <see cref="OutputCommand.TimerCount"/>, then revert.</summary>
    TemporaryOn = 5,
    /// <summary>Turn off for <see cref="OutputCommand.TimerCount"/>, then revert.</summary>
    TemporaryOff = 6,
}

/// <summary>Control code of a <see cref="TemporaryLedParams"/> block.</summary>
public enum TemporaryLedControlCode
{
    /// <summary>Leave the temporary state as it is.</summary>
    Nop = 0,
    /// <summary>Cancel the temporary state and show the permanent one.</summary>
    Cancel = 1,
    /// <summary>Set the temporary state and start its timer.</summary>
    Set = 2,
}

/// <summary>Control code of a <see cref="PermanentLedParams"/> block.</summary>
public enum PermanentLedControlCode
{
    /// <summary>Leave the permanent state as it is.</summary>
    Nop = 0,
    /// <summary>Set the permanent state.</summary>
    Set = 1,
}

/// <summary>What a <see cref="BuzzerCommand"/> does.</summary>
public enum BuzzerControlCode
{
    /// <summary>No tone (deprecated by OSDP; use <see cref="Off"/>).</summary>
    NoTone = 0,
    /// <summary>Off.</summary>
    Off = 1,
    /// <summary>The reader's default tone.</summary>
    DefaultTone = 2,
}

/// <summary>How a <see cref="TextCommand"/> is shown.</summary>
public enum TextControlCode
{
    /// <summary>Permanent text, no wrap.</summary>
    PermanentNoWrap = 1,
    /// <summary>Permanent text with wrap.</summary>
    PermanentWrap = 2,
    /// <summary>Temporary text, no wrap.</summary>
    TemporaryNoWrap = 3,
    /// <summary>Temporary text with wrap.</summary>
    TemporaryWrap = 4,
}

/// <summary>Which set of status bits a report carries.</summary>
public enum StatusReportType
{
    /// <summary>Input (contact) status, one entry per input.</summary>
    Input = 0,
    /// <summary>Output status, one entry per output.</summary>
    Output = 1,
    /// <summary>Local status: tamper and power.</summary>
    Local = 2,
    /// <summary>Reader tamper status, one entry per reader.</summary>
    Reader = 3,
}

/// <summary>Kinds of library notification.</summary>
public enum NotificationType
{
    /// <summary>The secure channel went up or down.</summary>
    SecureChannelStatus = 0,
    /// <summary>The PD went online or offline.</summary>
    PeripheralDeviceStatus = 1,
    /// <summary>A multipart operation started.</summary>
    MultipartStart = 2,
    /// <summary>A multipart operation made progress.</summary>
    MultipartProgress = 3,
    /// <summary>A multipart operation finished.</summary>
    MultipartDone = 4,
    /// <summary>The PD reported its identity.</summary>
    PdId = 5,
    /// <summary>A transparent-reader (TRS) card session changed state.</summary>
    TrsStatus = 6,
}

/// <summary>
/// Life-cycle of a transparent-reader (TRS) card session. A PD is told only
/// <see cref="Opened"/>, <see cref="Closed"/> and <see cref="CardReleased"/>.
/// </summary>
public enum TrsSessionStatus
{
    /// <summary>The reader is in transparent mode; APDUs flow.</summary>
    Opened = 0,
    /// <summary>The session closed (CP), or the CP took the reader out of transparent mode or restarted the link (PD).</summary>
    Closed = 1,
    /// <summary>The PD refused transparent mode, or a link error cut the session short.</summary>
    Failed = 2,
    /// <summary>The reader refused a presence-scan probe; the scan backs off and retries.</summary>
    ScanSuspended = 3,
    /// <summary>The reader cannot answer a card scan, so card removal will not be reported.</summary>
    ScanNoDeparture = 4,
    /// <summary>PD only: the CP ended its transaction with the card; disconnect it.</summary>
    CardReleased = 5,
}

/// <summary>Why a transparent-reader (TRS) card session failed.</summary>
public enum TrsFailReason
{
    /// <summary>Not a failure.</summary>
    None = 0,
    /// <summary>The reader stayed in its default mode.</summary>
    Refused = 1,
    /// <summary>A command of the session was NAKed; see <see cref="TrsStatusNotification.NakCode"/>.</summary>
    Nak = 2,
    /// <summary>A command of the session drew no usable reply.</summary>
    NoReply = 3,
    /// <summary>The PD went offline or was disabled.</summary>
    LinkDown = 4,
}

/// <summary>How a reader encodes the PIN digits it writes into a C-APDU.</summary>
public enum TrsPinFormat
{
    /// <summary>Digits as binary values.</summary>
    Binary = 1,
    /// <summary>Digits as packed BCD.</summary>
    Bcd = 2,
    /// <summary>Digits as ASCII characters.</summary>
    Ascii = 3,
}

/// <summary>What ends a secure PIN entry; combine with |.</summary>
[Flags]
public enum TrsPinComplete
{
    /// <summary>No condition.</summary>
    None = 0,
    /// <summary>The maximum number of digits was entered.</summary>
    MaxDigits = 1 << 0,
    /// <summary>The validation (enter) key was pressed.</summary>
    Key = 1 << 1,
    /// <summary>Entry timed out.</summary>
    Timeout = 1 << 2,
}

/// <summary>How a smart card talks to the reader.</summary>
public enum TrsCardProtocol
{
    /// <summary>ISO 7816 contact (T=0/T=1).</summary>
    Contact = 1,
    /// <summary>ISO 14443 A/B contactless.</summary>
    Contactless = 2,
}

/// <summary>Whether a smart card is in the reader's field, and on which interface.</summary>
public enum TrsCardStatus
{
    /// <summary>No presence has been reported.</summary>
    Unknown = 0,
    /// <summary>No card.</summary>
    NotPresent = 1,
    /// <summary>A card is present; interface not specified.</summary>
    Present = 2,
    /// <summary>A card is present on the contactless interface.</summary>
    PresentContactless = 3,
    /// <summary>A card is present on the contact interface.</summary>
    PresentContact = 4,
}

/// <summary>Kinds of multipart operation.</summary>
public enum MpMsgType
{
    /// <summary>File transfer.</summary>
    FileTransfer = 1,
    /// <summary>PIV data read.</summary>
    PivData = 2,
    /// <summary>General authenticate.</summary>
    GenAuth = 3,
    /// <summary>Challenge/response authenticate.</summary>
    CrAuth = 4,
    /// <summary>Biometric read.</summary>
    BioRead = 5,
}

/// <summary>How a multipart operation ended.</summary>
public enum MpOutcome
{
    /// <summary>Completed.</summary>
    Ok = 0,
    /// <summary>Completed; the PD is rebooting to apply it.</summary>
    OkRebooting = 1,
    /// <summary>Aborted.</summary>
    Aborted = 2,
    /// <summary>The PD did not recognise the content.</summary>
    Unrecognized = 3,
    /// <summary>The content was invalid.</summary>
    Invalid = 4,
}

/// <summary>How a submitted command or event settled.</summary>
public enum CompletionStatus
{
    /// <summary>Completed: the PD answered (CP), or the reply left the PD (PD).</summary>
    Ok = 0,
    /// <summary>Transport or protocol failure, or the peer refused it.</summary>
    Failed = 1,
    /// <summary>Removed by a flush.</summary>
    Flushed = 2,
    /// <summary>Removed because the device was disposed.</summary>
    Aborted = 3,
}

/// <summary>Biometric types.</summary>
public enum BioType
{
    /// <summary>Not specified.</summary>
    NotSpecified = 0,
    /// <summary>Right thumb print.</summary>
    RightThumbPrint = 1,
    /// <summary>Right index finger print.</summary>
    RightIndexFingerPrint = 2,
    /// <summary>Right middle finger print.</summary>
    RightMiddleFingerPrint = 3,
    /// <summary>Right ring finger print.</summary>
    RightRingFingerPrint = 4,
    /// <summary>Right little finger print.</summary>
    RightLittleFingerPrint = 5,
    /// <summary>Left thumb print.</summary>
    LeftThumbPrint = 6,
    /// <summary>Left index finger print.</summary>
    LeftIndexFingerPrint = 7,
    /// <summary>Left middle finger print.</summary>
    LeftMiddleFingerPrint = 8,
    /// <summary>Left ring finger print.</summary>
    LeftRingFingerPrint = 9,
    /// <summary>Left little finger print.</summary>
    LeftLittleFingerPrint = 10,
    /// <summary>Right iris scan.</summary>
    RightIrisScan = 11,
    /// <summary>Right retina scan.</summary>
    RightRetinaScan = 12,
    /// <summary>Left iris scan.</summary>
    LeftIrisScan = 13,
    /// <summary>Left retina scan.</summary>
    LeftRetinaScan = 14,
    /// <summary>Full face image.</summary>
    FullFaceImage = 15,
    /// <summary>Right hand geometry.</summary>
    RightHandGeometry = 16,
    /// <summary>Left hand geometry.</summary>
    LeftHandGeometry = 17,
}

/// <summary>Biometric template formats.</summary>
public enum BioFormat
{
    /// <summary>Not specified.</summary>
    NotSpecified = 0,
    /// <summary>Raw PGM image.</summary>
    RawPgm = 1,
    /// <summary>ANSI/INCITS 378 fingerprint template.</summary>
    AnsiIncits378 = 2,
}

/// <summary>Outcome of a biometric operation.</summary>
public enum BioStatus
{
    /// <summary>Success.</summary>
    Success = 0x00,
    /// <summary>Timed out.</summary>
    Timeout = 0x01,
    /// <summary>Unknown error.</summary>
    UnknownError = 0xFF,
}

/// <summary>NAK reasons a PD application may give through <see cref="CommandReply.Reject"/>.</summary>
public enum NakCode
{
    /// <summary>The requested biometric type is not supported.</summary>
    BioType = 7,
    /// <summary>The requested biometric format is not supported.</summary>
    BioFormat = 8,
    /// <summary>The command could not be processed.</summary>
    Record = 9,
}

/// <summary>Card data formats.</summary>
public enum CardFormat
{
    /// <summary>Raw bits, format unspecified.</summary>
    Unspecified = 0,
    /// <summary>Raw Wiegand bits.</summary>
    Wiegand = 1,
    /// <summary>
    /// ASCII characters (osdp_FMT). Not supported by LibOSDP: a CP ignores
    /// osdp_FMT replies, and submitting a card read in this format throws
    /// <see cref="ArgumentException"/>.
    /// </summary>
    Ascii = 2,
}

/// <summary>PD capability function codes (OSDP osdp_PDCAP).</summary>
public enum Capability
{
    /// <summary>Unused.</summary>
    Unused = 0,
    /// <summary>Contact status monitoring (inputs).</summary>
    ContactStatusMonitoring = 1,
    /// <summary>Output control.</summary>
    OutputControl = 2,
    /// <summary>Card data format.</summary>
    CardDataFormat = 3,
    /// <summary>Reader LED control.</summary>
    LedControl = 4,
    /// <summary>Reader audible output.</summary>
    AudibleControl = 5,
    /// <summary>Reader text output.</summary>
    TextOutput = 6,
    /// <summary>Time keeping.</summary>
    TimeKeeping = 7,
    /// <summary>Check character support.</summary>
    CheckCharacter = 8,
    /// <summary>Communication security.</summary>
    CommunicationSecurity = 9,
    /// <summary>Receive buffer size.</summary>
    ReceiveBufferSize = 10,
    /// <summary>Largest combined message size.</summary>
    CombinedMessageSize = 11,
    /// <summary>Smart card support.</summary>
    SmartCard = 12,
    /// <summary>Number of attached readers.</summary>
    Readers = 13,
    /// <summary>Biometrics.</summary>
    Biometrics = 14,
    /// <summary>Secure PIN entry.</summary>
    SecurePinEntry = 15,
    /// <summary>OSDP version.</summary>
    OsdpVersion = 16,
}
