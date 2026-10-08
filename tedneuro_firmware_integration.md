# fNIRS Firmware — TedNeuro Integration: Required Changes, Source Access & Build Guide

**Firmware reference version:** v1.0.0 (released 2026-10-08; see `protocol_version_v4.md` for the BLE protocol this build implements)

## 1. Summary

- **No firmware changes are required to integrate with TedNeuro against the current protocol** as documented in `protocol_version_v4.md`. The device already implements a stable v4 BLE protocol (GATT service, CONFIG, batched DATA0/DATA1, VERSION, DROPPED_COUNT).
- A short list of **optional/likely-useful additions** exists (see §2) if TedNeuro needs diagnostic visibility (sensor status/saturation flags) beyond raw counts — these are not implemented yet and would need to be scoped and prioritized together.

---

## 2. Comparison against the baseline sample code

Your reference point ("sample code") is the nRF5 SDK example this project's protocol lineage originates from (bare-metal/FreeRTOS + SoftDevice, single AS7341 sensor, `ble_as7341.c` GATT service, `led.c` 2-channel LED driver). The production firmware has diverged from that baseline in the following ways:

| Aspect | Baseline sample code | Current production firmware (v1.0.0) |
|---|---|---|
| Platform | nRF5 SDK + FreeRTOS + SoftDevice (S112/S132/S140) | Nordic nRF Connect SDK (Zephyr RTOS) |
| Sensor count | 1× AS7341 | 2× AS7341 (independent I2C buses), reported as DATA0/DATA1 |
| LED channels | 2 (RED %, IR %) | 3 (640nm / 680nm / 980nm, individually controlled) |
| Timing source | SoftDevice/RTOS default timers | Dedicated hardware RTC (nrfx, directly driven) with drift correction, decoupled from the BLE stack |
| GATT layout | `BLE_UUID_AS7341_SERVICE` / `_LED_CONFIG_CHAR` / `_DATA_CHAR_DEV0`/`DEV1` (same naming lineage) | Same UUID base and characteristic role (CONFIG, DATA0/1), but frame format has evolved through 4 protocol revisions (see `protocol_version_v4.md` §9) |
| Data framing | Simple per-sample notify | Variable-length **batched** frames (multiple samples per notification, negotiated by MTU) for radio power efficiency |
| Ambient light handling | None | Periodic **dark-frame** (LEDs-off) samples interleaved for ambient/dark-current subtraction |
| Saturation detection | None found in reference | Hardware-register-based (AS7341 STATUS2 ASAT bits), evaluated on-device for fault handling; **not transmitted** in the data record (no status field in protocol v4) |
| OTA / firmware update | None | MCUboot + Zephyr SMP (BLE-based) update and rollback |
| Reliability | None found in reference | Hardware watchdog, ring-buffer-backed BLE disconnect tolerance, defined safe-state fault handling |
| Other peripherals in baseline | EEG (ADS1299), motion (MPU), battery service | Not present / not used — out of scope for this device's current spec |

**Net effect:** if TedNeuro's integration work was scoped against the sample code's simpler protocol (single sensor, 2-channel LED, unbatched fixed-size frames), that assumption needs to be updated to the current spec in `protocol_version_v4.md`. This is very likely the source of the mismatches identified in the earlier Q&A (e.g. the assumed 8-byte fixed frame with an LED-position field, and the assumed 4-byte LED command).

### Firmware changes specifically for TedNeuro (open items, not yet implemented)

None of these are required to consume the current protocol — they would only matter if TedNeuro needs the capability:

| Candidate addition | Why it might matter to you | Status |
|---|---|---|
| Per-sample status/error byte in the DATA0/DATA1 record (sensor saturation, low-signal, I2C error) | Lets the app see sensor health without needing a debug/RTT session | Not implemented — open item |
| Persisted last-reset-reason exposed over BLE | Lets the app report *why* a device disconnected/rebooted | Not implemented — open item |
| Cumulative error-event counters (saturation count, I2C error count) alongside the existing `DROPPED_COUNT` | Session-level health summary without needing continuous monitoring | Not implemented — open item |
| Contact-quality / "signal ready" logic | Needed if TedNeuro expects the device itself to gate recording start | **Not implemented at all** — would be new scope, not a modification of existing logic |

