# SEFAM S.Box AUTO — SD card format analysis

**Settings decode, as of 2026-08-13.** Eleven of the twelve settings-record
fields are identified, and **every setting the vendor report prints is now
decoded or known to be absent**. Word 10 is the only unassigned field and nothing
printed is left to match it against. The patient circuit (word 9) and theoretical mask
leak (word 11) were settled by driving *Sefam Analyze* against a card with **no
device attached**, which writes its changes to `upload.dat` — see that section
for the method and its traps.

The same method placed the **humidifier and heated tube levels**, and showed that
neither is in the record the device writes. **Those two settings can never be
imported from an S.Box card**; that is now a known limit rather than an open
question.

**Status:** container and waveforms decode with the existing loader. **These
cards carry no event log**, and neither events nor therapy settings live
anywhere the loader currently looks — both are in the device memory image
(`<serial>.RAM`), which holds a settings history table and a lifetime
per-session archive with event counts and minute-by-minute trend data. Both
structures are decoded below and confirmed against a vendor report on one card
and against the card's own waveforms on a second. Every event of every type can
be placed to the minute it occurred; the remaining limits are that resolution
and the absence of event durations.

**Design decision, 2026-08-08:** the loader reads `<serial>.RAM`. It is the only
place this device keeps its events and settings, so the alternative is importing
waveforms and nothing else. **Implemented 2026-08-08** in
`SefamParsing::readMemoryImage()` and `SefamLoader::Open()`, gated on the session
having no `.LOG`, so the Rêve path is untouched. **Built and imported from a real
S.Box card the same day**, with waveforms, settings and events all rendering
sensibly — which rules out the structural failure modes: a block matched to the
wrong session, events placed outside the session span, settings taken from the
wrong slot. The counts themselves were checked separately against the
manufacturer's report, and the matching and decoding logic was simulated over
both sample cards before it was built.

**Branding:** OSCAR displays this device as a **Sefam** S.Box Auto. Only the Rêve
Auto is rebranded, to "Sanrai" — see the Branding section of
`SEFAM_REVE_CARD_ANALYSIS.md` for why, and for the rule `sefamBrandName()`
applies. No S.Box card seen so far carries any distributor marking, and the
vendor report for one of them came from *Sefam Analyze*.

### Matching sessions to archive blocks

A block carries no timestamp, so it is matched to a directory by minute count,
walking both lists newest-first. The count a directory should have is

```
minutes = floor(recordCount * 10 / 60) + 1
```

which holds on **23 of the 24 sessions** across both cards. The exception is a
3.5-minute session written seven weeks before the next one, where the card holds
half a minute more than the device counted — a torn or abandoned write. The
loader therefore tries an exact match first and accepts a one-minute discrepancy
only when nothing fits exactly; with that, both cards match completely (21 of 22
directories on card A, the 22nd being the data-less stub, and 3 of 3 on card B).

The scan skips backwards over unmatched blocks — necessary, since card A has
three archived sessions with no directory — but is bounded, and gives up rather
than sliding every older session onto the wrong block. Settings recovered this
way track card A's real history exactly: 8.0–15.0 through the June sessions,
8.5–15.0 from the session after the 04/07 23:46 change, 10.0–17.0 from the one
starting 15:54:55 against a 15:55 change, and 12.0–19.0 from the one starting
01:33:36 against a 01:34 change.

**Device:** SEFAM S.Box AUTO (APAP). `Created By=S.Box_AUTO`, firmware
`VER :A020400`. Same manufacturer and same firmware *platform* as the Rêve Auto,
but a different device class and a materially different card.

**Two cards examined**, from different devices with different therapy settings:

| | model code | sessions | span | vendor report |
|---|---|---|---|---|
| card A | `1200R` | 22 | 2026-06-23 → 2026-07-10, 53.9 h | yes, 6 days |
| card B | `1263R` | 3 | 2025-12-13 → 2026-02-07, 13.4 h | no |

Card A supplies the ground truth; card B is the independent check. Both model
codes report the same firmware version, and everything below holds on both
unless stated otherwise.

**Companion document:** `SEFAM_REVE_CARD_ANALYSIS.md` covers the Rêve Auto
`1279R` and is the reference for everything the two devices share (XOR-0xBF
header obfuscation, 10-second record framing, checksum/sequence trailer, `.INI`
semantics). This note records only what is **different or new** on the S.Box.

**Loader:** `sefam_loader.cpp` / `sefamDataParsing.cpp`. Both S.Box model codes
are now on the validated list in `sefamModelIsValidated()`, so an S.Box card no
longer raises the "untested device" warning. `1200R` earns that from its
manufacturer report; `1263R` rides on it, having the same product name, firmware
and card layout but no report of its own. Any other SEFAM model code still
warns.

---

**Comparing OSCAR against the vendor software?** Read
`SEFAM_ANALYZE_VS_OSCAR.md` first — it lists the differences that are structural
rather than defects, calibrated against a card that has a full vendor report.

## Card layout

```
<SD card root>/
├── upload.dat                  optional; see below
└── 1200R/ or 1263R/            model code
    └── <serial>/               device serial (digits only)
        ├── <serial>.RAM      2,097,307 bytes — device memory image
        ├── <serial>.BKP      2,097,307 bytes — previous memory image
        └── DATA_0 … DATA_nnn/       one per session, unpadded
            ├── DATA_nnn.INI    1,290 bytes — cleartext channel manifest
            ├── DATA_nnn.FLW    flow                  25 Hz
            ├── DATA_nnn.PRE    pressure               5 Hz
            ├── DATA_nnn.LK     leak                   1 Hz
            ├── DATA_nnn.DET    breath-phase bitfield 25 Hz
            ├── DATA_nnn.NSD    near-constant bitfield 25 Hz
            ├── DATA_nnn.Y17    event bitfield        25 Hz   ← not in the INI
            └── .ABD .HRT .PLS .POS .SPO .STS .THO    header-only stubs
```

