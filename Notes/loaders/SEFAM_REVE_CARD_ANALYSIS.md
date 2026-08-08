# SEFAM Rêve Auto — SD card format analysis

**Status:** format decoded and **validated against the vendor's own analyzer
report** for the same 31 sessions. Container, waveform scalings, event taxonomy
and therapy settings are all confirmed. Remaining gaps are small and listed at
the end.
**Device:** SEFAM Rêve Auto (APAP). `Created By=REVE_AUTO`, model code `1279R`,
firmware `VER :A010500`. SEFAM is a French sleep-medicine manufacturer.

**Branding:** sold in India as the **Sanrai Rêve Auto** (Sanrai Med is a
distributor). This is a **relabel, not a variant**: the underside of the case is
marked "Sefam", and "Sanrai" appears only on an applied sticker. So the hardware
is a SEFAM unit that was stickered at distribution, and there is no reason to
expect a separate Sanrai firmware line — though regional firmware builds on
identical hardware remain possible in principle, and only a second card would
settle that.

Treat SEFAM and Sanrai as one device family, the way `prisma_loader.cpp` covers
both Weinmann and Löwenstein.

Note the card itself carries **no brand string at all**: the only identity on it
is `REVE_AUTO`, the model code `1279R`, and the serial. Detection is unaffected
(the model code and `#03/` header are a solid fingerprint), but whatever brand
OSCAR displays is a naming choice rather than something read from the data, and
users will report this machine under either name.
**Loader:** `sefam_loader.cpp` / `sefamDataParsing.cpp`, added 2026-08-03 from
this analysis. Design: `Notes/loaders/SEFAM_LOADER_DESIGN.md`. **Awaiting testing
by a real user** — everything below was verified against this one card and the
manufacturer's report for it.

Related: `SD_CARD_FINGERPRINTS.md` has an earlier entry for the **SEFAM S.Box
AUTO** (model code `1263R`, firmware `VER :A020400`) — same firmware platform,
different device class. That entry's guess that the obfuscation key is `0x9F` is
**wrong**; see below. Its "~100 byte header" estimate is also wrong (it is 71).

---

## Card layout

```
<SD card root>/
└── 1279R/                      model code
    └── <serial>/               device serial (digits only; the full serial
        │                       is "1279R" + this, i.e. modelcode+serial)
        ├── <serial>.RAM        1,633,228 bytes — encrypted device RAM image
        ├── <serial>.BKP        1,633,228 bytes — previous RAM image
        └── DATA_000 … DATA_031/
            ├── DATA_nnn.INI    1,167 bytes — cleartext channel manifest
            ├── DATA_nnn.LOG    event log
            ├── DATA_nnn.FLW    flow                  10 Hz
            ├── DATA_nnn.PRE    pressure               5 Hz
            ├── DATA_nnn.LK     leak                   1 Hz
            ├── DATA_nnn.DET    breath-phase bitfield 10 Hz
            ├── DATA_nnn.NSD    near-constant bitfield 10 Hz
            ├── DATA_nnn.Y17    event bitfield        10 Hz   ← not in the INI
            └── .ABD .HRT .PLS .POS .SPO .STS .THO    header-only stubs
```

One directory per session, numbered sequentially, **not** by date. The sample
holds 31 complete sessions (2026-07-10 → 2026-07-30, 159.0 h) plus a
`DATA_031/` containing only a `.LOG` — the next session's log, opened but with
no recording yet. **A loader must tolerate a partial final directory.**

Sessions are mask-on/mask-off segments, not nights: several nights have a short
1–3 minute session immediately followed by the real 7–8 h one. Merging into
OSCAR days is the loader's job.

The `.ABD/.HRT/.PLS/.POS/.SPO/.STS/.THO` stubs are the polygraph channels the
platform supports but this device does not populate. **File size == header size
means "channel not recorded"** — that is the presence test, since the INI
declares the full schema regardless.

---

## Header obfuscation: XOR 0xBF — header only

The fixed-length text header of every `.LOG` and channel data file is XORed with
the constant `0xBF`. Confirmed by known-plaintext: the header contains the device
serial, which also appears in cleartext in the `.INI`.

```
'0'(0x30)^0xBF = 0x8F     '9'(0x39)^0xBF = 0x86
' '(0x20)^0xBF = 0x9F     'R'(0x52)^0xBF = 0xED
```

> **The obfuscation stops at the end of the header.** The sample and log bodies
> that follow are stored in the clear. An earlier revision of this document said
> the whole file was XORed; that was wrong. Every measurement in this document
> was computed on raw bodies and is unaffected — only the description was wrong —
> but a loader that descrambles the body will corrupt every sample and fail
> every checksum.
>
> Demonstrated on one 8.5 h session's `PRE` channel:
>
> | Body treated as | checksum + sequence | mean pressure |
> |---|---|---|
> | plaintext (correct) | **3075 / 3075** | **5.25 cmH₂O** |
> | XORed with 0xBF | 0 / 3075 | 15.39 cmH₂O |
>
> The manufacturer's report gives 5.2 cmH₂O for that session. Note also that the
> record checksums are computed over the bytes **as stored**, so they only
> validate against an undescrambled body.

This is obfuscation, not encryption — a single constant across the whole card.
The `.INI` is **not** obfuscated (plain text). The `.RAM`/`.BKP` are **not**
XOR-0xBF either (see below).

## File header (after XOR)

Channel files — **71 bytes, fixed**:

```
#03/<serial padded to 20 chars>/<YYMMDDhhmmss>/<32 hex chars>/
 │           20 bytes                12 bytes        32 bytes
 └─ format version
```

