---
release_date: 2020-06-28
release_version: v1.1.0
---

This release bumps up the minor number as the contract has changed. It
introduces 2 major changes (first 2 in enhancements). The rest is all
re-organization and fixes.

## Enhancements

- In PD Change app notification of incoming commands to polling to simplify API
- Replace circular buffer command queue with linked list queue
- Add SC status and status query methods
- Add assert guards for exposed methods
- Cleanup osdp.h by splitting it into multiple files
- Split cmake rules into subdirectories

## Fixes

- Add PD address to REPLY_COM in pd_build_reply
- Fix fd leak in read_pid
- Fix missing null char at atohstr()
- Fix memory under-alloc due to operator precedence issues
