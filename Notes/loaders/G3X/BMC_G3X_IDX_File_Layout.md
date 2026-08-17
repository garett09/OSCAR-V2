# BMC G3X `.idx` File Layout

*Based on `bmcG3xDataParsing.cpp` / `bmcG3xDataParsing.h`.*
*Compare with `BMC_IDX_File_Layout.md` for legacy BMC differences.*

---

## File Detection

The parser distinguishes G3X from legacy BMC by reading the first 32 bytes of the `.idx` file.
G3X files begin with the ASCII string: `"BMC G/E/P INDEX"` (15 characters, null-terminated).
Legacy BMC files begin with `0xAAAA`.

---

## File-Level Structure

| Region | Offset | Size | Description |
|--------|--------|------|-------------|
| File magic | 0x000 | 15 B | ASCII `"BMC G/E/P INDEX"` |
| Unknown | 0x00F | 33 B | Not parsed |
| Serial Number | 0x030 | 16 B | ASCII, null-terminated, trimmed |
| Unknown | 0x040 | 8 B | Not parsed |
| Part/config code | 0x048 | 16 B | ASCII part number, e.g. `"110A40113"`, `"880A40383"`, `"330E40333"` — **not** the product name; used to distinguish firmware families |
| Unknown | 0x058 | 168 B | Not parsed |
| Product name | 0x100 | 16 B | ASCII human-readable model, e.g. `"G3 A20"` or `"G3 B20A"` — **correct field for OSCAR model string** |
| Unknown | 0x110 | 8 B | Not parsed |
| Part code prefix | 0x118 | ~4 B | First 3 chars of the part code (e.g. `"110"`, `"880"`, `"330"`) — redundant with 0x048 |
| Unknown | 0x11C | 553 B | Not parsed |
| SC firmware build | 0x0345 | 20 B | Internal firmware version string, e.g. `"G3-2.SC.72.01"`, `"G3-2.SC.74.03"`, `"G3-2.SC.75.03"`. Fallback only — OSCAR prefers the user-facing version from the `.log` file. |
| Unknown | 0x0359 | ~1191 B | Not parsed |
| Per-day record array | 0x800 | N × 2048 B | One 2048-byte record per recorded day |

---

## Per-Day Record Layout (offsets relative to record start)

Records begin at file offset 0x800 and repeat every 2048 bytes (0x800).
A record is skipped if the first two bytes are not `0xAAAA`, or if the date is invalid.

### Section 1: Record Header (bytes 0x000–0x034)

| Offset | Size | Type | Field | Notes |
|--------|------|------|-------|-------|
| 0x000 | 2 | uint16 LE | Record magic | Must be `0xAAAA` |
| 0x002 | 6 | — | *unknown* | Not parsed |
| 0x008 | 1 | uint8 | Year | Add 1900 (not 2000 — different from legacy BMC) |
| 0x009 | 1 | uint8 | Month | 1–12 |
| 0x00A | 1 | uint8 | Day | 1–31 |
| 0x00B | 5 | — | *unknown* | Not parsed |
| 0x010 | 4 | uint32 LE | WaveStartOffset | Virtual byte offset into waveform file space |
| 0x014 | 4 | uint32 LE | WaveEndOffset | Virtual byte offset (exclusive end) |
| 0x018 | 4 | uint32 LE | WaveLength | Byte length of waveform data; record skipped if 0 |
| 0x01C | 4 | uint32 LE | EventStartOffset | Byte offset into `.evt` file |
| 0x020 | 4 | uint32 LE | EventEndOffset | |
| 0x024 | 4 | uint32 LE | EventLength | |
| 0x028 | 4 | uint32 LE | LogStartOffset | Byte offset into log file |
| 0x02C | 4 | uint32 LE | LogEndOffset | |
| 0x030 | 4 | uint32 LE | LogLength | |
| 0x034 | 76 B | — | *unknown* | Bytes 0x034–0x07F not parsed |

---

### Section 2: IT Sub-record (at record offset + 0x80)

Present only if bytes at [offset + 0x80] and [offset + 0x81] are `'I'` and `'T'`.
Offsets below are relative to the start of the IT sub-record (i.e., record + 0x80).

