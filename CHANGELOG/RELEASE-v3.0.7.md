---
release_date: 2024-11-11
release_version: v3.0.7
---

This release has been long overdue, bringing in a lot of enhancements and
stability fixes. Most notable change include PlatformIO support.

## Enhancements

- CI: Fix PlatformIO publish workflow
- scripts: Add release hooks for platformio
- PlatformIO: check-in a cached copy of generated headers
- PlatformIO: Add initial port for PlatformIO
- doc: Split osdp_pd_into_t into it's own section
- cmake: Always export compile_commands.json
- ci: Move to actions/upload-artifact@v4 to fix CI builds
- build: Make changes to allow nix/nixos builds
- libosdp: Make a copy of PD name from pd_info_t
- file: Add support various flow control flags
- API: Add support for PD status change events.
- file: Add support for keep alive messages
- cp: Add support for delaying file transfer
- file: Switch to using to BYTES to/from unsigned types macros
- file: Fix multiple assumptions about target endianess
- crypto: Anotate panic method with 'noreturn' attribute
- cmake: Add more sanitizers and rename the config key for it
- scripts: run_tests.sh: Switch to cmake based build and run invocations
- cmake, make: generate osdp_config.h in build/include dir
- doc: Update packet and data trace documentation
- Rename osdp_pcap.* as osdp_diag.* to allow for future enhancements
- examples/python: PD: Add singal handler to exit the script
- python: Enable packet trace builds by default
- API: Add a flag to enable packet trace at runtime
- scripts: Update run_tests.sh to call functions for different test suites
- cp: phy: Allow plaintext NACKs when SC is active
- cp: Add a log line when retrying SC after timeout
- cp: keyset complete don't request SC restart if PD is not online
- Prefix CP/PD tags to setup log line
- cp: Add entry for handling OSDP_ERR_PKT_NACK to add comments
- cp: Set reply_id to REPLY_INVALID before sending a command
- cp: Introduce "probe" state for PDs that don't respond
- doc: Update cmake invocations for cross-platform builds
- api: Remove OSDP_EXPORT decorations in declarations
- cmake: Exclude targets that cannot be built when using MSVC
- doc: Add section about vcpkg usage
- API: Add new setup flag to guard notification events
- API: Add support notification events
- scripts: Add a tests runner scripts
- doc: Add deprecation notice for osdpctl
- pytest: Add tests for pd_id and pd_cap API
- test: Add some info message when invoked with -n flag
- python: osdp: Add method in KeyStore to persisting a key
- python: osdp: Consume and export pd_id and pd_cap getters
- python: osdp_sys: Enable pd_cap and pd_id getters
- phy: Lower log level for packet_scan_sckip reports
- phy: Allow multiple mark bytes before SOM

## Fixes

- Fix handling of empty SCS17/SCS18 messages
- Annotate osdp_millis_now definition with __weak atrribute
- python: Fix LED commands with temporary and permanent records
- Copied osdp_export.h from correct location on install
- cp: Fix 'REPLY_BUSY' handling logic
- pd: Don't discard SC before acknowledging keyset command
- cp: Move cp_cmd_free() out of cp_translate_cmd()
- cp: Fix typo in flag CP_REQ_OFFLINE value
- file: Fix bug in the exclusive flag collection
- libosdp: Fix memory leak when file ops is registered
- cp: Fix loss of tamper bit when saving power bit in status event
- utils: Pull in fixes for loging and make
- utils: Bump submodule for windows build fix
- cp: Fix segfault in osdp_cp_teardown()
- python: Fix broken build due to wrong return type
- python: Fix potential null-ptr-deref during event callback
- Bump utils and fix some typos in osdp.h
- examples: Fix makefile and some minor udpates
- pd: Fix firmware_version reports in PD_ID
