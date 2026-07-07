---
release_date: 2024-02-20
release_version: v3.0.1
---

A quick follow up release adding a few key features that got left out in 3.0.0.
Most notably, packet trace and data trace infrastructure has been reworked and
a custom Wireshark protocol dissector for OSDP was added. Additionally, Some
efforts has been made to support Windows as a build platform.

## Enhancements

- libosdp: Add Packet scan skipped bytes instrumentation
- pcap: Move declarations to a separate header
- doc: Update info about dissector loading on windows
- libosdp: Add support for windows builds
- dissector: Append command/reply name to tree view for better analysis
- libosdp: Migrate rust code to a dedicated repo
- misc: dissector: Add support for data tracer
- libosdp: data_trace: Fix bug in packet length passed to tracer
- libosdp: pcap: Log the number of packets captured
- libosdp: pcap: Bump utils submodule to fix issues
- libosdp: Extend tracing infrastructure to DATA_TRACE
- libosdp: Add timestamp to trace files for uniqueness
- libosdp: Add a custom protocol disector for WireShark
- libosdp: Switch to pcap based packet tracing

## Fixes

- libosdp: Fix packet scan skipped bytes instrumentation
- Repaired references to osdp_millis_now()
- doc: Update debugging.rst and README.md with new tracing changes
- examples: python: Fixup some more minor issues
- libosdp: Rename samples/ as examples/ as it sounds better
- examples: python: Fix CP and PD examples for the higher level osdp module
- CI: Allow publish-pypi.yml to be activated on manual triggers
- CI: Update python publishing to multilinux
