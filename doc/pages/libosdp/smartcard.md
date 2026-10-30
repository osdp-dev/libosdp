---
lastUpdated: 2026-10-29
description: "Use the OSDP smartcard commands in LibOSDP — retrieve PIV objects, run GENAUTH/CRAUTH challenges, and drive a card directly with Transparent Reader Support (TRS): APDU sessions, presence scan and secure PIN entry."
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

TRS is part of every LibOSDP build, on both roles; there is nothing to enable.
The library owns the protocol mechanics — switching the reader in and out of
transparent mode, terminating the card at the end of a session, holding the bus
for a slow card, sizing APDUs to the link — so an application deals only in
cards and APDUs.

### A TRS conversation, end to end

Take a common setup: a contactless reader as the PD, and a CP application that
wants to read something from whatever ISO 14443-4 card is tapped on it. The
conversation goes like this:

1. **The PD says it can do it.** It advertises `OSDP_PD_CAP_SMART_CARD_SUPPORT`
   (compliance level 1, one reader). A PD without it NAKs every `CMD_XWR`.
2. **The CP learns the APDU budget.** Once the PD is online,
   `osdp_cp_trs_get_max_apdu_len()` returns the largest C-APDU the link can
   carry. Size your commands — and any `Le` you ask for — against it.
3. **The CP finds out a card is there.** Either the reader announces cards on
   its own, or the CP turns on the library's presence scan with
   `osdp_cp_trs_scan_enable()`. Either way, a card arriving is delivered as an
   `OSDP_EVENT_TRS` event carrying `OSDP_TRS_REPLY_CARD_INFO`: the card's
   protocol, its CSN (UID), and its ATR (contact) or ATS/ATQB (contactless).
4. **The CP opens a session** by submitting `OSDP_TRS_CMD_START`. If the scan
   is still holding the reader in transparent mode, the session adopts it with
   no extra traffic on the wire. An `OSDP_TRS_SESSION_OPENED` notification
   confirms the reader is ready.
5. **The CP talks to the card**, one `OSDP_TRS_CMD_SEND_APDU` at a time. Each
   answer arrives as an `OSDP_TRS_REPLY_CARD_DATA` event carrying the R-APDU
   (status word included), and is delivered *before* the command completes.
   The session stays open between commands, however long the application takes
   to decide what to send next.
6. **The CP closes the session** with `OSDP_TRS_CMD_STOP`. The library
   terminates the card transaction, restores the reader's default mode and
   raises `OSDP_TRS_SESSION_CLOSED`. With the scan on, the card leaving the
   field later is reported as an `OSDP_TRS_REPLY_CARD_PRESENT` event with
   `OSDP_TRS_CARD_NOT_PRESENT`.

The rest of this section fills in each step.

### CP side in C

Everything arrives on the two callbacks every CP already has. Enable
notifications (`OSDP_FLAG_ENABLE_NOTIFICATION`) to see the session life-cycle.

```c
static struct osdp_cmd start = {
    .id = OSDP_CMD_XWR, .trs = { .command = OSDP_TRS_CMD_START },
};
static struct osdp_cmd select_app = {
    .id = OSDP_CMD_XWR, .trs = { .command = OSDP_TRS_CMD_SEND_APDU },
};
static struct osdp_cmd stop = {
    .id = OSDP_CMD_XWR, .trs = { .command = OSDP_TRS_CMD_STOP },
};
static bool in_session;

int event_callback(void *arg, int pd, struct osdp_event *ev)
{
    if (ev->type == OSDP_EVENT_TRS) {
        switch (ev->trs.reply) {
        case OSDP_TRS_REPLY_CARD_INFO:
            /* A held card is announced again on every scan probe */
            if (in_session) {
                break;
            }
            /* A card is here: open a session and select an applet */
            in_session = true;
            osdp_cp_submit_command(ctx, pd, &start);
            select_app.trs.apdu.length = sizeof(SELECT_AID);
            memcpy(select_app.trs.apdu.data, SELECT_AID, sizeof(SELECT_AID));
            osdp_cp_submit_command(ctx, pd, &select_app);
            break;
        case OSDP_TRS_REPLY_CARD_DATA:
            /* R-APDU for the SEND_APDU about to complete; keep it */
            save_answer(&ev->trs.card_data.apdu);
            break;
        case OSDP_TRS_REPLY_ERROR:
            /* The reader could not complete the exchange */
            break;
        default:
            break;
        }
    } else if (ev->type == OSDP_EVENT_NOTIFICATION &&
               ev->notif.type == OSDP_NOTIFICATION_TRS_STATUS) {
        /* OPENED / CLOSED / FAILED, and the scan's own reports */
    }
    return 0;
}

void completion_callback(void *arg, int pd, struct osdp_cmd *cmd,
                         enum osdp_completion_status status)
{
    if (cmd == &select_app) {
        /* On OSDP_COMPLETION_OK the answer save_answer() kept is the
         * card's; act on it, then submit the next SEND_APDU or close.
         * A session that failed has already closed its band, so the
         * STOP is refused and there is nothing left to close. */
        if (osdp_cp_submit_command(ctx, pd, &stop) < 0) {
            in_session = false;
        }
    } else if (cmd == &stop) {
        in_session = false;
    }
}
```

