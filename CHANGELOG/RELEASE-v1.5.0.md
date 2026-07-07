---
release_date: 2021-04-17
release_version: v1.5.0
---

This release marks the end of OSPD specification v2.1.6 support. Post this
release, LibOSDP would start supporting new features added in IEC Edition 1.0
specification.

## Features

- libosdp: Introduce a lean-build system
- tests: Add a command level unit test executor
- libosdp: Add new command complete callback

## Enhancements

- libosdp: Add cmake option CONFIG_OSDP_STATIC_PD (configure --static-pd)
- pd: Use better NAK error code when rejecting a command due to SC inactive
- LibOSDP: Split build/send and receive/decode methods
- Add new typedef and document osdp_log_fn_t
- Remove implicit logging via printf()
- Avoid allocs by switching log buf to static char[]
- Add compile time switch to disable colours in osdp logs
- Add Makefile target to clone utils submodules at init
- phy: Allow non-conformant, 0 length, encrypted data blocks
- cp: Add bounded exponential back-off for offline retries
- cp: Add detection of a busy reply with sequence number 0
- Mark some methods with __weak for portability

## Fixes

- Clean src/osdp_cp.o even if it is not built due to --single-pd
- Just fix the description comment for OSDP_PD_NAK_SEQ_NUM
- Call osdp_logger_init() before setting up CP/PD for early logging
- Fix off-by-one at vsnprintf()'s return value assertion
- Move osdp_millis_*() into utils::utils.c
- Move unistd.h into CONFIG_DISABLE_PRETTY_LOGGING guard
- Enable github workflows on lean build to keep them in sync
- Fix coverity identified off-by-one out-of-bound array access
- Fix coverity identified logical dead-code
- sc: Fix potential out-of-bound buffer access
- cp: Bypass unneeded packet decoding for a busy reply
- cp: Don't regenerate random numbers for the challenge command
- cp: Add the OSDP_ONLINE_RETRY_WAIT_MS timeout
- pd: Fix device capabilities report
- Call to osdp_dump was using incorrect length
