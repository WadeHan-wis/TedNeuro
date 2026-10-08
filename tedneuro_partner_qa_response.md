# Response to TedNeuro Integration Questions

> **Status note (2026-10-08):** This is the answer set sent when the firmware was **v0.1.26**; the answers are kept as written. The current release is **v1.0.0**. Where the current behavior differs, use `protocol_version_v4.md` (v1.0.0) as the reference. Differences:
>
> | Question | Answer given at v0.1.26 | Current (v1.0.0) |
> |---|---|---|
> | Q12 device name | `TedNeuro_v0.1.26` | `TedNeuro_v1.0.0` |
> | Q14 dark frames | about 1 in 10 samples | **1 dark frame per 3 lit samples**; lit samples every 300ms with the required CONFIG |
> | Q14, Q17–18 defaults | free-running ~10Hz, ≈19.7ms integration, all LEDs 50% | power-on defaults are gain 256×, ≈19.7ms, 100/100ms, LED 25/50/75%; **the application must write CONFIG `05 01 19 32 4B 00 2C 01 64 00` after every connection** (≈99.8ms integration, 300/100ms cycle) |
> | Q30 delivered / latest version | v0.1.23 / v0.1.26 | latest release v1.0.0 |
> | Bit 31 of `seq_num` | dark flag | unchanged — mask it off (`& 0x7FFFFFFF`) before comparing sequence numbers |

One note up front: the GATT UUIDs you referenced (`...1525...`, `...1526...`) match our firmware exactly (base `xxxxxxxx-1212-efde-1523-785feabcd123`, service `0x1523`). However, there are a few corrections to make regarding the protocol itself — please review the packet-format answers below before implementing your parser. Full details are in the two attached documents, `protocol_version_v4.md` and `tedneuro_firmware_integration.md`.

---

## A. HbO/HbR signal processing (Q1–5)

**Q1–5: Not answerable — this does not exist in the firmware.** The MCU only acquires and transmits raw data per wavelength (Red630/Red680/NIR); it performs no Beer-Lambert conversion, no extinction-coefficient math, no baseline/reference logic, and computes no "water-sensitive" derived signal.

Physical wavelengths in use: LED1 = 640nm, LED2 = 680nm, LED3 = 980nm. The channel labeled "NIR" in the protocol is the AS7341's NIR photodiode channel (peak response ~910nm), used as the closest available filter to the 950/980nm source — it is a raw ADC channel, not a derived measurement.

## B. Signal / contact quality (Q6–10)

**Q6–9: Not answerable — there is no contact-quality algorithm in the firmware.** That said, to help address this, we've implemented ambient-light removal: a dark-frame value (sampled during LED-off time) is subtracted from the LED-on sampling data, which attenuates data variation caused by differences in contact quality.

**Q10:** We don't currently have prepared "good/bad" reference datasets. We do have a 3-day dataset collected on device firmware **v0.1.23** (the version we shared with you), but note that measurement quality may differ from the current latest development version, **v0.1.26**.

## C. BLE protocol (Q11–16)

**Q11: Confirmed.** Service UUID base is `0000**1523**-1212-efde-1523-785feabcd123`; `0x1525` = CONFIG (read+write), `0x1526` = DATA0 (NIR sensor 1, notify), `0x1527` = DATA1 (NIR sensor 2, notify). Also present: `0x1528` = VERSION (read-only, `fw_version` + `protocol_version`), `0x1529` = legacy SEQ (declared but no longer notified), `0x152A` = DROPPED_COUNT (read-only overflow counter).

**Q12:** Advertised device name: `TedNeuro_v{MAJOR}.{MINOR}.{PATCH}` (e.g. `TedNeuro_v0.1.26`), updated with every firmware patch. The advertising packet carries the 128-bit service UUID (`0x1523`); the full name is in the scan response. No manufacturer-specific data field is used.

**Q13: What you described matches an older/legacy version of the protocol, not the current one.** It is not a fixed 8 bytes — it's a variable-length batching frame: `[count (1 byte)][record × count]`, where each record is **14 bytes**: `timestamp_us` (u32 LE) + `seq_num_with_dark_flag` (u32 LE, bit 31 = dark-frame flag) + `Red630` (u16) + `Red680` (u16) + `NIR` (u16). **There is no LED-position field.**

**Q14:** Default sampling is free-running at the RTC tick rate (~10Hz / 100ms), with cycle/active-window gating configurable via CONFIG. As of v0.1.24, roughly 1 in every 10 samples is a "dark frame" (all LEDs off, for ambient-light removal) interleaved among the lit samples.

**Q15: This does not match our firmware.** The CONFIG characteristic (`0x1525`) is not 4 bytes — it's **10 bytes**: `[integration_time (u8, 20ms units)][location_interval (u8, seconds — currently unused, no effect on behavior)][LED1_PWM (u8, 0–100%)][LED2_PWM (u8)][LED3_PWM (u8)][LED4_PWM (u8, ignored — only 3 LEDs exist)][cycle_period (u16 LE, ms)][active_window (u16 LE, ms)]`.

**Q16:** A standard GATT write-with-response is sufficient at the transport level (the ATT status confirms success). There is no separate application-level acknowledgement characteristic — to confirm applied values, simply read CONFIG back (it supports read + write).

**Q17–18:** The firmware boots with hardcoded defaults (gain = 256×, integration time ≈ 19.7ms, always-on 10Hz operation with no gating, all three LEDs at 50% duty). It retains the last CONFIG value written until power-off. The connecting app must explicitly write its desired session settings after connecting — the firmware does not apply anything automatically beyond these boot defaults.

## D. Firmware source & modifications (Q19)

Please see the attached `tedneuro_firmware_integration.md` — it includes a comparison against your reference sample code and a build/toolchain guide for the current firmware version (v0.1.26).

## E. Cloud/server & session upload (Q20–26)

**Not answerable — no such backend exists in this codebase/project.** There is no HTTPS endpoint, upload logic, authentication scheme, server-side session handling, or retention policy anywhere in this firmware project.

## F. Patient identifiers / PHI (Q27–29)

**Not answerable — out of scope for firmware, and no such policy has been defined as far as we know.** This device firmware has no concept of patient identity at all — it only ever transmits sensor readings, a timestamp, and a monotonic sequence number. Patient ID format, PHI/compliance rules, and encryption/retention policy are not something firmware engineering can answer.

## G. Validation target (Q30–32)

**Q30:** The firmware version we delivered to you was **v0.1.23**. Since then, we've applied patches including ambient-light removal (dark-frame subtraction) and BLE stability improvements — the current latest firmware version is **v0.1.26**.

**Q31:** We don't have a physical unit or BLE capture ready to hand over right now.

**Q32:** Not yet defined — there are no formal end-to-end acceptance criteria (expected graphs, sample-rate tolerance, required session metadata, etc.) for a third-party integration scenario yet. This should be scoped out together once the protocol details above are settled.
