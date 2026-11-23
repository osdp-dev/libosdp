---
release_date: 2026-07-22
release_version: v4.0.0
---

LibOSDP v4.0.0 is a major release. It reworks the public API around one
channel per context and application-owned command and event memory, and it
completes the OSDP v2.2 message set: smart card (PIV, GENAUTH, CRAUTH),
biometrics, manufacturer status and error replies, a shared multi-part
transfer engine and Transparent Reader Support. The secure channel has been
hardened throughout, SCBK rotation survives lost replies and resets, and the
library gains .NET bindings, an ESP-IDF component and a PSA crypto backend for
Zephyr.

Almost every application needs source changes to move from v3. Start with
Breaking API changes and Migration notes below.

## Breaking API changes

- One channel per context: `osdp_cp_setup()` and `osdp_pd_setup()` take a
  `struct osdp_channel *` as their first argument, `channel` is gone from
  `osdp_pd_info_t` and `id` is gone from `struct osdp_channel`. All PDs of a
  CP share its one channel, which is closed once per context at teardown. A
  deployment with several buses creates one `osdp_t` per bus.
- Commands and events are always application-owned:
  `OPT_OSDP_APP_OWNED_QUEUE_DATA` (Zephyr
  `CONFIG_LIBOSDP_APP_OWNED_QUEUE_DATA`) is gone because that behaviour is
  the only one. A submitted `struct osdp_cmd` or `struct osdp_event` is
  queued by pointer, not copied, and stays owned by LibOSDP until its
  completion callback fires with `OSDP_COMPLETION_OK`, `FAILED`, `FLUSHED`
  or `ABORTED`.
- `osdp_cp_submit_command()` and `osdp_pd_submit_event()` refuse the
  submission when no completion callback is registered, because ownership
  could never be returned. The completion callbacks now receive a non-const
  pointer so the application can free what it allocated without a cast.
- `osdp_cp_send_command()` and `osdp_pd_notify_event()` are removed. They
  were deprecated aliases of `osdp_cp_submit_command()` and
  `osdp_pd_submit_event()` since v3.1.0.
- `struct osdp_event_notification` is now `struct osdp_notification`,
  `enum osdp_event_notification_type` is now `enum osdp_notification_type`
  and `OSDP_EVENT_NOTIFICATION_*` are now `OSDP_NOTIFICATION_*`. The struct
  is shared by CP events and PD commands, so the `event` prefix was
  misleading.
- Notifications carry typed payloads instead of `arg0`/`arg1`: `sc_status`,
  `pd_status`, `mp`, `pd_id` and `trs_status` members of a union
  discriminated by `type`. Every notification consumer must be rewritten,
  and no compiler will flag code that still reads the old meaning into a
  field.
- `OSDP_NOTIFICATION_COMMAND` is removed. It duplicated the completion
  callback with less information, so the completion callback is now the only
  way to learn a command's outcome. The enum is renumbered and gains
  `OSDP_NOTIFICATION_SENTINEL`.
- `osdp_log_callback_fn_t` takes a leading `int pd` argument ahead of
  `log_level`, `msg`, `file` and `line`. C code with the old signature
  compiles with a warning at best.
- Fixed-width fields: `osdp_pd_info_t::baud_rate` and `::flags` are
  `uint32_t` (every `OSDP_FLAG_*` lives at bit 16 or above and cannot be
  held by a 16-bit `int`), file transfer `size` and `offset` in the file ops
  are `uint32_t` (files over `INT_MAX` were truncated) and
  `osdp_event_cardread::length` is `uint16_t` to match its wire field.
  Format strings and signed comparisons on these fields need updating.
- Enum-valued fields carry their enum type: output, buzzer and text control
  codes, the biometric type, format and status fields,
  `osdp_pd_cap::function_code` and the LED colours. LED `control_code` stays
  `uint8_t` because its temporary and permanent blocks give the same number
  different meanings. Capability arrays must end with
  `OSDP_PD_CAP_SENTINEL`.
