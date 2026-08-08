# BMC G3X `.00x` Waveform File Format (Current Best Understanding)

**Status:** Reverse-engineered; not vendor-documented.
**Scope:** `<serial>.000`, `.001`, ... waveform files produced by BMC G3X devices.
**Implementation:** `oscar/SleepLib/loader_plugins/bmcG3xDataParsing.cpp`
**Last updated:** 2026-05-23

---

## 1) File-Level Structure

- File series: `<serial>.000`, `<serial>.001`, `<serial>.002`, ...
- Per-file virtual span: 64 MiB (`0x4000000`). Virtual byte offset = `fileIndex × 64MiB + localOffset`.
- Packet size: `0x800` bytes (2048).
- No global file header; file is a flat stream of fixed-size packets.
- Valid packet marker at packet offset `0x000`: bytes `AD AA` (little-endian `0xAAAD`).
- Packets not starting with `AD AA` are skipped.
- **Ring-buffer layout:** Each `.00x` file is a circular ring buffer of 32 768 packets (= 32 768 s ≈ 9.1 h at 1 Hz). The device records continuously and overwrites the oldest packet first; consecutive `.00x` files are sequential (last packet of `.NNN` is immediately followed by first packet of `.NNN+1`). The physical wrap point within a file can be found by scanning for the largest forward timestamp jump between consecutive packets.

---

## 2) Packet Offset Map

Confidence legend:
- **High** — used in parser and repeatedly validated against BMC PAP-Link screenshots.
- **Medium** — used in parser; interpretation plausible but not fully confirmed.
- **Low** — observed/tracked only; not yet used in OSCAR.

