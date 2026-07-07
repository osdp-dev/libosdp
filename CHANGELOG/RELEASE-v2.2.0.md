---
release_date: 2022-12-01
release_version: v2.2.0
---

Yet another minor release with accumulated fixes and enhancements. Notable
changes since last release include,

## Enhancements

- pd: add method to flush event queue
- Enable REPLY_ISTATR and REPLY_OSTATR support
- libosdp: Added opaque argument to command_complete_callback
- pytest: Add events tests
- phy: Only flush channel on errors in osdp_phy_state_reset
- phy: Don't attempt to send a NACK for some early failures
- libosdp: Make osdp_cmd_mfg and osdp_mfgrep identical
- cp: Do not propagate PHY errors as-is
- libosdp: Add a ring buffer to store the RX data
- libosdp: Move channel read/writes into phy layer
- unit-tests: Resurrect the old unit-tests modules
- libosdp: Add a softname for PDs in the info struct
- Avoid one ifdef by reorg and static inline-ing
- libosdp: Add support for I/O status monitoring/reporting
- Enable address sanitizer enabled builds

## Fixes

- libosdp: Fix CMD_ABORT's assigned command code
- cp: Fix cardread and keypress event decode cases
- unit-tests: Add a test case to validate the mark-byte skip
- pytest: Fix is_sc_active() to return boolean
- fixup: libosdp: Add support for I/O status monitoring/reporting
- Fix build failure
- Fix OSDP_QUEUE_SLAB_SIZE defintion and some typos
- Fix a heap-buffer-overflow in __cp_setup()
