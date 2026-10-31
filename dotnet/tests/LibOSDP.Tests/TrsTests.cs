// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Tests;

/// Transparent Reader Support across a live CP/PD link: a card session on the
/// PD's second reader, from START through an APDU exchange to STOP.
public sealed class TrsTests
{
    private static readonly byte[] CApdu = [0x00, 0xA4, 0x04, 0x00, 0x02, 0x3F, 0x00];
    private static readonly byte[] RApdu = [0x6F, 0x1A, 0x84, 0x0E, 0x90, 0x00];

    public static TheoryData<Transport, bool> AllLinks => E2E.AllLinks();

    [Theory]
    [MemberData(nameof(AllLinks))]
    public async Task BandCarriesAnApduToTheCardAndItsAnswerBack(Transport transport, bool secure)
    {
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure, OsdpFlags.EnableNotification));
        bus.Reply = command =>
        {
            // Answered from inside the handler: the reply goes out at once.
            if (command is TrsSendApduCommand send)
                _ = bus.Pd.SubmitEventAsync(new TrsCardDataEvent(send.Reader, 0, RApdu));
            return CommandReply.Ack;
        };
        Task<OsdpEvent> answer = bus.NextEventAsync(e => e is TrsCardDataEvent);
        Task<NotificationCommand> released = bus.NextCommandAsync<NotificationCommand>(
            n => n.Notification is TrsStatusNotification { Status: TrsSessionStatus.CardReleased });

        var send = new TrsSendApduCommand(1, CApdu);
        Task<CompletionStatus> start = bus.Cp.SubmitCommandAsync(Bus.Address, new TrsStartCommand(1));
        Task<CompletionStatus> sent = bus.Cp.SubmitCommandAsync(Bus.Address, send);
        Task<CompletionStatus> stop = bus.Cp.SubmitCommandAsync(Bus.Address, new TrsStopCommand(1));

        Assert.Equivalent(send, await bus.NextCommandAsync<TrsSendApduCommand>(), strict: true);
        Assert.Equivalent(new TrsCardDataEvent(1, 0, RApdu), await E2E.Settled(answer), strict: true);
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(start));
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(sent));
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(stop));
        Assert.Equal(new TrsStatusNotification(TrsSessionStatus.CardReleased, 1),
                     (await E2E.Settled(released)).Notification);
    }

    [Fact]
    public async Task ReaderErrorFailsTheExchangeButNotTheSession()
    {
        await using var bus = await Bus.StartAsync(E2E.Options(Transport.Loopback, false));
        bus.Reply = command =>
        {
            if (command is TrsSendApduCommand)
                _ = bus.Pd.SubmitEventAsync(new TrsErrorEvent(0x02));
            return CommandReply.Ack;
        };
        Task<OsdpEvent> error = bus.NextEventAsync(e => e is TrsErrorEvent);

        _ = bus.Cp.SubmitCommandAsync(Bus.Address, new TrsStartCommand());
        Task<CompletionStatus> sent = bus.Cp.SubmitCommandAsync(Bus.Address, new TrsSendApduCommand(0, CApdu));
        Task<CompletionStatus> stop = bus.Cp.SubmitCommandAsync(Bus.Address, new TrsStopCommand());

        Assert.Equal(new TrsErrorEvent(0x02), await E2E.Settled(error));
        Assert.Equal(CompletionStatus.Failed, await E2E.Settled(sent));
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(stop));
    }

    [Fact]
    public async Task LosingTheLinkFailsTheSessionAndSaysWhy()
    {
        await using var bus = await Bus.StartAsync(E2E.Options(Transport.Loopback, false, OsdpFlags.EnableNotification));
        // The card never answers, so the APDU is still waiting when the link goes
        bus.Reply = _ => CommandReply.Ack;
        Task<OsdpEvent> failed = bus.NextEventAsync(
            e => e is NotificationEvent { Notification: TrsStatusNotification { Status: TrsSessionStatus.Failed } });

        Assert.Equal(CompletionStatus.Ok,
                     await E2E.Settled(bus.Cp.SubmitCommandAsync(Bus.Address, new TrsStartCommand())));
        Task<CompletionStatus> sent = bus.Cp.SubmitCommandAsync(Bus.Address, new TrsSendApduCommand(0, CApdu));
        await bus.NextCommandAsync<TrsSendApduCommand>();
        bus.Cp.DisablePd(Bus.Address);

        Assert.Equal(new TrsStatusNotification(TrsSessionStatus.Failed, 0, TrsFailReason.LinkDown),
                     ((NotificationEvent)await E2E.Settled(failed)).Notification);
        Assert.Equal(CompletionStatus.Failed, await E2E.Settled(sent));
    }

    [Fact]
    public async Task CommandsOutsideTheBandsReaderAreRefused()
    {
        await using var bus = await Bus.StartAsync(E2E.Options(Transport.Loopback, false));
        // Bus.DefaultCapabilities advertises two readers besides the PD's own
        await Assert.ThrowsAsync<OsdpException>(() => bus.Cp.SubmitCommandAsync(Bus.Address, new TrsStartCommand(3)));
        Task<CompletionStatus> start = bus.Cp.SubmitCommandAsync(Bus.Address, new TrsStartCommand(1));
        await Assert.ThrowsAsync<OsdpException>(
            () => bus.Cp.SubmitCommandAsync(Bus.Address, new TrsSendApduCommand(0, CApdu)));
        Task<CompletionStatus> stop = bus.Cp.SubmitCommandAsync(Bus.Address, new TrsStopCommand(1));
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(start));
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(stop));
    }

    [Fact]
    public async Task ApduLimitsAndThePresenceScanAreReachable()
    {
        await using var bus = await Bus.StartAsync(E2E.Options(Transport.Loopback, false));
        Assert.InRange(bus.Cp.GetTrsMaxApduLength(Bus.Address), 1, 258);
        Assert.InRange(bus.Pd.GetTrsMaxApduLength(), 1, 258);
        Assert.True(bus.Cp.TrsScanEnable(Bus.Address, new TrsScanParams(Mode0DwellMs: 300, Reader: 1)));
        Assert.True(bus.Cp.TrsScanDisable(Bus.Address));
    }

    [Fact]
    public async Task ScbkCanOnlyBeReplacedOnADisabledPd()
    {
        await using var bus = await Bus.StartAsync(E2E.Options(Transport.Loopback, false));
        byte[] key = E2E.Pattern(16, 0x10);
        Assert.False(bus.Cp.SetScbk(Bus.Address, key));
        Assert.Throws<ArgumentException>(() => bus.Cp.SetScbk(Bus.Address, new byte[15]));
    }

    [Fact]
    public void SourceInfoNamesTheBuild() => Assert.False(string.IsNullOrEmpty(Osdp.SourceInfo));
}