| Offset | Size | Type | Meaning | Confidence |
|--------|-----:|------|---------|------------|
| `0x000` | 2 | `uint16 LE` | Packet magic `0xAAAD` | High |
| `0x002` | 2 | `uint16 LE` | **Calendar-day counter**: increments once per calendar day at approximately midnight; 143 = 2026-02-09, 144 = 2026-02-10, etc. Value is shared with the corresponding IDX record (`0x002` of each IDX data record). One .00x ring-buffer file typically spans multiple calendar days. | Medium |
| `0x004` | 6 | bytes | Timestamp: `year-1900, mon, day, hour, min, sec` | High |
| `0x00A` | 2 | `uint16 LE` | Pressure seed, hundredths cmH2O; used to prime pressure state if no EVT 0x42 precedes first packet | High |
| `0x074` | 2 | `uint16 LE` | Unknown timing parameter; range 172–261 cs observed (JCCPAP: 224–261, mean 234). Uncorrelated with `0x076` or `0x07E`. Sum `0x074 + 0x07E` ≈ 538–565 cs (device-dependent but near-constant, not tracking RR) — **not** patient Ti/Te. | Low |
| `0x076` | 2 | `uint16 LE` | Unknown timing parameter; range 276–309 cs observed (JCCPAP); positively correlated with RR (r=+0.20), negatively with pressure (r=−0.40). Strongly correlated with `0x07E` (r=+0.94); sum `0x076 + 0x07E` ≈ 602 cs (near-constant). | Low |
| `0x078` | 2 | `uint16 LE` | Quantised/binned version of `0x076`; 6 discrete values {277, 284, 290, 297, 303, 309} observed (JCCPAP); corr with `0x076` = 0.68. Likely a coarser representation of the same underlying signal. | Low |
| `0x07C` | 2 | `uint16 LE` | Unknown timing parameter; range 350–491 cs observed (JCCPAP); correlated with `0x07E` (r=+0.83), negatively with pressure (r=−0.36). Wider range than `0x07E`; relationship unclear. | Low |
| `0x07E` | 2 | `uint16 LE` | Unknown timing parameter; range 270–315 cs observed (JCCPAP). Positively correlated with RR (r=+0.25), negatively with pressure (r=−0.32). Strongly correlated with `0x076` (r=+0.94); sum `0x076 + 0x07E` ≈ 602 cs, sum `0x074 + 0x07E` ≈ 538–565 cs — both near-constant. **Not** patient Ti/Te. | Low |
| `0x082` | 2 | `uint16 LE` | Constant 1010 in all observed JCCPAP data; function unknown (possibly a pressure setpoint or threshold in hundredths cmH₂O = 10.10 cmH₂O). | Low |
| `0x08A` | 1 | `uint8` | **SpO2**, percent; `0` = unavailable (oximeter not connected or signal lost) | High |
| `0x08C` | 1 | `uint8` | **Pulse Rate**, beats/min; `0` = unavailable | High |
| `0x182` | `100×2` | `int16 LE[]` | **Total machine-output flow** = tidal + unintentional leak + intentional (vent) leak; 100 × int16 LE. Same waveform shape as `0x56E` but with a slowly-varying positive DC offset. Regression (R²=0.84, within-packet diff std ~6 raw units — essentially DC): `0x182 ≈ 0x56E + 1.009×leak_raw(0x52A) + 0.128×pressure_raw(0x76E) + 87.6`. The three terms correspond to the three flow components: (1) tidal flow (`0x56E`); (2) unintentional leak (`0x52A`, coeff ≈1.0, same raw units); (3) intentional vent leak — the pressure-dependent term `0.128×pressure_raw + 87.6` gives ≈22 L/min at 4 cmH₂O and ≈39 L/min at 12 cmH₂O, consistent with typical mask vent specs (20 L/min at 4 cmH₂O, 40 L/min at 12 cmH₂O). Not used — `0x56E` (tidal only, zero-baseline) is preferred. | Low |
| `0x24A` | `100×2` | `int16 LE[]` | **Mask Pressure** waveform, 100 samples at 100 Hz; loaded as `CPAP_MaskPressure`. On BiPAP devices this waveform spans the full IPAP–EPAP swing within each breath cycle (e.g. 902–1075 raw on a 9/10 cmH₂O BiPAP device), providing sub-second resolution of the pressure waveform. This is the source of any higher-resolution pressure appearance in OSCAR graphs — not the 5-sample-per-second scalar blocks. | Medium |
| `0x312` | `50×2` | `uint16 LE[]` | **Unknown waveform**, 50 samples; large DC offset (~15 000 raw); oscillates in phase with mask pressure (median per-packet corr 0.81 vs MaskPressure, 0.96 vs Flow); always positive; nearly identical to `0x3E4`. Likely a second pressure-like or flow-derived signal at 50 Hz. Not loaded. | Low |
| `0x380` | `50×2` | `uint16 LE[]` | **Pressure Wave** waveform, 50 samples at 50 Hz; loaded as `BMC_PressureWave`. Like `0x24A`, shows full IPAP–EPAP swing on BiPAP devices (e.g. 887–1022 raw on a 9/10 cmH₂O device). | Medium |
| `0x3E4` | `50×2` | `uint16 LE[]` | **Raw differential-pressure (flow sensor) ADC output**, 50 samples; uint16 with DC bias ~16 000 (sensor mid-rail); AC amplitude scales with flow (r=0.985 per-packet vs flow, r=0.157 vs PressureWave); DC offset drifts slowly over the night tracking sleep/pressure state. Packet-to-packet correlation with `0x312` is 0.979 (slightly lower than vs flow), with per-packet differences std=319 and max=5079 — two tap points or calibration paths for the same sensor. **Not** a flow-abnormality block: investigated as a possible analogue of the legacy BMC loader's flow-abnormality block (which immediately follows the Pressure Wave in that format — same relative offset relationship), but confirmed to be a flow waveform, not event flags. Not loaded. | Low |
| `0x52A` | 2 | `uint16 LE` | **Unintentional leak rate** (firmware SC.72 / user version G3-2.11.x only); `raw × 0.16 = L/min`. Confirmed for JCCPAP (Luna G3X, config `110A40113`). **Zero (unused) in firmware SC.74+** (e.g. Kavolodin G3 A20, config `880A40383`). OSCAR selects 0x52A or 0x568 automatically by sampling the first 200 packets of each day. See also `0x568`. | Medium |
| `0x52C` | 2 | `uint16 LE` | **Tidal Volume**, raw units; range 0–2559 observed | Low |
| `0x52E` | 2 | `uint16 LE` | **Minute Ventilation**, raw units | Low |
| `0x530` | 2 | `uint16 LE` | **Respiratory Rate**, breaths/min; range 0–29 bpm observed, mean ~15 | Medium |
| `0x538` | 2 | `uint16 LE` | Oscillating counter; range [0, 20 000] in steps of ~400; period ≈ 10–12 s; not correlated with pressure, leak, or flow. Likely a machine-internal timing or state counter. | Low |
| `0x53E` | 2 | `uint16 LE` | Six discrete values {5400, 5520, 5640, 5760, 5880, 6000} in steps of 120; slowly varying. Possibly a pressure setpoint or algorithm scalar in hundredths cmH₂O (range 54–60 cmH₂O is above normal therapy range, suggesting an internal machine parameter rather than a displayed value). | Low |
| `0x544` | 2 | `uint16 LE` | Breath-phase indicator; four values {0, 3, 1020, 1023} only; non-zero ≈ 57% of packets. Likely encodes inspiration/expiration phase or a related breath-cycle state machine. | Low |
| `0x546` | 2 | `uint16 LE` | Unknown; range 512–1023, mostly near max with a dip early in session | Low |
| `0x54A` | 2 | `uint16 LE` | Constant 1 in all observed data; likely fixed marker | Low |
| `0x54E` | 2 | `uint16 LE` | Mostly 1, rare spikes; sparse event flag, function unknown | Low |
| `0x550` | `5×2` | `int16 LE[]` | 5 samples at 5 Hz (200ms intervals) within the packet; signed range [−725, 519], mean ≈ −64, 99% nonzero. Tracks slowly-varying algorithm state — not IPAP/EPAP pressure (confirmed on BiPAP: slots remain near −64, not near 900/1000). Function unknown. | Low |
| `0x55A` | `2×2` | `uint16 LE[]` | 2 intra-packet samples; unsigned range [0, 43947], mean ≈ 3525, ≈78% nonzero. No significant correlation with flow or pressure. Function unknown. | Low |
| `0x55E` | `3×2` | `uint16 LE[]` | 3 intra-packet samples (0x55E, 0x560, 0x562); unsigned range [0, 65535], mean ≈ 7510, ≈78% nonzero. No significant correlation with flow or pressure. Function unknown. | Low |
| `0x564` | `2×2` | `uint16 LE[]` | 2 intra-packet samples (0x564, 0x566); unsigned range [0, 65535], mean ≈ 25657, 100% nonzero. Function unknown. | Low |
| `0x568` | 2 | `uint16 LE` | **Total mask leak rate** (present in all observed firmware versions); `raw × 0.16 = L/min`. Baseline ~15–17 L/min (intentional vent), with spikes to 100–350 L/min on mask repositioning / removal. **Primary leak source for firmware SC.74+** (Kavolodin/Patient2, config `880A40383`) where `0x52A` is always zero. In firmware SC.72 (JCCPAP), both fields are populated but uncorrelated (r ≈ 0.05), indicating they measure different quantities (0x52A = unintentional leak; 0x568 = total). OSCAR selects this field automatically when 0x52A is found to be unpopulated. The earlier hypothesis of an "inverse-RR" signal was incorrect. | Medium |
| `0x56A` | 2 | `uint16 LE` | **Hypopnea/flow-limitation duration counter** — part of a 4-stage shift register at bytes `0x56A`–`0x56D` (newest→oldest: `0x56D`, `0x56C`, `0x56B`, `0x56A`); counter increments each second while a reduced-ventilation event is in progress; value 1 = first second, 2 = second second, etc. (max observed: 8). ~150 events/night; median duration 4 s, max 55 s. During events: **tidal volume drops ~33%** (median 539→321 raw), RR drops from 13.4→11.6 bpm, APAP pressure rises (44% of events trigger subsequent boost). Flow waveform shape and HF content are **identical** to normal — the machine detects reduced ventilation (not waveform shape change). The lag byte (`0x56A`, i.e. `pkt[0x56A]`) is 1 packet behind the lead byte (`0x56B`, the high byte of this uint16). See also `0x56C`. Not yet loaded. | Low |
| `0x56C` | 2 | `uint16 LE` | Continuation of the 4-stage shift register at `0x56A`; the `0x56C` word (bytes `0x56C`=lo, `0x56D`=hi) leads the `0x56A` word by 1 packet. `0x56D` (hi byte) is the most current byte — it updates first on increment/reset. Functionally carries the same counter as `0x56A`; not needed in addition to `0x56A` once the correct byte is chosen. | Low |
| `0x56E` | `100×2` | `int16 LE[]` | **Flow Rate** waveform, 100 samples; correct shape and zero baseline | High |
| `0x674` | `5×2` | `uint16 LE[]` | Unknown pressure-range scalar; 5 samples at 5 Hz within the packet; slowly varying (400–844 observed); **not** IPAP or EPAP (on BiPAP device: value ≈ 500 while EPAP ≈ 900, IPAP ≈ 1000; the offset ≈ 404 raw is near-constant). Not tracking PT76E step changes. Possibly actual measured mask pressure vs commanded pressure, or a different physical quantity. Weakly correlated with PT76E (r=0.21). Not loaded. | Low |
| `0x680` | `5×2` | `uint16 LE[]` | Same signal as `0x68A` but with `0xFFFF` sentinel when boost is zero (instead of 0); 5 samples at 5 Hz. Non-FFFF values are identical to `0x68A`. Not loaded. | Low |
| `0x68A` | `5×2` | `uint16 LE[]` | **APAP pressure boost** (CPAP/APAP mode); 5 samples at 5 Hz within the packet; value = `PT76E − baseline_pressure`, hundredths cmH₂O. Zero when machine runs at minimum; nonzero when APAP algorithm has raised pressure. Boost doubles on successive events and decays between them. Active ~48% of packets on CPAP. On BiPAP device (B33BF....): value ≈ 200 when PS = 100 hundredths — function may differ from CPAP mode. The 5-Hz structure is confirmed: at a boost event, the new value first appears in the latest time slot(s), with the number of updated slots encoding the sub-second timing of the transition. Not loaded. | Low |
| `0x694` | `5×2` | `uint16 LE[]` | Leading indicator of the APAP pressure boost; 5 samples at 5 Hz; transitions 1–2 packets ahead of `0x68A` (boost appears in `0x694` before the pressure step in `0x76E`). Resets to zero at the moment `0x68A` picks up the value. Likely the machine's internal algorithm signal that triggers the boost, distinct from the confirmed boost at `0x68A`. Will correlate with apnea events (boosts are responses to apneas), but this is two steps removed from `.evt` records and adds no new clinical information — not worth investigating. Not loaded. | Low |
| `0x6A0` | `50×2` | `uint16 LE[]` | **Expiratory flow derivative**, 50 samples; always even (real = raw/2); peaks during expiration, near-zero during inspiration; exponential distribution (mode raw=4); related to but not a simple rescaling of the main flow at `0x56E`. Not loaded. | Low |
| `0x768` | 2 | `uint16 LE` | Mostly 1, rare spikes; pattern mirrors `0x54E` | Low |
| `0x76A` | 2 | `uint16 LE` | Constant 1 in all observed data; likely fixed marker | Low |
| `0x76C` | 2 | `uint16 LE` | **EPAP Pressure**, hundredths cmH2O; slowly varying (400–1247 observed); matches BMC "Pressure Trend" display | Medium |
| `0x76E` | 2 | `uint16 LE` | **IPAP Pressure**, hundredths cmH2O; identical to `0x76C` in CPAP mode; may differ in BiPAP/APAP mode. In APAP mode = `baseline + APAP_boost` (see `0x68A`). | Medium |
| `0x77A` | 2 | `uint16 LE` | Nearly constant; values 258–259 most of the time (0 rare, at session boundaries); function unknown | Low |
| `0x77C` | 2 | `uint16 LE` | Constant 1 in all observed data; likely fixed marker | Low |
| `0x77E` | 2 | `uint16 LE` | Constant 261 in all observed data; likely fixed parameter | Low |
| `0x788` | 2 | `uint16 LE` | Periodic machine-status field; zero 98.8% of packets; when nonzero, values 1–4 (distribution ~1:1:low:low); ~400 hits/night; median inter-hit interval ≈50 s (near-constant timer). Flow peak, RespRate, pressure, and leak are statistically identical when active vs inactive — **not** a patient event marker. Likely an algorithm-mode or signal-quality flag updated on a ~50 s cycle. | Low |
| `0x78A` | 2 | `uint16 LE` | Slowly varying integer; range and mode vary by device (36–40 mode 37 on one device, 38–41 mode 38 on JCCPAP). Weakly negatively correlated with pressure (r=−0.27). **Device-specific**: on the original device `0x78A + 0x78E = 0x792` (71) exactly; on JCCPAP `0x78A + 0x78E ≠ 0x792` (e.g. 38+35=73 ≠ 63). Not patient Ti/Te — I:E ratio `0x78A / 0x78E ≈ 1.09` (not ≈2.0 as expected for CPAP). Function unconfirmed; likely an algorithm timing or triggering parameter. | Low |
| `0x78C` | 2 | `uint16 LE` | Constant 72 in all observed data; likely fixed parameter | Low |
| `0x78E` | 2 | `uint16 LE` | Slowly varying integer; range and mode vary by device (33–37 mode 33 on one device, 35–37 mode 35 on JCCPAP). Complement of `0x78A` on the original device only; on JCCPAP the relationship `0x78A + 0x78E = 0x792` does not hold. | Low |
| `0x790` | 2 | `uint16 LE` | Nearly constant; values 1–3 (mode 2, ≥99% nonzero); rare zero at session boundaries; small mode/flag | Low |
| `0x792` | 2 | `uint16 LE` | **Device-specific constant or session-phase field**: 71 on the original device (where it equals `0x78A + 0x78E`); on JCCPAP takes two values — 63 (first ~77% of session) then 66 (final ~23%), with a single transition mid-session. Neither value equals `0x78A + 0x78E` for JCCPAP. Possibly encodes therapy mode or algorithm state. | Low |
| `0x794` | 2 | `uint16 LE` | **Hourly countdown timer**: decrements by 5 every 3600 seconds (1 hour), runs within a fixed range (195–237 observed on JCCPAP, 183–223 on the original device), then resets. Run lengths are nearly exactly 3600 packets (= 1 h at 1 Hz) between decrements. Possibly a calibration, firmware, or algorithm epoch counter. | Low |

