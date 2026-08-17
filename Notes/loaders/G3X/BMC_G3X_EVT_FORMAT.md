# BMC G3X `.evt` File Format (Current Best Understanding)

**Status:** Reverse-engineered; not vendor-documented.
**Scope:** `<serial>.evt` event/telemetry stream produced by BMC G3X cards.
**Implementation:** `oscar/SleepLib/loader_plugins/bmcG3xDataParsing.cpp`
**Last updated:** 2026-08-12 (0x0C/0x0D value2 characterised; 0x11-0x13 breath tick tag; 0x02 day sequence; 0x1A milliseconds; 0x43 span durations)

---

## 1) File-Level Structure

- File is a flat stream of fixed-size records.
- Record size: `0x20` (32 bytes).
- No global file header.
- Magic bytes at record offset `0x00`: `AE AA` (little-endian `0xAAAE`).
- Records with non-matching magic are skipped.
- The `.idx` file identifies the day's EVT slice via `EventStartOffset` / `EventEndOffset` (virtual byte offsets into the EVT file).
- If no valid pressure updates (`0x42`) are found in the day slice, the parser performs a fallback full-file scan for `0x42` records near the day's time window.

---

## 2) Record Layout (Offset Map)

Confidence legend:
- **High** — actively used by parser; semantics confirmed.
- **Medium** — structurally clear; semantic meaning partially confirmed.
- **Low** — observed only; not yet used in OSCAR.

| Offset | Size | Type | Meaning | Confidence |
|--------|-----:|------|---------|------------|
| `0x00` | 2 | `uint16 LE` | Record magic `0xAAAE` (bytes `AE AA`) | High |
| `0x02` | 2 | `uint16 LE` | **Day sequence** — a noon-to-noon day index, constant for every record of one therapy day and incremented as the clock passes 12:00 (confirmed 2026-08-07; see §2b) | High |
| `0x04` | 12 | bytes | Filler; always `0xFF` | Medium |
| `0x10` | 1 | `uint8` | **Message type** — primary decode key | High |
| `0x11` | 3 | `uint24 LE` | **Breath tick tag** (`0x11`–`0x13` as one little-endian 24-bit value) — on `0x0C`/`0x0D` it pairs an inspiration to its expiration (confirmed 2026-08-08; see §3a). Meaning differs by message type; on `0x43` byte `0x11` is always zero. | High |
| `0x13` | — | — | *(high byte of the `0x11` field above; not a separate field)* | High |
| `0x14` | 6 | bytes | **Timestamp**: `year-1900, month, day, hour, minute, second` | High |
| `0x1A` | 2 | `uint16 LE` | **Timestamp milliseconds** (0–999) — the sub-second part of the timestamp at `0x14`, so the full record time is 8 bytes spanning `0x14`–`0x1B` (confirmed 2026-08-05; see §2a). Formerly described here as an unused "value1" wrapping counter. | High |
| `0x1C` | 2 | `uint16 LE` | **value2** — for respiratory events: duration low 16 bits, milliseconds (confirmed 2026-03-25). For `0x09` only: low 16 bits of a uint32 spanning `0x1C`–`0x1F` (see §3). | High |
| `0x1E` | 2 | `uint16 LE` | **unk1e** — type-specific: `0x42` = IPAP hundredths cmH2O; `0x09` = high 16 bits of uint32 duration; `0x43` = unknown discrete value; **zero for all other observed types** (confirmed: respiratory events 0x01–0x08, 0x0A always have unk1e=0). | High/Low |

Records with invalid timestamp bytes are skipped.

---

## 2a) Timestamp Milliseconds (offset `0x1A`)

Reported by a contributor and confirmed 2026-08-05 against a full card
(246,260 records, firmware E5 platform). Earlier revisions of this document
called the field "value1" and described it as a wrapping counter that happened
to stay under 1000 — which is what a millisecond field looks like when it is
sampled at a fixed cadence. It is not a counter.

Evidence:

| Test | Result |
|------|--------|
| Range | Never exceeds 999 in any record of any type. Values spread evenly over 0–999 (~24,600 per 100 ms decile) — a 16-bit payload field would not be capped at exactly 999. |
| Ordering | Within a run of records sharing the same whole second, the field is non-decreasing in file order for 99.7% of pairs. Every exception is a transition between two *different* record types, which are not strictly interleaved in the stream. |
| `0x42` cadence | Pressure records are emitted on a 30 s timer. Including this field resolves the interval to 30,031 ms with an interquartile range of **32 ms**. Seconds-only quantises it to a flat 30,000 ms; an unrelated value would have scattered it across a full second. |
| Ti distribution | Inspiration→expiration intervals (`0x0C` → `0x0D`) go from a comb at 0 s / 1 s to a smooth unimodal distribution, median 842 ms, p05 442 ms, p95 1273 ms — a physically plausible inspiratory time. |

The `0x42` cadence test is the decisive one: a field unrelated to time could not
hold a 30-second interval to ±32 ms.

PAP-Link rounds the millisecond part to the nearest second for display
(≥500 rounds up). OSCAR event lists store milliseconds natively, so the loader
carries the value through unrounded rather than reproducing PAP-Link's rounding.
This means OSCAR and PAP-Link can legitimately differ by up to one second in the
displayed time of the same event, with OSCAR the more precise of the two.

Applies to every record type, not only respiratory events. Decoded by
`DecodeG3xEvtTimestamp()` in `bmcG3xDataParsing.cpp`; before 2026-08-05 the
loader truncated to the whole second, placing every event up to 999 ms early.

---

## 2b) Day Sequence (offset `0x02`)

Reported by a contributor and confirmed 2026-08-07 against a card holding 19
consecutive therapy days (firmware E5 platform). Formerly recorded here as a
"sequence-like counter ... appears structural".

The field is a day index. Every record of one therapy day carries the same value,
and the value increments by one as the clock passes 12:00 — the same noon-to-noon
day OSCAR uses. On the reference card the values ran 8 through 26 over 19
consecutive days with no gaps and no repeats, one value per day.

The boundary is sharp to well under a second. Only three records on the whole card
disagree with their day's value, all of them within 700 ms of noon on the one day
the contributor deliberately kept the machine running across the boundary:

```
seq=19  0x0C  11:59:59.330   <- written after the records below
seq=18  0x42  12:00:00.027
seq=18  0x41  12:00:00.027
```

That is the known out-of-order writing at work (records are not strictly
interleaved by timestamp), not a second-level ambiguity in the field itself.

One further exception: a single `0x09` (PB) record carried `seq = 0` where its
neighbours carried 14. The rest of that record decodes normally. One occurrence in
246,260 records; cause unknown, so treat the field as reliable but not guaranteed.

The counter's origin is the card format date, not the calendar — on the reference
card, which was formatted in early July, `seq = 0` extrapolates to a day about a
week before the first recorded night. Do not read the value as a day of month.

Not currently used by the loader, which derives the day from the IDX day record.
It is available as a device-authored cross-check if day assignment is ever in doubt.

---

## 3) Message Types

The table below reflects analysis of two nights of G3X data (file `A3112345678` and `B33BF123456`). Record counts in [brackets] are from night `B33BF123456` (2026-03-16), which is the primary reference night.

