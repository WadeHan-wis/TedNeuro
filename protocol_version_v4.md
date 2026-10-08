# fNIRS BLE Protocol Specification — v4

**Firmware reference version:** v0.1.39 (updated 2026-10-07; draft for v1.0.0 release - the protocol itself is unchanged since v4)
**Protocol version:** 4 (`BLE_PROTOCOL_VERSION = 4`, readable via the VERSION characteristic below)

## Scope

This document describes the BLE GATT interface exposed by the fNIRS device firmware: service/characteristic layout, the measurement configuration format, and the streamed data (batch) frame format.

---

## 1. GATT Service / Characteristics

Base UUID: `xxxxxxxx-1212-efde-1523-785feabcd123`

| UUID (16-bit shorthand) | Name | Properties | Size | Purpose |
|---|---|---|---|---|
| `0x1523` | AS7341_SERVICE | Service | — | Primary service UUID, also advertised for scan filtering |
| `0x1525` | AS7341_CONFIG | Read + Write | 10 bytes | Measurement configuration (gain/integration time/LED duty/cycle/active window) |
| `0x1526` | AS7341_DATA0 | Notify | variable (1 + 14×N bytes) | Batched samples from NIR sensor 1 |
| `0x1527` | AS7341_DATA1 | Notify | variable (1 + 14×N bytes) | Batched samples from NIR sensor 2 |
| `0x1528` | AS7341_VERSION | Read-only | 4 bytes | `fw_version` (u16 LE) + `protocol_version` (u16 LE) |
| `0x1529` | AS7341_SEQ | Notify (declared, inactive) | 4 bytes | Legacy — retained for backward compatibility only; no longer notified since protocol v3 (sequence number is now embedded in each DATA0/DATA1 record instead) |
| `0x152A` | AS7341_DROPPED_COUNT | Read-only | 4 bytes | Cumulative ring-buffer overflow counter (u32 LE) — poll this to distinguish a real data-loss gap from a benign notify delivery miss |

The device advertises the `AS7341_SERVICE` (0x1523) UUID; the full device name (see §4) is carried in the scan response, not in the advertising packet itself.

---

## 2. AS7341_CONFIG (0x1525) — 10 bytes, little-endian

| Offset | Size | Field | Range / Unit | Notes |
|---|---|---|---|---|
| 0 | 1 | `integration_time` | 1–255 (×20ms nominal) | Internally mapped to a fixed ATIME + variable ASTEP |
| 1 | 1 | `location_interval` | seconds | **Not implemented** — accepted and read back, but has no effect on device behavior |
| 2 | 1 | `led1_pwm` (640nm) | 0–100% | |
| 3 | 1 | `led2_pwm` (680nm) | 0–100% | |
| 4 | 1 | `led3_pwm` (980nm) | 0–100% | |
| 5 | 1 | `led4_pwm` | 0–100% | **Ignored** — hardware only has 3 LEDs |
| 6–7 | 2 | `cycle_period_ms` | ms (u16 LE) | Controls how often a measurement is taken |
| 8–9 | 2 | `active_window_ms` | ms (u16 LE) | Clamped to `cycle_period_ms` if larger |

**Power-on defaults** (before any CONFIG write, and again after every reboot/power cycle): gain = 256× (fixed — gain is not part of CONFIG), `integration_time` = 1 (≈19.7ms), `cycle_period` = `active_window` = one RTC tick (100ms, i.e. no gating — free-running at ~10Hz), LED duty 25% / 50% / 75% (640 / 680 / 980nm).

**Required application configuration for v1.0 (the validated measurement settings).** The power-on defaults above are *not* the validated measurement settings (integration time and cycle/active window differ). The application **must write the following CONFIG after every connection** (settings are lost on every reboot or power cycle):

| Field | Value | Result on the device |
|---|---|---|
| `integration_time` | 5 | ≈99.8ms (ATIME 99 fixed, ASTEP 358; not exactly 100ms) |
| `led1/2/3_pwm` | 25 / 50 / 75 (%) | 640nm 25%, 680nm 50%, 980nm 75% |
| `cycle_period_ms` | 300 | one measurement every 300ms (≈3.33Hz per sensor) |
| `active_window_ms` | 100 | LEDs on and sensors integrating for 100ms of each cycle |

Payload (hex, little-endian): `05 01 19 32 4B 00 2C 01 64 00` (offset 1 `location_interval` has no effect; offset 5 `led4_pwm` is ignored). Values are applied on the next 100ms device tick. Cycle and active window are rounded to the nearest 100ms tick (minimum 1 tick), so values should be multiples of 100ms. Read the characteristic back to confirm the applied values.

The firmware has no concept of a "session" — it simply retains the last CONFIG value written until power-off. The connecting application is expected to explicitly write its desired settings after connecting; nothing is applied automatically beyond the power-on defaults above.

Configuration can be read back at any time via a normal characteristic read on the same UUID, to confirm applied values.