| `0x070` | 2 | `uint16 LE` | Always 0 on JCCPAP. | Low |
| `0x072` | 2 | `uint16 LE` | Slowly varying; range 24340–24666, ~40 discrete values in steps of 8 (one occasional step of 9). No obvious correlation with clinical channels. Not investigated further. | Low |

Bytes `0x00C–0x06F` and other unlisted ranges are tracked in the diagnostics CSV but have no confirmed semantic meaning.

**5-sample-per-second intra-packet blocks:** Several scalar fields appear as groups of 5 consecutive `uint16` words (e.g. `0x674`, `0x680`, `0x68A`, `0x694`). These are **not** temporal shift registers carrying the value from previous packets. They are **5 samples taken at 200ms intervals within the current 1-second packet** (5 Hz). Because these fields track slowly-varying signals, all 5 slots are typically identical within any given packet. At transition moments the sub-second timing of the change is visible: the number of trailing slots that have already adopted the new value indicates when within the second the transition occurred (e.g. `[old, old, old, old, new]` = transition in the last 200ms; `[new, new, new, new, new]` = already fully transitioned at packet start). This was confirmed by examining APAP boost events where the new value propagates to all 5 slots within one subsequent packet — impossible for a one-slot-per-packet shift register. These blocks do **not** carry IPAP/EPAP pressure at 5 Hz; that data is in the 50-sample and 100-sample waveform arrays at `0x380` and `0x24A`. Test file used for BiPAP confirmation: `Notes/G3X/G3/SD card data/B33BF.....000`.

