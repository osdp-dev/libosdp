// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Interop;

namespace LibOSDP.Tests;

public sealed unsafe class EventMarshalTests
{
    public static TheoryData<OsdpEvent> Events => new()
    {
        new CardReadEvent(1, CardFormat.Wiegand, [0x12, 0x34, 0x56, 0x40], Bits: 26),
        new CardReadEvent(0, CardFormat.Unspecified, [0x55, 0xAA], Bits: 16),
        new KeyPressEvent(1, [9, 1, 9, 2]),
        new ManufacturerReplyEvent(0x153, [0x10, 9, 1]),
        new ManufacturerStatusEvent([0xDE, 0xAD]),
        new ManufacturerErrorEvent([]),
        new BioReadEvent(0, BioStatus.Success, BioType.RightThumbPrint, 80, [1, 2, 3]),
        new BioMatchEvent(0, BioStatus.Success, 90),
        new StatusEvent(StatusReportType.Input, [1, 0, 1, 0]),
        new PivDataEvent([1, 2, 3]),
        new GenAuthEvent([4, 5]),
        new CrAuthEvent([6]),
        new TrsCardInfoEvent(1, TrsCardProtocol.Contactless, [0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66],
                             [0x06, 0x77, 0x77, 0x71, 0x02, 0x80]),
        new TrsCardPresentEvent(1, TrsCardStatus.PresentContact),
        new TrsCardDataEvent(1, 0, [0x6F, 0x1A, 0x84, 0x0E, 0x90, 0x00]),
        new TrsPinCompleteEvent(1, 0, 2),
        new TrsErrorEvent(0x02),
    };

    [Theory(DisableDiscoveryEnumeration = true)]
    [MemberData(nameof(Events))]
    public void EventSurvivesNativeRoundTrip(OsdpEvent ev)
    {
        NativeEvent native = default;
        EventMarshal.ToNative(ev, &native);
        OsdpEvent? back = EventMarshal.FromNative(&native);
        Assert.NotNull(back);
        Assert.IsType(ev.GetType(), back);
        Assert.Equivalent(ev, back, strict: true);
    }

    [Fact]
    public void RawCardWithoutBitsUsesEveryByte()
    {
        NativeEvent native = default;
        EventMarshal.ToNative(new CardReadEvent(0, CardFormat.Wiegand, [0xFF, 0x01]), &native);
        Assert.Equal(16, native.U.CardRead.Length);
    }

    public static TheoryData<OsdpEvent, string> Invalid => new()
    {
        { new CardReadEvent(0, CardFormat.Wiegand, [1, 2, 3], Bits: 26), "Bits" },
        { new CardReadEvent(0, CardFormat.Ascii, "1234"u8.ToArray()), "Format" },
        { new CardReadEvent(0, CardFormat.Wiegand, new byte[65]), "Data" },
        { new KeyPressEvent(0, new byte[65]), "Data" },
        { new ManufacturerReplyEvent(0, new byte[129]), "Data" },
        { new BioReadEvent(0, BioStatus.Success, BioType.NotSpecified, 0, new byte[257]), "Template" },
        { new StatusEvent(StatusReportType.Output, new byte[65]), "Report" },
        { new PivDataEvent(new byte[257]), "Data" },
        { new NotificationEvent(new PdStatusNotification(true)), "ev" },
    };

    [Theory(DisableDiscoveryEnumeration = true)]
    [MemberData(nameof(Invalid))]
    public void InvalidEventIsRejectedBeforeReachingNative(OsdpEvent ev, string parameter)
    {
        var error = Assert.Throws<ArgumentException>(() =>
        {
            NativeEvent native = default;
            EventMarshal.ToNative(ev, &native);
        });
        Assert.Equal(parameter, error.ParamName);
    }

    [Fact]
    public void NullPayloadIsRejectedByName()
    {
        var error = Assert.Throws<ArgumentNullException>(() =>
        {
            NativeEvent native = default;
            EventMarshal.ToNative(new KeyPressEvent(0, null!), &native);
        });
        Assert.Equal("Data", error.ParamName);
    }

    [Fact]
    public void RejectedEventLeavesDestinationUntouched()
    {
        Assert.Throws<ArgumentException>(() =>
        {
            NativeEvent native = default;
            native.Type = NativeEventId.KeyPress;
            native.U.KeyPress.Data[0] = 0x7E;
            try
            {
                EventMarshal.ToNative(new CardReadEvent(0, CardFormat.Wiegand, [1, 2, 3], Bits: 26), &native);
            }
            finally
            {
                Assert.Equal(NativeEventId.KeyPress, native.Type);
                Assert.Equal(0x7E, native.U.KeyPress.Data[0]);
            }
        });
    }

    [Fact]
    public void ReceivedAsciiCardCountsBytesNotBits()
    {
        NativeEvent native = default;
        native.Type = NativeEventId.CardRead;
        native.U.CardRead.Format = (int)CardFormat.Ascii;
        native.U.CardRead.Length = 4;
        "1234"u8.CopyTo(new Span<byte>(native.U.CardRead.Data, 4));
        Assert.Equivalent(new CardReadEvent(0, CardFormat.Ascii, "1234"u8.ToArray()),
                          EventMarshal.FromNative(&native), strict: true);
    }

    [Fact]
    public void CardLengthFromNativeIsClampedToTheBuffer()
    {
        NativeEvent native = default;
        native.Type = NativeEventId.CardRead;
        native.U.CardRead.Format = (int)CardFormat.Wiegand;
        native.U.CardRead.Length = ushort.MaxValue;
        var card = Assert.IsType<CardReadEvent>(EventMarshal.FromNative(&native));
        Assert.Equal(NativeLimits.EventCardreadMaxDataLen, card.Data.Length);
    }

    [Fact]
    public void MultipartNotificationArrivesAsEvent()
    {
        NativeEvent native = default;
        native.Type = NativeEventId.Notification;
        native.U.Notif.Type = (int)NotificationType.MultipartDone;
        native.U.Notif.Data.Mp = new NativeMpNotification
        {
            MpType = (int)MpMsgType.FileTransfer, ObjectId = 3, Total = 100, Offset = 100,
            Outcome = (int)MpOutcome.Ok,
        };
        var ev = Assert.IsType<NotificationEvent>(EventMarshal.FromNative(&native));
        Assert.Equal(new MultipartNotification(NotificationType.MultipartDone, MpMsgType.FileTransfer,
                                               3, 100, 100, MpOutcome.Ok), ev.Notification);
    }

    [Fact]
    public void UnknownEventIsIgnored()
    {
        NativeEvent native = default;
        native.Type = 99;
        Assert.Null(EventMarshal.FromNative(&native));
    }
}
