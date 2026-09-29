# Sefam Analyze against OSCAR — what differs, and which differences are real

**Purpose:** a checklist for anyone comparing OSCAR's output for a SEFAM card
against *Sefam Analyze*. Several differences are structural and will never close.
Knowing which ones saves hours of chasing them as defects.

**Calibration basis.** The figures below come from the one card that has both a
full manufacturer report and a CSV export: a Rêve Auto, 31 sessions over 21 days,
159.00 h recorded. Every one of its 31 vendor rows was matched to its session
directory and compared field by field. Where a number is quoted as "vendor", it
is that card's own analyzer output.

---

## 1. AHI — OSCAR always counts central hypopnoeas; the analyzer is configurable

**This is the largest single difference, and it is a setting on the analyzer's side,
not a defect on ours.**

Sefam Analyze can be told to leave whole classes of event out of its AHI. The
switches are in **Analysis parameters → Compliance**, in a group headed
**"Respiratory events (reports only)"**:

- Excluding ramp
- Exclude time above limit leak (CPAP/Auto-CPAP only)
- **Exclude central hypopnoeas**
- Exclude central apnoeas

All four were unchecked when the calibration card's report was produced, and the
report echoes that state on its second line:

```
Including : CNT Apnoeas, CNT Hypopnoeas, Ramp, > Limit leak
```

**With nothing excluded, the analyzer counts central hypopnoeas.** Its per-session
window reports AHI as a count and an index, and both include CH — one session of the
calibration card shows AHI 25 events / 3.3 evts/h, which is `OBS AH 6` (5 OA + 1 OH)
plus `CNT AH 19` (12 CA + **7 CH**). The period summary, however, prints *two* AHI
lines whatever the setting:

```
AHI : OA, OH, CA         : 3.2
AHI : OA, OH, CA, CH     : 4.7
```

From the card's own event counts over its own recorded span:

| definition | computed | vendor prints |
|---|---|---|
| OA + OH + CA | **3.24** | 3.2 |
| OA + OH + CA + **CH** | **4.70** | 4.7 |

`schema::init()` puts `CPAP_CentralHypopnea` into `ahiChannels`
(`oscar/SleepLib/schema.cpp`), so **OSCAR always reports the second figure**, and it
applies no exclusions of its own.

> **Compare OSCAR against the analyzer's "AHI : OA, OH, CA, CH" line**, with all four
> exclusions off. Against the first line OSCAR reads 47 % high on this card for exactly
> the same events, and a Néa logs central hypopnoeas freely.

### The setting leaves no trace OSCAR can read

It is a preference of the PC application — the group is labelled "reports only" — and
it is not on the card:

- the settings record carries therapy settings only (mode, pressures, ramp, humidifier,
  mask leak, Comfort Control Plus, circuit diameter — see `SefamParsing::Settings`);
- the only text file on a card, `DATA_00x.INI`, is a channel manifest (`[Create Info]`,
  `[Start Record]`, `[Chan0]`…`[Chan11]`);
- scanning a whole Néa card for `hypop` / `exclu` / `AHI` / `IAH` turns up nothing but
  two byte coincidences inside the binary memory image.

So OSCAR cannot detect how a given report was generated, cannot mirror the setting, and
should not try to. A user whose analyzer has "Exclude central hypopnoeas" ticked will
always see OSCAR read higher, and the only record of that is the `Including :` line in
their own report.

### For support

Ask for the `Including :` line, and say which AHI line to compare against. If the user
is working from the per-session window instead, the arithmetic there is checkable:
`AHI` (count) should equal `OBS AH + CNT AH`, where `OBS AH = OBS Apnoeas + OBS
Hypopnoeas` and `CNT AH = CNT Apnoeas + CNT Hypopnoeas`. If it does not, an exclusion
is active.

## 2. The denominator is not worth chasing

Three candidate denominators on the same card:

| | hours |
|---|---|
| span recorded on the card | 159.00 |
| vendor "Operating time (ON/OFF)" | 159.30 |
| vendor "Total usage duration" | 158.83 |

They span 0.3 %, and the AHI is identical to two decimal places on all three.
OSCAR divides by usage with blower-off stretches excluded; the analyzer offers
both an operating time and a usage duration. On a card with far more blower-off
than this one the gap would widen, but it starts as noise.

## 3. Event counts agree closely, and the small gap is deliberate

| | card | vendor |
|---|---|---|
| obstructive apnoea | 248 | 242 |
| central apnoea | 191 | 190 |
| obstructive hypopnoea | 76 | 75 |
| central hypopnoea | 233 | 232 |
| snore | 1408 | 1399 |