**Waveform block survey (completed 2026-03-20):** All 50- and 100-word candidate regions have been examined:

| Range | Finding |
|-------|---------|
| `0x00C–0x073` | Mixed region: offsets `0x00C–0x071` are mostly **repeated-byte** uint16s (both bytes equal, e.g. `0x0505`, `0x1414`); byte values are *mostly* multiples of 5 but not exclusively (17 and 22 also appear). None of these words correlate significantly with pressure or RR (|r| < 0.11). `0x070` is all zero. `0x072` is a **separate field** that breaks the pattern: oscillating uint16 in range [24 348, 24 633] (24 unique values), bidirectional steps of ±8–212 per packet, 76% of packets change, mean ≈ 24 509; likely an internal accumulator or algorithm state at the boundary of this parameter block. |
| `0x08E–0x181` | Structured sub-blocks, not a waveform array: **`0x08E`** = zero; **`0x090–0x0A2`** = 10 sentinel words alternating between 23 750 (`0x5CC6`) and 24 000 (`0x5DC0`), except `0x096` which has rare larger values (24 250–27 250, <1.4% of packets); **`0x0A4–0x0B6`** = 10 flow-correlated words (range 32–616, mean 146); each word independently tracks instantaneous per-second flow (≈84 during expiration, ≈228 during inspiration); lag-1 autocorr ≈ −0.17 (oscillates with breathing); correlation with mean flow peaks at `0x0AA`–`0x0AC` (r = 0.86–0.87). This appears to be a 10-element rolling window of per-second flow (or related quantity). **`0x0B8–0x0180`** = all zeros. |
| `0x182–0x249` | Total machine-output flow (tidal + leak): 100 × int16, correct flow shape with slowly-varying positive DC offset = leak rate. See main table entry and §3. Not used. |
| `0x312–0x37F` | Unknown 50-sample waveform — see table entry above |
| `0x3E4–0x447` | Unknown 50-sample waveform — see table entry above |
| `0x448–0x50F` | All zeros |
| `0x510–0x527` | **Breathing-state shift register**: 12 × uint16 LE words; each word is `0xFFFF` when the corresponding second was in the inspiratory phase and `0xAAAA` when expiratory/idle. Acts as a 12-second rolling history of the breathing state. The count of `0xFFFF` words is very strongly correlated with mean flow (r = 0.904). This is the "region at 0x510" referenced in `bmcG3xDataParsing.cpp` (flow abnormality); however, the data actually encodes the inspiration/expiration phase history rather than a traditional flow-abnormality flag. Currently zeroed in the G3X loader pending further validation. |
| `0x528` | Partial/boundary byte of the shift register at `0x510`; values ≈ 0xAA or 0xFF (fragments). |
| `0x529` | Zero |
| `0x636–0x693` | Zero block `0x636–0x673` (all zeros, immediately after the flow waveform), then the known 5-word shift register groups at `0x674–0x67C` (pressure scalar), `0x67E` (zero), `0x680–0x688` (APAP boost with 0xFFFF sentinel), `0x68A–0x692` (APAP boost). See main table. |
| `0x694–0x69C` | 5 intra-packet samples at 5 Hz: APAP boost leading indicator (see scalar table) |
| `0x69D–0x69F` | Zeros |
| `0x6A0–0x702` | 50 × uint16 LE; values always even (LSB=0, real value = raw/2); 99.9% nonzero; mode at raw=4 (real=2), exponential distribution; peaks strongly during expiration (mean real 11.4) vs inspiration (6.1); near-zero during apnea. Related to expiratory flow but not a simple rescaling of 0x56E — scatter vs |flow50| is wide (IQR 20–41×). Likely a processed/compressed expiratory flow derivative. Not loaded (lower resolution than 0x56E which covers both phases). |
| `0x704–0x7FE` | Long zero region (`0x704–0x766`), followed by the known scalar fields (`0x768` onward — see main table), with all offsets from `0x796` to end of packet being zero. Not a waveform array. |

