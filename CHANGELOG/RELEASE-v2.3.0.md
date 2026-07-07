---
release_date: 2023-06-27
release_version: v2.3.0
---

A standard mid-year release with accumulated fixes and enhancements.
Interestingly, this release has a high number of community contributions :)

## Enhancements

- Add support for additional baud rate 57600
- Add deprecation notice for command_complete_callback API
- pyosdp: refactor make_(struct|dict) methods into translators
- pyosdp: Move similar methods closer for maintainability
- pyosdp: Refactor event handling method names
- pyosdp: refactor command handling method names
- pytest: Add tests for IO and status events
- pyosdp: export IO and status events to use in pytests
- libosdp: Add support for tamper and power status events
- phy: Allow sequence repeat packets
- libosdp: Limit max packet size to reported peer RX size
- pd: Add support for handling multiple records in output command
- pytest: More multidrop test related changes
- zephyr: Refactor command_callback into a method
- PD: Reset phy state when ignoring a packet
- libosdp: file: Promote to be a fist-class citizen
- libosdp: Allow file transfers to be aborted
- pd: Handle OSDP_EVENT_MFGREP case in pd_translate_event
- pyosdp, pytests: Add test case for mfgrep event

## Fixes

- pyosdp: Hoist cmd->id set in pyosdp_make_struct_cmd
- fix LED & Buzzer repeated command
- mbedtls: fix incorrect mode in ecb encryption
- fix: converts uint64 to uint32 for arm device capability.
- Disable FindMbedTLS.cmake to fix issue in #108
- Fix typos in secure channel docs
- Make installation with cmake happier
- pd: Fix CMD_ABORT length check
- cp: Add support for local status query command
- Fix sequence reversing issue during phy_reset()
- libosdp: Fix plethora of bugs in input/output events
- pyosdp: Drop Py_TPFLAGS_HAVE_GC flags on osdp class
- Fix issues in configure.sh
- corrected typo in doxygen documentation
- file: Fix bug in return value processing in build_command
- libosdp: raw loops to memcpy and other zephyr review fixes
- fixup! libosdp: file: Promote to be a fist-class citizen
- pytest: Add file transfer abort test
- unit-test: Fix test failure due to missing extern
- unit-test: Fix a use-after-free bug in test async runner
- fixup! pd: Handle OSDP_EVENT_MFGREP case in pd_translate_event
