---
lastUpdated: 2026-11-26
description: "Install the LibOSDP Python bindings — pre-built PyPI wheels, install from GitHub, and building from source on Linux, macOS, and Windows including the compiler toolchain each platform needs."
---
# Build and Install

The `libosdp` package ships a compiled C extension (`osdp._sys`) built from the
LibOSDP C sources. Most users install a pre-built wheel and never compile
anything; this page covers every install path and what a from-source build needs
on each platform.

## From PyPI (recommended)

```sh
pip install libosdp
```

Wheels are published for CPython 3.11–3.15 (plus the free-threaded 3.14t
and 3.15t builds) on Linux (manylinux and musllinux,
x86_64 and aarch64), macOS (x86_64, arm64, universal2), and Windows (AMD64,
ARM64). `pip` picks the matching wheel and no compiler is involved. The package
requires **Python 3.11 or newer** and has no runtime dependencies.

## From GitHub

To install a specific commit or branch without cloning:

```sh
pip install -e "git+https://github.com/osdp-dev/libosdp#egg=libosdp&subdirectory=python"
```

## From a source checkout

```sh
git clone https://github.com/osdp-dev/libosdp --recurse-submodules
cd libosdp/python
pip install .
```

::: warning
`--recurse-submodules` is required. The build vendors the C library and its
`utils` submodule into the package; a checkout missing the submodule will not
compile.
:::

When you build from source (a source tree, an sdist, or a platform without a
matching wheel), pip compiles the extension, so a working C toolchain must be
present.

### Per-platform toolchain

::: tip Linux
Install a C compiler and Python headers, e.g. on Debian/Ubuntu:
`sudo apt install build-essential python3-dev`. Standard `cc`/`gcc`/`clang`
work with no extra flags.
:::

::: tip macOS
Install the Command Line Tools once with `xcode-select --install`. The system
`clang` builds the extension with no extra flags.
:::

::: tip Windows
Build with **MSVC** (the "Desktop development with C++" workload from the Visual
Studio Build Tools). The build automatically adds `/Zc:preprocessor` on Windows,
which switches MSVC to its standards-conformant preprocessor — the extension's
macros do not compile under the legacy one. No manual flag is needed.
:::

All platforms also need `setuptools >= 83` and, for a git checkout, `git` on the
`PATH` so the build can stamp version metadata (it falls back to placeholders
when git or the checkout is absent, but still needs the vendored sources).

### Build-time environment variables

These are optional and mainly useful when developing or testing the bindings.
They are read by `setup.py` at build time, so set them before `pip install`:

| Variable | Effect |
|---|---|
| `OSDP_PD_ONLINE_TOUT_MS` | Override the PD online timeout (ms). Lowered by the test suite so offline tests don't wait out the 8s production default. |
| `OSDP_RESP_TOUT_MS` | Override the CP response timeout (ms). |
| `OSDP_COVERAGE=1` | Compile with gcov instrumentation for coverage runs. |

Example:

```sh
OSDP_PD_ONLINE_TOUT_MS=1000 pip install --no-build-isolation .
```

## Verify the install

```python
from osdp import ControlPanel, PeripheralDevice   # loads the C extension
print("libosdp ready")
```

If the imports succeed, the extension compiled and loaded. Head to
[Getting Started](/python/getting-started) for a first working app.