---

## 3) Waveform Regions and Resampling

The parser reads three waveform arrays from each packet and resamples them all to 50 output samples:

| Region | Offset | Source samples | Output samples | Raw type | Clamp | Exported |
|--------|--------|---------------:|---------------:|----------|-------|---------|
| Flow Rate | `0x56E` | 100 | 50 | `int16 LE` | ±2000 | Yes |
| Pressure Wave | `0x380` | 50 | 50 | `uint16 LE` | ≤4000 | **No** (`ExportPressureWaveform() = false`) |
| Mask Pressure | `0x24A` | 100 | 50 | `int16 LE` | [0, 4000] | Yes |

Resampling is direct indexed decimation (nearest sample, no anti-alias filter).

`0x182` (100 samples, `int16 LE`) is the total machine-output flow = tidal + unintentional leak + intentional vent leak. Its DC offset is the sum of the unintentional leak (`0x52A`) and a pressure-dependent intentional vent leak; see the offset table entry for the full regression. It is **not used** — `0x56E` (zero-baseline tidal flow) is preferred.

---

## 4) Scalar Channels Decoded from Each Packet

The following single-value fields are read from every packet and stored as `EVL_Event` EventLists:

| OSCAR channel | Offset | Decode | Scale / notes |
|---------------|--------|--------|---------------|
| `CPAP_Pressure` | `0x76E` | See §6 | Hundredths cmH2O stored directly in `Raw.IPAP`; `PressureChannelGain()` = 0.01 → 0.01 cmH2O resolution |
| `CPAP_IPAP` | `0x76E` | See §6 | Same as `CPAP_Pressure` |
| `CPAP_EPAP` | `0x76C` | See §6 | Identical to IPAP in CPAP mode |
| `CPAP_Leak` | `0x52A` or `0x568` | `uint16 LE` × 1.6 = tenths L/min; gain 0.1 → L/min | Offset selected at runtime by Phase 3.5 probe: 0x52A for fw SC.72, 0x568 for fw SC.74+. See §5. |
| `CPAP_TidalVolume` | `0x52C` | `uint16 LE`, gain 1.0 | Scale TBD |
| `CPAP_MinuteVent` | `0x52E` | `uint16 LE` × 0.1 | Scale TBD |
| `CPAP_RespRate` | `0x530` | `uint16 LE`, gain 1.0 | Breaths/min |
| `OXI_SPO2` | `0x08A` | `uint8`, gain 1.0 | %; stored 0 = no data (skipped) |
| `OXI_Pulse` | `0x08C` | `uint8`, gain 1.0 | BPM; stored 0 = no data (skipped) |
| `CPAP_Ti` | `0x074` | `uint16 LE` × 0.001 = seconds | **Not exported** (`ExportTimingChannels() = false`). Identification contradicted — see §9 open question 10. Sum 0x074+0x07E near-constant (~565 cs); not patient inspiratory time. |
| `CPAP_Te` | `0x07E` | `uint16 LE` × 0.001 = seconds | **Not exported** (`ExportTimingChannels() = false`). Identification contradicted — not patient expiratory time. See §9 open question 10. |

