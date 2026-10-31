// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Interop;

internal static unsafe class NotificationMarshal
{
    /// Null for a notification kind this binding does not know.
    internal static OsdpNotification? FromNative(NativeNotification* n) => (NotificationType)n->Type switch
    {
        NotificationType.SecureChannelStatus =>
            new SecureChannelStatusNotification(n->Data.ScStatus.Active != 0, n->Data.ScStatus.ScbkD != 0),
        NotificationType.PeripheralDeviceStatus =>
            new PdStatusNotification(n->Data.PdOnline != 0),
        NotificationType.MultipartStart or NotificationType.MultipartProgress or NotificationType.MultipartDone =>
            new MultipartNotification((NotificationType)n->Type, (MpMsgType)n->Data.Mp.MpType,
                n->Data.Mp.ObjectId, n->Data.Mp.Total, n->Data.Mp.Offset, (MpOutcome)n->Data.Mp.Outcome),
        NotificationType.PdId =>
            new PdIdNotification(PdId.FromNative(n->Data.PdId)),
        NotificationType.TrsStatus =>
            new TrsStatusNotification((TrsSessionStatus)n->Data.TrsStatus.Status, n->Data.TrsStatus.Reader,
                (TrsFailReason)n->Data.TrsStatus.Reason, (NakCode)n->Data.TrsStatus.NakCode),
        _ => null,
    };
}