| Type | Status | OSCAR mapping | `0x1A` | value2 / unk1e | Notes |
|------|--------|---------------|--------|--------|-------|
| `0x01` | Confirmed | `CPAP_Hypopnea` (UH) | Timestamp ms (§2a) | **Duration, ms** (confirmed 2026-03-25) | **Unclassified hypopnea.** Confirmed 2026-03-25 (Patient 2, B33BF123456). Duration = value2/1000 s, clamped to 10–180 s. 58 records across all Patient 2 nights; absent from JCCPAP. |
| `0x02` | Confirmed | `CPAP_Apnea` (UA) | Timestamp ms (§2a) | **Duration, ms** (confirmed 2026-03-25) | Unclassified apnea. Duration clamped to 10–180 s. |
| `0x03` | Confirmed | `CPAP_Obstructive` (OSA) | Timestamp ms (§2a) | **Duration, ms** (confirmed 2026-03-25) | Obstructive sleep apnea. [30] |
| `0x04` | Confirmed | `CPAP_ClearAirway` (CSA) | Timestamp ms (§2a) | **Duration, ms** (confirmed 2026-03-25) | Central sleep apnea. [20] |
| `0x07` | Confirmed | `CPAP_Hypopnea` (OH) | Timestamp ms (§2a) | **Duration, ms** (confirmed 2026-03-25) | Obstructive hypopnea subtype. PAP-Link displays OH and CH as distinct event types; OSCAR maps both to `CPAP_Hypopnea`. |
| `0x08` | Confirmed | `CPAP_Hypopnea` (CH) | Timestamp ms (§2a) | **Duration, ms** (confirmed 2026-03-25) | Central hypopnea subtype. PAP-Link displays OH and CH as distinct event types; OSCAR maps both to `CPAP_Hypopnea`. |
| `0x09` | Confirmed | `CPAP_PB` | Timestamp ms (§2a) | **uint32 LE duration, ms** — `value2` (0x1C) = low 16 bits, `unk1e` (0x1E) = high 16 bits (confirmed 2026-03-30 via Lijunjun and Kavolodin data) | **Periodic breathing episode start marker.** Timestamp marks the **START** of the episode, consistent with all other respiratory event types. Duration is a uint32 spanning offsets 0x1C–0x1F (reading only the low 16 bits gives wrong durations ~28s/23s; uint32 gives correct ~159s/154s matching PAP-Link on Lijunjun 2026-03-16). Primary PB source in OSCAR. Confirmed on Lijunjun (B20A SC.75) and Kavolodin (A20) data. |
| `0x0A` | Confirmed | `CPAP_RERA` | Timestamp ms (§2a) | **Duration, ms** (confirmed 2026-03-25; ~11 000 ms observed, ≈10 s per PAP-Link) | **Respiratory Effort Related Arousal.** Confirmed 2026-03-24: present in all five independently identified RERA windows from PAP-Link (JCCPAP data, 2026-02-12 through 2026-02-24). Two records at 02:59 and 03:10 on OSCAR day 2/22 match two PAP-Link RERAs on the same night. Duration confirmed ~10 seconds (PAP-Link). ~1–3 records/night on well-treated nights; up to ~200+ on nights with heavy FL. |
| `0x0B` | Unknown | Not decoded | Timestamp ms (§2a) | Multiples of 20, range 400–1060; **not** duration | Frequency varies dramatically night to night (1–1,439/night in JCCPAP; only 3 in G3X-2 on 2026-02-20). On high-FL nights fires once per breath alongside `0x0E` (mild FL), sharing the same value at `0x11`-`0x13` (the breath tick tag, §3a). Not a RERA marker (absent from confirmed RERA windows). value2/1000 = 0.4–1.1 s — sub-second, consistent with per-breath timing (breath period), not event duration. Not observed in B33BF123456 reference file. |
| `0x0C` | Per-breath inspiration | Timestamps collected; not decoded to channels | Timestamp ms (§2a) | **value2 = a per-breath volume-like magnitude** (§3b); `unk1e` always 0 | [8308]; mirrors `0x0D` count. **Not a leak source.** Timestamp collected into `rawInspirationTimestamps` for potential future AASM-based PB detection but not currently used. The value at `0x11`-`0x13` is the breath tick tag that pairs this record with its `0x0D` (§3a). |
| `0x0D` | Per-breath expiration | Not decoded | Timestamp ms (§2a) | **value2 = a per-breath volume-like magnitude** (§3b); `unk1e` always 0 | [8308]; mirrors `0x0C`. Carries the same tick tag as the `0x0C` it closes (§3a). |
| `0x0E` | Confirmed | `CPAP_FLG` (Mild) | Timestamp ms (§2a) | **Inspiration duration (Ti), ms** — multiples of 20 ms; median 1.62 s; decreases with FL severity (confirmed 2026-03-26). **Used** as bar width in OSCAR. | **Mild flow limitation.** Confirmed by PAP-Link alignment on two sessions (G3X-2 2026-02-20: 376 events matching PAP-Link mild pattern; JCCPAP 2025-12-20: 181 events). Timestamp = end-of-expiration trough; bar plots forward by value2 ms to cover the inspiratory peak. |
| `0x0F` | Confirmed | `CPAP_FLG` (Moderate) | Timestamp ms (§2a) | **Ti, ms** — multiples of 20 ms; median 1.42 s. **Used** as bar width. | **Moderate flow limitation.** Confirmed: G3X-2 2026-02-20 yielded 22 events vs PAP-Link 21; JCCPAP 2025-12-20 yielded 11 events. |
| `0x10` | Confirmed | `CPAP_FLG` (Severe) | Timestamp ms (§2a) | **Ti, ms** — multiples of 20 ms; median 1.18 s; wider spread than mild/moderate. **Used** as bar width. | **Severe flow limitation.** Confirmed: G3X-2 2026-02-20 yielded 6 events vs PAP-Link 5; JCCPAP 2025-12-20 yielded 5 events. |
| `0x40` | Confirmed | Not used | — | — | Session start marker. [1] Ref: `2026-03-16T00:27:53`. See §6. |
| `0x41` | Confirmed | Not used | — | — | Session end marker. [1] Ref: `2026-03-16T09:04:22`. See §6. |
| `0x42` | Confirmed | Fallback pressure | — | **value2** = EPAP hundredths cmH2O; **unk1e** (0x1E) = IPAP hundredths cmH2O | [470] Fallback pressure; overridden by PressureTrend (0x76C/0x76E). See §6. Confirmed 2026-03-25: PS = (unk1e − value2) / 100 cmH2O. |
| `0x43` | Partly decoded | Not decoded | Timestamp ms (§2a) | **uint32 LE ms duration spanning `0x1C`–`0x1F`** — `value2` = low 16 bits, `unk1e` = high 16 bits (confirmed 2026-08-07; see §4a) | Span record: the timestamp is the START and the uint32 is the length. Consecutive records tile a session contiguously. The quantity being segmented is still unknown. The earlier "value2 takes ~12 distinct values differing by ~5536" reading was the low word of the uint32 wrapping, not a set of discrete values; the "session-elapsed counter" at `0x1A` was the millisecond field. |
| `0x44` | Confirmed | `CPAP_PB` (secondary) | 0 | 0 | Periodic Breathing marker. Present on SC.74+/SC.75; absent on SC.72. Used only when `0x09` records are absent. See §4. |