**Note:** The I:E ratio computation (cycle = `0x074 + 0x07E`, `insp_permille = 1000 × raw_0x074 / cycle`) in the parser code is based on an identification that has since been contradicted. The sum of these two fields is near-constant and does not track the breath cycle time derived from the RR field. The fields `0x076` and `0x07E` are a more tightly correlated pair (r=+0.94) and may be more relevant to whatever algorithm timing these encode.

---

## 5) Leak Field and Scale

### Field selection (firmware-dependent)

Two firmware builds have been observed:

| Config code (IDX 0x48) | SC firmware (IDX 0x0345) | User version | Leak field | Signal |
|------------------------|--------------------------|--------------|------------|--------|
| `110A40113` (JCCPAP/Luna G3X) | `G3-2.SC.72.01` | G3-2.11.x | **`0x52A`** | Unintentional (mask-fit) leak |
| `880A40383` (Kavolodin G3 A20) | `G3-2.SC.74.03` | G3-2.12.x | **`0x568`** | Total mask leak (incl. intentional vent) |

In firmware SC.74+, `0x52A` is always zero and `0x568` carries a continuous total-mask-leak signal (baseline ~15–17 L/min = intentional vent, with spikes to 100–350 L/min on mask repositioning). In firmware SC.72, both `0x52A` and `0x568` are populated but are uncorrelated (r ≈ 0.05); `0x52A` is used as the confirmed unintentional leak source.

**OSCAR selection rule** (Phase 3.5 in `ReadDateSession()`): the firmware version string is checked first — `G3-2.11.x` / `SC.72` → `0x52A`; any other `G3-2.` prefix → `0x568`. If the firmware version is unavailable, the first 200 packets of the day are sampled: if fewer than 10% have non-zero `0x52A`, the alternate field `0x568` is used for the entire day.

### Scale

Default: `raw × 0.16 = L/min` (applies to both `0x52A` and `0x568`; equivalently `raw × 1.6` tenths of L/min).

The scale factor can be overridden via the environment variable `OSCAR_BMC_G3X_LEAK_SCALE`:
- Set to the L/min-per-raw value (e.g. `0.16`).
- The code multiplies by 10 internally to produce tenths of L/min for storage.

In OSCAR the `CPAP_Leak` EventList uses gain `0.1`, so the stored raw value is in tenths of L/min (gain × stored = displayed L/min).

The `.evt` file's `0x0C` records were examined as an alternative leak source and were found **not** to match the BMC leak display.

---

## 6) Pressure Source

**Current default (`kG3xUsePressureTrendForPressureChannel = true`):**
IPAP and EPAP are sourced from the per-packet pressure-trend fields in the waveform packet:
- `0x76C` → EPAP (hundredths cmH2O) → `Raw.EPAP`
- `0x76E` → IPAP (hundredths cmH2O; identical to EPAP in CPAP mode) → `Raw.IPAP`

