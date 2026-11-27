---
lastUpdated: 2026-11-27
title: "osdpctl (Historical)"
description: Historical documentation of osdpctl, a retired LibOSDP CLI for OSDP devices, superseded by Osprio Mini and Osprio View.
---

# osdpctl (Historical)

:::warning Historical
osdpctl is no longer maintained and there are no plans to rewrite it. This
page documents the tool as it last shipped, in LibOSDP v2.4.0. For new work,
use Osprio Mini and Osprio View, described below.
:::

## What replaced osdpctl

osdpctl was meant to grow into an OSDP device manager. We considered that
direction and discarded it: a command-line tool driven by configuration files
could not deliver all the features we had in mind for building and debugging
OSDP devices.

[Osprio Mini](https://osdp.dev/products/osprio-mini/) and
[Osprio View](https://osdp.dev/products/osprio-view/) now cover everything
osdpctl was intended for, and do it better:

- **Osprio Mini** is a hardware OSDP companion. It acts as the ACU (control
  panel) while you develop a PD, or as the PD while you develop an ACU, and
  gives you a tested baseline for comparing devices across vendors.
- It exposes every LibOSDP feature the way it is intended to be used, through
  the **Osprio View** web app. View drives the OSDP transactions and gives you
  extensive debugging tools while it does.

The rest of this page describes osdpctl as it was.

osdpctl is a tool that uses libosdp to setup/manage/control osdp devices. It
also serves as a starting point for those who intend to consume this library.
It cannot be used directly in applications as most of the time a lot more
product specific customizations are needed.

This tool brings in a concept called **channel** to describe the communication
between a CP and PD. Although OSDP defines this protocol to run on RS485
(serial), it is possible to run this over other mediums too. From a testing
perspective, running on a unix IPC channel is very convenient. To know more
about how this is achieved, look at `channel_*.c` files in this directory.

## Configuration files

Since OSDP requires a lot of configuration information to setup a PD/CP, its not
practical to pass all of them from the command line (I tried). So the tool uses
a configuration file (ini format) to get the settings needed to configure the
OSDP library (libosdp). This file determines if the service is to run as a CP or
PD. You can read more about the configuration file and the various keys it can
contain in the [configuration section](/osdpctl/configuration).

Some sample configuration files can be found inside the
[`config/` directory](https://github.com/osdp-dev/libosdp/tree/v2.4.0/osdpctl/config).

## Start / Stop a service

An OSDP service can be started by passing a suitable config file to `osdpctl`
tool. You can pass the `-f` flag to fork the process to background and the `-l`
flag to log output to a file or `-q` to disable logging. So to start a OSDP
process as a daemon, you can do the following:

```bash
osdpctl pd-0.cfg start -f -l /tmp/pd-0.log
```

To stop a running service, you must pass the same configuration file that was
used to start it.

```bash
osdpctl pd-0.cfg stop
```

## Send control commands to an OSDP service

A command to be sent to a running CP/PD service must be of the following format.
Some of these commands will in-turn be sent by the CP/PD device to its connected
counterpart over OSDP as a command/event.

For instance, if you send a LED command to a CP instance specifying a correct PD
offset, then that command is sent over to the relevant PD.

```
osdpctl <CONFIG> send <PD-OFFSET> <COMMANDS> [ARG1 ARG2 ...]
```

Here, `PD-OFFSET` is the offset number (starting with 0) of the PD in the config
file `CONFIG`. In PD mode, since there is only one PD, this is always set as 0.

This section will only document the `COMMANDS` and their arguments. You must
prefix `osdpctl <CONFIG> send <PD-OFFSET>` to each of these commands for them to
actually get through.

### CP commands

The following commands can be passed to a OSDP device that is setup as a CP. The
PD to which these commands are being sent must have the capability of executing
them. Refer to the [PD capabilities document](/protocol/pd-capabilities) for
more details.

#### LED

This command is used to control LEDs in a PD. It is of the format:
`led <led_no> <color> <blink|static> <count|state>`.

Examples:

```
led 0 red blink 5        # blink LED number 0 in red color for 5 times
led 1 amber blink 0      # blink LED number 1 in amber color forever
led 2 green static 1     # Turn on LED number 2 green color
led 1 blue static 0      # Turn off LED number 1 blue color
```

#### Buzzer

This command is used to control Buzzers in a PD. It is of the format:
`buzzer <blink|static> <count|state>`.

Examples:

```
buzzer blink 0          # beep the buzzer forever
buzzer blink 5          # beep the buzzer 5 times
buzzer static 1         # turn off the buzzer
buzzer static 0         # turn on the buzzer
```

#### Output

This command is used to control outputs in a PD. It is of the format:
`output <output_number> <state>`.

Examples:

```
output 0 1        # Set output number 0 high
output 2 0        # Set output number 2 low
```

#### Text

This command is used to control the text that is displayed on the PD. It is of
the format: `text <string>`.

Examples:

```
text 'Hello World'     # Set text "hello world" in display
```

#### Communication Params set

This command is used to set the communication parameters of a connected PD. It
is of the format: `comset <address> <baud_rate>`.

Examples:

```
comset 12 115200  # Set PD address to 12 and baud rate to 115200
```