| IT Offset | Size | Type | Field | Notes |
|-----------|------|------|-------|-------|
| 0x00 | 1 | char | 'I' | Signature |
| 0x01 | 1 | char | 'T' | Signature |
| 0x02 | 18 B | — | *unknown* | Not parsed |
| 0x14 | 4 | uint32 LE | DurationSeconds | Session duration |
| 0x18 | 12 B | — | *unknown* | Not parsed |
| 0x24 | 1 | uint8 | SessionCount | Number of sessions |
| 0x25 | 3 B | — | *unknown* | Not parsed |
| 0x28 | 2 | uint16 LE | PressureMinHundredths | cmH2O × 100; `0xFFFF` = absent |
| 0x2A | 2 | uint16 LE | PressureMaxHundredths | `0xFFFF` = absent |
| 0x2C | 2 | uint16 LE | PressureP95Hundredths | `0xFFFF` = absent |
| 0x2E | 2 | — | *unknown* | Not parsed |
| 0x30 | 2 | uint16 LE | PressureEPAPHundredths | `0xFFFF` = absent |
| 0x32 | 138 B | — | *unknown* | Bytes 0x32–0xBB not parsed |
| 0xBC | 2 | uint16 LE | AhiX100 | AHI × 100; `0xFFFF` = absent |
| 0xBE | 2 | uint16 LE | AiX100 | AI × 100 |
| 0xC0 | 2 | uint16 LE | HiX100 | HI × 100 |
| 0xC2 | 2 | uint16 LE | OaiX100 | OAI × 100 |
| 0xC4 | 2 | uint16 LE | CaiX100 | CAI × 100 |
| 0xC6 | 2 | uint16 LE | ReraIndexX100 | RERA index × 100 |
| 0xC8 | 2 | uint16 LE | EventTotalCount | |
| 0xCA | 2 | uint16 LE | EventObstructiveCount | |
| 0xCC | 2 | uint16 LE | EventCentralCount | |
| 0xCE | 2 | uint16 LE | EventHypopneaCount | |
| 0xD0 | 2 | uint16 LE | EventReraCount | |
| 0xD2 | 2 | int16 LE | EventOtherCount | Interpreted as signed |
| 0xD4 | — | — | End of IT sub-record | Total IT sub-record: at least 0xD4 bytes |

> **Caution (2026-08-07): the index fields at IT `0xBC`–`0xD2` do not hold on the E5
> platform.** On a 19-day E5 card, IT `0xBC` ("AHI x100") is 999–1000 on every single
> day and IT `0xBE` ("AI x100") stays within 957–969, neither of which tracks therapy.
> An exhaustive scan of all 2048 bytes of the day record found **no** offset whose value
> matches AHI, AI or HI computed from that day's `.evt` events and session hours
> (AHI ranged 0.95–14.05 across the 19 days, so the dynamic range is ample). The
> documented offsets may still be correct for the G3 devices they were derived from;
> they are not correct for E5.
>
> Nothing user-facing depends on this: `ItAhiX100` and its siblings are parsed in
> `bmcG3xDataParsing.cpp` but only ever printed inside a `BMCDEBUG` block. OSCAR
> derives the indices it displays from the imported events, not from these fields.

---

### I/E Ratio distribution summary at record `0x140`–`0x147`

**Confirmed 2026-08-08 against a PAP-Link readout.** Reported by a contributor and
verified exactly: for one night PAP-Link displayed (Avg, Med, P95, Max) =
(45.3, 42.7, 87.0, 94.4) and that day's record held 453, 427, 870, 944 — all four
matching to the tenth.

| Record offset | IT offset | Field | Encoding |
|---------------|-----------|-------|----------|
| `0x140` | IT `0xC0` | I/E ratio, **maximum** | percent x 10 (uint16 LE) |
| `0x142` | IT `0xC2` | I/E ratio, **average** | percent x 10 |
| `0x144` | IT `0xC4` | I/E ratio, **95th percentile** | percent x 10 |
| `0x146` | IT `0xC6` | I/E ratio, **median** | percent x 10 |

The stored quantity is I/E as a **percentage**, that is `Ti/Te x 100`, not
`Ti/(Ti+Te)`. An average of 45.3% is the ratio 0.453, which PAP-Link's 1:X form renders
as 1:2.21. Convert with `X = 100 / percent`.

Across the 19-day reference card the averages ran 32.2%–50.4% (1:3.11 to 1:1.98) and
`median <= P95 <= max` held on every day. One day's maximum exceeded 100% (106.6%,
i.e. 1:0.94) — a single breath with inspiration longer than expiration, not a decode
error.

