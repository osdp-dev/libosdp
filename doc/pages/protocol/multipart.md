---
lastUpdated: 2026-11-26
description: "How OSDP fragments large payloads — the spec section 5.10 multi-part format for smart-card commands, the separate file-transfer feature, and the under-specified biometric case — and how LibOSDP unifies all three behind one engine and one notification path."
---
# Multi-part Messages

Some OSDP payloads are too large to fit in a single packet. A PIV certificate,
a cryptographic challenge, a firmware image, or a fingerprint template can all
run to hundreds or thousands of bytes, while one OSDP packet carries only as
much as the negotiated maximum reply size allows.

The specification handles this in more than one place — and not always
completely:

- **Smart-card commands** (`REPLY_PIVDATAR`, `CMD_GENAUTH`/`REPLY_GENAUTHR`,
  `CMD_CRAUTH`/`REPLY_CRAUTHR`) use the *multi-part message* format of **spec
  section 5.10**: the payload is sliced into fragments, each prefixed with a
  header that says how big the whole payload is and where this fragment sits
  inside it, and the receiver stitches them back together. This is the only
  place the spec actually calls something a "multi-part message".
- **File transfer** (`CMD_FILETRANSFER` / `REPLY_FTSTAT`) is a **separate,
  self-contained feature** with its own command, its own status reply, and its
  own offset-based chunking. The spec does *not* define it as a multi-part
  message — it is its own thing.
