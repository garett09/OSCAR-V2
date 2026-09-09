# SEFAM Rêve Auto — SD card format analysis

**Status:** format decoded and **validated against the vendor's own analyzer
report** for the same 31 sessions. Container, waveform scalings, event taxonomy
and therapy settings are all confirmed. Remaining gaps are small and listed at
the end.

**Settings decode, as of 2026-08-13.** Four of the five accessory settings the
analyzer prints are decoded: humidifier level (byte 22), theoretical mask leak
(byte 10), and Comfort Control Plus level **and patient circuit** sharing byte 21.
**Only the heated tube is left.** It is confirmed only in the file the vendor
software writes, which is not something an ordinary card carries; byte 23 is a
good candidate for it here, on circumstantial grounds set out under "The Néa
Auto".

Three of those came from cards with a single setting deliberately changed. The
fourth, the circuit, came from driving *Sefam Analyze* against a card with no
device attached; the same exercise fixed the record's field order, which the
Rêve's own data can never settle — see "The settings record is the S.Box's
table".

**A second device on this platform, as of 2026-08-26.** A **Néa Auto** card
turned up and parses against this format unaltered. It carries a year of data on
a device whose settings were edited far more than the Rêve's, which closed the
practitioner/patient ramp question, exposed log code 13 as something other than a
settings record, and identified the therapy mode and the prescribed pressure —
the last two unassigned fields of any size in the record. See "The Néa Auto".

**Comparing OSCAR against the vendor software?** Read
`SEFAM_ANALYZE_VS_OSCAR.md` first. Several differences are structural — the AHI
definition above all — and it lists which ones will never close.

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
is `REVE_AUTO`, the model code `1279R`, and the serial. This was re-checked
across the whole sample collection — no SEFAM-family card, and no byte of either
memory image, contains "Sefam" or "Sanrai". Detection is unaffected (the model
code and `#03/` header are a solid fingerprint), but whatever brand OSCAR
displays is a naming choice rather than something read from the data, and users
will report this machine under either name.

**Decision (2026-08-10): OSCAR shows this device as a "Sanrai Rêve Auto".**
Since the brand cannot be read off the card, `sefamBrandName()` in
`sefam_loader.cpp` decides it per model: `Created By=REVE_AUTO` or model code
`1279R` gives brand `Sanrai`, and every other SEFAM-family device stays `Sefam`
until a card turns up that argues otherwise. The reasoning is that Sanrai is the
only name printed on the unit its owner actually has, so it is the name they
will look for. `resvent_loader.cpp` resolves the same Hoffrichter/Resvent
question the same way, per model.

Two consequences worth knowing:

- A **French-market Rêve, sold under SEFAM's own name, would also display as
  "Sanrai"** — the loader cannot tell them apart. The working hypothesis is that
  this case does not arise, because SEFAM's own equivalent is sold as the
  **Néa**: if `REVE_AUTO` only ever ships relabelled, "Sanrai" is right every
  time. Still unverified for the Rêve half — revisit the rule if a SEFAM-branded
  Rêve turns up. The other half is now settled: a **Néa Auto card confirmed that
  the Néa shares this platform** (2026-08-26), so the settings-menu section
  below, which borrows from a Néa manual on exactly that assumption, is on firm
  ground. See "The Néa Auto".

  A Néa card is recognised as of 2026-08-26 — by `NEA_AUTO` or model code
  `1265R` — and gets the "Sefam" brand and the Néa image. It is deliberately left
  off `sefamModelIsValidated()`, so importing one still raises the untested-device
  warning and prompts the user for a sample.

  **Evidence for the hypothesis, from the vendor software (2026-08-13).** Its
  manual lists the SEFAM range as S.Box, S.Box C, Néa Info, Néa Auto, Reve Info
  and Reve Auto, plus the bi-level S.Box Duo S/ST, Néa Duo S/ST and Ventea S/ST.
  So the **Néa is real, and is the Rêve's sibling** — an Info/Auto pair against an
  Info/Auto pair. More tellingly, the software's *create a new card* dialogue
  offers about four S.Box models and four Néa models and **does not offer the
  Rêve at all**, though it reads a Rêve card without complaint. That is what one
  would expect if the Néa is SEFAM's own-brand line and the Rêve ships relabelled
  for export. Suggestive, not proof.

  A real card arrived before that experiment was needed. The model code is
  `1265R` and the `Created By` string `NEA_AUTO`.
- `info.series` is **not** a brand field — it keys the device-image lookup in
  `MachineLoader::getPixmap()`. It stayed `"Sefam"` for one day, then became one
  value per device when the device photos were added (2026-08-10); see "Device
  images" below.

The brand is written to the `machines` row when the device record is first
created, and that row is never rewritten afterwards — `Machine::SaveToDatabase()`
refreshes series, model, model number and serial on an existing record but not
brand. A device imported before this change therefore keeps showing "Sefam";
re-importing into a fresh profile is the way to pick up the new name. That was
judged acceptable rather than worth a migration, since the loader has only ever
shipped in the v2.0.2 draft.

### Device images (added 2026-08-10)

**Built and confirmed rendering the same day** — both the Rêve and the S.Box show
their own photo.

Three photos live in `oscar/icons/` and are bound in through `Resources.qrc`:
`sanrai-reve.png`, `sefam-sbox.png`, `sefam-nea.png`. All lower case, matching
the convention of every other icon in that directory — the filenames must match
the `.qrc` entries exactly or the images vanish on case-sensitive filesystems,
which is most of the platforms OSCAR ships to.

`MachineLoader::getPixmap()` keys on `MachineInfo::series`, so each device that
needs its own image needs its own series value. `series` is an internal key —
nothing in the UI displays it, and its only other reader is a ResMed-specific
S9 check in `welcome.cpp` — so `sefam_loader.cpp` now assigns one per device
(`kSeriesReve`, `kSeriesSBox`, `kSeriesNea`). Both the registration and the
assignment read the same constants, which is deliberate: the AirSense 11 icon
was once invisible because `PeekInfo()` produced `"AirSense11"` while the icon
hash was keyed `"AirSense 11"` (`BUG_FIXES.md`, 3-part ResMed icon entry).

