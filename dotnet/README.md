# LibOSDP for .NET

[![NuGet](https://img.shields.io/nuget/v/LibOSDP?logo=nuget)](https://www.nuget.org/packages/LibOSDP)
[![Build CI](https://github.com/osdp-dev/libosdp/workflows/Build%20CI/badge.svg)](https://github.com/osdp-dev/libosdp/actions?query=workflow%3A%22Build+CI%22)

The [Open Supervised Device Protocol](https://doc.osdp.dev/protocol/) (OSDP,
IEC 60839-11-5) connects access control panels to the card readers, keypads,
and I/O modules on their RS-485 bus. A **control panel (CP)** polls and
commands its **peripheral devices (PDs)**; a PD answers and reports card reads,
key presses, and status. The **Secure Channel** encrypts and authenticates that
traffic with AES-128.

This package brings [LibOSDP](https://osdp.dev/) to .NET. It is the same C
engine that runs inside embedded readers, so a .NET CP or PD speaks exactly the
protocol the hardware does:

- CP and PD modes, every command and event including Transparent Reader
  Support (smart-card APDU sessions and the presence scan), Secure Channel
  with key provisioning (install mode, osdp_KEYSET), file transfer, and link
  metrics.
- Commands and events are C# records; submissions return a `Task` that
  completes when LibOSDP is done with them.
- Prebuilt native libraries for Windows (x64, Arm64), Linux (x64, Arm64,
  Arm32, musl x64 and Arm64), and macOS (Apple silicon, Intel). Nothing to
  compile.
- Trimming and NativeAOT compatible.

## Install

```sh
dotnet add package LibOSDP
```

## Control Panel

Open the RS-485 port, describe the PDs on it, and run the bus. `portName` is
something like `/dev/ttyUSB0` or `COM3`; the
[System.IO.Ports](https://www.nuget.org/packages/System.IO.Ports) package
provides `SerialPort`.

<!-- snippet:control-panel -->
```csharp
using var serial = new SerialPort(portName, 115200);
serial.Open();
using var channel = new StreamChannel(serial.BaseStream);

byte[] scbk = Convert.FromHexString("000102030405060708090A0B0C0D0E0F");
using var cp = new ControlPanel(channel, [new PdInfo(101) { Scbk = scbk }]);

cp.EventReceived += (_, e) =>
{
    if (e.Event is CardReadEvent card)
        Console.WriteLine($"PD {e.Address} read card {Convert.ToHexString(card.Data)}");
};

using var stop = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    stop.Cancel();
};
Task bus = cp.RunAsync(stop.Token);

if (await cp.WaitSecureChannelAsync(101, TimeSpan.FromSeconds(10), stop.Token))
{
    var green = new TemporaryLedParams(TemporaryLedControlCode.Set, OnCount: 5, OffCount: 5,
                                       OnColor: LedColor.Green, OffColor: LedColor.Black, TimerCount: 30);
    await cp.SubmitCommandAsync(101, new LedCommand(Reader: 0, LedNumber: 0, Temporary: green))
        .WaitAsync(stop.Token);
}
```

## Peripheral Device

<!-- snippet:peripheral-device -->
```csharp
using var serial = new SerialPort(portName, 115200);
serial.Open();
using var channel = new StreamChannel(serial.BaseStream);

byte[] scbk = Convert.FromHexString("000102030405060708090A0B0C0D0E0F");
PdCapability[] capabilities =
[
    new(Capability.LedControl, ComplianceLevel: 1, NumItems: 1),
    new(Capability.Readers, ComplianceLevel: 1, NumItems: 1),
];
using var pd = new PeripheralDevice(channel, new PdInfo(101) { Scbk = scbk }, capabilities);

pd.CommandHandler = command =>
{
    if (command is LedCommand led)
        Console.WriteLine($"LED {led.LedNumber}: {led.Temporary?.OnColor}");
    return CommandReply.Ack;
};

using var stop = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    stop.Cancel();
};
Task bus = pd.RunAsync(stop.Token);

var card = new CardReadEvent(ReaderNo: 0, CardFormat.Wiegand, Data: [0x12, 0x34, 0x56, 0x40], Bits: 26);
await pd.SubmitEventAsync(card).WaitAsync(stop.Token);
```

Call `RunAsync` (or `Refresh` at least every 50 ms) for as long as the device
should stay on the bus, and dispose the device when done.

## Documentation

Full documentation is at [doc.osdp.dev](https://doc.osdp.dev/), including a
.NET section:

- [Getting Started](https://doc.osdp.dev/dotnet/getting-started) — install and a minimal app
- [Build and Install](https://doc.osdp.dev/dotnet/build-and-install) — native libraries, platforms, building from source
- [Control Panel](https://doc.osdp.dev/dotnet/control-panel) — the `ControlPanel` class
- [Peripheral Device](https://doc.osdp.dev/dotnet/peripheral-device) — the `PeripheralDevice` class
- [Commands](https://doc.osdp.dev/dotnet/commands) — what a CP sends
- [Events](https://doc.osdp.dev/dotnet/events) — what a PD reports
- [API Reference](https://doc.osdp.dev/dotnet/api) — `PdInfo`, channels, file transfer, logging, enums

The protocol itself is described in the
[OSDP protocol guide](https://doc.osdp.dev/protocol/) and its
[commands and replies](https://doc.osdp.dev/protocol/commands-and-replies).

## Supported platforms

| OS | Runtime identifiers |
|---|---|
| Windows 10+ | `win-x64`, `win-arm64` |
| Linux, glibc 2.28+ | `linux-x64`, `linux-arm64` |
| Linux, glibc 2.31+ | `linux-arm` |
| Linux, musl (Alpine) | `linux-musl-x64`, `linux-musl-arm64` |
| macOS 12+ | `osx-arm64`, `osx-x64` |

## License and support

Apache-2.0. Report bugs and ask questions on
[GitHub issues](https://github.com/osdp-dev/libosdp/issues); the source is at
[github.com/osdp-dev/libosdp](https://github.com/osdp-dev/libosdp).