- Status reports carry one byte per entity, and `nr_entries` must equal what
  the PD supports for that report: the advertised capability count for
  input, output and reader status, and exactly two (tamper, power) for local
  status. A mismatched report is refused by `osdp_pd_submit_event()`. Reader
  status now carries one byte per attached reader, its first entry is reader
  1, and `OSDP_STATUS_REPORT_REMOTE` is renamed `OSDP_STATUS_REPORT_READER`.
- File transfer reports through the multi-part lifecycle:
  `OSDP_NOTIFICATION_MP_START`, `MP_PROGRESS` and `MP_DONE` with a
  `struct osdp_mp_notification` payload. `enum osdp_file_tx_outcome` is now
  `enum osdp_mp_outcome` (`OSDP_MP_OUTCOME_*`, values unchanged).
  `OSDP_CMD_FILE_TX_FLAG_CANCEL` is removed in favour of `osdp_cp_cancel()`,
  and a FILE_TX whose flags touch the upper byte, which is reserved for
  internal state, is refused.
- Setup-time calls are refused once the context has been refreshed:
  `osdp_cp_add_pd()`, `osdp_pd_set_capabilities()`,
  `osdp_file_register_ops()` and the four callback setters. The setters and
  `osdp_pd_set_capabilities()` now return `int` so the refusal can be
  reported. To take PDs in and out of service at runtime, use
  `osdp_cp_disable_pd()` and `osdp_cp_enable_pd()`.
- The threading contract is explicit: callers must serialize every API call
  with refresh, including `osdp_get_status_mask()`,
  `osdp_get_sc_status_mask()`, `osdp_cp_get_pd_id()` and
  `osdp_get_file_tx_status()`, which earlier documentation exempted.
  Refresh, `osdp_cp_add_pd()` and `osdp_file_register_ops()` are refused
  from inside callbacks, and every call is refused during teardown.
- `osdp_cp_modify_flag()` can no longer set `OSDP_FLAG_INSTALL_MODE` or
  clear `OSDP_FLAG_ENFORCE_SECURE` on a running PD. ENFORCE_SECURE is a
  one-way latch, and a request that mixes an allowed flag with a refused one
  changes nothing.
- Setup fails when `OSDP_FLAG_ENFORCE_SECURE` and `OSDP_FLAG_INSTALL_MODE`
  are both set, on either role, because they ask for opposite things.
- An ASCII card read is refused at submit. LibOSDP does not implement
  osdp_FMT, and the PD used to answer such an event with a bare ACK while
  reporting it sent.
- Public headers install under `include/libosdp/`. Installed consumers
  either include `<libosdp/osdp.h>` or use the pkg-config or CMake package,
  which add that directory to the include path.
- `OPT_OSDP_STATIC_PD` is now `OPT_OSDP_STATIC` and also covers the CP, so a
  CP can be built without heap allocation. `configure.sh --static-pd`
  remains as a deprecated alias for `--static`.
- Internal timestamps use `tick_t`. A bare-metal application that provides
  `osdp_millis_now()` must return `tick_t`, and `--use-32bit-tick-t` (with
  `--bare-metal`) narrows it to 32 bits.
- On a bare-metal target that c-utils cannot identify, `osdp_millis_now()`,
  `rand_u32()`, `osdp_calloc()` and `osdp_free()` have no default, so a
  missing implementation is a link error. Previously the clock froze at
  zero, the RNG was constant and the heap fell back to newlib's sbrk.
  Arduino, ESP-IDF, Zephyr and Mynewt are identified and need no hooks, and
  `OPT_OSDP_STATIC` builds need no heap hooks.
- `OPT_OSDP_RX_ZERO_COPY` is a compile-time ABI switch with no fallback. In
  a zero-copy build `struct osdp_channel` carries `recv_pkt`/`release_pkt`
  instead of `recv`, so the library and the application must agree on the
  flag.

## Behavior changes

- A PD that declines a command with an application-level NAK stays online,
  and the command completes FAILED. Only link-level NAKs (MSG_CHK, SEQ_NUM,
  SC_UNSUP, SC_COND), timeouts, malformed frames and unexpected replies
  still reset the link.
