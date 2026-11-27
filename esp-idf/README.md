# LibOSDP for ESP-IDF

ESP-IDF component wrapper for [LibOSDP](https://github.com/osdp-dev/libosdp), an
implementation of the Open Supervised Device Protocol (IEC 60839-11-5).

This directory holds the component definition. The **component root is the repo
root**, not this directory — ESP-IDF resolves a component to the directory that
contains `CMakeLists.txt`, and the packaging step for the ESP Component Registry
archives the component directory, so a component rooted here could never ship
`src/` and `utils/`.

| File | Role |
|---|---|
| `../CMakeLists.txt` | Branches on `ESP_PLATFORM` and hands over to `component.cmake` |
| `../Kconfig` | One `rsource` line — ESP-IDF globs `<component>/Kconfig` before registration, so it must live at the root |
| `../idf_component.yml` | Registry manifest |
| `component.cmake` | `idf_component_register()` — the port of `zephyr/CMakeLists.txt` |
| `Kconfig.libosdp` | Configuration — the port of `zephyr/Kconfig` |
| `src/osdp_esp.c` | Platform glue: tick source and entropy |

## Using it

Either add the dependency:

```bash
idf.py add-dependency "sidcha/libosdp"
```

or vendor it as a submodule, in which case it is picked up automatically:

```bash
git submodule add --recursive https://github.com/osdp-dev/libosdp.git components/libosdp
```

`--recursive` is not optional: LibOSDP's `utils/` is itself a submodule, and the
build fails on missing `utils/src/*.c` without it.

Then, in your component's `CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c" REQUIRES libosdp driver)
```

Configuration lives under `Component config → Open Supervised Device Protocol
(OSDP)` in `menuconfig`. Symbol names match the Zephyr module, so configuration
knowledge carries across.

## Platform notes

**Tick source.** `osdp_millis_now()` is backed by `esp_timer_get_time()`.
`xTaskGetTickCount()` is deliberately not used: at the default 10 ms FreeRTOS
tick it is too coarse for `OSDP_RESP_TOUT_MS` and the inter-packet timing in
`osdp_phy.c`.

**Entropy.** `rand_u32()` is backed by `esp_random()`. This only bites on the
**TinyAES** backend, which builds Secure Channel's random bytes out of
`rand_u32()`; the MbedTLS backend uses MbedTLS's own DRBG, and there nothing
references `rand_u32()` at all — the linker drops it. The override still matters
because the two backends are one Kconfig flip apart and LibOSDP's fallback on
ESP-IDF is libc `rand()`, a fixed sequence from a fixed seed.

Note that `esp_random()` is a true hardware RNG only while Wi-Fi or Bluetooth is
active, or while the SAR ADC entropy source is enabled. **An application running
Secure Channel over TinyAES without RF should call `bootloader_random_enable()`
before `osdp_pd_setup()`**, and `bootloader_random_disable()` before later
starting Wi-Fi or BT — the two cannot share the entropy source.

**Threading.** LibOSDP is not internally synchronised. Serialise every call
against one context onto a single task; see the `osdp_pd_refresh()` docs in
`osdp.h`.

**Zero-copy RX** (`CONFIG_LIBOSDP_RX_ZERO_COPY`) defaults to `n` here, unlike the
Zephyr module. It changes the shape of `struct osdp_channel` — the application
supplies `recv_pkt()`/`release_pkt()` instead of `recv()` — and ESP-IDF's UART
driver already ring-buffers internally, so the copy it avoids is one that
`uart_read_bytes()` performs anyway.

**PD-only applications** should set `CONFIG_OSDP_PD_MAX=1`. With
`CONFIG_LIBOSDP_STATIC=y` (the default) the PD arrays are sized to that symbol,
and the default of 126 costs several KB of RAM for PDs that will never exist.
LibOSDP has no option to drop the CP role from the build; `osdp_cp.c` always
compiles in, but `--gc-sections` removes most of it from a PD-only link.

## Publishing

```bash
compote component upload --namespace sidcha --name libosdp
```

Run it from a `--recursive` clone. The packer walks the working tree, so a
non-recursive clone would publish a component with an empty `utils/` that cannot
compile. Bump `version` in `../idf_component.yml` to match
`project(libosdp VERSION ...)` and `LIBOSDP_PRERELEASE` in `../CMakeLists.txt`
first — the registry refuses to overwrite an existing version.