Commands are queued **by reference** (see
[Control Panel](/api/control-panel#commands)), so each one must stay alive
until its completion callback fires — hence the statics above. Reusing a
command for the next APDU is fine once it has completed.

### Sessions (bands)

A card session is the *band* of commands from a `START` to its `STOP`, in
submission order:

    START, SEND_APDU, SEND_APDU, ..., STOP

The band can be submitted all at once, or built up one command at a time as
answers come back — the walk-through above does the latter. Between commands
the CP simply keeps polling, so the reader stays in transparent mode with the
card selected.

The band is enforced at submission, so misuse is an error from
`osdp_cp_submit_command()`, never a command that runs and has to be undone:

- Inside a band only TRS commands are accepted. An LED, buzzer or any other
  command would interrupt the card transaction, so it is refused until the
  `STOP` has been submitted.
- A card command outside a band is refused: the reader is not in transparent
  mode to receive it.
- A second `START` before the `STOP` is refused.

While a session is open the reader is in transparent mode, where it does not
report ordinary credential reads. Keep sessions short, and always close them.

To abandon a session along with the APDUs still queued in it, call
[`osdp_cp_flush_commands`](/api/control-panel#commands) and then submit `STOP`;
the flushed commands complete with `OSDP_COMPLETION_FLUSHED`. If the session
dies on its own before its `STOP` — the reader refuses transparent mode, or the
link drops — the rest of the band is completed `OSDP_COMPLETION_FLUSHED`, the
`STOP` included, and an `OSDP_TRS_SESSION_FAILED` notification says so. Commands
the application queued *after* the band keep their place and run as usual.

### Answers and errors

The reader's answers arrive as `OSDP_EVENT_TRS` events carrying a
`struct osdp_trs_reply`:

| `enum osdp_trs_reply_e` | Payload |
|-------------------------|---------|
| `OSDP_TRS_REPLY_CARD_INFO`    | Card entered the field — protocol, CSN, ATR/ATS |
| `OSDP_TRS_REPLY_CARD_PRESENT` | Card presence/status change |
| `OSDP_TRS_REPLY_CARD_DATA`    | An R-APDU from the card |
| `OSDP_TRS_REPLY_PIN_COMPLETE` | PIN-entry ceremony finished (status, tries) |
| `OSDP_TRS_REPLY_ERROR`        | Reader-reported error |

The card's answer to a `SEND_APDU` is always delivered to the event callback
**before** the command completes. An application can therefore stash the
answer in its event handler and act on it in the completion handler, with no
need to match replies to commands by hand. This holds whether the reader
answers at once or — for a slow card — acknowledges the command and sends the
answer on a later poll. In the second case the band waits for that answer
before sending the next card command, and a reader that stays silent for
`OSDP_TRS_ANSWER_TOUT_MS` (default 5 s) fails the command.

A card command completes `OSDP_COMPLETION_OK` once the card has answered,
whatever status word it answered with — `90 00` and `6A 82` are both answers;
interpreting them is the application's business. It completes
`OSDP_COMPLETION_FAILED` when the exchange itself did not happen: the reader
reported an `OSDP_TRS_REPLY_ERROR` (delivered first, as above), or never
answered. A reader error fails only that exchange; the session stays open and
the next command in the band runs. The meaning of an error `code` is
reader-defined — OSDP does not standardize it.

The session life-cycle is reported through `OSDP_NOTIFICATION_TRS_STATUS`
(payload `struct osdp_notification_trs_status`) with an
`enum osdp_trs_session_status_e`:

| Status | Seen by | Meaning |
|--------|---------|---------|
| `OSDP_TRS_SESSION_OPENED` | CP, PD | The reader entered transparent mode |
| `OSDP_TRS_SESSION_CLOSED` | CP, PD | `STOP` honoured (CP); the reader left transparent mode or the link restarted (PD) |
| `OSDP_TRS_SESSION_FAILED` | CP | The session could not open, or a link error cut it short |
| `OSDP_TRS_SCAN_SUSPENDED` | CP | The reader refused a presence-scan probe; the scan backs off and retries |
| `OSDP_TRS_SCAN_NO_DEPARTURE` | CP | The reader cannot answer a card scan, so card removal will not be reported |
| `OSDP_TRS_CARD_RELEASED` | PD | The CP ended its transaction with the card |

### Finding the card: the presence scan

Many readers only report smart cards while they are in transparent mode, and a
reader parked in transparent mode stops reporting ordinary credentials. The
presence scan resolves this for you. With `osdp_cp_trs_scan_enable()` on, and
no session open, the library time-slices the reader: it spends
`mode0_dwell_ms` in the default mode, where card and keypad reads work as
usual, then `mode1_dwell_ms` in transparent mode watching for a smart card, and
repeats. Ordinary card or keypad activity restarts the default-mode dwell, so
a probe never cuts into a read in progress.

```c
struct osdp_trs_scan_params params = { 0 };  /* zeroes pick the defaults */

osdp_cp_trs_scan_enable(ctx, pd_idx, &params);
```

When a probe sights a card, the sighting is delivered as an `OSDP_EVENT_TRS`
event and transparent mode is held for `hold_ms` (default 500 ms), so a `START`
submitted in response adopts the reader as-is. A held card is announced on
every probe; deduplicate on the application side. The scan pauses by itself
while a session or a file transfer runs, and resumes afterwards.

If the reader answers the card-scan command, the scan also reports the card
*leaving* (`OSDP_TRS_CARD_NOT_PRESENT`). A reader that does not raises
`OSDP_TRS_SCAN_NO_DEPARTURE` once per connection; the application then has to
detect removal itself, by treating a failed card command as the card being
gone. Readers that announce cards spontaneously in their default mode need no
scan at all — their reports are delivered as-is.

Shortening `mode0_dwell_ms` makes the scan *more* aggressive, not less: the reader spends less of each cycle where
ordinary credentials can be read. At 38400 baud, 75 ms was enough to make a
reader miss card reads outright.

### PD side

A PD that bridges a card reader advertises `OSDP_PD_CAP_SMART_CARD_SUPPORT`
and handles three things. The library itself answers everything about the
reader's *mode* — mode reads, mode sets, the card terminate at the end of a
session — and only hands the application the card work:

- **Card commands** arrive through the command callback as `OSDP_CMD_XWR` with
  `trs.command` set to `OSDP_TRS_CMD_SEND_APDU`, `OSDP_TRS_CMD_ENTER_PIN` or
  `OSDP_TRS_CMD_CARD_SCAN`. Answer with an `OSDP_EVENT_TRS` event:
  `CARD_DATA` for an APDU, `PIN_COMPLETE` for a PIN entry, `CARD_PRESENT` for
  a card scan, or `ERROR` when the exchange could not be done. Submit it from
  inside the callback to reply at once; return without submitting to have the
  library acknowledge the command, and submit the answer when the card is done
  — it goes out on the CP's next poll.
- **Card arrival** — submit an `OSDP_TRS_REPLY_CARD_INFO` event when a smart
  card enters the field, carrying its CSN and ATR/ATS.
- **Mode changes** — `OSDP_TRS_SESSION_OPENED` / `CLOSED` notifications follow
  the reader's mode as the CP sets it (expect a steady stream of them while
  the CP runs a presence scan), and `OSDP_TRS_CARD_RELEASED` means the CP has
  finished with the card: disconnect it, so the next session finds it as if
  it had just been presented.

```c
int command_callback(void *arg, struct osdp_cmd *cmd)
{
    static struct osdp_event ev;
    int len;

    if (cmd->id != OSDP_CMD_XWR ||
        cmd->trs.command != OSDP_TRS_CMD_SEND_APDU) {
        return 0;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = OSDP_EVENT_TRS;
    len = card_transceive(cmd->trs.apdu.data, cmd->trs.apdu.length,
                          ev.trs.card_data.apdu.data,
                          osdp_pd_trs_get_max_apdu_len(ctx));
    if (len < 0) {
        ev.trs.reply = OSDP_TRS_REPLY_ERROR;
        ev.trs.error.code = 0x01;  /* reader-defined */
    } else {
        ev.trs.reply = OSDP_TRS_REPLY_CARD_DATA;
        ev.trs.card_data.apdu.length = len;
    }
    osdp_pd_submit_event(ctx, &ev);  /* inline: the fast-card reply */
    return 0;
}
```

Bound what you take from the card by `osdp_pd_trs_get_max_apdu_len()`: a
`CARD_DATA` reply longer than that is refused, because the CP could not
receive it. Ask again for each answer — the value changes when the CP
announces its receive size. Events, like commands, are queued by reference
and must outlive their completion callback.

### Secure PIN entry

`OSDP_TRS_CMD_ENTER_PIN` carries a `struct osdp_trs_pin_entry`: a C-APDU
template plus a description of where the reader must insert the PIN — block
format, position and size, an optional PIN-length field, digit limits,
completion conditions and display prompts. The reader collects the PIN on its
own keypad, builds the final APDU and sends it to the card, so the PIN never
crosses the bus in the clear. The answer is an `OSDP_TRS_REPLY_PIN_COMPLETE`
event with the reader's status and the number of tries. A PIN entry waits as
long as the user takes; `OSDP_TRS_ANSWER_TOUT_MS` does not apply. A layout the
wire cannot express is refused at submission.

### APDU capacity

Two different limits apply, and only one of them is the one to design against:

- `OSDP_TRS_APDU_MAX_LEN` (default 258 — a 255-byte short-APDU body plus status
  word) sizes the buffer embedded in the command and event structures. It is
  the memory knob: raise it at build time for extended-length APDUs, lower it
  to save RAM.
- What fits on the wire is bounded by the packet size in use: the smaller of
  this build's `OSDP_PACKET_BUF_SIZE` and the peer's announced receive size,
  less framing. With the default 256-byte packets that is 216 bytes in each
  direction. `osdp_cp_trs_get_max_apdu_len()` and
  `osdp_pd_trs_get_max_apdu_len()` report it; build both ends with a larger
  `OSDP_PACKET_BUF_SIZE` (for example 512) to move full-size chunks.

An over-large C-APDU is refused at submission. TRS is a transparent tunnel and
does no chaining of its own: ISO 7816-4 command chaining, and reading a long
file in `Le`-sized pieces, are the application's job.

## Python

The same flows are available from the Python bindings. For the PIV commands,
see [Commands](/python/commands#smartcard) and
[Events](/python/events#smartcard) for the `PivData`, `GenAuth` and `CrAuth`
classes.

TRS maps onto `commands.TrsStart`, `TrsSendApdu`, `TrsEnterPin`,
`TrsCardScan` and `TrsStop`, the `events.TrsCardInfo`, `TrsCardPresent`,
`TrsCardData`, `TrsPinComplete` and `TrsError` events, and the
`ControlPanel.trs_scan_enable()` / `trs_get_max_apdu_len()` methods. A CP that
waits for a card, runs one APDU against it and closes the session:

```python
from osdp import (ControlPanel, LibFlag, NotificationType, PDInfo,
                  TrsSessionStatus, commands, events)

cp = ControlPanel([PDInfo(101, channel, flags=[LibFlag.EnableNotification])])
cp.start()
cp.online_wait(101)
cp.trs_scan_enable(101)

def exchange(capdu: bytes) -> bytes | None:
    cp.submit_command(101, commands.TrsSendApdu(apdu=capdu))
    while True:
        match cp.get_event(101, timeout=6):
            case events.TrsCardData(apdu=rapdu):
                return rapdu
            case events.TrsError() | None:
                return None
            case events.Notification(type=NotificationType.TrsStatus) as n \
                    if n.trs_status == TrsSessionStatus.Failed:
                return None

while True:
    match cp.get_event(101, timeout=1):
        case events.TrsCardInfo(csn=csn):
            print(f"card {csn.hex()}")
            cp.submit_command(101, commands.TrsStart())
            rapdu = exchange(bytes.fromhex("00A4040007D276000085010100"))
            cp.submit_command(101, commands.TrsStop())
```

The answer timeout above sits just past the library's own 5 s
`OSDP_TRS_ANSWER_TOUT_MS`, so a silent reader surfaces as a failed command
before `get_event()` gives up. On a `PeripheralDevice`, a command handler
answers a `commands.TrsSendApdu` by calling `submit_event()` with an
`events.TrsCardData` (or `events.TrsError`) and returning `None`.