If any of these are required for your integration, please specify which ones and we can scope the work.

---


## 3. Build / Toolchain Guide (current version, v1.0.0)

### 3.1 Prerequisites

| Component | Version used for this build |
|---|---|
| nRF Connect SDK (NCS) | v3.4.0 |
| Toolchain | NCS-bundled toolchain (includes `west`, CMake, Ninja, GCC ARM cross-compiler, Python) |
| Target board | `nrf52dk/nrf52832` (PoC v1 prototype; see `protocol_version_v4.md` §6 for known hardware caveats) |
| Build system | `west` (Zephyr's meta-tool), CMake + Ninja under the hood |

The simplest way to obtain a matching environment is via **nRF Connect for Desktop → Toolchain Manager**, installing **NCS v3.4.0** and its bundled toolchain. This provides a working `west`, compiler, and all Python dependencies without manual setup.

### 3.2 One-time workspace setup

If starting from just the firmware application source (not a full west workspace):

```bash
# From a directory that will become your NCS workspace root
west init -m <manifest-repo-url-or-local-path> --mr main
west update
```

If the firmware is provided as an application folder to be built inside an *existing* NCS v3.4.0 workspace (as is done internally), no `west init` is needed — just point `west build` at the application folder as shown below.

### 3.3 Build

Using the NCS-bundled toolchain's Python/environment (via nRF Connect for VS Code, or an "NCS Terminal" from Toolchain Manager, or manually with the bundled Python on `PATH`):

```bash
west build -b nrf52dk/nrf52832 <path-to-Application-folder>
```

For a **clean/pristine rebuild** (required after changing `VERSION`, `prj.conf`, or `Kconfig`-affecting files):

```bash
west build -b nrf52dk/nrf52832 <path-to-Application-folder> -p always
```

This is a **sysbuild** project (MCUboot is built automatically as a child image alongside the application) — no separate bootloader build step is needed.

### 3.4 Build outputs

| File | Location (relative to build dir) | Purpose |
|---|---|---|
| `zephyr/zephyr.elf` / `zephyr.hex` | `build/Application/` | Raw application image (unsigned) |
| `zephyr/zephyr.signed.bin` / `zephyr.signed.hex` | `build/Application/` | MCUboot-signed application image (`.hex` for wired flashing, `.bin` for mcumgr) |
| `zephyr/zephyr.hex` | `build/mcuboot/` | MCUboot bootloader (flash it first on a blank device) |
| `dfu_application.zip` | `build/` | OTA package (contains signed image + `manifest.json` with version metadata) for BLE-based firmware update (Zephyr SMP / nRF Connect Device Manager app) |

The v1.0.0 release files are collected, renamed and checksummed in `RELEASE_NOTES_v1.0.0.md` §1 (`TedNeuro_v1.0.0_dfu.zip`, `TedNeuro_v1.0.0.signed.bin`, `TedNeuro_v1.0.0_app.signed.hex`, `mcuboot.hex`).

`manifest.json` inside `dfu_application.zip` includes a `version_MCUBOOT` field (e.g. `"1.0.0+0"`) that should match the firmware's `AS7341_VERSION` characteristic value at runtime — use this to confirm you're testing against the expected build.

### 3.5 Flashing

- **Wired (SWD/J-Link)**: `west flash` (flashes MCUboot and the application; requires a J-Link or compatible debug probe connected to the board's SWD pins).
- **Wireless (OTA)**: use `dfu_application.zip` with a Zephyr SMP-compatible client (e.g. Nordic's "nRF Connect Device Manager" mobile app) against the already-running device over BLE — no wired connection needed. This is the normal update path once a device is deployed; wired flashing is primarily used for factory programming and low-level debugging.

### 3.6 Verifying a build against this document

After flashing, connect and read the `AS7341_VERSION` (0x1528) characteristic — it should report `protocol_version = 4`, matching `protocol_version_v4.md`. If a different value is returned, the firmware predates or postdates this spec and the wire format may differ.