Note the order in the record is max, average, P95, median — **not** the order PAP-Link
displays them in.

> This supersedes an earlier reading of these bytes as part of an event-index block
> (AHI/AI/HI/OAI/CAI/RERA at IT `0xBC`–`0xD2`). See the caution above: that block does
> not hold on E5, and IT `0xC0`–`0xC6` are these four I/E fields.

**Not yet available as an OSCAR channel.** The G3X path produces no I:E, Ti or Te —
`packetIePermille` is hardcoded to 0 in `bmcG3xDataParsing.cpp` because offsets
`0x074`/`0x07E`, once believed to be Ti/Te, were disproved. These four daily figures
could be surfaced as Device Settings rows, but they cannot be charted and will not feed
the Overview trends.

The better use is as an **oracle** for locating the per-sample field in the waveform
packet: the right offset is the one whose per-day average, median, P95 and maximum
reproduce these four values. Two candidate sources have been ruled out that way:

| Candidate | Result |
|---|---|
| Waveform tidal volume (packet `0x52C`) | Daily means overlap but maxima are out by a factor of two. Not it. |
| Ti/Te from tag-paired `.evt` breath markers (§3a of `BMC_G3X_EVT_FORMAT.md`) | Mean absolute error 5.2 pp on average, 4.4 pp on median, 16.6 pp on P95, with per-day maxima of 650%–4700% against a true 62%–107%. Correctly signed and the right order of magnitude, but not the device's own figure — the markers are detection instants, not the flow-derived breath timing the device summarises. |

---

### Section 3: TS Sub-record (at record offset + 0x280)

Present only if bytes at [offset + 0x280] and [offset + 0x281] are `'T'` and `'S'`.
Offsets below are relative to the start of the TS sub-record (i.e., record + 0x280).

| TS Offset | Size | Type | Field | Notes |
|-----------|------|------|-------|-------|
| 0x00 | 1 | char | 'T' | Signature |
| 0x01 | 1 | char | 'S' | Signature |
| 0x02 | 12 B | — | *unknown* | Not parsed |
| 0x0E | 2 | uint16 LE | PressureMinHundredths | cmH2O × 100; `0xFFFF` = absent |
| 0x10 | 2 | uint16 LE | PressureMaxHundredths | `0xFFFF` = absent |
| 0x12 | — | — | End of TS sub-record | Total TS sub-record: at least 0x12 bytes |

---

### Summary of Unknowns in Per-Day Record

| Range | Size | Status |
|-------|------|--------|
| 0x00F–0x02F (file header) | 33 B | Not parsed (before serial number) |
| 0x040–0x047 (file header) | 8 B | Not parsed (between serial and model) |
| 0x058–0x7FF (file header) | 1992 B | Not parsed |
| Record 0x002–0x007 | 6 B | Not parsed |
| Record 0x00B–0x00F | 5 B | Not parsed |
| Record 0x034–0x07F | 76 B | Not parsed (before IT sub-record) |
| IT 0x02–0x13 | 18 B | Not parsed |
| IT 0x18–0x23 | 12 B | Not parsed |
| IT 0x25–0x27 | 3 B | Not parsed |
| IT 0x2E–0x2F | 2 B | Not parsed |
| IT 0x32–0xBB | 138 B | Not parsed |
| Record 0x100–0x27F | ~384 B | Not parsed (between IT and TS) |
| Record 0x292–0x7FF | ~1390 B | Not parsed (after TS sub-record) |

---

## Waveform Files (.000, .001, …)

Waveform data is stored across one or more numbered files. The `.idx` file refers to waveform
data using a *virtual byte offset* that treats all files as a single linear address space:
- `fileIndex = virtualOffset / 64 MiB`
- `fileOffset = virtualOffset % 64 MiB`

Each file is up to **64 MiB** (`kG3xWaveformFileSpan`).
Waveform packets are **2048 bytes** each (`kG3xWaveformPacketSize = 0x800`).

### Waveform Packet Layout (2048 bytes)

See `Notes/G3X/BMC_G3X_00X_FORMAT.md` for the full field map and confidence levels.
Key offsets used by the current parser:

| Offset | Size | Type | Field | Notes |
|--------|------|------|-------|-------|
| 0x000 | 2 | uint8[2] | Magic | Must be `0xAD`, `0xAA` |
| 0x004–0x009 | 6 | uint8[6] | Timestamp | year-1900, month, day, hour, minute, second |
| 0x00A | 2 | uint16 LE | PressureSeed | cmH2O in hundredths; valid range 400–3500; fallback only if no EVT/IDX seed |
| 0x074 | 2 | uint16 LE | Raw0x74 | Confirmed **not** patient Ti — sum 0x074+0x07E near-constant (~565 cs); not exported |
| 0x07E | 2 | uint16 LE | Raw0x7E | Confirmed **not** patient Te; not exported |
| 0x08A | 1 | uint8 | SpO2 | Percent; 0 = unavailable |
| 0x08C | 1 | uint8 | PulseRate | BPM; 0 = unavailable |
| 0x24A | 200 B | int16 LE × 100 | MaskPressure | 100 samples; clamped to [0, 4000]; resampled to 50 output samples |
| 0x380 | 100 B | uint16 LE × 50 | PressureWave | 50 samples; clamped ≤4000; not exported (`ExportPressureWaveform = false`) |
| 0x510 | — | — | FlowAbnormality | Zeroed — pending further validation; not exported |
| 0x52A | 2 | uint16 LE | Leak | Firmware SC.72 / G3-2.11.x only; unintentional leak; raw × 1.6 = tenths L/min |
| 0x52C | 2 | uint16 LE | TidalVolume | Raw; scale factor TBD |
| 0x52E | 2 | uint16 LE | MinuteVentilation | Raw; scale factor TBD |
| 0x530 | 2 | uint16 LE | RespiratoryRate | Breaths/min |
| 0x56E | 200 B | int16 LE × 100 | Flow | 100 samples; clamped ±2000; scaled /10.0; resampled to 50 output samples |
| 0x568 | 2 | uint16 LE | AlternateLeak | Firmware SC.74+ / G3-2.12.x+; total mask leak; raw × 1.6 = tenths L/min |
| 0x76C | 2 | uint16 LE | PressureTrendEPAP | Hundredths cmH2O; drives `CPAP_EPAP` |
| 0x76E | 2 | uint16 LE | PressureTrendIPAP | Hundredths cmH2O; drives `CPAP_Pressure` / `CPAP_IPAP` |

All waveform arrays are resampled to 50 output samples.

**Leak source selection:** Firmware version string is checked first — G3-2.11.x / SC.72 uses
offset 0x52A (unintentional leak); G3-2.12.x+ (any other `G3-2.` prefix) uses 0x568 (total
mask leak). If the firmware version string is unavailable, the first 200 waveform packets are
sampled: if fewer than 10% have non-zero 0x52A, the alternate field 0x568 is used. EVT records
`0x0C` and `0x42` are **not** used as leak sources.

**Timing fields (0x074, 0x07E):** Confirmed **not** patient Ti/Te — their sum is near-constant
(~565 cs) regardless of respiratory rate, with no correlation with RR, tidal volume, or
pressure. `ExportTimingChannels() = false`; Ti, Te, and I:E channels are not exported to OSCAR.

---

## EVT File Records

The `.evt` file contains fixed-size 32-byte (`kG3xEvtRecordSize = 0x20`) records.
Records without magic `0xAE, 0xAA` at bytes 0–1 are skipped.

| Offset | Size | Type | Field | Notes |
|--------|------|------|-------|-------|
| 0x00 | 1 | uint8 | Magic byte 0 | Must be `0xAE` |
| 0x01 | 1 | uint8 | Magic byte 1 | Must be `0xAA` |
| 0x02 | 14 B | — | *unknown* | Not parsed |
| 0x10 | 1 | uint8 | MessageType | See table below |
| 0x11 | 3 B | — | *unknown* | Not parsed |
| 0x14 | 6 B | uint8[6] | Timestamp | year-1900, month, day, hour, minute, second |
| 0x1A | 2 | uint16 LE | Timestamp ms | Sub-second part of the timestamp at 0x14 (0–999); same for every MessageType. Confirmed 2026-08-05 |
| 0x1C | 2 | uint16 LE | Value2 | Meaning depends on MessageType |
| 0x1E | 2 | — | *unknown* | Not parsed |

### EVT Message Types

See `Notes/G3X/BMC_G3X_EVT_FORMAT.md` for full details and confidence levels.