Unlike brand, **series does propagate to an existing `machines` row** —
`Machine::SaveToDatabase()` includes it in the fields it refreshes — so an
already-imported device picks up its image on the next import even though it
keeps its old brand.

**The Néa image was registered before any Néa card existed** and was unreachable
until 2026-08-26, when `sefamModelOf()` learned to recognise one. An
unrecognised SEFAM model still keeps series `"Sefam"`, matches no entry, and
falls through to the generic CPAP image — using the Néa photo as the SEFAM
default was considered and rejected, since it would put a photo of a specific
device against whatever turned up.

Worth recording: **the Néa and Rêve product photos show the same physical
machine** — same wedge case, same touchscreen layout, same side port, differing
only in the printed logo. That was taken as supporting evidence at the time; the
first Néa card then confirmed it at the format level.

**Loader:** `sefam_loader.cpp` / `sefamDataParsing.cpp`, added 2026-08-03 from
this analysis. Design: `Notes/loaders/SEFAM_LOADER_DESIGN.md`. **Awaiting testing
by a real user** — everything below was verified against this one card and the
manufacturer's report for it.

Related: `SD_CARD_FINGERPRINTS.md` has an earlier entry for the **SEFAM S.Box
AUTO** (model code `1263R`, firmware `VER :A020400`) — same firmware platform,
different device class. That entry's guess that the obfuscation key is `0x9F` is
**wrong**; see below. Its "~100 byte header" estimate is also wrong (it is 71).

The S.Box is now documented in its own right in
`Notes/loaders/SEFAM_SBOX_CARD_ANALYSIS.md`. It differs from this device more
than the shared platform suggests: no `.LOG` at all, the short 38-byte header
with no UTC epoch, 25 Hz instead of 10 Hz on four channels, and an unencrypted
memory image. **Do not carry a conclusion from one model to the other without
re-testing it.**

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
- **sequence** = 1-based, strictly incremental — **but it can skip** (see below).
- Record sizes: 10 Hz → 103 B, 5 Hz → 53 B, 1 Hz → 13 B.
  (`.PLS` is 75 Hz / 16-bit → 1503 B, unexercised in this sample.)

`file_size == header + N * record_size` holds exactly for all 31 sessions ×
6 populated channels — the layout is confirmed, not inferred.

Session duration = `N × 10` seconds. Sample rates come from the `.INI`, so the
reader can be fully data-driven.

### The sequence counter skips, and a skip is not corruption

*Added 2026-08-26.*

The device sometimes **drops a record and keeps counting**: the sequence runs
`… 17, 18, 20, 21 …` and continues cleanly to the end of the file. Every checksum
still passes, before and after the hole, and `file_size == header + N × record_size`
still holds — the file is intact and one record's worth of samples was never
written.

Seen on two of the three sample cards, always as a single missing record, always
at the same position in every channel of the session:

| card | sessions affected | records past the skip |
|---|---|---|
| Néa Auto | 3 of 146 | 811, 128, 335 |
| S.Box `1200R` | 3 of 22 | 771, 718, 733 — those files begin at sequence **2** |
| Rêve Auto | none | — |

`readChannel()` originally treated a sequence mismatch the same as a checksum
failure and truncated the channel there. That discarded ~3.5 h on the Néa card,
and made those three S.Box sessions import as nothing at all — every channel
truncated to zero records, so `readSession()` rejected the session with "no
populated channels". Roughly 6.2 h on a real user's card, silently.

The reader now fills each missing record with `kInvalidSample` and carries on,
so later samples keep their true offset from the session start and the hole
renders as an ordinary drop-out. A checksum failure still truncates, and so does
a sequence that goes backwards or jumps further than the file has left to say.

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

> **This is measured correctly but reasoned from too narrowly.** A ±3 s window
> cannot see a constant offset larger than itself, and there is one: widening the
> search to ±60 s puts bit 3 at a sharp −28 s from every snore record (the modal
> offset in 28 of 30 sessions) and bit 0 at the implied start of 88 % of
> flow-limitation records. So `Y17` and the `.LOG` are *not* independent for at
> least two event types. The conclusion that `Y17` cannot supply events still
> holds — it over-counts both by 1.5–2× and tracks apnoeas and hypopnoeas poorly
> — but "does not line up" is the wrong reason. See the `Y17` section of
> `SEFAM_SBOX_CARD_ANALYSIS.md`.

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

**Code 2** payload `40 45 45 31 200 40 60 60 130 200 121 4 6 3 0`, with the
record's own bytes 9–10 (`80 164`) belonging to it as well — see
"The settings record is the S.Box's table" below for why those two count:

| Byte | Value | Meaning |
|---|---|---|
| 9 | 80 | **Prescribed pressure** ×0.1 cmH₂O; 80 = not set — see "The Néa Auto" |
| 10 | 164 | **Theoretical mask leak, lpm in bits 0–6** — `0x80 \| 36` — confirmed by a controlled change |
| 11 | 40 | **Ramp start pressure 4.0** (×0.1 cmH₂O) |
| 12 | 45 | **the patient's own ramp time**, minutes |
| 13 | 45 | **the practitioner's ceiling on it**, minutes |
| 14 | 31 | ramp mode and flags: bit 2 = I.Ramp, bit 4 = patient access lock |
| 15 | 200 | **Max pressure 20.0** (×0.1 cmH₂O) |
| 16 | 40 | **Min pressure 4.0** (×0.1 cmH₂O) |
| 17 | 60 | **Therapy mode**: 60 = A-PAP, 0 = CPAP — see "The Néa Auto" |
| 18 | 60 | constant 60 on all three models |
| 19 | 130 | **apnoea response pressure 13.0 cmH₂O** (named by the S.Box vendor report) |
| 20 | 200 | unassigned |
| 21 | 121 | **CC+ level in bits 7–6**, level = field + 1 — confirmed by a controlled change |
| 22 | 4 | **Humidifier level** — confirmed by a controlled change, see below |
| 23 | 6 | heated tube level? — candidate, see "The Néa Auto" |
| 24, 25 | 3, 0 | unassigned. Byte 24 reads 3 on both models after setup |