> **The leading tag is a format version, not a constant.** The Rêve Auto writes
> `#03/`; the **S.Box AUTO writes `#02/`**. A loader that matches `#03/`
> literally will reject S.Box cards outright. Accept any `#NN/`.
>
> Both observed versions share the same field layout, and header length is
> detected independently of the version, so the number is useful for diagnostics
> but should not gate parsing.

- `<YYMMDDhhmmss>` is **local** time and matches the `.INI` `[Start Record]`
  exactly.
- The 32-hex field is two 8-byte values: the first is the session start as a
  **UTC** Unix epoch, the second is an unidentified signature/checksum.
  In this sample local = UTC+2, consistent throughout.

`.LOG` files — **38 bytes, fixed**: the same string without the 32-hex field.

> **Do not locate the end of the header by scanning for the 4th `/`.** In
> `DATA_017.LOG` a payload byte decodes to `/` and shifts a delimiter-scanning
> parser by two bytes. Use the fixed lengths (71 / 38).

## Record structure — verified on all 31 sessions

After the header, data files are a flat array of fixed records, each holding
**exactly 10 seconds** of samples:

```
[ rate x 10 sample bytes ][ checksum 1 byte ][ sequence 2 bytes, big-endian ]
```

- **checksum** = low 8 bits of the sum of the record's data bytes.
  Verified 3075/3075 records on every channel of the longest session.
- **sequence** = 1-based, strictly incremental.
- Record sizes: 10 Hz → 103 B, 5 Hz → 53 B, 1 Hz → 13 B.
  (`.PLS` is 75 Hz / 16-bit → 1503 B, unexercised in this sample.)

`file_size == header + N * record_size` holds exactly for all 31 sessions ×
6 populated channels — the layout is confirmed, not inferred.

Session duration = `N × 10` seconds. Sample rates come from the `.INI`, so the
reader can be fully data-driven.

## The `.INI` — schema only

Cleartext, parseable with `QSettings::IniFormat`. Contains `[Create Info]`
(firmware, serial, creation date), `[BLE Device]`/`[Oximeter]`/`[PolyLink]`
pairing addresses, `[Start Record]`, and `[Chan0]`–`[Chan11]` giving each
channel's `Name`, `Type`, `Unit`, `Min`, `Max`, `Freq`, `Bit`.

**Every `.INI` on the card is byte-identical apart from the timestamp fields.**
There are **no therapy settings** — no mode, pressure range, ramp, EPR or
humidifier. This is the format's biggest gap for OSCAR (see *Open problems*).

`Y17` is not declared in the `.INI` at all, but is present and populated.

---

## Channel scaling — confirmed

| Channel | Rate | Formula | Basis |
|---|---|---|---|
| `FLW` | 10 Hz | `-180 + raw × 460/255` L/min | INI `Min=-180 Max=280`; zero at raw ≈ 99.8 |
| `PRE` | 5 Hz | `raw × 0.1` cmH₂O | see below |
| `LK` | 1 Hz | `raw × 0.6` L/min | INI `Min=0 Max=153` → 153/255 = 0.6 |

`raw == 255` is the **invalid/no-data sentinel** on all three (whole 10-second
records of `0xFF` appear when the blower is off).

**Pressure is ×0.1, not ×0.2.** Across all 31 sessions the true `PRE` range
(excluding the sentinel) is 0–154, i.e. **0.0–15.4 cmH₂O**; ×0.2 would imply
30.8 cmH₂O, which no CPAP delivers. Percentiles: p5 = 4.0, p50 = 5.1,
p90 = 7.5, p99 = 11.8 cmH₂O — a patient sitting near the 4 cmH₂O APAP floor
most of the night.

**`PRE` is a MASK pressure, and the card has no plain therapy pressure.** The
trace carries the breathing ripple — about 1.0 cmH₂O peak to peak, on a setpoint
that walks slowly (4.1 → 9.2 cmH₂O over one session). Nothing else on the card
carries a pressure:

- `PRE` is the only channel declared with `Unit=cmH20`.
- `DET`, `NSD` and `Y17` are bit fields, not analogue signals — see their
  sections below for the value histograms.
- No `.LOG` record reports a pressure. Of the 17 codes present, only 2 and 13
  have non-zero payloads, and both are settings records.
- The manufacturer's own report publishes only an **average** pressure per
  session, and shows `Prescribed pressure` as `-` because the mode is A-PAP.

A therapy-pressure trace must therefore be *derived*. The ripple separates
cleanly: a 10-second moving average cuts it to roughly 0.1 cmH₂O — the channel's
own quantisation step — while shifting the session average by under 0.007 cmH₂O.
See `SEFAM_LOADER_DESIGN.md` §6 for the filter choice and the measurements
behind it.

**Two independent cross-checks confirm the flow and leak scales simultaneously:**

1. At the same instant, the `FLW` baseline and `LK` agree. `FLW` raw ≈ 113 →
   23.8 L/min; `LK` raw ≈ 38 → 22.8 L/min. So `FLW` is **total** flow
   (patient + leak), not patient flow.
2. Binning `LK` by concurrent `PRE` over a whole night gives a clean orifice
   law `LK_raw ≈ k·√P` with k essentially constant:

   | PRE (cmH₂O) | 4.0 | 4.5 | 5.0 | 5.5 | 6.0 | 6.5 | 7.0 | 7.5 |
   |---|---|---|---|---|---|---|---|---|
   | mean LK raw | 38.1 | 41.2 | 45.1 | 46.7 | 47.1 | 49.7 | 57.9 | 57.7 |
   | k = LK/√P | 19.1 | 19.4 | 20.2 | 19.9 | 19.2 | 19.5 | 21.9 | 21.1 |

   Converted, that is ≈23 L/min at 4 cmH₂O rising to ≈37 at 10 — textbook
   vented-mask flow. So `LK` is **total leak including intentional mask vent**,
   which OSCAR must handle the way it does for ResMed/PRS1 total-leak devices.

