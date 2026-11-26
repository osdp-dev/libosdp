---
lastUpdated: 2026-06-24
description: "Build and install LibOSDP from source with CMake or the lean Make build — produces static and shared libraries for Linux, macOS, and Windows."
---
# Build and Install

To build libosdp you need a C compiler and CMake 3.14 or newer. The build
produces a static library (`libosdpstatic.a`) and a shared library
(`libosdp.so` / `.dylib` / `.dll`). See `examples/` for how to consume them
from your application.

This project supports two build systems: CMake (recommended) and a thin
Make-based "lean" build for cross-compile-only situations.

## Setting Up the Build Environment

At a minimum you need a C compiler, CMake 3.14 or newer, and Git (to clone the
repository with its submodules). The optional packages below unlock extra
features: OpenSSL or MbedTLS for a hardened [Secure Channel](/libosdp/secure-channel)
backend, Python 3 for the bindings and test suite, and Doxygen for the API
documentation.

### Linux (Debian / Ubuntu)

```sh
sudo apt install build-essential cmake git
# optional:
sudo apt install libssl-dev libmbedtls-dev python3 python3-pip doxygen
```

### Linux (Fedora / RHEL)

```sh
sudo dnf install gcc gcc-c++ cmake git
# optional:
sudo dnf install openssl-devel mbedtls-devel python3 python3-pip doxygen
```

### macOS

Install the Command Line Tools (which provide Clang) and CMake:

```sh
xcode-select --install
brew install cmake
# optional:
brew install openssl@3 mbedtls doxygen python
```

### Windows