> **Bytes 11 and 16 are not in the order this note first gave them.** Both read
> 40 on every Rêve card, because this device's minimum pressure and ramp start
> pressure are both 4.0, so the Rêve's own data cannot tell them apart. The S.Box
> table — six vendor-printed snapshots in which the two differ — fixes the order,
> and it is ramp start first. Same for bytes 12 and 13, both 45 here.
>
> **Fixed in the loader** (`sefamDataParsing.cpp` `parseSettings`): ramp start is
> payload index 0 and minimum pressure index 5. Bytes 12 and 13 are not an echo
> either — see "The Néa Auto" for what separates them and which one the loader
> reports.

Code 2 occurs 5 times. The report's `setting change` column is non-zero on
exactly the two sessions carrying multiple code-2 records (counts 1 and 2,
matching), and **byte 23 is the only byte that differs** between the pair on the
session reporting two changes — so byte 23 is the field that was edited.

Byte 23 changes 6 → 5 → 6 within one session across 60 seconds. This note once
took that as ruling byte 23 out of being any printed setting, on the grounds that
the analyzer prints the comfort, humidifier and heated-tube settings as identical
on every session row. **That argument does not hold**: the analyzer prints the
*latest* settings on every row regardless — the humidifier level demonstrably
varies on this card and its report shows one value throughout. Byte 23 is now a
candidate for the heated tube level; see "The Néa Auto".

> **Code 13 is not a settings record**, and this section originally said it was.
> The correction and the evidence are under "The Néa Auto". What follows is the
> original reading, kept because the byte positions are still right.

**Code 13** payload `[0/1] 0 3 40 100 200 0 [5–10] [0/1] 51 55 50 57 55 52`
carries 40, 100 and 200 at bytes 14, 15 and 16 — read at the time as this
device's minimum and maximum pressure, which they coincidentally equal — plus a
mode-like `3` at byte 13 and a varying 5–10 counter at byte 18. Bytes 20–25 are a
constant six-byte tail.

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

### Theoretical mask leak — byte 10, CONFIRMED by a controlled change

A third card was pulled from the same device with the **theoretical mask leak
changed from 36 to 34 lpm** and the Comfort Control Plus level from 2 to 3. It
settles both fields and refutes the previous reading of the mask leak outright.

The field is **byte 10, holding the value in lpm in its low seven bits**:

| | byte 10 | bits 0–6 | analyzer / tester |
|---|---|---|---|
| every record before the change | `0xA4` | 36 | report prints **36 lpm** |
| the record after it | `0x22` | 34 | tester set **34 lpm** |

Two points, exact, on a scale with no fitted constant — and the second was
**pre-registered**. The prediction table written before the card existed listed
"reads 34" as the outcome that would mean the setting is stored in literal lpm.
It does; the table only had the wrong byte. The value fits in seven bits with room
to spare, and 164 is outside any plausible leak, so bit 7 has to be a separate
flag.

> **The real value range is 18–60, not the manual's 20–60.** *Sefam Analyze*
> ships its mask database as `masks.txt` — `maker;type;model;leak`, 89 masks from
> eleven manufacturers — and a clinician sets this field either by typing a
> number or by picking a model from that list, which writes the table's lpm
> figure straight into the setting. The table spans **18 to 60**, and it holds
> two masks at **41**, so the Nea manual's "20 to 60 in steps of 2" describes the
> manual-entry menu rather than what a selected mask can supply. The loader's
> sanity bounds (`kMaskLeakMinLpm` / `kMaskLeakMaxLpm`) follow the table.

> **Why the field went unseen for so long.** Byte 10 is the low half of the
> 16-bit "argument" that every log record carries at offsets 9–10. For apnoea
> records that argument is a duration, so it was read as one field and excluded
> from the settings payload, which was taken to start at byte 11. For code 2 the
> argument is not an argument — it is two more settings bytes. The constant
> `arg = 20644` printed on every settings record was `0x50A4`, and its low byte
> was the mask leak all along.

**Bytes 17 and 18 are not the mask leak.** They read 60 before and 60 after,
across a change that moved the setting by two steps. The reading that made them
36.0 lpm at the leak channel's own 0.6 lpm per count — 60 × 0.6 — was arithmetic
that happened to land on the printed figure. The S.Box confirms it independently:
its settings table holds the same constant 60, 60 on a different device with a
different mask, where it cannot be that device's mask leak either.

**Do not retry any of these.** All three were live candidates before the third
card and all three are now dead: `round(lpm / 0.6)` and `lpm + 24` and
`2·lpm − 12` all predicted bytes 17/18 would move to 56–58, and neither byte
moved at all.

The vent-curve fit recorded against the second card — binning the 5th-percentile
leak by pressure over 749,448 paired samples, giving leak ≈ 9.94·√P lpm and so
34.4 lpm at the manual's reference 12 cmH₂O against a 36 lpm nameplate — was
sound physics and is left standing, but it tested the mask, not the byte map. It
never had the power to locate a field, and locating the field did not need it.

**Bit 7 of byte 10 is unexplained.** It was set on every record while the leak
read 36 and cleared on the record where it reads 34. See "Two bits cleared
together" below.

### Comfort Control Plus — byte 21 bits 7–6, CONFIRMED by a controlled change

The same card raised CC+ from level 2 to level 3, and byte 21 is the only other
settings byte that moved:

| | byte 21 | bits 7–6 | bits 5–0 | CC+ level |
|---|---|---|---|---|
| before | `0x79` | 1 | `0x39` | 2 |
| after | `0xB9` | 2 | `0x39` | 3 |

