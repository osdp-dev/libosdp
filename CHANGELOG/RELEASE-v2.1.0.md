---
release_date: 2022-04-15
release_version: v2.1.0
---

This minor release puts together the enhancements and fixed accumulated over the
last year. No dramatic changes.

## Enhancements

- pytest: Remove master_key_workflow test case
- pyosdp: cp_init switch to osdp_cp_setup2()
- pytest: Add set/clear flags methods
- libosdp: Add new exported method: osdp_cp_modify_flag()
- libosdp: Add new flag OSDP_PD_FLAG_IGN_UNSOLICITED
- pd: Add check on the length field of osdp_MFGREP
- pytest: remove the need to install python module for tests
- cmake: replace FindPython{Interp,Libs} with FindPython3
- LibOSDP: Consume logger from utils so we don't own that module
- libosdp: Remove -Werror; seems a bit excessive :)
- libosdp: Set an example by not using osdp_cp_setup()
- PD: Prevent out of order CMD_SCRYPT for safety
- libosdp: Mark osdp_cp_setup() as deprecated
- CP: Expose new API osdp_cp_setup2() to discourage use of master_key
- Phy: Nack secure messages received without an active SC session

## Fixes

- Fix an overflow bug in osdp_phy_check_packet()
- Add null checks on struct osdp_file pointer
- pyosdp: Fix handling of temporary flag in LED command
- pytest: Add LED command temporary: False test
- pyosdp: Expose set/clear flags method in the python wrapper
- CP fixup needed when communicating with hardware PD
- Fixed wrong command ID while logging "REPLY_PDID" in OSDP_CP_STATE_IDREQ
- Fix copy-paste issue in cmake/FindMbedTLS.cmake
