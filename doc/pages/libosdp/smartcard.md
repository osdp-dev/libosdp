---
lastUpdated: 2026-07-18
description: "Use the OSDP smartcard commands in LibOSDP — retrieve PIV objects and run GENAUTH/CRAUTH cryptographic challenges against a PD's smartcard, with multi-part fragmentation handled by the library."
---
# Smartcard Commands

OSDP defines three commands that let a CP talk to a smartcard attached to a
PD: `CMD_PIVDATA` retrieves the contents of a PIV object, and
`CMD_GENAUTH` / `CMD_CRAUTH` direct a cryptographic challenge to the card and
collect its response. LibOSDP implements all three, on both the CP and the PD
side.

The payloads involved — PIV certificates, cryptographic challenges and their
responses — are usually larger than a single OSDP packet. The protocol handles
this with *multi-part messages* (spec section 5.10): the payload is fragmented
on the wire with a total/offset/length header and reassembled by the receiver.
LibOSDP does all of this internally; applications only ever see complete
payloads.

| Command       | Payload direction               | Reply            |
|---------------|---------------------------------|------------------|
| `CMD_PIVDATA` | 5-byte request (single packet)  | `REPLY_PIVDATAR` (multi-part) |
| `CMD_GENAUTH` | Challenge (multi-part)          | `REPLY_GENAUTHR` (multi-part) |
| `CMD_CRAUTH`  | Challenge (multi-part)          | `REPLY_CRAUTHR` (multi-part)  |

## CP side

Submit the command like any other; the reassembled reply arrives as an event
on the CP event callback once the PD has answered:

```c
struct osdp_cmd cmd = {
    .id = OSDP_CMD_PIVDATA,
    .pivdata = {
        .oid = { 0x5f, 0xc1, 0x02 },  /* NIST SP 800-73-4 object id */
        .element = 7,
        .offset = 0,
    },
};
osdp_cp_submit_command(ctx, pd_idx, &cmd);
```

```c
int event_callback(void *arg, int pd, struct osdp_event *ev)
{
    if (ev->type == OSDP_EVENT_PIVDATAR) {
        /* ev->piv_reply.data / ev->piv_reply.length */
    }
    return 0;
}
```

`CMD_GENAUTH` and `CMD_CRAUTH` use `struct osdp_cmd_auth` (algorithm, key
reference, and the challenge bytes) and answer with `OSDP_EVENT_GENAUTHR` /
`OSDP_EVENT_CRAUTHR`, which carry the same `struct osdp_event_piv_reply`
payload.

Only one smartcard operation can be in flight per PD at a time (the spec
forbids interleaving multi-part transfers), and a smartcard operation cannot
run concurrently with a file transfer. A second submission while one is active
fails immediately.

## PD side

The PD must advertise `OSDP_PD_CAP_SMART_CARD_SUPPORT`; commands arriving at a
PD without it are NAKed. The command is delivered through the command
callback with the complete (reassembled) payload, and the application answers
by submitting the matching reply event:

```c
int command_callback(void *arg, struct osdp_cmd *cmd)
{
    if (cmd->id == OSDP_CMD_GENAUTH) {
        struct osdp_event ev = {
            .type = OSDP_EVENT_GENAUTHR,
            .piv_reply = { .length = resp_len },
        };
        memcpy(ev.piv_reply.data, resp, resp_len);
        osdp_pd_submit_event(ctx, &ev);  /* inline reply */
    }
    return 0;
}
```

Submitting the event from within the callback sends it as the direct reply.
If the card needs time, return 0 without submitting — the command is ACKed and
the reply event can be submitted later; the CP keeps polling and picks it up
when it appears. A CP-side watchdog (`OSDP_PIV_OP_TIMEOUT_MS`, default 10 s)
aborts an operation whose reply never materializes.

## Payload limits

Command and event payloads are bounded by `OSDP_PIV_DATA_MAX_LEN` (default
256 bytes; a compile-time knob on both sides). Raise it at build time if your
PIV objects or challenges are larger — the wire format itself allows up to
64 KiB per transfer.

## Progress notifications