**Not observed** in reference night (`B33BF123456`): `0x05`, `0x06`, `0x0B`, `0x0E`, `0x0F`, `0x10`. The flow-limitation types (`0x0E`/`0x0F`/`0x10`) and `0x0B` are absent from that file; PAP-Link shows no flow limitations for that patient — consistent with `0x0B` being related to mild FL activity.

---

## 3a) Breath Tick Tag (offsets `0x11`–`0x13`)

Reported by a contributor and confirmed 2026-08-08 on the reference card. Bytes
`0x11`, `0x12` and `0x13` form one little-endian 24-bit value. On the per-breath
records (`0x0C` inspiration, `0x0D` expiration) it is a device tick counter used to
**pair an inspiration with its expiration**.

| Test | Result |
|------|--------|
| Pairing | Of 116,672 adjacent `0x0C` -> `0x0D` pairs with valid tags, **116,672 share the same tag — 100.000%**. Including the two exception classes below the figure is 96.8%. |
| Tick period | Regressing tag against wall clock over 22 monotone runs gives 0.25600–0.25618 s per tick, median **0.25602 s** (3.9060 ticks/s). The contributor's "0.256 s, not exact" is right; the residual is device clock drift. |
| Continuity | The counter free-runs during therapy and resets 17 times across the card. It is **not** a wall-clock reference: between the starts of two consecutive nights it advances at an apparent 2.11 s per tick, so it cannot be used to time anything across a gap. |

### Exception classes

Three, matching the contributor's description:

- **`0x1000D6` (1,048,790) — orphan sentinel.** 3,828 records, overwhelmingly `0x0C`
  (3,740) with a few `0x0D` (83) and a handful of other types. Its high byte `0x10` is
  far outside the real counter's observed range (max `0x0A2A1A`), so it is a sentinel
  rather than a counter value. These records cannot be paired.
- **Zero.** 918 records, but only 58 of them are breath records (`0x0D`); the rest are
  `0x43` (146) and `0x44` (714), where a zero is the normal encoding rather than an
  error.
- **Residual glitches.** After excluding the two classes above, pairing is exact, so any
  remaining anomaly is below the resolution of this card.

### Caution: the tag on other record types is not a breath reference