## `DET` — breath-phase bitfield (10 Hz)

Not a data channel; a per-sample flag byte. Bit occupancy over one night and
mean concurrent flow:

| Bit | Set | Mean flow SET | Mean flow CLEAR | Reading |
|---|---|---|---|---|
| 5 | 35.1 % | **42.6 L/min** | **18.3 L/min** | **inspiration** — confirmed |
| 6 | 12.8 % | 42.9 | 24.4 | sub-phase of inspiration (peak flow) |
| 1 | 89.1 % | 26.5 | 29.0 | likely "valid / patient connected" |
| 0 | 6.3 % | 30.0 | 26.6 | unknown |
| 4 | 0.8 % | 39.4 | 26.7 | unknown |
| 7 | 4.7 % | 32.2 | 26.5 | unknown |
| 2, 3 | <0.3 % | — | — | unknown |

Bit 5's 35 % occupancy is exactly a normal I:E ratio and the flow separation is
unambiguous. This gives a loader free breath-phase segmentation without
detecting it from the flow signal.

## `Y17` — event bitfield (10 Hz), meanings unknown

Undeclared 10 Hz flag byte, 99 % zero. Bits 0, 2, 3, 4, 5 are used. Runs are
mostly 0.7–0.8 s with occasional 10 s ones. Per-hour run rates are stable
across sessions (bit5 ≈ 19–30/h, bit0 ≈ 7–14/h), which is the profile of a
real respiratory-event channel — but **which** events is not established.

**`Y17` runs do not line up with `.LOG` records** (only ~4 % of runs fall within
±3 s of any log entry), so the two are independent views, not duplicates.

Since the `.LOG` is now confirmed as the scored-event source, `Y17` is *not*
needed for AHI.

### `Y17` does NOT carry event extents — tested and excluded

It was long assumed here that `Y17`'s most likely role was per-sample event
**extent**, which would have supplied the hypopnea durations the log omits.
**That was tested directly and it fails.**

`Y17` is undeclared in the `.INI`, so its geometry was inferred from a channel
that is declared: 103 bytes per 10 s record against `PRE`'s 53, i.e. 100 samples
plus the usual 3-byte trailer — **10 Hz, 8-bit**, identical in every session on
the card. Every apnea and hypopnea in the log was then mapped onto it through
the session's UTC epoch, and the non-zero run containing each event measured:

| | OA | CA | OH | CH |
|---|---|---|---|---|
| Events with any `Y17` activity within ±2 s | 15/290 | 22/252 | 4/90 | 16/304 |
| Mean length of the run that is present | 0.77 s | 0.76 s | 0.70 s | 0.76 s |

Two independent reasons this cannot be an event extent:

1. **It is absent for ~93 % of events.** An extent channel would mark all of
   them.
2. **Where present the runs are ~0.75 s, and the same length for all four event
   types.** `Y17` marks brief instants, not spans. Nothing 16 seconds long is
   being recorded there.

Whatever `Y17` is — its per-hour run rates are stable across sessions, which
still looks like a real respiratory signal — it is not the hypopnea duration
source, and a loader can continue to ignore it.

`NSD` is a near-constant bit field. Counted over all 5,724,100 samples on the
card: `0` 99.636 %, `1` 17,610 samples (0.308 %), `4` 120 samples (0.002 %),
`255` sentinel 3,100 (0.054 %) — four distinct values in total. Not literally
empty, but there is nothing to decode from it here.

For comparison, `Y17` takes 11 distinct values card-wide (0, 1, 4, 8, 16, 32,
33, 36, 40, 128, 255) and `DET` takes 93, structured as
{low 2 bits} × {`0x00`, `0x10`, `0x20` … `0xF0`} with `0x02` alone accounting
for 55 % of samples. All three are bit fields; none can carry a pressure.

## `.LOG` — 49-byte event records

```
offset 0  : uint32 LE  Unix epoch (UTC)
offset 4  : uint32     always 0   (upper half of a 64-bit time_t)
offset 8  : uint8      event code
offset 9  : uint16 BE  argument
offset 11 : 38 bytes   zero, except codes 2/13 which populate ~bytes 11-25
```

### Event codes — CONFIRMED against the vendor report

The manufacturer's *Sefam Analyze* software was run over this same card,
producing a per-session CSV and a period synthesis. Matching its event counts
against the log census settles the taxonomy:

| Code | OSCAR event | log n | report n | log /h | report /h |
|---|---|---|---|---|---|
| 3 | **Obstructive Apnea** | 248 | 242 | 1.56 | 1.5 |
| 4 | **Central Apnea** | 191 | 190 | 1.20 | 1.2 |
| 5 | **Obstructive Hypopnea** | 76 | 75 | 0.47 | 0.5 |
| 6 | **Central Hypopnea** | 233 | 232 | 1.46 | 1.5 |
| 7 | **Snore** | 1408 | 1399 | 8.86 | 8.8 |
| 8 | **Flow limitation** | 961 | — | 6.05 | 6.1 |
| 2 | **Settings change** | 5 | 3 | | |
| 9 | therapy stop (`PRE`≈0, `LK`=255 → blower off) | 42 | | 0.26 | |
| 10 | unidentified | 260 | | 1.64 | |
| 13 | settings/state snapshot | 13 | | | |
| 1, 11, 12, 21, 24, 27, 28 | ≤11 each, administrative | | | | |

Agreement is **per session**, not just in total — on most of the 31 sessions the
four apnea/hypopnea counts match exactly. The residual over-counts (≤6 across
21 days) are the analyzer applying inclusion rules the raw log does not; its own
synthesis header says it filters on ramp and leak state.

Every derived index reconciles arithmetically:

```
AHI (OA,OH,CA)     1.56 + 0.47 + 1.20               = 3.23   report 3.2
AHI (OA,OH,CA,CH)  1.56 + 0.47 + 1.20 + 1.46        = 4.69   report 4.7
AHI obstructive    1.56 + 0.47                      = 2.03   report 2.0
AHI central        1.20 + 1.46                      = 2.66   report 2.7
```

**Apnea duration = argument × 0.1 s.** Decoded mean 16.5 s for code 3 and 12.8 s
for code 4, against a reported average apnoea duration of **17 s (OA) | 13 s
(CA)**. The hard floor at exactly 100 is the ≥10 s scoring rule.

**Hypopnea duration is *not* in the argument, and is not anywhere else on the
card either.** Measured over all 39 sessions of the second card:

| Code | Event | Records | Argument = 0 | Non-zero values |
|---|---|---|---|---|
| 3 | OA | 290 | **0** | mean 16.08 s, floor exactly 10.0, max 110.0 |
| 4 | CA | 252 | **0** | mean 12.59 s, floor exactly 10.0, max 49.0 |
| 5 | OH | 90 | **88** | 30.7 and 38.0 s only |
| 6 | CH | 304 | **299** | 24.0 – 30.1 s, five values |

Apneas carry a duration on *every* record. Hypopneas carry zero on 98 %, and the
seven that do not are **longer** than the 16 s / 15 s averages the report prints,
so they cannot be the source of those figures either. The 38-byte payload is all
zeros on every event code — only codes 2 and 13 use it.

So the analyzer computes hypopnea durations rather than reading them. The likely
route is re-scoring `FLW`: it takes counts from the `.LOG` (they match per
session) and apnea durations from the log argument (16.5 / 12.8 decoded against
17 / 13 reported), and hypopnea extent is precisely the quantity the device does
not store but a PC re-reading the waveform recovers easily. That is inference,
not proof, and confirming it would mean replicating their scoring — no payoff
for OSCAR, which renders events itself.

**`Y17` is positively excluded as the source — see its section below.**

> **One vendor inconsistency, not a decode error.** The report's per-session
> `FL Runs` column sums to 352 (2.22/h) while its own synthesis index for the
> same period says **6.1/h**. The decoded code-8 count gives **6.05/h**, matching
> the synthesis. Every other event type agrees across all three sources
> (CSV sum, synthesis index, decoded log), so the CSV column is the outlier —
> most likely it counts grouped *runs* while the index counts individual
> detections. Prefer the synthesis reading: **code 8 = flow limitation, 1:1**.

### Settings records — codes 2 and 13

Bytes 11–25 are zero on every code **except 2 and 13**, which carry the therapy
settings. Decoded against the report's printed settings (`Mode=A-PAP`,
`Min=4.0`, `Max=20.0`, `Ramp=45 min`, `Ramp pressure=4.0`, `CC+=Level 2`,
`Humidifier=Level 4`, `Heated tube=Present`):

**Code 2** payload `40 45 45 31 200 40 60 60 130 200 121 4 6 3 0`:

| Byte | Value | Meaning |
|---|---|---|
| 11 | 40 | **Min pressure 4.0** (×0.1 cmH₂O) |
| 12 | 45 | **Ramp 45 min** |
| 15 | 200 | **Max pressure 20.0** (×0.1 cmH₂O) |
| 16 | 40 | **Ramp pressure 4.0** (×0.1 cmH₂O) |
| 17, 18 | 60, 60 | candidate: **theoretical mask leak**, 60 × 0.6 lpm = 36.0, see below |
| 22 | 4 | **Humidifier level** — confirmed by a controlled change, see below |
| 13, 14, 19–21, 23–25 | 45, 31, 130, 200, 121, 6, 3, 0 | unassigned |

Code 2 occurs 5 times. The report's `setting change` column is non-zero on
exactly the two sessions carrying multiple code-2 records (counts 1 and 2,
matching), and **byte 23 is the only byte that differs** between the pair on the
session reporting two changes — so byte 23 is the field that was edited.

**Byte 23 is therefore ruled out as any of the reported settings.** It changes
6 → 5 → 6 within one session across 60 seconds, while the analyzer reports the
comfort, humidifier and heated-tube settings as *identical on every session*.
Whatever byte 23 holds, the analyzer does not print it.

**Code 13** payload `[0/1] 0 3 40 100 200 0 [5–10] [0/1] 51 55 50 57 55 52`
repeats min (40 → 4.0) and max (200 → 20.0) pressure and adds a mode-like `3`
at byte 13 and a varying 5–10 counter at byte 18. Bytes 20–25 are a constant
six-byte tail.

This **overturns the earlier conclusion that settings were unrecoverable** — they
are in the `.LOG`, not the encrypted `.RAM`. Enough is confirmed for a loader to
report mode, pressure range and ramp; the comfort/humidifier fields are
candidates pending a second card with different settings to vary them against.

### Humidifier level — byte 22, CONFIRMED by a controlled change

A second card was pulled from the same device after the humidifier level was
changed from 4 to 5 and nothing else was touched. This is the controlled
experiment the section below asks for, and it settles byte 22 outright.

The second card is a strict superset of the first: same serial directory, and
of the 468 files the two have in common, **465 are byte-identical**. The three
that differ are the two encrypted state blobs (`.RAM`, `.BKP`, expected) and the
one session directory that was still being written when the first card was
copied. The device accumulates rather than rotating, so the earlier sessions
carry over untouched and act as their own control.

Across every settings record on the second card:

| Record | Date | Byte 22 | Rest of the payload |
|---|---|---|---|
| code 2 | 07-10 | 4 | `40 45 45 31 200 40 60 60 130 200 121 · 6 3 0` |
| code 2 | 07-12 | 4 | identical |
| code 2 | 07-26 (×2) | 4 | identical but for byte 23, already ruled out |
| code 2 | 07-27 | 4 | identical |
| code 2 | 07-31 | 4 | identical but for byte 14, see below |
| code 2 | 08-05 | **5** | **identical** |