- **Biometric read** (`REPLY_BIOREADR`) is the awkward case: the spec allows a
  template to exceed a single packet but never specifies *how* it should be
  fragmented. That gap is why LibOSDP has to invent its own scheme (see
  [the special case](#the-special-case-biometric-read-replies) below).

LibOSDP's observation is that, underneath, these are all the same shape: an
ordered, offset-addressed transfer of a payload whose total size is known up
front. So rather than write three transfer loops, LibOSDP carries all of them
through **one internal engine**. That unification is an engineering optimization,
not something the standard mandates — and its main payoff is a single,
streamlined [notification path](#the-notification-path): every large transfer,
whatever its origin in the spec, reports its progress exactly the same way.

## The on-wire header

Whatever the source format, LibOSDP models a fragment with the same three
fields:

| Field      | Meaning                                        |
|------------|------------------------------------------------|
| `TOTAL`    | Full length of the reassembled payload         |
| `OFFSET`   | Byte offset of this fragment within the payload |
| `DATA_LEN` | Number of payload bytes in this fragment       |

`TOTAL` is fixed for the life of a transfer (every fragment must report the same
value); `OFFSET` advances as fragments arrive; the transfer is complete when
`OFFSET + DATA_LEN == TOTAL`. All three are little-endian.

The two on-wire formats LibOSDP unifies differ only in how wide those fields
are, so the engine works in two header widths:

| Width | `TOTAL`/`OFFSET` size | Header size | On-wire format |
|-------|-----------------------|-------------|----------------|
| **W16** | 16-bit | 6 bytes | The spec section 5.10 multi-part header — smart-card replies, and (by reuse) biometric read |
| **W32** | 32-bit | 10 bytes | The file-transfer format's wider offset/total fields |

W16 caps a single transfer at 64 KiB, which is ample for smart-card objects and
biometric templates. File transfer needs more headroom, so its format carries
32-bit fields — the W32 width — and LibOSDP models it with the same engine.

## One engine, many families

LibOSDP factors the fragmentation, reassembly, ordering, and progress-reporting
logic into a single, **family-agnostic engine** and drives it from each feature
that needs it. The engine never interprets the payload — it only moves bytes and
enforces the header rules — so the same code path carries a firmware image, a
PIV certificate, and a fingerprint template.

Every transfer is tagged with the family it belongs to
(`enum osdp_mp_msg_type`):

| Family | Value | Header | Spec basis | Payload |
|--------|-------|--------|------------|---------|
| `OSDP_MP_MSG_FILE_TRANSFER` | 1 | W32 | Separate file-transfer feature | `CMD_FILETRANSFER` |
| `OSDP_MP_MSG_PIV`           | 2 | W16 | Section 5.10 multi-part | `REPLY_PIVDATAR` |
| `OSDP_MP_MSG_GENAUTH`       | 3 | W16 | Section 5.10 multi-part | `CMD_GENAUTH` / `REPLY_GENAUTHR` |
| `OSDP_MP_MSG_CRAUTH`        | 4 | W16 | Section 5.10 multi-part | `CMD_CRAUTH` / `REPLY_CRAUTHR` |
| `OSDP_MP_MSG_BIOREAD`       | 5 | W16 | LibOSDP extension (under-specified in spec) | `REPLY_BIOREADR` (opt-in — see below) |

The family tag is what lets one progress-notification stream describe
completely different operations: the application learns *that* a transfer
started, progressed, or finished, and *which* feature it belonged to, without
the engine knowing anything about certificates or templates.

## Fragmentation and reassembly

The engine enforces a few rules that keep a transfer well-formed:

- **No forward gaps.** A fragment whose `OFFSET` skips past the next expected
  byte is rejected. Fragments must arrive in order.
- **Retransmit is tolerated.** A fragment that repeats bytes already received
  (a retransmission after a lost acknowledgement) is accepted and ignored up to
  the point already committed, so a dropped packet does not abort the transfer.
- **`TOTAL` must agree.** Once the first fragment fixes the total size, every
  later fragment must report the same value, and no fragment may push `OFFSET +
  DATA_LEN` past it.

Two OSDP details ride on top of the plain header:

- **First-fragment prefix.** GENAUTH and CRAUTH prepend a few control bytes
  (the algorithm and key reference from OSDP 2.2 Tables 32/33) that are *not*
  part of the reassembled payload and are not counted in `TOTAL`. The engine
  carries this prefix on the first fragment only.
- **Early termination / idle keep-alive.** A header-only fragment (its `OFFSET`
  at or past `TOTAL` with no data) means different things per family: for the
  section 5.10 families it signals early termination of the transfer; for file
  transfer it is the CP's idle keep-alive that holds the transfer open while the
  PD finishes processing what it already received.

## Buffer handling

The engine reassembles into storage the consumer provides, in one of two modes:

- **Buffer-bound.** The consumer hands the engine a fixed buffer and length, and
  fragments are copied into it. This is how the smart-card and biometric
  features work — the reassembled object lands in a bounded staging buffer.
  Every copy is bounds- and wrap-checked against the wire-supplied `OFFSET` and
  `DATA_LEN`, so a malformed or malicious header can never write outside the
  buffer.
- **Offset-addressed (ops).** Instead of a single buffer, the consumer supplies
  read/write callbacks addressed by offset. File transfer uses this so an
  arbitrarily large image can stream to and from storage without ever being held
  whole in memory.

The staging buffers for the fixed-buffer families are bounded by compile-time
limits, overridable at build time on both roles:

| Limit | Default | Applies to |
|-------|---------|------------|
| `OSDP_PIV_DATA_MAX_LEN` | 256 | PIV / GENAUTH / CRAUTH payloads |
| `OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN` | 256 | Biometric templates |

The receiver rejects a transfer up front if its advertised `TOTAL` exceeds the
bound, rather than partway through — so an over-large transfer fails cleanly
instead of overrunning. Raise the limit at build time if your objects are
larger; the wire format itself allows up to 64 KiB per W16 transfer.

## The notification path

Unifying the three transfer mechanisms behind one engine buys one concrete
thing: a **single notification path**. The spec's smart-card multi-part, the
separate file-transfer feature, and the invented biometric scheme are three
different things on the wire, but because LibOSDP runs them all through the same
engine, they all surface to the application through the *same* three
notifications — the application does not need three different ways to watch three
different kinds of large transfer.

The engine emits the notifications; a thin bridge hands each one to the
application on the normal delivery path (as an
[`osdp_notification`](/api/event-structure#notifications) — an event in CP mode,
a synthesized command in PD mode). The payload is `struct osdp_mp_notification`:

| Field | Meaning |
|-------|---------|
| `mp_type` | Which family this transfer belongs to (`enum osdp_mp_msg_type`) |
| `object_id` | Per-family id: the file id for file transfer, the PIV element for `OSDP_MP_MSG_PIV`, the key reference for GENAUTH/CRAUTH on a CP (0 on a PD), the reader for a biometric read |
| `total` | Full payload length |
| `offset` | Bytes transferred so far |
| `outcome` | Meaningful at `MP_DONE` — see the outcome table |

`mp_type` is what keeps the path streamlined without losing detail: one
subscription sees every large transfer, and the family tag tells the application
which feature each notification describes.

When notifications are enabled (`OSDP_FLAG_ENABLE_NOTIFICATION`), every
multi-part transfer reports its progress through three notifications:

| Notification | When it fires |
|--------------|---------------|
| `OSDP_NOTIFICATION_MP_START` | Once, at the start of a transfer. Always precedes `MP_DONE`, even for a transfer that fails immediately. |
| `OSDP_NOTIFICATION_MP_PROGRESS` | Each time a fragment advances the offset while the transfer is still short of `total`. The final fragment does not emit `MP_PROGRESS`. |
| `OSDP_NOTIFICATION_MP_DONE` | Exactly once, when the transfer terminates — for any reason. `mp.outcome` carries the result. |

`MP_DONE`'s outcome is an `enum osdp_mp_outcome`. `OK` and `ABORTED` apply to
every family; the other three are reported only by file transfer:

| Outcome | Meaning |
|---------|---------|
| `OSDP_MP_OUTCOME_OK` | Payload fully transferred and accepted |
| `OSDP_MP_OUTCOME_OK_REBOOTING` | File transfer: accepted; the PD will now reset |
| `OSDP_MP_OUTCOME_ABORTED` | Aborted — locally (`CMD_ABORT`, timeout, teardown) or by the remote |
| `OSDP_MP_OUTCOME_UNRECOGNIZED` | File transfer: the PD did not recognize the contents |
| `OSDP_MP_OUTCOME_INVALID` | File transfer: the PD rejected the data as malformed |

Because `MP_START` fires before any work and `MP_DONE` fires exactly once on
every terminal path, an application can bracket a transfer purely on these two
notifications: any operation that starts is guaranteed to report a final
outcome, whether it succeeds, is aborted with `CMD_ABORT`, times out, is refused
by the PD, or is cut short by the link going down. Reaching `offset == total` is
*not* itself the end — a sender may park at end-of-payload sending idle
keep-alives — so `MP_DONE`, not the offset, is the signal that a transfer is
over.

## Concurrency

A PD carries **one transfer at a time** through the engine. The spec forbids
interleaving section 5.10 multi-part messages, and the shared engine extends the
same single-transfer discipline to file transfer: a smart-card operation and a
file transfer cannot run concurrently, and a second smart-card submission while
one is in flight fails immediately. Fragments of the in-flight transfer are
pulled along by the ordinary `CMD_POLL` cycle between the CP and PD.

## Per-family notes

**File transfer** (`OSDP_MP_MSG_FILE_TRANSFER`) is the spec's own separate
feature, not a section 5.10 multi-part message — LibOSDP maps it onto the engine
because it is the same offset-addressed shape. It carries the W32 width, streams
through offset-addressed callbacks rather than a fixed buffer, and reports its
own `REPLY_FTSTAT` status alongside the shared `MP_*` notifications.

**PIV / GENAUTH / CRAUTH** (`OSDP_MP_MSG_PIV`/`GENAUTH`/`CRAUTH`) are the
smart-card replies — the one case the spec genuinely defines as section 5.10
multi-part. Described in full on the
[Smartcard Commands](/libosdp/smartcard) page. These replies are **always**
multi-part — there is no single-part variant — and GENAUTH/CRAUTH carry the
first-fragment algorithm/key prefix.

**Biometric read** (`OSDP_MP_MSG_BIOREAD`) is the special case, covered next.

## The special case: biometric read replies

`REPLY_BIOREADR` is different from the smart-card replies in a way worth calling
out. The OSDP standard defines **no** multi-part family for biometric data — a
biometric template is supposed to fit in a single reply. LibOSDP can still carry
a template larger than one packet, but doing so is **non-conforming to the
standard**, so it is *opt-in*: it engages only when both roles set
`OSDP_FLAG_BIOREADR_MULTIPART` (an init-only flag). With the flag clear, LibOSDP
behaves exactly as the spec requires and expects the whole template in one
packet.

The mechanism is a deliberate sleight of hand. The `REPLY_BIOREADR` header
(reader, status, type, quality, and a 16-bit length) happens to be **exactly the
same size as a W16 multi-part header** — six bytes. So LibOSDP sends the first
fragment through the ordinary engine and then overwrites its multi-part header
in place with a real `REPLY_BIOREADR` header whose `length` field carries the
*total* template size. There is no new command code and no new wire signal:

- **The PD advertises nothing extra.** It sends a normal-looking `REPLY_BIOREADR`
  whose `length` says "total = N" but which physically carries fewer than N
  bytes.
- **The CP detects multi-part by the mismatch.** If the bytes present equal the
  advertised total, it is an ordinary single-part reply; if fewer are present,
  the CP begins reassembly and pulls the remaining fragments on subsequent
  polls.
- **It stays opt-in even when the flag is set.** If a template happens to fit in
  one packet, the PD still sends a plain single-part reply; multi-part engages
  only when the template overflows.

This is why the [command table](/protocol/commands-and-replies) marks
`REPLY_BIOREADR` as multi-part *opt-in* rather than plain multi-part like the
PIV replies: the same family machinery is reused, but only by mutual agreement
and outside the letter of the standard.

## See also

- [Smartcard Commands](/libosdp/smartcard) — the PIV/GENAUTH/CRAUTH and TRS
  flows that use this engine.
- [Commands and Replies](/protocol/commands-and-replies) — which messages are
  multi-part, marked with the ⧉ glyph.
- [Event Structure](/api/event-structure#notifications) — the notification
  structures the lifecycle is reported through.
- [Control Panel](/api/control-panel#commands) /
  [Peripheral Device](/api/peripheral-device#events) — the by-reference command
  and event ownership lifecycle these transfers run inside.