- The CP no longer downgrades to checksum when a PD that advertises CRC-16
  NAKs a frame with MSG_CHK. It resends instead, within
  `OSDP_CMD_MAX_RETRIES`, which ends the reframe, NAK(SEQ_NUM) and
  five-minute offline cascade seen with such PDs.
- osdp_BUSY is handled per v2.2 section 7.19. The CP repeats the command
  with its original sequence number, BUSY must arrive in plaintext, and
  every retry counts against `OSDP_CMD_MAX_RETRIES`. A permanently busy PD
  fails the command and keeps the link up, instead of being retried forever.
- A PD retransmits its cached reply when the CP repeats a sequence number,
  and the CP resets the sequence to 0 before every new secure channel
  session.
- Multi-drop fixes: a CP or PD skips packets addressed to another context, a
  CP warns when it sees another CP on the bus, retry probes yield the bus to
  other PDs and transmit timeouts are computed correctly.
- In PD mode `OSDP_FLAG_ENABLE_NOTIFICATION` now delivers notifications
  through the command callback as an `osdp_cmd` with id
  `OSDP_CMD_NOTIFICATION`. The PD tracks online and offline from CP activity
  (`OSDP_PD_ONLINE_TOUT_MS`) and reports it with
  `OSDP_NOTIFICATION_PD_STATUS`.
- On the CP, `OSDP_NOTIFICATION_PD_STATUS` fires on every online transition,
  including a PD that leaves ONLINE to redo the secure channel handshake, so
  the notifications always agree with `osdp_get_status_mask()`.
- `OSDP_NOTIFICATION_SC_STATUS` also fires when the phy drops a session on
  its own (sequence restart, bad MAC or failed decrypt), so the application
  no longer believes the channel is up after the library has dropped it.
- Commands that drive a multi-part operation (FILE_TX, PIVDATA, GENAUTH,
  CRAUTH and a multi-part BIOREAD) complete when the operation finishes, not
  when it is accepted. The reply event reaches the event callback before the
  command completes. Each PD runs one such operation at a time (v2.2 section
  5.10.2), and a second one is refused at submit.
- `reader_no` is checked against `OSDP_PD_CAP_READERS` in three places: the
  CP refuses such a command at submit, the PD NAKs it and the PD refuses
  events for an unadvertised reader. Applications that never set the READERS
  capability will see these submissions fail.
- The PD NAKs output, LED, buzzer and text commands that carry an undefined
  control code with RECORD, instead of passing the application a code it
  cannot name.
- Once online, the CP sends osdp_ACURXSIZE so the PD sizes its replies for
  the CP's receive buffer. The command travels inside the secure channel and
  is sent again after every link restart. A PD that NAKs it stays online,
  and the PD refuses a size too small to reply in.
- The PD answers a status query with a status event submitted from inside
  the command callback, like every other data-carrying reply. Filling in the
  report in place still works.
- A dequeued PD event whose reply fails to build, or degrades into a NAK,
  completes FAILED instead of being dropped or reported as OK.
- File transfer: a PD app can hold a transfer open or refuse a file through
  its finalize op, which `OSDP_FILE_FINALIZE_TIMEOUT_MS` bounds. A PD that
  aborts a transfer, or whose write() fails, ends it with an FTSTAT abort
  status and stays online, and a frame the PD cannot take is NAKed instead
  of resetting the link.
- `CMD_RMODE` and `CMD_TDSET` are marked obsolete in the specification.
  RMODE stays unimplemented, while TDSET is now supported for legacy interop
  (see New features).

## New features

- Multi-part transfer engine shared by file transfer, smart card and
  biometric operations, implementing the TOTAL/OFFSET/DATA_LEN framing of
  v2.2 section 5.10 (16-bit) and file transfer (32-bit). It reports progress
  through `OSDP_NOTIFICATION_MP_START`, `MP_PROGRESS` and `MP_DONE`.
- `osdp_cp_cancel()` stops a running file transfer, PIV or biometric
  operation early. The command that started the operation completes FAILED
  with outcome `OSDP_MP_OUTCOME_ABORTED`.