**One setting changed by one step; exactly one byte changed, by exactly one
step.** With two points that both map to themselves, any affine reading is
pinned to identity — the byte is the level, unscaled and unbiased. Together with
the vendor manual's documented range of OFF to 10, which fits a raw byte
directly, that is enough to import.

The loader therefore reports it, as a SEFAM-specific `SETTING` channel rather
than through `CPAP_HumidSetting` — that generic id resolves through
`schema::channel["HumidSet"]`, and nothing in OSCAR ever registers a channel of
that name, so it is an empty channel and anything written to it is invisible.

Two limits worth keeping in view:

- **Only code 2 carries it.** A code 13 snapshot has a different payload shape
  and no humidifier field, so a session whose only settings record is a snapshot
  reports no humidifier level rather than an inherited one.
- **The change is bracketed, not timed.** Byte 22 reads 4 on 07-31 and 5 on
  08-05, with no settings record on the four nights between. Those nights
  inherit 4 by carry-forward. Whether code 2 is emitted *at* a change or as
  routine session bookkeeping is still unsettled — the vendor's own
  "setting change" column was non-zero only on sessions carrying *more than one*
  code 2 record, which argues for the routine reading and means the four
  intervening nights may have run at level 5.

**Byte 14 moved too, but on a different night.** It reads 31 (`0x1F`) on every
record except 07-31, where it is 29 (`0x1D`) — bit `0x02` cleared, four nights
before the humidifier record and back to 31 afterwards. It is therefore *not*
the humidifier. It remains the most flags-like byte in the record and is now
also known to be the only other one that ever varies, which makes it the best
candidate for the patient access lock or a tube/circuit state. No vendor report
exists for the second card, so what was different that night is unknown.

### Theoretical mask leak — bytes 17/18, strong candidate, not confirmed

The analyzer prints **36 lpm**, and **no byte anywhere on either card holds 36**.
That was checked exhaustively, not spot-checked: every distinct value at every
settings-record offset on both cards, the `.INI` text, and the cleartext head of
`.RAM`/`.BKP`. The only 36s in the logs are timestamp bytes and event-duration
arguments. So the figure is either derived, or it lives in the encrypted `.RAM`
where the analyzer can read it and we cannot.

The likely answer is that it *is* stored, in the leak channel's own units rather
than in lpm. The `.INI` declares `LK` as `Min=0 Max=153 Bit=8` — identical in all
39 sessions — which is exactly **0.6 lpm per count**, the same 0.6 the loader
already uses as the `CPAP_LeakTotal` gain. Then:

```
byte 17 = 60 counts x 0.6 lpm/count = 36.0 lpm
byte 18 = 60 counts x 0.6 lpm/count = 36.0 lpm
```

Exact, with no fitted constant, on a scale the card publishes about itself. The
two bytes cannot be told apart: both read 60 on every settings record on both
cards.

**This supersedes an earlier reading that treated byte 17/18 = 60 as "60 lpm",
the top of the documented 20–60 lpm range, and recorded the clash with the
printed 36 as unresolved.** There is no clash — 60 is not 60 lpm. The setting
range 20–60 lpm spans 33–100 counts, and 60 counts sits inside it.

The vendor's clinical manual for the Nea defines the setting as *"the
theoretical leak value of the mask at 12 cmH₂O, given in the corresponding
instructions for use of the mask"*, range 20 to 60 lpm. That fixes the reference
pressure as a constant, which makes an independent check possible. Fitting the
mask's vent curve from this card's own data — 749,448 paired leak/pressure
samples, taking the 5th-percentile leak in each 0.5 cmH₂O bin, since
unintentional leak only ever adds to the vent floor — gives leak ≈ 9.94·√P lpm,
so:

| | |
|---|---|
| Predicted at the manual's 12 cmH₂O | **34.4 lpm** |
| Mask nameplate, as printed by the analyzer | **36 lpm** |

4.5 % under: a real mask, measured through the device's own leak channel,
sitting just below its published figure. Supporting evidence, not proof.

**This does not contradict the k ≈ 11.7 lpm·√P⁻¹ quoted under § Channel
scaling** — the two fit different things. That one bins the *mean* of `LK`,
which is total leak, vent plus unintentional. This one takes the 5th percentile,
which isolates the vent. The mean necessarily runs higher, and using it here
would predict 11.7·√12 = 40.5 lpm against a 36 lpm nameplate — overshooting by
as much as the floor undershoots. The floor is the correct estimator for
comparison with a mask datasheet, precisely because unintentional leak is
strictly additive.

**A rejected alternative, recorded so it is not retried.** Byte 19 = 130 could
read as 13.0 cmH₂O, and the fitted curve gives 35.85 lpm there — but that
reference pressure was chosen *because* it matched, out of five candidates
spanning 4–20 on a monotone curve, so it was post-hoc. The manual then killed it
outright: the reference pressure is a fixed 12 cmH₂O from the mask datasheet, so
the device has no reason to store one. Byte 19 reverts to being a bit-field
candidate (`0x82`).

**The setting steps in 2 lpm increments**, per the Nea user manual — so the menu
offers 20, 22, … 60, twenty-one values in all. That is mildly awkward for the
0.6 lpm/count reading: only 24, 30, 36, 42, 48, 54 and 60 divide evenly into
0.6, so two thirds of the menu would have to be stored rounded. The observed
value happens to be one of the seven clean ones, which is either luck or a hint
that the reading is wrong.

It also admits a competing encoding that the even steps fit *better*:

| Encoding | byte at 36 lpm | Motivation |
|---|---|---|
| `round(lpm / 0.6)` | 60 ✓ | the `LK` channel's own declared scale |
| `lpm + 24` | 60 ✓ | preserves the 2 lpm step exactly as a step of 2 |
| `2·lpm − 12` | 60 ✓ | also integral on every menu value |

All three explain the observed 60. They diverge on any other value, which is
what makes a controlled change decisive — see the pre-registered predictions
below.

### Pre-registered predictions for a third card (recorded 2026-08-06)

A third card has been requested with **two** settings changed: mask leak
36 → 34 lpm, and Comfort Control Plus 2 → 3 if the device allows it.

These predictions are written down *before* the card exists, deliberately. The
byte 19 reference-pressure story above was arrived at by trying five candidates
and keeping the one that fitted, and it was wrong. Committing to the expected
values in advance means the next reading can only be a hit or a miss.

**Two settings changed at once is against the advice in this note**, and the
risk is real: if a byte moves that is not predicted here, it cannot be
attributed to one change or the other. The mitigation is that the two expected
bytes are far apart in value space — a mask-leak byte should land in the
mid-50s, a CC+ byte near 128 — so a moved byte's value should identify its
owner.

**Mask leak, 36 → 34 lpm.** The primary question is only whether bytes 17/18
move at all; if they do, they are the field. The exact value then picks the
encoding:

| Byte 17/18 reads | Conclusion |
|---|---|
| **57** | `round(lpm / 0.6)` — the `LK` count scale, rounding half up |
| **56** | same scale, truncating; or `2·lpm − 12` |
| **58** | `lpm + 24` |
| **34** | literal lpm — and bytes 17/18 were never the mask leak, since they would have had to read 36 before, not 60 |
| **unchanged at 60** | not the field |

If only *one* of the two bytes moves, that settles which is which. They have
been indistinguishable on every settings record on both cards so far.

**Comfort Control Plus, 2 → 3.** CC+ currently reads Level 2 and byte 19 is
`0x82`.

| Byte 19 reads | Conclusion |
|---|---|
| **131** (`0x83`) | low nibble is the CC+ level, `0x80` is something else — most likely the heated tube |
| **unchanged at 130** | the bit-field reading dies; byte 24 (= 3) is the next candidate |

**On "Level 2" versus an on/off toggle.** The manual describes CC+ in two
places — a *level* in the user menu and a *toggle* in the clinical menu — and
those are not in conflict. The natural reading is a clinician enable plus a
patient-selectable strength, which is exactly how ResMed EPR and Philips Flex
are structured: an on/off and a 1–3 level, reported together. That would also
explain a single byte carrying an enable bit and a small level field, which is
what `0x82` looks like.

If the device turns out to offer only on/off and the tester cannot set 3, that
is still a result worth recording: it would mean SA's "Level 2" is not a CC+
strength at all, and the whole byte 19 line of reasoning needs rethinking.

### Why the remaining comfort and accessory settings cannot be decoded

*(Written against the first card, before the humidifier was pinned. Everything
below still holds for the four settings other than the humidifier.)*

The analyzer's full device-settings panel for this card reads:

| Setting | Value |
|---|---|
| Comfort Control Plus | Level 2 |
| Patient Circuit | 15 mm |
| Theoretical mask leak | 36 lpm |
| Humidifier | Level 4 |
| Heated tube | Present |

**Every one of these is constant wherever a settings record exists**, so there is
nothing to correlate against. Three consequences worth recording so they are not
re-derived:

1. **No byte in code 2 holds 2, 15, 36 or 1.** The values present are 40, 45, 45,
   31, 200, 40, 60, 60, 130, 200, 121, 4, 6, 3. So Comfort Control Plus level,
   circuit diameter, mask leak and heated-tube presence are enum-coded,
   bit-packed, scaled, or simply not in this record. Byte 22 = 4 was the *only*
   byte matching any of the five, which was the whole basis for the humidifier
   candidacy — a value coincidence at the time, since confirmed independently.

   **Searching for the printed value was the wrong instinct**, and it cost time
   on the mask leak. The humidifier does store its level literally, but the mask
   leak looks to be stored on the leak channel's own scale, so the question to
   ask of a byte is not "does it equal the printed number" but "is there a scale
   the card itself declares on which it would". For the mask leak that scale was
   sitting in the `.INI` all along.

2. **The one discriminating session is unusable.** The analyzer reports
   `Heated tube = Missing` on exactly one session of 31 — and that session's
   `.LOG` contains **no code 2 and no code 13 record at all**. Its whole log is
   codes 28, 9, 10 and 7, every one of them zero-payload, and those codes also
   appear in 8–22 other sessions where the tube reads `Present`. So the single
   place where a setting differs carries no settings record to compare.

3. **`Patient Circuit` and `Theoretical mask leak` do not appear in the
   analyzer's CSV export at all** — only CC+, humidifier and heated tube do.
   That was once read as evidence they are not stored on the card. It is not:
   the mask leak almost certainly *is* stored, as bytes 17/18 in the leak
   channel's own 0.6 lpm counts — see "Theoretical mask leak — bytes 17/18"
   above. Absence from the CSV says something about the analyzer's export, not
   about the card.

Tempting bit-level readings exist — byte 19 is `0x82`, which would decompose as
`0x80` (tube present) `| 0x02` (CC+ level 2) — but with a single settings
combination on the card any such reading is unfalsifiable. **Nothing here should
be written into the loader until a card varies one of these fields.** Importing a
setting off a lone byte-value match risks showing a user a setting their device
does not have.

The prescribed resolution — one night from the *same* device with a **single**
setting changed, rather than a whole second card — was carried out for the
humidifier and worked exactly as predicted. The same method is the way to settle
the other four: see "Humidifier level — byte 22" above for what a clean result
looks like.

