---
lastUpdated: 2026-10-28
description: "Enable the OSDP secure channel in LibOSDP for encrypted, authenticated CP–PD links — covering SCBK keys, install mode, key rotation, random-number requirements, and secure channel setup."
---
# OSDP Secure Channel

OSDP uses AES with a key size of 128 bits (16 bytes). The Secure Channel (SC)
session is initiated by the CP, and either side can invalidate an established
session (PD by sending a NAK, CP by starting a new SC handshake). This document
explains the secure channel workflow to help application developers understand
failure logs emitted by LibOSDP.

LibOSDP has a working implementation of the secure channel. Any CP/PD using
this library will advertise this as an implicit capability. To disable secure
communication (e.g. for debugging), call `osdp_pd_set_capabilities()` after
`osdp_setup()` and set the compliance level of
`OSDP_PD_CAP_COMMUNICATION_SECURITY` to 0.

## Secure Channel Session

A SC session is initiated with a handshake involving 2 command-reply
transactions between the CP and PD. Once established, a session can be kept
active indefinitely until either party discards it or a timeout (400 ms) occurs.

The CP starts by sending `CMD_CHLNG`, to which the PD replies with
`REPLY_CCRYPT`. With this response the CP authenticates the PD and computes
its session keys. Next, the CP sends `CMD_SCRYPT`, which the PD uses to
authenticate the CP and compute its session keys. If all goes well, both CP and
PD will have a working set of session keys (s-enc, s-mac1, and s-mac2). The
final message of the handshake is the `REPLY_RMAC_I` response from the PD,
which carries the initial reply MAC the CP will use as the IV for encrypting
its next command (MAC chaining).

This process works if both the CP and PD have the same SCBK. If there is a
mismatch, the CP will fail to verify the PD's authenticity when it receives
`REPLY_CCRYPT`. At that point, LibOSDP will try a new SC handshake with
SCBK-Default to check if the PD is in install-mode. If that also fails, the
CP goes online without a secure session and retries every 10 minutes.

