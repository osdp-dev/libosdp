---
release_date: 2020-08-15
release_version: v1.2.0
---

This release marks the first HEAD of initial PD support in Zephyr RTOS upstream
(https://github.com/zephyrproject-rtos/zephyr). Lot of changes to coding style
were made to upstream the PD sources without too much variation to the one here.

## Enhancements

- Add support for sphinx documentation builds see: https://libosdp.gotomain.io
- Add a macro switch to disable Secure Channel
- Add utils git-submodule and delete all copied ad-hoc utils from this repo
- Add pkg-config file for libosdp
- PD: When seq number 0 is received, invalidate SC status
- Add github actions workflows for tests and release
- Rewrite PD and CP fsm for zephyr integration
- osdpctl: rewrite rs232.c into utils/serial.c
- osdpctl: remove pid file in osdpctl stop. Also check return code of kill
- Produce source and binary tarballs with cPack
- Bring changes from zephyr PD, post review and apply it to CP as well
- Control the externally exposed library symbols using `EXPORT` macro

## Fixes

- Make all internal methods static; scope all globals with `osdp_`
- Add colors for various log levels of libosdp
- Refactor all reference to struct osdp_pd as 'pd' everywhere
- Fix static analysis issues identified by Xcode
- travis: switch to Ubuntu 18.04 bionic for testing
- osdpctl: Fix bug in `hex2int()`
- osdpctl: Fix bug in load_scbk(); test key_store feature
- osdpctl make config as the first argument for all commands
- Fix sequence number 0 sent by CP does not reset connection in PD
- osdpctl: Make PD message queue channel owner, responsible for cleanup
- osdpctl: Fix segfault on channel cleanup code
