# LibOSDP - Open Supervised Device Protocol Library

[![Latest Release][1]][2]
[![Build CI][3]][4]
[![PyPI Version][16]][12]
[![PlatformIO Registry][17]][18]
[![Vcpkg Version][32]][33]
[![Crates.io LibOSDP version][34]][35]
[![Crates.io osdpctl version][36]][37]
[![NuGet Version][42]][43]

This is a cross-platform open source implementation of IEC 60839-11-5 Open
Supervised Device Protocol (OSDP). The protocol is intended to improve
[interoperability][40] among access control and security products. It supports
Secure Channel (SC) for encrypted and authenticated communication between
configured devices.

OSDP describes the communication protocol for interfacing one or more Peripheral
Devices (PD) to a Control Panel (CP) over a two-wire RS-485 multi-drop serial
communication channel. Nevertheless, this protocol can be used to transfer
secure data over any stream based physical channel. Read more about OSDP
[here][21].

This protocol is developed and maintained by [Security Industry Association][20]
(SIA).

## Salient Features of LibOSDP

  - Complete OSDP v2.2 command and reply set, on both the CP and the PD side
    ([see][22]). The one message left out on purpose is `osdp_FMT`, whose
    character count the specification leaves undefined.
  - Transparent Reader Support (TRS): a CP drives a smart card through the
    reader with raw ISO 7816 APDUs, including secure PIN entry, while the
    library runs the reader's mode switching and a background card presence
    scan ([see][45]).
  - Smart card (PIV data, GENAUTH, CRAUTH), biometric read/match and file
    transfer, with multi-part messages fragmented and reassembled by the
    library so applications only ever see whole payloads.
  - Supports secure channel communication (AES-128) by default and provides a
    custom init-time flag to enforce a higher level of security not mandated by
    the specification ([see][41]). A key rotation only takes effect once a
    session on the new key proves it; one that fails leaves the PD on the key
    it already had.
  - Asynchronous API: commands and events are queued by reference, and every
    one is reported back exactly once through a completion callback (done,
    failed, flushed or aborted). Link, session and transfer state changes are
    delivered as notifications.
  - Fully non-blocking; the application drives it with a periodic refresh, so
    it fits a superloop, an RTOS task or an event loop alike.
  - Can be used to setup a PD or CP mode of operation (see [examples][39]).
  - Exposes a well defined contract though a single [header file][38].
  - Cross-platform; can be built to run on bare-metal embedded devices, Linux,
    Mac, and Windows.
  - No run-time memory allocation. All memory is allocated at init-time, and
    a static build (`OPT_OSDP_STATIC`) does without a heap altogether.
  - Optional zero-copy receive path for RAM-constrained targets, letting the
    transport hand frames to the library without an intermediate ring buffer.
  - No external dependencies (for ease of cross compilation)
  - Provides Rust, Python3, .NET and C++ bindings for the C library for faster
    integration into various development phases.
  - Includes dozens of integration and unit tests which are incorporated in CI
    to ensure higher quality of releases.
  - Built-in, sophisticated, debugging infrastructure and tools, including
    packet capture and per-PD link metrics ([see][14]).
  - Packaged and distributed through various package repositories such as
    Cargo [crates][19], [PyPI][12], [Vcpkg][33], [PlatformIO][18], etc.,

## Usage Overview