The SCBK-Default retry happens **only for a PD that has never completed a
handshake on its real SCBK**. Once one has, that fallback is closed for the
rest of the CP's lifetime — see [After the first secure
session](#after-the-first-secure-session).

> **Note:** This is the default behavior. With ENFORCE_SECURE enabled, the
> behavior changes (see below).

## SCBK (Secure Channel Base Key)

This is the primary key used to derive the secure channel session keys. When
setting up a CP or PD, pass the SCBK in [`osdp_pd_info_t::scbk`](/api/pd-info).

If `osdp_pd_info_t::scbk` is `NULL`, the secure channel is disabled for that
device and the link runs in plain text. A PD in this state reports
`OSDP_PD_CAP_COMMUNICATION_SECURITY` at compliance level 0 — the field is a
bitmap of supported algorithms, so zero advertises none — and rejects any
message carrying a security block with `osdp_NAK(5)`. The CP reads that
capability and never starts a handshake.

The one exception is a PD that also passes `OSDP_FLAG_INSTALL_MODE`; see
[Install Mode](#install-mode). Passing `NULL` together with
`OSDP_FLAG_ENFORCE_SECURE` fails setup outright.

> **Note:** LibOSDP no longer supports master-key based SCBK derivation. Each PD
> must be configured with its own SCBK directly; the CP is responsible for
> maintaining the per-PD SCBKs, as recommended by the OSDP specification.

## Install Mode

Install mode is a provisioning-time, insecure mode that instructs the PD to
use SCBK-Default (a hardcoded key defined by OSDP) and allows the CP to set a
new SCBK. Once the PD receives a new SCBK, it automatically exits install mode,
preventing another SCBK key set.

Enable install mode by passing `OSDP_FLAG_INSTALL_MODE` in
`osdp_pd_info_t::flags`. **Do not pass this flag during normal operation.**

Passing the flag is the **only** way to permit SCBK-Default on a PD. Omitting
an SCBK does not imply install mode; a keyless PD without the flag refuses the
secure channel entirely. This matches OSDP v2.2 §D.8, which requires the PD to
have "an installer accessible selection ... that, while enabled, would enable
the SCBK-D based connection".

`OSDP_FLAG_INSTALL_MODE` and `OSDP_FLAG_ENFORCE_SECURE` cannot be combined —
one exists to permit SCBK-Default and the other to forbid it. Setting both
fails setup.

## After the First Secure Session

The SCBK-Default retry described above is a reasonable guess for a PD that has
never been keyed. It is not one for a PD that has: a device that answers an
address which already ran a secure channel, reports the same PD ID, and fails
the challenge on purpose would be handed a session on a key published in the
spec — and the CP would then send it the real SCBK for that address over that
session. Return the bus to the genuine PD and every later session is readable.

So the CP latches the first handshake that completes on the configured SCBK
and refuses SCBK-Default for that PD from then on. The link falls back to plain
text instead, or stays offline under ENFORCE_SECURE, and the failure is logged
at emergency level.

The latch survives the session ending, the PD going offline, and the re-INIT
that follows, but it lives in the CP context — it cannot survive a restart of
your application. Two consequences:

- **Replacing a PD on a secured address needs an explicit re-provisioning
  step**, on a controlled link. There is deliberately no automatic path.
- **If you need the protection to outlast a restart**, persist it yourself.
  `OSDP_NOTIFICATION_PD_ID` fires whenever the identity read during INIT
  differs from the last one seen for that address, which is what a swapped
  device looks like when it does not forge the ID. An identity change on an
  address that ran a secure channel is logged at emergency level.

## Entering Install Mode

If you are building a PD as a product, you'll need a method to force the PD
into install mode during provisioning to allow the CP to set a new SCBK.

A simple approach is a pin-hole tactile switch that, when pressed during
reboot, causes your application to pass the `OSDP_FLAG_INSTALL_MODE` flag.

This is not very secure since an attacker with physical access can do the same.
More secure alternatives include special configuration cards presented to an
RFID reader soon after boot to trigger install mode (as some HID devices
implement).

On the CP side, keying the SCBK while a PD is in install mode is what
[Osprio Provisioner](https://osdp.dev/products/osprio-view/provisioner-workspace/)
automates — it sets the new SCBK over a controlled point-to-point link and
verifies it with a fresh secure-channel handshake before the PD is handed to
the production bus.

## The ENFORCE_SECURE Flag

Enforce Secure is a LibOSDP flag introduced due to the insecure nature of
install mode. A CP/PD instructed to use this flag operates with stricter
security, disallowing certain OSDP features such as install mode.

When ENFORCE_SECURE is enabled:
- Secure Channel cannot be disabled
- The CP/PD will not operate without a secure channel if setup fails
- SCBK-Default (install mode) is not allowed; combining the two flags fails
  setup
- An `osdp_KEYSET` acknowledgement must be authenticated. Outside
  ENFORCE_SECURE a plaintext ack is tolerated, because some PDs discard their
  session before replying

The flag is also a one-way latch at runtime: `osdp_cp_modify_flag()` can set
it on a live PD but cannot clear it, and cannot set it on a PD configured
without an SCBK. `OSDP_FLAG_INSTALL_MODE` cannot be changed at runtime at
all.

When you see a `"... due to ENFORCE_SECURE"` failure message in the logs, the
CP/PD attempted an action that LibOSDP deemed a protected operation requiring
supervision.

## Persistence of Keys

LibOSDP does not retain the SCBK across power cycles. The application is
responsible for storing and providing the correct SCBK on each call to
`osdp_cp/pd_setup()`.

In CP mode, the CP owns the SCBK, so the CP app already has the key. In PD
mode, when the PD receives a new SCBK from the CP, the PD app gets a copy in
the `OSDP_CMD_KEYSET` callback.

## Key Rotation

A CP rotates a PD's SCBK by sending it an `osdp_KEYSET` over a secure channel
session. Neither side switches to the new key when the command is
acknowledged: an acknowledgement that is lost on the wire, or a reset at the
wrong moment, would otherwise leave the two sides on different keys with no
way back. Instead, both sides hold the new key beside the current one, start
a new session, and switch only once that session proves both of them have it.
Until then each keeps using its current key, so a rotation that fails for any
reason leaves the link where it was.

This happens inside LibOSDP. What the application has to do is persist the
key at the right moment, so that what it stores always includes the key the
other side is using.

### On the PD

The `OSDP_CMD_KEYSET` callback is called once a session on the new key has
proven it, not when the command arrives. That is the moment to write the key
to storage as the key in use:

- Return `OSDP_PD_CMD_BUSY` while the write runs. The CP repeats the command
  and the callback is called again; BUSY never tells the CP the new key is in
  use, so it keeps both keys until the write has landed.
- Return 0 once it is written. The PD switches to the new key.
- Return an error if the key cannot be stored. The PD stays on its current
  key and ends the session, and the CP falls back to the current key too.

A PD that resets before the callback simply boots on its stored key, which
the CP still holds. A PD needs to store only one key.

In install mode the callback is called when the command arrives, since there
is no real key to fall back on, and the key is used from the next session.

### On the CP

The CP is the side that has to store two keys for a while, because the PD
proves the new key first: a CP that resets between the PD switching and the
CP learning of it must still have the new key. Keep two key slots per PD and
an index naming the active one:

1. Write the new key into the inactive slot, then submit the `KEYSET`. If the
   write fails, do not submit.
2. Wait for the command's completion. LibOSDP completes it only once a
   session has settled the key, which may be well after the PD acknowledged
   it.
3. On `OSDP_COMPLETION_OK` the new key is in use: flip the active index. Keep
   this to a single small write so the window in which a reset leaves storage
   behind is as short as possible.
4. On `OSDP_COMPLETION_FAILED` the PD stays on its current key: erase the
   inactive slot.
5. On `OSDP_COMPLETION_ABORTED` (the CP was torn down first) it is not known
   which key the PD holds: keep both slots.

LibOSDP refuses another `KEYSET` for that PD until the completion arrives.

### CP Start-up

If the inactive slot holds a key when the CP starts, the last rotation was
never settled, and the PD may hold either key. Bring the PD up on the
inactive key first; if the secure channel does not come up on it, bring it
up on the active key. Whichever key the secure channel comes up on is the
active key from then on, and the other slot is erased.

Try the inactive key first. A PD that is still holding the new key beside its
current one gives it exactly one handshake: offered the new key, it switches;
offered the old key, it uses up that one attempt on a mismatch and only falls
back after the CP's next handshake, which LibOSDP starts after a 10-minute
wait.

The secure channel coming up is reported by `OSDP_NOTIFICATION_SC_STATUS`
(with `OSDP_FLAG_ENABLE_NOTIFICATION`) or `osdp_get_sc_status_mask()`. A
handshake that fails on the wrong key is followed by one SCBK-Default attempt,
which a PD outside install mode answers on its current key; the error it logs
is expected here.

## Random Number Generation

Each side of the secure channel handshake contributes a random number, but
they do different jobs. The session keys are derived from the SCBK and the
CP's random number alone — only its first six bytes, per OSDP v2.2 §D.4.1 —
so a weak CP random source can repeat session keys. The PD's random number
does not enter the session keys; together with the CP's, it goes into the
cryptograms each side uses to prove it holds the SCBK, so a weak PD random
source weakens the PD's check that the handshake is live rather than
replayed. **You are responsible for ensuring your hardware has a
cryptographically strong random source on both sides.**

Where the randomness comes from depends on the [crypto backend](/libosdp/build-and-install#crypto-backend-selection):

- **OpenSSL / MbedTLS:** nonces are drawn from the crypto library's CSPRNG,
  which is seeded from the operating system's entropy source. No extra work is
  needed on hosted platforms.
- **TinyAES (bundled fallback):** nonces come from the platform's `rand_u32()`.
  This is only as strong as the underlying source — on hosted builds it falls
  back to the C library's `rand()`, and on some microcontrollers it is a
  boot-seeded PRNG. Neither is suitable for production secure channel use.

On any target without a hardware TRNG wired in, override `rand_u32()` with a
real entropy source before deploying secure channel. See
[Porting to a New Platform](/libosdp/cross-compiling#porting-to-a-new-platform)
for how the platform hooks are provided.
