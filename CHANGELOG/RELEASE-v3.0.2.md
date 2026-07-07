---
release_date: 2024-03-09
release_version: v3.0.2
---

Yet another minor release to address some of the issues that were identified in
3.0.0 release. With this release, LibOSDP supports Windows as a first-class
citizen.

## Enhancements

- pytest: run.sh: Add an option to skip running the tests
- doc: Add LibOSDP compatibility table
- publish-pypi: Enable windows build/publish
- doc: Update security.md
- python: example: Add some CLI args and add a README.md
- pytest: Add some stage info for run.sh
- libosdp: phy: some minor houekeeping
- python: Deprecate master key based key derivation
- python: Remove concrete "channel" implementations
- libosdp: file: Make impossible conditions as BUG_ON()
- libosdp: file: Do not disrupt SC when file TX fails
- pcap: Replace ':' with '_' in pcap file name timestamp
- utils: bump submodule to fix __weak warning

## Fixes

- Fix bug in SC setup due to missing call to cp_keyset_complete
- python: Fix buggy call to PyArg_Parse due to type of len
- python: fix some minor issues for MSVC to build libosdp
- pcap: Fix file name character replacement bug
- dissector: Remove extra space in command ID text
- libosdp: Fix broken SC due to missing sc_init call
