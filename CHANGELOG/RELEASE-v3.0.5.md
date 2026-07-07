---
release_date: 2024-03-23
release_version: v3.0.5
---

More vcpkg integration issue fixes. This relaese is also about more platform
suppport.

## Enhancements

- ci: Enhance to run on different platforms; start doc builds
- cmake: Add options to disable shared/static builds

## Fixes

- examples: cpp: Use std::this_thread::sleep_for instead of usleep
- doc: Fix doxygen build by copying and patching osdp.h
- api: Fix some more doxygen formatting issues
- doc: Remove deprecated doxygen config entries
- cmake: cpack: Fix package definitions
- crypto: openssl: Remove call to ERR_print_errors_fp()
- crypo: Add missing headers for openssl.c
- api: Add OSDP_EXPORT to exported methods in osdp.h
- python: example: Update README.md with socat link option
- ci: Fix cross-platform build and change cpack artifact path
- cmake: Fix MSVC OSDP_EXPORT macro issue
- cmake: Fix MSVC compiler flag issue
- CI: Add an on-demand cross platform build check runner
- cmake: Fix broken CI build due utils build