**There is no `DATA_nnn.LOG` anywhere on either card.** On card A this was
checked against the untouched archive the sample arrived in as well as the
extracted tree — 334 archive entries, no `.LOG`. It is not a collection
accident; it is a device trait.

Card A holds 22 session directories, 2026-06-23 22:20 → 2026-07-10 08:49, 53.9 h
of recording. Its `DATA_111` is a directory whose channel files are all
header-only stubs — the session was opened and nothing was written. **A loader
must tolerate a directory with an `.INI` and no data.**

Card B holds only three, spanning almost two months: a 3.5-minute session in
mid-December, then a 7.0 h night on 31 January and a 6.3 h night on 7 February.
Sessions are retained sparsely and consecutive directory numbers can be weeks
apart, so **the numbering says nothing about elapsed time** — read every `.INI`.

Directory names are **not zero-padded** (`DATA_0` … `DATA_2`, `DATA_90` …
`DATA_111`), so they must be sorted numerically. This is the same trap the Rêve
note records.

### `upload.dat` is the settings write-back channel

A 568-byte `upload.dat` sits at the **card root**, outside the model directory,
XOR-0xBF obfuscated like the file headers. An earlier revision of this note
described it as "a staging file for the manufacturer's upload path" whose one
populated field is a patient identifier. **That was wrong.** It also carries the
full therapy settings block in the clear, and it is how a clinician's PC pushes a
settings change onto a card for the device to pick up.

This was established directly: *Sefam Analyze* 4.4.2, driven with no device
attached, writes `upload.dat` and **does not touch `.RAM` or `.BKP`**. The card
being edited is identified by the software as a *patient card*; the manual also
documents a *maintenance card* for configuring a fleet of devices, and whether
that path writes the same structure has not been checked.

```
0x000        0x19, constant
0x001        varies
0x002        0
0x003        '3'
0x004-0x017  20-char space-padded identity string
0x018-0x01A  section selector and a running sum over the payload
0x01B-0x01C  2, 27, constant
0x01D-0x027  8-char identity string, null-padded
0x028-0x224  zero
0x225-0x236  the settings block
0x237        per-device, constant across writes — not a payload checksum
```

The settings block holds the same twelve fields as the memory image's settings
record, in the block header's rotated order, plus extra bytes the record has no
room for:

| offset | field | card A | card B |
|---|---|---|---|
| 0x225 | settings word 10 | 50 | 80 |
| 0x226 | theoretical mask leak | 164 → 36 lpm | 40 lpm |
| 0x227 | minimum pressure ×10 | 120 | 70 |
| 0x228 | maximum pressure ×10 | 190 | 130 |
| 0x229 | ramp start pressure ×10 | 80 | 40 |
| 0x22A | one of the two ramp times | — | 45 |
| 0x22B | the other ramp time | — | 45 |
| 0x22C | apnoea response pressure ×10 | 130 | 130 |
| 0x22D | ramp mode, bit 2 = I.Ramp, bit 4 = patient access lock | 8 = T.Ramp | 12 = I.Ramp |
| 0x22E | **patient circuit**, bit 0 = 15 mm | 184 / 185 | 185 |
| 0x22F | constant 60 | 60 | 60 |
| 0x230-0x232 | 0, 5, 0 | constant | constant |
| **0x233** | **humidifier level**, 0 = off | 0 / 10 | 5 |
| **0x234** | **heated tube level** | 0 / 1 / 5 / 6 | 1 |
| 0x235-0x236 | 1, 0 | constant | constant |

**Controlled changes, one setting per write, same device:**

| written | 0x22E | 0x233 | 0x234 |
|---|---|---|---|
| circuit 22 mm | 184 | 5 | 0 |
| circuit 15 mm | **185** | 5 | 0 |
| CC+ off | **0** | 5 | 0 |
| CC+ level 1 | **56** = `0x38` | 5 | 0 |
| CC+ level 3 | **184** = `0xB8` | 5 | 0 |
| heated tube level 1 | 184 | 5 | **1** |
| heated tube level 5 | 184 | 5 | **5** |
| heated tube Auto | 184 | 5 | **6** |
| humidifier off | 184 | **0** | 0 |
| humidifier 2 | 184 | **2** | 0 |
| humidifier 10 | 184 | **10** | 0 |

Exactly one byte moved per change. **0x233 is the humidifier level** and
**0x234 the heated tube level** (0 = off, 1–5 = levels, 6 = Auto); the humidifier
is an exact identity over three points spanning its whole documented OFF–10 range,
no scale and no offset, and the heated tube behaves the same way.

**0x22E carries two settings at once** — the CC+ writes moved the same byte the
circuit writes had — which is what identified settings word 9 as a packed field.
See "Layout" for the bit map.

Both the circuit and the CC+ level transfer to the memory image, 0x22E being
settings word 9 — see "Layout". **The two accessory levels do not**: they sit past
0x22F, outside the twelve fields, so they are readable only from a file the
clinician's software wrote and cannot be recovered from a card the device
produced on its own.

### The patient access lock, and how the ramp is stored

Two further fields were settled the same way.

