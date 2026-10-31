// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// Something LibOSDP itself reports about a link. A CP receives these as
/// <see cref="NotificationEvent"/>s and a PD as <see cref="NotificationCommand"/>s,
/// only when <see cref="OsdpFlags.EnableNotification"/> is set.
/// </summary>
public abstract record OsdpNotification
{
    private protected OsdpNotification() { }
}

/// <summary>The secure channel went up or down.</summary>
/// <param name="Active">Whether the secure channel is now active.</param>
/// <param name="UsingDefaultKey">Whether it runs on the install-mode default key (SCBK-D).</param>
public sealed record SecureChannelStatusNotification(bool Active, bool UsingDefaultKey) : OsdpNotification;

/// <summary>The PD went online or offline.</summary>
/// <param name="Online">Whether the PD is now online.</param>
public sealed record PdStatusNotification(bool Online) : OsdpNotification;

/// <summary>A multipart operation (file transfer, PIV, biometrics) changed state.</summary>
/// <param name="Stage">Start, progress, or done.</param>
/// <param name="MessageType">Which kind of operation.</param>
/// <param name="ObjectId">Operation-specific identifier, e.g. the file ID.</param>
/// <param name="Total">Total bytes.</param>
/// <param name="Offset">Bytes done so far.</param>
/// <param name="Outcome">How it ended; meaningful when <paramref name="Stage"/> is <see cref="NotificationType.MultipartDone"/>.</param>
public sealed record MultipartNotification(NotificationType Stage, MpMsgType MessageType, int ObjectId,
                                           uint Total, uint Offset, MpOutcome Outcome) : OsdpNotification;

/// <summary>The PD reported its identity.</summary>
/// <param name="Id">The reported identity.</param>
public sealed record PdIdNotification(PdId Id) : OsdpNotification;

/// <summary>A transparent-reader (TRS) card session changed state.</summary>
/// <param name="Status">What happened.</param>
/// <param name="Reader">
/// Reader it concerns: on a CP, the session's reader, or the scanned reader for
/// the scan's statuses; on a PD, the reader whose card to release for
/// <see cref="TrsSessionStatus.CardReleased"/>, and 0 otherwise.
/// </param>
/// <param name="Reason">Why the session failed, for <see cref="TrsSessionStatus.Failed"/>.</param>
/// <param name="NakCode">The PD's NAK code, when <paramref name="Reason"/> is <see cref="TrsFailReason.Nak"/>.</param>
public sealed record TrsStatusNotification(TrsSessionStatus Status, byte Reader,
                                           TrsFailReason Reason = TrsFailReason.None,
                                           NakCode NakCode = 0) : OsdpNotification;