The raw hundredths value is stored directly (no quantisation). `PressureChannelGain()` returns 0.01 for the G3X loader, giving 0.01 cmH2O display resolution. The `BMC_PressureTrend`/`BMC_IPAPTrend` channels that previously duplicated this data have been removed.

**EVT 0x42 fallback:**
The `.evt` file's `0x42` therapy-pressure snapshot records are still parsed and merged into the waveform state machine by timestamp. When the pressure-trend override is active, these serve only as a seed — they prime the initial pressure state before the first waveform packet arrives and provide a fallback if the trend field is zero (e.g. machine-off packets).

Set `kG3xUsePressureTrendForPressureChannel = false` to revert to EVT-only pressure (step-wise updates from `0x42` snapshots).

**Pressure seed at `0x00A`:**
The per-packet pressure seed field is read but used only to prime the state if neither EVT `0x42` records nor pressure-trend values are available yet.

---

## 7) BMC Paired uint16 Pattern

BMC firmware writes the same `uint16` value into two consecutive `uint16` slots at `0x76C`/`0x76E`. In CPAP mode the two values are identical (both represent a single pressure). In BiPAP or APAP mode they may differ (EPAP at `0x76C`, IPAP at `0x76E`). When both halves are identical across all packets in a session, a single-pressure CPAP mode is inferred.

---

## 8) What `.00x` Provides vs What Comes from Elsewhere

**Sourced directly from `.00x` waveform packets:**
- Timestamp
- Pressure seed (`0x00A`)
- Flow Rate waveform (`0x56E`)
- Mask Pressure waveform (`0x24A`)
- Pressure Wave waveform (`0x380`) — stored in `BmcWaveformPacket.PressureWave` but not exported to OSCAR channels (`ExportPressureWaveform() = false`)
- Leak (`0x52A` fw SC.72, or `0x568` fw SC.74+; selected per §5)
- Tidal Volume (`0x52C`)
- Minute Ventilation (`0x52E`)
- Respiratory Rate (`0x530`)
- SpO2 (`0x08A`)
- Pulse Rate (`0x08C`)
- EPAP pressure trend (`0x76C`) — **drives `CPAP_EPAP`** at 0.01 cmH2O resolution
- IPAP pressure trend (`0x76E`) — **drives `CPAP_Pressure` and `CPAP_IPAP`** at 0.01 cmH2O resolution
- Unknown timing parameters (`0x074`, `0x07E`) — identification contradicted (not Ti/Te); **not exported** (`ExportTimingChannels() = false`); see §9 open question 10

**Sourced from `.evt` and merged by timestamp (fallback only):**
- IPAP / EPAP / Pressure (from message type `0x42` pressure snapshots) — overridden by PressureTrend when `kG3xUsePressureTrendForPressureChannel = true`

**Sourced from `.evt` as discrete events:**
- Respiratory events: OSA, CSA, UA, Hypopnea (message types `0x02`–`0x09`)
- Periodic Breathing episodes (message type `0x44`)

---

## 9) Known Open Questions

1. Confirmed scale factors for Tidal Volume (`0x52C`) and Minute Ventilation (`0x52E`) — units/gain not yet validated against BMC display.
2. Semantic meaning of `0x546` (range 512–1023, bounded counter or bitfield?).
3. Whether any additional paired `uint16` instances exist beyond `0x76C`/`0x76E`.
4. Whether `0x76C`/`0x76E` represent titrated target pressure, smoothed actual pressure, or something else.
5. Whether the leak scale of 0.16 L/min per raw unit holds across all firmware versions and device models.
6. Function of the sparse flag fields at `0x54E` and `0x768`.
7. Identity of `0x674`–`0x67C` — pressure-range scalar (400–822), slowly varying, not tracking PT76E step changes; possibly actual mask pressure vs commanded pressure.
8. Identity of `0x78A`/`0x78E` — slowly varying integers whose relationship to `0x792` is **device-specific**: on the original device they sum to 71 = `0x792`; on JCCPAP their sum (≈73) does not equal `0x792` (63). I:E ratio `0x78A / 0x78E ≈ 1.09` (not ≈2.0 typical for CPAP), ruling out patient Ti/Te. Likely algorithm timing or triggering parameters; function unconfirmed.
9. ~~Identity of `0x56A`/`0x56C`~~ — **RESOLVED**: see offset table. Strong evidence for hypopnea/flow-limitation counter: tidal volume drops ~33% (median 539→321 raw), RR drops 13.4→11.6 bpm, APAP boost rises after 44% of events. Best candidate: the machine's internal reduced-ventilation/hypopnea detection flag. Whether this matches the `.evt` hypopnea records exactly still needs direct comparison. Whether to expose as a channel in OSCAR is TBD.

10. Semantic meaning of `0x074`, `0x076`, `0x07C`, `0x07E` — confirmed **not** patient Ti/Te. Key observations:
    - `0x076` and `0x07E` are strongly correlated (r=+0.94), both positively correlate with RR (r≈+0.20–0.25) and negatively with pressure (r≈−0.32–0.40); sum ≈ 602 cs.
    - `0x074` is uncorrelated with `0x076`/`0x07E`; sum `0x074 + 0x07E` ≈ 538–565 cs (near-constant, device-dependent).
    - `0x07C` correlates with `0x07E` (r=+0.83) but has a much wider range; sum `0x076 + 0x07C` ≈ 753 cs.
    - `0x078` is a 6-level quantised version of `0x076`.
    - These are likely machine-internal algorithm timing parameters (trigger, cycle, pressure control) rather than patient respiratory timing.