For respiratory events (0x01–0x0A), **value2** is duration in **milliseconds**
(confirmed 2026-03-25). Duration is clamped to 10–180 seconds for all types except 0x09
(PB, unclamped). Timestamp marks the **START**. The field at 0x1A, formerly recorded here
as an unused "value1" counter, is the timestamp's millisecond part (confirmed 2026-08-05;
see `BMC_G3X_EVT_FORMAT.md` §2a).

| Type | Interpretation | value2 / unk1e (0x1E) |
|------|---------------|----------------------|
| 0x01 | UH — Unclassified hypopnea → `CPAP_Hypopnea` | Duration ms |
| 0x02 | UA — Unclassified apnea → `CPAP_Apnea` | Duration ms |
| 0x03 | OSA → `CPAP_Obstructive` | Duration ms |
| 0x04 | CSA → `CPAP_ClearAirway` | Duration ms |
| 0x07 | OH — Obstructive hypopnea → `CPAP_Hypopnea` | Duration ms |
| 0x08 | CH — Central hypopnea → `CPAP_Hypopnea` | Duration ms |
| 0x09 | **PB episode start marker** → `CPAP_PB`; timestamp = START; duration = uint32 at 0x1C–0x1F (low 16 = value2, high 16 = unk1e), ms | — / high 16 bits of uint32 duration |
| 0x0A | RERA → `CPAP_RERA` | Duration ms (~10 s) |
| 0x0B | Unknown (per-breath; not decoded) | Multiples of 20, not duration |
| 0x0C | Per-breath inspiration (timestamps collected; not decoded to channels) | — |
| 0x0D | Per-breath expiration (not decoded) | — |
| 0x0E | **Mild flow limitation** → `CPAP_FLG` grade 1; timestamp = end-of-expiration | Ti in ms (bar width) |
| 0x0F | **Moderate flow limitation** → `CPAP_FLG` grade 2 | Ti in ms (bar width) |
| 0x10 | **Severe flow limitation** → `CPAP_FLG` grade 3 | Ti in ms (bar width) |
| 0x40 | Session start marker (not used — waveform heuristic is more accurate) | — |
| 0x41 | Session end marker (not used) | — |
| 0x42 | Pressure snapshot; value2 = **EPAP** hundredths cmH2O; unk1e = **IPAP** hundredths cmH2O | EPAP / IPAP |
| 0x43 | Unknown (~10-min cadence; not decoded) | — |
| 0x44 | PB timestamp (secondary; used only if 0x09 absent; no duration) | — |

**Leak source:** Sourced from waveform packet offset `0x52A` (firmware SC.72 / G3-2.11.x) or
`0x568` (firmware SC.74+ / G3-2.12.x+). See the waveform packet section above for the
selection rule. EVT records are not used as a leak source.

---

## Key Differences from Legacy BMC Format

| Feature | Legacy BMC | BMC G3X |
|---------|-----------|---------|
| File magic | `0xAAAA` per packet | ASCII `"BMC G/E/P INDEX"` at file start |
| Year encoding | Year - 2000 (uint8) | Year - 1900 (uint8) |
| IDX packet size | 512 bytes | 2048 bytes |
| IDX packets start | File offset 0x800 | File offset 0x800 |
| Waveform packet size | 256 bytes (0x100) | 2048 bytes (0x800) |
| Waveform file addressing | File index via `.NNN` extension, packet index × 0x100 | Virtual linear address space, 64 MiB per file |
| Machine settings in IDX | Yes (offset 0x140–0x165 per packet) | No — mode inferred from pressure min/max |
| Serial number source | USR file at offset 0x2D | IDX file at offset 0x030 |
| Model number source | USR file at offset 0x2296 | IDX file at offset 0x100 (product name, e.g. `"G3 A20"`); 0x048 is part/config code, not product name |
| Session data source | USR file (BmcUsrSession) | IDX records + EVT file |
| Waveform packet magic | `0xAAAD` | `0xAD, 0xAA` |
| Flow samples per packet | 25 | 100 (resampled to 50) |
| Pressure wave samples | 25 | 50 (not resampled; not exported) |
| Mask pressure samples | — | 100 (resampled to 50) |
| Flow abnormality samples | 25 | zeroed (pending validation; not exported) |
| Statistics sub-records | None | IT (indices/totals) and TS (pressure range) |