So **level = field + 1**, pinned by two points exactly as the humidifier byte
was, with room in two bits for levels 1–4 — of which the vendor manual documents
1, 2 and 3.

**Byte 21 turned out to carry a second setting**, found later on the S.Box by
driving *Sefam Analyze* with no device attached (see that note). The full field:

| bits | meaning |
|---|---|
| 7–6 | CC+ level − 1 |
| 5–3 | set when CC+ is enabled; all clear when it is off — see the caveat below |
| 0 | patient circuit: set = 15 mm, clear = 22 mm |

`0x79` therefore reads as CC+ level 2 **and 15 mm**, and this card's report prints
exactly that pair. Writes of CC+ off, 1 and 3 produced `0x00`, `0x38` and `0xB8`,
which is where the enable bits and the level field come from; a separate pair of
writes moved bit 0 alone between the two diameters.

Decoded across every settings record on every card held — 478 in total, 467 S.Box
archive blocks and 11 Rêve code 2 records — the level never falls outside the
documented 1–3, and both cards that have a vendor report match it on **both**
settings at once. Circuit Select, the vendor's third diameter choice, cannot be
expressed by a single bit and would read as 22 mm; no card has exercised it.

> **Bits 5–3 are read as an enable, and that is not fully proven.** Bit 7 moved in
> the CC+ level writes *and* in the patient-access-lock writes, so those two
> experiments cannot separate "bits 5–3 mark CC+ enabled" from other readings of
> the same byte. Nothing shipped depends on the difference — on all 478 real
> records from three devices the bits are set and the level agrees with every
> vendor report — but a card with CC+ genuinely switched off has never been seen,
> and that is the case the reading is guessing at. Byte 21 held 121 on all eleven
settings records spanning a month — through three humidifier changes and the two
isolated nights when byte 14 dropped to 29 — and moved once, on the record where
CC+ changed.

**Byte 14 cannot be the level even though it moved at the same moment.** It went
`0x1F` → `0x0F`, clearing bit 4. Read as any field that contains bit 4, the value
*decreases*: bits 3–4 read 3 → 1, bits 4–5 read 1 → 0. The level increased. No
reading of byte 14 rises by one step here, and byte 21's does.

**The change is visible in the pressure trace, which is what makes it more than a
byte correlation.** CC+ is expiratory relief, so raising it must deepen the drop
from inspiratory to expiratory mask pressure. Measured breath by breath — using
`DET` bit 5 for the phase, so the slowly-walking APAP setpoint cannot influence
it — over the 34 full nights on the card:

| | 34 nights before | the night after |
|---|---|---|
| median inspiration → expiration pressure swing | 0.345 – 0.535 cmH₂O | **0.707** |
| median inspiratory flow amplitude | 23.1 – 29.3 L/min | 27.3 |
| median total leak | 22.2 – 27.6 L/min | 22.8 |
| swing ÷ flow amplitude | 1.355 – 1.827 | **2.587** |

The swing lands 32 % above the highest of 34 nights, and 42 % above the highest
ratio, while the flow amplitude and the leak sit mid-range and match the
immediately preceding night almost exactly (27.0 L/min and 22.8 L/min). **So the
circuit did not change and the patient's breathing did not change; only the
pressure the device delivered against that breathing did.** A mask swap — the
obvious confound, given the tester also changed the configured mask leak — would
have moved the vent leak and the flow amplitude together with the swing. It moved
neither. One CC+ level is worth roughly **0.26 cmH₂O** of extra expiratory relief
on this device.

