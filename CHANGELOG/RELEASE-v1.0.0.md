---
release_date: 2020-05-29
release_version: v1.0.0
---

This is the first release of libosdp. It has been in pre-release for a
long time now and has been tested sufficiently well for a first release.
Rate of bug fixes has gone down significantly.

## Features

- Supports OSDP CP mode of operation
- Supports OSDP PD mode of operation
- Supports Secure Channel for communication
- Ships a tool: osdpctl to consume and library and setup/manage osdp devices
- Documents key areas in the protocol that are of interest for consumers of this library.
- So far, this library's CP implementation has been tested with multiple OSDP PDs including HID's devices and found to be working as expected. The PD implementation has been tested with it's own CP counterpart so this some level of confidence on the that too.