When notifications are enabled (`OSDP_FLAG_ENABLE_NOTIFICATION`), each
operation reports its lifecycle through the multi-part notifications
`OSDP_NOTIFICATION_MP_START`, `MP_PROGRESS` and `MP_DONE`, tagged with the
message family (`OSDP_MP_MSG_PIV`, `OSDP_MP_MSG_GENAUTH` or
`OSDP_MP_MSG_CRAUTH`). `MP_DONE` fires exactly once per operation with the
outcome — including for operations that abort (PD refusal, timeout,
`CMD_ABORT`, or the link going down). File transfers and multi-part biometric
replies report through the same notifications. See
[Multi-part Messages](/protocol/multipart#transfer-lifecycle-and-notifications)
for the full lifecycle and outcome codes.

## Transparent Reader Support (TRS)

The PIV commands on this page are *mediated*: the CP asks for an object or
hands over a challenge, and the PD deals with the card. OSDP also defines a
*transparent* mode (`CMD_XWR` / `REPLY_XRD`, IEC 60839-11-5) where the CP drives
the smartcard directly — it opens a card session, exchanges raw ISO 7816 APDUs
with the card, can run secure PIN entry on the reader, and closes the session
when done.

The two models complement each other — it is good to have both:

| | PIV commands (this page) | TRS (transparent mode) |
|---|---|---|
| Card logic lives in | the PD | the CP |
| Interaction shape | one request, one (multi-part) reply | a session of APDU exchanges |
| CP needs card know-how | no — asks for objects/challenges | yes — speaks ISO 7816 to the card |
| Typical use | PIV object retrieval, card authentication | custom applets, EMV flows, PIN entry |

If the PD firmware understands the card, the PIV commands keep the CP simple.
When the CP application must own the card conversation — nonstandard applets,
multi-step protocols, PIN ceremonies — TRS hands it the raw card channel.

TRS is compiled in when LibOSDP is built with `OPT_BUILD_OSDP_TRS`.

### The command surface

The CP drives a card through `OSDP_CMD_XWR` commands, each carrying a
`struct osdp_trs_cmd` whose `command` selects one of:

| `enum osdp_trs_cmd_e` | Action |
|-----------------------|--------|
| `OSDP_TRS_CMD_START`     | Open a card session on the reader |
| `OSDP_TRS_CMD_SEND_APDU` | Send a C-APDU (`struct osdp_trs_apdu`) to the card |
| `OSDP_TRS_CMD_ENTER_PIN` | Run a secure EMV PIN-entry ceremony on the reader |
| `OSDP_TRS_CMD_CARD_SCAN` | Scan the field for a card |
| `OSDP_TRS_CMD_STOP`      | Close the card session |

A C-APDU travels in `struct osdp_trs_apdu` (`length` + `data[OSDP_TRS_APDU_MAX_LEN]`).
PIN entry uses the richer `struct osdp_trs_pin_entry`, which describes the PIN
block format, digit limits, completion conditions, and prompt messaging the
reader enforces locally so the PIN never crosses the bus in the clear.

### Sessions

TRS commands are submitted as a **session band**: a `START`, the
`SEND_APDU`/`ENTER_PIN`/`CARD_SCAN` exchanges, and a closing `STOP`, all queued
in order. The whole band must be submitted before the CP takes the bus — it is
all-or-nothing on `STOP`, and while a session is open only TRS commands are
accepted. To abandon a session, call
[`osdp_cp_flush_commands`](/api/control-panel#commands) and then submit `STOP`;
the flushed APDUs complete with `OSDP_COMPLETION_FLUSHED`.

The reader's answers arrive as `OSDP_EVENT_TRS` events carrying a
`struct osdp_trs_reply`:

| `enum osdp_trs_reply_e` | Payload |
|-------------------------|---------|
| `OSDP_TRS_REPLY_CARD_INFO`    | Card entered the field — protocol, CSN, ATR/ATS |
| `OSDP_TRS_REPLY_CARD_PRESENT` | Card presence/status change |
| `OSDP_TRS_REPLY_CARD_DATA`    | An R-APDU from the card |
| `OSDP_TRS_REPLY_PIN_COMPLETE` | PIN-entry ceremony finished (status, tries) |
| `OSDP_TRS_REPLY_ERROR`        | Reader-reported error |

The session life-cycle itself is reported through
`OSDP_NOTIFICATION_TRS_STATUS`, whose payload is a
`struct osdp_notification_trs_status`; its `status` field is an
`enum osdp_trs_session_status_e`. A CP app sees `OPENED` when transparent
mode is entered, `CLOSED` when a `STOP` is honored, and `FAILED` when a
session cannot be established or is cut short by a link error. A PD app is
told too, since the library drives the reader's mode itself: `OPENED` and
`CLOSED` as the CP changes the reader's mode, including on a link restart,
and `CARD_RELEASED` when the CP terminates the card.

```c
/* One transparent-card session: open, select an applet, close. */
struct osdp_cmd start = { .id = OSDP_CMD_XWR, .trs = { .command = OSDP_TRS_CMD_START } };
osdp_cp_submit_command(ctx, pd_idx, &start);

struct osdp_cmd apdu = {
    .id = OSDP_CMD_XWR,
    .trs = {
        .command = OSDP_TRS_CMD_SEND_APDU,
        .apdu = { .length = select_len },
    },
};
memcpy(apdu.trs.apdu.data, select_apdu, select_len);
osdp_cp_submit_command(ctx, pd_idx, &apdu);   /* R-APDU arrives as OSDP_EVENT_TRS */

struct osdp_cmd stop = { .id = OSDP_CMD_XWR, .trs = { .command = OSDP_TRS_CMD_STOP } };
osdp_cp_submit_command(ctx, pd_idx, &stop);
```

### APDU capacity

A C- or R-APDU is bounded by `OSDP_TRS_APDU_MAX_LEN` (default 258 — a 255-byte
short-APDU body plus status word, sized for PIV-class GET RESPONSE chains). This
buffer is embedded in the command and event structs, so it is also the memory
knob; raise it at build time for extended-length APDUs. Carrying full-size
chunks also needs the OSDP packet buffer widened (`OSDP_PACKET_BUF_SIZE`, default
256). TRS is a transparent tunnel — it does no APDU chaining of its own, so an
over-large C-APDU is rejected at submission; ISO 7816-4 command chaining is the
application's responsibility. The usable APDU size is also bounded by the PD's
negotiated maximum packet size, so a C-APDU that fits the buffer can still be
rejected if the link's packet size is smaller.

## Python

The same flow is available from the Python bindings — see
[Commands](/python/commands#smartcard) and [Events](/python/events#smartcard)
for the `PivData`, `GenAuth` and `CrAuth` classes.