### Settings menu structure, from a vendor manual

A user manual for the SEFAM **Nea** documents the full settings menu. Whether
the Nea shares this platform is **not verified**, but its documented ranges are
consistent with everything observed here, so they are recorded as constraints on
the code 2 byte map.

**User menu:** treatment parameters (pressure, usually clinician-locked);
comfort (ramp time, Comfort Control Plus level); humidification, **OFF to 10**.

**Clinical menu:**

| Setting | Documented range |
|---|---|
| Mode | CPAP (fixed) or APAP |
| Min pressure | 4–20 cmH₂O |
| Max pressure | up to 20 cmH₂O (APAP) |
| Fixed pressure level | 4–20 cmH₂O (CPAP) |
| Ramp type | I RAMP, T RAMP or OFF |
| Ramp time | 5–45 min |
| Ramp pressure | start pressure |
| CC+ (Comfort Control Plus) | toggle in the clinical menu, *level* in the user menu — see below |
| IS (Intelligent Start) | toggle |
| Circuit select | 15 mm or 22 mm |
| Mask select theoretical leak | 20–60 lpm **in steps of 2**, being the mask's leak **at 12 cmH₂O** taken from its datasheet |
| Patient access lock | Ramp, CC+, IS |

What this does and does not settle:

1. **It corroborates the confirmed bytes.** Min 4.0 is the bottom of the 4–20
   range, max 20.0 the top, and ramp 45 the top of 5–45. Consistent with this
   card running the same firmware family — supporting evidence, not proof.
2. **Humidification runs 0–10**, so byte 22 = 4 is dimensionally plausible as
   "Level 4". This was later **confirmed** by a controlled 4 → 5 change; the
   documented range is what says a raw byte needs no scaling.
3. **Circuit select is a two-way choice (15 or 22 mm)**, so it can only be an
   enum or a single bit. That explains why no byte holds 15 or 22, and rules out
   any further search for a literal diameter.
4. ~~**Mask leak is configured over 20–60 lpm, and bytes 17 and 18 both hold
   exactly 60**, the top of that range, yet the analyzer prints 36 — unresolved.~~
   **Superseded.** The clash was an artefact of assuming the byte was in lpm.
   Read in the leak channel's own 0.6 lpm counts, 60 counts *is* 36.0 lpm.
   See "Theoretical mask leak — bytes 17/18".
5. **Byte 21 = 121 → 12.1 cmH₂O** falls inside the documented 4–20 fixed-pressure
   range, making it a candidate for the CPAP **Pressure Level** — a setting that
   is inactive in A-PAP, which is consistent with the analyzer printing
   `Prescribed pressure = -` on every session. Untested.
6. **Ramp type has three states and mode two**, so both are small enums. Byte 24
   = 3 and code 13 byte 13 = 3 are the candidates, though neither is a natural
   index for "I RAMP".
7. **Patient access lock covers three toggles**, so a small bitmask exists
   somewhere. Byte 14 = 31 (`0x1F`, five low bits set) is the most flags-like
   byte in the record, and the second card shows it is also the only byte other
   than 22 and 23 that ever varies — it drops to 29 (`0x1D`) on one night.
8. **Two documented features have never been seen in any card data:** Intelligent
   Start and the patient access lock.
9. **CC+ appears as a toggle in the clinical menu and as a level in the user
   menu**, which reads at first like a contradiction with the analyzer's
   "Level 2". It is not. A clinician enable plus a patient-selectable strength
   is the same structure as ResMed EPR and Philips Flex, and it is what a byte
   like `0x82` — one high bit set, a small value in the low bits — would look
   like. Unconfirmed, but it is the reading to test first.

## `.RAM` / `.BKP` — encrypted, not readable

Both 1,633,228 bytes. The first 74 bytes are structured cleartext (`4c 00 00 01`,
then a uint32 LE timestamp at offset 4, then a fixed block); everything after is
**8.000 bits/byte entropy in every one of 398 4 KB windows** — no plaintext, and
not XOR-0xBF.

The `.RAM` and `.BKP` in this sample were written ~6 h apart and differ in only
**1,803 of 1.6 M bytes (0.1 %)**, with all other offsets byte-identical. Content
changes therefore stay positionally local, which rules out CBC/CTR with a varying
IV and points to ECB or a fixed keystream. That makes a known-plaintext attack
conceivable *if* a card ever arrives with its device settings independently
known — but nothing is readable today.

---

## Validation summary

Everything below was checked against *Sefam Analyze* output for the same 31
sessions, not merely inferred from the card:

| Quantity | Result |
|---|---|
| Session→directory mapping | 31/31, `DATA_nnn` ↔ report session `n+1`, timestamps exact |
| Event taxonomy (OA/CA/OH/CH/snore/FL) | confirmed, per session and in total |
| Derived indices (AHI and components) | reconcile arithmetically to ±0.05 |
| Apnea durations | 16.5 / 12.8 s decoded vs 17 / 13 s reported |
| `PRE` × 0.1 cmH₂O | mean pressure matches report on **30 of 31 sessions within ±0.17 cmH₂O**, most within ±0.05 |
| Total recorded time | 159.0 h decoded vs "operating time 159 h 18" reported |
| Therapy settings | min/max pressure, ramp time and ramp pressure all recovered from log code 2 |
| Mask-disconnect seconds | `LK`==255 matches exactly where disconnects are long (412 s session: 412 decoded) |

The one session where mean pressure differs (−1.08 cmH₂O) is the 20-minute
session that was 23 % mask-disconnected — the analyzer excludes disconnected
time from the average, so the disagreement is expected and explains itself.

