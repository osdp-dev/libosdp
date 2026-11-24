---
lastUpdated: 2026-11-26
description: "LibOSDP channel abstraction — implement the send, receive, flush and close callbacks that connect the OSDP library to your RS-485 or serial transport, including the zero-copy receive variant."
---
# Communication Channel

LibOSDP uses a *channel* to communicate with devices. It is up to the user to
define a channel and pass it to [`osdp_cp_setup`](/api/control-panel) or
[`osdp_pd_setup`](/api/peripheral-device). There is one channel per context:
a CP shares it among all of its PDs. A channel is defined as:

```c
struct osdp_channel {
    void *data;
    osdp_read_fn_t recv;
    osdp_write_fn_t send;
    osdp_flush_fn_t flush;   /* optional */
    osdp_close_fn_t close;   /* optional */
};
```

All callbacks must be non-blocking; LibOSDP calls them from inside
`osdp_cp_refresh()` / `osdp_pd_refresh()`.

<!-- doxygen:struct:osdp_channel:members -->

## Sending

`send` is **all or nothing**, with a three-way return:

| Return | Meaning |
|---|---|
| `len` | The whole packet was queued for transmission. |
| `0` | The channel cannot take the packet right now (a half-duplex turnaround gap not yet elapsed, an earlier packet still draining). LibOSDP calls `send` again on a later refresh with the **same** finalized bytes; nothing is rebuilt and no sequence number or secure channel state advances in between. |
| `< 0` | Fatal error; the packet is dropped. |

There is no partial-write loop: a return between `0` and `len` is treated as a
failure, so a callback that cannot queue the whole buffer must return `0` and
wait to be asked again.

<!-- doxygen:typedef:osdp_write_fn_t -->

## Receiving

<!-- doxygen:typedef:osdp_read_fn_t -->

<!-- doxygen:typedef:osdp_flush_fn_t -->

<!-- doxygen:typedef:osdp_close_fn_t -->

### Zero-copy receive

A build with `OPT_OSDP_RX_ZERO_COPY` (`--zero-copy` in `configure.sh`;
`CONFIG_LIBOSDP_RX_ZERO_COPY` on Zephyr, where it is the default) replaces
`recv` with a pair of callbacks, and the transport takes over framing:

```c
struct osdp_channel {
    void *data;
    int (*recv_pkt)(void *data, const uint8_t **buf, int *max_len);
    void (*release_pkt)(void *data, const uint8_t *buf);
    osdp_write_fn_t send;
    osdp_flush_fn_t flush;
    osdp_close_fn_t close;
};
```

- `recv_pkt` (type `osdp_read_pkt_fn_t`) returns 0 when a complete packet is
  available, pointing `*buf` at it and setting `*max_len` to how many bytes
  LibOSDP may read there; it returns non-zero when there is none.
- `release_pkt` (type `osdp_release_pkt_fn_t`) hands that buffer back.
  LibOSDP calls it exactly once for every packet `recv_pkt` returned.

The library then needs no receive ring buffer of its own (`OSDP_RX_RB_SIZE`),
which is a large RAM saving on small targets.

The option changes the layout of `struct osdp_channel`, so the library and the
application must be built with the same setting; it is an ABI choice, not a
run-time toggle. `osdp_get_abi_info()` reports it as `OSDP_ABI_OPT_RX_ZERO_COPY`
(see [Miscellaneous](/api/miscellaneous#build-information)).
