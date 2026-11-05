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

LibOSDP's [Secure Channel](/libosdp/secure-channel) needs an AES implementation. Three backends are
supported; the default `auto` mode probes for OpenSSL, then MbedTLS, and
falls back to the bundled TinyAES if neither is found (with a warning,
since TinyAES is not hardened to the same degree as OpenSSL/MbedTLS).

| CMake                                       | configure.sh         | Behavior                                                       |
|---------------------------------------------|----------------------|----------------------------------------------------------------|
| `-DOPT_OSDP_CRYPTO_BACKEND=auto` (default)  | `--crypto auto`      | Pick OpenSSL → MbedTLS → TinyAES (warn on TinyAES fallback)    |
| `-DOPT_OSDP_CRYPTO_BACKEND=openssl`         | `--crypto openssl`   | Require OpenSSL; hard fail if absent                           |
| `-DOPT_OSDP_CRYPTO_BACKEND=mbedtls`         | `--crypto mbedtls`   | Require MbedTLS; hard fail if absent                           |
| `-DOPT_OSDP_CRYPTO_BACKEND=tinyaes`         | `--crypto tinyaes`   | Force the bundled implementation; no probe, no warning         |

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
    GIT_TAG        v3.2.0  # update to latest release
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
