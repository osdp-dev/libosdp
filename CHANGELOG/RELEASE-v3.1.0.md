---
release_date: 2025-09-25
release_version: v3.1.0
---

This release introduces hotplug support, adaptive CRC handling, improved
sequnce number handling, and several CP/PD enhancements. It also includes
critical bug fixes for MFG commands, memory leaks (python), and protocol
stability.

## Enhancements

- API: Add support for PD hot-plug/unplug
- unit-tests: Add coverage for commands and events
- tests: Fix and enhance Python OSDP test infrastructure
- unit-tests: Add CP/PD startup time fuzzers
- CP: Improve sequence number NAK handling and recovery
- unit-tests: Add more phy layer tests
- CP: Add adaptive checksum/CRC-16 support with automatic fallback
- CI: Add support for more python platforms
- API: provide export macros locally
- Add initial support to build LibOSDP as a Zephyr module
- cmake: Add option to enable bare metal builds
- cmake: Rename CONFIG_XXX with OPT_XXX
- cp: Reset retry_count when PD responds
- pd: Add missing call to osdp_sc_init()
- API: Rename cp_send_command and pd_notify_event
- API: Add channel close callback
- API: CP: Add support for command broadcast
- CP: Extend debug logging for events
- API: PD: Add new COMESET_DONE command
- API: Extend PD status report mask to byte array
- API: Deprecate OSDP_CARD_FMT_ASCII
- API: Add new PD capabilities introduced in OSDP v2.2
- API: Add support to include PDs after osdp_cp_setup

## Fixes

- MFG: remove command field on OSDP 2.2
- Fix manufacturer command callback return value handling
- python: Fix a plethora of memory leaks and other issues
- Fix Wireshark Lua dissector issue on newer Lua versions
- PD: Initialize event objects to prevent undefined behavior
- pd: Fix bug in MFGREP events from application
- build: Fix breakage due to CRC16 re-org
- Fix platformio CP example
- Phy: Fix packet header scan
- pytest: skip broken test test_command_status()
- PD: Fix some random cases where pd->reply_id could be invlaid
- pd: Fix do_command_complete callback handling
- crypto/tinyaes: Typecast RAND_MAX to float to fix warning
- fix offline notification
