---
lastUpdated: 2026-11-26
description: "LibOSDP pd_info_t reference — configure an OSDP device address, baud rate, capabilities, and secure channel keys when creating a CP or PD context."
---
# PD Info

`osdp_pd_info_t` is a user-provided structure that describes how the PD is to
be set up. In CP mode, the app describes the PDs it wants to communicate with
using an array of `osdp_pd_info_t` structures.

<!-- doxygen:struct:osdp_pd_info_t:members -->

## Setup Flags

OSDP setup in CP or PD mode can be influenced by the following flags (set in
`osdp_pd_info_t::flags`). Some are effective only in CP or PD mode — see
individual flag documentation below. The `OSDP_FLAG_ENFORCE_SECURE` and
`OSDP_FLAG_INSTALL_MODE` flags are explained in detail under
[Secure Channel](/libosdp/secure-channel); `OSDP_FLAG_CAPTURE_PACKETS` is
covered in [Debugging](/libosdp/debugging).

<!-- doxygen:define:OSDP_FLAG_ENFORCE_SECURE -->

<!-- doxygen:define:OSDP_FLAG_INSTALL_MODE -->

<!-- doxygen:define:OSDP_FLAG_IGN_UNSOLICITED -->

<!-- doxygen:define:OSDP_FLAG_ENABLE_NOTIFICATION -->

<!-- doxygen:define:OSDP_FLAG_CAPTURE_PACKETS -->

<!-- doxygen:define:OSDP_FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK -->

<!-- doxygen:define:OSDP_FLAG_BIOREADR_MULTIPART -->

## Command and event data ownership

LibOSDP queues submitted commands and events **by reference** — it does not copy
them. The payload you submit must outlive the library's use of it, and ownership
returns to you through a completion callback. This applies to
[`osdp_cp_submit_command`](/api/control-panel#commands) and
[`osdp_pd_submit_event`](/api/peripheral-device#events); see those pages for the
exact contract.
