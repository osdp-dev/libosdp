---
release_date: 2019-12-15
release_version: v0.3-beta-2
---

Release notes for v0.3-beta-2.

## Features

- Added new tool osdpctl to create, manage and control osdp devices
- Add PD context to CP logs for clarity

## Enhancements and fixes

- Fix PD stale pointer issue in osdp_setup
- Fix MAC generation bug in Secure Channel
- Fix Data offset bug in Secure Channel
- Fix issues reported by -Wimplicit-fallthrough
- Fix memleak in PD command queue alloc
- Fix wrap around bug in cmd_queue
- Fix many clang static code analysis issues
