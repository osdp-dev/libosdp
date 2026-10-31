// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Interop;

namespace LibOSDP.Tests;

public sealed unsafe class CommandMarshalTests
{
    public static TheoryData<OsdpCommand> Commands => new()
    {
        new OutputCommand(1, OutputControlCode.TemporaryOn, 300),
        new LedCommand(0, 1,
            new TemporaryLedParams(TemporaryLedControlCode.Set, 10, 10, LedColor.Red, LedColor.Black, 10),
            new PermanentLedParams(PermanentLedControlCode.Set, 1, 0, LedColor.Green, LedColor.Black)),
        new LedCommand(0, 0, Temporary: new TemporaryLedParams(TemporaryLedControlCode.Set, 3, 3, LedColor.Amber, LedColor.Black, 5)),
        new LedCommand(0, 0, Permanent: new PermanentLedParams(PermanentLedControlCode.Set, 1, 0, LedColor.Blue, LedColor.Black)),
        new BuzzerCommand(0, BuzzerControlCode.DefaultTone, 2, 2, 3),
        new TextCommand(0, TextControlCode.PermanentNoWrap, 20, 1, 1, "LIBOSDP"),
        new KeysetCommand(1, Enumerable.Range(0, 16).Select(i => (byte)i).ToArray()),
        new ComsetCommand(101, 115200),
        new ComsetDoneCommand(101, 115200),
        new ManufacturerCommand(0x030201, [9, 1, 9, 2, 6, 3]),
        new FileTransferCommand(7),
        new StatusCommand(StatusReportType.Input, [1, 0, 1]),
        new StatusCommand(StatusReportType.Local, []),
        new BioReadCommand(0, BioType.RightIndexFingerPrint, BioFormat.AnsiIncits378, 50),
        new BioMatchCommand(0, BioType.LeftThumbPrint, BioFormat.RawPgm, 60, [1, 2, 3]),
        new TdSetCommand(2026, 10, 5, 12, 30, 45),
        new PivDataCommand([0x5F, 0xC1, 0x02], 1, 0),
        new GenAuthCommand(0x07, 0x9A, [1, 2, 3, 4]),
        new CrAuthCommand(0x07, 0x9A, [5, 6, 7, 8]),
        new TrsStartCommand(1),
        new TrsStopCommand(1),
        new TrsSendApduCommand(1, [0x00, 0xA4, 0x04, 0x00, 0x02, 0x3F, 0x00]),
        new TrsEnterPinCommand(1, 30, 5, new TrsPinBlock(TrsPinFormat.Bcd, true, 40, 8),
                               new TrsPinLengthField(4, 4), 4, 12, TrsPinComplete.Key | TrsPinComplete.MaxDigits,
                               1, 0x0409, 2, [0x00, 0x40, 0x0D], [0x00, 0x20, 0x00, 0x80, 0x08, 1, 2, 3, 4, 5, 6, 7, 8]),
        new TrsCardScanCommand(1),
    };

    [Theory(DisableDiscoveryEnumeration = true)]
    [MemberData(nameof(Commands))]
    public void CommandSurvivesNativeRoundTrip(OsdpCommand command)
    {
        NativeCmd native = default;
        CommandMarshal.ToNative(command, &native);
        OsdpCommand? back = CommandMarshal.FromNative(&native);
        Assert.NotNull(back);
        Assert.IsType(command.GetType(), back);
        Assert.Equivalent(command, back, strict: true);
    }

    [Fact]
    public void MarshallingNeverTouchesTheQueueNode()
    {
        NativeCmd native = default;
        native.Node.Next = 0x1234;
        native.Flags = 0x55;
        CommandMarshal.ToNative(new BuzzerCommand(0, BuzzerControlCode.Off, 0, 0, 0), &native);
        Assert.Equal(0x1234, native.Node.Next);
        Assert.Equal(0x55u, native.Flags);
    }