**The patient access lock is bit 4 of the ramp-mode field.** Two writes differing
only in the software's "Editable by the patient" checkbox moved byte 0x22D from
**8** to **24** — bit 4 alone, set meaning the patient *cannot* change the ramp,
Comfort Control Plus or Intelligent Start. The single checkbox governs all three
settings together, so this is one flag, not the three the vendor manual's wording
implies.

That value transfers to the memory image, 0x22D being settings word 3, and it
explains the one mode value this note could never place: card B's factory slot
holds **28**, which is 12 (I.Ramp) with bit 4 set. It also explains the Rêve's
byte 14, unexplained in that note since the first card — see "Byte 14 bit 4 is
the patient access lock" there.

**Ramp OFF is stored as duration zero, not as a mode.** A write made with both
ramp settings off carries word 2 = 0, word 1 = 0 and the mode byte still reading
8. That matches card A's own history exactly: the vendor report prints "Disabled"
for precisely the slots whose table entries are mode 8 with word 2 = 0. There is
no separate "off" mode value.

**Word 1 is the patient's ramp time and word 2 the practitioner's ceiling —
CONFIRMED 2026-08-26.** The software's Parameters panel offers a *Practitioner*
setting — `OFF`, `I.Ramp`, or `T.Ramp Maxi` 5 to 45 minutes in steps of 5 — and a
separate *Patient* setting limited to values at or below it, which reduces to
On/Off when the mode is I.Ramp. The vendor's API agrees, exporting
`GetMaxRampTime` on one class and `GetRampTime` on another.

This note previously called it unconfirmed, because the rival reading — "word 1
merely echoes the duration in T.Ramp" — fitted the settings *table* just as well:
its I.Ramp slots carry word 1 = 0 and its T.Ramp slots carry word 1 = word 2.
Two things settled it, and one of them was in this card all along.

- **The session archive, which was never checked for this.** Across all 467
  blocks of card A the patient's value never exceeds the ceiling and falls
  strictly below it in five distinct T.Ramp states — 0/5, 0/20, 5/10, 5/20 and
  10/20. An echo cannot produce those.
- **A Néa card.** Its practitioner value stayed at 30 minutes for a year while
  the patient's moved 30, 10, 0, 20, 10 across five records. See
  `SEFAM_REVE_CARD_ANALYSIS.md`, "The Néa Auto".

`rampMinutesFor()` now reports the patient's value, except under I.Ramp — mode
bit 2 — where the patient has no duration of their own and the practitioner's is
the only answer. Checked against card A's own report: every ramp figure it prints
is unchanged, on both its I.Ramp and its T.Ramp sessions. It changes 46 of the
467 blocks, none of them inside the report's window.

The attempts to write the two values differing through *Sefam Analyze* all failed
— a practitioner maximum of 45 with a patient value of 20 produced files whose
ramp bytes stayed at zero, and marking the pressure section as changed turned
both ramp settings off without them being touched. That experiment is no longer
needed.

### The file is a write command, not a snapshot

**A field's byte means something only when that write was carrying it.** This is
not a subtlety — it is demonstrated: `0x233` reads **5** in all five circuit and
heated-tube writes, while the device's actual humidifier level is **2**, as both
the vendor report and the software's own dialogue show. The real value appears in
none of the seven files. Reading an uncarried field gives a placeholder, not the
setting.

The same explains ramp duration reading 0 in those writes on a card whose report
says 15 min.

**Bytes 0x018-0x019 are a big-endian bitmask of which fields the write carries:**

| write | mask |
|---|---|
| patient circuit | `0x0010` |
| Comfort Control Plus | `0x0008` |
| ramp | `0x0004` |
| humidifier | `0x0100` |
| heated tube | `0x0200` |
| patient access lock | `0x4000` |

> **The mask does not fully predict which bytes move.** One write carrying only
> the lock bit also changed the comfort/circuit byte. Treat the mask as a strong
> hint about intent, not as a guarantee about content.

Byte 0x01A is a running sum over the payload: under an unchanged mask it reads
28, 30 and 38 across humidifier writes of 0, 2 and 10, and moved +1 when the
circuit byte went 184 → 185. Three points on one field, tracking one-for-one.

This also explains why card B's file looked like a complete and correct settings
dump while card A's single-field writes did not: card B's mask is `0x74FC`, a
dozen bits set — a full write. **So a full write *is* a usable snapshot; a
single-field write is not.**

> **Circuit Select could not be exercised.** The software refuses to offer it
> unless the device is already set that way, which matches its manual. Since bit
> 0 only says "is 15 mm", Circuit Select is indistinguishable from 22 mm.

**Treat this file as personal data**, more so than before: it carries a patient
identity string — a locale-specific "undefined" placeholder on the card examined
— *and* that patient's therapy prescription. OSCAR has no reason to read it, and
a backup that copies the card verbatim will carry it along.

> **Do not import settings from it.** It records what a clinician *sent*, not
> what the device accepted or ran, it is present only when someone happened to
> write settings, and its unselected sections are meaningless. It is a decoding
> instrument, not a data source.

---

## Header: the short 38-byte variant, and it matters

Channel files carry the **short** header, not the Rêve's 71-byte form:

```
#02/1200R<serial padded to 20 chars>/260705020303/
```

`#02/` is the format version (the Rêve writes `#03/`); the loader already treats
the tag as a version rather than a constant. What differs materially is that
**the 32-hex field is absent**, so there is no UTC epoch on this card — only the
12-character device-local timestamp. `FileHeader::utcEpoch` stays 0 and
`FileHeader::length` is 38 for every channel file, including the ones that carry
data. Header length is detected, never assumed, so waveforms decode correctly.

