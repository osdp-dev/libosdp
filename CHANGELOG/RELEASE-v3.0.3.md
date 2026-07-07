---
release_date: 2024-03-13
release_version: v3.0.3
---

Minor release with a few fixes required for rust development to progress

## Enhancements

- make: Add support for make install
- api: cp: Add method to flush command queue
- python: osdp_sys: Add some error context for public API
- doc: Add libosdp/compatibility to top level index.rst

## Fixes

- doc: Fixup doc in multiple places
- api: More doxygen fixups
- cmake: Fix packet trace and data trace linkage
- api: Fix regression caused by doxygen formatting commit
- api: Fix doxygen comments style all over osdp.h
- Fix minor typos in the Wireshark payload dissector
- doc: More README updates
- doc: Update README; fix compatibility table
- libosdp: Fix issue with SC retry timer
- utils: Pull in fix for time usage on Windows
