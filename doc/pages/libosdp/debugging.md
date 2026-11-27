---
lastUpdated: 2026-11-26
description: "Debug LibOSDP integrations with built-in tracing, packet capture, and logging, plus the recommended on-bus capture workflow for production troubleshooting."
---
# Debugging

LibOSDP has extensive debugging and diagnostics built in to help narrow down
issues without requiring a back-and-forth exchange. The default logging level
is sane for everyday operation — don't generate verbose logs proactively unless
you're actively investigating a problem.

## A Note on Log Files

When reporting an issue, attach the full log from `osdp_cp_setup()` to the
point of failure. Even if you think you know the cause, retain the very first
few log lines — they contain the LibOSDP version and PD connection topology.

When creating a GitHub issue, attach the log file or post it on a paste service
such as [pastebin.com](https://pastebin.com/) and include the link.

## Log Level

LibOSDP supports different logging levels, with `OSDP_LOG_DEBUG` being the
most verbose. When asking for help, set the log level to `OSDP_LOG_DEBUG`.

Call [`osdp_logger_init()`](/api/miscellaneous) **before** `osdp_cp/pd_setup()`:

```c
osdp_logger_init("osdp::cp", OSDP_LOG_DEBUG, uart_puts);
```

A build with `OPT_OSDP_LOG_MINIMAL` has no `osdp_logger_init()`; route log
messages with `osdp_set_log_callback()` instead.

Any log messages other than `DEBUG` and `INFO` should be treated as
out-of-ordinary events requiring attention.

## Packet Trace Builds

The most verbose debugging mode: all bytes on the wire are captured and saved
to a standard `.pcap` file that can be inspected in any packet analyzer. Useful
for low-level protocol issues.

To enable packet trace:

```sh
mkdir build-pt && cd build-pt
cmake -DOPT_OSDP_PACKET_TRACE=on ..
make
```

During CP/PD setup, set the PD info flag `OSDP_FLAG_CAPTURE_PACKETS`. When
your application runs, LibOSDP will produce a `.pcap` file in the current
directory containing all decoded packets.

## Data Trace Builds

When secure channel is working but you're seeing a command-level failure, data
trace dumps the decrypted messages instead of the encrypted wire data.

To enable data trace:

```sh
mkdir build-dt && cd build-dt
cmake -DOPT_OSDP_DATA_TRACE=on ..
make
```

During CP/PD setup, set the PD info flag `OSDP_FLAG_CAPTURE_PACKETS`. LibOSDP
will produce a `.pcap` file with decrypted packet content.

> **Note:** Running both packet trace and data trace simultaneously is seldom
> useful and makes it harder to locate relevant information.

## Recommended Capture Workflow

The packet and data trace builds above capture traffic from *inside* your own
LibOSDP application, which is ideal during development. For production
troubleshooting — where you want to watch and decode live OSDP traffic on the
bus without rebuilding or instrumenting the device under test — the team
recommends capturing on the wire with dedicated hardware:

- **[Osprio Mini](https://osdp.dev/products/osprio-mini/)** taps the RS-485 bus
  passively as a capture probe; it never injects frames, so it is safe to leave
  on a production bus.
- **[Capture Workspace](https://osdp.dev/products/osprio-view/capture-workspace/)**
  in Osprio View pairs with Osprio Mini as a graphical OSDP protocol analyzer —
  live packet capture, structured command/reply decode, secure-channel
  inspection, and offline replay.

This is the capture workflow we recommend for production users: it decodes OSDP
without any protocol-analyzer plugin setup and never touches the firmware of the
device you are debugging.