Confirm the geometry arithmetically rather than trusting the header type: a
`.FLW` of 66,324 bytes is `38 + 262 × 253`, and `66,324 − 71` is not a whole
number of records.

---

## The `.INI` — same schema mechanism, different rates

Cleartext, `[Chan0]`…`[Chan10]`, identical field names to the Rêve. The rates
are **not** the Rêve's:

| Channel | S.Box | Rêve | Declared in `.INI` |
|---|---|---|---|
| `FLW` | 25 Hz | 10 Hz | yes |
| `PRE` | 5 Hz | 5 Hz | yes |
| `LK` | 1 Hz | 1 Hz | yes |
| `DET` | 25 Hz | 10 Hz | yes |
| `NSD` | 25 Hz | 10 Hz | **no** |
| `Y17` | 25 Hz | 10 Hz | **no** |

`NSD` and `Y17` are present and populated but undeclared, so their geometry has
to be inferred from a channel that *is* declared. On this card all four of
`FLW`/`DET`/`NSD`/`Y17` have identical file sizes, which pins them to the same
25 Hz framing.

**Never hard-code a sample rate for this family.** The `.INI` is authoritative
for the declared channels, and the undeclared ones must be sized by comparison,
per card.

The `[Create Info]` block gives `Created By`, the full serial, the firmware
version and a creation timestamp; `[Start Record]` gives the session start as
separate Y/M/D/h/m/s fields plus a programmed and a real record duration.

### Three sessions imported as nothing, because their sequence starts at 2

*Found 2026-08-26.*

`DATA_94`, `DATA_95` and `DATA_96` on this card — 771, 718 and 733 records, about
6.2 hours in total — imported as **nothing at all**, and the only trace was a
`no populated channels` warning that read like an empty directory.

Their channel files are intact: every checksum passes and the file size is an
exact whole number of records. The record **sequence counter simply starts at 2**
rather than 1, the device having dropped the first record and carried on counting.
`readChannel()` treated any sequence mismatch as corruption and truncated at the
last good record — which here was none of them, so every channel came back empty
and `readSession()` rejected the session.

Fixed by filling a skipped sequence number with invalid samples and continuing.
See `SEFAM_REVE_CARD_ANALYSIS.md`, "The sequence counter skips, and a skip is not
corruption", for the full picture — the same defect cost a Néa card three
sessions' worth of data too.

---

## Why OSCAR shows waveforms but no events and no settings

Two independent blockers, both in the current loader:

1. **Events and settings come only from `.LOG`.** `sefam_loader.cpp` builds every
   event list from `data.log`, and the whole therapy-settings block is inside
   `if (lastKnown.valid)`, which is only ever set by `parseSettings()` on a log
   record of code 2 or code 13. No log ⇒ no mode, no pressures, no ramp, no
   humidifier, and no events. The loader never opens `<serial>.RAM`, which is
   where this device keeps all of it.
2. **Event placement needs a UTC epoch the S.Box does not write.** The event loop
   is gated on `data.header.utcEpoch != 0`, because events are positioned
   relative to the session start using the epoch from the long header. On this
   card that is always 0, so even a card that *did* carry a `.LOG` would import
   no events without a second time base being wired in.

Fixing (1) without fixing (2) would achieve nothing on this device class.

---

## Vendor report for this card — the ground truth

The sample came with a *Sefam Analyze* 4.4.2 report covering six days of it. The
report is owner-password protected only, so it extracts with an ordinary PDF text
tool; no user password is needed.

It reports a full event set — AHI 8.3 (OA/OH/CA), 14.8 including central
hypopnoeas, snoring index 88.4/h, flow-limitation runs 11.1/h, mean apnoea 13 s
and mean hypopnoea 17 s — and a complete settings list. **So the analyzer obtains
both events and settings from this card**, or from data it has accumulated about
this device. That is what makes the gaps below worth chasing rather than
declaring impossible.

The report also carries device lifetime counters that appear nowhere in the
session files: total sessions, configuration date, global period in days. Those
are candidates for the same memory image the settings turned out to live in.

---

## Settings: a 12-slot history table inside `.RAM` / `.BKP`

**This is the main new finding, and it is confirmed, not inferred.**