> **The change night is the one night OSCAR will not show the new value**, and
> that is a display quirk rather than a decode problem. Both sessions of that
> night start before midnight, so they land on the same OSCAR day: a 6-minute
> session recorded while the menu was still open, carrying the *old* settings,
> and then the 7.5 h night carrying the new ones. `daily.cpp` sources its Device
> Settings panel from `Day::firstSession()`, which returns the first entry in the
> session list rather than the longest or the latest, so the panel shows the
> six-minute session's CC+ 2 and 36 lpm. The loader stores both sessions'
> settings correctly and the following night displays the new values.
>
> Left alone deliberately (user's call, 2026-08-11): `daily.cpp` is shared by
> every loader and this only bites on a night when settings change between two
> sessions. Worth knowing before treating it as a SEFAM bug.

### The settings record is the S.Box's table, one byte per field

The Rêve's code 2 payload and the S.Box's 12-field settings record
(`SEFAM_SBOX_CARD_ANALYSIS.md`) are **the same structure**. The S.Box stores each
field as a uint16 little-endian and the Rêve as a single byte, and the field
order is identical.

The alignment is read against the S.Box's **archive block header**, not its
settings table, because the block header carries the record already rotated —
`[word 10, word 11, word 0 … word 9]` — and that rotation is exactly the Rêve's
byte order. Verified directly on all 467 blocks of one S.Box card:

| position | S.Box block word | Rêve byte | Field | S.Box | Rêve |
|---|---|---|---|---|---|
| 0 | 29 | 9 | per-device, unassigned | 50 | 80 |
| 1 | 30 | **10** | **theoretical mask leak, lpm in bits 0–6** | 164 | 164 → 34 |
| 2 | 31 | 11 | ramp start pressure ×10 | 80 | 40 |
| 3 | 32 | 12 | ramp duration echo | 15 | 45 |
| 4 | 33 | 13 | ramp duration, minutes | 15 | 45 |
| 5 | 34 | 14 | ramp mode + flags | 9 | 31 → 15 |
| 6 | 35 | 15 | maximum pressure ×10 | 190 | 200 |
| 7 | 36 | 16 | minimum pressure ×10 | 120 | 40 |
| 8 | 37 | 17 | constant | 60 | 60 |
| 9 | 38 | 18 | constant | 60 | 60 |
| 10 | 39 | 19 | apnoea response pressure ×10 | 130 | 130 |
| 11 | 40 | **21** | **CC+ level and patient circuit, packed** | 184 | 121 → 185 |

Rêve byte 20 has no S.Box counterpart and is unassigned; see the note below.

Twelve consecutive positions line up. Four carry identical values on the two
models — the 60, 60, 130 run and the 164 — and the rest each hold that model's
own correct setting: the Rêve's ramp start 4.0, ramp 45 min, maximum 20.0 and
minimum 4.0 all match its vendor report under this alignment, as the S.Box's
8.0 / 15 min / 19.0 / 12.0 match its own.

**The Rêve's record continues past the twelve and the S.Box's does not.** Bytes
21–25 are Rêve-only, and two of them are known: byte 21 is the CC+ level and
byte 22 the humidifier level, both confirmed by controlled changes. **So the
S.Box's word 10 is not the Rêve's CC+ byte** — the position that lines up with
word 10 is the Rêve's byte 9, which is unassigned on both models. Anyone carrying
the Rêve's `bits 7–6` reading across to that word will be wrong.

The S.Box does have both settings — its vendor report prints `Humidifier Level 2`
and `Comfort Control Plus Level 3` — so they are stored somewhere on that model
too, just not where this record puts them. Word 10 is the surviving candidate
there; see the S.Box note. *(An earlier revision of this section said the S.Box
"appears to have no humidifier". That was a guess from the record's shape and the
report contradicts it.)*

> **Byte 21 is the S.Box's word 9, and byte 20 is a Rêve-only insertion.** The
> field-by-field alignment above runs true through byte 19, then the Rêve carries
> one extra byte — byte 20, reading 200 on every record, which is also this
> card's maximum pressure and may simply be a repeat of it. Byte 21 is what
> corresponds to the S.Box's word 9.
>
> This was got wrong once. An earlier revision counted byte 20 as word 9, found
> its bit 0 clear where the Rêve's report says 15 mm, and recorded the circuit as
> "does not transfer, unresolved". The mistake was the off-by-one, not the
> encoding: byte 21 decodes to 15 mm and matches.

Two things follow that the Rêve's data could never have given on its own:

1. **The field order is settled.** The Rêve has minimum pressure equal to its
   ramp start pressure (both 4.0) and its ramp duration equal to its ramp echo
   (both 45), so its own records cannot separate either pair. The S.Box's do,
   across six vendor-printed snapshots in which minimum, maximum, ramp duration,
   ramp mode and ramp start pressure all vary.
2. **Byte 19 = 130 is a pressure, not a bit-field.** The S.Box vendor report
   names it: apnoea response pressure, 13.0 cmH₂O. The `0x82` bit-field reading —
   `0x80` tube present, `0x02` CC+ level 2 — is dead, and so is the byte 19
   reference-pressure idea already rejected above. It was a plain number all
   along.

> **The obvious alignment is two positions off, and it very nearly stuck.**
> Lining the Rêve's byte 11 up with the S.Box's *table* word 0 also matches the
> 60, 60, 130 run and every pressure, because those sit at the same offset either
> way. It fails only at the ends: it puts the mask leak in the previous slot's
> last word, and it leaves the Rêve's humidifier byte sitting where the table's
> mask leak would be. The block header settles it, since there the record is
> stored in the Rêve's own order with nothing before or after to borrow from.

### Byte 14 bit 4 is the patient access lock

Byte 14 holds the ramp mode plus flags, and its bit 4 is the **patient access
lock** — set means the patient cannot change the ramp, Comfort Control Plus or
Intelligent Start.

Established on the S.Box, where the vendor software can toggle it directly: two
`upload.dat` writes differing only in the "Editable by the patient" checkbox
moved that model's ramp-mode byte from 8 to 24, which is bit 4 alone. The single
checkbox governs all three settings at once, so the lock is one flag rather than
the three the vendor manual's wording suggests.

It explains two values that had no explanation before:

- **The Rêve's own byte 14.** It reads 31 (`0x1F`, bit 4 set — locked) on every
  record for a month, then 15 (`0x0F`, bit 4 clear — unlocked) on the last one,
  which was written during the session where the clinical menu was being used.
  Both values are the same ramp mode with the lock differing.
- **The S.Box's "otherwise unseen mode value of 28"**, which appears only in one
  card's factory slot. 28 is 12 (I.Ramp) with bit 4 set. Not a mode at all.

> **A superseded reading, recorded so it is not retried.** This section
> previously offered a *factory-defaults or not-yet-modified-by-a-clinician*
> marker, on the grounds that the bit survived a month of user-menu humidifier
> changes and cleared on the first clinical-menu edit. That story fitted the
> observations but was untestable on this device, and it is wrong: the S.Box
> exercises the same bit in both directions on demand.

**Byte 10 bit 7 is still unexplained.** It was set while the leak read 36 and
cleared when it read 34, on the same record as the lock change, and nothing since
has moved it. It is the last unattributed bit in the Rêve's settings record.


### What is left, and why the last one is hard

*(Written against the first card, when all five accessory settings were open.
Four have since been pinned. The argument below now applies only to the heated
tube.)*

The analyzer's full device-settings panel for the first card reads:

| Setting | Value | Status |
|---|---|---|
| Comfort Control Plus | Level 2 | **byte 21 bits 7–6**, confirmed |
| Theoretical mask leak | 36 lpm | **byte 10 bits 0–6**, confirmed |
| Humidifier | Level 4 | **byte 22**, confirmed |
| Patient Circuit | 15 mm | **byte 21 bit 0**, confirmed on the S.Box and agreeing here |
| Heated tube | Present | open — **byte 23** is a candidate, see "The Néa Auto" |

**The survivor is reported here as a two-state setting that has never varied**,
so there is nothing on this card to correlate it against. The vendor software
stores it as a level rather than a flag, which is what makes byte 23 plausible.
The unassigned candidates are bytes 20, 23, 24 and byte 14's bits 0 and 1;
byte 9 has since been shown to be a settable pressure, not a spare.

1. **The one discriminating session is unusable.** The analyzer reports
   `Heated tube = Missing` on exactly one session of 31 — and that session's
   `.LOG` contains **no code 2 and no code 13 record at all**. Its whole log is
   codes 28, 9, 10 and 7, every one of them zero-payload, and those codes also
   appear in 8–22 other sessions where the tube reads `Present`. So the single
   place where a setting differs carries no settings record to compare.

2. **`Patient Circuit` and `Theoretical mask leak` do not appear in the
   analyzer's CSV export at all** — only CC+, humidifier and heated tube do.
   That was once read as evidence they are not stored on the card, and the mask
   leak has since been found sitting in byte 10. Absence from the CSV says
   something about the analyzer's export, not about the card, so the same
   inference should not be drawn about the patient circuit either.

**Searching for the printed value turned out to be the right instinct after
all** — but only once the search covered the whole record. The humidifier stores
its level literally in byte 22 and the mask leak stores its lpm literally in
byte 10; what hid the second one for two cards was not a scale, it was reading
bytes 9–10 as a log argument and never looking there. An earlier revision of this
note drew the opposite lesson and told the reader to stop asking "does this byte
equal the printed number". That advice was wrong, and it is what cost the time.

**Nothing should be written into the loader for the last two until a card varies
one of them.** Importing a setting off a lone byte-value match risks showing a
user a setting their device does not have. The method that settled the other
three is unchanged: one night from the *same* device with a **single** setting
changed. It has now worked three times out of three.

> **On changing two settings at once.** The third card changed the mask leak and
> CC+ together, against this note's own advice, and it still resolved — but only
> because both fields moved in ways their own values identified, and it left two
> stray bits that cannot be attributed to either change. That is exactly the cost
> the warning predicted. One setting at a time remains the rule.

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
   ~~**Superseded.** Read in the leak channel's own 0.6 lpm counts, 60 counts
   *is* 36.0 lpm.~~ **Both readings were wrong.** The mask leak is byte 10, in
   plain lpm; bytes 17 and 18 are an unrelated constant that the S.Box holds too.
   The documented 20–60 lpm range in steps of 2 is what byte 10 actually carries.
5. ~~**Byte 21 = 121 → 12.1 cmH₂O** is a candidate for the CPAP Pressure Level.~~
   **Dead.** Byte 21 is the CC+ level in bits 7–6; its low six bits held `0x39`
   across a change of level and are not a pressure. The device has no fixed
   pressure to store in A-PAP anyway, which is why the analyzer prints
   `Prescribed pressure = -`.
6. **Ramp type has three states and mode two**, so both are small enums. Byte 14
   carries the ramp mode — the S.Box uses 8 for T.Ramp and 12 for I.Ramp in the
   same field — so byte 24 = 3 is no longer needed to explain it.
7. ~~**Patient access lock covers three toggles**, so a small bitmask exists
   somewhere.~~ **RESOLVED: byte 14 bit 4**, and it is **one** toggle, not three
   — the vendor software's single checkbox governs the ramp, CC+ and Intelligent
   Start together. See "Byte 14 bit 4 is the patient access lock". Byte 14's bit 1,
   which drops on two isolated nights, is still unexplained.
8. **Intelligent Start has never been seen in any card data.**
9. **CC+ appears as a toggle in the clinical menu and as a level in the user
   menu**, which reads at first like a contradiction with the analyzer's
   "Level 2". It is not, and the level half is now confirmed in byte 21 bits 7–6.
   The clinician enable, if it is stored separately at all, has not been found —
   ~~the `0x82` byte 19 reading~~ is dead, byte 19 being the apnoea response
   pressure. A clinician enable plus a patient-selectable strength remains the
   right model; only the enable is still missing.

## The Néa Auto — a second device on this platform

*Added 2026-08-26, from the first Néa card seen.*

**Identity:** `Created By=NEA_AUTO`, model code `1265R`, firmware `VER :A010400`.
Card layout `<root>/1265R/<serial>/DATA_nnn/`, three-digit directory numbers.

**It is the Rêve's format, not merely a similar one.** Same `#03/` tag, same
71-byte channel headers with the same 12 declared channels at the same rates,
same 49-byte log records, same XOR 0xBF on headers only, and a `.RAM` of exactly
the Rêve's size (1 633 228 bytes) that is likewise encrypted — the archive marker
the S.Box uses does not appear at its offset. All 146 sessions of a card spanning
2025-09-18 to 2026-08-24 parse against the existing code with nothing changed:
every channel payload is a whole number of records, no header fails, no session
is a stub, and every session id is unique and in order. Three sessions truncate
on a checksum or sequence mismatch, which the loader already handles.

The Néa is worth far more than a second confirmation, because this device was
**edited constantly** where the Rêve's barely moved: 31 settings records against
the Rêve's 11, a clinician setup visible in the middle of them, and a patient who
changed comfort settings most nights.

### The practitioner and patient ramp times — RESOLVED

**Byte 12 is the patient's own ramp time. Byte 13 is the practitioner's ceiling
on it.** They are not an echo of one another.

This was the last open question about the settings record, and it was
unanswerable from the two earlier cards: the Rêve holds 45 in both, and the
S.Box's settings table holds them equal on every T.Ramp slot and the patient's at
0 on every I.Ramp slot — exactly what an echo would look like.

The Néa separates them. Its practitioner value sat at 30 minutes for the whole
year while the patient's moved across five separate records:

| date | byte 12 (patient) | byte 13 (ceiling) | byte 14 |
|---|---|---|---|
| factory | 45 | 45 | 29 — I.Ramp, locked |
| after clinician setup | 30 | 30 | 25 — T.Ramp, locked |
| next day | 10 | 30 | 25 |
| five days later | **0** | 30 | 25 |
| next day | 20 | 30 | 25 |
| three days later | 10 | 30 | 25 |

The reverse assignment is impossible: the vendor software will not let the
patient's value exceed the practitioner's, so a ceiling of 0 with a patient
setting of 30 cannot occur.

**The S.Box archive says the same thing, and always did.** The earlier argument
was made against that device's settings *table*; its session archive was never
checked for this. Over all 467 blocks of one S.Box card the patient's value never
exceeds the ceiling, and falls strictly below it in five distinct T.Ramp states
(0/5, 0/20, 5/10, 5/20, 10/20). An echo cannot do that.

**Ramp mode bit 2 = I.Ramp.** The S.Box's table stores 12 where its report prints
I.Ramp and 8 where it prints T.Ramp; the Rêve reads 31 with a report that prints
I.Ramp; the Néa reads 29 (I.Ramp) in its factory record and 25, 11 and 9 (T.Ramp)
afterwards. Bit 3 is set on every value ever seen, and bits 0 and 1 are still
unattributed — bit 1 in particular flips back and forth night to night on both
the Rêve and the Néa without the ramp times moving.

**What the loader reports:** the patient's value, except under I.Ramp, where the
patient chooses only on or off and has no duration of their own — there the
practitioner's is the only answer. That rule reproduces every ramp figure printed
on both manufacturer reports held, so it changes nothing on a Rêve or an S.Box
card; on the Néa it corrects 27 of 146 sessions. It is
`SefamParsing::rampMinutesFor()`.

### Log code 13 is not a settings record — CORRECTED

Code 13 was read as a settings snapshot and used as a fallback wherever a session
carried no code 2. It is not one. Its three pressure bytes read **40, 100, 200**
in every code 13 record on both cards — including Néa records written months
after that device's prescription had been narrowed to 6.0–7.5:

| | code 13 bytes 14/15/16 | actual prescription then |
|---|---|---|
| Rêve, all 13 records | 40, 100, 200 | min 4.0, max 20.0 |
| Néa, records to 2025-09 | 40, 100, 200 | min 4.0, max 20.0 |
| Néa, records from 2026-05 | 40, 100, 200 | **min 6.0, max 7.5** |

They are the model's settable range and its default — 4.0, 10.0, 20.0 — not what
the device was set to. The record also carries a six-digit ASCII number that is
constant per device and differs between the two.

The reading survived this long because the Rêve's own prescription *is* 4.0–20.0,
so the fallback happened to produce the right answer there. On the Néa two
sessions carry a code 13 record and no code 2, and reported the full pressure
range instead of the real one — and left that behind as the carried-forward
settings for every session after them. The fallback is gone; a session with no
settings record of its own now keeps the previous session's, which is what the
device's write-on-change behaviour implies.

### The therapy mode is byte 17, and byte 9 is the prescribed pressure — CONFIRMED

*Settled 2026-08-26 against the vendor software's per-day settings table.*

These were the last two unassigned fields of any size in the settings record, and
they belong together.

| byte | meaning | A-PAP | CPAP |
|---|---|---|---|
| 17 | **therapy mode** | 60 | 0 |
| 9 | **prescribed pressure**, ×0.1 cmH₂O | 80 = not set | the pressure |

Both were dismissed for a long time on the strength of the two devices first
examined, where neither ever moves: byte 17 was written down as "constant 60" and
byte 9 as "per-device, unassigned".

**What settles it.** *Sefam Analyze* has a per-day settings table with a **Mode**
column and a **Prescribed pressure** column — the latter also appears in its CSV
export, between Mode and Min. pressure. Across the three cards held:

| | byte 17 | byte 9 | SA Mode | SA Prescribed pressure |
|---|---|---|---|---|
| Rêve, every record | 60 | 80 | A-PAP | `-` |
| Néa factory record | 60 | 80 | A-PAP | *(blank)* |
| Néa, every record after setup | **0** | **70** | **CPAP** | **7.0** |

**And the S.Box proves it from measurement alone**, without reference to any
vendor output. That model records a mean pressure for every minute of every
session, so the two groups can simply be compared:

| byte 17 | blocks | delivered p10–p90 spread | median == byte 9 |
|---|---|---|---|
| **0** | 5 | **0.10 cmH₂O** | **5 of 5** |
| 60 | 225 | 3.17 cmH₂O | 4 of 225 |

Flat and sitting exactly on the prescribed pressure, against titrating across
three cmH₂O. Over the whole archive 11 of 467 blocks read CPAP.

This also explains the observation that first drew attention to byte 9: the Néa's
delivered pressure sat dead flat at 7.0 for eight months — p05 6.5, p50 7.0,
p99 7.3 across 185 000 samples in one night — while its minimum and maximum read
4.0 and 20.0. It was not an auto device pinned at one pressure. It was a CPAP,
and the band it was not using was still stored beside the pressure it was.

**The loader consequence.** `MODE_APAP` was hard-coded, so every SEFAM session
that ran fixed was reported as an auto band the device never titrated across:
**145 of the 146 sessions on the Néa card**, and 11 blocks on the S.Box. Both
paths now read the mode and publish either `CPAP_Pressure` or the band, never
both — see `setPressureMode()` in `sefam_loader.cpp`.

> **Superseded, recorded so it is not retried.** This section previously argued
> that the mode was *not* in the settings record, on the grounds that no field
> moved at the boundary where the Néa's prescription changed. That reasoning was
> sound but the boundary was the wrong one to look at: the mode changed at the
> **clinician setup** in September, not at the pressure change the following May,
> and byte 17 moved exactly there. It also proposed writing a mode change through
> *Sefam Analyze* to identify the byte. That experiment is no longer needed.

### Two bytes that were called constant

*(Byte 17, the third, turned out to be the therapy mode — see above. Byte 18 is
still 60 on every record of every card.)*

- **Byte 23** — see below.
- **Byte 24.** Reads 3 on the Rêve and on the Néa after setup, 7 in the Néa's
  factory record. Settable, unassigned.

### Byte 23 — the heated tube level, probably

Not confirmed, and no controlled change can reach it: the only model whose
settings the vendor software can write is the S.Box, and the S.Box's settings
record has no such byte. The case for it is circumstantial but consistent:

- **Position.** In the file *Sefam Analyze* writes, the heated tube byte sits
  immediately after the humidifier byte. Byte 23 sits immediately after byte 22,
  which is the confirmed humidifier level.
- **Range.** The vendor's heated tube setting is off, levels 1–5, or Auto — 0 to
  6. Every value ever seen in byte 23 is inside it: 5 and 6 on the Rêve, 2 to 6
  on the Néa.
- **Behaviour.** It moves independently of the humidifier, night to night, on
  both devices — which is what a patient-adjustable comfort setting does. Both
  devices have a heated tube; the Rêve's report prints "Heated tube: Present".

Not imported. An unverified byte shown as a therapy setting is worse than showing
nothing. What would confirm it is a manufacturer report for a Rêve or Néa card
that prints a heated tube *level* rather than its presence.

### What the loader does with a Néa card

Recognised by `NEA_AUTO` or model code `1265R`, displayed as "Néa Auto", brand
"Sefam", with the Néa device image that was already registered. It takes the
Rêve's import path unchanged.

It is **deliberately not** on `sefamModelIsValidated()`'s list, so importing one
still raises the untested-device notice. No manufacturer report exists for a Néa,
and the therapy mode question above is unresolved.


## `.RAM` / `.BKP` — encrypted, not readable **on this model**

The S.Box writes the same two files **unencrypted**, and its therapy settings
turned out to live in them as a 12-slot table of 24-byte records — see
`SEFAM_SBOX_CARD_ANALYSIS.md`. That table is not present in the Rêve's images
(searched for its known minimum/maximum pressure pair in every plausible width
and order), which is consistent with the encryption below rather than with the
structure being absent. Nothing here contradicts what follows; it just means the
files are worth attacking, not ignoring.

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
| Therapy settings | min/max pressure, ramp time and ramp pressure all recovered from log code 2 (field *order* within the record was later fixed against the S.Box table) |
| Mask-disconnect seconds | `LK`==255 matches exactly where disconnects are long (412 s session: 412 decoded) |

The one session where mean pressure differs (−1.08 cmH₂O) is the 20-minute
session that was 23 % mask-disconnected — the analyzer excludes disconnected
time from the average, so the disagreement is expected and explains itself.

Three further fields were confirmed by a different method: later cards from the
same device with a known setting changed and no analyzer output to check against,
so the confirmation rests on the controlled change rather than on a printed
report.

| Field | Byte | Change | Result |
|---|---|---|---|
| Humidifier level | 22 | 4 → 5 | one byte moved, 4 → 5 |
| Theoretical mask leak | 10, bits 0–6 | 36 → 34 lpm | 36 → 34, and bytes 17/18 refuted |
| Comfort Control Plus | 21, bits 7–6 | level 2 → 3 | field 1 → 2, and the pressure trace agrees |

The CC+ result carries an independent physical check that the other two do not:
the measured expiratory pressure relief rose past the range of all 34 preceding
nights while flow amplitude and leak stayed put. See its section.

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
4. **Comfort and accessory setting bytes** in code 2. Status of the five:
   - ~~Humidifier level~~ **RESOLVED: byte 22**, confirmed by a controlled
     single-setting change.
   - ~~Theoretical mask leak~~ **RESOLVED: byte 10, bits 0–6, in plain lpm**,
     confirmed by a controlled 36 → 34 change. The bytes 17/18 candidacy is
     **refuted** — neither byte moved.
   - ~~Comfort Control Plus level~~ **RESOLVED: byte 21, bits 7–6**, level =
     field + 1, confirmed by a controlled 2 → 3 change and visible in the
     pressure trace as 0.26 cmH₂O of extra expiratory relief.
   - ~~Patient circuit~~ **RESOLVED: byte 21 bit 0**, set = 15 mm, from a pair of
     controlled writes on the S.Box. It shares its byte with CC+, which is why no
     byte ever held 15 or 22.
   - **Heated tube** is unassigned but no longer unrecoverable: **byte 23** is a
     candidate on circumstantial grounds. See "The Néa Auto".
9. **Byte 10 bit 7** cleared on the same record as the mask-leak and CC+ changes
   and belongs to neither setting's value. It is set on both the Rêve and the Néa
   and has never moved otherwise. *(Byte 14 bit 4, once paired with it here, is
   the patient access lock — see its section.)*
10. ~~**Practitioner versus patient ramp time**~~ **RESOLVED: byte 12 is the
    patient's, byte 13 the practitioner's ceiling.** Settled by the Néa card and
    corroborated across all 467 blocks of an S.Box archive. See "The Néa Auto".
11. ~~**The therapy mode is not in the settings record.**~~ **RESOLVED
    2026-08-26: it is byte 17**, 60 = A-PAP and 0 = CPAP, with byte 9 the
    prescribed pressure and 80 meaning none. Confirmed against the vendor
    software's per-day Mode and Prescribed pressure columns on two devices, and
    independently from the S.Box's own per-minute pressures. The loader no longer
    hard-codes `MODE_APAP`. See "The Néa Auto".
12. **Byte 14 bits 0 and 1.** Bit 2 is I.Ramp and bit 4 the patient access lock;
    bit 3 is set on every value ever seen. Bit 1 flips night to night on both the
    Rêve and the Néa with no other field moving, which looks like a patient
    toggle, but nothing observed pins it.
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
  useful next artefact, and it does not need a different device. It has now
  settled three fields out of three attempts. Note that a settings record has to
  actually appear afterwards — four nights on the second card carry none at all —
  so the card should be pulled a night or two after the change, not the same
  morning.
- **A bi-level card** — an S.Box Duo, Néa Duo or Ventea. Byte 17 has been seen
  holding two values, A-PAP and CPAP, and the vendor DLL exports `IsBilevel*`
  alongside `IsCPAP`, so a third value almost certainly exists. The loader treats
  anything it does not recognise as A-PAP and warns.
- **A manufacturer report for a Rêve or a Néa** that prints a heated tube
  *level*. That would confirm or kill byte 23, which no controlled change can
  reach.
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
