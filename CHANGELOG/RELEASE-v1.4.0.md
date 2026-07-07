---
release_date: 2021-01-31
release_version: v1.4.0
---

This relase brings in a lot of bug fixes and some minor enhancements.

## Enhancements

- doc: Add Doxygen and breathe to talk with sphinx
- LibOSDP: Add osdp.hpp and cpp samples
- cp: Add new flag FLAG_SC_DISABLED when master_key == NULL
- Add support for adpative MARK bytes on packets
- libosdp: consume only processed bytes from rx_buf
- test: Get test log level from test context
- Add a toplevel cmake offload makefile
- PD: Add API osdp_pd_set_capabilities()
- PD: Add CAP validation on received commands
- PD: Allow only specific commands in plain text when ENFORCE_SECURE
- Consume new hexdump API; Modify packet_trace logs
- Move src/include/* to src/ for simplicity
- PD: Reset SC active status bit when there is a timeout
- pyosdp: promote set_loglevel() as osdp class static method
- libosdp: Add support for multiple PDs connected in a single channel
- Add compile time switch CONFIG_OSDP_SKIP_MARK_BYTE
- Allow LibOSDP to export its build rules to other modules
- libosdp: Optionally, find and use openssl if available

## Fixes

- pyosdp: cp: change type of master_key to python bytes
- cp: Fix master keyset functionality
- libosdp: cleanup logs and remove some duplicate code
- Fix python warnings -- bugs
- Disambiguate logging macro from the loglevel enum entry
- CP: Fix some ENFORCE_SECURE holes
- Reset seq_number before moving to ONLINE without SC
- fix: map the public osdp_cmd_e id codes when sending commands to the OSDP API function osdp_cp_send_command()
- fix in using correct context pointer for PD (pd_ctx instead of cp_ctx)
- The PD address 0 is also a valid address!
- Fix the check of the checksum of received packets.
- Avoid receiving invalid data by flushing rx before each send.
- do not flush rx queue when skipping commands. when skipping commands addressed to other nodes the rx queue should not be flushed. The complete command is already received and flushing the rx queue only risks removing bytes in the next command.
- PD: Discard SC flag on MAC verification and decryption errors
- Fix REPLY_RAW card data length bug - Related-To: #31
- Fix constant time compare in osdp_ct_compare()
- pyosdp: fix untested PD capabilities code flow
- Fix CONFIG_OSDP_PACKET_TRACE that was flooding logs with osdp_POLL
- cp: Fix unexpected reply condition missing break
- Fix osdp_event_cardread length bits/bytes mixup