- Smart card commands osdp_PIVDATA, osdp_GENAUTH and osdp_CRAUTH, gated on
  `OSDP_PD_CAP_SMART_CARD_SUPPORT`. The PD answers through the command
  callback, inline or deferred, and both the challenge and the reply may
  span several packets.
- Biometric commands osdp_BIOREAD and osdp_BIOMATCH with their replies,
  gated on the biometrics capability. `OSDP_FLAG_BIOREADR_MULTIPART`, set on
  both roles, lets a BIOREADR template span several packets (up to
  `OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN`, now 256).
- osdp_MFGSTATR and osdp_MFGERRR replies as `OSDP_EVENT_MFGSTATR` and
  `OSDP_EVENT_MFGERRR`. A PD can answer CMD_MFG, the biometric and smart
  card commands, and osdp_OUT (with osdp_OSTATR) by submitting the reply
  from inside the command callback, and it goes out immediately.
- osdp_TDSET (time and date set) as `OSDP_CMD_TDSET`, gated on
  `OSDP_PD_CAP_TIME_KEEPING`.
- A PD command callback can return `OSDP_PD_CMD_BUSY` to answer osdp_BUSY
  while it finishes slow work, such as persisting a key. The CP then repeats
  the command unchanged.
- `OSDP_NOTIFICATION_PD_ID` tells the CP application when the identity a PD
  reports is new or has changed for its address.
- SCBK rotation that survives lost replies and resets: both roles stage a
  new key until a secure channel session proves it, and `osdp_cp_set_scbk()`
  replaces a disabled PD's key without tearing down the context.
- `osdp_get_metrics()` reports per-PD link and protocol counters in
  `struct osdp_metrics`.
- `osdp_get_abi_info()` reports struct sizes and ABI-relevant build options,
  so a binding can refuse to run against a library built with a different
  layout.
- Minimal logger mode (`OPT_OSDP_LOG_MINIMAL`) hands log messages to the
  application unformatted from a static logger context. Built this way, a
  bare-metal LibOSDP needs nothing from libc but snprintf() and vsnprintf().
- Command and event buffer limits are `#ifndef`-guarded so constrained
  targets can shrink every queued slot, and all tunables now live in
  `src/osdp_tunables.h`.
- `OPT_OSDP_EXCLUSIVE_ROLE` overlays the static CP and PD pools for builds
  that never run both roles at once.
- `osdp.hpp` wraps every exported C function again.

## Transparent Reader Support

- Transparent Reader Support (osdp_XWR and osdp_XRD) tunnels smart card
  APDUs from the CP, through the PD, to the card, so the control panel talks
  to the card directly and a compromised reader cannot forge a credential.
  It is built unconditionally and gated at runtime on
  `OSDP_PD_CAP_SMART_CARD_SUPPORT`.
- The CP application submits `OSDP_CMD_XWR` with a `struct osdp_trs_cmd`
  (START, SEND_APDU, ENTER_PIN, CARD_SCAN, STOP) naming the reader. It
  receives card answers as `OSDP_EVENT_TRS` and session progress as
  `OSDP_NOTIFICATION_TRS_STATUS`. A card answer always reaches the
  application before its command completes, and START and STOP complete on
  the session's outcome.
- A failed session reports why in the TRS status notification (refused, NAK
  with its code, no reply, link down, idle). A session with no card command
  for `OSDP_TRS_IDLE_TOUT_MS` (30 s) is closed, so a forgotten STOP cannot
  leave the reader in transparent mode.
- The PD application answers card commands from the command callback either
  synchronously or deferred (ACK now, answer on a later poll), which keeps a
  shared multi-drop bus moving while a slow card works. The CP holds the
  band until the deferred answer arrives or `OSDP_TRS_ANSWER_TOUT_MS`
  passes. The PD application is also told when the CP opens or closes
  transparent mode and releases a card.
- `osdp_cp_trs_scan_enable()` runs a background card presence scan for
  readers that stop reporting ordinary credentials in transparent mode. It
  reports a card arriving or leaving, falls back to poll-only on readers
  that decline the card scan and reports that once per connection.