    public static TheoryData<OsdpCommand, string> Invalid => new()
    {
        { new TextCommand(0, TextControlCode.PermanentNoWrap, 0, 1, 1, new string('x', 33)), "Data" },
        { new TextCommand(0, TextControlCode.PermanentNoWrap, 0, 1, 1, "café"), "Data" },
        { new KeysetCommand(1, new byte[33]), "Data" },
        { new ManufacturerCommand(1, new byte[65]), "Data" },
        { new StatusCommand(StatusReportType.Input, new byte[65]), "Report" },
        { new BioMatchCommand(0, BioType.NotSpecified, BioFormat.NotSpecified, 0, new byte[129]), "Template" },
        { new PivDataCommand([1, 2], 0, 0), "ObjectId" },
        { new GenAuthCommand(0, 0, new byte[257]), "Data" },
        { new NotificationCommand(new PdStatusNotification(true)), "command" },
        { new LedCommand(0, 0, Temporary: new TemporaryLedParams((TemporaryLedControlCode)258, 1, 1, LedColor.Red, LedColor.Black, 1)), "ControlCode" },
        { new LedCommand(0, 0, Permanent: new PermanentLedParams((PermanentLedControlCode)257, 1, 1, LedColor.Red, LedColor.Black)), "ControlCode" },
    };

    [Theory(DisableDiscoveryEnumeration = true)]
    [MemberData(nameof(Invalid))]
    public void InvalidCommandIsRejectedBeforeReachingNative(OsdpCommand command, string parameter)
    {
        var error = Assert.Throws<ArgumentException>(() =>
        {
            NativeCmd native = default;
            CommandMarshal.ToNative(command, &native);
        });
        Assert.Equal(parameter, error.ParamName);
    }

    [Fact]
    public void NullPayloadIsRejectedByName()
    {
        var error = Assert.Throws<ArgumentNullException>(() =>
        {
            NativeCmd native = default;
            CommandMarshal.ToNative(new KeysetCommand(1, null!), &native);
        });
        Assert.Equal("Data", error.ParamName);
    }

    [Fact]
    public void RejectedCommandLeavesDestinationUntouched()
    {
        NativeCmd native = default;
        native.Id = NativeCmdId.Buzzer;
        native.U.Buzzer.RepCount = 0x42;
        NativeCmd* p = &native;
        Assert.Throws<ArgumentException>(() => CommandMarshal.ToNative(new PivDataCommand([1, 2], 0, 0), p));
        Assert.Equal(NativeCmdId.Buzzer, native.Id);
        Assert.Equal(0x42, native.U.Buzzer.RepCount);
    }

    [Fact]
    public void LengthFromNativeIsClampedToTheBuffer()
    {
        NativeCmd native = default;
        native.Id = NativeCmdId.Text;
        native.U.Text.Length = 200;
        var text = Assert.IsType<TextCommand>(CommandMarshal.FromNative(&native));
        Assert.Equal(NativeLimits.CmdTextMaxLen, text.Data.Length);
    }

    [Fact]
    public void LedBlockWithNopControlCodeComesBackAsAbsent()
    {
        NativeCmd native = default;
        CommandMarshal.ToNative(new LedCommand(0, 0), &native);
        var led = Assert.IsType<LedCommand>(CommandMarshal.FromNative(&native));
        Assert.Null(led.Temporary);
        Assert.Null(led.Permanent);
    }

    [Fact]
    public void NotificationArrivesAsCommand()
    {
        NativeCmd native = default;
        native.Id = NativeCmdId.Notification;
        native.U.Notif.Type = (int)NotificationType.SecureChannelStatus;
        native.U.Notif.Data.ScStatus.Active = 1;
        native.U.Notif.Data.ScStatus.ScbkD = 1;
        var cmd = Assert.IsType<NotificationCommand>(CommandMarshal.FromNative(&native));
        Assert.Equal(new SecureChannelStatusNotification(true, true), cmd.Notification);
    }

    [Fact]
    public void UnknownCommandIsIgnored()
    {
        NativeCmd native = default;
        native.Id = 99;
        Assert.Null(CommandMarshal.FromNative(&native));
    }
}