---

## 10) Channels Searched for but Not Found in `.00x`

The following clinically relevant channels were specifically sought in the `.00x` waveform packets and are believed to be **absent** from this format:

| Channel | Search result | Expected location |
|---------|---------------|-------------------|
| **Snoring** | No `.00x` candidate found. `0x056A` was ruled out (hypopnea, not snoring). `0x788` is a periodic machine timer. No waveform field shows elevated high-frequency content during plausible snoring periods. | Likely in `.evt` as discrete event records (hypothesised code `0x01`). |
| **Flow limitation** | No `.00x` candidate found. Waveform shape analysis confirmed no per-packet flow-limitation flag: normalized inspiratory flow shape and HF content are statistically identical between flagged and normal packets across all tested offsets. | May be in `.evt` as discrete event records, or not reported by this device. |
| **Ti / Te (inspiration / expiration time)** | `0x074` and `0x07E` are confirmed **not** patient Ti/Te (sum ~565 cs, near-constant regardless of RR, no correlation with RR or tidal volume). `0x78A` and `0x78E` are also confirmed **not** patient Ti/Te: their ratio `0x78A / 0x78E ≈ 1.09` (expected ≈2.0 for typical CPAP I:E), and on JCCPAP their sum (≈73) does not match `0x792` (63). No confirmed direct Ti/Te field found in `.00x` packets. | Ti/Te is not exposed in any confirmed form. May be derivable from the flow waveform at `0x56E` by detecting zero crossings, or may only be available from `.evt` records. |

---

## 11) Practical Validation Guidance

When testing parser changes:
- Compare OSCAR graphs vs BMC PAP-Link screenshots on the same time window.
- Inspect the diagnostics CSV (`g3x-diagnostics/*.csv`) for packet-level raw fields and computed min/max.
- Check debug output for `wave_packets`, `out_of_order_packets`, `leak_scale`.

This note should be updated whenever decode offsets, scales, or channel assignments change.

---

## 12) Associated IDX File (`<serial>.idx`)

One IDX file accompanies the set of `.00x` waveform files for each device.

**File-level structure:**
- Total size is always an exact multiple of 2048 bytes.
- Laid out as N records of 2048 bytes each.
- Record 0: **file header** — starts with `"BMC G/E/P INDEX\0"` at offset 0; device serial string at offset `0x30`. Not a data record.
- Records 1..N-1: one **calendar-day record** per day of use.

**Calendar-day record layout** (offsets relative to record start):

| Offset | Size | Type | Meaning |
|--------|-----:|------|---------|
| `+0x00` | 2 | `uint16 LE` | Record magic `0xAAAA` |
| `+0x02` | 2 | `uint16 LE` | **Calendar-day counter** (same value as waveform packet `0x002` for that day) |
| `+0x04` | 2 | `uint16 LE` | Waveform packet size = `0x0800` = 2048 |
| `+0x06` | 2 | `uint16 LE` | `1` (version or format indicator) |
| `+0x08` | 4 | `byte[4]` | **OSCAR day start date/time**: `year-1900, month, day, 0x0C` (noon = 12:00). Encodes the noon boundary that begins the OSCAR day corresponding to this record. |
| `+0x10` | 4 | `uint32 LE` | Cumulative waveform byte offset before this day (= packet_count_before × 2048) |
| `+0x14` | 4 | `uint32 LE` | Cumulative waveform byte offset after this day |
| `+0x18` | 4 | `uint32 LE` | Waveform bytes written on this day |
| `+0x1C` | 4 | `uint32 LE` | Another accumulating offset (second stream; possibly EVT data) |
| `+0x20` | 4 | `uint32 LE` | Another accumulating offset |
| `+0x24` | 4 | `uint32 LE` | Second-stream per-day amount |
| `+0x2C` | 2 | `uint16 LE` | Third accumulating offset |
| `+0x30` | 2 | `uint16 LE` | Third per-day amount |

Sub-record at `+0x80` (partially decoded):
- `+0x80`: ASCII `'IT'` = `0x4954` (sub-record marker)
- `+0x82`: `0x0001`, `+0x84`: `0x0001`
- `+0x86`: `0xFFFF`
- `+0x88`: same 4-byte date as `+0x08`
- `+0x90`: day counter (same as `+0x02`)
- `+0x94`: cumulative packet count = (cumulative byte offset at `+0x14`) / 2048

**Relationship to waveform packets:** The calendar-day counter at IDX `+0x02` is identical to the calendar-day counter in waveform packet `0x002`, allowing waveform files to be matched to IDX records. The IDX `+0x08` date gives the noon start of the OSCAR day; each waveform packet's `0x004` timestamp gives the actual wall-clock time that packet was recorded.