Install [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/)
(the "Desktop development with C++" workload provides MSVC), plus
[CMake](https://cmake.org/download/) and [Git for Windows](https://git-scm.com/download/win).
For the optional OpenSSL / MbedTLS dependencies, the simplest path is vcpkg —
see [Using vcpkg](#using-vcpkg) below, which is also the recommended way to
consume LibOSDP on Windows.

:::tip
Once the toolchain is in place, clone the repository with its submodules and
follow the [CMake Build](#cmake-build) steps below:

```sh
git clone https://github.com/osdp-dev/libosdp --recurse-submodules
cd libosdp
```
:::

## CMake Build

```sh
cmake -B build
cmake --build build --parallel
```

To install to a prefix:

```sh
cmake --build build --target install
# or:
cmake --install build --prefix /usr/local
```

The install lays down:

```
${prefix}/lib(64)/libosdp.so*           # shared library + soname symlinks
${prefix}/lib(64)/libosdpstatic.a       # static library
${prefix}/include/libosdp/osdp.h        # C API
${prefix}/include/libosdp/osdp.hpp      # C++ wrapper
${prefix}/include/libosdp/osdp_export.h # visibility macros
${prefix}/lib(64)/pkgconfig/libosdp.pc  # pkg-config metadata
${prefix}/lib(64)/cmake/libosdp/        # CMake package config (find_package)
```

To build only one variant, set `OPT_BUILD_SHARED=OFF` or
`OPT_BUILD_STATIC=OFF`. At least one must remain enabled.

```sh
cmake -B build -DOPT_BUILD_SHARED=OFF   # static-only
cmake -B build -DOPT_BUILD_STATIC=OFF   # shared-only
```

## Make Build

The Make build is a stripped-down alternative for [cross-compile](/libosdp/cross-compiling) or
embedded scenarios where pulling in CMake is undesirable. It builds the
static library only.

```sh
./configure.sh
make
make install        # installs libosdp.a, headers, libosdp.pc
make check          # runs unit tests
```

See `./configure.sh --help` for all flags.

## Crypto Backend Selection

LibOSDP's [Secure Channel](/libosdp/secure-channel) needs an AES implementation. The CMake
and lean Make builds support three backends; the default `auto` mode probes for OpenSSL, then MbedTLS, and
falls back to the bundled TinyAES if neither is found (with a warning,
since TinyAES is not hardened to the same degree as OpenSSL/MbedTLS).

| CMake                                       | configure.sh         | Behavior                                                       |
|---------------------------------------------|----------------------|----------------------------------------------------------------|
| `-DOPT_OSDP_CRYPTO_BACKEND=auto` (default)  | `--crypto auto`      | Pick OpenSSL → MbedTLS → TinyAES (warn on TinyAES fallback)    |
| `-DOPT_OSDP_CRYPTO_BACKEND=openssl`         | `--crypto openssl`   | Require OpenSSL; hard fail if absent                           |
| `-DOPT_OSDP_CRYPTO_BACKEND=mbedtls`         | `--crypto mbedtls`   | Require MbedTLS; hard fail if absent                           |
| `-DOPT_OSDP_CRYPTO_BACKEND=tinyaes`         | `--crypto tinyaes`   | Force the bundled implementation; no probe, no warning         |

These choices apply to the CMake and lean Make builds. The embedded
integrations pick a backend of their own (see
[Platform Integrations](#platform-integrations)):

- **Zephyr** selects it in Kconfig: `CONFIG_LIBOSDP_CRYPTO_PSA` (the default;
  the PSA Crypto API, which needs Zephyr 4.3 or later),
  `CONFIG_LIBOSDP_CRYPTO_MBEDTLS` or `CONFIG_LIBOSDP_CRYPTO_TINYAES`.
- **ESP-IDF** selects it in Kconfig too: `CONFIG_LIBOSDP_CRYPTO_PSA` (the
  default; the PSA Crypto API of the SDK's mbedtls component, which routes
  AES through the hardware accelerator where the chip has one),
  `CONFIG_LIBOSDP_CRYPTO_MBEDTLS` (ESP-IDF 5.x only, since Mbed TLS 4 in
  ESP-IDF 6 drops its AES API) or `CONFIG_LIBOSDP_CRYPTO_TINYAES`.
- **PlatformIO / Arduino** always builds the bundled TinyAES.

## Running the Test Suite

The full release-style test suite (PyTest + unit tests for both build
systems) is wrapped in:

```sh
./scripts/run_tests.sh
```

For just the C unit tests under CMake:

```sh
cmake --build build --target check  # convenience wrapper
ctest --test-dir build               # native CTest entry point (also works in IDEs)
```

For the lean Make build:

```sh
make check
```

## Compile-time Configuration Options

LibOSDP can be configured by passing `-DOPT_XXX=ON/OFF` flags to CMake.
The same options are exposed by `configure.sh` flags for the lean build.

| configure.sh flag     | CMake option                  | Default | Description                               |
|-----------------------|-------------------------------|---------|-------------------------------------------|
| `--packet-trace`      | `OPT_OSDP_PACKET_TRACE`       | OFF     | Enable raw packet trace for diagnostics   |
| `--data-trace`        | `OPT_OSDP_DATA_TRACE`         | OFF     | Enable command/reply data buffer tracing  |
| `--skip-mark`         | `OPT_OSDP_SKIP_MARK_BYTE`     | OFF     | Don't send the leading mark byte (0xFF)   |
| `--zero-copy`         | `OPT_OSDP_RX_ZERO_COPY`       | OFF     | Enable zero-copy RX (recv_pkt/release_pkt)|
| `--log-minimal`       | `OPT_OSDP_LOG_MINIMAL`        | OFF     | Minimize logger RAM/stack (embedded)      |
| `--no-colours`        | `OPT_DISABLE_PRETTY_LOGGING`  | OFF     | Don't colorize log outputs                |
| `--static`            | `OPT_OSDP_STATIC`             | OFF     | Build without dynamic memory allocation   |
| `--lib-only`          | `OPT_OSDP_LIB_ONLY`           | OFF     | Only build the library                    |
| `--crypto LIB`        | `OPT_OSDP_CRYPTO_BACKEND`     | `auto`  | Crypto backend (auto/openssl/mbedtls/tinyaes) |
| N/A                   | `OPT_BUILD_SANITIZER`         | OFF     | `address` (ASan/UBSan/LSan) or `thread` (TSan) |
| N/A                   | `OPT_BUILD_SHARED`            | ON      | Build shared library                      |
| N/A                   | `OPT_BUILD_STATIC`            | ON      | Build static library                      |
| `--bare-metal`        | `OPT_BUILD_BARE_METAL`        | OFF     | Build for bare-metal targets              |
| `--use-32bit-tick-t`  | `OPT_USE_32BIT_TICK_T`        | OFF     | Use uint32_t tick\_t; requires bare-metal |

> **Tip:** The `--packet-trace` and `--data-trace` options are explained in the
> [debugging guide](/libosdp/debugging), including how to inspect the captured
> `.pcap` files.

## Tunables and Buffer Sizes

Beyond the options above, two sets of `#ifndef`-guarded macros can be
overridden at build time by defining them on the compiler command line.

**Library tunables** live in `src/osdp_tunables.h`: protocol timings, retry
counts and internal buffer sizes. Commonly changed ones:

| Macro | Default | Meaning |
|---|---|---|
| `OSDP_PACKET_BUF_SIZE` | 256 | Largest packet this build sends or receives |
| `OSDP_RESP_TOUT_MS` | 200 | How long a CP waits for a PD's reply |
| `OSDP_PD_ONLINE_TOUT_MS` | 8000 | Silence after which the peer is reported offline |
| `OSDP_PD_SC_TIMEOUT_MS` | 8000 | Idle time after which a PD drops its secure channel session |
| `OSDP_PD_SC_RETRY_MS` | 600000 | Wait before a CP retries a failed secure channel handshake |
| `OSDP_CMD_MAX_RETRIES` | 8 | Retries before a CP gives up on a command |
| `OSDP_CP_MAX_PDS` | 8 | Most PDs one CP context can manage in an `OPT_OSDP_STATIC` build |
| `OSDP_TRS_ANSWER_TOUT_MS` | 5000 | How long a TRS band waits for a slow card's answer |
| `OSDP_TRS_IDLE_TOUT_MS` | 30000 | Idle time after which a TRS session is closed |

These are private to the library, so only the library needs them:

```sh
cmake -B build -DCMAKE_C_FLAGS="-DOSDP_PACKET_BUF_SIZE=512"
CCFLAGS="-DOSDP_PACKET_BUF_SIZE=512" ./configure.sh
```

On Zephyr and ESP-IDF these are Kconfig symbols with a `CONFIG_` prefix
(`CONFIG_OSDP_PACKET_BUF_SIZE`, `CONFIG_OSDP_RESP_TOUT_MS`, ...), except
`OSDP_CP_MAX_PDS`, which is set from `CONFIG_OSDP_PD_MAX`; see
[Platform Integrations](#platform-integrations).

**Payload limits** are the `*_MAX_LEN` / `*_MAX_DATALEN` macros in
`include/osdp.h`, for example `OSDP_PIV_DATA_MAX_LEN`, `OSDP_TRS_APDU_MAX_LEN`
and `OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN`. They size the arrays embedded in
`struct osdp_cmd` and `struct osdp_event`, so **changing one changes the
layout of the public structures**. Define the same values for the library and
for every translation unit of your application that includes `osdp.h`; a
mismatch corrupts memory at the API boundary. Neither Kconfig integration has
a symbol for them, so pass them as compile definitions there too.

A binding or application that cannot see the library's build flags can ask
the library what it was built with: `osdp_get_abi_info()` fills a
`struct osdp_abi_info` with the sizes of `struct osdp_cmd`, `struct
osdp_event`, `struct osdp_channel` and `osdp_pd_info_t`, and the options that
change them (`OPT_OSDP_RX_ZERO_COPY`, `OPT_OSDP_LOG_MINIMAL`). See
[Miscellaneous](/api/miscellaneous#build-information).

## Platform Integrations

The repository also carries ready-made integrations for embedded build systems
and a .NET binding.

**Zephyr.** The repository is a Zephyr module (`zephyr/module.yml`, Kconfig in
`zephyr/Kconfig`). Enable it with `CONFIG_LIBOSDP=y`. Defaults worth knowing:

| Symbol | Default |
|---|---|
| `CONFIG_LIBOSDP_CRYPTO_PSA` | y (the crypto backend choice) |
| `CONFIG_LIBOSDP_STATIC` | y (no heap allocation) |
| `CONFIG_LIBOSDP_RX_ZERO_COPY` | y (the channel supplies `recv_pkt`/`release_pkt`) |
| `CONFIG_OSDP_PACKET_BUF_SIZE` | 1024 |

Most of the protocol timing and sizing tunables have a `CONFIG_OSDP_*` symbol
with the default of `src/osdp_tunables.h`. Two differ:
`CONFIG_OSDP_PACKET_BUF_SIZE` (1024) and `CONFIG_OSDP_PD_MAX` (126), which
sets `OSDP_CP_MAX_PDS`. The TRS, PIV, biometric and file-transfer timeouts
have no symbol.

**ESP-IDF.** The repository root is an ESP-IDF component (`idf_component.yml`,
with the component definition in `esp-idf/`), published to the ESP Component
Registry as `sidcha/libosdp`. Add it with
`idf.py add-dependency "sidcha/libosdp"`, or add the
repository under `components/` as a submodule, with its own `utils/`
submodule initialised too. Its options are under *Component config → Open
Supervised Device Protocol (OSDP)* in `menuconfig`, with the same symbol names
as the Zephyr module. `CONFIG_LIBOSDP_STATIC` defaults to y,
`CONFIG_LIBOSDP_RX_ZERO_COPY` to n and `CONFIG_OSDP_PACKET_BUF_SIZE` to 1024. The component supplies the tick source
(`esp_timer`) and entropy (`esp_random()`) itself.

**PlatformIO / Arduino.** `library.json` makes the repository a PlatformIO
library, published to the PlatformIO Registry as `LibOSDP`; add it to
`lib_deps` in `platformio.ini`. It builds as a bare-metal target with the
bundled TinyAES backend. Sketches are in `examples/platformio/`.

**.NET.** The `LibOSDP` NuGet package (`dotnet add package LibOSDP`) wraps the
C library for .NET 10 and ships prebuilt native libraries for
Windows, Linux and macOS. Its sources are in `dotnet/` and examples in
`examples/dotnet/`. It compares its own structure layout with the native
library's through `osdp_get_abi_info()` before using it.

## Add LibOSDP to Your CMake Project

### find_package (recommended)

Build and install LibOSDP, then in your project's `CMakeLists.txt`:

```cmake
find_package(libosdp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE libosdp::libosdp)
```

`libosdp::libosdp` is an alias that resolves to the shared library when
both variants were installed, falling back to the static library when
only static was built. If you specifically want one or the other, use
`libosdp::osdp` (shared) or `libosdp::osdpstatic` (static) directly.

The package config carries the crypto backend dependency through
`find_dependency()`, so a static-link consumer of an OpenSSL-built
LibOSDP automatically picks up `OpenSSL::Crypto` without further setup.

### add_subdirectory

You can also embed LibOSDP directly in your build:

```cmake
add_subdirectory(third_party/libosdp)
target_link_libraries(my_app PRIVATE libosdp::libosdp)
```

The same `libosdp::libosdp` / `libosdp::osdp` / `libosdp::osdpstatic`
aliases are defined in the build tree, so consumer code is identical
whether LibOSDP is installed or vendored.

### FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(libosdp
    GIT_REPOSITORY https://github.com/osdp-dev/libosdp.git
    GIT_TAG        v4.0.0  # update to latest release
)
FetchContent_MakeAvailable(libosdp)
target_link_libraries(my_app PRIVATE libosdp::libosdp)
```

### Using pkg-config

LibOSDP installs a `libosdp.pc` file alongside the library. With
`pkg-config` on your `PKG_CONFIG_PATH`:

```sh
gcc my_app.c $(pkg-config --cflags --libs libosdp) -o my_app
```

The `Cflags` adds `-I${includedir}/libosdp`, so `#include <osdp.h>`
works as written. For static linking, `pkg-config --static` adds the
crypto backend (`-lcrypto` or `-lmbedcrypto`) declared via
`Requires.private`.

### Using vcpkg

A port for LibOSDP is available in vcpkg upstream. Follow the
[getting started guide](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started)
to set up vcpkg, then:

```shell
mkdir osdp_app && cd osdp_app
vcpkg new --application
vcpkg add port libosdp
```

Build with the vcpkg toolchain file:

```shell
cmake -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -B build .
cmake --build build
```

> **Note:** vcpkg is the recommended method to consume LibOSDP on Windows.