One further field, **humidifier level**, was confirmed by a different method: a
second card from the same device with only that setting changed. No analyzer
output exists for the second card, so the confirmation rests on the controlled
change rather than on a printed report — see "Humidifier level — byte 22".

## Open problems

1. **Hypopnea durations** are not on the card. Codes 5/6 carry a zero argument on
   98 % of records, the payload is zero on every event code, and ~~`Y17`~~ is now
   **positively excluded** (absent for ~93 % of events; runs of ~0.75 s where
   present). The analyzer must compute them, most plausibly by re-scoring `FLW`.
   Not worth chasing further: OSCAR renders events itself, so replicating a
   vendor scoring algorithm buys nothing. What *is* worth remembering is that the
   loader's 16 s / 15 s flag-placement constants came from this one patient's
   report and are not device constants — see "Loader viability".
2. **`Y17` bit meanings** still unknown, and it is now known *not* to be an event
   extent channel (see its section). It is not the apnea source either — the
   `.LOG` is — and its runs do not line up with log records, so it remains an
   independent signal of unknown purpose. Nothing depends on it.
3. **Log code 10** (260 records, 1.64/h) is unidentified and matches no report
   column.
4. **Comfort and accessory setting bytes** in code 2. Byte 23 is ruled out
   entirely. Status of the five:
   - ~~Humidifier level~~ **RESOLVED: byte 22**, confirmed by a controlled
     single-setting change.
   - **Theoretical mask leak: bytes 17/18**, strong candidate — 60 counts on the
     leak channel's declared 0.6 lpm scale is exactly the printed 36.0 lpm.
     Predicts byte 17/18 = 50 if the setting is changed to 30 lpm.
   - **Comfort Control Plus level, patient circuit, heated tube** remain
     unassigned. No byte holds 2, 15 or 1, so they are enum, bit-packed, scaled
     or absent.

   A controlled single-setting change settles each of these; see "Why the
   remaining comfort and accessory settings cannot be decoded".
5. ~~**Report "Average leaks"**~~ **RESOLVED.** The analyzer reports
   *unintentional* leak in **L/s**: its 0.06 / 0.05 / 0.03 for three sample
   sessions are 3.6 / 3.0 / 1.8 L/min, against OSCAR's derived `CPAP_Leak` of
   2.77 / 3.73 / 1.73 for the same sessions. The card itself gives **total**
   leak, which OSCAR imports as `CPAP_LeakTotal` and converts.
6. The 8-byte header signature is unidentified (not needed to read data).
7. `NSD` is 99.64 % zero in this sample, with only four distinct values;
   purpose unknown.
8. `.RAM`/`.BKP` remain encrypted — but this no longer matters, since the
   settings turned out to be in the `.LOG`.

## What would still help

- **One night with a single setting changed** — still the cheapest and most
  useful next artefact, and it does not need a different device. This is how
  byte 22 was settled: the humidifier moved 4 → 5 and exactly one byte moved
  4 → 5. Changing several settings at once is much less informative, because
  several unassigned bytes hold plausible values.
  **Comfort Control Plus is the best probe reachable without clinician access**,
  and it comes with a prediction: CC+ is at Level 2 and byte 19 is `0x82`, so if
  the low nibble is the level, byte 19 should read 129 (`0x81`) at Level 1 or
  131 (`0x83`) at Level 3. If it does not move, that bit-field reading dies and
  byte 24 (= 3) is next. Ask for a one-step change, 1 or 3, to keep it
  unambiguous.
  Note that a settings record has to actually appear afterwards — four nights
  on the second card carry none at all — so the card should be pulled a night
  or two after the change, not the same morning.
- **Requested 2026-08-06 and pending:** a card with mask leak changed 36 → 34 lpm
  and CC+ changed 2 → 3. Expected byte values are written down in advance under
  "Pre-registered predictions for a third card" — read that *before* looking at
  the card.
- **Circuit select switched between 15 mm and 22 mm** would locate a two-state
  field that cannot be found by value matching, since neither diameter is stored
  literally.
- **A card in fixed-CPAP mode** — would confirm the mode encoding (only `A-PAP`
  has ever been seen) and show which log codes change.
- A card whose report shows **non-zero "No breath"** events, to identify code 10.

## Loader viability

**Good.** The container was always the easy part; the vendor report has now also
settled the two things that actually blocked a useful loader — the event
taxonomy and the therapy settings. A loader can produce flow, pressure, leak,
breath phase, OA/CA/OH/CH with real durations for apneas, snore, flow
limitation, AHI, and the pressure/ramp settings, with every one of those
validated against the manufacturer's own analysis of the same nights.

Two caveats for implementation. Apnea durations come from the log argument but
**hypopnea durations do not, and cannot be recovered** — they are not on the
card in any form (open problem 1), so they stay zero-duration flags.

> **A live wart, worth knowing about.** Because the hypopnea log timestamp sits
> at the event *end*, the loader shifts those flags back for display by
> `kObstructiveHypopneaPlacementMs` = 16 s and `kCentralHypopneaPlacementMs` =
> 15 s. **Those two numbers are one patient's 21-day averages from one vendor
> report** — not device constants. For any other user they will be wrong by
> however much that person's hypopneas differ, in whichever direction. The
> stored duration stays zero so nothing false appears in a tooltip, but the flag
> position is a guess calibrated on a single dataset. Revisit it the first time
> a second real user's data can be compared.

And the analyzer applies inclusion filtering
(ramp, leak) that the raw log does not, so OSCAR's counts will run a few events
higher across a 3-week period; that is a scoring-policy difference, not a bug,
and it should not be "fixed" by inventing filters to match.

Note the platform is shared with the S.Box AUTO (different model code, different
firmware, 25 Hz flow, and a shorter header in that sample). Header length and
sample rates must be taken from the data and the `.INI`, never hardcoded, if one
loader is to cover both.
