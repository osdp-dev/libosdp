---
lastUpdated: 2026-10-29
description: "LibOSDP peripheral device (PD) C API — implement an OSDP PD, advertise capabilities, respond to control panel commands, and report events."
---
# Peripheral Device

The following functions are used when OSDP is used in PD mode. The library
returns a single opaque pointer of type `osdp_t` where it maintains all
internal data. All applications consuming this library must pass this context
pointer to all API calls.

## Device Lifecycle Management

The PD is described to `osdp_pd_setup` with an [`osdp_pd_info_t`](/api/pd-info)
structure.

<!-- doxygen:typedef:osdp_t -->

<!-- doxygen:function:osdp_pd_setup -->

<!-- doxygen:function:osdp_pd_refresh -->

<!-- doxygen:function:osdp_pd_teardown -->

## PD Capabilities

See [PD capabilities](/protocol/pd-capabilities) for the full list of function
codes and compliance levels a PD can advertise.

<!-- doxygen:struct:osdp_pd_cap:members -->

<!-- doxygen:enum:osdp_pd_cap_function_code_e -->

<!-- doxygen:function:osdp_pd_set_capabilities -->

## Commands

<!-- doxygen:typedef:pd_command_callback_t -->

<!-- doxygen:function:osdp_pd_set_command_callback -->

Refer to the [command structure](/api/command-structure) document for more
information on how the `cmd` structure is framed.

::: info
For `CMD_MFG`, callback return values use normal ACK/NAK behavior (negative
return → NAK, non-negative return → ACK). Any manufacturer-specific reply
payload must be sent asynchronously by submitting an `OSDP_EVENT_MFGREP` event
with `osdp_pd_submit_event`.
:::

## Events

When a PD app has an event (card read, key press, etc.) to report to the CP,
it creates the corresponding event structure and calls `osdp_pd_submit_event`
to deliver it to the CP on the next `osdp_POLL` command.

<!-- doxygen:function:osdp_pd_submit_event -->

::: warning Event lifetime — queued by reference
An accepted event is queued **by reference**; LibOSDP does not copy it. The
`event` you pass must stay alive and unmodified from a successful submission
until the library is done with it. An event that lives on the stack of a function
that returns (a command callback, say), or one reused for a second submission
while the first is still queued, corrupts the queue.

Ownership returns to your application when the event **completion callback** fires
for that pointer (register it with `osdp_pd_set_event_completion_callback`). Every
accepted event is reported exactly once — including those dropped by
`osdp_pd_flush_events` (`OSDP_COMPLETION_FLUSHED`) and `osdp_pd_teardown`
(`OSDP_COMPLETION_ABORTED`) — so a heap-allocated event should be freed in that
callback. On a `-1` return the event was never queued and is yours to reuse at
once.
:::

<!-- doxygen:function:osdp_pd_flush_events -->

A PD that bridges a smart-card reader answers each card command with an
`OSDP_EVENT_TRS` event (see
[Transparent Reader Support](/libosdp/smartcard#transparent-reader-support-trs)),
and bounds the
card answers it relays by:

<!-- doxygen:function:osdp_pd_trs_get_max_apdu_len -->

### Event Completion

Register a completion callback to learn when each submitted event leaves your
ownership. The outcome is reported as an
[`osdp_completion_status`](/api/control-panel#command-completion).

<!-- doxygen:typedef:pd_event_completion_callback_t -->

<!-- doxygen:function:osdp_pd_set_event_completion_callback -->

Refer to the [event structure](/api/event-structure) document for more
information on how to populate the `event` structure.
