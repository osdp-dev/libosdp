---
release_date: 2026-03-16
release_version: v3.2.0
---

This marks the last release for v3 of LibOSDP as v4 will break a few API to switch
to a new "one channel per osdp_t" data model.

## Enhancements

- Python: Enable support for app owned queue data
- API: Add app-owned queue mode with completion callbacks
- Add some quality of life improvements for Zephyr
- chore: Add macro guards around config.h.in entries
- Add python support for zero-coppy and add a pytest
- API: Add support for a new zero-copy RX path
- allow REPLAY_OSTATR as reply for CMD_OUT
- Add helper methods to read/write from buffers
- cp: Remove unused flag PD_FLAG_HAS_SCBK
- LibOSDP: Decouple OSDP_FLAG_XXX and PD_FLAG_XXX
- Add static inline helpers for various OSDP_FLAG_* macros
- API: Reorder attribute macros for Clang compatibility
- PD: Add internal command validation

## Fixes

- Fix build git version tags
- tests: Fix broken command unit tests
- Fix docs build
- test: Fix failure due to capabilities
- tests: Fix warning about unused functions
- python: Fix deadlocks by using with statement in locks
- pyosdp: Fix error propagation to help debugging
- fixup! Initialize command and event structs before the are used
- Initialize command and event structs before the are used
- pyosdp: Fix status command event and struct builders
- fix Stat Decode error in file transfer keep alive mode
- Avoid unused variable warnings in case __ASSERT evaluates to NOP.
- PD: Fix warning about unhandled cases in validate_command
- CP: Fix un‑annotated fall‑through in notify_command_status
- fixup! PD: Add internal command validation
- CP: Fix bug in sequence number progression
- PD: Fix non-compliant REPLY_RMAC_I status byte
- Remove redundant pd->reply_id sets and add missing javadoc entry