An OSDP bus has exactly one CP talking to one or more PDs; LibOSDP can act as
either. Your application interacts through three constructs (not to be confused
with the protocol's own terminology):

  - **Channel** - the transport that lets two OSDP devices talk to each other
  - **Commands** - a call for action from a CP to one of its PDs
  - **Events** - a call for action from a PD to its CP

You implement the `osdp_channel` interface, describe the device with
`osdp_pd_info_t`, create an `osdp_t` context, and call `osdp_cp/pd_refresh()` at
least once every 50ms to meet the protocol's timing requirements. From there a
CP sends commands and receives events, while a PD notifies events and handles
commands.

See the [API documentation][26] and [examples][39] for the full workflow.

## Language Support

### C/C++ API

LibOSDP core is written in C. It exposes a [minimal set of API][26] to setup
and manage the life-cycle of OSDP devices. See `include/osdp.h` or
`include/osdp.hpp` for more details.

### Rust API

LibOSDP is available via [crates.io][10]. See [rust/README.md][11] for more
info and usage examples.

### Python API

LibOSDP is available as a [python package][12]. See [python/README.md][13] for
more info and usage examples.

### .NET API

LibOSDP is available as a [NuGet package][43]. See [dotnet/README.md][44] for
more info and usage examples.

## Supported Commands and Replies

OSDP has certain command and reply IDs pre-registered. LibOSDP implements every
one that OSDP v2.2 defines except `osdp_FMT`. You can see a list of commands
and replies and their support status in LibOSDP [here][22].

## Build & Install

You need a C compiler and CMake 3.14 or newer. Clone with submodules and build:

```sh
git clone https://github.com/osdp-dev/libosdp --recurse-submodules
cd libosdp
cmake -B build .
cmake --build build --parallel
```

This produces `libosdp.so` and `libosdpstatic.a`; downstream CMake projects can
consume the library with `find_package(libosdp CONFIG REQUIRED)` and link
against the `libosdp::libosdp` target. LibOSDP also ships a lean Make-based build
for cross-compile and embedded targets that needs nothing but a C compiler.

The [Build and Install guide][6] on doc.osdp.dev is the full source of truth: it
covers setting up the build environment on Linux, macOS, and Windows, the crypto
backend and compile-time options, cross-compilation, running the test suite, and
consuming LibOSDP from your own CMake, pkg-config, or vcpkg project.

## Contributions, Issues and Bugs

The Github issue tracker doubles up as TODO list for this project. Have a look
at the [open issues][31], PRs in those directions are welcome.

If you have a idea, find bugs, or other issues, please [open a new issue][28]
in the github page of this project [https://github.com/osdp-dev/libosdp][24].

You can read more on this [here](CONTRIBUTING.md).

## License

This software is distributed under the terms of Apache-2.0 license. If you don't
know what that means/implies, you can consider it is as "free as in beer".

OSDP protocol is also open for consumption into any product. There is no need
to,
 - obtain permission from SIA
 - pay royalty to SIA
 - become SIA member

The OSDP specification can be obtained from SIA for a cost. Read more at our
[FAQ page][27].

## Support the development

Since this is no longer a hobby project, it takes time and effort to develop
and maintain this project. If you are a user and are happy with it, consider
supporting the development by donations though my [GitHub sponsors page][15].
Your support will ensure sustained development of LibOSDP.

[1]: https://img.shields.io/github/v/release/osdp-dev/libosdp?display_name=tag&logo=github
[2]: https://github.com/osdp-dev/libosdp/releases/latest
[3]: https://github.com/osdp-dev/libosdp/workflows/Build%20CI/badge.svg
[4]: https://github.com/osdp-dev/libosdp/actions?query=workflow%3A%22Build+CI%22
[6]: https://doc.osdp.dev/libosdp/build-and-install
[10]: https://crates.io/crates/libosdp
[11]: https://github.com/osdp-dev/libosdp-rs/tree/master/libosdp
[12]: https://pypi.org/project/libosdp/
[13]: https://github.com/osdp-dev/libosdp/tree/master/python
[14]: https://doc.osdp.dev/libosdp/debugging
[15]: https://github.com/sponsors/sidcha
[16]: https://img.shields.io/pypi/v/libosdp?logo=python&link=https%3A%2F%2Fpypi.org%2Fproject%2Flibosdp%2F
[17]: https://badges.registry.platformio.org/packages/sidcha/library/LibOSDP.svg
[18]: https://registry.platformio.org/libraries/sidcha/LibOSDP
[19]: https://crates.io/search?q=libosdp
[20]: https://www.securityindustry.org/industry-standards/open-supervised-device-protocol/
[21]: https://doc.osdp.dev/protocol/
[22]: https://doc.osdp.dev/protocol/commands-and-replies
[24]: https://github.com/osdp-dev/libosdp
[26]: https://doc.osdp.dev/api/
[27]: https://doc.osdp.dev/protocol/faq
[28]: https://github.com/osdp-dev/libosdp/issues/new/choose
[31]: https://github.com/osdp-dev/libosdp/issues
[32]: https://img.shields.io/vcpkg/v/libosdp
[33]: https://vcpkg.link/ports/libosdp
[34]: https://img.shields.io/crates/v/libosdp?style=flat&logo=rust&logoColor=DDD&label=crate%20%3A%20libosdp&link=https%3A%2F%2Fcrates.io%2Fcrates%2Flibosdp
[35]: https://crates.io/crates/libosdp
[36]: https://img.shields.io/crates/v/osdpctl?style=flat&logo=rust&logoColor=DDD&label=crate%20%3A%20osdpctl&link=https%3A%2F%2Fcrates.io%2Fcrates%2Fosdpctl
[37]: https://crates.io/crates/osdpctl
[38]: https://github.com/osdp-dev/libosdp/blob/master/include/osdp.h
[39]: https://github.com/osdp-dev/libosdp/tree/master/examples
[40]: https://doc.osdp.dev/libosdp/compatibility
[41]: https://doc.osdp.dev/libosdp/secure-channel
[42]: https://img.shields.io/nuget/v/LibOSDP?logo=nuget
[43]: https://www.nuget.org/packages/LibOSDP
[44]: https://github.com/osdp-dev/libosdp/tree/master/dotnet
[45]: https://doc.osdp.dev/libosdp/smartcard#transparent-reader-support-trs
