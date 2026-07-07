---
release_date: 2021-09-19
release_version: v2.0.0
---

This major release brings support for new features added in IEC Edition 1.0 of
the OSDP specification and other bug fixes and API changes.

## Enhancements

- pytest: Add test coverage in pytest
- sc: Fix API inconsistency in compute_session_keys()
- libosdp: Deprecate ad-hoc unit testing
- libosdp: Document members of struct osdp and struct osdp_pd
- pyosdp: Add support for file transfer command
- CP: Add public method to get PD ID and capability
- Tiny-AES: Update source files to latest version
- libosdp: Add support for MbedTLS
- libosdp: Cleanup error handling between phy and CP/PD
- Deprecate Master key based key derivation
- Add support for OSDP File Transfer
- Advertise and decode peer_size capability
- Add support for CMD_ABORT, CMD_ACURXSIZE, CMD_KEEPACTIVE
- Add support for additional baud rates 19200 and 230400

## Fixes

- libosdp: Change API to handle any number PDs in get_(sc_)status_mask
- libosdp: Double SC timeout to be a bit more flexible
- PD: Fix bug when phy wants wait for more data by PD discards rx_buf
- PD: Fix packet trace logging of sent bytes
- libosdp: USE_SCBK-D has a higher precedence in choice of SCBK
- file_tx: Many fixes and enhancements
- libosdp: Fix slab_free() assertion bug in release builds
- SC: Fix SCBK clobber in osdp_compute_session_keys()
- libosdp: Discard Secure Channel if a KEYSET is ACKed in plaintext
- CP: Fix packet consumption from rx_buffer issue
- cp: Allow applications to KEYSET with SCBK or master_key
- Fix: Allow keyset with SCBK; guard master_key route with ENFORCE_SECURE
- Ignore reply packet coming from another PD.
- Fix the master key check in osdp_cp_setup()