---

## 3. AS7341_DATA0 / AS7341_DATA1 (0x1526 / 0x1527) — batched sample frames

Each characteristic streams samples from one of the two onboard NIR sensors independently. Both use the same frame format.

### Frame layout

| Field | Size | Description |
|---|---|---|
| `sample_count` | 1 byte | Number of records in this notification (≥ 1) |
| `records[sample_count]` | 14 bytes each | See below |

### Record layout (14 bytes, little-endian)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0–3 | 4 | `timestamp_us` | u32 LE, device-relative microsecond counter. **Wraps every 2³² µs (≈71.58 minutes)** — this is expected, defined-behavior overflow, not a fault. Do not treat a decreasing value as an error by itself; use it only for short-interval timing, or unwrap it if you need an absolute session clock. |
| 4–7 | 4 | `seq_num_and_flag` | u32 LE. **Bit 31** = dark-frame flag (see §5). **Bits 0–30** = monotonic sequence number (wraps at ~2.1 billion — not a practical concern). |
| 8–9 | 2 | `red630` | u16 LE, raw ADC count |
| 10–11 | 2 | `red680` | u16 LE, raw ADC count |
| 12–13 | 2 | `nir` | u16 LE, raw ADC count |

**Timing with the required configuration (§2).** Per sensor, a lit sample is taken every 300ms; after every third lit sample one dark frame (§5) follows 100ms later, and the next lit sample comes 200ms after the dark frame (lit-to-lit interval stays 300ms). Sensor 1 (DATA0) and sensor 2 (DATA1) carry the same `timestamp_us` and `seq_num` for each sample. `timestamp_us` is the device's tick counter, so lit samples can show a one-tick (100ms) offset in a small fraction of samples (about once per minute: a 0.2s/0.4s pair); the average interval remains 0.300s and no samples are lost.

**Delivery latency.** Samples are queued on the device and flushed in batches at most every 2 seconds, so data arrives in bursts, up to 17 records per notification (at MTU 247). Use `seq_num` and `timestamp_us` — not the notification arrival time — to order and time samples.

`sample_count` (i.e. the batch size, N) is not fixed — it is computed automatically per connection from the negotiated ATT MTU (up to a maximum of 17 records/frame at MTU 247). If MTU negotiation fails, it falls back to 1 record/frame, using the same frame format (no separate legacy format).

Values are **raw, unprocessed sensor counts** — no calibration, no Beer-Lambert conversion, no baseline subtraction is applied on-device except for the dark-frame mechanism below.

---

## 4. Advertising / Connection Parameters

| Item | Value | Status |
|---|---|---|
| Advertised name | `TedNeuro_v{MAJOR}.{MINOR}.{PATCH}` (e.g. `TedNeuro_v0.1.39`) | Updated every firmware patch release |
| Connection interval | 30ms | Requested by the device; the central may override |
| Peripheral latency | 4 | Initial value |
| Supervision timeout | 4000ms | Requested by the device; the central may override |
| Target MTU | 247 (244-byte payload) | Falls back automatically if the central rejects it |
| Tx power | -8dBm (fixed) | **Applied from v0.1.39.** v0.1.34-v0.1.38 reported -8dBm but actually transmitted at 0dBm (the setting was rejected); range and battery measurements taken with those versions are 0dBm figures |
| Pairing / bonding | Disabled | Intentional for now; may be revisited before production |

---

## 5. Dark-frame subtraction (introduced in v4)

Starting at firmware v0.1.24, an additional **dark frame** is inserted into the stream: a measurement taken with all LEDs off, using the *same* gain/integration-time settings as the surrounding lit samples. This exists to let the receiving application subtract out ambient light and sensor dark current:

The ratio is **1 dark frame per 3 lit samples (3:1) since firmware v0.1.34** (it was 1 per 10 lit samples in v0.1.24-v0.1.33). Dark frames are additional samples, not replacements: the lit sample rate (3.33Hz per sensor with the §2 configuration) is not reduced.

```
corrected_signal = lit_raw - most_recent_dark_raw   (per channel, no scaling needed —
                                                       both use identical gain/integration time)
```

Because the AS7341's raw count is linear with integration time (a pure accumulation, not a multiplicative/gain-dependent transform, given equal gain/integration settings), a straight subtraction is valid with no scaling factor.

Implementation notes for the receiving application:
- Identify dark frames via bit 31 of `seq_num_and_flag` (§3), not by any separate field.
- Dark frames arrive at a lower rate than lit frames — hold the most recent dark value per sensor and reuse it for each subsequent lit sample until a new one arrives.
- Dark frames should **not** be plotted alongside lit frames as-is; they are expected to read near zero/ambient level (about 0.4-1.5 counts in a dark room for sensor 1, 2-4 counts for sensor 2; several counts higher in a lit room).
- If ambient light changes rapidly between a dark frame and the following lit frames (e.g. a strong external light source flickering), the subtraction can produce negative results — this is a known, physically-expected limitation of the technique, not a bug.

