---
release_date: 2024-02-14
release_version: v3.0.0
---

This Major release bring rust support, CP state machine cleanups, API cleanups,
and many other enhancemtns and fixes.

## Enhancements

- Add rust support (single entry for 80+ patches)
- Rewrite osdpctl in rust (single entry for many patches)
- chore: Update copyright year to 2024
- python: vendor LibOSDP sources when packaging
- python: Remove code referring to CMakeLists.txt version
- API, python: Add LED color constants from v2.2 of the spec
- doc: Update top-level README.md
- libosdp: cp: Move keyset completion handler to get_next_ok_state()
- tests: pytest: Add install_mode SCBK set test
- libosdp: Unify StatusReport query and event structs
- libosdp: cp: Move functionality away from command builders
- pytests: Remove timeout from CP status test
- python: Adapt to changes to status event/command handling
- libosdp: Rework status command and event handling
- libosdp: cp: Reword event callback mechanism to use requests
- libosdp: Permit REPLY_COM response for CMD_COMSET
- rust: Add method to make a pair of unix channels
- libosdp: Split events data maxlen macro as we have for commands
- libosdp: Reduce command/event pool size
- python: Add a get_file_tx_status() for PD
- test: unit-test: Add command and event callback logging for file tx command
- pyosdp: Move set_log_level() to be module method
- tests: unit-tests: Add initial skeleton for command test
- libosdp: cp: Rework CP state machine
- libosdp: cp: Remove some more master key related code
- libosdp: cp: Move call to cp_cmd_free into cp_translate_cmd
- libosdp: cp: Add BUG() macro and call for file TX cmd translate
- libosdp: cp: Decouple internal and external command queue
- cmake: utils,samples: Bump minimum required version
- libosdp: CP: Lower comeset reply log level to info
- libosdp: API: Do not indiate SC active when using SCBK-D
- libosdp: phy: Also ignore commands with PD address.MSB set
- libosdp: phy: Ignore reply with PD address.MSB cleared
- Increasing the max data contained in an OSDP enevt to 128bytes
- utils: Bump submodule to latest commit
- doc: Add some notes about make build
- libosdp: API: Remove osdp_set_command_complete_callback
- libosdp: Set PD address to 0x7F for broadcast packets
- libosdp: API: common: Increase PD offline status timeout to 600ms
- scripts: Unify release generation scripts into one place
- rust: Add workaround for osdp_config.h.in copy to OUT_DIR
- rust: cp: Add support for the new status command
- libosdp: cp: Add different query options for OSDP_CMD_STATUS
- libosdp: cp: Refactor API command handling (translating) logic
- python: Update README.me and samples after testing
- python: osdp: pd: Add support sc_wait() method
- python: osdp: Add support for non-blocking get_{command,event}
- cmake: Remove python/ subdirectory from chain
- python: Merge test/pytest/testlib as main python package
- libosdp: const-annotate some more public API
- libosdp: Const-ize parameters of exposed methods
- libosdp: Remove deprecated methods tree-wide
- libosdp: Add support for Magenta and Cyan LED colors
- libosdp: Demote osdp_logger_init to a macro alias

## Fixes

- utils: Bump submodule to fix build failure due to VLA
- Fix regression in Rust binding. Commit 51b626c94bc39242f9793c916471bdadafe24679 increased buffer size to 128 bytes. Adjust binding accordingly.
- make: Fix configure.sh for linux
- libosdp: Fix warnings that break rust build
- utils: Bump submodule to fix log line termination issue
- libosdp: phy: Fix PACKET_TRACE command byte offset
- libosdp: phy: Fix packet trace mark byte logging
- doc: security: Update doc to have latest info; fix a typo
- libosdp: cp: Fix broken keyset for an install_mode PD
- CP: API: Fix callback signature in cpp header
- python: osdp_sys: fix memory leak in pyosdp_add_pd_cap
- pytest: Fix run.sh to be invokable from other dirs
- python: Fix pd_info builder after const qualification of info->cap
- src/osdp_common.c: waring: _GNU_SOURCE redefined
- utils: version bump - pickup fixes for gcc warnings/errors
- unit-test: Fix broken build due to old API drop changes
- Fix potential null pointer deref at LOG_* call site
- cp: disallow unexpected SC responses