The one- and two-event drifts are session-boundary effects. **The six-event gap on
obstructive apnoea is intentional**: the loader keeps the handful of events that
fall inside a blower-off span, which the analyzer discards. `SefamLoader::Open()`
says so at the point where blower-off spans are applied — only 7 of 1568 events
on this card are affected, they sit at the detection boundaries, and dropping them
would have disturbed counts already reconciled against the report.

## 4. Flow limitation — the analyzer disagrees with itself

The card holds **961** flow-limitation records. The analyzer reports both:

| analyzer view | value | as an index |
|---|---|---|
| report summary, "FL Runs" | 6.1 /h | = 961 ÷ 159.0 — **agrees with the card** |
| CSV export, "FL Runs" column | 352 total | = 2.2 /h — **does not** |

Most likely the CSV column counts *runs* of consecutive flow limitation and the
summary index counts individual events. Whatever OSCAR reports, it will look
wrong against one of the two analyzer views. Nothing here needs fixing until it
is known which the vendor considers authoritative.

## 5. Waveforms will not match sample for sample, by construction

- **Pressure.** The card carries no therapy-pressure signal at all. `PRE` is a
  *mask* pressure and carries the breathing ripple, so `CPAP_Pressure` is derived
  in the loader by a 10-second moving average — see the long comment in
  `SefamLoader::Open()` for why an average and not a median. The analyzer draws
  its own trace from the same source by its own method. Session averages agree;
  individual samples will not.
- **Hypopnea flag positions.** The card records no event durations, so the loader
  places obstructive and central hypopnea flags using the vendor's published mean
  durations — **16 s and 15 s**. Those came from one patient's report and are
  **not device constants**. Counts are unaffected; positions on any other card,
  the Néa included, are approximate.
- Apnoea flags are placed at the start of the event, from the duration the log
  does carry, matching the ResMed convention.

## 6. Settings — resolved, and the traps that remain

Mode, prescribed pressure, both ramp times, comfort level, patient circuit, mask
leak and humidifier all now agree with the analyzer's per-day settings table
wherever a card has data. See `SEFAM_REVE_CARD_ANALYSIS.md`. What is left:

- **The analyzer has a data source the card does not.** One Néa card has a
  **215-day hole** in which the analyzer nonetheless shows per-day settings,
  including changes. Directory numbering is consecutive across the hole, so
  nothing was pruned — the card simply was not in the machine. The leading
  candidate is the `<serial>.RAM` memory image, which on the S.Box holds a
  *lifetime* archive far exceeding the session directories; but the Rêve and Néa
  images are 8.00 bits per byte with no null byte in 1.6 MB, i.e. encrypted.
  **Any difference inside such a window is not an OSCAR defect.**
- **Both programs use a noon-to-noon day.** The analyzer's settings dates map onto
  the card's records only under that rule — plain calendar dates do not fit, and
  neither does subtracting a day. Dates are directly comparable.
- **OSCAR hides short sessions by default.** `Machine::AddSession()` drops any
  session shorter than the `IgnoreShorterSessions` preference — **5 minutes** out
  of the box — from the day record. The session is still imported and stored. A
  short bench session at setup will appear in the analyzer and not in OSCAR until
  that slider is set to 0.
- **The report's session rows carry the latest settings, not each session's.**
  The per-day *Settings* dialog is genuinely per-day; the session table is not.
  Where they differ on a card whose settings changed, OSCAR is the more accurate.
- **Humidifier does not exist on the S.Box**, and the **heated tube level** is
  not imported on any model. Neither is in the settings record. The analyzer gets
  them from elsewhere.
- **Leak units differ.** The analyzer reports *unintentional* leak in **L/s**;
  the card carries *total* leak, which OSCAR imports as `CPAP_LeakTotal` and
  converts. See `SEFAM_REVE_CARD_ANALYSIS.md`, open problem 5.

---

## Checklist before reporting a difference as a defect

1. What does the report's `Including :` line say? Any of the four exclusions in
   Analysis parameters → Compliance changes the analyzer's AHI and is invisible to
   OSCAR (§1).
2. Which AHI line is being read — `AHI : OA, OH, CA` or `AHI : OA, OH, CA, CH`?
   OSCAR's figure is the second.
3. Does the card have data for that day at all? Check the session directories,
   not the analyzer.
4. Is the session shorter than the `IgnoreShorterSessions` preference?
5. Is it flow limitation? The analyzer's two views disagree with each other.
6. Is it a waveform sample or a flag position, rather than a count?
