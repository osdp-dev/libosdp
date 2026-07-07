---
release_date: 2020-09-16
release_version: v1.3.0
---

With this release, LibOSDP now has python bindings. It is possible to setup a
device as a Control Panel or Peripheral Device through python. Also, PD now
supports alternate replies in the form of OSDP events.

## Enhancements

- libosdp: Add flag ENFORCE_SECURE to make secure assumptions
- libosdp: Resolve PD address / offset inconsistencies
- libosdp: Remove CONFIG_OSDP_SC_ENABLED macro switch
- libosdp: deprecate osdp_cp_send_cmd_*() methods in favour of osdp_cp_send_command()
- libosdp: add PD app command callback support
- Make OSDP_FLAG_INSTALL_MODE a setup time flag
- Add support for manufacturer specific commands
- Add support for osdp_FMT, osdp_RAW, osdp_KEYPAD and handle their osdp_events
- Move command alloc/queue API into CP region
- Add checks that a pd is online before queueing commands.
- osdpctl: consume utils/channel.c and remove native impl
- libosdp: Add support for callback data in notifiers

## Fixes

- Fix srand issue in tests, pyosdp, and osdpctl
- PD: Fix bug PD_FLAG_SC_ACTIVE not removed when PD goes offline
- PD: always check the timeout on receiving packets
- libosdp: Deprecate command queues for PD mode
- Add osdp_events and make changes globally
- Fix multiple checkpactch issues found in zephyr upstream
- Move PD offset validation above context dereference
- Move COMSET out of CONFIG_OSDP_SC_ENABLED macro guard
- libosdp: fix COMSET command regression
- Refactor some command struct members for consistency
- Fix python memory leak and python keyset error check.
- libosdp: Fix bug in PD_MASK() macro
- libosdp: fix missing cmd_dequeue return check
- Rebuild utils with -fPIC to fix gcc/linux build issues
- doc: Add protocol/faq update README.md
- Consume slab allocation from utils and remove native implementation
- Return aligned memory from slab allocator.
- Fix doc links in README.md
- Fix multiple broken links in doc/ after sphinx move
- Update AES to to latest kokke/tiny-AES-c