- `osdp_cp_trs_get_max_apdu_len()` and `osdp_pd_trs_get_max_apdu_len()`
  report the APDU size that actually fits the negotiated packet size.
  `OSDP_TRS_APDU_MAX_LEN` defaults to 258 and sizes the buffer embedded in
  every command and event; full-size APDUs also need `OSDP_PACKET_BUF_SIZE`
  raised to 512.
- `enum osdp_trs_error_e` recommends a vocabulary for the otherwise
  reader-defined TRS error byte, which is reported as `reader_status`.
- Interop with readers in the field: a NAKed card command is never
  retransmitted and fails only that command, out-of-session XRD replies no
  longer take the PD offline and card sightings among them become events,
  and Mode-Set is accepted without its optional config byte. A PD restores
  the reader's default mode when the CP restarts the link.

## Security

- A PD without an SCBK no longer brings up a secure channel on SCBK-D unless
  the installer set `OSDP_FLAG_INSTALL_MODE`. Otherwise it advertises no
  secure channel and NAKs CHLNG, so anyone on the bus can no longer key a
  fresh PD.
- SCBK-D and the all-zero key are refused as real keys at setup and in
  KEYSET on both roles. A keyless install-mode PD answers a CHLNG for a real
  key on a throwaway key and refuses the SCRYPT that would complete it.
- Once a secure channel has been established on an address, the CP refuses
  to fall back to SCBK-D there. This closes a swap attack that would have
  handed the real SCBK to an impostor, and a PD identity change on such an
  address is logged at emergency level.
- The CP verifies RMAC_I in constant time and honours the SCS_14 status byte
  before turning the secure channel on, and it falls back to plaintext at
  sequence 0 after a refused handshake instead of retrying in a tight loop.
- While a session is active, a handshake-only security block type (SCS_11 to
  SCS_14) is accepted only on its handshake message, and plaintext NAKs are
  dropped (v2.2 D.1.1). Either case now restarts the secure channel instead
  of steering the link.
- With `OSDP_FLAG_ENFORCE_SECURE` the CP requires a MAC'd KEYSET
  acknowledgement, and the CP warns when a PD is configured without an SCBK.
- Key material is zeroized at function exit, at teardown (including a
  pending KEYSET key) and when the CP grows its PD array.
- Inbound frame hardening: the security block length is bounded to the
  frame, the mark byte counts against the packet buffer, packet length
  subtraction cannot underflow, fragmented headers cannot wedge the framer,
  decrypt refuses negative and empty lengths, and out-of-bounds reads and
  writes in KEYPAD/RAW decode and file transfer are fixed.
- Command and event data lengths are bounded at submit and again when the
  reply or command is built, so an oversized or negative length is refused
  instead of read past its array.
- The MbedTLS backend keeps no shared AES state between contexts, seeds its
  DRBG once at setup and stops on any library failure instead of relying on
  assert(). Before, a failed entropy draw under NDEBUG left a predictable
  nonce.
- The XWR command is NAKed by a PD that does not advertise smart card
  support.
- A BUSY loop and the BUG() reached when a deferred send was retried were
  both triggerable by peer or transport behaviour alone, and both are fixed
  (see Behavior changes and Fixes).

## Fixes

- A CP whose channel `send()` returned 0 (transient, retry later) hit BUG()
  on the next refresh and, on bare metal, hung until the watchdog reset it.
  A deferred send is now retried with the same finalized bytes as the
  channel contract promises.
- Feature engines stay bound to their PD when `osdp_cp_add_pd()` grows the
  PD array. Before, a running transfer reported into freed memory.
- Per-PD storage is freed when adding a PD fails, and adding PDs to an empty
  array no longer passes NULL to memcpy().
- With `OPT_OSDP_STATIC`, a CP and a PD context in the same process no
  longer share a feature slot, which had corrupted file transfers between
  them.
- File engine: re-registering ops no longer orphans a live transfer, a
  FILE_TX submitted from a FILE_TX completion is refused instead of being
  wiped, an empty file the CP refuses is closed again and a transfer is
  aborted on link loss.