---

## 6. Known hardware caveats affecting interpretation

- **LED3 wavelength**: specified as 950nm, but the currently deployed prototype board ("PoC v1") is populated with a 980nm LED part instead. Firmware field names and this document refer to it as "950nm" throughout for consistency, but the actual physical wavelength is 980nm on current hardware.
- **"NIR" channel**: this is the AS7341's NIR photodiode channel (~910nm peak response), used as the closest available filter to the 950/980nm LED — it is a raw ADC channel, not a derived or named-wavelength-specific measurement.
- **Sensor 2 (DATA1) baseline signal** is consistently much weaker than sensor 1 (DATA0): typically 5-80 counts versus 1,500-5,000 counts on a sleeping wearer (sensor 2 is the far detector). It can also differ by a factor of 3-4 between sessions of the same device (placement dependent). It should not be assumed to be a parsing error.
- **A sensor that fails initialization at boot is excluded until the next reboot** and its raw fields are sent as **0** for the whole session. The protocol has no status field, so an excluded sensor cannot be distinguished from a genuine zero reading except by the value staying exactly 0. Known cause under investigation: poor solder contact on the sensor package on some prototype units (reflowing the sensor restored operation on the units tested). Applications should treat a channel that is constantly 0 as "sensor unavailable".
- **No wear-detection status.** The device cannot tell whether it is on the skin. A detached device keeps streaming; typical signs in raw counts are a sudden change of the NIR/640nm ratio and of the sensor 2 level, followed by flat signals without a cardiac component. Detached segments should be excluded from analysis by the application.
- **Device clock.** The device time base runs about **+107ppm (0.011%) faster** than the phone clock (about 4.4s over 12 hours; cause under investigation). There is no absolute time on the device: use the phone's receive time as the wall-clock reference and `seq_num` plus the 300ms cycle for sample ordering.

---

## 7. Connection loss and buffering

- After a connection is established the device blinks its LEDs and starts measuring automatically; the application should write CONFIG right after connecting (see the required configuration in §2).
- If the connection drops, the device **keeps measuring and buffering for 22 seconds** (220 device ticks; buffer capacity 220 samples) and delivers the backlog in order after reconnection. `seq_num` stays continuous across the gap.
- If the connection is not re-established within 22 seconds the device enters a low-power standby (single slow LED blink) and **stops measuring**; data in that period is not recorded (the device has no on-board storage).
- If the buffer overflows, the oldest samples are overwritten and `AS7341_DROPPED_COUNT` (0x152A) increases; compare it with gaps in `seq_num` to tell real data loss from a missed notification.
- Do not rely on the 22s figure for design: it is limited by the device's RAM and is not a product guarantee.

## 8. Firmware update (OTA, MCUboot + SMP over BLE)

- The device supports over-the-air updates with the standard SMP image-management service (nRF Connect Device Manager / mcumgr). The update file is `dfu_application.zip`; the image version comes from the firmware `VERSION` file and must match the firmware's own version fields.
- An uploaded image is applied in **test** mode: the device swaps to it at the next reboot and runs it once. **The application (or operator) must send Confirm after verifying the new firmware**; an unconfirmed image is reverted to the previous version at the next reset, whether the reset was a power cycle or a fault.
- Verified on 2026-10-07 with v0.1.39 as the running image (one unit): an unconfirmed image reverts at the next reset; an image that crashes and halts after boot is reverted at the next reset (the hardware watchdog is expected to reset a halted device within about 4 seconds; whether the watchdog or a manual power cycle caused the reset was not separately verified); an image with a corrupted signature is not executed; power loss at 70% of an upload leaves the previous firmware intact and the upload can be repeated; a normal update stays after Confirm.
- The signing key is currently the build system's default debug key and pairing/encryption is disabled, so the update service is unauthenticated. Both are intentional for the current development stage.

## 9. Protocol version history

| Version | Change |
|---|---|
| v1 | Fixed 8-byte frame, no timestamp/seq_num, included a now-dead LED-index field |
| v2 | Frame expanded to 16 bytes, added `timestamp_us`/`seq_num` directly |
| v3 | Switched to the variable-length batching frame (`[count] + record×N`), removed the dead LED-index field |
| **v4 (current)** | Same frame structure as v3; repurposes bit 31 of `seq_num` as a dark-frame flag (no frame-length change) |

Firmware changes since v0.1.26 that do not change the protocol: dark-frame ratio 1:10 to 3:1 with lit-to-lit timing kept at 300ms (v0.1.34-v0.1.37), Tx power -8dBm actually applied (v0.1.39), boot-time handling of a failed sensor (v0.1.32), default LED duty 25/50/75% (v0.1.36).

Applications should read `AS7341_VERSION` (0x1528) at connection time and confirm `protocol_version == 4` before assuming this frame layout.