Respiratory and flow-limitation records (`0x01`–`0x10`) carry values in the same numeric
range, and every one of the 680 with a valid tag has that tag appearing on some breath
record. But the nearest such breath is a **median 14.9 s away** (P95 28.8 s, max 79.4 s),
which is far too distant to be the concurrent breath — the matches are collisions caused
by the counter resetting and reusing values. Do not use the tag to tie an event to a
breath.

Other types: `0x42` holds a near-constant (byte `0x11` is 52 or 53); `0x40`/`0x41` hold
small round values with byte `0x11` zero; `0x43` always has byte `0x11` zero, leaving the
uint16 at `0x12` (see §4a).

### What this does and does not give us

It supersedes the earlier note that `0x11` is a "run tag ... non-zero during FL episodes,
same value held across consecutive breaths" — that was the low byte of this counter.

Pairing makes per-breath Ti (`0x0C` -> matching `0x0D`) and Te (that `0x0D` -> next
`0x0C`) computable, which is tempting because the G3X path currently exports no Ti, Te
or I:E at all. **It is not accurate enough to publish.** Reconstructing I/E this way and
comparing against the device's own daily figures in the IDX day record gives a mean
absolute error of 5.2 pp on the average and 4.4 pp on the median, 16.6 pp on the P95, and
per-day maxima of 650%–4700% against a true 62%–107%. The markers are detection instants,
not the flow-derived breath timing the device summarises. Publishing channels built from
them would repeat the mistake that led to `packetIePermille` being hardcoded to 0.

---

## 3b) `value2` on the Breath Records (`0x0C` / `0x0D`)

A contributor plotted `(timestamp, value2)` for `0x0C` alone, `0x0D` alone, and the
two together, and reported that all three traces resemble the tidal-volume waveform
without being identical to it, with the ranges looking like a linear transformation
of each other. Investigated 2026-08-12 on the reference cards.

The resemblance is real, the linear transformation is not exact, and following it up
produced a useful result in a different file — see `BMC_G3X_00X_FORMAT.md` §5a, where
the tidal volume and minute ventilation **scale factors are now settled**. The
per-breath records themselves stay undecoded.

### What the field is

`value2` is populated on every `0x0C` and `0x0D` record (241 080 of 241 080 on the
reference card), `unk1e` is always zero, and the values are not quantised to any step:

| | min | p05 | median | p95 | max | distinct |
|---|---|---|---|---|---|---|
| `0x0C` | 21 | 145 | 239 | 457 | 1452 | 964 |
| `0x0D` | 21 | 102 | 249 | 438 | 1711 | 1091 |

Correlation against the waveform tidal volume field (`0x52C`), sampled two seconds
after the breath, is r = +0.817 for `0x0C`, +0.776 for `0x0D` and **+0.845 for the
sum** — high, but nothing like an identity. Only 0.8% / 2.5% of tidal-volume updates
take a value that any nearby breath record also carries, and `(v2C + v2D) / TV` spans
1.20–2.22 (median 1.64). So the two are different measurements of the same underlying
quantity, not the same number in different units.

### Three readings ruled out

- **Not Ti / Te.** Against the timestamp-measured inspiration time the correlation is
  only r = +0.351. Reconstructing the day's I/E as `v2C / (v2C + v2D)` and comparing
  against the device's own daily figures (IDX `0x140`–`0x146`, §9 open question 9)
  gives MAE 9.2 pp on the average, 7.6 pp on the median and 14.1 pp on the maximum —
  *worse* than the tag-paired timestamp reconstruction in §3a (5.2 / 4.4 pp), and the
  reconstruction sits at 46–53% on every night while the device reports 32–50%. This
  does **not** answer open question 9.
- **Not a leak-uncompensated volume.** If the waveform field were the leak-corrected
  version of this one, the gap between them would track leak. It does not:
  `(v2C + v2D) / TV` against leak gives r = −0.002 (`0x52A`) and +0.017 (`0x568`),
  and against pressure trend r = −0.007.
- **Not the source the waveform field averages.** Smoothing the per-breath series over
  3–45 breaths lowers the correlation monotonically (0.845 → 0.641), so `0x52C` is an
  instantaneous per-breath value, not a mean of these.

### Why it is not worth decoding further

OSCAR already reads tidal volume directly from the waveform at 1 Hz, and §5a of the
`.00x` note shows that field updates once per breath in lockstep with these records.
A second per-breath proxy for a channel we already carry adds nothing, and no linear
map from `value2` to it survives the residual analysis above.