- `osdp_cp_is_pd_enabled()` returns false for an invalid PD index instead of
  true.
- `sc_handshake_count` and `sc_failure_count` in `struct osdp_metrics` count
  secure channel activations and tear-downs as documented.
- A command ID inside the name table's range but with no entry no longer
  dereferences NULL, and an unknown capability code is logged as UNKNOWN.
- The software receive ring is drained on error-path resets, and the PD
  keeps its staged reply buffer across a zero-copy release.
- Bare-metal builds keep assert() and BUG() off stdio, so newlib's FILE
  layer, second heap and syscall stubs are no longer linked in.
- Format specifier and int-width fixes for targets where `uint32_t` is
  `unsigned long`, such as ESP32 and newlib ARM.

## Bindings

- Python: commands and events are typed dataclasses (`commands.LED(...)`,
  `events.CardRead(...)`) instead of dicts, and out-of-range fields raise
  `ValueError` at construction. Errors raise instead of returning `None` or
  `False`. Every existing Python application needs updating.
- Python: notifications follow the C rework with typed payloads
  (`MpOutcome`, `Notification.mp_outcome`, `NotificationType.PdId`,
  `.pd_id`), and `StatusReportType.Remote` is renamed `.Reader`.
- Python: new bindings for the smart card, biometric, TDSET and Transparent
  Reader Support features. `ControlPanel.trs_session()` is a card session
  helper, and there are also `ControlPanel.set_scbk()`,
  `PeripheralDevice.trs_get_max_apdu_len()`, `osdp.set_log_handler()` and
  `osdp.SerialChannel` (behind the `serial` extra).
- Python: device state is tracked from library notifications, so status
  checks no longer call into C or take a lock. Waits check their condition
  before sleeping.
- Python: fixed a refcount bug, an event dict leak, a use-after-free in
  `register_file_ops`, an unchecked allocation and a crash for PDs built
  without capabilities, and a malformed SCBK is rejected. Python 3.11 is now
  the minimum, and wheels are smoke-tested on every platform.
- .NET: new LibOSDP binding on NuGet for .NET 10, covering every exported
  function, with a load-time ABI check against the native library and
  native builds for each runtime ID.
- C++: `osdp.hpp` is back in sync with `osdp.h`, with const-correct methods
  and wrappers for TRS, SCBK replacement and ABI info.

## Build and platform

- Crypto backend selection with `--crypto` or `OPT_OSDP_CRYPTO_BACKEND`:
  `auto` (the default, which tries OpenSSL, then MbedTLS, then the bundled
  TinyAES), `openssl`, `mbedtls` or `tinyaes`. A build that names no backend
  now picks a system library instead of silently falling back to TinyAES.
- PSA Crypto API backend, which is the Zephyr default
  (`CONFIG_LIBOSDP_CRYPTO_PSA`), for Mbed TLS 4 and hardware AES. Host
  builds do not offer it yet.
- Zephyr: new `CONFIG_LIBOSDP_STATIC`, `CONFIG_LIBOSDP_LOG_MINIMAL`,
  `CONFIG_LIBOSDP_PACKET_TRACE` and `CONFIG_LIBOSDP_DATA_TRACE` options,
  Zephyr 4.5 support and quality-of-life fixes for integration. Static
  allocation and zero-copy RX are on by default there.
- ESP-IDF component for ESP-IDF 5.5 or later, published to the component
  registry as `sidcha/libosdp`, using `esp_timer_get_time()` and
  `esp_random()`.
- Arduino and PlatformIO are first-class platforms: ESP32 and ESP8266 use
  their hardware TRNG. Cores without one (AVR, SAMD) fall back to a
  `micros()`-seeded PRNG, which is not a CSPRNG, so Secure Channel
  deployments there must provide `rand_u32()`.
- Windows and MSVC build support, `--lib-only` and `OPT_OSDP_LIB_ONLY` for
  library-only builds, and the shared library's DLL is installed on Windows.
