# BMC G3X `.set` File Format — Device Settings

**Date:** 2026-07-28
**Status:** Decoded, cross-validated, and **fully implemented** (2026-08-01) in
`BmcG3xData::DecodeTsBlock()` for all four modes plus the comfort settings, reading the
**per-day TS block from the IDX** in preference to `.set`. Air tube type remains the only
undecoded field and is suppressed rather than guessed.
**Reference cards:**
- **E5 B25A Plus** reference card — firmware `E5-1.02.05.02`, `.set` is 2048 B
- **G3 A20** reference card — firmware `G3-2.12.54.13`, `.set` is 1536 B

---

## The settings are duplicated per-night inside the IDX

**The `.set` file is not the only copy.** Every populated IDX day record (0x800 bytes,
starting at IDX offset 0x800) contains five tagged 256-byte sub-blocks:

| Record offset | Tag | Content |
|---|---|---|
| 0x080 | `IT` | daily statistics (AHI, event counts, pressures) |
| **0x180** | **`SS`** | **system settings — byte-identical to the `.set` SS block** |
| 0x280 | `TS` | per-day therapy values |
| 0x380 | `UH` | not decoded |
| 0x480 | `UP` | not decoded |

On the reference card the IDX `SS` block matches the `.set` `SS` block **256 bytes out of
256, on all 19 nights**. The same additive checksum applies (95 of 95 sub-blocks verified).

Two consequences:

1. **Any write-back experiment must patch both files.** Editing only `.set` leaves an
   untouched copy of the same settings in the IDX — which is why a first round of
   single-byte `.set` probes produced no change in PAP-Link on any of seven bytes.
2. **The per-day copy is the more correct source for OSCAR.** `.set` holds the device's
   *current* configuration, so applying it to historical nights is wrong the moment a
   setting is changed — every past night would be relabelled with today's values. The IDX
   carries what was actually in effect each night. `ApplySetFileSettings()` currently reads
   `.set`; switching it to the per-day IDX `SS`/`TS` blocks (falling back to `.set`) would
   fix that. Not yet done — no card in hand has a mid-history settings change to verify
   against, and on this card the two agree exactly.

The IDX `TS` block uses the **same field layout** as the `.set` `TS` block. Its values vary
night to night (e.g. offset 0x0E takes 750/800/850 across this card's nights where `.set`
holds a single 750) simply because the settings were genuinely changed over time — `.set`
holds only the current values, the IDX holds what was in force on each night.

> An earlier revision of this note claimed the IDX `TS` block was "not simply a copy" and
> that the `.set` field map should not be assumed to apply. That was wrong, and was based
> only on seeing the values differ. Corrected 2026-08-01 against an external decode
> validated 15/15 against PAP-Link.

## Why this matters

`BmcG3xData::ReadDateSession()` **never opens the `.set` file**. It does:

```cpp
std::memset(&dateSession.MacineSettings, 0, sizeof(BmcMachineSettings));
dateSession.MacineSettings.Mode = BmcMode::CPAP;
```

and then *infers* pressure settings from the IDX **observed daily pressure summary**
(`ItPressureMinHundredths` / `TsPressureMaxHundredths`), setting
`Mode = AutoCPAP` whenever `max > min`.

Consequences reported by a user comparing OSCAR against PAP-Link on the E5 card:

| Setting | OSCAR showed | Actual (PAP-Link) | Cause |
|---|---|---|---|
| Mode | AutoCPAP | AutoS | Hard-coded `CPAP`, then inferred from observed pressures |
| Pressure Max | 10.89 | Max IPAP 15.0 | 10.89 is an *observed* value, not a setting |
| Pressure Min | 7.5 | Min EPAP 7.5 | Coincidence — observed min equalled the setting |
| PS | n/a | 3.5 | Never parsed |
| Initial P | 7.5 | Initial EPAP 5.5 | Never parsed |
| Mask / Tube / Auto On / Auto Off / Ramp / SmartB | defaults | real values | Never parsed (`memset` 0) |

Note that a settings value of `10.89` is itself the tell: real BMC settings are always
whole multiples of 0.5 cmH₂O.