Unlike the Rêve's, the S.Box memory image is **not encrypted**. It opens with a
155-byte plaintext header — a length prefix, the last session's `YYMMDDhhmmss`
timestamp, the firmware version string and two hex-text configuration blocks —
followed by a 2 MiB raw image containing UTF-16 path strings and binary
structures. (The Rêve's `.RAM`/`.BKP` are high-entropy throughout with zero
ASCII; see the Rêve note. **Do not assume one model's image tells you anything
about the other's.**)

The 155-byte header is nearly invariant. Between the `.RAM` and `.BKP` of the
same card it differs only in its timestamp, one three-byte field in each of the
two hex-text blocks, and a two-byte tail that behaves like a checksum. Between
the two *cards* the second hex block differs in exactly one byte. That block is
**not** the therapy settings: it holds identical values on two devices whose
minimum and maximum pressures differ (7.0/13.0 against 12.0/19.0), which rules
out the reading that first suggests itself. What it does hold is unknown.

### How it was found

The vendor report prints a **settings *history*** — six timestamped snapshots,
not one current set — and the minimum pressure changes across them:

| When | Min | Max | Ramp | Ramp pressure |
|---|---|---|---|---|
| 2026-06-23 22:21 | 8.0 | 15.0 | 45 min [I.Ramp] | 6.5 |
| 2026-07-04 17:38 | 6.0 | 15.0 | Disabled | 6.5 |
| 2026-07-04 17:40 | 8.0 | 15.0 | Disabled | 6.5 |
| 2026-07-04 23:46 | 8.5 | 15.0 | 15 min [T.Ramp] | 8.0 |
| 2026-07-06 15:55 | 10.0 | 17.0 | 15 min [T.Ramp] | 8.0 |
| 2026-07-08 01:34 | 12.0 | 19.0 | 15 min [T.Ramp] | 8.0 |

Four of the six coincide with a session start to within a minute; the two on
2026-07-04 at 17:38 and 17:40 have no session directory at all, so the device
records a settings change made outside therapy.

Six *varying* values in a known order is a search key. Scanning the 2 MiB image
for any column reproducing `80, 60, 80, 85, 100, 120` (tenths of a cmH₂O) at a
constant stride returns **exactly one location in the whole file**, at stride 24;
the maximum-pressure sequence sits two bytes ahead of it. A single hit in 2 MiB
is the evidence — this is not a value that happened to match.

### Layout

A table of **twelve 24-byte records, each twelve uint16 little-endian**,
beginning at offset **16338** — the same offset on both cards, from different
devices and different model codes, so it is fixed at least for firmware
`A020400`. Locating it by shape rather than by offset is still the safer
implementation.

| word | meaning | card A | card B |
|---|---|---|---|
| 0 | ramp start pressure, ×10 cmH₂O | 40–80 | 40 |
| 1 | **the patient's own ramp time**, minutes; 0 = the patient turned it off | 0 or 15 | 0 or 45 |
| 2 | **the practitioner's ceiling** on it, minutes; 0 = ramp disabled | 0, 15, 45 | 45 |
| 3 | ramp mode (8 = T.Ramp, 12 = I.Ramp) plus **bit 4 = patient access lock** | 8, 12 | 12, and 28 = I.Ramp + locked in the factory slot |
| 4 | maximum pressure, ×10 cmH₂O | 40–190 | 130, 200 |
| 5 | minimum pressure, ×10 cmH₂O | 40–120 | 40, 70 |
| 6 | **therapy mode**: 60 = A-PAP, 0 = CPAP (see below) | 0 or 60 | 60 |
| 7 | 60 | constant | constant |
| 8 | 130 = the apnoea response pressure the report prints as 13.0 cmH₂O | constant | constant |
| 9 | **CC+ level (bits 7–6) and patient circuit (bit 0)**, packed (see below) | 184 → Level 3, 22 mm | 185 → Level 3, 15 mm |
| 10 | **prescribed pressure**, ×10 cmH₂O; 80 = not set. Used only when word 6 says CPAP | 50 | 80 |
| 11 | theoretical mask leak, lpm in bits 0–6 (see below) | 164 → 36 lpm | 164 → 36, 40 → 40 |

Words 0–8 were the confirmed part. **Word 11 was identified later, on the Rêve**,
whose code 2 log record turned out to be this same record with one byte per field
instead of one uint16 — see "The settings record is the S.Box's table" in
`SEFAM_REVE_CARD_ANALYSIS.md`. There it is byte 10, pinned by a controlled change
of the setting from 36 to 34 lpm.

**Word 11 is now confirmed on this model too, not merely inherited.** Card A's
vendor report prints `Theoretical mask leak 36 lpm`, and that card stores 164 =
`0x80 | 36` in all 467 of its archive blocks. Card B, which has no report, reads
36 on its first session and 40 on the other two — both inside the 18–60 lpm span
of the vendor's own mask table, which is what the setting is populated from. See
the Rêve note under "Theoretical mask leak — byte 10".

**Word 9 packs two settings**: Comfort Control Plus and the patient circuit.

| bits | meaning |
|---|---|
| 7–6 | CC+ level − 1 |
| 5–3 | set when CC+ is enabled; all clear when off — not fully proven, see the Rêve note |
| 0 | patient circuit: set = 15 mm, clear = 22 mm |

Established by driving *Sefam Analyze* with no device attached. Writes of CC+
off, level 1 and level 3 produced `0x00`, `0x38` and `0xB8`; a separate pair of
writes moved bit 0 alone, 184 for 22 mm and 185 for 15 mm. Card A's own vendor
report then agrees on **both** halves at once: it prints `Comfort Control Plus
Level 3` and `Patient circuit 22 mm`, and the device had stored `0xB8`.

Bit 0 is the vendor's own `IsCalibTube15mm` flag, which answers a question this
analysis carried for weeks: **no byte holds 15 or 22 because the diameter is
never stored as a number.** The flag only says "is 15 mm", so the vendor's third
choice, *Circuit Select*, cannot be distinguished from 22 mm.

The block headers show both halves genuinely varying over card A's 467 sessions
— CC+ at levels 1, 2 and 3, and the circuit at both diameters — while its
settings table, which holds only current values, reads 184 throughout and matches
the report's "latest settings". Card B reads `0xB9` on all three of its sessions.
**Across all 478 settings records held, on both models, the level never falls
outside the manual's documented 1–3.**

**Word 10 remains unassigned, and there is now nothing left to guess it as.**
Card A's report prints `Humidifier Level 2` and `Comfort Control Plus Level 3`,
and its word 10 is 50 = `0x32` — nibbles 3 and 2, which looked like both levels
packed into one byte. Both halves of that guess are dead: the humidifier moved to
0 and then to 10 with word 10 sitting at 50 throughout, and CC+ turned out to
live in word 9. A two-field match out of one byte was a coincidence, which is why
it was recorded as a candidate rather than believed.

Every setting the vendor report prints is now accounted for elsewhere, so word 10
holds something the reports never show. It differs per device (50 against 80) and
is constant within each, which fits a calibration constant better than a
setting.

**Word 11 is the one field whose position had to be read across the record
boundary** in this table: it sits immediately *before* the next slot's ramp start
pressure. The archive block header stores the same record already rotated, with
words 10 and 11 leading, which is the Rêve's own byte order and is what settled
the alignment.

Decoded, the twelve slots read:

```
slot  ramp P  ramp            max / min
 0     6.5    45 min I.Ramp   15.0 /  8.0
 1     6.5    disabled        15.0 /  6.0
 2     6.5    disabled        15.0 /  8.0
 3     8.0    15 min T.Ramp   15.0 /  8.5
 4     8.0    15 min T.Ramp   17.0 / 10.0
 5     8.0    15 min T.Ramp   19.0 / 12.0
 6     4.0    45 min I.Ramp    4.0 /  4.0
 7     4.0    45 min I.Ramp    7.0 /  4.0
 8     4.5    45 min I.Ramp    7.0 /  4.0
 9     6.5    45 min I.Ramp   16.0 /  9.0
10     8.0    15 min T.Ramp   17.0 / 10.0
11     8.0    15 min T.Ramp   19.0 / 12.0
```

Slots 0–5 reproduce the vendor's six printed snapshots **exactly and in
chronological order across five independent fields** — thirty of thirty field
values. Slots 6–11 are six older configurations, consistent with a device the
report describes as having years of history. The `.RAM` and `.BKP` images hold
byte-identical tables.

### Independent confirmation on card B

Card B has no vendor report, so the decode was checked against the pressure the
device actually delivered. Its table reads **minimum 7.0, maximum 13.0, ramp
45 min I.Ramp starting at 4.0** — and the mask pressure agrees:

| session | 5th pct | median | 99.9th pct | max |
|---|---|---|---|---|
| night 1 | 4.2 | 8.5 | **13.0** | 15.1 |
| night 2 | 4.0 | 6.7 | 9.6 | 11.4 |

The 99.9th percentile of night 1 lands exactly on the decoded 13.0 ceiling — the
device pressed against its limit and stopped there — and the 5th percentile of
both nights sits on the decoded 4.0 ramp start, which is what a 45-minute ramp
should produce. (`PRE` is a *mask* pressure carrying the breath ripple, so the
raw maximum overshoots the ceiling; see the Rêve note on `PRE`.) Nothing in
those numbers was used to derive the layout, so this is a genuine test of it and
not a restatement.

Card B's slot 0 is a factory record — 20.0/4.0, the full device range, with an
otherwise unseen mode value of 28 — followed by three identical therapy records.
Slots 4–8 are zeroed and slots 10–11 repeat slots 2–3.

### Traps

- **The record boundary is 8 bytes earlier than the obvious one.** Starting each
  record where the max/min column starts matches minimum and maximum pressure on
  all six entries but breaks the ramp fields on three of them. Anyone re-deriving
  this will land on the wrong alignment first; the ramp fields are the check that
  catches it.
- **Slot order is not plain chronological.** On card A slots 0–5 are the six most
  recent and 6–11 six older ones, so the table wraps; on card B the used slots
  are 0–3 with 10–11 repeating 2–3. That said, **"take the last non-zero slot"
  yields the correct current settings on both cards** — slot 11 on card A
  matches the vendor's "latest settings for the period", and slot 11 on card B
  matches the delivered pressure above. Use that rule, but do not assume the
  earlier slots are in order: neither card exercises the wrap unambiguously.
- **There are no timestamps.** The six change times the analyzer prints appear
  nowhere in the image. Searched: epoch seconds at four UTC offsets in both byte
  orders, minutes since several plausible bases, text and BCD date forms, and the
  very distinctive 120-second gap between two of the changes. Nothing matches. So
  the analyzer's change *dates* come from its own database, not from the card.

### What this means for a loader

Minimum and maximum pressure, ramp duration, ramp mode and ramp start pressure
are all recoverable for the S.Box. Because the change times are absent, settings
can only be applied as "current" — and on this card the settings changed three
times inside a six-day period, so attributing per-session settings would be
wrong today.

**Still unidentified:** humidifier level, patient circuit diameter and
heated-tube presence. Only card A has a report, so there is no labelled value to
match card B's differing words against — the same wall the Rêve note describes
under "What is left, and why the remaining two are hard". What card B narrowed
was *where* to look: **words 9, 10 and 11**, which differ between the two devices
while words 6, 7 and 8 do not. One of those three has since been named on the
Rêve — word 11 is the theoretical mask leak — leaving **words 9 and 10**.

~~Word 6 or 7 holding 60 on both cards is consistent with the Rêve's mask-leak
candidate, 0.6 lpm per count giving 36 lpm.~~ **That reading is dead.** Words 6
and 7 held 60 on a Rêve across a mask-leak change from 36 to 34 lpm, so they are
an unrelated constant on both models. The 60 × 0.6 = 36 arithmetic was a
coincidence, and a persuasive one — it is recorded here only so it is not
rediscovered and believed a second time.

---

## Events: a per-session archive inside `.RAM` / `.BKP`

**The events are on the card.** They are not an event log — there is no list of
individual event times anywhere — but a **per-session summary archive**, one
block per session for the whole life of the device, in the same memory image as
the settings.

### Structure

The archive begins at relative offset **0x10000** (file offset 0x1009b, since the
155-byte header precedes the image) and is a **contiguous chain**: each block is
88 bytes of header followed by one 6-byte record per minute of the session, and
the next block starts immediately after. Every block opens with the marker
**0xFEA1**. Walking the chain gives **467 blocks on card A** — exactly the "Total
number of sessions: 467" its vendor report prints — and 3 on card B.

Walk the chain; do not scan for the marker. 0xFEA1 also occurs inside the
per-minute data, and a scan produces false blocks.

### Block header — 44 uint16, big-endian

Note the endianness: the settings table earlier in the image is little-endian,
this header is big-endian.

| word | meaning |
|---|---|
| 0 | 0xFEA1 marker |
| 13 | obstructive apnoeas |
| 14 | central apnoeas |
| 15 | obstructive hypopnoeas |
| 16 | central hypopnoeas |
| 17 | snores |
| 18 | flow-limitation runs |
| 24 | session duration in **minutes** |
| 29–40 | the 12-word settings record, in the order `w10 w11 w0…w9` of the settings table, with the ramp-mode word one higher |
| others | unidentified |

Words 13–18 are six **contiguous columns in the same order the vendor report
prints its six indices**. Summed over the fifteen blocks the report covers:

| word | sum | report index × usage | type |
|---|---|---|---|
| 13 | 58 | 59 | AI obstructive 1.7/h |
| 14 | 26 | 28 | AI central 0.8/h |
| 15 | 199 | 201 | HI obstructive 5.8/h |
| 16 | 228 | 226 | HI central 6.5/h |
| 17 | 3059 | 3067 | snoring 88.4/h |
| 18 | 381 | 385 | FL runs 11.1/h |

The 1–2 % residual is a denominator question, not an identification one: the
report's indices are printed to one decimal, and no single usage figure
reproduces all six exactly — the report itself prints AHI-including-CH as both
14.7 and 14.8 on different pages.

Two further cross-checks on the same fifteen blocks: their minute counts sum to
**2147 min = 35 h 47**, exactly the report's operating time, and there are
exactly **fifteen** of them against the report's "15 Sessions" — three more than
the card has session directories, which is why the directory count never
matched.

### Matching blocks to sessions

Card A's last 22 blocks correspond one-to-one, in order, to its 22 session
directories, with every minute count matching the directory's record count.
**Word 19 is monotonic but is not a timestamp** in any unit tried — the ratio
between block deltas and elapsed time varies by more than an order of magnitude
between consecutive pairs. Matching therefore rests on order plus duration,
which is workable but is the weakest link in the whole decode.

### Per-minute records — 6 bytes

**Every event type is placed to the minute.** Byte 2 is four independent 2-bit
counters, one per event type; bytes 3 and 4 are 7-bit counters with a separate
flag in bit 7.

| byte | bits | meaning |
|---|---|---|
| 0 | all | mean mask pressure ×10 for that minute |
| 1 | all | continuous, present every minute — unidentified, leak-like |
| 2 | 0–1 | obstructive apnoeas this minute (0–3) |
| 2 | 2–3 | central apnoeas this minute |
| 2 | 4–5 | obstructive hypopnoeas this minute |
| 2 | 6–7 | central hypopnoeas this minute |
| 3 | 0–6 | flow-limitation runs this minute; **bit 7 is a separate flag** |
| 4 | 0–6 | snores this minute; **bit 7 is a separate flag** |
| 5 | all | continuous, present every minute — unidentified |

Evidence, over **every block on both cards — 470 in total, zero mismatches**:

| column | test | result |
|---|---|---|
| byte 2, four 2-bit fields | sums vs header words 13–16 | 297 blocks exact, 173 both-zero, **0 wrong** |
| byte 3 & 0x0f | sum vs header word 18 | **470/470 exact** |
| byte 4 & 0x7f | sum vs header word 17 | **470/470 exact** |
| byte 0 | vs minute means of `PRE` | r = 0.9996 and 0.9988, mean error 0.05 cmH₂O |

Mask bit 7 off byte 3 before summing: reading it raw fails on 105 of 467 blocks
on card A, which is exactly how this was nearly missed — the first six blocks
examined happened to have no minute with that flag set.

The pressure correlation is the load-bearing check on the *mapping*: it confirms,
independently of any vendor report, that a given block belongs to a given
session, minute for minute.

### What a loader could do with this

Everything except sub-minute placement. Event **counts, AHI, per-day indices and
flags on the charts** are all available, since each event can be emitted at the
minute it occurred; so are minute-resolution snoring, flow limitation and mean
pressure. The one limit is resolution: a flag can be placed within the correct
minute but not at the correct second, and the card carries no event duration.

Because events can be given times, they can go into ordinary `EventList`s and no
special summary-only handling is needed — which matters, since
`Session::UpdateSummaries()` clears `m_availableChannels` and rebuilds it **only
from `eventlist`**, so a channel carrying a count with no events would be dropped
before it was ever saved. (`Session::setCount()` does add to
`m_availableChannels`, so the counts-only route is possible, but it would have to
run *after* `UpdateSummaries()`. Avoid needing it.)

---

## This card settles the therapy mode, from its own data

*Added 2026-08-26.*

The Rêve and the Néa each hold one mode for their whole history, so neither can
prove which byte carries it — but this card ran both, and it records a **mean
pressure for every minute of every session**. That makes the question answerable
without any vendor output at all. Splitting all 467 archive blocks on word 6:

| word 6 | blocks | delivered p10–p90 spread | median pressure == word 10 |
|---|---|---|---|
| **0** | 5 | **0.10 cmH₂O** | **5 of 5** |
| 60 | 225 | 3.17 cmH₂O | 4 of 225 |

*(Counting only blocks with at least 30 minutes of valid pressure; over the full
archive 11 of 467 read CPAP.)*

A 0.10 cmH₂O spread is a flat line, and it sits on word 10 every time. So **word
6 = 0 is CPAP and 60 is A-PAP, and word 10 is the pressure prescribed for it** —
the value the vendor software prints in its "Prescribed pressure" column, and
leaves blank on every A-PAP row.

Both fields had been written off in this note: word 6 as a constant, word 10 as
"differs per device, and nothing the reports print is left to assign to it". The
second was true only because the report's *summary* page has no prescribed-pressure
row. The CSV export does.

Eleven blocks of this card were imported as an auto band the device never
titrated across, until the loader learned to read word 6.

## `Y17` on the S.Box — tested as an event source, rejected

With no `.LOG`, `Y17` is the only per-sample channel that could plausibly carry
events, so it was tested rather than assumed.

Over the six days the vendor report covers (35.6 h of recording), `Y17` is
97.5 % zero and takes 15 distinct values, all combinations of seven bits. Every
run of every bit is **exactly 0.80 s** — 20 samples at 25 Hz. It marks instants
at fixed width; it carries no event extent.

| bit | pulses | per hour | vendor index for the nearest event type |
|---|---|---|---|
| 0 | 683 | 19.2 | FL runs 11.1/h |
| 2 | 67 | 1.9 | obstructive AI 1.7/h |
| 3 | 2703 | 75.9 | snoring 88.4/h |
| 4 | 70 | 2.0 | — |
| 5 | 540 | 15.2 | — |
| 6 | 1 | 0.03 | — |
| 7 | 23 | 0.6 | central AI 0.8/h |

Bits 2 and 7 land close to the obstructive and central apnoea indices, and bit 3
is the most frequent bit against snoring as the most frequent event, but bits 0
and 5 have no counterpart, and no bit or pair of bits reproduces the hypopnoea
indices (obstructive 5.8/h, central 6.5/h).

The same test was run on the Rêve, where a `.LOG` provides ground truth. There,
bit 3 aligns with snore records and bit 0 with flow-limitation starts, but both
over-count by 1.5–2×, and apnoea and hypopnoea correspondence is weak. **`Y17`
is not a usable event source on either device.**

> The Rêve note's statement that `Y17` runs "do not line up" with `.LOG` records
> is too strong and should be revisited: the alignment test there used a ±2 s
> window, and a constant offset larger than that hid a real correspondence for
> two of the six event types. The conclusion — `Y17` cannot supply events —
> survives; the reasoning does not.

`DET` (127 distinct values on this card) and `NSD` (82) were also profiled; both
behave as the Rêve note describes and neither resembles an event marker.

---

## Open problems

1. **Event resolution is one minute, and there are no durations.** Every event of
   every type can be placed in the minute it occurred, but not at a second, and
   the card records no event length. Whether the analyzer's own waveform pages
   place flags more precisely than this — and if so from what — is not settled.
   *(Two earlier revisions of this note were wrong here: the first said no event
   source existed at all, because it searched the memory image for the Rêve's
   49-byte log-record shape; the second said byte 2 could not be decoded, on the
   strength of six blocks rather than all 470.)*
2. ~~**Word 10 is the only unassigned field.**~~ **RESOLVED 2026-08-26: word 10
   is the prescribed pressure and word 6 is the therapy mode.** Both readings
   this note previously offered were wrong — word 10 is not a per-device
   calibration constant, and word 6 is not a constant at all. See below, and
   `SEFAM_REVE_CARD_ANALYSIS.md`, "The therapy mode is byte 17".

   **The humidifier and heated tube levels are not in this record at all**: both
   were located in `upload.dat`, both sit outside the twelve fields, and neither
   is recoverable from a card the device wrote. That is a real limit on what a
   loader can ever report for this model, not a gap waiting to close.
3. ~~**The practitioner and patient ramp times are not separated.**~~
   **RESOLVED 2026-08-26: word 1 is the patient's, word 2 the practitioner's
   ceiling.** Settled by this card's own session archive and by a Néa card. See
   "The patient access lock, and how the ramp is stored".
4. **The settings table's slot ordering** is still not understood. The offset is
   now confirmed on two devices, and "last non-zero slot" gives the right
   current settings on both, but the ring's write order does not.
5. **The rest of the 2 MiB image is undescribed.** The header's two hex-text
   configuration blocks are byte-identical between the two snapshots apart from
   the timestamp and two three-byte fields, and the device lifetime counters the
   report prints must be somewhere.

---

## What would still help

- **A bi-level card** — an S.Box Duo, Néa Duo or Ventea. Word 6 has been seen
  holding A-PAP and CPAP, and the vendor DLL exports `IsBilevel*` beside
  `IsCPAP`, so a third value almost certainly exists. The loader reports anything
  it does not recognise as A-PAP, and warns.
- **An analyzer report for card B**, or for any second S.Box, to confirm the
  circuit and mask leak on a second device rather than resting both on card A.
- **A card whose analyzer report shows a settings change at a known session
  boundary**, to test whether change times can be inferred rather than read.
- **Confirmation of whether the analyzer scores events itself.** If it does, the
  absence of an event store on the card is the answer, not a gap, and the loader
  should say so plainly rather than importing waveforms silently.