- OpenBSD support: `rand_u32()` uses arc4random(), and LibOSDP is packaged
  in the OpenBSD ports tree as `devel/libosdp`.
- CMake: `find_package(libosdp)` works for every crypto backend, install
  paths and pkg-config output are fixed, the MbedTLS include and library
  paths are exported like OpenSSL's and the minimum CMake version is 3.14.
  The git revision can be supplied by the caller, and a parent project's git
  state is no longer stamped on the library.
- `configure.sh` passes `--crypto-ld-flags` to the linker verbatim.
- Removed packet capture and other hosted-only libc surface from
  `__BARE_METAL__` builds, and moved the logger out of each PD to save RAM.
- The unit tests run as separate suites in parallel, under both RX paths,
  with static allocation and under ASan and TSan. The interop check proves
  two revisions are wire- and API-compatible.
- The guides behind doc.osdp.dev now live in `doc/`, so each release branch
  carries the documentation for the API it ships.

## Migration notes

- Pass your `struct osdp_channel` to `osdp_cp_setup(&channel, num_pd, info)`
  or `osdp_pd_setup(&channel, info)` and drop `channel` from every
  `osdp_pd_info_t`. A CP with PDs on several buses becomes one context per
  bus.
- Register a completion callback before the first submit, keep each
  submitted command or event alive until its completion fires and free or
  reuse it there. A submit that returns -1 was never queued and stays yours.
- To learn a command's outcome, use the completion callback instead of
  `OSDP_NOTIFICATION_COMMAND`. Read notifications through their typed member
  (`n->sc_status.active`, `n->pd_status.online`, `n->mp.outcome`) instead of
  `arg0`/`arg1`.
- Rename `osdp_cp_send_command()` to `osdp_cp_submit_command()`,
  `osdp_pd_notify_event()` to `osdp_pd_submit_event()`,
  `osdp_event_notification` to `osdp_notification` and
  `OSDP_STATUS_REPORT_REMOTE` to `OSDP_STATUS_REPORT_READER`.
- Add the leading `int pd` argument to your log callback.
- Call `osdp_cp_add_pd()`, `osdp_pd_set_capabilities()`,
  `osdp_file_register_ops()` and the callback setters before the first
  refresh, and check their return value.
- File transfer: watch `OSDP_NOTIFICATION_MP_DONE` and read `mp.outcome`
  instead of `FILE_TX_DONE`, and call
  `osdp_cp_cancel(ctx, pd, OSDP_MP_MSG_FILE_TRANSFER)` instead of submitting
  a FILE_TX with the cancel flag.
- Advertise `OSDP_PD_CAP_READERS` if your commands or events name a
  sub-reader, and send full status reports sized to the PD's capabilities.
- End capability arrays with `OSDP_PD_CAP_SENTINEL`.
- Include `<libosdp/osdp.h>`, or keep `<osdp.h>` and take the include path
  from pkg-config or `find_package(libosdp)`.
- A PD that relied on starting without a key and being keyed over SCBK-D
  must now set `OSDP_FLAG_INSTALL_MODE`. A CP that replaces a PD on an
  address that already ran a secure channel needs an explicit
  re-provisioning step, because the CP no longer falls back to SCBK-D there.
- On a bare-metal target that c-utils cannot identify, provide
  `osdp_millis_now()` (returning `tick_t`), `rand_u32()` from a hardware
  entropy source and, unless building with `OPT_OSDP_STATIC`,
  `osdp_calloc()` and `osdp_free()`.
- PD: a KEYSET on a real-key session reaches the command callback when a
  session on the new key proves it, not when the command arrives. The
  callback still means "persist this as the key in use", and it may return
  `OSDP_PD_CMD_BUSY` while it does.
- CP: a KEYSET completes when a session settles the new key, not on the ACK.
  OK means the new key is in use and FAILED means the PD stays on its
  current key. Store both keys until the completion fires.
- Python: move from dict payloads to the `osdp.commands` and `osdp.events`
  dataclasses, catch exceptions instead of checking return values and update
  notification and status handlers for the typed payloads and
  `StatusReportType.Reader`.