---

## 4) Periodic Breathing Episodes

### Primary source: `0x09` records (all firmware)

`0x09` records are PB episode start markers confirmed on Lijunjun (B20A SC.75) and Kavolodin (A20) data (2026-03-30).

- **Timestamp** = START of the PB episode (opposite convention from all other respiratory events).
- **Duration** = uint32 LE at offsets 0x1C–0x1F: `(value2) | (unk1e << 16)`, in milliseconds.
  - Reading as uint16 (`value2` only) gives wrong short durations; must use the full uint32.
- **OSCAR channel:** `CPAP_PB`, start-time based (EndTime = StartTime + duration passed to OSCAR; OSCAR draws backward to show the span).

### Secondary source: `0x44` records (SC.74+/SC.75 only)

`0x44` records are single-point timestamps with no duration field. Present on newer firmware (SC.74+/SC.75); absent on SC.72. The parser groups consecutive records into episodes:

- **Gap threshold:** If two consecutive `0x44` records are more than **10 minutes** apart, they belong to separate episodes.
- **Minimum records per episode:** Groups of fewer than **2 records** are discarded.
- **Episode duration:** span from first to last record in the group, plus a 10-second trail.

Reference night pattern (2026-03-16): 26 total `0x44` records; 2 genuine episodes (~01:15 and ~04:40); solo scattered records approximately every 25–30 minutes throughout the night (suppressed by minimum-records filter).

---

## 4a) `0x43` Span Records

Reported by a contributor and confirmed 2026-08-07 on the reference card
(366 records over 19 days).

`value2` (`0x1C`) and `unk1e` (`0x1E`) are one little-endian uint32 holding a
duration in milliseconds. The record timestamp is the **start** of that span, and
consecutive records tile the session end-to-end.

| Test | Result |
|------|--------|
| Quantisation | All 366 durations are exact multiples of 1000 ms. 291 are exact multiples of 60,000 ms. |
| Range | 1 s to 169 min; median 9 min. |
| Tiling | For all 314 consecutive pairs inside one session, `t1 + dur1` lands within ±1.0 s of `t2` (269 of them within ±0.2 s). The ±1 s spread is the duration's own one-second quantisation. |
| Session start | The first span of a session begins 0.7–1.2 s after the `0x40` marker. |
| Session end | For all 52 sessions containing span records, the last span ends within ±1.5 s of the `0x41` marker (median +0.04 s) — the closing span is truncated to the session end, which is why the non-minute durations exist. |

So the record type is structurally solved: it partitions each session into
contiguous, mostly whole-minute intervals. **What is being partitioned is still
unknown** — the uint32 consumes the whole payload, leaving no measurement in the
record. Byte `0x11` is zero on every one of these records, leaving the uint16 at `0x12`
as the only candidate carrier (98 distinct values over 0–1000, with 146 of the 366
records holding zero, and no obvious relation to the duration).

This resolves what was open question 7 (`unk1e` in `0x43` is indeed a high duration
word) and the `value2` half of open question 2.

---

## 5) Respiratory Event Decode Rules

For message types `0x01`–`0x08`, `0x0A` (RERA), and `0x09` (PB):
- `0x1A` = the timestamp's millisecond part (§2a), not a payload field.
- `value2` = duration in **milliseconds** (confirmed 2026-03-25 by comparing value2/1000 against OSCAR waveform graphs).
- Duration = `round(value2 / 1000)` seconds, clamped to `[10, 180]` seconds.
- `StartTime` = record timestamp; `EndTime` = `StartTime + duration`.
  (**Timestamp marks the START** for all respiratory event types — confirmed 2026-04-03 by PAP-Link position comparison: an OSA event PAP-Link reports as 2:22:10–2:22:28 has EVT timestamp 2:22:10 and OSCAR displays the event correctly as 2:22:10–2:22:28.)
- Events are mapped to OSCAR respiratory EventLists and assigned to the session whose time window contains `StartTime`.

**`0x09` (PB marker) — duration only:**
- Duration is uint32 LE at 0x1C–0x1F: `(value2) | (unk1e << 16)`, in milliseconds.
  - `unk1e` is non-zero here because PB episodes exceed the uint16 ceiling (~65.5 s).
  - For all other respiratory event types, `unk1e` is zero — this is not a universal uint32 convention; `0x09` is the confirmed exception.