---

## File layout

The file is a sequence of **256-byte blocks**. Block 0 is a header; the rest are tagged
by their first two ASCII bytes.

| Tag | Meaning |
|---|---|
| `SH` | Settings header — timestamp of last change + change counter |
| `SS` | System settings (non-therapy: mask, tube, ramp, auto on/off, …) |
| `TS` | Therapy settings for **one** mode |

### Block checksum (offset 0xFE, u16 LE)

The last two bytes of every block are a **little-endian uint16 sum of bytes 0x00–0xFD**
of that block — a plain additive checksum, not a CRC.

```python
checksum = sum(block[0:254]) & 0xFFFF      # stored LE at block[254:256]
```

Confirmed on **all 14 blocks across both reference cards**, with no exceptions, and by a
round-trip test: recomputing every checksum over unmodified data reproduces the original
file byte-for-byte.

This is what makes controlled write-back experiments possible — a byte can be changed and
the block re-checksummed so the file stays internally consistent (see "What would settle
it" below).

It also explains an oddity noted earlier: the stale and live AutoS blocks differ in content
yet carry the same trailer. Their byte sums simply happen to be equal, which an additive
checksum cannot distinguish. Treat the trailer as an integrity check only — never as a
block identity.

Observed block sequences:

```
E5 B25A Plus:  SH  SS  TS(6)  SS  TS(0) TS(1) TS(2) TS(6)
G3 A20:        SH  SS  TS(1)  SS  TS(0) TS(1)
```

So the file is: header, then **section A** = `SS` + `TS` for the active mode, then
**section B** = `SS` + a `TS` preset for every mode the device supports.

The two `SS` blocks are byte-identical in both cards.

### Which block is authoritative

Section A and section B disagree on the E5 card:

| AutoS field | Section A (0x200) | Section B (0x700) | PAP-Link |
|---|---:|---:|---:|
| Min EPAP | 9.00 | **7.50** | **7.5** |
| Max IPAP | 17.00 | **15.00** | **15.0** |
| Initial EPAP | 6.00 | **5.50** | **5.5** |
| PS | 4.00 | **3.50** | **3.5** |

**Section B is current.** Three independent confirmations:

1. PAP-Link's displayed values match section B exactly on all four fields.
2. The IDX *observed* daily pressure minimum for this card is 7.50 cmH₂O — impossible if
   Min EPAP were section A's 9.00.
3. On the G3 A20 card section A's `TS` and section B's `TS(1)` are **byte-identical**,
   so the rule is consistent there too.

Section A appears to be a stale snapshot (settings as of some earlier point). Do not use it.

**Rule: use the last `TS` block whose mode byte equals `SS[28]`.**

---

## `SH` block (offset 0x000)

| Offset | Type | Meaning | E5 B25A Plus | G3 A20 |
|---|---|---|---|---|
| 0x00 | char[2] | `"SH"` | | |
| 0x0C | 6 bytes | Timestamp of last settings change | 2026-07-08 22:24:06 | 2026-06-11 09:19:36 |
| 0x12 | u16 LE | Settings-change counter | 101 | 962 |

The timestamp uses the same encoding as `DecodeG3xTimestamp()` in
`bmcG3xDataParsing.cpp`: `year = 1900 + byte0`, then month, day, hour, minute, second.
The 8-byte record at 0x0C is repeated at 0x14, 0x20 and 0x28.

---

## `TS` block — therapy settings

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | char[2] | `"TS"` |
| 0x08 | u8 | **Mode** — matches the existing `BmcMode` enum |

The mode byte values line up exactly with `BmcMode` in `bmcDataParsing.h`
(`CPAP=0, AutoCPAP=1, S=2, ST=3, T=4, Titration=5, AutoS=6`).

All pressure fields are **u16 LE, hundredths of cmH₂O**. Field positions are
**mode-specific** — the block is effectively a union overlaid per mode.

### Mode 0 — CPAP
| Offset | Meaning | E5 B25A Plus | G3 A20 |
|---|---|---:|---:|
| 0x0A | Set pressure | 8.00 | 15.00 |
| 0x18 | Initial pressure | 4.00 | 15.00 |

### Mode 1 — AutoCPAP
| Offset | Meaning | E5 B25A Plus | G3 A20 |
|---|---|---:|---:|
| 0x0E | Min pressure | 6.00 | 6.00 |
| 0x10 | Max pressure | 15.00 | 15.00 |
| 0x18 | Initial pressure | 4.00 | 6.00 |

### Mode 2 — S
| Offset | Meaning | E5 B25A Plus |
|---|---|---:|
| 0x0A | EPAP | 4.00 |
| 0x12 | IPAP | 12.00 |
| 0x18 | Initial EPAP | 4.00 |

### Mode 6 — AutoS ✅ *confirmed against PAP-Link, 5/5 fields*
| Offset | Meaning | E5 B25A Plus | PAP-Link |
|---|---|---:|---:|
| 0x0E | Min EPAP | 7.50 | 7.5 ✓ |
| 0x16 | Max IPAP | 15.00 | 15.0 ✓ |
| 0x18 | Initial EPAP | 5.50 | 5.5 ✓ |
| 0x1A | **Rise time, milliseconds** (u16) | 300 | 300 ms ✓ |
| 0x2A | PS | 3.50 | 3.5 ✓ |

**Rise time (0x1A)** was initially mis-read as a pressure (300 → "3.00 cmH₂O"). It is
milliseconds. Two independent checks:

- 300 is the **only** uint16 equal to 300 anywhere in the 2048-byte file.
- Across this card's five `TS` blocks it reads 0 (CPAP), 0 (AutoCPAP), 200 (S),
  300 (current AutoS), 100 (stale AutoS) — zero exactly for the single-pressure modes,
  which have no IPAP/EPAP transition to ramp.

**Still unidentified in the AutoS block:** bytes 0x30, 0x31, 0x32 — see below.

### Confidence

- **Mode byte (0x08) and the AutoS field map:** high — the AutoS values match PAP-Link
  exactly on all four reported fields.
- **CPAP / AutoCPAP / S field maps:** medium — self-consistent and plausible across two
  devices, but **no ground-truth readout has been obtained** for those modes. The AutoCPAP
  map agrees on min/max across both cards, which is encouraging but not proof.

---

## CONFIRMED FIELD MAP — supersedes the speculation further down

Contributed by an external reverse engineer (2026-08-01) with a Python reference decoder,
and **validated 15/15 against a PAP-Link readout** of the reference card, including every
field this note previously listed as unidentified. Independently re-validated here against
a second card (G3 A20, AutoCPAP) where it also produces self-consistent values matching
that card's own `.set` block.

All offsets are within a `TS` block — either the per-day copy at IDX `record+0x280` or a
block in `.set`. Both use this same layout.

### Comfort — present in every mode

| Offset | Size | Field | Encoding |
|---|---|---|---|
| 0x82 | u8 | Ramp Time | minutes; `0`=Off; **Auto if `flags & 0x01`** |
| 0x86 | u8 | Reslex | `0`=Off, else 1–3 |
| 0x87 | u8 | Humidifier | `0`=Off, else 1–5; **Auto if `flags & 0x04`** |
| **0x8A** | u8 | **Flags** | bit0 Ramp=Auto, bit2 Humidifier=Auto, bit5 I Sens=Auto, bit6 E Sens=Auto |
| 0x8C | u8 | Auto On | `0`=Off, `1`=On |
| 0x8D | u8 | Auto Off | `0`=Off, `1`=On |
| 0x8E | u8 | Mask Type | `0`=Full Face, `1`=Nasal, `2`=Nasal Pillows, `3`=Other |
| 0xB3 | u8 | Leak Alert | `0`=Off, `1`=On |

### Mode (0x08)

`0`=CPAP, `1`=AutoCPAP, `2`=S, `6`=AutoS. Matches OSCAR's existing `BmcMode` enum.

### Per-mode therapy fields

All pressures are u16 LE in hundredths of cmH₂O.

| Field | Offset | CPAP | AutoCPAP | S | AutoS |
|---|---|:-:|:-:|:-:|:-:|
| Treat P / EPAP | 0x0A | ✓ | | ✓ | |
| Min APAP / Min EPAP | 0x0E | | ✓ | | ✓ |
| Max APAP | 0x10 | | ✓ | | |
| IPAP | 0x12 | | | ✓ | |
| Max IPAP | 0x16 | | | | ✓ |
| Initial P / Initial EPAP | 0x18 | ✓ | ✓ | ✓ | ✓ |
| Rise Time (u16, **ms**) | 0x1A | | | ✓ | ✓ |
| Ti Min (u8, ×0.1 s) | 0x1E | | | ✓ | |
| Ti Max (u8, ×0.1 s) | 0x20 | | | ✓ | |
| Pressure Support | 0x2A | | | | ✓ |
| I Sens (u8) | 0x30 | | | ✓ | ✓ |
| E Sens (u8) | 0x31 | | | ✓ | ✓ |
| Pres. Response (u8) | 0x32 | | ✓ | | ✓ |
| Backup RR (u8) | 0x34 | | | ✓ | |
| Smart C/A/B (u8) | 0x36 | ✓ | ✓ | | ✓ |

- **I Sens / E Sens:** `7`=Very High, `6`=High, `5`=Medium High, `4`=Medium,
  `3`=Medium Low, `2`=Low, `1`=Very Low. Auto per the flags byte.
- **Pres. Response:** `1`=Standard, `2`=Soft, `3`=Fast.
- **Smart:** `0`=Off, `1`=On — same offset, named per mode (SmartC / SmartA / SmartB).

### Smart sub-block — 0x60–0x77 ✅ *confirmed against PAP-Link, all modes*

When the Smart flag at `0x36` is on, the device runs a second pressure set that PAP-Link
displays under the SmartA / SmartB / SmartC heading. Those values live in their own
sub-block at `0x60`, in the same u16 LE hundredths-of-cmH₂O encoding.

Confirmed 2026-08-10 from a contributor's purpose-built dataset: one day record was
rewritten with TS blocks in three different modes, each imported into PAP-Link and
screenshotted. Every value below was read back off the vendor display. All four IDX
variants carry a valid block checksum.

| Field | Offset | CPAP | AutoCPAP | AutoS |
|---|---|:-:|:-:|:-:|
| Smart P / Smart EPAP | 0x6A | ✓ 14.00 | ✓ 9.50 | ✓ 11.00 |
| Smart Min P / Smart Min EPAP | 0x6E | — | ✓ 9.50 | ✓ 11.50 |
| Smart Max P | 0x70 | — | ✓ 17.00 | |
| Smart Min IPAP | 0x72 | | | ✓ 13.50 |
| Smart Max IPAP | 0x74 | | | ✓ 19.00 |

- **Smart PS is not stored.** Derive it: `PS = [0x72] - [0x6E]`, i.e. Smart Min IPAP
  minus Smart Min EPAP. On the reference block 13.50 - 11.50 = 2.00, matching PAP-Link's
  "Smart PS 2.0 cmH₂O". This is the mirror of the main block, where Min IPAP is the
  derived value and PS is stored at `0x2A`.
- **`0x6A` is patch-proven**, not merely value-matched: changing it from `03 B6` (9.50)
  to `04 4C` (11.00) in an AutoCPAP record moved PAP-Link's "Smart P" from 9.5 to 11.0
  and left Smart Min P and Smart Max P untouched. The rest of the table is confirmed by
  exact value match across three modes.
- **`0x68` mirrors the main Initial pressure** at `0x18` in all three modes (4.50 / 7.00
  / 8.50). PAP-Link shows no separate "Smart Initial", so treat it as a copy rather than
  a distinct setting.
- **CPAP mode still populates `0x6E` and `0x70`** (8.50 and 14.50 on the reference
  block) even though PAP-Link displays only Smart P there. Do not publish them in CPAP
  mode — the vendor does not.
- `0x60` and `0x62` hold the same small value twice (41 in two records, 38 in the third);
  unexplained. `0x64`, `0x66` and `0x76` are `0xFFFF` filler.
- `0x70` is zero on every AutoS day observed, consistent with Smart Max P being an
  AutoCPAP-only field.

#### Gate on the flag at 0x36, never on the values

The sub-block is **not** cleared when the feature is switched off. Across the 34 day
records of the reference card, 29 have Smart off — and three of those still carry a full,
plausible set of values (6.50 / 8.50 / 8.50 / — / 10.50 / 15.00) left over from when the
feature was last configured. The other 26 are zeroed.

So the device sometimes zeroes the block and sometimes does not. A loader that decides
"Smart is in use" by testing whether the values are non-zero will invent Smart settings on
those nights, where PAP-Link shows none. The flag at `0x36` is correct on all 34 days;
it is the only reliable gate.

> **Correction to the contributor's note:** Smart Max P was reported at `0x79`. That
> offset is odd-aligned and lands inside the `0xFF` filler. The value 17.00 is at
> **`0x70`**, matching the main block's Max APAP at `0x10`.

The sub-block resembles the main block offset by `0x60` for the shared fields (`0x0A`→
`0x6A`, `0x0E`→`0x6E`, `0x10`→`0x70`) but diverges after that — Max IPAP is at `0x16`
in the main block and `0x74` here, not `0x76`. Treat the offsets as empirical.

### Air tube type — 0x8F

Offset located 2026-08-02 by a contributor who changed the setting on the device, powered it
off and captured the IDX. The byte uses the **same encoding as `BmcAirTubeType` and as the
legacy BMC IDX field**, so it maps across directly:

| Raw | Meaning |
|---|---|
| `0` | Unheated 19 mm |
| `1` | Unheated 15 mm |
| `2` | Heated 19 mm |
| `3` | Heated 15 mm |

Sizes are BMC's own naming. BMC calls the standard hose **19 mm** (inner diameter) on its
settings screen and in its manuals; other vendors call the same physical tube 22 mm, after
its end connectors. OSCAR's rule is to report what the device reports, so BMC channels say
19 mm while e.g. `PRS1_HoseDiam` says 22 mm — see GitLab #256.

**Values 1 and 2 are confirmed.** A bilevel reference card carries `1` on every one of its
19 nights and PAP-Link reports "15mm normal" for all of them; the heated label for `2` was
checked against a heated G3. Value 1 also explains the original defect — the field was not
being read at all, so `AirTubeType` kept its zero default and a slim-hose card was reported
as the standard one.

Value `0` is not directly confirmed but is the only remaining unheated option, and a G3 A20
reference card carries it on 12 of its 172 nights (`2` on the other 160).

Value `3` has not been observed on any card yet; it is inherited from the legacy encoding.

> **Watch the device's own labels.** The BMC UI calls the 22 mm tube "19 mm" — inner
> diameter rather than outer — for what is the same physical tube. A contributor note
> derived from the device UI lists 15 mm as `0` and 19 mm as `1`, which is the opposite of
> what the data shows; the two values appear to be transposed there. The card and PAP-Link
> agree with each other on all 19 nights, so that is what is implemented. Worth keeping in
> mind for any future field located the same way.

### What this corrects

The `SS`-block candidate hunt recorded below was a dead end — Auto On, Auto Off, Ramp and
Mask all live in the **`TS`** block, not `SS`. That is why a round of single-byte `SS`
probes produced no change in PAP-Link on any of seven bytes. The `0x30`–`0x32` triple was
correctly located but mis-assigned: the correct order is I Sens, E Sens, Pres. Response.

## `SS` block — system settings

Only one field is confidently identified:

| Offset | Meaning | E5 B25A Plus | G3 A20 |
|---|---|---|---|
| 0x1C | **Active therapy mode** | 6 (AutoS) ✓ | 1 (AutoCPAP) |

This matches PAP-Link's reported mode on the E5 card, and agrees with the mode byte of
the last `TS` block on both cards.

Raw `SS` bytes 0x08–0x2F:

```
E5 B25A Plus  0f 00 00 00 0f 00 00 00 0f 00 00 00 0f 00 00 00
              00 00 65 00 06 00 00 04 ff 00 01 ff 00 00 01 ff
              ff 01 01 00 00 08 01 01
G3 A20        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
              00 00 65 00 01 01 00 00 ff 00 00 ff 00 00 00 ff
              ff 01 00 00 00 0b 01 00
```

`0x1A` = `65 00` (101) on **both** devices — a constant of unknown meaning, distinct from
the `SH` change counter (which is 101 on the E5 card but 962 on the G3 A20 card).

**The remaining `SS` fields — mask type, air-tube type, auto on, auto off, ramp time,
Reslex, humidifier — are still NOT decodable.** Not for lack of ground truth any more, but
because several settings share the same encoded value, so they cannot be told apart.

### Candidate narrowing (2026-07-29)

A PAP-Link readout of the E5 card gives nine known values: Auto On **on**, Auto Off **on**,
Ramp **Auto**, Air tube **15 mm**, Mask **Nasal**, Pres Response **Soft**, I Sens
**Med High**, E Sens **Medium**, Rise Time **300 ms**.

Expected encodings, taken from the legacy IDX parser and the registered channel options in
`bmc_loader.cpp`: Auto On / Auto Off on = 1; air tube 15 mm = 1 (`0`=Normal 22mm,
`1`=Normal 15mm); mask Nasal = 1 (`0`=Full Face, `1`=Nasal); ramp Auto = `0xFF` (the value
`bmc_loader.cpp` tests for).

**Four of those settings encode to the same value, 1.** The whole `SS` block has only five
offsets that read 1 on the E5 and 0 on the G3 A20:

```
0x05  0x22  0x26  0x2A  0x2F
```

Five candidates for four settings, all indistinguishable — every one of them is a plausible
Auto On / Auto Off / air-tube / mask byte. No assignment can be made from this card alone.

Every byte differing between the two cards, for reference:

| SS offset | E5 | G3 A20 | note |
|---|---:|---:|---|
| 0x05 | 1 | 0 | candidate |
| 0x08, 0x0C, 0x10, 0x14 | 15 | 0 | four u32s all = 15; unexplained |
| 0x1C | 6 | 1 | **active mode** (confirmed) |
| 0x1D | 0 | 1 | |
| 0x1F | 4 | 0 | |
| 0x22, 0x26, 0x2A, 0x2F | 1 | 0 | candidates |
| 0x2D | 8 | 11 | |
| 0xFE | — | — | block trailer |

Ramp = Auto could not be located: no byte is `0xFF` on the E5 where it is not also `0xFF`
on the G3 A20, and the block's tail is `0xFF` filler, so the ramp field may not live in
`SS` at all.

### Pres Response / I Sens / E Sens — likely the `TS` triple at 0x30–0x32

Three consecutive `TS` bytes hold three small values, and all three changed together
between the stale and current AutoS snapshots:

| TS block | mode | 0x30 | 0x31 | 0x32 |
|---|---|---:|---:|---:|
| CPAP | 0 | 0 | 0 | 0 |
| AutoCPAP | 1 | 0 | 0 | 1 |
| S | 2 | 6 | 2 | 0 |
| AutoS (current) | 6 | 5 | 4 | 2 |
| AutoS (stale) | 6 | 7 | 5 | 3 |

Three bytes, three unaccounted-for settings (Pres Response, I Sens, E Sens), zero on CPAP
where trigger sensitivities do not apply. `0x31` = 4 is consistent with I Sens "Med High"
on a 1–5 scale, but `0x30` = 5 does not fit E Sens "Medium" on that same scale, so the
mapping is **not** established. Recorded as a lead, not a decode.

### What would settle it

Two readings of the **same** card differing in exactly **one** setting. Each such pair pins
one byte outright.

The obvious route — ask the device owner to change a setting and re-export — is slow and
depends on someone else. **The faster route is to run the experiment backwards:** edit a
byte in a copy of the `.set` file, fix that block's checksum (above), open the result in
PAP-Link, and see which displayed setting moved. That makes the byte the independent
variable and needs no device access at all.

For this to be sound, each probe file must change exactly one candidate byte and leave
every already-decoded field intact, so any observed change is unambiguously attributable.
Worth checking per probe: the active-mode byte still selects the same `TS` block, and the
five confirmed AutoS values are unchanged.

Two practical points:

- The two `SS` copies (0x100 and 0x300) are byte-identical and it is not known which one is
  authoritative, so a probe should change **both** — otherwise a null result is ambiguous
  between "wrong byte" and "wrong copy".
- For `TS`, change only the **live** block for the active mode. The stale snapshot is a
  useful control: if editing it changed the display, the block-selection rule would be
  wrong.

Note that four of the target settings (Auto On, Auto Off, air tube 15 mm, mask Nasal) all
encode to `1` on the reference card, which is exactly why passive comparison stalled — but
they map to four *differently named* fields in PAP-Link, so a one-byte-at-a-time probe
attributes them cleanly.

---

## Implementation notes

The display plumbing already exists — `bmc_loader.cpp` (~lines 273–331) already maps
every `BmcMode` including `AutoS` onto OSCAR channels:

```cpp
if (machineSettings.Mode == BmcMode::AutoS) {
    oscarSession->settings[CPAP_EPAPLo]        = machineSettings.AutoS_MinEPAP;
    oscarSession->settings[CPAP_IPAPHi]        = machineSettings.AutoS_MaxIPAP;
    oscarSession->settings[BMC_INITIAL_EPAP]   = machineSettings.AutoS_InitialEPAP;
    ...
}
```

and the mode channel already registers `addOption(6, "AutoS")`. The G3X path is the only
thing that never populates `BmcMachineSettings`. So the work is confined to
`BmcG3xDataParsing` — no new channels or UI required.

**Regression risk:** existing G3 A20 / B20A users currently get min/max from the observed
IDX pressure summary. Switching them to `.set` values changes what they see (to the
*correct* values, but it is a visible change). Consider whether to apply this to all G3X
devices or gate it initially.

---

## What was implemented (2026-07-29)

`BmcG3xData::ApplySetFileSettings()` in `bmcG3xDataParsing.cpp`, called from both exit
paths of `ReadDateSession()` (the EVT-only path and the waveform path) *after* the existing
pressure inference, so decoded settings supersede inferred ones.

**AutoS only.** The function reads `SS[0x1C]` for the active mode and takes the **last**
`TS` block carrying that mode. If the active mode is not AutoS it returns false and the
inferred settings stand — so CPAP and AutoCPAP devices are untouched. Confirmed by
simulation against both reference cards:

| Card | Active mode | Result |
|---|---|---|
| E5 B25A Plus | 6 = AutoS | block 0x700 → 7.5 / 15.0 / 5.5 / PS 3.5 — matches PAP-Link 4/4 |
| G3 A20 | 1 = AutoCPAP | falls through, inferred settings kept |

Fields populated: `Mode`, `AutoS_MinEPAP`, `AutoS_MaxIPAP`, `AutoS_InitialEPAP`,
`AutoS_PS` (new field added to `BmcMachineSettings`), and `AutoS_MinIPAP` **derived** as
`minEPAP + PS` — exact for a fixed-PS mode, and not read from any confirmed field.

A range check (`4 ≤ minEPAP ≤ 25`, `minEPAP ≤ maxIPAP ≤ 30`) guards against a corrupt or
misread block; on failure the function logs a warning and keeps the inferred settings.

**One related fix in `bmc_loader.cpp`:** the AutoS branch hard-coded
`settings[CPAP_PS] = 0` despite mapping the session to `MODE_BILEVEL_AUTO_FIXED_PS`, a
*fixed pressure-support* mode. It now reports `AutoS_PS`. Parsers that cannot recover PS
leave the field at 0, preserving previous behaviour — notably the legacy BMC parser, which
does not populate it.

### Consistency check

Max IPAP 15.0 with PS 3.5 implies a maximum EPAP of 11.5. The observed daily pressure
maximum on this card is 10.89 cmH₂O, inside the resulting `[7.5, 11.5]` EPAP band — an
independent check that the field map is right.