- Not subject to the 10–180 s clamp (PB episodes can be several minutes long).

**Flow limitation events `0x0E`/`0x0F`/`0x10`:**
- Timestamp = end-of-expiration trough (start of inspiration).
- `value2` = inspiration duration (Ti) in ms; used as bar width.
- Bar plotted forward from timestamp to timestamp + Ti, with a zero bookend 100 ms before timestamp.

---

## 6) Session Boundary Markers (0x40, 0x41) and Pressure Decode Rules (0x42)

**0x40 — Session start marker:** Timestamp = moment machine begins therapy recording. Reference night: `2026-03-16T00:27:53`. Not used for graph boundaries — `findStableStartMs()` is more accurate as it excludes startup noise. Second-precision only.

**0x41 — Session end marker:** Timestamp = moment machine stops therapy recording. Reference night: `2026-03-16T09:04:22`. Not used — waveform-derived end is equivalent and sub-second precise.

**Both markers appear many times per file** — 63 start/end pairs over 19 days on the
reference card, several per night where therapy was interrupted. (This answers what
was open question 4.)

**The firmware injects a marker pair at noon.** Confirmed 2026-08-07 by a
contributor who deliberately kept the machine running across 12:00:

```
0x40 Start  11:58:21.546  seq=18
0x41 End    12:00:00.027  seq=18
0x40 Start  12:00:00.117  seq=19
0x41 End    12:05:56.250  seq=19
```

The end/start pair is 90 ms apart and straddles noon exactly; no user could stop and
restart a machine in that interval, and it was the only marker pair within 90 s of
noon anywhere on the card. The day sequence at `0x02` (§2b) increments across the
same boundary.

Consequence for OSCAR: a therapy session never spans the noon boundary in the source
data — the device has already split it, on the same boundary OSCAR uses for its own
day. Nothing in the loader needs to split such a session, and none has been observed
to need it.

### Pressure Decode Rules (0x42)

- `value2` (offset 0x1C) = **EPAP** in hundredths of cmH2O. Zero values are ignored.
- `unk1e` (offset 0x1E) = **IPAP** in hundredths of cmH2O. Confirmed 2026-03-25: for the G3 B20A (Lijunjun, PS = 1.00 cmH2O), `unk1e` = `value2` + 100 in every record.  For a pure CPAP device (no pressure support) IPAP = EPAP so both fields carry the same value.
- All `0x42` records within the day's time window are collected, sorted by timestamp, and merged into the waveform packet stream in timestamp order.
- If no `0x42` records are found in the IDX day slice, a ±12-hour fallback scan is performed across the full EVT file.
- **Note:** With `kG3xUsePressureTrendForPressureChannel = true` (current default), the `CPAP_Pressure`/`CPAP_IPAP`/`CPAP_EPAP` channels are overridden by the per-packet PressureTrend values (0x76C/0x76E from the waveform packet). The `0x42` values remain in the state machine and are used only when no PressureTrend is available (e.g. to seed initial pressure before the first waveform packet).
- **TODO:** Update the EVT parser to read EPAP from `value2` and IPAP from `unk1e`, so bilevel devices display correct IPAP/EPAP channels rather than the same value for both.

---

## 7) Timing / Cadence Notes

- Timestamp granularity: 1 millisecond (6-byte whole second at `0x14` plus the millisecond field at `0x1A`; see §2a).
- Multiple records can share the same second, and are ordered within it by the millisecond field.
- Cadence is message-type dependent; `0x42` fires approximately every 10–30 seconds; `0x0C`/`0x0D` fire approximately once per second.
- Do not assume a fixed record interval.

---

## 8) How OSCAR Currently Uses the `.evt` Stream

1. **Phase 1 — EVT parse (day slice):**
   - `0x42` → pressure snapshot list (sorted by timestamp, merged into waveform loop).
   - `0x01`–`0x08`, `0x0A` → raw respiratory event list (timestamp = START, value2 = duration ms).
   - `0x09` → raw PB episode list (timestamp = START, uint32 duration at 0x1C–0x1F).
   - `0x0E`/`0x0F`/`0x10` → raw flow limitation list (timestamp = inspiration trough, value2 = Ti ms).
   - `0x44` → raw PB timestamp list (secondary; used only if 0x09 absent).
   - All other types → counted for diagnostics, not decoded.

2. **Phase 2 — Respiratory event mapping:**
   - Raw events mapped to `BmcRespiratoryEventType` (OSA, CSA, UA, HYP, RERA).
   - `0x07` (OH) and `0x08` (CH) both map to `HYP` → `CPAP_Hypopnea`. PAP-Link distinguishes these; OSCAR does not.
   - Placed into `dateSession.RespiratoryEvents` with computed start/end times.

3. **Phase 2b — PB collection:**
   - `0x09` PB events appended directly to `dateSession.RespiratoryEvents` (start + duration already computed).
   - `0x44` grouping algorithm runs only when `0x09` records are absent (firmware fallback).

4. **Phase 3–4 — Waveform loop:**
   - Waveform packets iterated in file order.
   - For each packet, any `0x42` pressure updates with timestamps ≤ packet timestamp are merged into current pressure state before the packet is processed.

5. **Session assignment:**
   - Respiratory events (including PB) assigned to the `BmcSession` whose time window contains the event's `StartTime`.

---

## 9) Known Open Questions

1. Exact function of `0x0C` and `0x0D`. Partly narrowed 2026-08-12 (§3b): `value2` is a per-breath magnitude that correlates strongly with tidal volume (r = +0.845 for the pair sum) but is not a linear function of it, is not Ti/Te, is not leak-related, and is not what the waveform field averages. Low priority — OSCAR already carries tidal volume from the waveform.
2. **What quantity `0x43` segments.** Structure resolved 2026-08-07 (§4a): the record is a start timestamp plus a uint32 ms duration, and consecutive records tile the session. The measurement itself has not been found; the only unexplained field left in the record is the uint16 at `0x12`.
3. Whether the duration field semantics (`value2` in milliseconds) hold consistently across all firmware versions.
4. ~~Whether `0x40` / `0x41` appear more than once per file~~ — **answered 2026-08-07**: yes, many times (63 pairs over 19 days on the reference card), and the firmware injects a pair at the noon boundary. See §6.
5. ~~Meaning of the sub-byte (offset `0x11`) run tag in `0x0C`/`0x0D`~~ — **answered 2026-08-08**: `0x11`-`0x13` is a 24-bit tick counter pairing each inspiration with its expiration. See §3a. What the same field means on the FL types `0x0E`/`0x0F`/`0x10` is still open — it is in the counter's range but does not resolve to a concurrent breath.
6. Whether `0x09` is absent on SC.72 firmware (same firmware generation that lacks `0x44`), or whether it exists but was not observed in available SC.72 test data.
7. ~~Whether `unk1e` in `0x43` encodes a high duration word~~ — **answered 2026-08-07**: it does. See §4a.
8. Cause of the single `0x09` record carrying `seq = 0` at `0x02` where its neighbours carry the correct day index (§2b). One occurrence in 246,260 records.
9. Where the **per-sample** I:E ratio lives in the waveform packet. The daily summary is settled: the IDX day record carries I/E as percent x 10 at `0x140` (max), `0x142` (average), `0x144` (P95) and `0x146` (median), confirmed 2026-08-08 against a PAP-Link readout — see `BMC_G3X_IDX_File_Layout.md`. Those four values are an oracle for the per-sample field: the right offset is the one whose per-day statistics reproduce them. Ruled out so far: waveform tidal volume (`0x52C`), Ti/Te reconstructed from tag-paired breath markers (§3a), and `value2` on the breath records read as Ti/Te (§3b). Until it is found the G3X path exports no I:E, Ti or Te — `packetIePermille` is hardcoded to 0.

---

## 10) Practical Validation Workflow

- Compare OSCAR event counts and AHI against BMC PAP-Link on the same night.
- Inspect debug output (build with `-DBMCDEBUG`) for `BmcG3xData respiratory summary day ...` and `BmcG3xData PB day ...`.
- Use the diagnostics CSV (`g3x-diagnostics/*.csv`) for packet-level correlation (BMCDEBUG builds only).
- When adding a new message type mapping, validate at both full-night and zoomed timeline scale.

This note should be updated whenever `.evt` message type mappings, grouping parameters, or decode rules change.
