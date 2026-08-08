# SD Card Fingerprints — Real-World Samples

Concrete file-listing and header fingerprints of real SD cards from CPAP devices
OSCAR handles. Useful when evaluating an unknown card: a new card can be matched
against the **observed** layout of an existing format, not just the minimum
sentinel each loader's `Detect()` requires.

Companion docs:
- `Notes/loaders/LOADER_DETECTION_PATTERNS.md` — general detection strategies and per-loader sentinels.
- `Notes/loaders/AEONMED_AS100_CARD_ANALYSIS.md` — example of comparing a new card against this catalogue.

Convention for each entry:
- Top-level tree (sizes in bytes for exactness).
- Header magic / identity offsets, byte-precise.
- What the loader's `Detect()` actually checks vs. what the real card carries.
- Notable extras and open questions.

---

## BMC G3X (G3 A20)

**Sample:** `V:/TestFiles/JCCPAP G3X-2`
**Device:** BMC G3 A20, serial `A3123456789`, firmware `G3-2.SC.72.01`, part code `110A40113`.
**Loader:** `bmcg3x_loader.cpp` + `bmcG3xDataParsing.cpp`.

### File set

```
<basename = serial>
├── A3123456789.000          67,108,864 bytes  (exactly 64 MiB)  ─┐
├── A3123456789.001          67,108,864 bytes                     │  63 numbered waveform
├── A3123456789.002          67,108,864 bytes                     │  data files (.000–.062),
│   …                                                             │  each exactly 64 MiB
├── A3123456789.062          67,108,864 bytes                    ─┘
├── A3123456789.evt          68,897,920 bytes  (~65.7 MiB, event log)
├── A3123456789.idx             325,632 bytes  (index; carries identity header)
├── A3123456789.log             609,280 bytes  (firmware/diagnostic log)
├── A3123456789.set              17,796 bytes  (device settings)
├── nP3-35.CHN                  …                                ─┐
├── nP3-35.ENG               4,925,496 bytes                      │  9 UI-language
├── nP3-35.ESP                  …                                 │  localization files
├── nP3-35.FRA / .GER / .GRE / .ITA / .POL / .POR  …             ─┘  (BMC firmware payload)
```

Total: 76 files at the root, ~4 GiB. The card has been filled to a near-fixed
budget — 63 × 64 MiB looks like a pre-allocated ring/chunk buffer for waveforms.

### What `Detect()` checks vs. what's there

`bmcg3x_loader.cpp:19` requires only:
1. The legacy BMC loader does **not** also claim the path (no `*.USR` triplet).
2. Some `*.idx` whose first 32 bytes start with magic `"BMC G/E/P INDEX"`.
3. A sibling `*.000` (same basename).

Everything else (`.evt`, `.log`, `.set`, the 62 extra `.NNN` files, the 9
language files) is invisible to `Detect()` but consumed by `BmcG3xData::ReadData()`.

### IDX header — confirmed byte-precise layout

Read from offset 0 of `A3123456789.idx` (matches the comment block at
`bmcG3xDataParsing.cpp:1695`):

| Offset | Length | Field | Value on this sample |
|---|---|---|---|
| `0x0000` | 16 | Magic | `"BMC G/E/P INDEX"` + NUL |
| `0x0020` | 4 | Sub-marker | `"BMC"` + NUL |
| `0x0030` | 16 | **Serial** | `A3123456789` |
| `0x0048` | 16 | **Part / config code** | `110A40113` (not the human model) |
| `0x0100` | 16 | **Product name** | `G3 A20` (human-readable model) |
| `0x0120` | 4 | Compatibility code | `A31` ASCII |
| `0x0140` | 22 | Cloud endpoint | `data2.icodeconnect.com` (BMC upload server) |
| `0x0162` | 8 | Sibling model code | `G4600` |
| `0x0200` | 2 | Record marker | `"SF"` (settings frame, 256-byte block follows) |
| `0x0233` | 16 | Serial (repeat) | `A3123456789` |
| `0x0270` | ~26 | Hyphenated hex group ID | `5B2A4236-43373408-54483441` (looks like device-pairing key) |
| `0x0300` | 2 | Record marker | `"MF"` (machine frame) |
| `0x0345` | 20 | **Firmware build** | `G3-2.SC.72.01` (internal SC build string) |

Each "frame" block is 256 bytes (`0x100`), padded with `0xFF`; the last 2 bytes
of each block (`0x0?FE..0x0?FF`) carry a non-`FF` checksum-like trailer
(e.g. `0x2B11`, `0xD98D`, `0xDBB6`).

### Notable extras / observations

- **`.evt` is huge** (~64 MiB) — events live in their own bulk file, parallel to
  waveform chunks. Worth checking whether this is pre-allocated or grows with
  use; the size being close to (but not exactly) 64 MiB hints at "almost-full
  fixed budget".
- **`.set` is small and likely text/structured-binary** — 17 KB suggests this is
  the user-visible device settings dump, separate from the runtime `SF` frames
  inside `.idx`.
- **`.log` is mid-sized** (~600 KB) and named to match what
  `bmcG3xDataParsing.cpp:1693` calls the "companion `.log` file" used for
  firmware-version extraction.
- **Language files (`nP3-35.*`)** are payload for the device's own UI — not
  consumed by OSCAR. Their presence does, however, distinguish a "freshly
  prepared" BMC G3X card from a partial dump (these are large and unlikely to
  be missing on a real device).
- **Cloud endpoint** `data2.icodeconnect.com` in the IDX is a permanent
  identifier — useful as an additional content-magic if the BMC top-of-file
  ASCII ever varies between firmware versions.
- **Numbered files always exactly 64 MiB** — strong invariant for sanity
  checks; if a `*.NNN` is a different size, the card is truncated or corrupt.

### Open questions

- Are `.NNN` files filled sequentially (`.000` → `.062`) or as a ring buffer?
  Index header probably says — worth verifying against
  `BmcG3xData::ResolveIdxFile()` and the day-entry table.
- The `5B2A4236-…` group ID at offset `0x0270` — appears once in IDX, not yet
  decoded. May be a cloud-pairing key irrelevant to OSCAR.
- Whether older G3X firmware uses different IDX magic (the code uses
  `startsWith("BMC G/E/P INDEX")`, so trailing-byte differences are tolerated).

---

## BMC Luna G3 (legacy BMC loader)

**Sample:** `V:/TestFiles/BMC Luna`
**Device:** BMC Luna G3 ("G3 B25A"), serial `B3924B12345`, firmware `G3-2.00.77.01`.
**Loader:** `bmc_loader.cpp` + `bmcDataParsing.cpp` (the "legacy" BMC loader).
G3X loader explicitly defers when `*.USR` is present (`bmcg3x_loader.cpp:27`).

### File set

```
<basename = last-8-chars of full ID>
├── 24B02412.000           16,777,216 bytes  (exactly 16 MiB)  ─┐
├── 24B02412.001           16,777,216 bytes                     │  30 numbered waveform
│   …                                                            │  data files (.000–.029),
├── 24B02412.029           16,777,216 bytes                     ─┘  each exactly 16 MiB
├── 24B02412.USR            1,160,471 bytes  (user/identity — legacy sentinel)
├── 24B02412.idx               62,976 bytes  (index; no G3X magic)
├── 24B02412.evt              861,408 bytes  (event log)
├── 24B02412.log              647,424 bytes  (firmware/diagnostic log)
```

Total: 34 files at the root, ~480 MiB allocated. **No language files** —
Luna G3 firmware is smaller-footprint than the G3X touchscreen lineage.

### What `Detect()` checks vs. what's there

`BmcData::DirectoryHasBmcData()` (`bmcDataParsing.cpp:465`) requires:
1. Exactly one `*.USR` in the directory (`GetUsrFilePath` returns `nullptr`
   if multiple are present).
2. A sibling `<basename>.idx`.
3. A sibling `<basename>.000`.

The `.evt` and `.log` and the additional 29 numbered files (`.001`–`.029`) are
invisible to `Detect()` but read by the parser.

### IDX header — legacy BMC layout

The legacy IDX file is **structurally different from G3X**:

| Offset | Length | Field | Value on this sample |
|---|---|---|---|
| `0x0000` | 8 | Filler — eight ASCII spaces (`0x20`×8) | (no "BMC G/E/P INDEX" magic) |
| `0x0020` | 16 | Header bytes | `01 00 00 08 00 02 c8 00 c8 00 …` |
| `0x0034` | 16 | **Full device ID** (model+serial concatenated) | `B3924B12345` |
| `0x005C` | 4 | Data-file extension reference | `.bpd` (not present on this card; older format extension?) |
| `0x006F` | 4 | Data-file extension reference | `.000` (current extension actually used) |
| ≥ `0x80` | … | `0xFF` padding then sparse fixed-stride records | |

ASCII strings further into the IDX list firmware-pack expectations:

| String | Meaning |
|---|---|
| `BMCG2.UPK`, `CSP.UPK` | Firmware update package names — Luna runs on BMC's **G2 platform** |
| `nP2-24`, `nP2-35` | UI language-pack file basenames (Luna firmware uses `nP2-` prefix, G3X uses `nP3-`) |
| `SW-200`, `WL-100`, `SG-200`, `WL-200` | Sibling product/model codes (BMC G2 family) |
| `dl.tmp` | Download/update temp filename |

### USR file — legacy BMC user/identity

The `.USR` is the **legacy-only sentinel**, not present on G3X cards. Header:

| Offset | Length | Field | Value on this sample |
|---|---|---|---|
| `0x0000` | 4 | Header magic | `53 01 47 83` (`S.G.` — possibly "Settings" sub-format `01`, version `0x8347`) |
| `0x0034` | 16 | Full device ID | `B3924B12345` (also in IDX) |
| `0x004D` | 3+ | Firmware string fragment | `G3-` (truncated in first 0x80 bytes — full string in `.log`) |
| `0x0050+` | … | Packed binary settings, mostly `0xFF`-padded | |

Strings extracted from full USR:

| String | Meaning |
|---|---|
| `G3 B25A` | **Product name** (corresponds to "Luna G3" in user-facing material) |
| `data.icodeconnect.com` | BMC cloud endpoint (G3X uses `data2.icodeconnect.com` — note the `2`) |
| `LG 3700` | Possibly a sibling/lineage product code |

### LOG file — useful identifier strings

| String | Meaning |
|---|---|
| `B3924B12345` | Device ID (repeated) |
| `G3-2.00.77.01` | **Firmware version** (analogous to G3X's `G3-2.SC.72.01`) |
| `24B02412.idx`, `24B02412.evt` | Self-references — the firmware logs its own filenames |
| `310170403583672` | 15-digit numeric ID (IMEI-shaped — likely cellular modem ID for cloud uploads) |

### Legacy BMC vs G3X — side-by-side discriminators

When fingerprinting an unknown BMC card, these distinguish the two generations:

| Feature | Legacy (Luna G3) | G3X (G3 A20) |
|---|---|---|
| `*.USR` file | **Present** (1.1 MB) — sentinel | **Absent** |
| `.idx` magic at offset 0 | 8 spaces, then sparse data | ASCII `"BMC G/E/P INDEX"` |
| `.idx` size | ~63 KB | ~318 KB |
| Numbered data chunks | 16 MiB each | 64 MiB each |
| Numbered chunk count (observed) | 30 (`.000`–`.029`) | 63 (`.000`–`.062`) |
| `.evt` size | ~860 KB | ~64 MiB (huge) |
| Language files on-card | **No** (`nP2-*` referenced only in IDX) | **Yes** (`nP3-35.*` — 9 files) |
| Firmware platform string | `BMCG2.UPK` (G2 platform) | (`SF`/`MF` frame markers in IDX) |
| Cloud endpoint | `data.icodeconnect.com` | `data2.icodeconnect.com` |
| Product naming | `G3 B25A` (Luna) | `G3 A20` |
| Firmware version pattern | `G3-2.00.77.01` | `G3-2.SC.72.01` |
| Filename basename | Last 8 chars of full ID (`24B02412` from `B3924B12345`) | Full serial (`A3123456789`) |

### Observations / open questions

- Both generations share the `.idx + .evt + .log + .NNN` skeleton — BMC has a
  consistent platform philosophy, only the IDX header and the `.USR` are
  diverging artefacts of the G2 → G3X firmware evolution.
- `.bpd` extension referenced in legacy IDX at `0x5C` but not present on this
  card — older BMC firmware may have used `.bpd` for chunk files before
  switching to numbered `.NNN`.
- `LG 3700` in the USR — uninterpreted; possibly a sibling product family.
- BMC "G2 platform" devices (Luna G3) and "G3X-platform" devices (G3 A20) are
  both branded "G3" by BMC — the OSCAR loader names ("BMC legacy" vs "BMC G3X")
  reflect the firmware platform, not the marketing generation.

---

## ResMed AirCurve 10 VAuto (AS10 platform)

**Sample:** `C:/Users/Guy/OneDrive/house/Medical/CPAP - APAP/FlashPAP/Guy VAuto`
**Device:** ResMed AirCurve 10 VAuto, serial `23201575820`, firmware `24_M36_V9`,
product code `37094`.
**Loader:** `resmed_loader.cpp`.

### File set

```
<root>/
├── DATALOG/                                     2011 per-day folders
│   ├── 20201015/                                (oldest day)
│   │   ├── <yyyymmdd>_<hhmmss>_EVE.edf + .crc   events
│   │   ├── <yyyymmdd>_<hhmmss>_BRP.edf + .crc   waveform (largest)
│   │   ├── <yyyymmdd>_<hhmmss>_PLD.edf + .crc   per-minute / logged data
│   │   └── <yyyymmdd>_<hhmmss>_SAD.edf + .crc   statistics / apnoea data
│   …
│   └── 20260517/                                (newest day)
│       ├── 20260517_225558_EVE.edf      1,088 bytes
│       ├── 20260517_225605_BRP.edf    100,302 bytes  (short session)
│       ├── 20260517_225606_PLD.edf     12,442 bytes
│       ├── 20260517_225606_SAD.edf      3,686 bytes
│       ├── 20260517_231306_BRP.edf  3,088,966 bytes  (full session)
│       ├── 20260517_231307_PLD.edf    272,066 bytes
│       ├── 20260517_231307_SAD.edf     84,030 bytes
│       └── (matching .crc files, 8 bytes each)
├── Identification.tgt          282 bytes   text key-value identity file
├── Identification.crc            4 bytes   binary CRC32 of Identification.tgt
├── SETTINGS/                              42 files: config groups and operation logs
│   ├── <X>GL.tgt + <X>GL.crc             21 config groups (X ∈ A,B,C,D,E,I,M,N,P,R,S,U,V,X)
│   ├── QXH.tgt+.crc, QXJ.tgt+.crc        extra config sets
│   └── ABR.log, DLL.log, ELI.log, ERR.log, TRR.log,
│       TXC.log, TXE.log, TXH.log, TXW.log, ZRL.log
└── STR.edf                  115,708 bytes  cumulative summary EDF
```

Total: 2011 day-folders, ~5.5 years of nightly data (Oct 2020 → May 2026).
The card has not been wiped — older AS10 cards may show similar long retention.

### What `Detect()` checks vs. what's there

`resmed_loader.cpp:309` requires:
1. `DATALOG/` directory at root.
2. `STR.edf` file at root.

That's it. `Identification.tgt`/`.json` is consumed by `PeekInfo()`
(`resmed_loader.cpp:332`) to extract device identity, not by `Detect()`. The
`SETTINGS/` folder isn't gated either — the loader can run without it (degraded).

### `Identification.tgt` — text key-value identity (AS10 platform)

Plain ASCII, one `#KEY VALUE` pair per blank-line-separated record:

| Key | Meaning | Value on this sample |
|---|---|---|
| `#IMF` | Identification Major Format version | `0001` |
| `#VIR` | Vendor Interface Revision | `0066` |
| `#RIR` | Reference Interface Revision | `0064` |
| `#PVR` | Product Version Revision | `0065` |
| `#PVD` | Product Version Detail | `001A` |
| `#CID` | Color/Config ID | `CX036-009-013-026-102-100-101` |
| `#RID` | Revision ID | `000D` |
| `#VID` | Vendor ID | `0009` |
| `#SRN` | **Device serial number** | `23201575820` |
| `#SID` | Software ID | `SX567-0401` |
| `#PNA` | **Product name** | `AirCurve_10_VAuto` |
| `#PCD` | Product code | `37094` |
| `#PCB` | PCB code (GS1 barcode-format) | `(90)R370-7518(91)T1(21)02064459` |
| `#MID` | Manufacturer ID | `0024` |
| `#FGT` | **Firmware tag** | `24_M36_V9` |
| `#BID` | Bootloader ID | `SX577-0200` |

This is the **AS10 identity format**. The **AS11 platform** (AirSense 11) uses
`Identification.json` instead — a JSON tree under
`FlowGenerator.IdentificationProfiles.Product` (see `resmed_loader.cpp:347-369`).
Both platforms share everything else (DATALOG / STR.edf / SETTINGS).

### EDF (European Data Format) — standard on-card encoding

ResMed uses the **standard EDF specification** for every data file. STR.edf
header at offset 0 (first 256 bytes are fixed-width ASCII):

```
0 [version=0]        X X X X 4EEF 6A0B   (patient ID / fixed signature)
Startdate 19-APR-2025 X X X SRN=23201575820  MID=36  VID=9
19.04.25 12.00.00  25088  EDF             394   86400.0097
```

Fields decoded:
- `Startdate` field carries `19-APR-2025` — ResMed's start-date convention is
  `DD-MMM-YYYY` (uppercase 3-letter month).
- Recording-ident line includes `SRN=`, `MID=`, `VID=` — same identifiers as in
  `Identification.tgt`.
- Header gives `25088` data records of `86400.0097` seconds each — one record
  per day, ~25 thousand days range (well beyond actual data span — a fixed
  allocation).
- `394` is the channel-count field in the EDF header.

### Per-session EDF filename suffixes

Each session emits four EDFs into the day's folder:

| Suffix | Meaning | Typical size |
|---|---|---|
| `_BRP` | **B**reath **R**ate / waveform **P**rofile — flow & pressure waveforms (full-rate samples) | 100 KB short / 3 MB full session |
| `_PLD` | **P**er-minute **L**ogged **D**ata (low-rate channels: leak, AHI, mask pressure) | 10–300 KB |
| `_SAD` | **S**ettings / **A**pnoea **D**ata — event statistics summary | 3–85 KB |
| `_EVE` | **EVE**nts — apnoea/hypopnoea/leak events with timestamps | 1–5 KB |

Filename pattern: `<yyyymmdd>_<hhmmss>_<suffix>.edf`.
Within a single day, sessions are distinguished by their `<hhmmss>` timestamp.

### SETTINGS/ folder contents

| File pattern | Purpose |
|---|---|
| `<X>GL.tgt` + `<X>GL.crc` | Global config groups — one pair per parameter family (one-letter prefix). 21 pairs observed. |
| `QXH.tgt + QXH.crc`, `QXJ.tgt + QXJ.crc` | Additional "Q-prefix" config sets — likely Quick-settings/heuristics. |
| `*.log` (10 files: ABR/DLL/ELI/ERR/TRR/TXC/TXE/TXH/TXW/ZRL) | Operation logs — firmware diagnostics, errors, transitions, warnings, etc. |

`*.tgt` files are small (12–230 bytes) — these are binary or compact-text
config blobs, each with a CRC sibling.

### CRC sibling convention

CRC files are all **binary** (raw CRC bytes, not ASCII hex). Sizes vary by file class:

- **Root `Identification.crc`** — 4 bytes binary.
- **`SETTINGS/<X>GL.crc`** — 4 bytes binary per `.tgt` config group. No `.crc` sidecar
  for `*.log` operation logs in this AS10 sample (the 10 `.log` files have no CRC).
- **`DATALOG/<basename>.crc`** — 8 bytes binary per `.edf`. (Previously documented
  here as ASCII hex; that was wrong — `od -c` confirms raw binary.)
- **No root `STR.crc`** on AS10 — `STR.edf` is uncovered.

### AS10 vs AS11 — quick discriminator

When fingerprinting an unknown ResMed AS-series card:

| Feature | AS10 (this sample) | AS11 |
|---|---|---|
| Identification file | `Identification.tgt` (`#KEY VALUE` text) | `Identification.json` (nested JSON) |
| Loader-side identity extraction | `parseIdentLine()` line-by-line | `scanProductObject()` walks JSON |
| Everything else (DATALOG, STR.edf, SETTINGS, EDF format, CRC pattern) | Identical | Identical |

`resmed_loader.cpp:339` checks for `Identification.json` first and warns if both
exist ("somebody is reusing an SD card w/o re-formatting").

### Observations / open questions

- **EDF format is a major OSCAR-friendly trait** — ResMed is the only mainstream
  CPAP brand using a documented international waveform standard. This makes
  parser scope predictable.
- **Two CRC encodings** on the same card is unusual — worth noting in case a
  future loader needs CRC verification across platforms.
- **Long history on this card is accumulated, not native.** ResMed devices
  *do* auto-prune the SD card (typical on-card retention is much shorter than
  the 5.5 years shown here). The 2011 day-folders in this sample came from
  copying the card contents to a backup periodically and rebuilding the
  combined view off-card. OSCAR import performance against very-large cards
  is still worth thinking about, but a typical user's live card will be
  considerably smaller than this.
- **The 25088 data-records / 86400-sec stride in STR.edf** corresponds to ~68
  years of allocation. STR.edf is a rolling summary that grows over the device's
  lifetime, but the header pre-declares a much larger ceiling.
- **GS1 barcode PCB code** (`#PCB`) and **15-digit IMEI-shaped SRN** are
  consistent identifiers across the AS10 line; useful for cross-referencing
  warranty/regulatory databases if needed.

---

## ResMed AirSense 10 CPAP (second AS10 sample — summary-only model)

**Sample:** `C:/Users/Guy/OneDrive/house/Medical/CPAP - APAP/FlashPAP/Teresa CPAP`
**Device:** ResMed AirSense 10 CPAP (basic CPAP), serial `23171962445`,
firmware `24_M36_V3`, product code `37015`.
**Loader:** `resmed_loader.cpp` (shares everything with the VAuto entry above).

This sample serves two purposes: confirms the AS10 framework holds across CPAP
vs bilevel variants, and documents the **AirSense 10 CPAP basic** model's
known limitation — it doesn't write detailed per-session data, only daily
summary records into `STR.edf`.

### Top-level structure — identical to AirCurve VAuto

Same skeleton: `DATALOG/` + `Identification.tgt` + `Identification.crc` +
`SETTINGS/` + `STR.edf`. The 42-file `SETTINGS/` contents are byte-for-byte the
same filename set as VAuto (different content, identical layout).

### What's different on this card

| Field | Teresa (AS10 CPAP) | Guy (AC10 VAuto) | Notes |
|---|---|---|---|
| `#PNA` | `AirSense_10_CPAP` | `AirCurve_10_VAuto` | The human-readable model — single field decides device type |
| `#PCD` | `37015` | `37094` | Numeric SKU per model |
| `#FGT` | `24_M36_V3` | `24_M36_V9` | Firmware suffix differs per variant |
| `#VID` | `0003` | `0009` | **Vendor/Variant ID — appears to be a per-model numeric ID independent of `#PNA`** |
| `#SID` | `SX567-0306` | `SX567-0401` | Software ID — `SX567-` prefix is common, suffix per model |
| `#VIR` / `#PVR` | `0065` / `0064` | `0066` / `0065` | Slight rev offsets (older firmware shipped with the CPAP) |
| `#PCB` | `(90)R370-7421(91)B1…` | `(90)R370-7518(91)T1…` | Different PCB family within the AS10 line |
| `#CID` | `CX036-003-013-026-101-100-100` | `CX036-009-013-026-102-100-101` | Color/config tuple — second triplet (003 vs 009) mirrors `#VID` |
| `#BID`, `#MID`, `#IMF` | `SX577-0200`, `0024`, `0001` | Same | These are **AS10-platform invariants** — don't help distinguish models |
| STR.edf size | 49,608 bytes | 115,708 bytes | Scales with retained history |
| STR.edf channel-count field | `252` | `394` | **Bilevel devices declare ~50% more channels** (IPAP + EPAP + trigger/cycle vs single CPAP pressure) |
| STR.edf record count | `13824` | `25088` | Pre-allocated ceilings, not actual usage |
| DATALOG folders | 212 | 2011 | Days with a folder — not necessarily days with data |
| Date range | 2017-10-19 → 2018-06-19 (~9 months) | 2020-10-15 → 2026-05-17 (~5.5 years) | |

### Notable: every DATALOG folder is empty — and that is expected

All 212 day-folders under `DATALOG/` exist but contain **zero session EDFs**.
This is **normal output for the AirSense 10 CPAP (basic)** — that model
records daily summary statistics into `STR.edf` only and does **not** write
the per-session `_BRP/_PLD/_SAD/_EVE` files that the other AS10-family devices
(AutoSet, Elite, AirCurve bilevel variants) produce. The device is therefore
of limited use for OSCAR's detailed analysis: only the daily roll-ups in
`STR.edf` are loadable; waveform and event-level analysis aren't available.

**Loader behaviour against this card:**
- `Detect()` returns `true` — both sentinels (`DATALOG/` and `STR.edf`) are
  present.
- `PeekInfo()` parses `Identification.tgt` successfully — device identity
  shows correctly.
- `Open()` succeeds; OSCAR shows daily-summary channels (AHI, leak, hours,
  pressure) from `STR.edf` but no waveforms or detailed events.

This makes the AirSense 10 CPAP recognisable from `Identification.tgt` alone
(`#PNA AirSense_10_CPAP`, `#VID 0003`, `#PCD 37015`) before even looking at
DATALOG. Worth flagging to users early — "your device doesn't record the
detail OSCAR needs for charts" — rather than presenting an apparently
successful import that lacks the expected views.

### What this sample adds to the fingerprint vocabulary

- **`#VID` is a stable per-model numeric ID.** When matching an unknown AS10
  card to a known model without parsing `#PNA`, `#VID` should suffice.
- **STR.edf channel count is a fast bilevel-vs-CPAP discriminator** (~252 for
  CPAP, ~394 for bilevel) — readable from the EDF header without scanning the
  full file.
- **AS10-platform invariants** (`#MID 0024`, `#BID SX577-0200`, `#IMF 0001`,
  `SETTINGS/` file set) hold across CPAP/Auto/Elite/VAuto/ST/ASV — useful when
  fingerprinting a future AS10 sibling, since these fields confirm
  "AS10-platform" without telling you the specific model.

---

## ResMed AirSense 11 AutoSet (first AS11 sample)

**Sample:** `C:/Users/Guy/Downloads/Brucemac_Airsense11_39523 Bruce McKenzie/ResMed-39523-23233658718`
**Device:** ResMed AirSense 11 AutoSet, serial `23233658718`, product code
`39523` (USA region), firmware application `SW04600.11.8.0.0.a65babae1`,
hardware revision `R390-7667`.
**Loader:** `resmed_loader.cpp` — same loader as AS10, with AS11-specific
branches gated on the presence of `Identification.json`.

This is the **first AS11 sample** in the catalogue and the counterpart to the
two existing AS10 entries (AirCurve VAuto and AirSense 10 CPAP basic). It
documents the AS11-specific deltas: a JSON identity replacing the AS10
`#KEY VALUE` text file, a JSON `CurrentSettings` replacing the 42-file
`SETTINGS/*GL.tgt + *.log` cluster, two new per-session EDF suffixes
(`_CSL` and `_SA2`), a pre-allocated `journal.jnl` at the root, and the
removal of per-session `.crc` sidecars from `DATALOG/`.

### Card-root structure

```
<root>/
├── DATALOG/                                     2 per-day folders (small sample)
│   ├── 20240707/                                5 sessions in this OSCAR day
│   │   ├── <yyyymmdd>_<hhmmss>_CSL.edf  832     Cheyne-Stokes annotations
│   │   ├── <yyyymmdd>_<hhmmss>_EVE.edf 832-1k   events
│   │   ├── <yyyymmdd>_<hhmmss>_BRP.edf 7K-1.3M  waveform (largest)
│   │   ├── <yyyymmdd>_<hhmmss>_PLD.edf 3K-122K  per-minute logged data
│   │   └── <yyyymmdd>_<hhmmss>_SA2.edf 1K-54K   statistics / apnoea data
│   └── 20240708/                                empty (no sessions yet)
├── Identification.json    762 bytes  JSON device identity (AS11 format)
├── Identification.crc       4 bytes  binary CRC32 of Identification.json
├── SETTINGS/
│   ├── CurrentSettings.json  2,315 bytes  JSON settings snapshot
│   └── CurrentSettings.crc       4 bytes  binary CRC32
├── STR.edf              20,920 bytes  cumulative summary EDF (small — fresh card)
├── journal.jnl          16,384 bytes  pre-allocated, all-zero on this card
└── System Volume Information/  Windows artefact — not part of the format
```

5 sessions on 2024-07-07 (start at 22:13, 22:14, 23:20, 00:57, 02:55 — one
real night straddling midnight), no sessions on 20240708 — this is a fresh
card with just 2 OSCAR days of data.

### What `Detect()` checks vs. what's there

`resmed_loader.cpp:309` requires:
1. `DATALOG/` directory at root.
2. `STR.edf` file at root.

Identical to AS10 — `Detect()` makes no AS10/AS11 distinction. The
branching happens in `PeekInfo()` (`resmed_loader.cpp:332`), which first
probes for `Identification.json`: if present, AS11 JSON path; otherwise
falls back to the legacy `Identification.tgt` parser. The comment at
line 339 reads *"Check for AS11 file first, just in case"* — and line 345
warns *"Old Ident.tgt file is ignored"* if both are present (someone
reusing an AS10 card without reformatting).

### `Identification.json` — JSON identity (AS11 format)

Single-line JSON with the full identity tree under
`FlowGenerator.IdentificationProfiles`:

```json
{"FlowGenerator":{"IdentificationProfiles":{
  "Product":{
    "UniversalIdentifier":"6e2fa67e-61a7-4d56-a81e-841be8d3cf29",
    "SerialNumber":"23233658718",
    "SerialNumberVerificationCode":"",
    "ProductCode":"39523",
    "ProductName":"AirSense 11 AutoSet",
    "FdaUniqueDeviceIdentifier":"",
    "ProductGeographicIdentifier":"USA"},
  "Hardware":{
    "HardwareIdentifier":"(90)R390-7667(91)1S001(21)2237F54933"},
  "Software":{
    "BootloaderIdentifier":"SW04601.00.1.1.0.736edbdfd",
    "ApplicationIdentifier":"SW04600.11.8.0.0.a65babae1",
    "ConfigurationIdentifier":"CF04600.11.03.00.a65babae1",
    "PlatformIdentifier":46,
    "VariantIdentifier":3,
    "RegionIdentifier":0,
    "ProfileVariationIdentifier":"00000000-0000-3000-8000-000011046003",
    "DataVersionIdentifier":11,
    "DataModelVersionIdentifier":"2.3.0.46f0081fc"}}}}
```

Cross-walk to the AS10 `#KEY VALUE` schema:

| AS10 (`Identification.tgt`) | AS11 (`Identification.json`) | This sample |
|---|---|---|
| `#SRN` serial number | `Product.SerialNumber` | `23233658718` |
| `#PNA` product name | `Product.ProductName` | `AirSense 11 AutoSet` |
| `#PCD` product code | `Product.ProductCode` | `39523` |
| `#PCB` PCB GS1 barcode | `Hardware.HardwareIdentifier` | `(90)R390-7667(91)1S001(21)2237F54933` |
| `#FGT` firmware tag | `Software.ApplicationIdentifier` | `SW04600.11.8.0.0.a65babae1` |
| `#BID` bootloader ID | `Software.BootloaderIdentifier` | `SW04601.00.1.1.0.736edbdfd` |
| `#VID` numeric variant | `Software.VariantIdentifier` | `3` |
| `#MID` (manufacturer / platform ID) | `Software.PlatformIdentifier` | `46` |
| (none — new) | `Product.UniversalIdentifier` (UUID) | `6e2fa67e-61a7-…` |
| (none — new) | `Product.ProductGeographicIdentifier` | `USA` |
| (none — new) | `Software.DataModelVersionIdentifier` | `2.3.0.46f0081fc` |

The loader's `scanProductObject()` (`resmed_loader.cpp:331` forward decl) walks
the `Product` object and populates the same `MachineInfo` struct that
`parseIdentLine()` fills on AS10.

### `AS11TestedModels` whitelist — this sample is *not* in it

`resmed_loader.cpp:69-80` (gated on `INCLUDE_AS_MODEL_VERIFICATION`) lists:

```
39420, 39421, 39423, 39463, 39483, 39485, 39491, 39494, 39517, 39520
```

PCD **`39523`** (this card) is **not** in the list. In normal builds the
whitelist is compiled out, so import proceeds regardless; it only matters
for tester builds, where this device would log "untested model" warnings.
Worth noting as a candidate for the list if a future round confirms the
loader handles it cleanly, but per project policy this is **a candidate
pending verification, not a prescribed code change** — import has to be
exercised end-to-end before any whitelist entry is added.

### Per-session EDF filename suffixes — two changes from AS10

```
20240707_221311_CSL.edf      <-- NEW vs AS10
20240707_221311_EVE.edf      same as AS10
20240707_221318_BRP.edf      same as AS10
20240707_221318_PLD.edf      same as AS10
20240707_221318_SA2.edf      <-- renamed from SAD on AS10
```

| Suffix | AS10 | AS11 | Loader-side handling |
|---|---|---|---|
| `_BRP` | yes | yes | Waveform import (unchanged) |
| `_PLD` | yes | yes | Per-minute import (unchanged) |
| `_EVE` | yes | yes | Event import (unchanged) |
| `_SAD` | yes | — | AS10 statistics file |
| `_SA2` | — | yes | AS11 statistics file — `resmed_loader.cpp:2570` maps **both** `SAD` and `SA2` to internal type `EDF_SAD` |
| `_CSL` | — | yes | Cheyne-Stokes annotations — `resmed_loader.cpp:2572` maps to `EDF_CSL`; `LoadCSL()` at line 3425 parses `CSR Start` / `CSR End` annotation pairs and emits a `CPAP_CSR` event list. Annotation-only EDF (no signal channels) |

The `_CSL` file is small (832 bytes — header only, plus annotations) — a
new way ResMed externalises Cheyne-Stokes data that on AS10 was buried in
the statistics file.

### `SETTINGS/` — radically simplified from AS10

AS10 carries ~42 files: 21 `<X>GL.tgt + .crc` config-group pairs, two `Q*`
sets, and 10 `*.log` operation-log files. AS11 collapses all of that to a
**single** `CurrentSettings.json` + `CurrentSettings.crc` pair. The JSON
holds the entire active configuration as a structured tree:

```json
"ActiveProfiles":{
  "TherapyProfile":"AutoSetProfile",
  "FeatureProfiles":["ComfortFeature","EprFeature","AutoRampFeature",
    "SmartStartStopFeature","CircuitFeature","ClimateFeature",
    "LanguageFeature","UserSolutionFeature","TemperatureFeature",
    "PatientViewFeature","TimeZoneFeature","CareCheckFeature",
    "DeviceHealthFeature","ReminderFeature","DisplayFeature",
    "MaskSenseFeature"]},
"TherapyProfiles":{
  "AutoSetProfile":{"TherapyMode":"AutoSet","MaxPressure":15.0,
                    "MinPressure":5.0,"StartPressure":4.0},
  "AutoSetForHerProfile":{...},
  "CpapProfile":{...}},
"FeatureProfiles":{
  "ComfortFeature":{"AutoSetComfort":"Off"},
  "EprFeature":{"EprEnable":"On","EprType":"Ramp","EprPressure":2},
  "AutoRampFeature":{"RampEnable":"Auto","RampTime":20},
  ...}
```

Active mode on this device is `AutoSetProfile` with pressure range
**5–15 cmH₂O**, start pressure 4, ramp Auto/20 min, EPR Ramp/+2,
SmartStart+Stop on, climate manual (humidifier off, heated tube 27°C
auto), patient view Full, USA TZ −07:00.

**Loader use of `CurrentSettings.json`: none.** A grep of `resmed_loader.cpp`
for `CurrentSettings` returns no matches. The AS11 settings file is **not
read by the loader** — settings come from STR.edf signals like on AS10. The
JSON is therefore "human-friendly extra," useful for fingerprinting but not
currently consumed by OSCAR.

### `journal.jnl` — pre-allocated, all-zero on this card

A 16,384-byte root file consisting entirely of `\0` bytes on this sample
(`tr -d '\0' | wc -c` returns 0). Not referenced anywhere in
`resmed_loader.cpp` (no grep matches). Most likely a write-ahead /
transaction journal the device pre-allocates at format time and lazily
populates during use; on a lightly-used card it stays zero. **Useful as
an AS11 marker** — AS10 cards don't have it.

### CRC sidecar convention — simplified from AS10

| File | AS10 | AS11 |
|---|---|---|
| Root identity CRC | `Identification.crc` — 4 bytes binary | `Identification.crc` — 4 bytes binary (same) |
| SETTINGS CRC | `<X>GL.crc` per config group — 8 bytes ASCII-hex | `CurrentSettings.crc` — 4 bytes binary |
| DATALOG per-session CRC | Yes — 8-byte ASCII-hex sibling per `.edf` | **None** — `find DATALOG -name "*.crc"` returns zero matches on this card |

AS11 drops the per-session CRC sidecars entirely. Plausibly the
filesystem-level / EDF-level integrity is now considered sufficient, or
verification has moved into the device's internal journal.

### AS10 vs AS11 — discriminator table (updated with first AS11 data)

| Feature | AS10 | AS11 (this sample) |
|---|---|---|
| Identity file | `Identification.tgt` (`#KEY VALUE` ASCII) | `Identification.json` (single-line JSON) |
| Settings folder | `SETTINGS/<X>GL.tgt + .crc` ×21 + `Q*` + 10 `*.log` files (~42 entries) | `SETTINGS/CurrentSettings.json + .crc` (2 entries) |
| Pre-allocated journal | None | `journal.jnl` (16KB, all-zero on fresh card) |
| Per-session EDF suffixes | `_BRP _PLD _SAD _EVE` | `_BRP _PLD _SA2 _CSL _EVE` |
| Per-session CRC sidecar | Yes (8-byte binary per `.edf`) | None |
| SETTINGS CRC encoding | 4-byte binary (per `.tgt`) | 4-byte binary (single `CurrentSettings.crc`) |
| Identity CRC encoding | 4-byte binary | 4-byte binary (unchanged) |
| `Detect()` requirements | `DATALOG/` + `STR.edf` | `DATALOG/` + `STR.edf` (unchanged) |
| STR.edf record stride | `86400.0097` sec/day | `86400.0078` sec/day (marginal — same daily cadence) |
| Loader entry path | `parseIdentLine()` | `scanProductObject()` (probed first) |

### Observations / open questions

- **JSON identity is a net win for fingerprinting.** No ambiguous key
  abbreviations, structured nesting, UUIDs for cross-device tracking. A
  future AS11 sibling sample should be trivial to extract identity from
  without bespoke parsing.
- **`PlatformIdentifier:46` and `DataModelVersionIdentifier:"2.3.0"`** look
  like the cleanest AS11-generation discriminators if future AS11 hardware
  ships with newer revisions — both are explicit numeric/string fields,
  unlike the AS10 `#PCB` GS1 barcode which has to be substring-matched.
- **Whitelist gap.** PCD `39523` not in `AS11TestedModels`. **Candidate
  pending end-to-end verification** — same policy as the Prisma model
  candidates: don't prescribe the code edit until the loader is confirmed
  to import this device's data correctly.
- **The `CurrentSettings.json` is currently OSCAR-invisible.** Worth
  thinking about whether some AS11-specific settings (e.g. `MaskSenseToggle`,
  `SoundcheckFeatureToggle`, `PatientView`) deserve display in OSCAR —
  AS10 cards expose comparable settings via the `<X>GL.tgt` parsing path
  that AS11 has obsoleted. Out of scope for fingerprinting but documented
  for future loader work.
- **STR.edf is small (20 KB) because the card is fresh** — only a couple
  days of summary data. The full-history STR.edf on a long-running AS11
  card should grow comparably to AS10 (~100 KB+) and is worth resampling
  once such a card surfaces, to confirm whether the AS11 STR.edf schema
  differs from AS10's `394`-channel layout.
- **`journal.jnl` worth resampling on an actively-used card.** If on a
  long-running card it's still all-zero, it's vestigial; if it carries
  content, there's a parsing target the loader is currently ignoring.

---

## ResMed S9 AutoSet (first S9-platform sample — the oldest ResMed format)

**Sample:** `C:/Users/Guy/Downloads/S9_SD_Card/S9_SD_Card`
**Device:** ResMed S9 AutoSet, serial `23141699984`, product code `36005`,
firmware `21_S9_ASET_EPR`, software ID `SX474-1203`, bootloader `SX525-0300`.
**Loader:** `resmed_loader.cpp` — the S9 path predates AS10 and uses a
**flat DATALOG layout** (no per-day subdirectories). The loader's
`ScanFiles()` (`resmed_loader.cpp:1221`) explicitly comments *"First list
any EDF files in DATALOG folder - Series 9 devices"* — it handles flat S9
files and per-day AS10/AS11 subdirectories in the same scan.

This is the **first S9 sample** in the catalogue and completes the ResMed
platform progression: **S9 (2011-era) → AS10 (2014) → AS11 (2021)** all
served by one loader. Its main interest is showing how the S9 format
differs structurally from the modern (AS10/AS11) layout while sharing the
same EDF data-file format and `Identification.tgt` `#KEY VALUE` identity
scheme.

### Card-root structure

```
<root>/
├── DATALOG/                                    flat — no per-day folders
│   ├── 20191208_231340_EVE.edf  +.crc          one set of 3 EDFs per session
│   ├── 20191208_231340_PLD.edf  +.crc          (no BRP on every session — see below)
│   ├── 20191208_231340_SAD.edf  +.crc
│   ├── 20191209_044354_EVE.edf  +.crc
│   …                                           1290 .edf+.crc files = 215 sessions
│   └── 20200204_032934_SAD.edf  +.crc          newest
├── Identification.tgt    211 bytes  text key-value identity (S9 format)
├── Identification.crc      4 bytes  binary CRC32
├── SETTINGS/                                   35 files: 11 GL.tgt + 4 *.log + CRCs
│   ├── AGL.tgt/.crc, BGL.tgt/.crc, CGL.tgt/.crc, DGL.tgt/.crc, EGL.tgt/.crc,
│   ├── IGL.tgt/.crc, MGL.tgt/.crc, NGL.tgt/.crc, PGL.tgt/.crc, RGL.tgt/.crc,
│   ├── SGL.tgt/.crc, VGL.tgt/.crc, XGL.tgt/.crc                (13 config groups, 4-byte CRCs)
│   └── ABR.log/.crc, ERR.log/.crc, TXE.log/.crc, ZYL.log/.crc  (4 op-logs, 8-byte CRCs)
├── STR.edf            42,844 bytes  cumulative summary EDF
├── STR.crc                 8 bytes  binary CRC of STR.edf (AS10/AS11 don't have this)
└── Journal.dat        32,768 bytes  pre-allocated, mostly 0xFF (NAND erased-state)
```

Date span: 2019-12-08 → 2020-02-04 (~2 months, 215 sessions, ~1.3 sessions
per OSCAR day). Filenames are `<yyyymmdd>_<hhmmss>_<suffix>.edf` exactly as
on AS10/AS11 — only the **directory layout** changes.

### What `Detect()` checks vs. what's there

`resmed_loader.cpp:309`: `DATALOG/` directory at root, plus `STR.edf` file at root.
Both present. Identical detection criteria across S9/AS10/AS11.

### `Identification.tgt` — S9 key set is a **subset** of AS10's

Same `#KEY VALUE` text format as AS10 but with two S9-specific keys (`#VRN`,
`#VPC`, `#NID`) and missing several AS10 keys (`#IMF`, `#VIR`, `#PVR`,
`#PVD`, `#CID`):

```
#VRN 20141441219     <-- S9-only: long numeric, possibly "version revision number"
#VPC 3688P           <-- S9-only: product code (alt encoding of #PCD)
#NID 0001            <-- S9-only: numeric ID
#VID 0001            <-- variant ID
#SRN 23141699984     <-- serial number
#SID SX474-1203      <-- software ID (SX474 prefix — distinct from AS10's SX567)
#RID 0064            <-- revision ID
#PNA S9_AutoSet      <-- product name (underscore-joined, same as AS10 style)
#PCD 36005           <-- product code
#PCB 7239S1-45153526 <-- PCB code (S9 dash format, NOT the GS1 barcode format AS10 uses)
#MID 001A            <-- manufacturer ID (hex; STR.edf shows decimal 26)
#FGT 21_S9_ASET_EPR  <-- firmware tag (much more readable than AS10's `24_M36_V9`)
#BID SX525-0300      <-- bootloader ID (SX525 prefix vs AS10's SX577)
```

Key discriminators vs AS10/AS11:

| Key | S9 (this sample) | AS10 (VAuto) | AS10 (CPAP) | AS11 (AutoSet) |
|---|---|---|---|---|
| `#PNA` | `S9_AutoSet` | `AirCurve_10_VAuto` | `AirSense_10_CPAP` | (`ProductName: "AirSense 11 AutoSet"`) |
| `#PCD` | `36005` | `37094` | `37015` | (`ProductCode: "39523"`) |
| `#FGT` | `21_S9_ASET_EPR` | `24_M36_V9` | `24_M36_V3` | (`ApplicationIdentifier: "SW04600.11.8…"`) |
| `#SID` prefix | `SX474-` | `SX567-` | `SX567-` | (`SW04600.11.…`) |
| `#BID` prefix | `SX525-` | `SX577-` | `SX577-` | (`SW04601.…`) |
| `#MID` | `001A` (=26) | `0024` (=36) | `0024` | (`PlatformIdentifier: 46`) |
| `#VRN` / `#VPC` / `#NID` | present | absent | absent | absent |
| `#IMF` / `#VIR` / `#PVR` / `#PVD` / `#CID` | absent | present | present | (different JSON keys) |
| `#PCB` format | `7239S1-45153526` (dash) | `(90)R370-7518(91)T1(21)…` (GS1) | `(90)R370-7421…` (GS1) | (`HardwareIdentifier` in GS1) |

Each platform generation has its own ID-key vocabulary. The `#PCB` format
change is the clearest visual discriminator — S9 uses a simple dash-joined
PCB part-number, AS10/AS11 use GS1 barcode notation.

### DATALOG layout — flat, not per-day

The most structural difference from AS10/AS11. S9 dumps every session's
EDFs directly into `DATALOG/`:

```
DATALOG/20191208_231340_EVE.edf
DATALOG/20191208_231340_PLD.edf
DATALOG/20191208_231340_SAD.edf
DATALOG/20191209_044354_EVE.edf
…
```

AS10/AS11 use `DATALOG/<yyyymmdd>/<yyyymmdd>_<hhmmss>_*.edf`.

The loader handles both by listing `DATALOG/*.edf` first (line 1225) for S9,
then iterating subdirectories (line 1241+) for AS10/AS11. Year-only
4-character directory names (line 1246) are recognised as backup-tree
folders — used when OSCAR maintains its own copy of the data outside the
card.

### Per-session EDF suffixes — three on S9, four on AS10, five on AS11

| Suffix | S9 | AS10 | AS11 | Purpose |
|---|---|---|---|---|
| `_EVE` | yes | yes | yes | Events (apnoea/hypopnoea) |
| `_PLD` | yes | yes | yes | Per-minute logged data |
| `_SAD` | yes | yes | renamed `_SA2` | Statistics / apnoea data |
| `_BRP` | yes (only on full sessions) | yes (every session) | yes | Waveform (Breath Rate Profile) |
| `_CSL` | — | — | yes | Cheyne-Stokes annotations |

On this S9 card, `_BRP` files appear less frequently than the other three —
many sessions have only `_EVE/_PLD/_SAD` triplets. Likely the device only
writes waveform data when the session crosses some duration threshold.
Worth confirming on a second S9 sample.

### SETTINGS/ — S9 has fewer config groups than AS10 and **has** `.log` CRCs

| Aspect | S9 (this sample) | AS10 |
|---|---|---|
| Config groups (`*GL.tgt`) | 13 (A,B,C,D,E,I,M,N,P,R,S,V,X) | 21 (adds F,G,H,J,K,O,Q,T,U,W and others) plus `Q*` files |
| `*GL.crc` size | 4 bytes binary | 4 bytes binary |
| Operation logs (`*.log`) | 4 (ABR, ERR, TXE, ZYL) | 10 (ABR, DLL, ELI, ERR, TRR, TXC, TXE, TXH, TXW, ZRL) |
| `.log` CRC sidecars | **Yes** (8 bytes binary each) | **None** — `.log` files have no `.crc` on AS10 |

Two notable shifts:
- AS10 nearly doubled the SETTINGS surface (13 → 21 config groups, 4 → 10
  operation logs) — newer feature set means more configuration state to
  persist.
- AS10 dropped CRC protection on operation logs (probably non-critical
  diagnostic data; integrity not worth the write cost).

### `STR.crc` — present on S9 only

S9 writes `STR.crc` at the root: 8 bytes binary CRC of `STR.edf`. AS10 and
AS11 omit this — they trust EDF-internal integrity for the cumulative
summary. S9 was the most CRC-paranoid of the three generations.

### `Journal.dat` — 32 KB, mostly 0xFF (NAND erased state)

A 32,768-byte root file consisting almost entirely of `0xFF` bytes
(`tr -d '\0' | wc -c` → 32,256 non-zero bytes, of which the first 64 are
all `\377`). Looks like a pre-allocated write-ahead / transaction journal
the device clears to erased-state (`0xFF`) at format and lazily populates
during use. **Not referenced anywhere in `resmed_loader.cpp`** — same as
AS11's `journal.jnl`. The S9 version is twice the size (32 KB vs 16 KB on
AS11) and uses `0xFF` rather than `\0` as the empty marker — consistent
with raw NAND being the underlying storage. AS10 has no equivalent file
on the user's VAuto sample — it appears S9 and AS11 bracket the AS10 era.

### STR.edf — much smaller pre-allocation than AS10/AS11

S9 STR.edf header (offsets in EDF spec):
```
Startdate 16-JAN-2019  SRN=23141699984  MID=26  VID=1
16.01.19 12.00.00  7424  EDF  385  86400.0028
```

- **Header length: 7424 bytes** (vs AS10's `20224` and AS11's `20224`) —
  fewer cumulative-summary channels declared.
- **Pre-allocated record count: 385 days** (≈1 year), vs AS10 VAuto's
  `25088` (≈68 years). The S9 was designed for a much shorter retention
  ceiling.
- **Daily stride: `86400.0028` seconds/record** — same one-record-per-day
  cadence as AS10/AS11, with a slightly different fractional offset.
- **`MID=26` in STR.edf == `#MID 001A` in Identification.tgt** — confirms
  `#MID` is **hexadecimal** in `Identification.tgt` and decoded to decimal
  in the recording-ident line.

### CRC encoding summary across all three ResMed generations

| File class | S9 | AS10 | AS11 |
|---|---|---|---|
| Root `Identification.crc` | 4 B binary | 4 B binary | 4 B binary |
| Root `STR.crc` | **8 B binary** (present) | absent | absent |
| `SETTINGS/<X>GL.crc` (per `.tgt`) | 4 B binary | 4 B binary | — |
| `SETTINGS/CurrentSettings.crc` | — | — | 4 B binary |
| `SETTINGS/<X>.crc` (per `.log`) | **8 B binary** | **absent** | — |
| `DATALOG/<basename>.crc` (per `.edf`) | 8 B binary | 8 B binary | **absent** |

ResMed never used ASCII-hex CRCs — all sizes here are raw binary bytes.
(Prior catalogue entries claimed "8-byte ASCII hex"; that was wrong and
has been corrected in the AS10 / AS11 sections.)

### Observations / open questions

- **S9 is the lineage's namesake.** All loader code uses `RMS9_` prefixes
  (`resmed_loader.cpp:46-48`) even for AS10/AS11-specific channels —
  the loader was originally written for the S9 and extended forward.
  The `STR_ResMed_S9 = "S9"` constant at line 57 is the canonical platform
  string for this generation.
- **Flat-DATALOG vs per-day folders is the single clearest S9-vs-AS10/11
  discriminator** without parsing identity — a glance at `DATALOG/` is
  enough.
- **`#PCB` format change marks the S9→AS10 generational boundary.**
  S9's dash-joined PCB code (`7239S1-45153526`) became AS10's GS1 barcode
  format (`(90)R370-7421(91)B1(21)…`) — likely a regulatory / supply-chain
  change at ResMed.
- **Mode encoding back-translation** at `resmed_loader.cpp:1772` shows the
  S9 mode-byte vocabulary is the **canonical internal representation** —
  AS11 modes (0/1/2/3) are translated *back* to S9/AS10 codes during
  import. Reinforces that S9 is the platform OSCAR's data model
  fundamentally targets.
- **Likely the OSCAR codebase's most exercised import path** historically —
  S9 was the dominant ResMed CPAP for the better part of a decade and is
  still in widespread use; the loader has had many years to mature against
  it. This sample isn't expected to reveal new loader issues, but it
  documents the format for future comparisons against unknown ResMed-like
  cards.
- **`Journal.dat` deserves a follow-up.** Worth confirming on another S9
  card whether the 32 KB is always mostly-0xFF or whether long-running
  cards accumulate content. Same question applies to AS11's `journal.jnl`.

---

## Philips Respironics DreamStation 2 (PRS1 model 410-series)

**Sample:** `C:/Users/Guy/Downloads/Arie Klerk - A KLERK PRS1-410X150C-D031944288144E/PRS1-410X150C-D031944288144E`
**Device:** PRS1 model `410X150C` (DreamStation 2 family), encoded serial
`D031944288144E`. Device folder name: `75FAE30D` (likely a device-ID hash, not
the printable serial).
**Loader:** `prs1_loader.cpp` + `prs1_parser*.cpp`.

### File set

```
<root>/
├── P-SERIES/
│   ├── LAST.TXT                          8 bytes — points to active device folder ("75FAE30D")
│   └── 75FAE30D/                         device folder (hex name, not human-readable serial)
│       ├── PROP.BIN                    526 bytes — DreamStation 2 binary properties (encrypted)
│       ├── LOG.SEQ                     276 bytes — sequence/log file (encrypted)
│       ├── D/                          7 daily-summary files
│       │   ├── 000.B03                 344 bytes (first day, slightly larger)
│       │   ├── 001.B03 … 006.B03       242 bytes each (other days)
│       ├── E/                          empty in this sample (events folder)
│       ├── P0/                         per-session data — 302 files (78 sessions)
│       │   ├── 00000004.B01            session header (357-412 bytes)
│       │   ├── 00000004.B02            session body (281–2381 bytes, varies)
│       │   ├── 0000001D.B05            session extra (appears from session 0x1D onward)
│       │   …
│       │   └── 00000073.B05            (last session, hex ID = 115)
│       └── U/                          empty (updates? user data?)
├── System Volume Information/          Windows NTFS metadata (ignore)
└── .dropbox.device                     Dropbox sync marker (ignore — not on real card)
```

Session IDs in `P0/` are **hexadecimal**, zero-padded to 8 digits — `00000004`
through `00000073` (i.e. 4 to 115 in decimal). Gaps in the sequence are
expected (cancelled / very-short sessions may not produce files).

### What `Detect()` checks vs. what's there

`prs1_loader.cpp:572` (`PRS1Loader::Detect`) → `FindMachinesOnCard`
(`prs1_loader.cpp:600`) requires:
1. A folder named `P-Series` (case-insensitive — also matches `P-SERIES`, `p-series`).
2. Inside it, one or more subdirs containing either:
   - `PROP*.TXT` — System One / DreamStation 1, **or**
   - `PROP.BIN` — **DreamStation 2** (this sample).

Multiple device folders under `P-SERIES/` are sorted by `lastModified()`
(oldest first), so the most recent device is picked last in the iteration.

`LAST.TXT` at `P-SERIES/` level (8 bytes containing the active device-folder
name) is **not** consulted by `Detect()` — but it's a useful sanity check when
multiple device subdirs exist.

### Encrypted-archive header — common to every file under the device folder

**Every file in this device folder** (PROP.BIN, LOG.SEQ, every B01/B02/B03/B05)
shares an identical first ~96 bytes:

| Offset | Length | Field | Value on this sample |
|---|---|---|---|
| `0x00` | 2 | Record type | `0x000D` (LE) — "key envelope" |
| `0x02` | 2 | Version | `0x0001` |
| `0x04` | 2 | Flags | `0x0001` |
| `0x06` | 2 | Payload length | `0x0024` = 36 |
| `0x08` | 36 | **Per-card UUID (ASCII)** | `3bd04f46-c9a9-44ed-9243-eeaa74a50964` |
| `0x2C` | 2 | Next field type | `0x000C` |
| `0x2E` | 12 | Binary blob (likely IV/signature) | `fb48 be25 9c59 af42 78ff c5c5` |
| `0x3A` | 2 | Next field type | `0x0010` |
| `0x3C` | 16 | Binary blob (likely AES key wrap or HMAC) | `3fce 45f0 1fa3 c915 d184 cc0d 2e84 f1f7` |

After this preamble (~`0x4E`), the payload is **non-readable binary** —
indistinguishable from random data, strongly suggesting block-encrypted
content (AES-128 likely, given the 16-byte field widths).

The same UUID (`3bd04f46-…-eeaa74a50964`) appears in **every file** on this
device — it's a per-card key identifier, not a per-file value. This is a
distinctive signature for DreamStation 2 cards: if you see a UUID like
`3bd04f46-…` at offset `0x08` of PROP.BIN, the rest of the card belongs to
the same encryption envelope.

### Subdirectory roles

| Folder | Content | Inferred purpose |
|---|---|---|
| `D/` | `NNN.B03` (3-digit decimal IDs, 242 or 344 bytes) | **D**aily summary records (one file per active day) |
| `E/` | (empty here) | **E**vents — likely per-session event records |
| `P0/` | `XXXXXXXX.B01`, `.B02`, `.B05` (8-digit hex session IDs) | **P**er-session data, partition 0 — header (B01), body (B02), extras (B05) |
| `U/` | (empty here) | **U**pdates? **U**ser? Unknown — empty on this sample |

The `D/` count (7 files) is much smaller than the `P0/` count (78 sessions) —
many sessions can occur per day, but daily summaries collapse them. `7 days`
of daily summaries spread across `78 sessions` is consistent with frequent
short-naps + nightly use during a single week of recording.

### B05 appearance partway through the session sequence

The first `.B05` file is `0000001D.B05` (session 0x1D = 29). Sessions 4–28 have
only B01+B02; sessions 29 onward also have B05. This split is **not random** —
it correlates with either:
- A firmware update enabling a new data class, or
- A user setting change that started recording an additional sensor channel
  (e.g. an oximetry module was attached).

A future loader/diagnostic should be tolerant of this — the presence of `.B05`
varies session-by-session even within the same device folder.

### PRS1 generation discriminators

When fingerprinting an unknown PRS1 card, this fingerprint vs the
already-documented (in code) System One / DreamStation 1 variants:

| Feature | System One / DreamStation 1 | DreamStation 2 (this sample) |
|---|---|---|
| Properties file | `PROP.TXT` / `PROP1.TXT` (plain-text key-value) | `PROP.BIN` (encrypted binary) |
| Session data | `<id>.001` / `.002` / `.005` (decimal IDs, unencrypted) | `<id>.B01` / `.B02` / `.B05` (hex IDs, encrypted) |
| Daily summary | `*.001` summary in same folder | Dedicated `D/` subdir, `.B03` files |
| Event log | Mixed into session files | Dedicated `E/` subdir (may be empty) |
| Per-card key | None — files are plain | UUID header in every file |
| Device-folder name | Typically printable serial or model code | 8-char uppercase hex (e.g. `75FAE30D`) — appears to be a hash |
| `LAST.TXT` at P-SERIES level | (haven't seen) | Present, points to active device folder |

The loader's `Detect()` accepts both via `PROP*.TXT` glob vs `PROP.BIN` explicit
check — but the parsing is substantially different downstream.

### Observations / open questions

- **Encryption is the dominant feature of DreamStation 2 data.** Whatever loader
  work needs to happen here, it has to involve the encryption layer — the
  per-card UUID is presumably the salt/key-identifier for an AES scheme tied
  either to firmware or to a known constant.
- **Model code `410X150C`** is encoded in the folder name only — `PROP.BIN`
  doesn't expose it in cleartext. The model lookup probably has to live in
  cleartext-decrypted PROP.BIN content.
- **Device-folder name `75FAE30D`** doesn't match the printable serial
  `D031944288144E` from the source-dir name. It may be a fixed-length hash; if
  the same device dumps a second card the same hex should reappear.
- **Empty `E/` and `U/`** — would be useful to compare with another DreamStation 2
  sample that has events recorded, to learn what those filenames look like.
- **System One / DreamStation 1 sample needed.** Would let us compare the
  unencrypted plain-text path side-by-side and confirm the discriminators above.

---

## Yuwell YH690F (Format D — BreathCare III)

**Sample:** `C:/Users/Guy/Downloads/HarryDuBois- Yuwell YH690F- Ivan`
**Device:** Yuwell YH690F, full model-serial `YH690F-246350193`.
**Loader:** `yuwell_loader.cpp` → `YuwellFormatD` (BreathCare III sub-format,
also handles YH-680 / YH-690 family).

### File set

```
<SD card root>/                                ← passed to Detect()
└── YH690F-246350193/                          model-serial (must start with "YH")
    ├── RunLog.bys                  0 bytes    Format D marker (INSIDE MODEL-SERIAL, not at root)
    ├── summer.bys                 50 bytes    cross-session summary (all zeros on this sample —
    │                                            likely tracks which sessions have been read off;
    │                                            name appears to be "summary" with a transliteration tweak)
    └── 00100001/                              session-instance subdir (8-char zero-padded hex)
        ├── 0100001d.bys          339,891 bytes  Flow / "d"etail waveform
        ├── 0100001m.bys            5,102 bytes  Per-"m"inute summary records
        └── 0100001s.bys               77 bytes  Session "s"ummary + identity header
```

Only **one session** recorded on this card. A populated card would have many
`00xxxxxx/` siblings under the MODEL-SERIAL directory, each with its own
`d.bys` + `m.bys` + `s.bys` triplet.

### Yuwell Format A through D — quick reminder

Yuwell has four sub-formats; each gets its own `Detect()` and the
`YuwellFactory` (`yuwell_loader.cpp:1529`) tries them A → B → C → D, first
match wins:

| Format | Models | Sentinel | This card matches? |
|---|---|---|---|
| **A** (YH-550) | flat: `<root>/RunLog.bys` + `<root>/MODEL-SERIAL/0XXXXXXX.BYS` | `RunLog.bys` at root level (one above MODEL-SERIAL) | **No** — `RunLog.bys` is inside MODEL-SERIAL here, not above it |
| **B** (YH-580) | `<root>/YHSD-NEW.BYS` exactly 64 KB | single fixed-size root file | **No** |
| **C** (YH-830) | `<root>/MODEL-SERIAL/0XXXXXXX.BYS` (no RunLog) | bare MODEL-SERIAL with direct `.BYS` files | **No** — extra layer of subdirs here |
| **D** (YH-680/690, BreathCare III) | two-level: `<root>/MODEL-SERIAL/00xxxxxx/<id>d.bys + m.bys + s.bys` | inner-level `RunLog.bys` + `s.bys` with `YH…` at offset `0x20` | **YES** |

### What `Detect()` reads

`YuwellFormatD::Detect` (`yuwell_loader.cpp:1044`) calls `GetModelSerials()`
(`yuwell_loader.cpp:1072`):
1. Lists subdirs of the given path.
2. For each subdir whose name starts with `"YH"` (case-insensitive):
   - Requires a `RunLog.bys` inside that subdir.
   - Lists its session subdirs (`00xxxxxx/`).
   - For each session, opens any `*s.BYS` file.
   - Reads the **first 0x30 bytes**; skips 0x20 bytes, then reads 16 bytes as
     the model-serial string.
   - If that string starts with `"YH"`, records it.

So **the canonical identity check is the 16-byte ASCII model-serial at offset
`0x20` of the session summary file** — confirmed against the directory name as
a cross-check.

### Session-summary file (`*s.bys`) — 77 bytes, decoded

`0100001s.bys` byte-precise layout:

| Offset | Length | Field | Value on this sample |
|---|---|---|---|
| `0x00` | 8 | Record header (`01 01`) + **timestamp** (Y M D H M S — BCD/binary YYMMDDHHMMSS) | `19 05 11 03 22 15` → **2025-05-17 03:34:21** (session start) |
| `0x08` | 8 | Header (`19 05 11`) + **session-end timestamp** | `19 05 11 08 11 18` → **2025-05-17 08:17:24** (≈ 4h 43m session) |
| `0x10` | 16 | Small counters / stats | `00 00 00 00 00 00 66 00 00 00 0f 00 00 00 00 00 1b 01 1b 01` (mixed) |
| **`0x20`** | **16** | **Model-serial ASCII** | **`YH690F-246350193`** (loader identity-check target) |
| `0x30` | 8 | Marker + session-start ts repeated | `01 00 19 05 11 03 22 15` |
| `0x38` | 8 | Trailing flags | `00 0c 10 20 10 00 00 00` |
| `0x40` | 13 | Padding / extra fields | `00 00 00 00 00 00 00 00 00 00 00 03 1e` |

The session-start timestamp at offset `0x02` is the canonical session anchor —
appears again at `0x32`. Format is **plain 6-byte BCD-ish**: `YY MM DD HH MM SS`
where each byte is a small binary number (not BCD-encoded), so `0x19 = 25`,
`0x05 = 5`, etc.

### Per-minute file (`*m.bys`) — structure observed

First 16 bytes are a header: `19 05 11 03 22 15 1b 01 41 58 00 00 00 00 00 00`
— session-start timestamp + flags + ASCII tag `AX`. After that, **per-minute
records of ~16 bytes each** stream out:

```
<u16 LE> <u16 LE> <byte> <byte> <byte> <byte> <byte> 0xFE <2-char ASCII tag> <padding zeros>
```

The 2-char ASCII tags step through the alphabet column-wise: `AX, BY, CX, CX,
DX, DX, EY, FX, FW, GY, GW, HX, …`. These look like a per-minute **state /
flag code** — first char advances roughly once per minute (alphabetic
counter?), second char alternates X/Y/W (possibly mask-fit / leak grade /
breathing classification).

### Detail file (`*d.bys`) — flow waveform

First 8 bytes are again the timestamp+flags header `19 05 11 03 22 15 1b 01`.
After that, the file is a **stream of single-byte values** that visibly trace
respiratory flow patterns:

```
22 22 22 22 23 23 23 23 24 23 24 55 59 59 57 56 56 54 51 4E 4B 4B 48 45 …
```

Values cluster in the low-30s to mid-80s (decimal), rising and falling in
breath-shaped patterns. With 339,883 sample bytes after the 8-byte header and
a ~4h 43m session duration (~17 000 seconds), this gives a **sample rate of
~20 Hz** — consistent with a CPAP-class flow sensor.

The values are likely **raw 8-bit flow-rate** in some device unit (cm/sec or
0.1 L/min), not ASCII despite the printable range — small respiratory flows
happen to land in the printable ASCII band.

### Identity summary

The card carries the model-serial in **three places**, two of which the loader
relies on:

| Where | Value | Loader-checked |
|---|---|---|
| Source dir name (passed in by user/import) | `HarryDuBois- Yuwell YH690F- Ivan` (irrelevant) | No |
| MODEL-SERIAL directory name | `YH690F-246350193` | **Yes** — listed and required to start with `"YH"` |
| `*s.bys` offset `0x20` | `YH690F-246350193` | **Yes** — authoritative identity per loader comment "Much more source-of-truth" |

### Observations / open questions

- **Format D's `RunLog.bys` sits inside MODEL-SERIAL**, unlike Format A where
  it's at the SD-card root. This is the key Format-A-vs-Format-D discriminator
  if you only see a `RunLog.bys` and need to figure out which.
- **The `*s.bys` 0x30-byte identity prefix** is the canonical fingerprint for
  Yuwell BreathCare III cards. Future YH-series devices that ship with this
  same layout should be auto-claimed by Format D.
- **`summer.bys` (50 bytes, all zeros)** — appears to be a placeholder for a
  single-record summary that hasn't been filled in. The YH-680 sample below
  confirms `summer.bys` is a **per-session catalog** at **50 bytes per
  session record** (start/end timestamps + identity ASCII). The YH690F's
  all-zero record suggests the session here was never formally "closed" by
  the device, even though the `s.bys`/`m.bys`/`d.bys` payload exists.
- **`AX/BY/CX` ASCII tags in `m.bys`** — first column appears to be a minute
  counter that increments by row, second column may be a small enum of state
  letters. Worth correlating against another session of the same device to
  confirm.
- **Flow byte units** — the loader code uses `lpm4` / `lpm20` (custom 4/20 cmH2O
  leak constants) but the raw byte → flow-rate conversion factor for this
  format isn't reflected in the current entry; would need to check
  `OpenSession` to be sure.

---

## Yuwell YH-550 (Format A)

**Sample:** `C:/Users/Guy/Downloads/Centurix-YuwellYH550 Chris Read`
**Device:** Yuwell YH-550A, full model-serial `YH550A-248420161`.
**Loader:** `yuwell_loader.cpp` → `YuwellFormatA`.

### File set

```
<SD card root>/                                ← passed to Detect()
├── RunLog.bys                       0 bytes   Format A marker (at the ROOT, above MODEL-SERIAL)
└── YH550A-248420161/                          MODEL-SERIAL (must start with "YH")
    ├── 00100001.BYS              4,242 bytes  ─┐
    ├── 00100002.BYS              1,672 bytes   │  208 session files,
    ├── 00100003.BYS              1,572 bytes   │  decimal IDs 1–208,
    │   …                                       │  sizes 202–6,122 bytes
    └── 00100208.BYS              4,792 bytes  ─┘  (avg ~2.6 KB)
```

Total: ~540 KB across 208 sessions — **summary-only data, no waveform**.
Compare to the YH690F sample (Format D), where a single session's flow file
was 340 KB. The YH-550 is a lower-spec device that records only per-minute and
session-summary records, not high-cadence flow.

### Format A vs Format D — discriminator at a glance

| Feature | Format A (YH-550, this sample) | Format D (YH690F, BreathCare III) |
|---|---|---|
| `RunLog.bys` location | **At SD card root**, one level above MODEL-SERIAL | **Inside MODEL-SERIAL**, alongside session dirs |
| Sessions are | **Single flat files** named `<id>.BYS` directly under MODEL-SERIAL | **Triplets** in `MODEL-SERIAL/<id>/<id>d.bys + m.bys + s.bys` |
| Session IDs | Decimal, zero-padded (`00100001`–`00100208`) | Numeric subfolder (`00100001/`) |
| Per-session detail | Summary-only (~2 KB/session) | Full flow waveform (~340 KB) + minute + summary |
| `summer.bys` | Absent | Present at MODEL-SERIAL level (50 bytes) |
| Loader header read | `0xA0` bytes (160) per file | `0x30` bytes (48) per `*s.bys` |
| **Identity offset** in the read header | **`0x1E`** | **`0x20`** |
| Identity length | 16 ASCII chars starting `YH…` | Same |

These two formats demonstrate why the Yuwell loader uses a factory: the file
layouts diverge enough that distinct detectors are warranted, even though both
read the model-serial from a fixed offset of a fixed-length header window.

### Session file (`.BYS`) — header layout (first 0xA0 bytes)

From `00100001.BYS`:

| Offset | Length | Field | Value on this sample |
|---|---|---|---|
| `0x00` | 6 | **Session-start timestamp** (`YY MM DD HH MM SS`, each byte binary) | `19 08 15 00 2a 17` → **2025-08-21 00:42:23** |
| `0x06` | 6 | **Session-end timestamp** | `19 08 15 07 29 21` → **2025-08-21 07:41:33** (≈ 6h 59m) |
| `0x0C` | 18 | Mixed flags / settings counters | `01 0a 28 32 96 32 00 00 03 04 00 01 00 00 09 00 3a 01` |
| **`0x1E`** | **16** | **Model-serial ASCII** | **`YH550A-248420161`** (loader identity-check target) |
| `0x2E` | 2 | u16 LE counter | `a3 01` (= 0x01A3 = 419) |
| `0x30` | 2 | u16 LE counter | `9f 01` (= 0x019F = 415) |
| `0x32` | 2 | u16 LE counter | `f9 28` (= 0x28F9 = 10489) |
| `0x36` onwards | … | Per-minute records (~16 bytes each, pattern repeats with incrementing third byte until plateau) | `00 02 28 00 …` → `00 02 29 00 …` → `00 02 2a 00 …` → … |

The session-summary structure is the same shape as Format D's `s.bys` — small
header + identity ASCII + minute-grain records — but on Format A everything
lives in **one file per session**.

### Session count and date span

- Session IDs run **decimal** `00100001` → `00100208` (note: hex sort and decimal
  sort agree below 9, then diverge for IDs `00100010`+ where decimal continues
  but hex would also work — the directory listing showed clean numeric order).
- Session 1 (`00100001.BYS`): start `2025-08-21 00:42`, duration ~7 hours.
- Session 208 (`00100208.BYS`): start `2026-01-01 01:56`, duration shorter.
- Date span: **2025-08-21 → 2026-01-01** = ~4.5 months, 208 sessions = ~1.5
  sessions/day on average (suggests naps + nightly use, or multiple short
  awakening-segmented sessions per night).

### Observations / open questions

- **`RunLog.bys` is required and empty (0 bytes)** — pure presence-marker. The
  loader doesn't read its content; if it were missing, Format A's detect would
  fail and the card would fall through to other formats (none of which would
  match this layout) and be rejected entirely.
- **Format A → Format D evolution.** The YH-550 (Format A) clearly predates the
  YH-680/690 family (Format D) — the newer devices added session subdirectories,
  flow-rate detail recording, a `summer.bys` cross-session summary, and moved
  `RunLog.bys` inside MODEL-SERIAL. The identity-offset shift from `0x1E` to
  `0x20` likely reflects two extra bytes inserted in the session-header for the
  newer firmware.
- **Session ID `00100001`** — starting at 100001 (rather than 1) suggests an
  internal session counter that's been pre-incremented; possibly the device
  resets on firmware update or the lower IDs are reserved.
- **Decimal vs hex session IDs.** YH-550 (Format A) sessions read cleanly as
  decimal numbers (`208` files, `00100001`–`00100208`). YH690F (Format D)
  session-directory names also looked decimal-shaped (`00100001`). Whether
  this is true decimal or a coincidence of the small numbers involved (no
  hex letters appearing yet) isn't certain without a card that has hit
  `00100010`–`0010001F`.

---

## Yuwell YH-680B (Format D, summary-only — no flow data)

**Sample:** `C:/Users/Guy/Downloads/AAThibert-Yuwell680 Adem Arkadas-Thibert`
**Device:** Yuwell YH-680B, full model-serial `YH680B-253100097`.
**Loader:** `yuwell_loader.cpp` → `YuwellFormatD` (same as YH690F).

Same loader path and overall shape as the YH690F entry above. This card adds
three concrete data-points that the YH690F sample left open or undersampled.

### File set

```
<SD card root>/                                  ← passed to Detect()
└── YH680B-253100097/                            MODEL-SERIAL
    ├── RunLog.bys                  0 bytes      Format D presence marker (same as YH690F)
    ├── summer.bys              2,100 bytes      per-session catalog — 50 bytes × 42 sessions
    ├── 0100001/                                 session subdir (7 digits — see below)
    │   ├── 0100001m.bys           494 bytes     per-minute summary
    │   └── 0100001s.bys           104 bytes     session summary + identity
    ├── 0100002/
    │   …
    └── 0100042/
        ├── 0100042m.bys         1,196 bytes
        └── 0100042s.bys            77 bytes
```

**42 sessions, every one of them has `m.bys` + `s.bys`, none have `d.bys`.**
The YH-680B is a **summary-only device** — it doesn't record the high-cadence
flow waveform that YH690F captures.

The code comment at `YuwellFormatD::Detect` flags `d.BYS` as "(This file is
optional)" — the YH-680B confirms that "optional" really means "absent on
some Format-D devices entirely," not just "skipped for very short sessions."

### Session-subdir naming: 7 vs 8 digits

| Device | Session subdir | Inner file basename |
|---|---|---|
| YH690F (prior sample) | `00100001/` (8 digits) | `0100001s.bys` (7 digits) |
| YH680B (this sample) | `0100001/` (7 digits) | `0100001s.bys` (7 digits) |

YH-680B is consistent (subdir name and file basename are both 7 digits and
identical). YH690F prepends a zero on the subdir name only. The loader's
filesystem-walk approach (`entryInfoList` over all subdirs of MODEL-SERIAL)
tolerates both — no length constraint is applied — so this discrepancy is
benign for OSCAR but worth flagging if anyone hard-codes a parser.

### `summer.bys` — decoded

The YH690F's `summer.bys` was 50 bytes of zeros and its purpose was unclear.
The YH-680B's `summer.bys` is **2 100 bytes — exactly 50 bytes × 42 sessions**
— making the format obvious:

> **`summer.bys` is a per-session catalog with one fixed-size 50-byte record
> per recorded session, appended in session order.**

Each 50-byte record (offset within `summer.bys` for session `N` is `(N-1) * 50`):

| Offset within record | Length | Field | Value on this card, session 1 |
|---|---|---|---|
| `0x00` | 2 | Record header (type-version) | `02 01` |
| `0x02` | 6 | **Session-start timestamp** (Y M D H M S — binary bytes) | `19 06 0F 12 08 3A` → 2025-06-15 18:08:58 |
| `0x08` | 6 | **Session-end timestamp** | `19 06 0F 12 23 3A` → 2025-06-15 18:35:58 (≈ 27 min) |
| `0x0E` | 8 | Flags / zero | `00 00 00 00 00 00 00 00` |
| `0x16` | 6 | Small counters | `00 46 00 00 00 00` |
| `0x1C` | 4 | More counters | `1B 00 1B 00` |
| `0x20` | 16 | **Model-serial ASCII** | `YH680B-253100097` |

The 6-byte timestamps are the same `Y M D H M S` binary format used in the
`s.bys` header (each byte is a small binary number, not BCD-encoded).

So this 2,100-byte `summer.bys` catalog records 42 sessions spanning June 2025
→ March 2026 (with a multi-month gap visible in the timestamps — sessions 30+
jump from August 2025 directly to March 2026, suggesting the device was set
aside and resumed later).

### `s.bys` header byte 0 — version marker differs between devices

| Device | `s.bys` first 2 bytes | `summer.bys` record header |
|---|---|---|
| YH690F | `01 01` | (all zero — record never written) |
| YH680B | `02 01` | `02 01` (matches s.bys) |

Hypothesis: **byte 0 of the session header is a device-class / firmware-version
marker**: `0x01` for full-flow Format D devices (YH690F), `0x02` for
summary-only Format D devices (YH-680B). Byte 1 (`0x01`) is a constant version.

If that hypothesis holds, a future YH-series card could be tier-classified
from byte 0 of any `s.bys` without needing to enumerate the session
directories looking for `d.bys` presence. Worth verifying against more
samples.

### Identity-offset stays at `0x20`

Same as YH690F — `s.bys` carries the 16-byte ASCII model-serial at offset
`0x20`, exactly where the loader looks. Sample confirms `YH680B-253100097` at
that offset on both session 1 and session 42.

### Device-tier comparison across Yuwell samples seen so far

| Device | Format | Flow file? | Detail tier | Loader path |
|---|---|---|---|---|
| YH-550A | A | n/a | Summary only (1 file per session, ~2.6 KB) | `YuwellFormatA` |
| **YH-680B** | **D** | **No** | **Summary only (m+s, no d), ~600 bytes per session** | `YuwellFormatD` |
| YH690F | D | Yes (340 KB) | Full waveform + minute + summary | `YuwellFormatD` |

The YH-680B sits between the two extremes: same nested layout as the
flow-recording YH690F, but the same detail level as the older flat-layout
YH-550. Useful corner-case for understanding that **format = layout**,
**device tier = which optional files exist within that layout**.

### Observations / open questions

- **Without `d.bys`, OSCAR has no waveform to chart for YH-680B sessions** —
  only per-minute summary and session totals. Similar UX to the AS10 CPAP
  basic, but the underlying reason is different: AS10 CPAP basic never
  records detail at all; YH-680B records minute-grain detail but no
  sample-grain flow.
- **Session-header byte 0 as a tier marker** (`0x01` vs `0x02`) is a guess
  from two samples; another flow-recording Format D device (YH-690 non-F, or
  a second YH690F) would confirm whether `0x01` correlates strictly with
  flow-data presence.
- **The YH690F's all-zero `summer.bys`** is more puzzling now that we know
  the file's true structure — that single record should have been filled in
  for the session that's on the card. Possibly that session was never
  formally "closed" by the device (card was pulled mid-session) and only the
  per-session files got flushed.
- **Session subdir naming inconsistency (7 vs 8 digits)** is benign for the
  current loader but the difference may track to firmware family — YH-680
  family uses 7 digits, YH-690 family pre-zeros to 8. Confirmable with
  another YH-690 sample.

---

## Yuwell YH-580 (Format B — single 64 KB ring-buffer file)

**Sample:** `C:/Users/Guy/Downloads/ksetmb-YUWELLYH580 sergey`
**Device:** Yuwell **YH580C**, serial `23689005`, model-serial string
`YH580C-23689005` (16-byte field at file offset `0x84`).
**Loader:** `yuwell_loader.cpp` — `YuwellFormatB` path (`Detect` at
line 386, `Open*` at line 446+).
**Data span:** 141 session summaries, starting 2025-08-06. Total
on-card payload: **64 KB exactly.** Card was reported by the user as
"very little data" — that's the entire YH-580 format, not a partial
copy.

This fills the **`YuwellFormatB` slot** in the catalogue (Formats A/D
already documented; B and C were Detect-table entries only). YH-580
sits at the low-storage end of the Yuwell taxonomy — a single
**fixed-size 64 KB ring-buffer file** holds the device's entire data
state, in stark contrast to the YH690F's multi-megabyte per-session
files. Strong-signal addition: there's no waveform data on a YH-580
card at all, only session summaries plus a small detail-record region.

### Card-root structure

```
<root>/
├── YHSD-NEW.BYS     65,536 bytes (exactly 64 KB) — the entire data file
└── YHSD-OLD.BYS          0 bytes — empty placeholder
```

That's it. Two files, no subdirectories, no `RunLog.bys`, no
`MODEL-SERIAL/` folder, no per-session detail files. The empty
`YHSD-OLD.BYS` plausibly serves as the "previous generation" buffer in
some swap scheme — the loader explicitly comments *"We don't care about
this"* at line 389. (Reading it on this sample confirms it really is
zero bytes.)

### What `Detect()` checks vs. what's there

`yuwell_loader.cpp:386-414`:

1. `YHSD-NEW.BYS` exists at root.
2. Its size is **exactly** `0x10000` (64 KB). Off-by-one fails.
3. `GetModelSerials()` reads 16 bytes from offset `0x84` and checks the
   string starts with `"YH"`.

Both conditions met on this card:
- `YHSD-NEW.BYS` is exactly 65,536 bytes.
- Offset `0x84` reads as ASCII `YH580C-23689005`.

### `YHSD-NEW.BYS` — internal layout

Decoded against `YuwellFormatB::OpenMachine` (`yuwell_loader.cpp:475-606`):

| Offset | Length | Field | This sample |
|---|---|---|---|
| `0x00` | 4 | Magic — ASCII `"AAAA"` (loader doesn't enforce, but present) | `41 41 41 41` |
| `0x04` | 1 | mode | `01` |
| `0x05` | 1 | ramp (minutes) | `0x14` = 20 |
| `0x06` | 1 | initial_pressure (cmH₂O × 10) | `0x28` = 40 → 4.0 |
| `0x07` | 1 | pressure_setting (CPAP fixed mode) | `00` |
| `0x08` | 1 | maximum_pressure (×10) | `0x78` = 120 → 12.0 |
| `0x09` | 1 | minimum_pressure (×10) | `0x32` = 50 → 5.0 |
| `0x0A` | 1 | humidity | `00` |
| `0x0B` | 1 | fps_level | `02` |
| `0x0C` – `0x1E` | 19 | (skipped) |  |
| `0x1F` | 2 (LE) | record_count | `8d 00` = **141 sessions** |
| `0x21` – `0x83` | 99 | (skipped) |  |
| `0x84` | 16 | model-serial ASCII | `YH580C-23689005` |
| `0x94` – `0x0BFF` | 2924 | reserved gap — mostly zero / 0xFF |  |
| `0x0C00` | 30 × `record_count` | session summary table (~4.2 KB used) | 141 × 30 = 4230 B |
| `0x0C00 + 30·N` – `0x75FF` | unused tail of summary region | (padding to detail-region start) |  |
| `0x7600` – `0xFFFF` | 38,912 bytes | per-session detail records, pointed to by per-summary `(offset_high, offset_low) + 0x7600` |  |

### Session summary record — 30 bytes each

Fields (offsets within record):

| Off | Bytes | Field | Notes |
|---|---|---|---|
| `0x00` | 6 | start: yy mm dd hh mm ss | yy = year − 2000 |
| `0x06` | 6 | end: yy mm dd hh mm ss | |
| `0x0C` | 8 | settings snapshot (mode, ramp, init, set, max, min, humidity, fps) | mirrors header layout |
| `0x14` | 1 | oai_count | obstructive apnoea count |
| `0x15` | 1 | hi_count | hypopnea count |
| `0x16` | 2 | (skipped) | |
| `0x18` | 1 | avg_leak_vol | |
| `0x19` | 1 | avg_pressure | |
| `0x1A` | 2 | offset_high : offset_low — 16-bit file offset of detail data (BE bytes, added to `0x7600`) | |
| `0x1C` | 1 | (skipped) | |
| `0x1D` | 1 | session_minutes | matches end − start to within ~1 min |

First summary on this card (offset `0x0C00`):

```
19 08 06 01 2c 01      start = 2025-08-06 01:44:01
19 08 06 03 3a 03      end   = 2025-08-06 03:58:03   (134 min elapsed)
01 14 28 00 78 32 03 02  settings (mode=1, ramp=20, init=4.0, max=12.0, min=5.0, humid=3, fps=2)
03 01                   oai=3, hi=2
00 00                   (skipped)
37 02                   avg_leak_vol=0x37, avg_pressure=0x02
00 00                   offset → 0x0000 + 0x7600 = 0x7600 (start of detail region)
00                      (skipped)
85                      session_minutes = 0x85 = 133 ✓
```

Plausible decode end-to-end: a 133-minute session on 2025-08-06 at 01:44 with
the device's standard 20-minute ramp from 4.0 to 12.0 cmH₂O.

### Detail-record region (`0x7600` – `0xFFFF`)

38,912 bytes split among 141 sessions = ~276 bytes per session on average — a
tight per-session detail budget. Likely per-session event tables (apnoea
timestamps, leak markers) rather than waveform samples. The loader's
`offset = ((offset_high << 8) + offset_low) + 0x7600` formula chains each
summary to its detail block, and detail blocks are presumably variable-size
records within the 38 KB pool, packed forward and wrapping when full.

### How YH-580 differs from every other Yuwell sample on file

| Aspect | YH-580 (this — Format B) | YH-550 (Format A) | YH-680B (Format D summary) | YH690F (Format D full) |
|---|---|---|---|---|
| Top-level file shape | **Single 64 KB `YHSD-NEW.BYS` at root** | `RunLog.bys` + `MODEL-SERIAL/*.BYS` | Two-level `MODEL-SERIAL/<session>/[d/m/s].bys` | Same as YH-680B |
| Total on-card payload | **64 KB fixed** (regardless of session count) | hundreds of KB | ~540 KB | multi-MB |
| Waveform data | **None** | None | None | Yes |
| Per-session detail | ~276 bytes/session in `0x7600`+ pool | Whole per-session BYS file | Per-session subdir with summer.bys | Per-session subdir with full d/m/s set |
| Session-count ceiling | **~141** (limited by 38 KB detail pool + 30 B summaries) | Several hundred | Several hundred | Several hundred |
| Storage model | **Ring buffer** (NEW + OLD swap) | Per-session files accumulate | Per-session subdirs accumulate | Per-session subdirs accumulate |
| Identity source | `0x84` of YHSD-NEW.BYS | Directory name | `s.bys` offset `0x20` | `s.bys` offset `0x20` |

The 64 KB ceiling means **YH-580 cannot ever accumulate more than a few months
of nightly use** on a single card — once 141 (or so — actual ceiling depends
on detail-pool packing) summaries are written, the oldest sessions are
overwritten. The `NEW.BYS` / `OLD.BYS` pair likely swaps to maintain one
generation of history during the rewrite.

### Why "very little data" is the format's design

This isn't a half-empty card or a half-broken copy — **64 KB exactly is the
full YH-580 dataset.** OSCAR will import 141 daily summaries with pressure/
leak/AHI components, but no flow waveform charts, no per-minute trends, no
event-by-event timelines. The device is essentially a "summary stats logger"
with the same display fidelity as the ResMed AirSense 10 CPAP basic or the
DreamStation 200X110 brick — except it doesn't even pretend to write
per-session files for the host software to discover.

### Observations / open questions

- **Magic `"AAAA"` at offset 0** — loader comment at line 534 says it's not
  enforced because it *"may be different for some machines."* For the
  YH-580 sample we have, it is `41 41 41 41`. If another Format B model
  surfaces (the loader allows it — Format B's `GetModelSerials` just
  requires `"YH"` prefix), the magic might differ.
- **2924-byte reserved gap (`0x94` – `0x0BFF`)** is mostly zero/0xFF on this
  sample. Some of it likely carries per-day aggregates or display state
  that the loader currently skips. Worth a slow read on a second YH-580
  card if one surfaces, to see what the device writes there beyond
  identity.
- **Detail-region (`0x7600`+) decoding is unknown territory.** The loader
  reads summaries but I haven't traced what it does with `offset` to
  decode per-session event data. If the device tracks individual
  apnoea/hypopnea timestamps in the detail region, OSCAR could in
  principle render them — but if the loader currently only consumes
  the summary counts, the events are present-but-ignored. **Not a fix
  prescription** — pending verification on what OSCAR actually shows
  for this card.
- **YH580C suffix in the model-serial.** `YH580` is the family; the `C`
  is a hardware-revision letter (compare YH-550 "A/B Models" and YH-680
  "A/B Models" from the loader's header comment). Future YH-580 samples
  may show A/B/D revisions — same Format B is expected.
- **Empty `YHSD-OLD.BYS` is consistent with the device having only
  recently filled its first generation of the NEW buffer** (or having
  rotated very rarely). On a long-used card, OLD would likely carry the
  previous generation's 64 KB. Worth confirming on a longer-running
  YH-580 sample.

---

## Yuwell YH-825A BiPAP (Format C — first sample; IPAP/EPAP decode added 2026-05-21)

**Sample:** `C:/Users/Guy/Downloads/Yuwell825ST Houzouris/SD Card`
**Device:** Yuwell **YH825A** BiPAP (BreathCare II Bi-PAP family per
`yuwell_loader.cpp:31`), serial `V25738024`, model-serial string
`YH825A-V25738024`.
**Loader:** `yuwell_loader.cpp` — `YuwellFormatC` path (`Detect` line 708,
`OpenSession` line 839).
**Data span:** 23 per-session `.BYS` files, March 2026.
**Status:** Loader detects and now imports IPAP/EPAP correctly. The
original Format C decode handled only modes `0x00` (CPAP) and `0x06` (APAP);
the five BiPAP-class modes (S / T / ST / VGPS / AUTOS = `0x01-0x05`) fell
through to `MODE_UNKNOWN` and `pressure` was read from byte `0x0C`, which
is zero on BiPAP records. Fix shipped 2026-05-21 — see `Notes/BUG_FIXES.md`
and GitLab issue #178. **IPAP / EPAP are LE `u16 * 10` at record offsets
`0x02-0x03` and `0x04-0x05`** (confirmed against the Yuwell BreathCare
vendor app: 15-min ramp from IPAP 8.5 / EPAP 4.5 to IPAP 16 / EPAP 12,
exact byte-for-byte match across 12 sampled records).

This fills the **`YuwellFormatC` slot** in the catalogue. The loader was
written for the YH-830 (also Format C, CPAP/APAP class) per the comment
at line 715; YH-825 is the **BiPAP variant** in the same family and shares
the file-layout but appears to put pressures in different record offsets.

### Card-root structure

```
<root>/
├── SN.BYS                                       16 bytes — ASCII model-serial only
└── YH825A-V25738024/                            device folder (directory name = model-serial)
    ├── 00100001.BYS                             session 1
    ├── 00100002.BYS                             session 2
    …
    └── 00100023.BYS                             session 23 (23 sessions total)
```

`SN.BYS` content (16 bytes): `YH825A-V25738024` — exact ASCII, no padding
or framing. Not consulted by `Detect()` or any parsing path I found in
`yuwell_loader.cpp` — the loader gets the same identity from inside the
session files. Useful as a fingerprint *signal* (presence of `SN.BYS`
suggests this Yuwell variant) but not used.

### Format C subdirectory layout (vs A/D)

| Layout aspect | Format A (YH-550) | Format C (YH-825/830) | Format D (YH-680/690) |
|---|---|---|---|
| `RunLog.bys` | At root | None | Inside `MODEL-SERIAL/` |
| `MODEL-SERIAL/` subdir | Yes | Yes | Yes |
| Per-session location | `MODEL-SERIAL/0XXXXXXX.BYS` (flat) | `MODEL-SERIAL/00100XXX.BYS` (flat) | `MODEL-SERIAL/<sessionId>/<id>d/m/s.bys` (two-level) |
| Per-session files per session | 1 | 1 | 3 (detail + minute + summary) |
| Root marker file | `RunLog.bys` (size 0) | **`SN.BYS` (16 bytes ASCII serial)** — first time seen | — |
| Identity offset within `.BYS` | `0x20` | **`0x27`** | `0x20` |

The `SN.BYS` at root and the `0x27` identity offset are **the two
distinctive Format C signatures**. The loader's `GetModelSerials()` at
line 729 reads the first session file's bytes `0x27`–`0x36` and checks
the `"YH"` prefix.

### What `Detect()` checks

`yuwell_loader.cpp:708-727` simply calls `GetModelSerials()` and accepts
the card if at least one subdirectory starting with `"YH"` contains a
`.BYS` file whose offset-`0x27` 16-byte field starts with `"YH"`. Both
satisfied here:
- `YH825A-V25738024/` matches the `"YH"` directory-name prefix.
- `00100001.BYS` offset `0x27` reads as `YH825A-V25738024` ASCII.

### Session file (`00100XXX.BYS`) — internal layout

Decoded against `YuwellFormatC::OpenSession` (`yuwell_loader.cpp:839-1013`).
Format: **60-byte header + N × 40-byte per-minute records.**

**Header (60 bytes)**:

| Offset | Length | Field | First-session value |
|---|---|---|---|
| `0x00` | 2 (LE) | start_year | `0x07EA` = 2026 |
| `0x02` | 1 | start_month | `0x03` = March |
| `0x03` | 1 | start_day | `0x18` = 24 |
| `0x04` | 1 | start_hour | `0x13` = 19 |
| `0x05` | 1 | start_minute | `0x39` = 57 |
| `0x06` | 1 | start_second | `0x0F` = 15 |
| `0x07` | 2 (LE) | finish_year | `0x07EA` = 2026 |
| `0x09` | 5 | finish month/day/hour/min/sec | 2026-03-24 20:22:17 |
| `0x0E` | 2 (LE) | record_count | `0x0019` = 25 records ✓ matches 25-min session |
| `0x10` | 5 | skipped | |
| `0x15` | 1 | humidity_settings | `00` |
| `0x16` | 17 | skipped | |
| `0x27` | 16 | model-serial ASCII | `YH825A-V25738024` |
| `0x37` | 5 | skipped | |

**Per-minute record (40 bytes each)**:

Existing loader field map (`yuwell_loader.cpp:945-972`):

| Offset | Field (loader) | First-record value | Apparent reality |
|---|---|---|---|
| `0x00` | constant `0xF9` skipped | `f9` ✓ | |
| `0x01` | `mode` | `03` | BiPAP mode code (loader expects 0/1 for CPAP/APAP) |
| `0x02` | skipped (loader treats as reserved) | **`57`** | **Candidate IPAP × 10 — varies per record** |
| `0x03` | skipped | `00` | High byte of IPAP (LE short) |
| `0x04` | skipped | **`2f`** | **Candidate EPAP × 10 — varies per record** |
| `0x05` | skipped | `00` | High byte of EPAP (LE short) |
| `0x06`–`0x0B` | skipped | `00 00 00 00 00 00` | Reserved / additional BiPAP fields |
| `0x0C` | `pressure` | **`00`** | **Always zero on this card** |
| `0x0D` | `initial_pressure` | **`00`** | **Always zero** |
| `0x0E`–`0x11` | skipped | `13 02 02 02` | |
| `0x12` | `ramp` | `0f` = 15 | Plausible |
| `0x13`–`0x14` | skipped | `03 21` | |
| `0x15` | `tidal_volume` (LE short) | `e6 00` = 230 | Plausible |
| `0x17` | `leak_volume` | `0a` = 10 | Plausible |
| `0x18` | skipped | `00` | |
| `0x19` | `minute_volume` | `04` | Plausible |
| `0x1A`–`0x1E` | skipped | `00 00 00 4e 00` | |
| `0x1F` | `inspiratory_ratio` | `2f` | |
| `0x20`–`0x21` | skipped | `7f ff` | |
| `0x22` | `respiratory_rate` | `14` = 20 | Plausible |
| `0x23` | `oai` | `00` | |
| `0x24` | `hi` | `00` | |
| `0x25` | skipped | `00` | |
| `0x26` | `cai` | `00` | |
| `0x27` | skipped (record-trailing `0xFA`) | `fa` | |

### The pressure-zero finding — confirmed across multiple sessions

Verified on the first record of session 1 and session 23, and a mid-session
record from session 23:

| Sample | Bytes 0x02-0x05 (IPAP/EPAP candidates) | Byte 0x0C (loader's `pressure`) |
|---|---|---|
| Session 1 record 1 | `57 00 2f 00` → IPAP 8.7, EPAP 4.7 | `00` |
| Session 1 record 2 | `5e 00 36 00` → IPAP 9.4, EPAP 5.4 | `00` |
| Session 23 record 1 | `55 00 2d 00` → IPAP 8.5, EPAP 4.5 | `00` |
| Session 23 record 5 | `6a 00 42 00` → IPAP 10.6, EPAP 6.6 | `00` |

Bytes 0x02 and 0x04 vary independently between records (sensible for a
BiPAP — IPAP and EPAP change minute-to-minute as a function of the
device's response to flow). Byte 0x0C is uniformly zero.

### Diagnosis

The loader's record-decode at `yuwell_loader.cpp:945-972` was written
against the YH-830 (CPAP/APAP), which has one effective pressure setting
per minute. YH-825 is a BiPAP, and the device writes **two** pressure
values per minute (IPAP and EPAP). On this card, those appear to be
stored as little-endian 16-bit values at record offsets `0x02` and `0x04`,
which the loader currently treats as reserved/skipped. The byte at offset
`0x0C` that the loader reads as `pressure` is unused by the BiPAP
firmware, so it stays at zero.

The result for OSCAR: a YH-825 imports cleanly with no errors, daily
hours/usage and tidal-volume/respiratory-rate channels populate correctly,
but the **pressure chart shows a flat zero line for every session**.

The loader's own comment at line 902 hints at this:
```cpp
maximum_pressure = 150; // TODO: This doesn't seem to exist?
// Missing stuff, especially the CPAP mode and ramp time
// unsigned char mode, minimum_pressure, maximum_pressure;
// unsigned char avg_leak_volume, avg_pressure;
```
The author knew the Format C record layout was incompletely decoded.

### Pressure decode — candidate locations (pending verification)

Per project policy this is **a candidate decoding, not a prescribed code
change.** The IPAP/EPAP placement at record offsets `0x02-0x03` and
`0x04-0x05` is what bytes are present on this single YH-825 sample; it
has not been verified against the vendor's own software or a second YH-825
sample. If the candidate decoding holds across more YH-825 cards (and any
future YH-820 sample), the loader's record-decode at
`yuwell_loader.cpp:945+` could read those two LE shorts as IPAP/EPAP and
emit them as separate channels — but only after that verification.

### Comparison across the four Yuwell samples now in catalogue

| Aspect | YH-550 (Format A) | YH-580 (Format B) | YH-825 (Format C, this) | YH-680/690 (Format D) |
|---|---|---|---|---|
| Device class | CPAP/APAP | CPAP/APAP | **BiPAP** | CPAP/APAP |
| Storage model | per-session files | single 64 KB root file | per-session files | per-session subdirs (3 files each) |
| Root marker | `RunLog.bys` (size 0) | `YHSD-NEW.BYS` (64 KB) | **`SN.BYS` (16 B ASCII)** | (none required) |
| Identity offset in `.BYS` | `0x20` | `0x84` of YHSD-NEW | `0x27` | `0x20` |
| Per-minute record size | (varies) | 30 B summaries + variable detail | **40 B** | (varies by file class) |
| Pressure decode complete? | yes | yes | **No — BiPAP IPAP/EPAP not extracted** | yes |
| Waveform data | None | None | None | YH-690 yes; YH-680 no |

### Observations / open questions

- **`SN.BYS` (16 ASCII bytes at root, no framing) is a new artifact in
  the catalogue** — Formats A/B/D don't have it. Useful as a Format C
  signal alongside the `0x27` identity offset and the YH-prefixed
  subdirectory.
- **`mode = 0x03` in BiPAP records** vs the loader's `YUWELL_FORMATC_CPAP`
  and `YUWELL_FORMATC_APAP` constants (presumably `0x00` and `0x01`) —
  `0x03` is unrecognized, so the loader sets `CPAP_Mode = MODE_UNKNOWN`
  (`yuwell_loader.cpp:985`). A future BiPAP mode constant (`0x03 =
  YUWELL_FORMATC_BIPAP`?) would let the loader at least label the
  session correctly. Candidate, pending verification.
- **Per-record `mode` byte** — the loader comment at line 953 wonders if
  the device allows mode changes mid-session. The byte is constant at
  `0x03` across all 23 sessions on this card; no support for the
  mode-switching hypothesis here.
- **Trailing `0xFA` at byte `0x27` of each record** (the loader's last
  skipped byte) — same role as the leading `0xF9` magic: record framing.
  Worth a parser sanity check but the loader skips both so the framing
  is implicitly validated by file-size arithmetic.
- **23 sessions over an unknown calendar span** — `00100001` to
  `00100023` numbering suggests an internal session counter starting at
  decimal 100001. If the device wraps at 100999 the lifetime ceiling is
  999 sessions; if it overflows into 0010100x and beyond, the ceiling is
  higher. Not enough samples to characterise.
- **`SN.BYS`'s 16-byte ASCII form is suspiciously regular** — it
  matches the same 16-byte slot the per-session file uses at offset
  `0x27`. The device may be writing the model-serial to two places for
  convenience; redundant but defensible.

---

## Philips Respironics DreamStation Go Auto (PRS1 model 500-series)

**Sample:** `C:/Users/Guy/Downloads/DS-Go-Auto-500G150-P-SERIES Two Dogs`
**Devices on this card:** **two** PRS1 devices' data on the same SD card:

| Device folder | Model | Serial | Software | FD (first) | LD (last) | Status |
|---|---|---|---|---|---|---|
| `34718904` | `500G150` (DS Go Auto) | `J34718904AA3C` | `V1.1.6.1694` | 2023-01-31 | 2023-08-11 | **active** (per `LAST.TXT`) |
| `22164904` | `500G110` (DS Go) | `J22164904CC5C` | `V1.1.4.1572` | 2018-08-30 | 2022-12-18 | older — kept on card |

**Loader:** `prs1_loader.cpp` + `prs1_parser*.cpp`. This is the **cleartext**
PRS1 path (DreamStation 1 / DreamStation Go family), the counterpart to the
DreamStation 2 entry above.

### File set

```
<root>/
└── P-SERIES/
    ├── LAST.TXT                          8 bytes — points to active device folder ("34718904")
    ├── 34718904/                         active device (DS Go Auto 500G150)
    │   ├── PROP.TXT                    337 bytes — cleartext key=value device properties
    │   ├── ACD.SEQ                       7 bytes — short device-sequence record
    │   ├── LOG.SEQ                      74 bytes — recent-log sequence
    │   ├── D/                           14 daily summaries (000.003 … 013.003)
    │   ├── E/                           empty in this sample
    │   ├── P0/                         450 session files (150 sessions × 3 extensions)
    │   │   ├── 00000005.001            ─┐
    │   │   ├── 00000005.002             │  one session = three files,
    │   │   ├── 00000005.005             │  hex session ID, three extensions
    │   │   …                            │
    │   │   ├── 0000009C.001             │  highest session ID = 0x9C = 156
    │   │   └── 0000009C.005            ─┘
    │   └── U/                           empty (updates? user?)
    └── 22164904/                        older device (DS Go 500G110)
        ├── PROP.TXT, ACD.SEQ, LOG.SEQ   same skeleton
        ├── D/                          271 daily summaries (vs PROP says DFN=61 — see below)
        ├── E/                           1 event file
        ├── P0/                         1,032 session files (vs PROP says PFN=332)
        ├── P1/                           429 session files in a SECOND data partition
        └── U/                           empty
```

### What `Detect()` checks vs. what's there

Same as for DreamStation 2: `prs1_loader.cpp:572` requires `P-Series/`
(case-insensitive) with at least one subdir containing either `PROP.TXT` or
`PROP.BIN`. This card has the **cleartext `PROP.TXT`** path.

The loader sorts device folders by `lastModified()` (oldest first) so both
`22164904` and `34718904` will be processed — `LAST.TXT` is **not consulted**
by detect or by loading order, only by sorting; the loader picks up whichever
folder it iterates last (mtime-newest) as the most-recent.

### `PROP.TXT` — cleartext key=value properties

Active device (`34718904`):

```
CF=4
SN=J34718904AA3C
MN=500G150
PT=0x73
DF=0
DFV=3
F=0
FV=6
SV=V1.1.6.1694
FD=1675156441       ← First Date (Unix epoch, 2023-01-31 09:14:01 UTC)
LD=1691726401       ← Last Date  (Unix epoch, 2023-08-11 02:00:01 UTC)
FN=0
PFN=450             ← P0/ File count
EFN=0               ← E/ file count
DFN=14              ← D/ file count
SID=0,1,2,3,4,5,6
BK=0x000000000006e12a3188   ← Boot key?
SK=0x00004dc8020dbd7d2815   ← Session key
EK=0x00000000000800000000   ← Event key
DK=0x0000000e030aac6fb16f   ← Daily key
TS=0,7,0,0,0,0,0
DC=0,0,1,1,1,1,1,1          ← Day Counts? boolean week pattern
P=
G=
DPK=
VC=0xBCC849B2               ← Verification Code (checksum)
```

| Key | Meaning |
|---|---|
| `CF` | Config Format version (`4` here) |
| `SN` | Serial Number (13-char: `J` + 8-digit device-folder name + 4-char suffix) |
| `MN` | **Model Number** — `500G150` = DS Go Auto, `500G110` = DS Go, `410X150C` = DS 2 family, `560P*` = System One, etc. |
| `PT` | Product Type byte (hex) |
| `DF` / `DFV` | Data Format / version |
| `F` / `FV` | Firmware family / version |
| `SV` | Software Version string (humans-readable) |
| `FD` / `LD` | First / Last date as **Unix epoch seconds** |
| `PFN` / `EFN` / `DFN` | "Active" file counts for `P0/`, `E/`, `D/` (does **not** match actual on-disk counts when files have been retained beyond pruning — see below) |
| `SID` | Session ID range (e.g. `0,1,2,3,4,5,6`) |
| `BK` / `SK` / `EK` / `DK` | Hex blobs — likely per-subsystem state hashes or encryption keys |
| `VC` | Verification Code — a checksum over the rest of the file |

The serial number format `J<8-digit-folder-name><4-char>` means the **device-folder
name is literally the middle of the printable serial**: e.g. `34718904` →
serial `J34718904AA3C`. This is a clean discoverable mapping (unlike
DreamStation 2's hex-hash folder names where the relationship isn't obvious).

### File counts: PROP.TXT lies about the actual on-disk total

On the older device (`22164904`):

| Folder | Actual files | PROP says | Gap |
|---|---|---|---|
| `D/` | 271 | `DFN=61` | 210 stale daily-summary files |
| `E/` | 1 | `EFN=1` | matches |
| `P0/` | 1,032 | `PFN=332` | 700 stale session files |
| `P1/` | 429 | (no `P1FN` key) | (entire second partition not counted in PROP) |

PROP's `*FN` keys describe the **logically-current** file set, not the
filesystem reality. Older files are retained on disk after the device has
considered them "pruned" — they aren't deleted, just dropped from PROP's
bookkeeping. A loader has two options:
1. Trust PROP and read only the IDs in `SID` (misses historical data).
2. Read the actual filesystem (gets old data, but must handle out-of-PROP-range
   sessions gracefully).

OSCAR's PRS1 loader scans the filesystem directly (per `PRS1Loader::Open()`).

### `Px/` partitioned data — second partition on overflow

`22164904` has both `P0/` and `P1/` — same `.001/.002/.005` structure in
each. The split is **not** alphabetical: `P0/` highest session is below `P1/`'s
session range. Looks like the device fills `P0/` to a cap (~1000 files) then
rolls over to `P1/`. A future card could have `P2/`, `P3/`, etc.

### File header layout (cleartext)

Every file in `P0/`, `P1/`, `D/`, and likely `E/` begins with a structured
12-byte header. From `34718904/P0/00000005.001`:

| Offset | Length | Field | Value on this sample |
|---|---|---|---|
| `0x00` | 2 | u16 LE — record length | `03 b4` = 948 |
| `0x02` | 2 | u16 LE — flags / sub-type | `00 00` (varies between `.001` and `.002`/`.005`) |
| `0x04` | 1 | reserved | `00` |
| `0x05` | 1 | **File-family marker** | `06` (consistent across all `P0/`, `D/` files) |
| `0x06` | 1 | **File-type ID — matches the extension's last digit** | `01` for `.001`, `02` for `.002`, `05` for `.005`, `03` for `D/*.003` |
| `0x07` | 1 | Protocol version | `05` |
| `0x08` | 2 | Flags | `00 00` |
| `0x0A` | 4 | **Unix timestamp LE — session start time** | `ca 8d b9 63` = `0x63b98dca` = 2023-01-05 12:27:54 UTC |
| `0x0E`+ | … | Type-specific payload | (varies) |

All three files for a single session (`<id>.001`, `.002`, `.005`) share the
same byte-`0x0A` Unix timestamp — useful for grouping files into sessions
even if filenames are obfuscated. The `D/*.003` daily-summary files carry the
day's anchoring timestamp at the same offset.

### Daily summaries vs sessions

- `D/` contains **one file per day** that the device was used (here, 14 daily
  files for the 7-month active span — significant gaps between use).
- `P0/` contains **one triplet per session** (here, 150 sessions over 14 days
  of use = ~10 sessions/day → suggests aggressive nap-segmentation rather than
  one long night per day).
- Session IDs in `P0/` are zero-padded hex (`00000005` to `0000009C` — letters
  do appear, e.g. `0000009C`, confirming hex encoding).
- Daily IDs in `D/` are zero-padded **decimal** (`000`–`013` for 14 days).
  Subtle but real difference from `P0/` IDs.

### DreamStation 1/Go vs DreamStation 2 — final discriminator table

| Feature | DS 1 / DS Go (this sample) | DS 2 (Arie Klerk sample) |
|---|---|---|
| Properties filename | **`PROP.TXT`** (cleartext key=value) | **`PROP.BIN`** (encrypted) |
| Session extensions | **`.001`, `.002`, `.005`** | **`.B01`, `.B02`, `.B05`** |
| Daily-summary extension | **`.003`** | **`.B03`** |
| File-type marker (byte `0x06`) | Plain integer matching extension | Encrypted — not visible |
| Per-card UUID header | None (cleartext) | Yes — `3bd04f46-…` repeated in every file |
| Header structure | 12-byte cleartext with Unix timestamp at `0x0A` | ~96-byte UUID envelope |
| Device-folder name | Embedded inside printable serial (`34718904` ↔ `J34718904AA3C`) | Opaque 8-char hex hash (`75FAE30D`) |
| `ACD.SEQ` file | **Present** (7 bytes) | Absent |
| `LOG.SEQ` size | 74 bytes (structured cleartext) | 276 bytes (encrypted) |
| Multi-device on one card | Possible (two device folders observed) | (single in our sample, but loader supports it) |
| Multi-partition (`P1/`+) | Possible (observed on older device) | (only `P0/` in our sample) |
| Model numbers in this family | `500G110`, `500G150`, also `5xxP*`, `560P*` etc. | `4xxX*` family |

The `PROP.TXT` vs `PROP.BIN` filename remains the single clean detect-time
discriminator — but **once into parsing**, the file-extension pattern
(`.0Y` vs `.BY`) is even more reliable: every P-Series file under DS 2's
encrypted regime uses `.B<n>` extensions throughout.

### Observations / open questions

- **Two devices on one card is a real-world scenario** that any PRS1 loader
  needs to handle gracefully. OSCAR's `FindMachinesOnCard()` already returns a
  list, but UI-side handling (which device the user actually wants) may need
  thought.
- **`PROP.TXT` lying about file counts** is normal, not corruption. A loader
  that relies on PROP's `PFN`/`DFN`/`EFN` to size buffers or for progress
  reporting will be wrong on any card with retained-but-pruned older sessions.
- **Header type byte at `0x06`** is an excellent integrity check — if a file
  named `*.001` has byte `0x06 != 0x01`, the extension is lying or the file
  is corrupt.
- **DS Go model code `500G150` vs `500G110`** — the trailing digit appears to
  distinguish minor revisions (Auto vs non-Auto, possibly humidifier presence).
  Worth correlating against the PRS1 model-number lookup tables.
- **The `BK/SK/EK/DK` hex blobs in PROP.TXT** could be encryption keys for
  later firmwares, or just opaque state hashes. They aren't referenced by the
  current cleartext loader path, but might gate future features.
- **`ACD.SEQ` (7 bytes)** — small fixed-shape; possibly an "Active Card
  Designation" or "Activation Counter". DS 2 doesn't have it. Could be useful
  as a generation-discriminator on its own (`ACD.SEQ` present → DS 1/Go;
  absent → DS 2).

---

## Philips Respironics DreamStation CPAP 200X110 (long-running brick, 5 partitions)

**Sample:** `C:/Users/Guy/Downloads/Glyn Gowing - PRS1-200X110-J189837947F19/PRS1-200X110-J189837947F19`
**Device:** Philips Respironics DreamStation CPAP (basic), model `200X110`,
serial `J189837947F19`, software `V1.1.1.2860`, `Family=0 / FamilyVersion=6`.
**Loader status:** `prs1_loader.cpp` — **supported as a brick.** Model
`200X110` is in `s_PRS1ModelInfo::m_bricks` (`prs1_loader.cpp:180` —
`{ "251P", "261CA", "261P", "200X110", "501V" }`). The loader detects and
loads the device but `IsBrick(info.modelnumber)` returns true, triggering
`emit deviceReportsUsageOnly(info)` (`prs1_loader.cpp:996-998`). OSCAR
shows usage hours and the warning "this device reports usage only" rather
than full charts.

This is the **first PRS1 brick** in the catalogue, the **third DS1/Go
cleartext** sample, and the longest-running PRS1 card seen so far —
**~4 years 3 months** of use (2018-03-14 → 2022-06-18) accumulated across
**5 Px partitions** with **3083 session files**. The brick equivalent on
the ResMed side is the AirSense 10 CPAP basic, which writes daily
summaries only into STR.edf and leaves DATALOG empty. The 200X110 is more
verbose — it does write per-session and per-day files — but at ~120 bytes
each, those payloads carry only start/stop timestamps and usage counters,
not the per-minute or waveform channels OSCAR would chart.

### Card-root structure

```
<root>/
├── .fseventsd/                                 macOS Spotlight metadata (not part of Philips format)
│   ├── 63657337d0b7195c                       (124 bytes)
│   ├── 63657337d0b7195d                       (75 bytes)
│   └── fseventsd-uuid                         (36 bytes)
└── P-SERIES/
    ├── LAST.TXT                                8 bytes ("18983794" — points to active device folder)
    └── 18983794/                               device folder (last 8 digits of serial)
        ├── ._PROP.TXT                          4,096 bytes — macOS AppleDouble resource fork (not part of format)
        ├── PROP.TXT                            320 bytes — cleartext device properties (abbreviated-key form)
        ├── LOG.SEQ                             74 bytes — recent-log sequence
        ├── D/                                  2 daily summaries (000.003, 001.003 — 57 and 40 bytes)
        ├── E/                                  empty
        ├── P0/                                 1000 session files (00000003.000 … ~0x3EA.000)
        ├── P1/                                 702 session files (000001F7.000 …)
        ├── P2/                                 500 session files (000003EB.000 …)
        ├── P3/                                 500 session files (000005DF.000 …)
        └── P4/                                 381 session files (000007D3.000 … 0000094F.000)
```

The `.fseventsd/` directory and `._PROP.TXT` are macOS injections from the
user putting the card into a Mac for capture — Spotlight indexing
artifacts and AppleDouble resource forks respectively. **Neither is part
of the Philips format.** A fingerprint check should ignore both: any
PRS1 card moved through a Mac will pick them up.

### PROP.TXT — abbreviated-key form

This card uses the **short-key** form of PROP.TXT (single- and double-letter
keys), in contrast to the BiPAP A40 sample's **long-key** form
(`CardFormat=`, `SerialNumber=`, `ModelNumber=`). Both are read by the
same loader — `prs1_loader.cpp:679-683` maps abbreviations to long
forms (e.g. `MN → ModelNumber`, `F → Family`, `FV → FamilyVersion`, `PT
→ ProductType`).

```
CF=2                          (CardFormat — bumped from 1 on the A40 sample)
SN=J189837947F19              (SerialNumber — J-prefix DS1/Go family)
MN=200X110                    (ModelNumber — DreamStation CPAP basic, brick)
PT=0x58                       (ProductType — distinct from A40's 0x55)
DF=0                          (DataFormat)
DFV=3                         (DataFormatVersion — older than A40's 2; not strictly monotonic)
F=0                           (Family — consumer DS1/Go/2 lineage)
FV=6                          (FamilyVersion — DreamStation 1/Go/2 generation)
SV=V1.1.1.2860                (SoftwareVersion — V-prefix build, older than DS Go's V1.1.4/V1.1.6)
FD=1521050400                 (FirstDate — 2018-03-14 22:00:00 UTC)
LD=1655551165                 (LastDate — 2022-06-18 11:19:25 UTC)
FN=1                          (FolderNum — active partition pointer)
PFN=202                       (PatientFolderNum — total patient-folder count?)
EFN=0                         (EquipFolderNum)
DFN=1                         (DataFolderNum)
SID=0,1,2,3,4,5,6             (Session-ID list — 7 values matching the 5+pad partitions)
BK=0x00004f2c00062a4fbaab     (BlockKey — 10-byte hex value)
SK=0x0000000002026034884e     (SessionKey — 10-byte hex)
EK=0x00000000000800000000     (EventKey — 10-byte hex, mostly zero)
DK=0x0000000203088901be3a     (DataKey — 10-byte hex)
TS=0,7,0,0,0,0,0              (TimeStamps array — 7 values matching SID)
DC=0,0,0,0,0,0,0              (DayCounts array — all zero, brick doesn't track per-day data)
VC=0xE57BB8BB                 (ValidCheck — file checksum)
```

Compared with the DS Go Auto sample (`500G150`, also F=0 V=6):

| Field | DS CPAP (this, brick) | DS Go Auto |
|---|---|---|
| `MN` | `200X110` (brick) | `500G150` (full data) |
| `SV` | `V1.1.1.2860` | `V1.1.6.1694` |
| `PT` | `0x58` | `0xC6` |
| `CF` (CardFormat) | `2` | (presumably 2 — same generation) |
| `DFV` (DataFormatVersion) | `3` | (likely lower — older firmware) |
| `D/` daily files | 2 | 14 |
| `Px/` partitions | 5 (P0-P4) | 1 (P0) |
| Total session files | 3083 | ~257 |
| Data span | ~4 years 3 months | ~7 months |

### Multi-partition overflow — first sample with 5 partitions

The DS Go Auto entry noted that `Px/` is partitioned on overflow. This is
the first card in the catalogue with the partition mechanism fully
exercised — P0 through P4, with session IDs running 0x003 through ~0x94F
(= ~2380 distinct session IDs across 3083 files; some session IDs may be
written to more than one partition during rotation).

Sample partition-start IDs:
- P0: starts at 0x003, 1000 files
- P1: starts at 0x1F7 (= 503)
- P2: starts at 0x3EB (= 1003)
- P3: starts at 0x5DF (= 1503)
- P4: starts at 0x7D3 (= 2003), ends 0x94F (= 2383), 381 files

P1 onward each step by `0x1F4` = 500 IDs — partitions hold ~500 sessions
each, with P0 sized larger (1000). Once a partition fills, a new one is
created. The active partition pointer is the `FN` (FolderNum) field in
PROP.TXT.

OSCAR's loader presumably scans every Px sub-folder via the directory
iteration in `FindMachinesOnCard()` and the per-partition logic in the
existing parsers — full coverage of this card would require walking all
five.

### Session-file format — brick-shape (120 bytes typical)

`P0/00000003.000` hex dump (first 120 bytes — entire file):

```
03 78 00 00 00 06 00 03 00 00 00 f5 2a a8 5a 08
00 01 01 35 02 09 03 04 04 02 05 02 06 04 07 08
1c 00 01 01 00 01 00 01 02 00 00 0a 01 8c 2b 01
00 2c 01 1e 2d 01 28 2e 01 80 2f 01 00 30 01 03
35 02 40 60 36 01 00 38 01 00 39 01 00 40 01 02
3c 01 00 3e 01 80 3f 01 00 03 00 00 40 60 04 32
00 07 00 00 00 00 00 00 a0 9e 02 17 00 01 16 00
29 00 00 00 df 3c 11 86
```

Structure (standard PRS1 chunked-file header, decoded from
`prs1_parser.cpp` knowledge):
- byte 0: `0x03` — chunk type marker
- bytes 1-4 (LE): `0x00000078` = **120** — chunk length (matches file size)
- byte 5: `0x06` — family-version (matches `FV=6`)
- bytes 6-10: chunk metadata
- bytes 11-15: `f5 2a a8 5a 08` — timestamp + counter (`0x5AA82AF5` = 2018-03-14 22:00:53 UTC, matches PROP.TXT `FD=1521050400`)
- middle: tag-length-value records (TLV) — settings and usage counters
- last 4 bytes: `df 3c 11 86` — chunk CRC32

`D/000.003` (57 bytes) has the same `03 ?? 00 00 00 06 ...` envelope — the
daily file uses the same chunked structure with smaller payload. The
`.003` suffix marks file class 3 (daily summary), versus `.000` for
session-record files. **No `.002` (waveform) and no `.001` (event log)
files exist on this card** — those classes are the ones the brick simply
doesn't emit.

### LOG.SEQ — recent-log sequence

`74 bytes` of binary content (visible mostly as control characters). Same
filename and approximate size as the DS Go Auto's `LOG.SEQ` (`74 bytes`
exactly) — appears to be a fixed-format short-log buffer. Not a brick
discriminator; both brick and full-data DS1/Go devices write one.

### Date span and pruning

**~4 years 3 months** of usage on a single card with no archiving — the
longest active-device history in this catalogue (S9 carried more
*recorded* days but had been backed up off-card and reconstituted). This
suggests:
- DS1/Go-family bricks do **not** auto-prune the card. Sessions accumulate
  across partitions until the user (or clinician) clears it.
- The 5-partition × ~500-file structure may be the device's storage
  ceiling — if a future sample shows more than 5 partitions, the storage
  ceiling can be revised upward.
- For OSCAR's import-performance budget, **this is the realistic
  worst-case PRS1 card size** to optimise against, not the smaller DS Go
  Auto sample.

### Brick model class — what OSCAR shows

When `IsBrick(modelnumber)` returns true (`prs1_loader.cpp:237-249`):
- The device is registered and identified normally.
- `deviceReportsUsageOnly(info)` is emitted — the UI shows a notice that
  this device records usage only, not detailed therapy data.
- Import still walks the D/ and Px/ files (the loader doesn't shortcut on
  the brick flag) — usage hours, mask-on/off times, and pressure
  settings still come through.
- AHI, leak, flow, and pressure waveforms are not available — the device
  doesn't write them.

The `m_bricks` set has 5 members: `251P`, `261CA`, `261P`, `200X110`,
`501V`. The first three are System One bricks; `200X110` is the
DreamStation 1 brick; `501V` is the Dorma 500 (System One 60-Series).
Additionally, `IsBrick` falls back to **"any model whose first character
is `2`"** (`prs1_loader.cpp:243-245`) for unknown models — a reasonable
heuristic given the `2xx`/`2xxx` model-number prefix pattern Philips uses
for entry-level CPAPs.

### What this sample adds to the catalogue

- **First Philips brick** — establishes the brick-import behaviour next to
  the existing AS10 CPAP basic brick-equivalent on the ResMed side.
- **First multi-partition Philips card** — exercises the Px overflow
  mechanism the DS Go Auto entry hinted at.
- **Longest-running active PRS1 sample** — 4-year history pushes the
  import-performance envelope.
- **Abbreviated-key PROP.TXT form** — confirms both short-key
  (`SN=`, `MN=`, …) and long-key (`SerialNumber=`, `ModelNumber=`, …)
  syntaxes coexist within the same loader; the loader handles both via
  the key-translation table at `prs1_loader.cpp:679-683`.
- **macOS-injected metadata** (`.fseventsd/`, `._PROP.TXT`) — a recurring
  fingerprinting nuisance for cards that have passed through a Mac.
  Future fingerprint matches should treat both as "ignore — host OS
  artifact," not as Philips-format features.

### Observations / open questions

- **`SID=0,1,2,3,4,5,6` has 7 entries but only 5 Px partitions exist** —
  may include a buffered "next-partition" pointer and a sentinel.
- **`TS=0,7,0,0,0,0,0`** — the `7` in position 1 likely indicates P1 has
  the most-recent stored timestamp; other positions are zero. Worth
  cross-referencing with the actual file mtimes on a future card.
- **`DC=0,0,0,0,0,0,0` is all-zero** — consistent with the brick not
  tracking per-day data. On a non-brick card this array would likely
  carry daily-session counts.
- **`DFV=3` is lower than the A40's `DataFormatVersion=2`** despite
  being released later. The "DFV" version field doesn't behave
  monotonically across families — F=0 V=6 brick uses `DFV=3`; F=3 V=4
  vent uses `DFV=2`. Field appears to be **family-local**, not global.

---

## Philips Respironics BiPAP A40 (hospital NIV — PRS1-detected, **not supported**)

**Sample:** `C:/Users/Guy/Downloads/AKLERK 20240205_Philips_BiPAP_A40 Arie Klerk/20240205`
**Device:** Philips Respironics BiPAP A40 non-invasive ventilator, serial
`V12281862919F`, model number `1109596`, software version `3.6.5`,
ProductType `0x55`, **Family=3 / FamilyVersion=4**.
**Loader status:** `prs1_loader.cpp` **detects this card** (`P-SERIES/` is
present) but **rejects the device as unsupported** — the
`(family=3, familyVersion=4)` combination is **not in `s_PRS1TestedModels`**
(`prs1_loader.cpp:158-165` covers only F3 versions 0/3/6 — the C-Series,
System One 60-Series, and DreamStation BiPAP S/T trees). `IsSupported()`
at line 978 fails, the loader emits `deviceIsUnsupported` at line 989, and
no session data is imported.

The BiPAP A40 is a **hospital-grade non-invasive ventilator** (NIV — same
class as the Trilogy / A30 / A40 Pro line), distinct from Philips's
consumer CPAP/APAP lineup (DreamStation / DS Go / DS 2). This is the
fourth Philips device in the catalogue and the first that is **outside
the supported PRS1 family table** despite triggering the PRS1 detection
path. Its main value is documenting the **dual-tree SD layout** the A40
writes: a P-SERIES stub that satisfies PRS1 detection, plus a parallel
`BIPAP-A/` tree where the **real** therapy data lives in standard EDF+D
files that no current OSCAR loader reads.

### Card-root structure — two parallel data trees

```
<root>/
├── BIPAP-A/                                  <-- real therapy data (EDF+D, ignored by all loaders)
│   ├── A2312000.EDF … A2402001.EDF           ~43 annotation files (events)
│   ├── D2312000.EDF … D2402001.EDF           ~43 daily/summary files (per-minute aggregates)
│   └── W2401150.EDF … W2402050.EDF           ~22 waveform files (3-4 MB each, ~83 MB total)
└── P-SERIES/                                 <-- PRS1-compatible stub
    ├── LAST.TXT                              8 bytes — points to active device folder ("12281862")
    └── 12281862/                             device folder (last 8 digits of serial)
        ├── PROP.TXT                          298 bytes — cleartext device properties
        ├── E/                                empty
        └── P0/                               64 session-pair files (00000C2.001/.002 … 00000101.001/.002)
```

No `D/`, no `Px/n>0`, no `ACD.SEQ`, no `LOG.SEQ` — the P-SERIES side is
much sparser than the DreamStation Go Auto's P-SERIES layout (which has
all of those). The A40 appears to write a minimal P-SERIES manifest for
PRS1 SD-card recognition while keeping its actual data in `BIPAP-A/`.

### What `Detect()` checks vs. what's there

`prs1_loader.cpp:572` (`PRS1Loader::Detect`) → `GetPSeriesPath()` finds
`P-SERIES/` (case-insensitive) → `FindMachinesOnCard()` walks each child
folder looking for `PROP*.TXT` or `PROP.BIN`. The `12281862/PROP.TXT`
satisfies this; `Detect()` returns true. The `BIPAP-A/` tree is **not
consulted** by `Detect()` — only `P-SERIES/`.

### `PROP.TXT` — fields and the `(F3, V4)` rejection

```
CardFormat=1
SerialNumber=V12281862919F
ModelNumber=1109596
ProductType=0x55
DataFormat=0
DataFormatVersion=2
Family=3
FamilyVersion=4
SoftwareVersion=3.6.5
FirstDate=1706978789       (= 2024-02-03 14:46:29 UTC)
LastDate=1706978789        (single date — fresh card / single session)
PatientFolderNum=0
PatientFileNum=4
SessionID=00000101         (= 257 decimal; matches the highest P0/ file pair)
EquipFileNum=0
ValidCheck=ab2b
```

Compared with the DS Go Auto sample's PROP.TXT:

| Field | A40 (this) | DS Go Auto (existing entry) | Notes |
|---|---|---|---|
| `ModelNumber` | `1109596` (7 digits) | `500G150` (3 digits + suffix) | Different model-number namespace |
| `ProductType` | `0x55` | `0xc6` | Per-platform ID |
| `Family` | **3** | 0 | F3 = bilevel S/T family |
| `FamilyVersion` | **4** | 6 | F3V4 not in tested-model table |
| `SerialNumber` prefix | **V** | J | A40 serials start `V`, DS Go/1/2 start `J` |
| `SoftwareVersion` | `3.6.5` (semver) | `V1.1.6.1694` (V-prefix + build) | Different version format |
| `DataFormat` / `DataFormatVersion` | `0` / `2` | `0` / `2` | Same data format declared — but `(F3,V4)` payload layout still unverified |

The `Family=3` lineage in OSCAR's table:

| Family | FV | Models | Class |
|---|---|---|---|
| 3 | 0 | 1061401 | BiPAP S/T (C Series) |
| 3 | 3 | 1061T, 1160P | BiPAP S/T 30, AVAPS 30 (System One 60 Series) |
| 3 | **4** | **none** | **← this A40 sample's slot, currently empty** |
| 3 | 6 | 1030X110, 1030X150, 1130X110, 1131X150, 1130X200 | DreamStation BiPAP S/T 30 / AVAPS 30 |

`(F3, V4)` plausibly belongs to the **A-Series ventilator generation**
(A30/A40/A40 Pro/A-Series Trilogy adjacency) that sits between System One
60 (V3) and DreamStation (V6) chronologically. **No code change is
prescribed here** — per project policy, adding to the tested-model table
requires verifying that the existing parser actually decodes `(F3,V4)`
session payloads correctly, which has not been attempted.

### `BIPAP-A/` — the data tree no loader reads

A flat directory with **three file classes**, all standard EDF+D
(`reserved` = `EDF+D` → EDF Plus Discontinuous, the spec for files with
gaps and annotations). Filename pattern `<class><YYMM><nnn>.EDF`:

| Class | Per-record duration | Signals | First two channel labels | Purpose | Typical size |
|---|---|---|---|---|---|
| `A` | 1 sec | 1 (annotations only) | (EDF+D annotations track) | Event log | 2.9–8 KB |
| `D` | 120 sec | 15 | `IPAP Measrd`, `IPAP Min` | Per-2-minute aggregated channels (pressure, leak, etc.) | 4–198 KB |
| `W` | 10 sec | 8 | `Press Patient`, `Flow CompTotal` | High-rate waveform — pressure & flow | 50 KB–4.5 MB |

Recording-ident string in every header: `V12281862919F 1109596 0x8408 3.6.5`
— **serial, model, productCode(hex), softwareVersion** — same identity as
PROP.TXT but with an additional product-code field `0x8408` (no obvious
counterpart in PROP.TXT; `ProductType=0x55` is unrelated). Patient field:
`MLHT40 0` — likely a Philips internal device-type code.

Filename suffix decoding (provisional, **not fully verified**):
- `<YY><MM>` is straightforward — `2312` = Dec 2023, `2401` = Jan 2024, `2402` = Feb 2024.
- `<nnn>` appears sequential within month for A/D files (000-023 in Dec,
  000-016 + 041 in Jan), but W files step by 10 (150, 160, 170, …, 310 in
  Jan; 010, 020, … in Feb). The W jump-by-10 may indicate that waveform
  files only start above a session-duration threshold and the suffix
  encodes some other index. Worth confirming on a second A40 sample.
- A and D files share suffixes (e.g. `A2312000` matches `D2312000`),
  suggesting they're paired per session. W files have their own numbering.

Data range: 2023-12-18 → 2024-02-05 (~7 weeks), ~43 sessions visible
across A/D pairs, ~22 W (waveform-bearing) sessions. Total `BIPAP-A/`
payload ≈ 85 MB — dominated by W files.

### Comparison across all four Philips entries

| Aspect | DS 2 (410X150C) | DS Go Auto (500G150) | DS Go (500G110) | **BiPAP A40 (1109596)** |
|---|---|---|---|---|
| OSCAR loader status | Supported (encrypted DS2 path) | Supported (cleartext DS1/Go path) | Supported (cleartext DS1/Go path) | **Detected but rejected** (F3V4 unsupported) |
| Top-level tree | Device-hash folder under root | `P-SERIES/<serial>/` | `P-SERIES/<serial>/` | **Both** `BIPAP-A/` **and** `P-SERIES/<serial>/` |
| Identity file | `PROP.BIN` (encrypted) | `PROP.TXT` (cleartext) | `PROP.TXT` (cleartext) | `PROP.TXT` (cleartext) |
| Family / FV | 0 / 6 | 0 / 6 | 0 / 6 | **3 / 4** |
| ModelNumber namespace | 6-char alnum (`410X150C`) | 4-7 char alnum (`500G150`) | 4-7 char alnum (`500G110`) | **7-digit numeric (`1109596`)** |
| Serial prefix | (encrypted, decoded `D03194…`) | `J` (e.g. `J34718904AA3C`) | `J` (e.g. `J22164904CC5C`) | **`V`** (e.g. `V12281862919F`) |
| SoftwareVersion format | (encrypted) | `V1.1.6.1694` | `V1.1.4.1572` | **`3.6.5`** (semver) |
| Real session data location | Encrypted device-hash folders | `P-SERIES/<serial>/D/` + `Px/` | `P-SERIES/<serial>/D/` + `Px/` | **`BIPAP-A/` (standard EDF+D)** |
| Standard EDF on card? | No | No | No | **Yes — all BIPAP-A files** |

The A40 is the first Philips device in this catalogue to use **standard
EDF on the SD card**. ResMed has always used EDF; Philips's consumer
CPAP/APAP line uses proprietary binary (cleartext or encrypted). For the
hospital ventilator family Philips appears to have followed the medical-
device convention of EDF rather than rolling its own.

### Why the P-SERIES stub exists

Plausibly two reasons:
1. **SD-card recognition by the DreamMapper / Care Orchestrator software**
   — `P-SERIES/<serial>/PROP.TXT` is how Philips's PC and mobile tools
   identify a Philips SD card. Without it, the same tools that read
   DreamStation cards wouldn't even attempt to open the A40 card.
2. **Common formatting tool** — Philips may use one SD-formatter that
   always lays down a P-SERIES skeleton. The A40 then populates the
   session-counter files (`P0/0000xxxx.001/.002`) for log-keeping while
   the actual therapy data goes into BIPAP-A.

The `P0/` files are non-empty binary (header bytes look like the standard
PRS1 chunked-file format), so the A40 *does* write something into the
PRS1-shaped tree — but whether OSCAR's `prs1_parser_vent.cpp` could decode
those files for `(F3, V4)` is unknown and would need direct verification.

### Observations / open questions

- **Two parallel data trees on one card is a new pattern** in the
  catalogue. Closest precedent is the DS Go Auto card carrying two
  *devices'* data in two `P-SERIES/<serial>/` siblings — but that's the
  same loader path for both. The A40 has **one device** writing to **two
  different formats** simultaneously. Suggests a transitional design,
  or a deliberate split between "high-fidelity local archive" (BIPAP-A)
  and "consumer-tooling compatibility" (P-SERIES).
- **`(F3, V4)` is a candidate pending verification** — adding it to
  `s_PRS1TestedModels` is a one-line edit, but per project policy we
  don't extend the supported table until the parser is confirmed to
  decode this device's `P0/` files correctly end-to-end. The cheaper path
  to A40 support may be writing a small loader that reads `BIPAP-A/*.EDF`
  directly, since those files are already in a documented international
  format. Out of scope for fingerprinting; logged as future work.
- **`MLHT40` in the EDF Patient field** appears to be a Philips internal
  device-class code. Worth checking other Philips hospital-device EDFs
  if any surface (Trilogy, A30, etc.) for a similar `MLHT*` prefix —
  could be a reliable signature of the A-Series tree.
- **`ProductType=0x55` and `0x8408` in EDF recording-ident**: two distinct
  product-code-style values for the same device. `0x55` is the PRS1
  PROP.TXT-style 1-byte type code; `0x8408` is a wider EDF-side code.
  The relationship between the two is unknown; either could be a useful
  discriminator if more A-Series samples surface.
- **No data older than 2023-12-18** on a card with capacity for ~85 MB and
  active through 2024-02-05 — only ~7 weeks of history. The A40 either
  prunes aggressively or this card was recently re-formatted before
  capture.
- **`PatientFileNum=4` vs ~64 P0 file pairs**: PROP.TXT claims 4 patient
  files, but `P0/` contains ~64 session-pair files (`.001`/`.002`). Either
  `PatientFileNum` refers to something else (folder count? archived
  patient profiles?) or the field isn't maintained the same way it is on
  DS Go.

---

## DeVilbiss BlueLake DV64D ("IntelliPAP 2 Blue StandardPlus") — first DV6 sample

**Sample:** `C:/Users/Guy/Downloads/Cuppa 500 - Cuppa (1)/Cuppa 500 - Cuppa`
**Device:** DeVilbiss BlueLake **DV64D** ("Blue StandardPlus"), serial
`ND16129035`, firmware `V023RN20160118` (built 2016-01-18), bootloader
`V014BL20150630`.
**Loader:** `intellipap_loader.cpp` — DV6 path (`OpenDV6` at
`intellipap_loader.cpp:91+` for DV5, separate code for DV6 lower in the
file). The model `DV64D` is in `testedModels` at line 622.
**Loader detection caveat:** this sample required user-side reshaping
before detection succeeded — see "On-disk layout" below.

This fills the previously-empty `intellipap_loader.cpp` slot in the
catalogue. DV6 is the DeVilbiss DV64-series (sold as "IntelliPAP 2"
internationally, and "BlueLake" / "Blue StandardPlus" / "Blue AutoPlus"
in some markets) — the successor to the earlier DV5x IntelliPAP. The DV5x
path uses `SL/` + `SET1`; the DV6 path uses `DV6/` + `SET.BIN`. Both are
handled by the same loader, dispatched by which subdirectory `Detect()`
finds.

### On-disk layout — `DV6/` wrapper required by `Detect()`

`intellipap_loader.cpp:60-86`:

```cpp
if (dir.cd(DV6)) {            // DV64
    return dir.exists(SET_BIN) ? true : false;
}
```

The loader requires a `DV6/` subdirectory inside the selected folder, and
`SET.BIN` inside that. **This sample as received had the `.BIN` files at
the root of the user's directory, no `DV6/` wrapper** — so `Detect()`
returned false and OSCAR rejected the card.

Diagnosis: when the SD card was copied to disk, the **contents** of the
on-card `DV6/` folder were extracted directly into the destination
directory, losing the wrapper. **User-side fix verified:** putting all
the `.BIN` files inside a `DV6/` subdirectory inside the parent folder
made the loader detect and import the card normally. No code change
required; no evidence that newer DV6 firmware writes flat.

Lesson for future fingerprinting: when an SD card image appears to match
a loader's file set but `Detect()` fails, **check whether a single
wrapper directory is missing** before considering a format change.

### Card-root structure (after adding `DV6/` wrapper)

```
<root>/
└── DV6/
    ├── VER.BIN          128 bytes  device identity (serial / model / firmware / bootloader)
    ├── SET.BIN          128 bytes  current settings — Detect() sentinel
    ├── SF.BIN           512 bytes  settings-history / extended-config block
    ├── A.BIN             56 bytes  small fixed-shape header (alarms? "A" = ?)
    ├── F.BIN            176 bytes  small fixed-shape (flags? family?)
    ├── O.BIN             56 bytes  small fixed-shape (options?)
    ├── H1.BIN            64 bytes  hour-meter (primary)
    ├── H2.BIN            64 bytes  hour-meter (secondary — likely backup of H1)
    ├── SC.BIN           256 bytes  SmartCode log — activation/access codes (4 entries visible)
    ├── S.BIN        114,511 bytes  per-day summary table (DV6_S_REC records, ~50 bytes each)
    ├── S_SES1.BIN       250 bytes  current-session summary (single record)
    ├── U.BIN         31,097 bytes  usage log
    ├── E.BIN      9,125,056 bytes  event log (high-rate events)
    ├── L.BIN      9,855,056 bytes  leak waveform
    └── R.BIN     21,060,056 bytes  flow/pressure waveform (largest — high-res data)
```

The three large files (`E.BIN`, `L.BIN`, `R.BIN`) — total ~38 MB — carry
the high-resolution waveform/event streams. Per `intellipap_loader.cpp`
comments at lines 604-607, **DV64 keeps high-res flow & pressure for only
~100 hours of use** on the SD card; older days fall back to one-per-minute
pressure (and lose flow entirely). So the size of these files reflects
recent-use buffer state, not lifetime accumulation.

### `VER.BIN` — device identity (128 bytes)

```
offset  bytes                                   meaning
0x00    00                                      leading byte / record marker
0x01    "ND16129035" + 00                       serial number (ND prefix = DeVilbiss)
0x0C    "DV64D" + 00 + padding                  model number
0x18    "V023RN20160118" + 00                   firmware version + build date YYYYMMDD
0x28    "V014BL20150630" + 00                   bootloader version + build date
0x38    01 08 02 a5 83 02 00 . 84 02            internal version codes / packed counters
0x42+   00... padding to byte 127
0x7F    a8                                      trailing byte (CRC8? checksum?)
```

Plain-text identity fields make this one of the easier formats to
fingerprint by eye. `ND` is a stable DeVilbiss serial prefix; `DV64D`
matches the loader's tested-models table exactly.

### `SET.BIN` — current settings (128 bytes)

```
offset  bytes                                   meaning
0x00    00                                      leading byte
0x01    "ND16129035" + 00                       serial number (echoed for cross-file consistency)
0x0C    04 01 00 50 00 5f 00 50 00              settings block — pressure / EPR / ramp values
0x15    0a 32 0a 32 00 00                       further settings
0x1B    f0 00 00 03 00 03 05 04 …               feature flags / scheduled values
0x7F    fb                                      trailing byte (likely CRC)
```

Loader cross-reference: `SET_BIN_REC` struct at `intellipap_loader.cpp:728`
defines the field layout this binary blob decodes into. The byte-1 echo
of the serial number is a useful integrity check — DV6 puts the serial in
nearly every file's header.

### `S.BIN` — per-day summary records

114,511 bytes / ~50 bytes per `DV6_S_REC` (`intellipap_loader.cpp:678+`) →
~2,200 day records of capacity. The struct fields cover pressure
percentiles (50/90/95th), leak percentiles, AHI components
(OA/CA/Hypopnea), mask-fit %, tidal volume, breath rate, snore index,
%-time in expiratory puff / flow limitation / periodic breathing — a rich
daily-aggregate set without needing the waveform files.

First record header begins: `00 53 4e 44 31 36 31 32 39 30 33 35 00 …` →
the byte 0 is `0x00`, then `'S'` (= `0x53`) as a record-class marker, then
the serial `ND16129035` — same pattern as the other files. The first
day-record content starts immediately after.

### `H1.BIN` / `H2.BIN` — hour-meter pair

Both 64 bytes, **byte-for-byte identical** in this sample's first 48
bytes. Header pattern:

```
00 03 01 73 6e 73 6e 73 6e 73 6e 73 6e 00 N D 1 6 1 2 9 0 3 5 00 0e be 0b 00 00 …
```

The `73 6e` ("sn" — ASCII "snsn…") repeat at offset 3 is a fixed-pattern
identifier; serial `ND16129035` follows at offset 14. The differing byte
at end (final byte = `d8`) is presumably a checksum / generation counter
update on whichever copy was most recently written. H1/H2 are
near-certainly a redundant pair (primary + backup) for the hour-meter,
matching DeVilbiss's defensive design around the most regulator-relevant
field on a CPAP (hours-of-use).

### `SC.BIN` — SmartCode log (256 bytes)

```
00 ND16129035 00
W9FA-YFJJ-1DF9-7Y5F 00 …
WX9F-W4F2-CWX4-WFLE 00 …
WK…
```

16-character dash-separated codes (looks like 4-letter quartets, base32-
ish character set: WXYZF…). These are **DeVilbiss SmartCodes** — the
encoded one-page compliance/adherence codes a clinician can read from the
device LCD or a web tool. SC.BIN logs the most recent few. Not a
fingerprinting target on its own, but a useful "yes this is DeVilbiss"
secondary signal alongside the `ND` serial prefix.

### `S_SES1.BIN` — current-session summary (250 bytes)

```
9b 57 84 27 1e e2 84 27 1e 48 02 00 00 00 00 00 …
```

Two `0x27843...` little-endian timestamps at offsets 1-4 and 5-8 (= start
and end of the active session, in the device's epoch). DeVilbiss uses a
**2002-01-01 UTC epoch** for timestamps (`intellipap_loader.cpp:288` —
`QDateTime epoch(QDate(2002, 1, 1), QTime(0, 0, 0), QTimeZone("UTC"))`),
so these decode to real wall times via that offset.

### Loader's three-tier file roles

| File class | Role | Loader function |
|---|---|---|
| VER.BIN / SET.BIN / SF.BIN | Identity & current settings | `PeekInfo` + setup at line 1377+ |
| S.BIN / S_SES1.BIN | Daily summaries + active session | line 1405+ |
| U.BIN | Cumulative usage log | line 1335+ |
| R.BIN / L.BIN / E.BIN | High-resolution waveforms & events (last ~100 hrs) | lines 1651 / 2167 / 2355 |
| H1.BIN / H2.BIN / A.BIN / F.BIN / O.BIN / SC.BIN | Auxiliary state (hours, SmartCode, alarms, options) | Various; some may not be consumed |

### Quick DV6 vs DV5 discriminators

For matching a future DeVilbiss SD card:

| Feature | DV5x (IntelliPAP) | DV6x (BlueLake / IntelliPAP 2) — this sample |
|---|---|---|
| Subdirectory | `SL/` | `DV6/` |
| Settings sentinel | `SET1` | `SET.BIN` |
| File naming | mixed | single-letter `.BIN` |
| Serial prefix | (different — not seen here) | `ND` |
| Model prefix | `DV5*` (e.g. `DV54`) | `DV64*`, `DV63*` |
| Loader path in source | `OpenDV5()` (line 91) | `OpenDV6()` (downfile) |

### Comparison across all "summary-only-ish" / quirky CPAP samples

| Aspect | AS10 CPAP basic | DS CPAP 200X110 (brick) | YH-680B | DV64D (this) |
|---|---|---|---|---|
| Loader status | Supported | Supported (brick flag) | Supported (Format D summary) | Supported (full DV6 path) |
| Waveform on card? | None | None | None | **Yes — recent ~100 hrs in R.BIN/L.BIN/E.BIN** |
| Daily summaries | STR.edf only | D/ files (tiny, 40-57 B each) | summer.bys | S.BIN (~50 B per day, ~2200 day capacity) |
| OSCAR view | Daily AHI/leak/hours, no waveform | Usage hours only | Daily AHI/leak/hours, no waveform | Full charts on recent days, low-res pressure on older |

DV6 is **not** a "summary-only" device — it just degrades to summary
resolution after ~100 cumulative hours have rolled over the high-res
buffer.

### Observations / open questions

- **The flat-files-without-wrapper layout is a copy artifact, not a
  format change.** Verified by adding a `DV6/` wrapper directory: the
  loader detected and imported normally. No detection-path change is
  warranted.
- **Recommend keeping this caveat in any future "first contact with a
  DV6 card" diagnostic** — copies-from-Mac and copies-via-some-archivers
  occasionally lose top-level directory wrappers; a quick check for
  "files at root match the expected DV6 file set" can guide the user to
  re-wrap before debugging.
- **The DV6 loader was written ~2018-2019** (per project context) and the
  high-res-data-100-hours behaviour is documented at
  `intellipap_loader.cpp:604-607`. A "pressure graph looks wrong" symptom
  on days older than that window is **expected stepped rendering**, not a
  bug — but worth confirming whether the symptom is on recent or old
  days before investigating further.
- **`A.BIN`, `F.BIN`, `O.BIN` aren't obviously referenced** by the loader
  (greps for those filenames find no matches in `intellipap_loader.cpp`).
  Possibly written by the device for its own state-tracking and ignored
  by OSCAR — low-stakes gap to investigate if a future DV6 sample shows
  data drift the loader can't explain.
- **`DV63E` ("IntelliPAP 2 AutoPlus") not in this catalogue** — would be
  useful to see whether DV63E uses an identical file set or a variant.
  The loader supports it via the same DV6 path.

### Second DV64D sample — confirms fixed-buffer architecture (flipmike)

**Sample:** `C:/Users/Guy/Downloads/DV6 flipmike/DV6`
**Device:** Also `DV64D`, serial `ND18B29041`, firmware `V031RN20181128`
(2018-11-28 build), bootloader `V018BL20180214`. Newer firmware/bootloader
vintage than Cuppa (V023 → V031), same model, same file set.

What the second sample confirms:

- **Waveform/event ring buffers are fixed-size by model**, not
  cumulative. `E.BIN` `9,125,056`, `L.BIN` `9,855,056`, `R.BIN`
  `21,060,056` — **byte-identical** to Cuppa across a different device
  and a ~2-year firmware-vintage gap. That's strong evidence these are
  pre-allocated rolling buffers sized to the ~100-hour high-res window
  the loader comments describe. Their disk footprint says nothing about
  how much of that window is actually populated.
- **Fixed-size auxiliary files also identical**: `SET.BIN` 128,
  `SF.BIN` 512, `SC.BIN` 256, `H1/H2.BIN` 64, `A/O.BIN` 56, `VER.BIN`
  128, `S_SES1.BIN` 250 — all identical bytes-of-storage across both
  samples. Model-level format, not per-firmware.

Variable-size files (genuine usage signal):

| File | Cuppa (DV64D, 2016 fw) | flipmike (DV64D, 2018 fw) | Likely interpretation |
|---|---|---|---|
| `S.BIN` | 114,511 | 99,661 | Per-day records — flipmike fewer days |
| `U.BIN` | 31,097 | 40,655 | Usage log — flipmike denser per-session events |
| `F.BIN` | 176 | 92 | Variable record array prefixed by `'F'` + serial; flipmike has ~half as many records |

**Serial-prefix decoding (provisional, two-sample inference):**
`ND<YY><…>` pattern across both samples — Cuppa `ND16129035`, flipmike
`ND18B29041`. `ND16…` = 2016 device, `ND18…` = 2018 device. Firmware
versions track manufacture vintage: V023RN (2016 device) → V031RN (2018
device). Bootloader similar (V014BL → V018BL). Not a strict
loader-supported claim — a fingerprinting heuristic worth recording.

**No behaviour differences observed** between firmware vintages at the
file-format level — the +8 firmware revisions and 2-year manufacturing
gap produced no detectable format change. DV64D output appears stable
across the V023→V031 firmware range.

---

## BMC iBreeze 20A / Resvent iBreezer / Hoffrichter Point 3

**Sample:** `C:/Users/Guy/Downloads/ibreeze 20a card data kake26`
**Device:** BMC iBreeze 20A (BiPAP ventilator), serial `GB-2B467658`, hardware `V01.11.01`.
**Loader:** `resvent_loader.cpp` — written for the **Resvent** brand (Resvent Medical), not BMC.
**Data range:** 2023-11-18 → 2023-12-12 (~25 days).

**Note on brand relationships — two intersecting family trees:**

There are two independent axes here, and the BMC iBreeze 20A sits at their intersection:

**1. The `THERAPY/CONFIG/RECORD` software platform** (horizontal axis — format, not brand):
The `THERAPY/CONFIG/RECORD` directory structure is used by multiple brands that share
the same underlying firmware platform:
- Resvent iBreezer (Resvent Medical — the loader's primary target)
- BMC iBreeze 20A (BMC Medical — this sample)
- Hoffrichter Point 2 / Point 3 (German brand)

One loader (`resvent_loader.cpp`, class name `"Resvent"`) handles all of them because
the SD card format is identical regardless of the brand label on the device.

**2. The BMC brand** (vertical axis — brand, not format):
BMC Medical produces multiple product lines, each using a *different* SD card format
handled by a *different* OSCAR loader:
- BMC Luna G3 → `bmc_loader.cpp` (`.USR` + `.idx` + `.000`–`.029`)
- BMC G3 A20 → `bmcg3x_loader.cpp` (`.idx` + `.evt` + `.000`–`.062`)
- BMC iBreeze 20A → `resvent_loader.cpp` (`THERAPY/CONFIG/RECORD` hierarchy)

The BMC iBreeze 20A is where these two trees cross: it is a BMC-branded device that
runs the `THERAPY/CONFIG/RECORD` platform shared with non-BMC brands.

### File set

```
<SD card root>/
└── THERAPY/
    ├── CONFIG/                               device configuration
    │   ├── SYSCFG                            device identity (model, serial, HW version)
    │   ├── VERSION                           format version + Unix timestamp
    │   ├── SETTING                           system settings (brightness, language, etc.)
    │   ├── CHECK.TXT                         last maintenance check date (plaintext)
    │   ├── ALARM / COMFORT / TCTRL           subsystem configs
    │   └── N_CPAP, N_APAP, H_ST, F_OSA …   mode-specific therapy configs (~30 files)
    ├── LOG/
    │   ├── 14/51577600   105 bytes           text diagnostic log fragment
    │   ├── 14/74732800
    │   ├── 16/81228800
    │   ├── 16/99977600
    │   └── 17/00236800 … 17/02396800         entries named by byte offset in log stream
    └── RECORD/
        └── 202311/                           month folder (YYYYMM)
            └── 18/                           day folder (DD, calendar day)
                ├── STAT            364 bytes aggregate daily summary (text key=value)
                ├── STAT01          ~180 bytes per-session summary (same format)
                │   …
                ├── STAT04
                ├── EV01             ~40 bytes event records (text)
                │   …
                ├── ALM01             4 bytes alarm record (binary)
                ├── W01_01        180,100 bytes waveform chunk (binary) ─┐ session 01
                ├── W01_02        180,100 bytes                          ─┘ two chunks
                ├── W02_01 … W02_08                                     session 02 (8 chunks)
                ├── W04_01 … W04_10                                     session 04 (10 chunks)
                ├── P01_01         20,188 bytes parameter chunk (binary)
                │   …
                └── P04_10
```

- Sessions are numbered `01`–NN per day; each session has separate `W` and `P` chunk files.
- Up to 10 sessions per day observed (STAT01–STAT10, EV01–EV10, …).
- Up to 14 chunk files per session observed (`W01_01`–`W01_14`).
- W file size is fixed at 180,100 bytes regardless of actual data length (zero-padded).
- This structure is completely unrelated to the BMC Legacy and BMC G3X loaders.

### Identity detection

`THERAPY/CONFIG/SYSCFG` contains the device model and serial in a text-with-checksum format:

```
<4-byte CRC> models=iBreeze 20A\n
             sn=GB-2B467658\n
             hw=V01.11.01\n
             num=2\n
```

The text lines are fixed-width padded (each key=value line is space-padded to ~32 chars before `\n`).

**Detect sentinel:** directory contains `THERAPY/CONFIG/SYSCFG` whose text content includes `models=`.
No `.USR`, `.idx`, `.evt`, or `.00x` files are present.

### File format — config, STAT, and EV files (text with CRC)

All non-binary files share the same framing:
- **Bytes 0–3:** 4-byte CRC or hash of the text content.
- **Bytes 4+:** ASCII text, key=value pairs, each line space-padded to a fixed column width.

**STAT fields (per-session summary):**

| Field | Meaning |
|---|---|
| `VentId` | Ventilation profile ID used for this session |
| `secStart` | Session start as Unix timestamp (seconds) |
| `secUsed` | Session duration in seconds |
| `secHumid` | Seconds humidifier was active |
| `timePB` | Time in periodic breathing (seconds) |
| `cntAHI` | AHI event count |
| `cntOAI` | Obstructive apnea count |
| `cntCAI` | Central apnea count |
| `cntAI` | Apnea-index event count (other) |
| `cntHI` | Hypopnea count |
| `cntRERA` | RERA count |
| `cntSNI` | Snore index count |
| `VentMode` | Ventilation mode ID |
| `cntBreath` | Total breath count |
| `cntSelfBreath` | Spontaneous (unassisted) breath count |
| `medPress` | Median pressure (cmH2O, 3 decimal places) |
| `medIPAP` | Median IPAP (cmH2O) |
| `medEPAP` | Median EPAP (cmH2O) |
| `medLEAK` | Median leak rate (L/min) |

**EV fields (event records, text, comma-delimited):**

```
ID=<type>,DT=<unix_timestamp>,DR=<duration_sec>,GD=<grade>,\n
```

Event type IDs observed: 19 (likely OAI), 20 (likely CAI).

### File format — W and P binary files

Both waveform (`W`) and parameter (`P`) files share the same binary header:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | CRC / checksum |
| `0x04` | 2 | Flags (observed `0x0000`) |
| `0x06` | 6 | Timestamp: `YY MM DD HH MM SS` (binary, year = `0x17` → 2023) |
| `0x0C` | 2 | Chunk parameter (observed `0x0060` for W, `0x0180` for P) |
| `0x0E` | 2 | Chunk parameter |
| `0x10` | 2 | Sample interval: `0x000A` = 10 Hz for W; `0x001E` = 30-second granularity for P |
| `0x12` | 2 | Channel count: `0x0002` for W, up to `0x000B` = 11 for P |
| `0x14` | 12 | Padding / reserved |
| `0x24` | 32 × N | Channel descriptors (one per channel) |
| `0x24+32N` | … | Interleaved signed int16 LE samples, one per channel per time step |

**Channel descriptor (32 bytes):**

| Offset within descriptor | Size | Field |
|---|---|---|
| `0x00` | 12 | Channel name (null-terminated, null-padded) |
| `0x0C` | 8 | Unit string (null-terminated, null-padded) |
| `0x14` | 4 | float32 LE — max range in physical units |
| `0x18` | 4 | float32 LE — min range? or other scale |
| `0x1C` | 4 | Sample count or stride |

**W file channels** (2 channels, 10 Hz):

| Channel name | Unit | Physical meaning |
|---|---|---|
| `Pressure` | `cmH2O` | Mask pressure waveform |
| `Flow` | `L/min` | Respiratory flow waveform |

**P file channels** (up to 11, 30-second granularity):

| Channel name | Unit |
|---|---|
| `Press` | `cmH2O` |
| `IPAP` | `cmH2O` |
| `EPAP` | `cmH2O` |
| `Leak` | `L/min` |
| `Vt` | `ml` |
| `MV` | `L/min` |
| `RR` | `Bpm` |
| `Ti` | `s` |
| `I:E` | (ratio) |
| `SpO2` | (%) |
| `PR` | (bpm) |

The last two channels (`SpO2` and `PR`) are part of the platform-standard channel set
declared in every P file across all three brands — even when no pulse oximeter is
attached, in which case the samples and STAT min/avg/max for them are all zero.

**W file sizes:** 180,100 bytes fixed (= 4-byte CRC + 96-byte header + 2 × 32-byte channels + 180,000 bytes of data ≈ 45 min at 10 Hz × 2 channels × 2 bytes).

**P file size:** 20,188 bytes observed.

**ALM file size:** 4 bytes (content unknown — possibly a single uint32 alarm code).

### How this differs from the BMC-specific platforms

The BMC brand spans three separate SD card formats — this entry's iBreeze 20A is
*not* in the same firmware family as either of the other two BMC product lines:

| Feature | BMC Legacy (`bmc_loader`) | BMC G3X (`bmcg3x_loader`) | iBreeze 20A (`resvent_loader`) |
|---|---|---|---|
| Card layout | `<serial>.USR` + `.idx` + `.000`–`.029` at root | `<serial>.idx` + `.evt` + `.000`–`.062` at root | `THERAPY/` subdirectory hierarchy |
| Day indexing | records inside `.USR` | `.idx` fixed-stride records | `RECORD/YYYYMM/DD/` folders |
| Session events | binary in `.USR`/`.evt` | binary `.evt` stream | text `EV{nn}` files per session |
| Waveforms | binary `.000`–`.029` (16 MiB each) | binary `.000`–`.062` (64 MiB each, circular) | binary `W{nn}_{mm}` chunks (~180 KB each) |
| Parameters / trends | n/a | summary in `.idx` records | binary `P{nn}_{mm}` chunks (~20 KB each) |
| Identity location | `.USR` binary header | `.idx` binary header | `THERAPY/CONFIG/SYSCFG` text |
| Device class | CPAP / BiPAP (sleep) | CPAP / Auto (sleep) | BiPAP ventilator (IPAP/EPAP clinical) |
| Brand-exclusive? | Yes (BMC only) | Yes (BMC only) | **No** — also Resvent, Hoffrichter |

The loader class name is `"Resvent"` and the brand is reported as `"iBreeze"`.
Confirmed working per the loader comment: `"Resvent     iBreeze 20A     works."`.

### Observations / open questions

- **`THERAPY/LOG/` file naming by byte offset** (e.g. `17/00236800` → decimal byte offset
  100,186,112) — the subdirectory `17` may be a log-file sequence number. Log files are
  plain UTF-8 text with tab-delimited lines: `YYYY/MM/DD HH:MM:SS\t<message>`.
- **`VentId` vs `VentMode`** in STAT files — `VentId` appears to be a therapy profile
  index (4 on the aggregate STAT, 1 on per-session STAT01); `VentMode` is the mode
  running within that session. The mapping to human-readable names (CPAP, APAP, ST, PC)
  is in the `CONFIG/` files (`N_CPAP`, `N_APAP`, `H_ST`, etc.).
- **CONFIG files with mode names** — `N_CPAP`, `N_APAP`, `H_ST30`, `C_PC`, `F_OSA`, etc.
  suggest a naming scheme: first char = N(ight)/H(ybrid)/C(ardiac)/F(lex)/O(ther),
  second char = therapy mode abbreviation. These files likely contain the per-mode
  pressure and timing settings.
- **Session chunking** — W and P files split into chunks with `_{mm}` suffix. With
  W files fixed at 180,100 bytes (~45 min) and sessions running 3–8 hours, a typical
  night uses 4–14 W chunks. The chunk boundary doesn't necessarily align with an hour.
- **`num=2` in SYSCFG** — meaning unknown; may be a hardware configuration count or
  firmware-protocol revision.
- **Event ID mapping** — only IDs 19 and 20 observed in this sample. A larger dataset
  is needed to map all IDs to OSCAR event types (OAI, CAI, HI, RERA, etc.).

---

## Hoffrichter Point 3 AutoCPAP — second `THERAPY/CONFIG/RECORD` sample (cross-brand)

**Sample:** `C:/Users/Guy/Downloads/netvipec_Hoffrichter-Point-3`
**Device:** Hoffrichter Point 3 AutoCPAP, serial `GB-2B411108`, hardware `V01.08.00`.
**Loader:** `resvent_loader.cpp` (same as BMC iBreeze 20A above).
**Data range:** single day, 2023-04-29 (one ~7.9-hour session, 16 W/P chunks each).

This sample is the second `THERAPY/CONFIG/RECORD` card analysed, on a **different brand**
from the BMC sample above. It confirms the platform spans Resvent / BMC / Hoffrichter,
and surfaces several cross-brand and cross-firmware variations the single iBreeze 20A
sample couldn't show.

### Top-level structure — minor differences from iBreeze 20A

```
<SD card root>/
└── THERAPY/
    ├── CFGDUP/           ← **present and empty on this sample** (absent on iBreeze 20A)
    ├── CONFIG/           ← same SYSCFG/SETTING/N_CPAP/H_ST… layout as iBreeze 20A
    ├── LOG/              ← same text diagnostic logs
    └── RECORD/202304/29/ ← single day folder
```

The `resvent_loader.cpp` comment already documents `THERAPY/CFGDUP` as *"??? ALways
empty"* — this sample confirms its presence, while the BMC iBreeze 20A sample has no
`CFGDUP` folder at all. So **CFGDUP is optional** and at minimum varies between firmware
revisions or device families on the same platform.

### Identity — `THERAPY/CONFIG/SYSCFG`

```
<4-byte CRC> models=POINT 3 AutoCPAP\n
             sn=GB-2B411108\n
             hw=V01.08.00\n
             num=2\n
```

Same key=value text format as the iBreeze 20A SYSCFG. The model string is the most
reliable brand discriminator at detect time.

### Serial-number prefix — strong OEM signature across brands

Side-by-side serial numbers on the two samples:

| Brand | Model | Serial | First six |
|---|---|---|---|
| BMC | iBreeze 20A | `GB-2B467658` | `GB-2B4` |
| Hoffrichter | Point 3 AutoCPAP | `GB-2B411108` | `GB-2B4` |

Both serials start with **`GB-2B4`** despite being different brand labels. This is the
expected fingerprint of a **shared OEM manufacturer** — the `GB-` prefix and the `2B4`
hardware-family code appear to be assigned at the factory, before the customer-facing
brand sticker goes on. The closing 6 digits (`467658` vs `411108`) are the per-unit
serial. So serial-prefix alone is a strong "is this a Resvent-platform card" indicator,
regardless of which brand printed the label.

This shouldn't be relied on as the *only* detection signal (a future brand could ship
on the same platform with a different prefix), but combined with `SYSCFG`'s `models=`
field it pins the device to platform+brand+model unambiguously.

### P-file channel set — platform-wide, not device-class-dependent

Re-reading the P-file header on this **AutoCPAP** (single-pressure) device shows the
**identical 11-channel declaration** as the iBreeze 20A **BiPAP** sample:

```
Press, IPAP, EPAP, Leak, Vt, MV, RR, Ti, I:E, SpO2, PR
```

Both `IPAP` and `EPAP` are declared even though an AutoCPAP doesn't have separate
inspiratory/expiratory levels, and `SpO2`/`PR` are declared even with no pulse oximeter
attached. This confirms the channel set is **a platform-wide schema**, not derived from
the device's capabilities. Channels not relevant to the running device contain zero or
echo the single-pressure value.

This is a useful invariant for the loader: it can hard-code the P channel layout
(11 channels at fixed 32-byte stride starting at `0x24`) instead of having to dispatch
on device model.

### STAT field set — varies between firmware versions

The Hoffrichter STAT aggregate has substantially **more fields** than the iBreeze 20A
STAT aggregate, despite being from an older calendar date (April 2023 vs Nov–Dec 2023):

```
... cnt* fields (same on both) ...
medPress, medIPAP, medEPAP, medLEAK   ← present on both samples
medVt, medMV, medRR, medTi, medIE     ← Hoffrichter only — extra median fields
minSpo2, minPR                         ← Hoffrichter only
p95Press, p95IPAP, p95EPAP, p95LEAK,
p95Vt, p95MV, p95RR, p95Ti, p95IE     ← Hoffrichter only — 95th percentile per channel
avgSpo2, avgPR                         ← Hoffrichter only
maxPress, maxIPAP, maxEPAP, maxLEAK,
maxVt, maxMV, maxRR, maxTi, maxIE,
maxSpo2, maxPR                         ← Hoffrichter only — per-channel max
```

The extra fields aren't device-class-dependent (this AutoCPAP has them; the BiPAP iBreeze
doesn't), so the variation is **firmware-version-driven**, not device-driven. The
Hoffrichter Point 3 firmware (`V01.08.00`, 2023-04) writes a richer STAT than the BMC
iBreeze 20A firmware (`V01.11.01`, 2023-12) — despite the BMC being a *higher* hardware
version, the BMC STAT carries *fewer* statistics. Counterintuitive but consistent with
OEMs picking different firmware feature sets for their re-badged products.

**Loader implication:** STAT parsing must be tolerant of missing optional fields. The
`med*` / `p95*` / `min*` / `avg*` / `max*` keys are all optional and may or may not be
present on a given card.

### `VentId` values seen so far

| Sample | Aggregate `VentId` | Per-session `VentId` |
|---|---|---|
| BMC iBreeze 20A (BiPAP) | 4 | 1 or 2 |
| Hoffrichter Point 3 (AutoCPAP) | 1 | **16** |

`VentId=16` from this AutoCPAP sample is **outside** the `RESVENT_DEVICE_MODE` enum
defined in `resvent_loader.h:155-164`, which only enumerates values 1, 3, 10–15. So
the loader's mode-mapping code doesn't currently have a case for VentId=16 — worth
checking whether the loader handles it gracefully or falls through silently. (A future
sample of the same device on different settings would clarify whether 16 is a stable
AutoCPAP marker or a firmware-specific code.)

### W file & P file binary headers — bit-for-bit identical structure

The Hoffrichter P01_01 header at offsets 0x00–0x23 matches the iBreeze 20A layout exactly:
- `0x00`: 4-byte CRC (per-file, different value)
- `0x04–0x05`: `00 00` flags
- `0x06–0x0B`: timestamp `17 04 1D 16 3A 02` → 2023-04-29 22:58:02 (same `YY MM DD HH MM SS` binary format)
- `0x12–0x13`: `0B 00` = 11 channels (matches iBreeze)
- Channel descriptors at `0x24+`, 32 bytes each, in the same order

W and P file sizes are also identical to the iBreeze 20A sample (180,100 bytes for W,
20,188 bytes for P). The platform format is binary-stable across brands.

### Cross-brand discriminator summary

When fingerprinting an unknown `THERAPY/CONFIG/RECORD` card to identify the brand and
model, work through this in order:

1. **Verify platform:** `THERAPY/CONFIG/SYSCFG` exists with text containing `models=`.
2. **Brand-and-model identification:** read the `models=` value verbatim — this is the
   only reliable in-file brand discriminator. Examples observed:
   - `models=iBreeze 20A` → BMC iBreeze 20A (BiPAP)
   - `models=POINT 3 AutoCPAP` → Hoffrichter Point 3 (AutoCPAP)
   - Future: a Resvent iBreezer sample is expected to write its own model string.
3. **Confirm OEM platform:** `sn=` should begin `GB-` (so far observed on both samples).
   This is supporting evidence, not a hard rule — future cards on the same platform may
   use different serial prefixes.
4. **Optional CFGDUP folder:** presence or absence varies by firmware/family. Don't
   rely on either case.
5. **STAT field set:** check for `p95*` / `max*` / `min*` keys to detect the richer
   firmware STAT variant — they're optional.

### Observations / open questions

- **Sample 1 was BiPAP, sample 2 is AutoCPAP** — yet both use the *same* loader code path
  and *same* channel layout. The platform's design treats both device classes uniformly,
  populating only the relevant channels at runtime.
- **`GB-2B4xxxxxx` serial prefix** appears OEM-wide. Tracking which 6-digit prefixes
  belong to this platform across future samples would let the loader make a faster
  pre-filesystem decision for ambiguous cards.
- **VentId=16 (AutoCPAP per-session)** isn't in the loader's enum — needs verification
  that the resvent loader treats it sensibly. Could be a Hoffrichter-specific extension,
  could be a generic newer/older firmware mode code.
- **CFGDUP is "always empty"** per the loader code comment but is *sometimes absent
  entirely* (iBreeze 20A had no CFGDUP folder at all). Detection should not require
  CFGDUP.
- **A Resvent-branded iBreezer sample is still missing** from this fingerprint
  catalogue. With three brands (Resvent / BMC / Hoffrichter) confirmed on the same
  platform, the matching exercise will be cleanest once we have at least one sample
  per brand.

---

## Löwenstein Prisma 20A (Prisma LINE family)

**Sample:** `C:/Users/Guy/Downloads/DJC9-Lowenstein-Prisma20A Darren Condon/Prisma`
**Device:** Löwenstein Prisma 20A, serial `30350959`, firmware `5.07` (build `2023-0310-1825-Eyra`).
**Loader:** `prisma_loader.cpp` (also supports Prisma SMART and Prisma Soft).
**Data range:** 2025-01-14 → 2025-02-06 (~24 days of trend data).

### File set

```
<SD card root>/
├── DCM/
│   └── dcm.zip                       1,143,279 bytes — manufacturer's PC decoder
│                                     bundle (.NET DLLs + locale resources)
├── System Volume Information/        Windows NTFS metadata (ignore)
├── Upload_30350959.pcloud               23,863 bytes — cloud-upload staging file,
│                                     named after the device serial (ZIP archive)
├── config.pcfg                           1,117 bytes — Prisma LINE config marker
│                                     (Prisma SMART would be config.pscfg)
├── therapy.pdat                     10,435,426 bytes — therapy data archive
└── logs/                             plain-text host-process logs
    ├── connman.log                   (network manager)
    ├── develop.log                   (development/diagnostic)
    ├── kern.log                      (Linux kernel)
    ├── pppd.log                      (point-to-point daemon — modem/dial-up)
    ├── service.log                   (service-level events)
    ├── therapy-sw.log                (therapy software, current)
    └── therapy-sw1.log               (therapy software, rotated)
```

**Key oddity:** `.pcfg`, `.pdat`, `.pcloud`, and `DCM/dcm.zip` are all **ZIP archives**
with `PK\x03\x04` magic at offset 0. The custom extensions are just rebrands; standard
ZIP tooling reads them directly.

### What `Detect()` checks vs. what's there

`prisma_loader.cpp:552` requires **only one of**:
1. `config.pscfg` at the given path (Prisma SMART family), OR
2. `config.pcfg` at the given path (Prisma LINE family — this sample).

Everything else (`DCM/`, `therapy.pdat`, `logs/`, `Upload_*.pcloud`) is invisible to
detection.

### `config.pcfg` — XML config archive

Internal structure:
```
config.pcfg (ZIP) {
  mnt/flash/conf/configuration.xml    2,141 bytes — therapy settings (DCM parameter IDs)
  mnt/flash/conf/device.xml             779 bytes — device identity + firmware versions
}
```

The Linux-style internal paths (`mnt/flash/...`) confirm the device runs Linux on a
flash filesystem mounted at `/mnt/flash`. The ZIP is effectively a snapshot of a few
files from that filesystem.

### `device.xml` — full identity (XML)

```xml
<?xml version="1.0" encoding="utf-8"?>
<?weinmann version="1.1" type="dde-tpg"?>
<DeviceID>
  <DeviceType value="10"/>
  <DeviceSerialNumber value="30350959"/>
  <MainboardSerialNumber value="240961771"/>
  <BlowerIndex value="0"/>
  <FWVersion value="5.07"/>
  <FWRevision value="0002"/>
  <FWBuild value="2023-0310-1825-Eyra"/>
  <FWGITHASH value="01fd90b"/>
  <KernelGITHASH value="f717b6b975f730b190c81ef0280e998323cf442b"/>
  <BuildrootGITHASH value="78a3ff9b40a59def49e09e369495250d40c82589"/>
  <PMVersion value="2.18.3"/>
  <StatisticVersion value="2.4.2"/>
  <NotificationVersion value="00.00.01"/>
  <LoggerVersion value="00.01.02"/>
  <MCVersion value="1fd90b.1.0"/>
  <MainboardHWVersion value="11"/>
  <DisplayHWVersion value="24"/>
  <DeviceVariant value="0"/>
  <DeviceBranding value="2"/>
</DeviceID>
```

Notable identifiers:
- **`<?weinmann version="1.1" type="dde-tpg"?>` processing instruction** — top of every
  Prisma XML file. `weinmann` (lowercase) is the firmware's internal vendor tag — the
  company's **original name**. The current consumer-facing brand is "Löwenstein"
  (Weinmann was renamed to / acquired into Löwenstein Medical). Firmware-internal tags
  retain the historical name; box labels and current marketing use "Löwenstein". The
  loader name `Prisma` reflects the product family that spans both naming eras.
- **`DeviceType=10`** — *not* in the loader's `s_PrismaTestedModels` list
  (`prisma_loader.cpp:507-513`), which currently enumerates `0x92` (Prisma Smart),
  `0x91` (Prisma Soft), `22` (prisma25S), `23` (prisma25ST). The loader's fallback
  returns "Unknown Model" — but `config.pcfg` still triggers detection, so import
  proceeds. This sample likely represents a Prisma 20A entry that should be added to
  the model table; the loader code comment at line 500 notes "was created for
  PrismaSmart, should be extended to support PrismaLines as they stabilize."
- **`FWBuild value="2023-0310-1825-Eyra"`** — `Eyra` appears to be the firmware project
  codename for this platform generation; build timestamp `2023-03-10 18:25`.
- **GIT hashes** (`FWGITHASH`, `KernelGITHASH`, `BuildrootGITHASH`) — embedded build
  provenance. Useful for forum-style issue triage ("what firmware are you on?").
- **`DeviceBranding=2`** — likely encodes Löwenstein vs Weinmann vs another regional
  brand. Worth correlating across samples.

### `configuration.xml` — settings (DCM parameters)

```xml
<DCM MODUL_VERSION="2.18.3">
<OBL>
<P id="1001" val="20180003" />
<P id="1002" val="10" />
<P id="1003" val="2" />
<P id="1005" val="1" />
... many more parameter IDs ...
</OBL>
</DCM>
```

Settings are encoded as `<P id="<numeric ID>" val="<value>" />` tuples — opaque without
the DLL decoder in `DCM/dcm.zip` (or `prisma_loader.cpp`'s parameter table).
`MODUL_VERSION` matches `PMVersion` from device.xml (both `2.18.3`).

### `therapy.pdat` — therapy data archive (105 entries)

Internal layout, grouped by purpose:

```
therapy.pdat (ZIP) {
  mnt/flash/conf/
    device.xml                          (copy of identity)
    config_notifications.ini
    config_operatingtime.ini

  mnt/flash/data/therapy/
    therapy_hash.txt                    42 bytes — integrity hash over therapy data
    events/YYYYMMDD/event_NNNNNN.xml   per-session event timeline (XML, 5–87 KB)
    signals/YYYYMMDD/signal_NNNNNN.wmedf   per-session waveform (EDF, 1 MB+)
    trendcurves/YYYYMMDD/trendCurves.tc   per-day trend (~500 bytes; 24 dates here)

  mnt/flash/data/statistics/
    statistics_year.bin                 59,224 bytes — annual rollup (binary)

  mnt/flash/data/debug/
    therapy-sw.log, kern.log, etc.      (same logs as root-level /logs but inside archive)

  mnt/flash/data/maintenance/
    service.log, mc_calibr.bin
}
```

- **Per-session triples**: `events/<date>/event_NNNNNN.xml` + `signals/<date>/signal_NNNNNN.wmedf`
  share a session number (`NNNNNN`). Sessions are numbered globally (not reset per
  day) — this sample shows `event_000015.xml` through `event_000044.xml` (30
  sessions over 24 days, ~1.25/day average; matches one bedtime session plus occasional
  daytime/nap segments).
- **Date directories use `YYYYMMDD`** (8-digit, no separators). One session can span
  midnight, so a session file may live in the "wrong" day's folder relative to its
  internal timestamps.
- **Older sessions are pruned**: this card shows session IDs starting at `000015`,
  so the device had recorded at least 14 prior sessions that no longer live on the
  card. The pruning is by session count, not by date.

### `signal_*.wmedf` — Weinmann-flavoured EDF

The first 256 bytes of `signal_000015.wmedf` are a **standard EDF header**:

```
0  "1       Patient Name                                       "
                                                          ↑ patient ID field
80 "Recording start at Mon, 20.01.2025 23:04:28          ..."
```

This is the **EDF specification** (same standard ResMed uses), with `.wmedf` extension
to distinguish it from generic EDF. The "Recording start at Mon, 20.01.2025 23:04:28"
string is human-readable German-style date format inside the EDF recording-ident field.

Compared with ResMed's per-session EDFs:
- ResMed splits each session into 4 EDFs (BRP, PLD, SAD, EVE).
- Prisma writes **one EDF + one XML event file** per session. Event metadata is in XML,
  not in the EDF event channel.

### `event_*.xml` — XML event timeline

Top of `event_000015.xml`:

```xml
<?xml version="1.0" encoding="utf-8"?>
<desc>
  <DeviceEvent DeviceEventID="0" Time="0" ParameterID="1003" NewValue="2"/>
  <DeviceEvent DeviceEventID="0" Time="0" ParameterID="1001" NewValue="20180003"/>
  ... (every settings change and respiratory event during the session)
</desc>
```

`ParameterID` cross-references the same IDs used in `configuration.xml` (`1001`, `1002`,
`1003`, ...). At `Time=0` (session start) the events restate the current settings; later
events carry non-zero `Time` values (milliseconds or seconds since session start — needs
verification).

### `Upload_30350959.pcloud` — cloud-upload staging file

23,863 bytes, ZIP archive named after the device serial (`30350959` matches
`DeviceSerialNumber` in device.xml). This is the file the device would push to
Löwenstein's cloud service when network connectivity is available. Not parsed by OSCAR.

### `DCM/dcm.zip` — manufacturer's Windows decoder DLLs

```
MCC.P3ParameterSet.dll    1,114,624 bytes   .NET assembly — Parameter ID → name/units map
MCC.Plugin.dll                9,216 bytes   plugin host
MCC.Plugin.V2.dll             6,144 bytes   plugin host v2
MCC.Plugin.V3.dll             5,632 bytes   plugin host v3
Strings_de.xaml               3,839 bytes   German UI strings
Strings_en.xaml               3,824 bytes   English UI strings
```

These are **Löwenstein's own Windows-side decoder DLLs**, shipped on the SD card so
that Löwenstein's PrismaTS PC application can interpret the data even on a system that
doesn't have PrismaTS pre-installed. `MCC` = "Modular Configuration Component";
`P3ParameterSet` = "Prisma 3 Platform Parameter Set" (presumably the 3rd-gen Prisma
data model).

**For OSCAR's purposes these DLLs are useful as a static reference for parameter ID
decoding** — extracting the ID→name/units mapping from `MCC.P3ParameterSet.dll` (via a
.NET reflection tool like ILSpy) is the most accurate way to label new `ParameterID`
values not yet in `prisma_loader.cpp`. The DLLs themselves aren't loaded or executed by
OSCAR.

### Root `logs/` directory — duplicates from inside therapy.pdat

The plain-text logs at `<root>/logs/` (`connman.log`, `develop.log`, `kern.log`,
`pppd.log`, `service.log`, `therapy-sw.log`, `therapy-sw1.log`) are byte-identical to
the `mnt/flash/data/debug/*` entries inside `therapy.pdat`. The device writes them
twice — once into the live ZIP archive for staged upload, and once unpacked on the
SD card root for direct inspection (e.g. plugging the card into a Windows PC).

OSCAR can ignore the root `logs/` directory entirely; everything it needs is inside
`therapy.pdat`.

### How Prisma differs from every other format catalogued

| Feature | Prisma LINE (this sample) | All others above |
|---|---|---|
| Storage container | ZIP archives with renamed extensions (`.pcfg`, `.pdat`, `.pcloud`) | Plain files or hierarchical folders |
| OS on device | Embedded Linux (paths `mnt/flash/…`) | Microcontroller / RTOS |
| Identity format | XML (`device.xml` with versioned `<DeviceID>` schema) | Text key=value or binary headers |
| Settings format | XML (`<P id="N" val="V"/>`) | Text key=value or binary tables |
| Event format | XML per session (one file per session) | Binary streams or text records |
| Waveform format | **Standard EDF** (with custom `.wmedf` extension) | Proprietary binary (BMC, Resvent, PRS1) or standard EDF (ResMed) |
| Cloud upload | Built-in (`Upload_<serial>.pcloud` staging file, modem via `pppd`) | Mostly post-hoc (uploaded from PC software) |
| PC decoder shipped on-card | Yes (`DCM/dcm.zip` with .NET DLLs) | No |
| Bundled debug logs | Yes (`logs/` and `mnt/flash/data/debug/`) | Rare |

**The shipped-with-decoder pattern is unique to Prisma in OSCAR's catalogue so far.**
Most other devices treat the SD card as private storage; Prisma treats it as a
self-contained, externally-readable bundle complete with its own decoding tools.

### Observations / open questions

- **`DeviceType=10` not in `s_PrismaTestedModels`** — this Prisma 20A would import via
  the generic LINE path but report "Unknown Model". Candidate for the tested-models
  table once end-to-end loader behaviour against this device is verified
  (see "Prisma model candidates pending verification" below).
- **`Eyra` firmware codename** — internal project name for the 20A's platform. Worth
  watching for the same codename on related Prisma LINE models (20C, 25, etc.) to
  confirm a shared firmware lineage.
- **Brand vs vendor tag divergence**: device says `<?weinmann ?>` internally (original
  company name) but is sold as Löwenstein on the box (current name post-rename /
  acquisition). The loader name `Prisma` papers over both. Older Weinmann-branded
  hardware in the field (e.g. Weinmann Somnobalance, Somnocomfort) is the same firmware
  lineage and should not be treated as a separate vendor.
- **`DeviceBranding=2`** — almost certainly encodes which OEM label the device shipped
  under. Worth decoding once we have samples under different brand labels.
- **The duplicate `logs/` at SD root** is wasted space (~3 MB) for OSCAR's purposes
  but is convenient for end users who can read logs without ZIP tools. Don't try to
  reconcile the two copies — they're meant to be identical.
- **Session ID `000015` start** — at least 14 earlier sessions were pruned off this
  card. PrismaTS (manufacturer software) presumably uploads-then-deletes; the
  Upload_*.pcloud staging file may indicate which sessions have already been
  transferred to the cloud and can be safely pruned. Open question: does OSCAR need
  to coordinate with the upload state, or does the device guarantee that pruned
  sessions are never relevant again?
- **`MCC.P3ParameterSet.dll` is a goldmine** if parameter ID coverage is incomplete —
  it's a .NET assembly that can be statically inspected (no execution) to extract
  the full ID→name/units/range mapping the device firmware uses internally.

---

## Löwenstein Prisma 25ST — second Prisma LINE sample (cross-model invariance)

**Sample:** `C:/Users/Guy/Downloads/chamomile6-Prisma-25ST Deepak Vinod/Prisma`
**Device:** Löwenstein Prisma 25ST, serial `30325987`, firmware `5.07` (build `2023-0310-1825-Eyra`).
**Loader:** `prisma_loader.cpp` — `DeviceType=23` IS in `s_PrismaTestedModels` (as `prisma25ST`).
**Data range:** 2025-01-11 → 2025-02-04 (~24 days).

This second Prisma LINE sample answers most of the open questions the 20A sample left.

### Top-level structure — same skeleton, two omissions

```
<SD card root>/
├── DCM/                        same MCC.P3ParameterSet.dll decoder bundle
├── config.pcfg                 1,122 bytes (vs 1,117 on 20A — near-identical)
├── therapy.pdat               11,867,768 bytes (~13% larger than 20A — see below)
└── logs/                       same plain-text host-process logs
```

**Missing** compared to the 20A sample:
- No `System Volume Information/` — this card was never mounted on Windows.
- No `Upload_<serial>.pcloud` staging file — cloud upload either disabled or no pending
  upload at the time of card extraction.

Both omissions are environmental (mount history / cloud setup) and have no bearing on
detection or import. **The card is still a valid Prisma LINE card** — `config.pcfg` is
the only detection sentinel, and it's present.

### Headline finding: 20A and 25ST run *identical firmware*

Comparing `device.xml` across the two LINE samples:

| Field | Prisma 20A | Prisma 25ST | Identical? |
|---|---|---|---|
| `<?weinmann ?>` PI | `version="1.1" type="dde-tpg"` | same | ✓ |
| `DeviceType` | **`10`** | **`23`** | ✗ |
| `DeviceSerialNumber` | `30350959` | `30325987` | ✗ (per-unit) |
| `MainboardSerialNumber` | `240961771` | `240311936` | ✗ (per-unit) |
| `BlowerIndex` | `0` | `0` | ✓ |
| `FWVersion` | `5.07` | `5.07` | ✓ |
| `FWRevision` | `0002` | `0002` | ✓ |
| `FWBuild` | `2023-0310-1825-Eyra` | `2023-0310-1825-Eyra` | ✓ |
| `FWGITHASH` | `01fd90b` | `01fd90b` | ✓ |
| `KernelGITHASH` | `f717b6b…` (40 chars) | `f717b6b…` (same 40 chars) | ✓ |
| `BuildrootGITHASH` | `78a3ff9b…` | `78a3ff9b…` | ✓ |
| `PMVersion` | `2.18.3` | `2.18.3` | ✓ |
| `StatisticVersion` | `2.4.2` | `2.4.2` | ✓ |
| `NotificationVersion` | `00.00.01` | `00.00.01` | ✓ |
| `LoggerVersion` | `00.01.02` | `00.01.02` | ✓ |
| `MCVersion` | `1fd90b.1.0` | `1fd90b.1.0` | ✓ |
| `MainboardHWVersion` | `11` | `11` | ✓ |
| `DisplayHWVersion` | `24` | `24` | ✓ |
| `DeviceVariant` | `0` | `0` | ✓ |
| `DeviceBranding` | `2` | `2` | ✓ |

**Every version field is byte-identical.** Same firmware git hash, same kernel git hash,
same buildroot git hash. So the **Eyra** firmware project is a single binary that
runs across the entire Prisma LINE family — 20A, 25ST, and presumably the others
(20C, 25S, 30ST, 40ST, etc.). The device differentiates models via the firmware-internal
`DeviceType` field, not by shipping different builds.

This is a major loader-side simplification: **one code path is correct for all LINE
models**. The `s_PrismaTestedModels` table only needs to learn the DeviceType → display
name mapping per model; the rest of the parsing logic is unified.

`DeviceBranding=2` being identical across two different models is also confirmation
that **`DeviceBranding` encodes the OEM label, not the model**. Both are Löwenstein-branded
(branding=2). A Weinmann-branded sample of the same hardware would presumably have a
different `DeviceBranding` value.

### `DeviceType=23` IS in the loader's tested list

`prisma_loader.cpp:511` already maps `"23"` to `"prisma25ST"`. So this sample identifies
correctly out of the box, while the 20A sample (`DeviceType=10`) falls through to the
"Unknown Model" fallback. The 20A is a candidate for the tested-models table once
end-to-end loader behaviour has been verified against an actual 20A card.

### Data volume scales with device capability

| Measurement | Prisma 20A (AutoCPAP) | Prisma 25ST (Bilevel ST) | Ratio |
|---|---|---|---|
| `therapy.pdat` total | 10.4 MB | 11.9 MB | 1.14× |
| `statistics_year.bin` | 59,224 bytes | 86,088 bytes | 1.45× |
| Per-day `trendCurves.tc` size | ~400–600 bytes | ~3,000–6,000 bytes | ~10× |
| Per-day event_*.xml count | ~1.25 | ~8.6 | 7× |
| Sessions in 24 days | 30 (IDs 15→44) | 206 (IDs 65→270) | 7× |
| Per-session waveform (`.wmedf`) | ~1 MB | (matches event count, varies) | ~7× more files |

The 25ST's much higher session count per day is the most striking difference: **206
sessions in 24 days = 8.6/day** versus the 20A's **30 sessions in 24 days = 1.25/day**.
Two plausible explanations:

1. The bilevel ST device segments therapy more finely. Mask removal, mode changes
   (S→T transitions), or pressure-adjustment events may end one session and start
   another, where the AutoCPAP would just log them as in-session events.
2. The user's usage pattern (frequent mask off/on, daytime naps, periodic ventilation
   needs) — this is a respiratory-support device, not a sleep-only CPAP, so
   intermittent daytime use is expected.

Without per-session duration data extracted from the `.wmedf` files (their EDF headers
include duration), it's not possible to say which contributes more. Worth checking once
OSCAR successfully imports both samples.

The **trend-curve size scaling** (10× larger per day on the bilevel) reflects more
channels per data point — single pressure for the 20A vs IPAP + EPAP + timing channels
for the 25ST. Same logic as ResMed's STR.edf channel count being ~50% larger on
bilevel than CPAP (per the AS10 entries above).

### Per-session counter pruning is by count, not date

Session ID range on this card: `000065` → `000270`. So **at least 64 earlier sessions
have been pruned** off the card — the device kept the most recent ~206 sessions and
discarded the rest. The 20A sample showed the same pattern (`000015` → `000044`,
14 prior pruned). Pruning policy is **session-count-based**, not date-based.

This matters for OSCAR: a user importing a Prisma card may have already lost older
sessions that were on the card before their last PrismaTS upload. The cloud upload
(`Upload_*.pcloud`) likely captures sessions before they're pruned — so a complete
record exists only by combining card data with cloud-uploaded history, which OSCAR
doesn't pull.

### Loader gap closure required

Open questions from the 20A entry, now answered or refined:

| 20A entry question | Status after 25ST sample |
|---|---|
| `DeviceType=10` not in `s_PrismaTestedModels` | Still open — file says it's a Prisma 20A. Candidate pending end-to-end loader verification. |
| `Eyra` firmware codename — shared across models? | **Confirmed shared** — 20A and 25ST run byte-identical firmware. |
| `DeviceBranding=2` decoding | **Refined** — same value on two different models from the same OEM (Löwenstein), so it encodes brand not model. A Weinmann-branded sample would clarify the mapping. |
| Does OSCAR need to coordinate with cloud upload state? | Still open. This sample has no `Upload_*.pcloud` staged at all — either no pending upload, or cloud upload was disabled. Cloud-pruning interaction needs a Prisma user who uses both PrismaTS and OSCAR to confirm. |
| Are `event_*.xml` ID values the same across models? | Likely yes (same firmware) — same `ParameterID` namespace. Worth spot-checking a few against the 25ST event files. |

### Observations / open questions

- **Same FWBuild across both Prisma LINE samples** means a future Prisma 20C, 30ST,
  or 40ST on the same firmware era will look the same to OSCAR at the loader level —
  only the `DeviceType` integer changes. Maintaining the model lookup table is the
  only ongoing per-model loader work.
- **`Eyra` codename remains undecoded** — could be the platform name, a release-train
  name, or both. Worth noting if older Prisma LINE samples carry a different codename
  (which would indicate a generational firmware boundary).
- **Per-session pruning rate observed across two samples:** 20A keeps ~30 sessions
  before pruning; 25ST keeps ~206. The pruning threshold may scale with session size
  (smaller sessions allow more retention) or with available flash space. Two samples
  isn't enough to pin down the policy.
- **High session-segmentation rate on the 25ST** — 8.6/day average. If this generalises
  to other bilevel Prisma devices, OSCAR's per-day session grouping logic will be
  exercised more heavily on Prisma cards than on CPAP cards.

---

## Löwenstein Prisma VENT V50-C (not currently recognized by OSCAR)

**Sample:** `C:/Users/Guy/Downloads/AKLERK Lowenstein Prisma Vent V50C Arie Klerk/.../WM30620/WM30620`
**Device:** Löwenstein Prisma VENT **V50-C** (life-support ventilator), serial `34019414`, Weinmann article `WM30620`, firmware `3.9.0011` (branch `release-P34A11.3-3.09`).
**Loader:** **None — prisma_loader doesn't match this format.** Detection sentinel is different (`prismaVENT.sdpvdat`, not `config.pcfg`/`config.pscfg`).
**Data range:** session-numbered `0002` (2023-03-22) through `0024` (2023-09-14), 21 numbered days with gaps.

### Card-root structure — distinct from Prisma LINE

```
<SD card root>/                       (Weinmann part-number-named: WM30620)
├── WM30620/                          duplicate nesting in source dump (one WM dir
│                                     per article number could appear here on a
│                                     service-tech card carrying multiple machines)
│   ├── SN<serial>/                   one folder per device, prefixed "SN"
│   │   ├── 0005_2023-06-22.zip       per-day data ZIP — name = day# + date
│   │   ├── 0006_2023-06-23.zip
│   │   │   …
│   │   ├── 0024_2023-09-14.zip
│   │   ├── battery/                  per-session battery .wmedf files
│   │   ├── trendcurve/               per-day .tc trend files
│   │   ├── logs/                     plain-text host-process logs
│   │   ├── configuration.json        JSON therapy/alarm config (NOT XML)
│   │   ├── device.xml                XML identity with NEW schema (Device_settings)
│   │   └── prismaVENT.sdpvdat        **0-byte presence marker — likely the sentinel**
│   └── plugin/
│       └── 100859904/                directory named for the DCM version integer
│           └── dcm.zip               manufacturer's .NET decoder bundle
└── WM30620.rar                       compressed copy of the WM30620 tree
                                      (user's archival artefact, not on-card)
```

The outer `WM30620/WM30620` double-nesting is **how the dump was packaged**, not how
the card was laid out — assume one `WM<article-number>/` level on the actual card.
`WM30620.rar` is the dumper's own backup archive.

### What a `Detect()` would have to check

The existing `prisma_loader.cpp:552` checks for `config.pcfg` or `config.pscfg` at the
given path. Neither file exists on this card — hence "not recognized."

Reasonable sentinels for a Prisma VENT detector, in order of specificity:
1. **`SN<digits>/prismaVENT.sdpvdat`** (0-byte presence marker — the most distinctive).
2. `SN<digits>/configuration.json` + `SN<digits>/device.xml` together (both must exist).
3. `WM<digits>/` directory naming at root (article-number folder).

Detection probably needs to walk one or two levels into the card tree, since the
sentinel files live under `WM<n>/SN<n>/`, not at the SD root.

### `device.xml` — new schema (`Device_settings`)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Device_settings>
  <DCM_version>6.3.0</DCM_version>
  <Device_battery_installed>1</Device_battery_installed>
  <Device_battery_installed_str>Yes</Device_battery_installed_str>
  <Device_battery_support>1</Device_battery_support>
  <Device_battery_support_str>Yes</Device_battery_support_str>
  <Device_branding>1</Device_branding>
  <Device_branding_str>Loewenstein medical</Device_branding_str>
  <Device_fw>3.9.0011</Device_fw>
  <Device_fw_branch>release-P34A11.3-3.09</Device_fw_branch>
  <Device_sn>34019414</Device_sn>
  <Device_type>5</Device_type>
  <Device_type_str>V50-C</Device_type_str>
  <Mainboard_sn>230110174</Mainboard_sn>
  <WM_number>30620</WM_number>
</Device_settings>
```

Key differences from the Prisma LINE `<DeviceID>` schema:

| Aspect | Prisma LINE (20A / 25ST) | Prisma VENT V50-C (this) |
|---|---|---|
| Root element | `<DeviceID>` | `<Device_settings>` |
| Element naming | `<DeviceType value="…"/>` (attribute) | `<Device_type>5</Device_type>` (text content) |
| Self-documenting strings | None | `*_str` companions (e.g. `Device_type_str = "V50-C"`) |
| Vendor PI | `<?weinmann version="1.1" type="dde-tpg"?>` | **None** (no PI line) |
| FW git hashes | Yes (`FWGITHASH`, `KernelGITHASH`, `BuildrootGITHASH`) | None (firmware version string only) |
| Firmware project codename | `Eyra` in `FWBuild` | `P34A11.3` in `Device_fw_branch` |
| DCM version | `2.18.3` | **`6.3.0`** (much higher) |
| Branding integer for Löwenstein | `2` (unconfirmed — no `_str`) | `1` (explicitly "Loewenstein medical") |

**The branding integer is *not* stable across firmware platforms** — LINE writes
`DeviceBranding=2`, VENT writes `Device_branding=1`, both for Löwenstein-branded
devices. Don't compare branding integers across schemas.

The VENT schema is later/cleaner — every numeric field has a `_str` companion. This
is a different firmware codebase, not just a different model on the same code.

### `configuration.json` — JSON config with 3-profile arrays

```json
{
"configuration": {
  "version": 100859904,
  "checksum": 418986550,
  "therapy": {
    "AirTrapControl":          [0, 0, 0],
    "AlarmApnoe":              [0, 0, 0],
    "AlarmLeakageHigh":        [70, 0, 0],
    "AlarmMinuteVolumeHigh":   [0, 0, 0],
    "AlarmFrequencyHigh":      [0, 0, 0],
    "AutoEPAPmin":             [600, 600, 600],
    "AutoEPAPmax":             [1200, 1200, 1200],
    "AutoEPAPdeltaPinsp":      [600, 600, 600],
    "CPAP":                    [600, 600, 600],
    "EPAP":                    [400, 400, 400],
    "Frequency":               [60, 140, 140],
    "HFT_Flow":                [40, 40, 40],
    "HoseCompliance":          [0, 430, 430],
    "HoseResistanceP1Flow":    [0, 0, 0],
    "HoseResistanceP2Flow":    [3672, 1673, 1673],
    "HoseResistanceP3Flow":    [5538, 4915, 4915],
    …
    "HoseResistanceP8Pressure":[3333, 4050, 4050]
  }
}}
```

Every parameter is a **3-element array** — the device stores **three independent
therapy profiles** that a clinician can configure and the patient can switch between.
This is standard practice for non-invasive ventilators where day/night/transport
profiles are pre-set.

The `version` integer (`100859904`) matches the `plugin/100859904/` directory name
— the DCM parameter dictionary version is stored as a directory.

Notable parameters tell us this is a **proper clinical ventilator**, not a sleep CPAP:

| Parameter | What it tells us |
|---|---|
| `AlarmApnoe`, `AlarmFrequencyHigh/Low` | Life-safety alarms — patient breathing-rate monitoring |
| `AlarmLeakageHigh`, `AlarmMinuteVolumeHigh/Low` | Mask-fit and ventilation-volume alarms |
| `AlarmPulseHigh/Low`, `AlarmSpO2High/Low` | Pulse-oximetry integration (likely external SpO2 module) |
| `AutoEPAPmin/max`, `AutoEPAPdeltaPinsp` | Auto-EPAP with delta-Pinsp coupling (advanced bilevel) |
| `HFT_Flow` | **High-Flow Therapy** support (HFNC = high-flow nasal cannula) |
| 8-point `HoseResistanceP{1-8}Flow/Pressure` calibration | Multi-point hose calibration; supports pediatric / specialty circuits |
| `Frequency` 60–140 in profile arrays | Backup respiratory rate (units appear to be ×0.1 bpm → 6.0–14.0 bpm) |
| `EPAP=400`, `CPAP=600`, `AutoEPAPmin=600` | Pressures in ×0.1 cmH2O units (40, 60, 60 cmH2O… or more likely ×0.01 hPa → 4, 6 cmH2O) — needs unit confirmation |

### Per-day data ZIPs — `NNNN_YYYY-MM-DD.zip`

Each daily ZIP holds the day's sessions as paired EDF + XML:

```
0005_2023-06-22.zip {
  0001.wmedf  + event_0001.xml    session 1 of day 5
  0002.wmedf  + event_0002.xml    session 2 of day 5
  …
  0019.wmedf  + event_0019.xml    session 19 of day 5
}
```

- The leading `0005` is the **day number** (consecutive integer counter, not the date).
  Day numbers in this sample: 0005, 0006, 0007 … 0024 (range 5–24 → 20 sequential
  day numbers with gaps where the device wasn't used — no 0017).
- Inside each ZIP, sessions are numbered `0001.wmedf`, `0002.wmedf`, … re-starting
  from 1 each day.
- Day 5 (2023-06-22) shows **19 sessions** — much higher than the consumer 25ST's
  ~8/day. Consistent with a ventilator that creates a new session on every device-on /
  parameter-change / alarm-resolution event throughout the day.
- File sizes vary widely (380 bytes for tiny event XMLs up to 1.8 MB for long
  `.wmedf` waveforms) — sessions are very short to multi-hour, mixed.

### `trendcurve/` and `battery/` — sibling per-day data

- **`trendcurve/NNNN_YYYY-MM-DD.tc`** — one file per recorded day, same numbering
  scheme as the daily ZIPs. The Prisma LINE put these inside `therapy.pdat`; the
  VENT splits them out to a dedicated folder.
- **`battery/NNNN_NNNN_YYYY-MM-DD.wmedf`** — per-battery-session EDF files,
  filename pattern `<day#>_<session#>_<date>.wmedf`. Recorded when the unit ran
  on battery. The V50-C supports portable/transport use, so battery sessions are
  a routine data class.

### `prismaVENT.sdpvdat` — empty sentinel

0 bytes. Just a presence marker — same pattern as Yuwell Format D's `RunLog.bys`
(also empty). The `.sdpvdat` extension stands for "SD prismaVENT data" / "Prisma
VENT data file" and is the cleanest signal that this is a VENT-family card. **A
future loader's Detect() should anchor on this filename.**

### `plugin/100859904/dcm.zip` — DCM decoder, indexed by version

`100859904` is the configuration's `version` integer; it's used as the directory
name so multiple DCM versions can coexist on one card (e.g. after a firmware
update). Inside `dcm.zip` is presumably the same `MCC.*.dll` + `Strings_*.xaml`
bundle as the LINE format, but for the VENT-side parameter dictionary (note
`DCM_version 6.3.0` here vs LINE's `2.18.3` — different DCM generation).

### `logs/` — filenames distinguish VENT from LINE

| LINE logs | VENT logs |
|---|---|
| connman.log | **connmanSw.log** |
| develop.log | (absent) |
| kern.log | kern.log |
| pppd.log | (absent — no modem on V50-C) |
| service.log | **serviceSw.log** |
| therapy-sw.log | **therapy.log** |
| therapy-sw1.log | (absent) |
| (absent) | **assert.log** |
| (absent) | **controlSw.log** |
| (absent) | **icc-diver.log** |
| (absent) | **loggerSw.log** |
| (absent) | **mc.log** |
| (absent) | **net.log** |

The `*Sw.log` ("Software") suffix and the additional `assert.log`/`controlSw.log`
imply a **safety-critical software architecture** with stricter component separation.
LINE-family devices share some log filenames (kern.log) but the rest of the
logger is reorganised.

### How VENT V50-C differs from Prisma LINE

| Aspect | Prisma LINE (20A / 25ST) | Prisma VENT V50-C |
|---|---|---|
| Detect sentinel at root | `config.pcfg` or `config.pscfg` | **`SN<n>/prismaVENT.sdpvdat`** (one level down) |
| Path depth to data | Files at root | Inside `WM<article>/SN<serial>/` |
| Data container | One large `therapy.pdat` ZIP | **One ZIP per day** (`NNNN_YYYY-MM-DD.zip`) |
| Therapy session location | `mnt/flash/data/therapy/signals/<date>/signal_NNNNNN.wmedf` inside `therapy.pdat` | `NNNN.wmedf` inside the day's ZIP |
| Event-record location | XML inside `therapy.pdat` events folder | XML inside the same per-day ZIP |
| Trend-curve location | Inside `therapy.pdat` | Separate top-level `trendcurve/` folder |
| Config format | XML (`<DCM><OBL><P id=N val=V/></OBL></DCM>`) | **JSON** (3-profile arrays) |
| Identity schema | `<DeviceID>` element, attribute-based | `<Device_settings>` element with `_str` companions |
| Firmware project | **Eyra** (`P3` platform — Prisma 3) | **P34A11** (different platform / generation) |
| DCM version | `2.18.3` | **`6.3.0`** |
| Cloud upload staging | `Upload_<sn>.pcloud` ZIP at root | Not observed on this card |
| Device class | Sleep CPAP / Auto / Bilevel | **Clinical home ventilator** (HFT, multi-profile, life-safety alarms) |
| Loader status | Supported (model name lookup needs `DeviceType=10` for 20A) | **Unsupported — needs new detector + parser** |

### What it would take to support this in OSCAR

1. **New Detect path**: walk into `WM<digits>/SN<digits>/` and check for
   `prismaVENT.sdpvdat`. Could live in `prisma_loader.cpp` as a sibling detector
   (the way `bmcg3x_loader` is sibling to `bmc_loader`), or in a new
   `prismavent_loader.cpp` if the parsing diverges enough.
2. **JSON config parser** (instead of the existing XML config parser). Qt's
   `QJsonDocument`/`QJsonObject` is fine here — no extra dependencies needed.
3. **Per-day ZIP iterator**: walk `NNNN_YYYY-MM-DD.zip` files, extract each
   session pair `NNNN.wmedf` + `event_NNNN.xml`, parse together.
4. **Different parameter ID namespace**: VENT's `configuration.json` uses
   **string parameter names** (`AlarmApnoe`, `HoseResistanceP1Flow`) instead of
   LINE's **numeric IDs** (`P id="1001"`). The mapping table is in
   `plugin/<dcm-version>/dcm.zip`. OSCAR needs either a hand-curated subset
   (the few channels users care about) or a tool to extract the table from the
   .NET DLL once.
5. **Channel set**: ventilator channels are a superset of CPAP/Bilevel — RR /
   tidal volume / minute volume / SpO2 / pulse / alarm states. Existing OSCAR
   channels cover most of this; HFT-specific channels (`HFT_Flow`) may need
   new ChannelIDs.
6. **Multi-profile awareness**: the device has 3 stored profiles; the active
   profile during each session needs to be recorded as session metadata so the
   user can tell which profile produced what data.

### Observations / open questions

- **`Device_type=5` → "V50-C"** is the model lookup for this generation. The
  `_str` companion fields make a tested-models table unnecessary if the loader
  trusts the device's self-reporting (which is safer than maintaining a static
  list).
- **`Device_branding=1` "Loewenstein medical"** with an explicit `_str` confirms
  the integer-to-brand mapping for this firmware platform — but only for this
  platform. LINE's `DeviceBranding=2` cannot be assumed to mean the same brand
  via a different integer.
- **`P34A11` firmware platform name** is distinct from the LINE family's
  `Eyra` codename. This is a different software platform built for life-support
  use (different certification path, different safety architecture, hence the
  separate logger components and assertion log).
- **The 0-byte `prismaVENT.sdpvdat` file** is a clear platform marker. A future
  Prisma VENT model (V40, V60, etc.) would likely use the same filename for the
  same purpose — the file is the platform's "this is a VENT-class card" signature.
- **Three nested folder levels deep** (`WM<article>/SN<serial>/<data>`) is
  uncomfortably deep for a Detect() shortcut. The loader will either need to
  walk a few levels by convention or anchor purely on file content (e.g.
  recursive scan for `prismaVENT.sdpvdat`).
- **Day-number-only filenames** (`0005_2023-06-22.zip`, `0001.wmedf` inside) mean
  session ordering across days requires combining the day number with the
  internal session number — there's no single global session counter like
  LINE's `event_000270.xml`.
- **HFT (High-Flow Therapy)** support in the config means this device covers a
  therapy class OSCAR has never modelled. If real-world V50-C users want HFT
  charts, OSCAR needs new ChannelIDs for HFT flow rate, FiO2 (if measured),
  etc. — separate from CPAP pressure channels.
- **`Mainboard_sn 230110174` and `Device_sn 34019414`** are independent serial
  numbers. The mainboard SN is useful for hardware-level diagnostics; the
  device SN is the user-facing serial. Both should be retained in OSCAR's
  machine record.

---

## Löwenstein Prisma 25S — third Prisma LINE sample (older Eyra firmware)

**Sample:** `C:/Users/Guy/Downloads/tolnaiz - tolnaiz-221009-prismaline-25S`
**Device:** Löwenstein Prisma 25S, serial `30151177`, firmware `5.05` (build `2021-0730-1454-Eyra`).
**Loader:** `prisma_loader.cpp` — `DeviceType=22` IS in `s_PrismaTestedModels` (as `prisma25S`).
**Data range:** starts 2021-12-22 (and continues — full range not surveyed here).

This sample's main contributions to the catalogue are (1) confirmation that the **Eyra**
firmware codename is a long-running platform name spanning multiple years of releases,
and (2) a minimal Prisma LINE SD card layout — no decoder bundle, no logs, no upload
staging.

### Top-level structure — minimal LINE card

```
<SD card root>/
├── __MACOSX/             macOS resource-fork metadata (artefact of how the dump
│                         was packaged; not on-card)
├── config.pcfg            1,131 bytes — Prisma LINE config marker
└── therapy.pdat          10,453,358 bytes — therapy data archive
```

**Both auxiliary folders absent**:
- No `DCM/` (the manufacturer's .NET decoder bundle).
- No `logs/` (root-level plain-text log copies).
- No `Upload_<sn>.pcloud` staging file.

The card still detects and imports — `prisma_loader.cpp:Detect()` only needs
`config.pcfg`. The auxiliary folders on the 20A sample were **device options**, not
required artefacts. A real-world card may carry none, some, or all of them
depending on firmware build, cloud-service enrollment, and whether the device's
PC-side tooling has ever rewritten the card.

### `device.xml` — confirms older Eyra firmware

```xml
<DeviceType value="22"/>                          ← 22 = prisma25S (in loader table)
<DeviceSerialNumber value="30151177"/>
<MainboardSerialNumber value="211160948"/>
<FWVersion value="5.05"/>                         ← older than 20A/25ST (5.07)
<FWRevision value="0017"/>
<FWBuild value="2021-0730-1454-Eyra"/>            ← built 2021-07-30, same Eyra codename
<FWGITHASH value="2960d5f"/>                      ← different commit from 5.07
<PMVersion value="2.17.8"/>                       ← older DCM (vs 2.18.3 on 5.07)
<StatisticVersion value="2.4.1"/>                 ← older (vs 2.4.2)
<MainboardHWVersion value="11"/>                  ← same mainboard family as 20A/25ST
<DisplayHWVersion value="28"/>                    ← different display from 20A/25ST (24)
<DeviceBranding value="2"/>                       ← same Löwenstein code as other LINE
<DeviceVariant value="0"/>
```

### What this sample adds

**1. `Eyra` codename spans years.** Builds seen so far on the same codename:

| Sample | FWBuild | FWVersion | Date | Git HASH |
|---|---|---|---|---|
| 25S (this) | `2021-0730-1454-Eyra` | `5.05` | 2021-07-30 | `2960d5f` |
| 20A | `2023-0310-1825-Eyra` | `5.07` | 2023-03-10 | `01fd90b` |
| 25ST | `2023-0310-1825-Eyra` | `5.07` | 2023-03-10 | `01fd90b` |

**Eyra is the multi-year firmware platform name** for the entire Prisma LINE family,
spanning at least mid-2021 through early 2023 (likely longer). The codename does
*not* identify a specific release — it identifies the firmware project.

**2. `MainboardHWVersion=11` is stable across all three LINE samples** — the same
mainboard is used in 25S (2021 firmware), 20A (2023), and 25ST (2023). This is the
platform-defining hardware version.

**3. `DisplayHWVersion` does vary**: `28` here vs `24` on the 20A and 25ST samples.
Display assemblies are model-specific (or batch-specific) and aren't part of the
platform identity. **Don't rely on DisplayHWVersion as a model discriminator** —
neither this sample (25S, display 28) nor the 25ST sample (display 24) lines up
with any obvious pattern.

**4. Loader handles older firmware fine.** No version-specific code path is needed
in `prisma_loader.cpp` — the same XML schema, same Linux-filesystem paths inside
`therapy.pdat`, same `<P id="N" val="V"/>` settings encoding. Eyra has been stable
through at least 2 minor versions and ~18 months of development.

### Observations / open questions

- **`PMVersion` and `StatisticVersion` track FW version.** They tick up alongside
  FW (`5.05 → 5.07` matches `PM 2.17.8 → 2.18.3` and `Statistic 2.4.1 → 2.4.2`).
  These are internal subsystem versions; cross-checking them against `FWVersion`
  can confirm a card hasn't been tampered with (or that a manual file copy didn't
  scramble subsystems).
- **`DeviceVariant=0` everywhere** so far — needs a non-zero sample to confirm what
  it encodes. Could be a hardware-revision lane, a regional SKU, or unused.
- **`__MACOSX/` directory in this sample** is purely from the user's Mac-side
  extraction (the dump was zipped on macOS). Detect must not rely on absence of
  such folders — real cards never have `__MACOSX/` directly written by the device.
- **The 25S sample is the smallest "clean LINE" card observed** — just two files
  at root, no auxiliary folders. This is the minimal viable Prisma LINE fingerprint
  for detection logic and tests.

---

## Löwenstein Prisma 30ST — fourth Prisma LINE sample (Eyra build matrix)

**Sample:** `C:/Users/Guy/Downloads/Prisma30ST Macka/Prisma`
**Device:** Löwenstein Prisma 30ST, serial `30167542`, firmware `5.05` (build `2022-0118-1647-Eyra`).
**Loader:** `prisma_loader.cpp` — **`DeviceType=27` not in `s_PrismaTestedModels`** (will import as "Unknown Model").
**Data range:** 2024-06-11 → 2024-06-25 (14 days, one gap on 06-17).

This fourth Prisma LINE sample shows that **Eyra `5.05` is a build train, not a single
binary** — different cards labelled `5.05` carry different subsystem git hashes.

### `device.xml`

```xml
<DeviceType value="27"/>                          ← gap: not in loader's table
<DeviceSerialNumber value="30167542"/>
<MainboardSerialNumber value="220110609"/>
<FWVersion value="5.05"/>                         ← same major version as the 25S sample
<FWRevision value="0022"/>                        ← but newer revision (25S had 0017)
<FWBuild value="2022-0118-1647-Eyra"/>            ← later 5.05 build (Jan 2022)
<FWGITHASH value="cd37170"/>                      ← different commit from 25S's 2960d5f
<KernelGITHASH value="c662cf81e54dc1abbb06b8f7ff79f3d69c6cf532"/>
<BuildrootGITHASH value="78a3ff9b40a59def49e09e369495250d40c82589"/>
<PMVersion value="2.17.8"/>                       ← same as 25S
<StatisticVersion value="2.4.1"/>                 ← same as 25S
<MainboardHWVersion value="11"/>                  ← stable platform
<DisplayHWVersion value="24"/>                    ← back to 24 (25S had 28)
<DeviceBranding value="2"/>                       ← Löwenstein, as expected
```

### Eyra build matrix (now with four data points)

| Sample | FW | Rev | Date | `FWGITHASH` | `KernelGITHASH` | `BuildrootGITHASH` |
|---|---|---|---|---|---|---|
| 25S | 5.05 | 0017 | 2021-07 | `2960d5f` | `c662cf81…` | `645ee54a…` |
| 30ST | 5.05 | 0022 | 2022-01 | `cd37170` | `c662cf81…` | `78a3ff9b…` |
| 20A | 5.07 | 0002 | 2023-03 | `01fd90b` | `f717b6b9…` | `78a3ff9b…` |
| 25ST | 5.07 | 0002 | 2023-03 | `01fd90b` | `f717b6b9…` | `78a3ff9b…` |

Reading the matrix:

- **Same `FWVersion+FWRevision` ⇒ identical firmware** (20A and 25ST are both
  `5.07/0002`, identical down to every git hash).
- **Same `FWVersion`, different `FWRevision` ⇒ different binary** (25S `5.05/0017`
  vs 30ST `5.05/0022` differ in FWGITHASH and BuildrootGITHASH but share the same
  KernelGITHASH).
- **`FWVersion` is the user-facing version**; `FWRevision` is the internal build counter.
  A user reporting "I'm on 5.05" can be on either `0017` or `0022` (or any value in
  between or after) — they're meaningfully different builds.
- **Subsystems version independently**: between 25S (`5.05/0017`) and 30ST
  (`5.05/0022`), the kernel was *not* updated (same hash) but the buildroot was —
  the buildroot move to `78a3ff9b…` happened during the `5.05` build train, before
  the kernel move to `f717b6b9…` which came with `5.07`.

**Loader implication:** OSCAR shouldn't dispatch on `FWVersion` alone for any
version-specific behaviour. If a bug needs to be worked around on a specific build,
gate on the FWGITHASH (or FWRevision combined with FWVersion). Treat `FWVersion` as
a marketing string, not a code-path discriminator.

### `DeviceType=27` is a second candidate gap

`s_PrismaTestedModels` in `prisma_loader.cpp:507-513` covers `22` (Prisma 25S) and
`23` (Prisma 25ST). The 30ST's `27` is a second LINE-family candidate (alongside the
20A's `10`) pending end-to-end loader verification — see "Prisma model candidates
pending verification" below. Both devices' cards detect and parse as LINE-family but
report "Unknown Model" in OSCAR's machine dialog until verification confirms it's
safe to add the entries.

### Session pruning policy — not a fixed count

Cumulative pruning data across LINE samples:

| Sample | Sessions kept | Date span | Sessions/day | Highest session ID seen |
|---|---|---|---|---|
| 20A | 30 | 24 days | 1.25 | 000044 (≥14 pruned) |
| 25ST | 206 | 24 days | 8.6 | 000270 (≥64 pruned) |
| 30ST | 34 | 14 days | 2.4 | 000475 (≥440 pruned) |

The retained-session count varies from 30 to 206, so **pruning is not a fixed-count
policy**. Two plausible drivers:

1. **Flash budget**: device keeps as many sessions as fit in the SD's reserved area;
   short sessions allow more retention than long ones.
2. **Time-based pruning**: device discards sessions older than ~N days. The 30ST card
   shows 14 days of data; if pruning is "keep 14 days", that lines up.

A non-truncated history sample would help confirm whether the cap is days-of-data
or sessions-or-MB. For now, OSCAR's loader should not expect to find every session
the device ever recorded — older data may be cloud-only.

### Observations / open questions

- **`DisplayHWVersion=24` on 30ST (matches 20A/25ST)** vs 25S's 28 — display
  hardware lineage isn't strictly chronological. The 25S's display version is
  the outlier, possibly because it's a different display panel batch.
- **`KernelGITHASH=c662cf81…` is shared between 25S and 30ST** (both `5.05`) —
  confirming that the kernel update to `f717b6b9…` came with the `5.07` jump.
- **30ST has very short retained history (14 days)** despite session ID 475
  showing 440+ prior pruned sessions. This is the strongest evidence yet that
  pruning is time-based, not session-count-based — a high-segmentation user
  generates many sessions/day but only the last ~2 weeks survive on the card.

---

## Prisma model candidates pending verification

This catalogue has captured Prisma SD cards from multiple LINE-family models. Some
of those models are **not currently in `prisma_loader.cpp`'s `s_PrismaTestedModels`
table** even though their cards detect and import via the LINE code path. They are
*candidates* for the tested-models table — but **adding them to the code requires
prior end-to-end verification that the loader processes that specific device's data
correctly** (channels, events, trend curves, statistics, etc.), not just that the
card detects.

This section is the running list to revisit when verification work is planned.

| `devid` / `DeviceType` | Firmware platform | Product line / model | Sample reference | Status |
|---|---|---|---|---|
| `0x91` (hex string) | Firefly | Prisma SOFT line | "A. P - PrismaSOFT" | In loader's table — identifies correctly. End-to-end verification still useful. |
| `0x92` (hex string) | Firefly | Prisma SMART line | *(no sample yet)* | In loader's table — no sample in catalogue to confirm. |
| `22` (decimal string) | Eyra | Prisma LINE / 25S | "tolnaiz-221009-prismaline-25S" | In loader's table — already verified. |
| `23` (decimal string) | Eyra | Prisma LINE / 25ST | "chamomile6-Prisma-25ST" | In loader's table — already verified. |
| `10` (decimal string) | Eyra | Prisma LINE / 20A | "DJC9-Lowenstein-Prisma20A" | **Candidate.** Detects on Eyra path, reports "Unknown Model". End-to-end loader behaviour not yet verified. |
| `27` (decimal string) | Eyra | Prisma LINE / 30ST | "Prisma30ST Macka" | **Candidate.** Detects on Eyra path, reports "Unknown Model". End-to-end loader behaviour not yet verified. |
| `5` (with `_str=V50-C`) | P34A11 | Prisma VENT / V50-C | "AKLERK Lowenstein Prisma Vent V50C" | **Different firmware platform.** Loader doesn't detect at all. Sentinel and config schema differ — would need separate loader work, not a table entry. |

For each LINE-family candidate, verification should at least confirm:
- Detect runs and identifies the card as Prisma LINE.
- Identity dialog shows correct serial / firmware.
- Day list populates correctly across the retained date range.
- Trend curves render in the daily view with sensible units.
- Per-session waveforms (`signal_*.wmedf`) load and display.
- Events from `event_*.xml` map to recognisable channels.
- Compare against another already-supported LINE model (25S or 25ST) and look for
  channel-set or unit divergences.

The Prisma VENT V50-C is *not* a candidate for the same table because its detection
sentinel is `prismaVENT.sdpvdat`, not `config.pcfg`. Adding VENT support is a separate,
larger task documented in the V50-C entry above.

---

## Löwenstein Prisma SOFT — Firefly firmware platform (Prisma SOFT product line)

**Sample:** `C:/Users/Guy/Downloads/A. P - PrismaSOFT/Prisma SOFT`
**Device:** Löwenstein Prisma SOFT, serial `0xbe1ea` (= decimal `778730`), firmware `3.7.0007` (build `2019-0717-1504-Firefly`).
**Loader:** `prisma_loader.cpp` — `devid="0x91"` IS in `s_PrismaTestedModels` (as `Prisma Soft`).
**Data range:** 2022-07-21 → 2022-08-03 (14 day-folders, 32 days summarised in statistic.psstat).

**Product-line vs firmware-platform note:** Löwenstein markets the **Prisma SOFT**
as its own product line — it is *not* part of the Prisma SMART line, despite the two
sharing the same underlying firmware project (`Firefly`) and SD card format. The
original loader was written for the Prisma SMART line (`prisma_loader.cpp:500` comment:
*"was created for PrismaSmart, should be extended to support PrismaLines as they
stabilize"*), then expanded to handle Prisma SOFT and Prisma LINE as additional
lines. So **Löwenstein's "lines" are marketing groupings**, while the **firmware
platforms** (Firefly / Eyra / P34A11) are the engineering reality the loader cares about.

This sample is the SOFT-line representative on the **Firefly** firmware platform. The
SMART line presumably writes a card with the same structure but `devid="0x92"`
(see candidates table below — no SMART sample is in this catalogue yet).

### Card-root structure

```
<SD card root>/
├── 0000000000/                         placeholder "session 0" — empty data
│   └── log/                            (empty in this sample)
├── 0000778730/                         primary device directory — name = device SN in decimal
│   ├── 20220721/                       per-day folder (YYYYMMDD)
│   │   ├── event_087.xml               session events
│   │   ├── event_088.xml
│   │   ├── event_089.xml
│   │   ├── signal_087.wmedf            session waveform (EDF)
│   │   ├── signal_088.wmedf
│   │   ├── signal_089.wmedf
│   │   └── trendCurves.tc              per-day trend
│   ├── 20220722/                       …
│   │   …
│   ├── 20220803/                       (newest day in this sample)
│   └── log/                            device-level diagnostic logs
│       ├── develop.log
│       └── service.log
├── Dcm/                                **note capitalization** — `Dcm`, not LINE's `DCM`
│   └── dcm.zip                         manufacturer's .NET decoder bundle
├── config.pscfg                        Prisma SMART config marker (567 bytes, JSON)
└── statistic.psstat                    aggregate statistics (16,995 bytes, JSON)
```

### What `Detect()` checks vs. what's there

`prisma_loader.cpp:552` checks for `config.pscfg` (SMART) or `config.pcfg` (LINE). This
sample has **`config.pscfg`** — so the SMART code path activates. `Detect()` requires
nothing else at root.

### Directory naming — top-level dir is device serial in decimal

The directory `0000778730/` matches the device's logical serial number from
`config.pscfg`:

```
"sn": "0xbe1ea"  →  0xBE1EA = 778,730 (decimal, zero-padded to 10 digits)
```

The `0000000000/` sibling is a **placeholder** — empty `log/` subdir, no data. Probably
a "no-device-bound-yet" stub or a slot for a different device's data on a multi-device
service card.

### `config.pscfg` — JSON, single line

```json
{
  "version": "1.1.0",
  "acfl": 0,
  "devid": "0x91",
  "date": "1659614873",
  "hash": "fir-1658767023",
  "dev": {
    "setversion": "1.1.0",
    "fwversion": "3.7.0007",
    "fwname": "2019-0717-1504-Firefly",
    "fwhash": "cabf3a76",
    "sn": "0xbe1ea",
    "hwsn": "0xd1e798b",
    "blidx": "0x1",
    "devid": "0x91",
    "hwversion": "6"
  },
  "cfg": {
    "1": 145, "5": 0, "6": 1, "7": 400, "8": 2000, "9": 1050, "10": 1050,
    "11": 400, "12": 400, "13": 2, "14": 0, "15": 1, "16": 3, "17": 1,
    "18": 45, "19": 30, "20": 0, "21": 22, "22": 1, "23": 15, "24": 24,
    "25": 1, "26": 19, "27": 2, "28": 0, "29": 0, "30": 8, "31": 0, "32": 0,
    "33": 2, "34": 0, "35": 0, "36": 0, "37": 0, "38": 2100
  },
  "crc": "0x5348634b"
}
```

Key fields:

| Field | Meaning | Sample value |
|---|---|---|
| `version` | Config schema version | `1.1.0` |
| `devid` | **Device class ID** (loader lookup key) | `0x91` → "Prisma Soft" |
| `dev.fwversion` | Firmware version | `3.7.0007` |
| `dev.fwname` | Firmware build name | `2019-0717-1504-Firefly` |
| `dev.fwhash` | Firmware commit hash | `cabf3a76` |
| `dev.sn` | Device serial (hex) | `0xbe1ea` |
| `dev.hwsn` | Hardware serial (hex, separate) | `0xd1e798b` |
| `dev.hwversion` | Hardware version | `6` |
| `dev.setversion` | Settings schema version | `1.1.0` |
| `dev.blidx` | Blower index | `0x1` |
| `cfg.<n>` | Therapy parameter ID → value (small int IDs) | e.g. `9 = 1050` |
| `crc` | Config CRC32 (hex) | `0x5348634b` |
| `date`, `hash` | Unix timestamps | (last-modified and integrity tags) |

The whole file is **a single line of JSON** — no whitespace. That makes manual reading
awkward but parsing trivial.

### Headline finding: **Firefly** codename (Prisma SMART platform)

`fwname` carries the platform/build codename:

```
2019-0717-1504-Firefly
```

`Firefly` is the firmware project codename used by the **Prisma SMART** and **Prisma
SOFT** product lines (which Löwenstein markets as separate lines but which share one
firmware project). It is entirely separate from `Eyra` (Prisma LINE product family)
and `P34A11` (Prisma VENT product line). So three distinct firmware projects under
the Prisma umbrella so far, plus four distinct product lines:

| Firmware platform (codename) | Product lines | `devid`/`DeviceType` examples | Config format | Card layout |
|---|---|---|---|---|
| **Firefly** | Prisma SMART, Prisma SOFT (this) | `0x91` (Soft), `0x92` (Smart) — hex strings | JSON (`.pscfg`) | Per-day folders + root stats |
| **Eyra** | Prisma LINE (20A, 25S, 25ST, 30ST, …) | `10`, `22`, `23`, `27` — decimal strings | XML inside ZIP (`.pcfg`) | One `therapy.pdat` ZIP |
| **P34A11** | Prisma VENT (V50-C, presumably V40 / V60) | `5` (with `_str=V50-C`) | JSON | Per-day ZIPs |

### `statistic.psstat` — root-level aggregate (JSON)

Multi-line JSON with the same `dev` identity block, plus aggregate `use` counters and
a `days` array:

```json
{
  "version": "1.2.0",
  "date": "1659614400",
  "hash": "1659614873",
  "dev": { … same as config.pscfg … },
  "use": {
    "1": 14635, "2": 14427, "3": 14820, "4": 14820, "31": 1659614873
  },
  "days": [
    { "day": { "cfg": "<embedded JSON config snapshot>", "5": 1657011145, "6": 166, … } },
    { "day": { … } },
    …
    { "day": { … } }   // 32 day records total
  ],
  "crc": "0x454da715"
}
```

Each `day.cfg` is a **string containing escaped JSON** — the config snapshot
that was active on that day. So daily settings changes are recoverable from
`statistic.psstat` without parsing the per-day folders.

Per-day fields (small numeric keys, meaning to be cross-referenced against the
`MCC.P3ParameterSet.dll` decoder):

| Key | Likely meaning |
|---|---|
| `cfg` | Stringified JSON config snapshot for the day |
| `5` | Day start as Unix timestamp |
| `6`, `7` | Usage duration in minutes (likely with-mask vs without-mask) |
| `10` | Array of 20 ints — pressure-band histogram? |
| `16`–`20` | Per-event-class counters (OAI, CAI, HI, RERA, snore — exact mapping TBD) |
| `21` | Array of 33 ints — per-hour histogram |
| `37`, `38`, `42`, `44` | Smaller counters; exact meaning TBD |

The presence of stats in `statistic.psstat` means **OSCAR can render daily summaries
without opening per-session signal files** — useful for fast import / daily list
rendering.

### Per-day session files

```
0000778730/20220721/
├── event_087.xml          XML event timeline (same schema as LINE event_*.xml)
├── event_088.xml
├── event_089.xml
├── signal_087.wmedf       EDF waveform (same .wmedf format as LINE / VENT)
├── signal_088.wmedf
├── signal_089.wmedf
└── trendCurves.tc         per-day trend curves
```

- **Session IDs are global, not per-day** — the day above has sessions `087`, `088`,
  `089`. Later days continue from `090` upwards.
- **One `trendCurves.tc` per day folder** — unlike LINE where they're all packed into
  `therapy.pdat` under `trendcurves/<date>/`.
- **`.wmedf` extension and EDF format are platform-wide** — same encoding as LINE
  and VENT.

### `cfg` parameter ID namespaces are platform-specific

The two Prisma families use overlapping but **separate** parameter-ID number spaces:

| Sample type | Example IDs in `cfg` |
|---|---|
| SMART/SOFT (this) | `1`, `5`, `6`, `7`, …, `38` (small ints, dense low range) |
| LINE (20A, 25ST, …) | `1001`, `1002`, `1003`, …, `1212` (1000-series) |

So even though both platforms encode therapy parameters as ID→value, the mappings
*cannot be shared* between them. Each family needs its own decoder (or its own
extraction from the bundled `MCC.*.dll`).

### Loader's `s_PrismaTestedModels` mixes hex and decimal

```cpp
static const PrismaTestedModel s_PrismaTestedModels[] = {
    { "0x92", "Prisma Smart" },     // hex string, Firefly platform
    { "0x91", "Prisma Soft"  },     // hex string, Firefly platform  ← this sample
    { "22"  , "prisma25S"    },     // decimal string, Eyra platform
    { "23"  , "prisma25ST"   },     // decimal string, Eyra platform
    { ""    , ""             }
};
```

The hex-vs-decimal split correlates with platform:

- **`Firefly` (SMART) devices** write `devid` as a **hex string** (`"0x91"`, `"0x92"`)
  in JSON config.
- **`Eyra` (LINE) devices** write `DeviceType` as a **decimal integer** (`22`, `23`)
  in XML config.

The loader stores both as strings and looks them up byte-for-byte against the device's
self-reported value — so the mixed-format table is correct, not an oversight. If a
future detector code path produces a hex value for a LINE device or a decimal value
for a SMART device, the lookups will silently miss; the existing string-equality
behaviour assumes the two namespaces stay separated by their natural format.

### Comparison across the three Prisma firmware platforms

(Product-line names — SMART / SOFT / LINE / VENT — are Löwenstein marketing groupings;
the firmware platforms below are what the loader actually has to dispatch on.)

| Feature | Firefly (SOFT line, this — also SMART line) | Eyra (LINE) | P34A11 (VENT) |
|---|---|---|---|
| Firmware codename | `Firefly` | `Eyra` | `P34A11` |
| Detect sentinel | `config.pscfg` (root) | `config.pcfg` (root) | `<WM>/<SN>/prismaVENT.sdpvdat` |
| Identity format | JSON (`{ "dev": { … } }`) | XML (`<DeviceID>` with attributes) | XML (`<Device_settings>` with `_str` companions) |
| Identity location | `config.pscfg` (cleartext) | `mnt/flash/conf/device.xml` (inside ZIP) | `device.xml` (cleartext at SN dir) |
| Statistics file | `statistic.psstat` (JSON, root) | `mnt/flash/data/statistics/statistics_year.bin` (binary, inside ZIP) | None at root — likely per-session |
| Parameter IDs | Small ints (`1`, `5`, `9`, …, `38`) | 1000-series ints (`1001`, `1002`, …) | String names (`AlarmApnoe`, `HoseResistanceP1Flow`) |
| Parameter format in config | JSON `cfg: {id: val}` | XML `<P id="N" val="V"/>` | JSON `{name: [v1, v2, v3]}` (3-profile) |
| Session storage | Per-day folder `YYYYMMDD/` with `event_NNN.xml` + `signal_NNN.wmedf` | One `therapy.pdat` ZIP containing per-day sub-paths | Per-day ZIP file `NNNN_YYYY-MM-DD.zip` |
| Session ID scope | Global (counter, e.g. `087`) | Global (e.g. `event_000270.xml`) | Per-day (`0001.wmedf` inside daily ZIP, restarts each day) |
| Trend curve location | Per-day inside session folder | Inside `therapy.pdat` (`trendcurves/<date>/`) | Top-level `trendcurve/` folder |
| `devid` / `DeviceType` style | Hex string (`0x91`, `0x92`) | Decimal int (`10`, `22`, `23`, `27`) | Decimal int (`5`) with `_str` companion |
| DCM bundle | `Dcm/dcm.zip` | `DCM/dcm.zip` (note casing) | `plugin/<dcm-version>/dcm.zip` |

### Observations / open questions

- **`Firefly` codename is the SMART-platform counterpart to LINE's `Eyra`.** Together
  with VENT's `P34A11`, this means **three independent firmware projects** ship under
  the Prisma name. Loader work is necessarily per-platform.
- **`Prisma Soft` (`devid=0x91`) is in the loader's tested-models list** so this card
  should identify correctly. End-to-end verification against the loader's actual data
  rendering (channels, daily statistics, event mapping) is still worth doing, but the
  identity gap closures noted for the LINE 20A/30ST don't apply here.
- **`statistic.psstat` carries pre-computed daily summaries** — significantly faster
  for OSCAR to populate the daily list from this file than from per-session
  `.wmedf` files. The 32-day `days` array here covers ~5 weeks; whether SMART devices
  ever exceed this retention is unknown.
- **Day-cfg-as-string in statistic.psstat** is unusual but useful — a daily settings
  history is recoverable from a single root file. OSCAR could surface a "settings
  changed on day X" indicator if desired.
- **Two top-level device dirs (`0000000000/` placeholder and `0000778730/` actual)**
  suggest the format can hold multiple devices' data on one card. A multi-device
  SMART card would have several non-zero serial-numbered directories alongside the
  zero placeholder. Detection logic should walk all non-zero serial-numbered dirs,
  not just pick one.
- **Older firmware than LINE samples**: `3.7.0007` (2019) here vs LINE's `5.05`/`5.07`
  (2021–2023). The SMART platform may have been frozen earlier (Soft has been
  superseded by newer Prisma LINE models for new prescriptions), making this
  vintage typical for in-the-field SMART cards.
- **`hwsn = 0xd1e798b`** (decimal 220,041,099) is independent of `sn = 0xbe1ea` —
  same dual-serial pattern as VENT's `Device_sn` / `Mainboard_sn`. Both should be
  retained for diagnostics.

---

## SEFAM S.Box AUTO (loader now exists — see note below)

**Sample:** `C:/Users/Guy/Downloads/SEFAM-2 Wagmar Barbosa de Souza` (SD card root —
the user's reference path `…/1263R/24462543` is two levels into the card).
**Device:** SEFAM S.Box AUTO (sleep diagnostic / autotitration device), model code `1263R`, serial `24462543`, firmware `VER :A020400`.
**Loader:** `sefam_loader.cpp` / `sefamDataParsing.cpp` (added 2026-08-03).
The loader accepts **any** SEFAM card matching `<digits><letter>/<digits>/DATA_nnn/`,
reading sample rates and channel names from each session's `.INI` and detecting
header length rather than assuming it. Only the **Rêve Auto (`1279R`)** has been
validated end to end against a manufacturer report, so any other model code —
including this S.Box AUTO `1263R` — imports on a best-effort basis and raises
`deviceIsUntested()`.

**Re-tested against the finished loader on 2026-08-03.** Parsing this sample
required three fixes, all now in the loader:

1. **Format tag is `#02/`, not `#03/`.** The tag is a version field. Matching
   `#03/` literally rejected the card outright.
2. **`Created By=S.Box_AUTO`** — punctuation differs from the Rêve's
   `REVE_AUTO`, so model-name matching normalises out `.`, `_` and spaces.
3. **Session directories are `DATA_0`, not `DATA_000`.** Unpadded names sort
   `DATA_10` before `DATA_2`, which would carry therapy settings backwards in
   time on a card with ten or more sessions. Sorting is now numeric.

What this card yields: **3 sessions, 13.44 h**, flow / pressure / total leak, all
record checksums passing. It carries **no `.LOG` files at all**, so it has no
events and no therapy settings — the loader's no-log path, previously untested.
Model code `1263R` correctly raises `deviceIsUntested()`.
**Device class:** **Sleep diagnostic / polygraph**, not a CPAP. Channel set includes
abdominal-effort and thoracic-effort belts, SpO2, pulse rate, body position, heart
rate — Type-3 PSG-class channels in addition to flow/pressure/leak.

### Card-root structure

```
<SD card root>/
├── 1263R/                                model code (the device's hardware family ID)
│   └── 24462543/                         device serial number
│       ├── 24462543.BKP        2,097,307 bytes — RAM backup (same size as .RAM)
│       ├── 24462543.RAM        2,097,307 bytes — device RAM dump (~2 MiB)
│       ├── DATA_0/                       per-session folder (one session in CPAP mode)
│       ├── DATA_1/                       …
│       └── DATA_2/
├── System Volume Information/            Windows NTFS metadata (ignore)
└── upload.dat                  568 bytes — cloud-upload staging or session manifest
                                          (binary, possibly encoded — see below)
```

The two-level `<model>/<serial>/` nesting under the SD root is the canonical layout.
A future detector would anchor on either:
- the presence of a `<digits>R/<digits>/DATA_0/` tree, **or**
- the more specific `DATA_<N>/DATA_<N>.INI` sentinel file (text, easy to validate).

The model code `1263R` is hardware-family-encoded: the trailing `R` suggests a hardware
revision letter, but without more samples this is a guess. The device's serial number
appears in two places — as the directory name and concatenated with the model code
inside the INI: `Serial Number=1263R24462543`. The combined form is what the device
considers its identity string.

### `DATA_<N>/` — one folder per session

Each `DATA_<N>` folder contains the same set of files: one per channel plus a `.INI`
manifest. File extension = channel code:

| Extension | Channel name | Unit | Sample rate (Hz) | Sample bits | Min | Max | Listed in INI? |
|---|---|---|---|---|---|---|---|
| `.FLW` | Flow | lpm | 25 | 8 | −180 | 280 | Chan0 |
| `.PRE` | Pressure | cmH2O | 5 | 8 | 0 | 255 | Chan1 |
| `.LK` | Leak | lpm | 1 | 8 | 0 | 153 | Chan2 |
| `.DET` | Event detection | — | 25 | 8 | 0 | 255 | Chan3 |
| `.SPO` | SpO2 | % | 1 | 8 | 0 | 255 | Chan4 |
| `.HRT` | Heart rate | bpm | 1 | 8 | 25 | 280 | Chan5 |
| `.PLS` | Pulse (oximetry) | — | 75 | 16 | 0 | 65535 | Chan6 |
| `.STS` | Status / state | — | 5 | 8 | 0 | 255 | Chan7 |
| `.THO` | Thoracic effort | — | 10 | 8 | 0 | 255 | Chan8 |
| `.ABD` | Abdominal effort | — | 10 | 8 | 0 | 255 | Chan9 |
| `.POS` | Body position | — | 1 | 8 | 0 | 255 | Chan10 |
| `.NSD` | (Nasal sensor data?) | — | (same as 25 Hz) | — | — | — | **Not in INI** |
| `.Y17` | Unknown | — | (same as 25 Hz) | — | — | — | **Not in INI** |
| `.INI` | **Channel manifest** (text) | — | — | — | — | — | self-describing |

The INI file is plain text in Windows-INI format and is the **strongest asset for any
future loader** — it self-describes every channel's name, unit, sample rate, bit depth,
and min/max range. A loader can populate OSCAR channels generically from the INI without
hard-coding any per-device knowledge.

Example INI (excerpt from `DATA_0.INI`):

```ini
[Create Info]
Created By=S.Box_AUTO
Serial Number=1263R24462543
Version=VER :A020400
Date=13/12/25 23:45:50
[Oximeter]
TYPE=NONE
BDA=  :  :  :  :  :
[BLE Device]
Local BDA=000000000000
Distant BDA=000000000000
[Start Record]
Hour=23
Min=45
Sec=50
Day=13
Month=12
Year=2025
Programmed Record Duration=28800
Real Record Duration=28800
[Chan0]
Name=FLW
Description=NO
Type=4
Unit=lpm
Min=-180
Max=280
Freq=25
Bit=8
[Chan1]
…
```

Sections present:
- `[Create Info]` — device identity (`Created By` model string + `Serial Number` + firmware version + recording date)
- `[Oximeter]` — `TYPE=NONE` here means no oximeter was attached for this recording
- `[BLE Device]` — Bluetooth pairing addresses (all-zero here = unpaired)
- `[Start Record]` — split timestamp (Hour/Min/Sec/Day/Month/Year as separate keys) plus duration in seconds
- `[Chan0]`–`[Chan10]` — 11 declared channels with full metadata

### Channel file sizes reveal a fixed header + raw-sample layout

`DATA_0` file sizes match the per-channel sample rates with a constant header offset:

| File | Size (bytes) | Sample rate | Computed header | Samples after header |
|---|---|---|---|---|
| FLW | 5,351 | 25 Hz | ~100 | ~5,250 |
| DET | 5,351 | 25 Hz | ~100 | ~5,250 |
| NSD | 5,351 | (likely 25 Hz) | ~100 | ~5,250 |
| Y17 | 5,351 | (likely 25 Hz) | ~100 | ~5,250 |
| PRE | 1,151 | 5 Hz | ~100 | ~1,050 |
| LK | 311 | 1 Hz | ~100 | ~210 |
| HRT, PLS, POS, SPO, STS, ABD, THO | 38 each | varies | 38 | **0** (header only — sensor disabled) |

Solving `(5351 − h) / (1151 − h) = 5` (the 25:5 Hz ratio) gives **h ≈ 101 bytes** of
file header. Cross-checks against PRE:LK and 25 Hz:1 Hz are consistent. So the format
is:

```
<file>:
  bytes 0…0x64        per-file header (~100 bytes; channel metadata, timestamp, etc.)
  bytes 0x65…EOF      raw samples — 1 byte each for 8-bit channels, 2 bytes each for 16-bit (PLS only)
```

### Channels with `38 bytes` = sensor not connected

The 38-byte files all correspond to channels in this session that **had no sensor
attached**: SpO2/Pulse/HeartRate (no oximeter — `[Oximeter] TYPE=NONE`), ABD/THO
(no effort belts), POS (no position sensor). The device writes a stub file (header
only, no samples) for every declared channel even if it wasn't recording — so the
INI's channel list is the **schema**, and the file's size reveals whether the channel
**actually carries data**.

This makes sense for the S.Box AUTO's dual role: when used as a CPAP autotitration
device, only FLW/PRE/LK/DET/NSD/Y17 are populated; when used as a Type-3 polygraph,
the effort belts and oximetry channels would have full data.

### Data is XOR-scrambled — key is `0xBF` (SOLVED)

Raw bytes in the data files are biased toward the high half of the byte range:
```
FLW: 9c 8f 8d 90 8e 8d 89 8c ed 8d 8b 8b 89 8d 8a 8b
     8c 9f 9f 9f 9f 9f 9f 9f 90 8d 8a 8e 8d 8e 8c 8d
```

> **Resolved on a later sample.** The guess recorded here originally — that the
> key was `0x9F` — was **wrong**. The key is a constant **`0xBF`**, applied to
> the fixed-length text **header** of every `.LOG` and channel file. The sample
> body after the header is stored in the clear, and its record checksums only
> validate against undescrambled bytes. (The `.INI` is cleartext throughout, and
> `.RAM`/`.BKP` are separately encrypted.) Confirmed by known-plaintext: the file
> header is text containing the device serial, which also appears in cleartext in
> the `.INI`.
>
> ```
> '0'(0x30)^0xBF = 0x8F    ' '(0x20)^0xBF = 0x9F    'R'(0x52)^0xBF = 0xED
> ```
>
> The run of `0x9F` above is not a zero region — it is the **run of trailing
> spaces** in the 20-character serial-number field of the header.
>
> The header is a fixed-length text record, not ~100 bytes as estimated in the
> section above: **71 bytes** for channel files, **38 bytes** for `.LOG`. The
> body is not a flat sample array either — it is fixed records of 10 seconds of
> samples plus a checksum and sequence number.
>
> Full decode of the container, the header fields, the record framing, the
> confirmed flow/pressure/leak scalings, and the open questions on events and
> settings: **`Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md`** (SEFAM Rêve Auto,
> model code `1279R` — same firmware platform as the S.Box AUTO documented here).

### `BKP` and `RAM` files at serial level

Both `24462543.BKP` and `24462543.RAM` are **2,097,307 bytes** — exactly equal in size.
Likely interpretation: the `.RAM` is the device's live RAM dump after the last session
(used for crash recovery / firmware diagnostics) and the `.BKP` is an earlier-known-good
backup of the same RAM image. They aren't per-session data — they're device-level
state files at the serial-number folder level.

### `upload.dat` at SD root

568 bytes, byte distribution similar to the channel data files (looks XOR-scrambled).
Naming suggests it's a **cloud-upload staging file** or per-card manifest of what's
been transferred. Not consumed by any loader; ignore for now.

### Recording-duration anomaly

The INI says `Programmed Record Duration=28800` and `Real Record Duration=28800`
(both 8 hours), but `DATA_0`'s 25 Hz channels only carry ~5,250 samples (~3.5 minutes
at 25 Hz). Likely explanations:
- The duration fields in the INI declare the **session's intended/maximum length**, not
  the actual data on the card. The device records to internal flash and the SD card
  copy may be a digest, a most-recent-snippet, or a tail buffer.
- Or `DATA_0` is a startup/test session and the full-night recording is in
  `DATA_1` / `DATA_2`. Sizes of those folders' channel files would clarify.

Without an existing loader to confirm semantics, this is the kind of mismatch worth
flagging early in any new loader's verification: never assume `Real Record Duration`
matches sample count.

### What a future SEFAM loader would need

> **Superseded — the loader was built on 2026-08-03.** The list below was the
> pre-implementation estimate and is kept only to show what the analysis
> anticipated. For what was actually built, read
> `Notes/loaders/SEFAM_LOADER_DESIGN.md`. Two items below turned out wrong: the
> XOR covers the header only (not the whole file), and `.Y17`/`.NSD` are not
> merely "ignorable" — `Y17` is the suspected home of the hypopnea durations the
> log omits.

1. **New Detect path**: walk into `<digits>+R/<digits>/DATA_<N>/` and check for
   `DATA_<N>.INI`. Or anchor on the SD-root combination of `<modelcode>/<serial>/`
   plus `upload.dat`.
2. **INI parser** — trivial (standard `[section]` + `key=value` text). Qt's
   `QSettings` with `QSettings::IniFormat` handles it directly.
3. **XOR-decoder** for channel data files — confirm key first.
4. **Per-channel reader** — read header (~100 bytes), then raw samples at the rate
   declared in the INI. Map `Name=FLW` etc. to OSCAR's existing channel IDs (most
   already exist: flow, pressure, leak, SpO2, pulse, position, effort).
5. **`.Y17` and `.NSD` channels** — not in the INI; their meaning needs separate
   reverse engineering or vendor documentation. A loader could ignore them initially.
6. **Sessions** — one per `DATA_<N>/`. Recording start timestamp comes from the
   `[Start Record]` section. Sessions appear capped at 3 per card here (DATA_0/1/2);
   it isn't clear yet whether older sessions are pruned or whether a fuller card would
   have more.

### Observations / open questions

- **SEFAM is a French sleep medicine company**; the S.Box AUTO is one of their
  combined diagnostic/CPAP-titration devices. The product is sold in clinic and
  home-care contexts; OSCAR is unlikely to encounter the dedicated diagnostic mode
  (PSG-class data) often, but the format is the same.
- **Self-describing INI** is the loader-friendliness highlight of this format —
  unlike BMC and Prisma where channel layouts are firmware-internal, SEFAM bakes
  the schema into every session folder. A loader doesn't need to know in advance
  what channels exist.
- **Empty-stub channel files** (38 bytes for disabled sensors) are a clever
  design — every session has the same file set, simplifying directory walking, but
  the actual data presence is signaled by file size. Loader logic: skip files
  whose size equals the header size (~100 bytes? need to confirm exact value).
- **Model code `1263R`** — the `R` suffix is suggestive of a hardware-revision
  letter (A/B/C/R). Other SEFAM devices may use `1263A` / `1264R` / etc. The
  loader should match the digits-then-letter pattern, not require an exact code.
- **XOR scheme is the main reverse-engineering hurdle** — likely a single-byte key
  (`0x9F` suggested by the trailing-zero pattern), but a multi-byte rolling key is
  also possible. Easy to brute-force once a known-plaintext sample is available
  (e.g. flow at a known time of zero breathing should give 0x00 ± noise).
- **`.BKP`/`.RAM` exact equal size of 2,097,307 bytes** is just under 2 MiB
  (2,097,152). The +155 byte excess is unusual; might be a header on top of a 2 MiB
  flash region, or might be aligned to a different size constant.
- **No OSCAR loader candidate exists** — this is a request for a new loader, not
  a model-table addition. Implementation would be a multi-week effort starting with
  XOR-key reverse engineering, then channel-by-channel decoder.

---

## VentMed DreamSleep DS6 (no existing loader)

**Sample:** `C:/Users/Guy/Downloads/Jonathan Cameron - VentMed-DreamSleep-DS6-jmcameron/VentMed DS6`
**Device:** VentMed DreamSleep DS6 (CPAP / APAP — Chinese-manufactured, sold via
DreamSleep / VentMed branding, often as a lower-cost alternative to mainstream brands).
**Loader:** **None — no VentMed / DreamSleep / `.ds1` references anywhere in
`oscar/SleepLib/`.** The `viatom_loader.cpp` in OSCAR is an **oximetry importer**,
not a CPAP loader — it handles Viatom CheckMe / O2Ring family pulse oximeters via
manual file import. Its `Detect()` always returns false (`viatom_loader.cpp:44`)
because oximeters aren't auto-detected from CPAP cards. Viatom is not a candidate
for VentMed support and shouldn't be confused with a CPAP loader.
**Data range:** 2021-07-12 → 2022-11-14 (per-day files spanning ~16 months).

### Card-root structure

```
<SD card root>/
├── 01022022.ds1           one file per used day; sizes vary 512 B – 642 KB
├── 01092021.ds1
├── 02092021.ds1
│   …
├── 14112022.ds1           (most recent day on this card — 14 Nov 2022)
└── Report/                pre-generated PC-software output (BMP charts + INI)
    ├── Report.ini
    ├── picAHITrend.bmp
    ├── picAvgPressTrend.bmp
    ├── picMonthTrend.bmp
    ├── picP90Trend.bmp
    └── picP95Trend.bmp
```

**Flat layout** — no subdirectories per day or per month. Each used day produces one
`.ds1` file at the SD root. Date format is **DDMMYYYY** (e.g. `14112022.ds1` =
14 November 2022 — note this is *day-first*, distinguishing from American-style
MMDDYYYY which would make `13072021.ds1` ambiguous, but `30082021.ds1` and
`31082021.ds1` exist, confirming day-first).

`Report/` is **not on the device's SD card** as written by the device — it's the
output of the manufacturer's Windows PC software. Evidence:
- `Report.ini` line 9: `picMonthTrend=C:\Users\Jonathan\Desktop\VentMed DS6\Report\picMonthTrend.bmp`
- The embedded path includes `C:\Users\Jonathan\Desktop\...`, so the reports were
  written by the user's PC software after extracting the card to disk. Detection
  logic should ignore `Report/` entirely.

### What a `Detect()` would have to check

There is no obvious sentinel filename — every file shares the `.ds1` extension and
DDMMYYYY date naming. Reasonable detection heuristics:

1. The SD root contains one or more files matching the regex `\d{8}\.ds1$`.
2. The first 2 bytes of any `.ds1` file are **`80 16`** (consistent signature across
   all .ds1 files inspected on this card — see "File format" below).
3. No competing loader's sentinel is present (e.g. no `THERAPY/`, `P-SERIES/`,
   `STR.edf`, etc.).

The `\d{8}\.ds1` naming convention plus the `80 16` lead-in together make a strong
detection signature.

### `.ds1` file format — 4-byte fixed-size TLV records

Each `.ds1` file is a stream of **fixed 4-byte records**. The first byte is a
**record tag** (`0x80`, `0x81`, `0x82`, `0x83`, `0x88`, `0x90`, `0x98`), and the
remaining 3 bytes are tag-specific payload. Most files are heavily zero-padded after
the data region (small days have 1–2 sessions of metadata followed by ~1.5 KB of
`0x00` filler).

Observed pattern from `01022022.ds1` (2,048 bytes; small recording):

```
Offset  Bytes              Interpretation (provisional)
0x000   80 16 02 02        Section header — session-start marker (tag 0x80)
0x004   81 06 22 34        Timestamp / config record (tag 0x81) — values 0x22 0x34
                           may encode year (2022 = 0x22? = 34? unclear) and minutes
0x008   88 12 00 01        Setting record (tag 0x88) — ID 0x12, value 0x0001
0x00C   88 00 00 00        Setting ID 0x00, value 0x0000
0x010   88 01 00 00        Setting ID 0x01, value 0x0000
0x014   88 02 00 01        Setting ID 0x02, value 0x0001
0x018   88 03 00 01        Setting ID 0x03, value 0x0001
0x01C   88 04 00 02        Setting ID 0x04, value 0x0002
0x020   88 05 01 02        Setting ID 0x05, value 0x0102 (= 258)
0x024   88 06 01 48        Setting ID 0x06, value 0x0148 (= 328)
0x028   88 07 00 3C        Setting ID 0x07, value 0x003C (= 60)
0x02C   88 08 01 7A        Setting ID 0x08, value 0x017A (= 378)
0x030   88 09 00 64        Setting ID 0x09, value 0x0064 (= 100)
0x034   88 0A 00 14        Setting ID 0x0A, value 0x0014 (= 20)
0x038   88 0B 00 21        Setting ID 0x0B, value 0x0021 (= 33)
0x03C   88 0C 00 03        Setting ID 0x0C, value 0x0003
0x040   88 0D 00 03        Setting ID 0x0D, value 0x0003
0x044   88 11 00 03        Setting ID 0x11, value 0x0003
0x048   88 0E 00 64        Setting ID 0x0E, value 0x0064
0x04C   88 0F 00 01        Setting ID 0x0F, value 0x0001
0x050   88 10 3F 22        Setting ID 0x10, value 0x3F22 (= 16162)
0x054   88 14 7F 7F        Setting ID 0x14, value 0x7F7F = "unused" sentinel
0x058   88 18 7F 7F        Setting ID 0x18, value 0x7F7F
0x05C   88 1C 7F 7F        … 0x1C
0x060   88 20 7F 7F        … 0x20
…       (continues with 7F 7F values at IDs 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C)
0x080   98 3C 3C 00        Section terminator (tag 0x98) — marks end of settings block
0x084   90 00 00 00        Data record (tag 0x90) — payload 00 00 00
0x088   90 00 07 55        Data record — payload 00 07 55
0x08C   90 00 00 00
…       (more 0x90 records — looks like per-event or per-minute snapshots)
0x0C0   98 3C 3C 00        Second section terminator
0x0C4   82 16 02 02        Tag 0x82 — possibly "second session" or "previous day" marker
0x0C8   83 06 22 35        Tag 0x83 — pairs with 0x82 the way 0x81 pairs with 0x80
0x0CC   00 00 00 00 …      Zero padding for remainder of file
```

Provisional record-tag taxonomy (needs vendor confirmation):

| Tag | Likely role | Payload pattern |
|---|---|---|
| `0x80` | Session-start marker (active session) | `16 <day> <month>`? |
| `0x81` | Session-config / timestamp | 3 bytes — possibly year + min/sec |
| `0x82` | Previous-session marker (mirrors 0x80 with different payload) | `16 <day> <month>` |
| `0x83` | Previous-session config (mirrors 0x81) | similar |
| `0x88` | Settings entry (one per setting) | `<setting-ID> <value-LE-or-BE>` |
| `0x90` | Per-event / per-sample data | `<sub-ID> <value-MSB> <value-LSB>` |
| `0x98` | Section terminator | always `3C 3C 00` here |

**`0x7F 0x7F` = "channel not present / setting disabled"** — used to fill out
unused entries in fixed-position settings tables. Same idea as SEFAM's
header-only 38-byte stub files: every session declares the same schema, and the
sentinel value flags absence.

### Long-session file (`10072022.ds1`) data region

In the 642 KB `10072022.ds1`, after the same `0x80`/`0x81`/`0x88` header (~128 bytes),
the body is dense `0x90`-tagged records:

```
0x080: 98 3c 3c 00 90 04 40 00 90 07 16 2e 90 0a 17 38
0x090: 90 0a 53 0f 90 0b 6e 2d 90 0d 28 59 90 0c 25 22
0x0a0: 90 0d 23 74 90 0d 02 77 90 0d 43 70 90 0d 65 05
…
```

The second byte (sub-ID) ranges `0x04 0x07 0x0A 0x0B 0x0C 0x0D 0x0E 0x0F …` —
these increment roughly monotonically, suggesting they're **time-position indices**
(minutes since session start? frame counters?). The last 2 bytes vary continuously
— consistent with sample data (pressure, flow, leak — one channel per index range?).

Mid-file the pattern stays `90 20 XX XX` for hundreds of records:
```
0x2570: 9020 0236 9020 023f 9020 0246 9020 024c
0x2580: 9020 024f 9020 0261 9020 0315 9020 033d
```

So sub-ID `0x20` (= 32) might correspond to a specific channel (flow? pressure?)
that gets a high volume of samples, while smaller sub-IDs (`0x04`–`0x10`) appear
once at session start (setup / initial-values phase). This is speculative until
the vendor's data layout is reverse-engineered, but it's enough for a future
loader to start probing.

### File-size variation tells us session intensity

| File | Size | Likely interpretation |
|---|---|---|
| `01022022.ds1` | 2,048 bytes | Header + minimal data → short or skipped session (file size = round 2 KB suggests fixed page-size flash write) |
| `14112022.ds1` | 512 bytes | Even shorter — possibly just the session-start ping with no usage |
| `10072022.ds1` | 642,048 bytes | Full-night recording |

The Report.ini shows many days with `lst<N>_<col>=-` (dashes) — meaning the PC
software couldn't extract usable data. Likely correlates with the small `.ds1`
files: when usage is < some threshold, the device writes a near-empty stub.

### `Report/Report.ini` — manufacturer summary

Plain Windows INI:

```ini
[Report]
mName=
mHeight= cm
mWeight= Kg
mBMI=
mNeck=
mBirth=
mTel=
picMonthTrend=C:\Users\Jonathan\Desktop\VentMed DS6\Report\picMonthTrend.bmp
… (more BMP paths, all with the user's Windows path embedded) …
mRange=10/16/2022 _ 11/14/2022
mAvgP95=--
mAvgP90=--
mAvgAHI=--
mNoDays=0
mLonger4h=0
mAvgDuration=--
ListCol=8
ListRow=31
lst1_1=11/14/2022
lst1_2=00:00:01
lst1_3=0.9
lst1_4=4.0
lst1_5=0.0
lst1_6=0
lst1_7=4.0
lst1_8=4.0
lst2_1=11/13/2022
lst2_2=-
…
```

Fields (column 1 = date; cols 2–8 = usage / AHI / P90 / P95 / etc.):
- Column 2: usage duration (`HH:MM:SS` or `-` if none)
- Column 3–8: AHI, P90 pressure, P95 pressure, etc. (mapping not fully verified)

The PC software pulls per-day stats from each `.ds1` and aggregates them into the
INI + BMP charts. OSCAR would generate its own equivalents from raw `.ds1` data —
no value in parsing `Report/`. **A loader should explicitly skip `Report/`** because
the embedded user paths are a privacy leak if propagated downstream.

### How VentMed DS6 differs from every other CPAP format catalogued

| Aspect | DS6 (this) | ResMed AS10 | PRS1 / DS Go | BMC G3X | Resvent (BMC iBreeze, Hoffrichter Pt3) | Prisma LINE |
|---|---|---|---|---|---|---|
| File granularity | **One file per day** | Per-session EDF set in day folder | Per-session 3-file set in flat folder | Big circular .NNN files + .idx | Per-session W/P chunks in day folder | One annual ZIP (`therapy.pdat`) |
| Filename encoding | **DDMMYYYY decimal** | yyyymmdd_hhmmss | hex 8-digit session ID | serial-based | NNNN_YYYY-MM-DD | session counter in ZIP |
| File format | **4-byte fixed TLV** with tag bytes | EDF | Encrypted (DS2) or plain (DS1) binary | Custom binary (.evt/.idx) | Mixed text/binary | XML + EDF in ZIP |
| Per-day file size variation | 512 B – 640 KB | EDF size scales with session length | Per-file sizes vary | N/A (fixed-size .NNN) | Fixed-size W chunks | N/A |
| Identity location | **Probably encoded in `0x81` records — not yet decoded** | Identification.tgt | PROP.TXT / PROP.BIN | .idx header | THERAPY/CONFIG/SYSCFG | device.xml inside ZIP |
| Decoder DLLs on card | No | No | No | No | No | Yes (DCM/dcm.zip) |
| Pre-generated reports on card | **Yes** (Report/ with BMPs — but user-side, not device-side) | No | No | No | No | No |

The 4-byte fixed-record TLV is the **smallest record granularity** of any format in
this catalogue — most CPAPs use variable-length packets. That's a simple loader once
the tag taxonomy is locked down, but reverse-engineering the tag meanings is the
main hurdle.

### Observations / open questions

- **Date format DDMMYYYY** (not MMDDYYYY) makes this format unambiguously non-US in
  origin even though the device is sold worldwide. DreamSleep marketing materials in
  Europe and Australasia confirm the day-first convention.
- **`0x80 0x16` lead-in across every `.ds1` file** is the strongest detection
  signature. `0x16` may be a format-version byte (= 22 — coincidence with 2022 in
  the year value? probably not, since it appears in 2021 files too).
- **Pairs of section markers** (`0x80`/`0x82`, `0x81`/`0x83`) suggest each `.ds1`
  may carry the *current* day's data plus a *previous* day's data — a rolling
  two-day buffer. Worth checking against the small files (which contain just a
  header pair and almost nothing else) to see whether the second pair always
  reproduces the prior file's content.
- **Sub-ID `0x20` dominates the data region** — likely a high-rate channel (flow?
  pressure?). Lower sub-IDs (`0x04`-`0x10`) appear at session start as one-shot
  initial-state values.
- **`0x7F 0x7F` sentinel** is reused throughout settings records as "this entry
  is unused / disabled". A loader can skip any `0x88 <id> 7F 7F` record.
- **`Report/` is user-side output, not device-side data** — embedded absolute
  Windows paths leak the original user's filesystem layout. A loader should
  ignore the folder entirely both for correctness (the .ds1 files are
  authoritative) and for privacy.
- **No OSCAR loader for this format** — `viatom_loader.cpp` is unrelated (Viatom
  oximeters, not CPAPs, and never auto-detected). A new VentMed/DreamSleep loader
  would need:
  1. Detect: SD root has files matching `\d{8}\.ds1` whose first 2 bytes are `80 16`.
  2. Parser: walk fixed 4-byte records, dispatch on tag byte. Stop at first
     all-zero record (end of valid data, start of padding).
  3. Tag taxonomy: lock down the meaning of `0x80`/`0x81`/`0x82`/`0x83`/`0x88`/
     `0x90`/`0x98` against a small known-setting card (e.g. user reports their
     min/max pressure → look for matching values in `0x88 <id> XX XX` records).
  4. Channel decoder: identify which `0x90` sub-IDs correspond to flow / pressure
     / leak / event channels; correlate with the Report.ini summary values.
  5. Skip `Report/` and treat it as opaque.
- **Multi-week effort** for a new loader — falls in the same bucket as Prisma
  VENT as "new format, no existing loader candidate." Distinct task from the
  Prisma LINE table-entry additions. (SEFAM was formerly in this bucket; its
  loader was built on 2026-08-03.)

---

# Catalogue coverage

Snapshot of what this catalogue covers as of the latest round of fingerprinting, and
the known gaps where future samples would round it out.

## CPAP loaders with at least one sample documented

| Loader file | Brand / family | Samples in catalogue |
|---|---|---|
| `bmc_loader.cpp` | BMC Luna G3 (legacy) | BMC Luna G3 |
| `bmcg3x_loader.cpp` | BMC G3X (modern) | BMC G3 A20 |
| `intellipap_loader.cpp` | DeVilbiss IntelliPAP / BlueLake (DV5x + DV6x) | **DV64D** "Blue StandardPlus" — two samples: Cuppa (2016 device, firmware V023, required wrapper restore); flipmike (2018 device, firmware V031). Same format across vintage |
| `prisma_loader.cpp` | Löwenstein Prisma (multi-platform) | Prisma 20A, 25S, 25ST, 30ST (LINE / Eyra); Prisma SOFT (SOFT line / Firefly) |
| `prs1_loader.cpp` | Philips Respironics System One / DreamStation | DreamStation 2 (encrypted), DreamStation Go Auto + DreamStation Go (cleartext, two-device card), **DreamStation CPAP 200X110** (brick — 4-year card, 5 Px partitions, 3083 sessions), **BiPAP A40** (PRS1-detected but unsupported — `(F3,V4)` not in tested-model table; real data lives in parallel `BIPAP-A/` EDF+D tree no loader reads) |
| `resmed_loader.cpp` | ResMed S9 / AirSense / AirCurve (S9/AS10/AS11) | S9 AutoSet (S9 — flat DATALOG), AirCurve 10 VAuto (AS10), AirSense 10 CPAP basic (AS10, summary-only), AirSense 11 AutoSet (AS11) |
| `resvent_loader.cpp` | Resvent platform (Resvent / BMC iBreeze / Hoffrichter) | BMC iBreeze 20A, Hoffrichter Point 3 AutoCPAP |
| `sefam_loader.cpp` | SEFAM (also sold as Sanrai in some markets) | **Rêve Auto `1279R`** — validated end to end against a manufacturer *Sefam Analyze* report for the same 31 sessions. S.Box AUTO `1263R` documented above but **not** re-tested since the loader was written. Added 2026-08-03; **awaiting real-user testing** |
| `yuwell_loader.cpp` | Yuwell YH-series | YH-550A (Format A), YH-580C (Format B — single 64 KB ring-buffer file at root), **YH-825A** (Format C, BiPAP S/T — IPAP/EPAP decode added 2026-05-21; LE u16×10 at record offsets `0x02`/`0x04`), YH-680B (Format D summary-only), YH690F (Format D full) |

## CPAP loaders with no sample yet (would round out the catalogue)

| Loader file | Brand / family | What a sample would add |
|---|---|---|
| `icon_loader.cpp` | Fisher & Paykel Icon | F&P CPAP format coverage |
| `sleepstyle_loader.cpp` | Fisher & Paykel SleepStyle | The newer F&P platform (replaced Icon) |
| `mseries_loader.cpp` | Philips Respironics RemStar M-Series | Pre-System One / pre-DreamStation Philips lineage — completes the PRS1 generational picture (M-Series → System One → DreamStation 1 → DreamStation 2) |
| `weinmann_loader.cpp` | Weinmann SOMNOsoft / SOMNOBalance | Pre-Prisma Weinmann devices — predecessors to the Löwenstein Prisma family on the same vendor's older firmware |
| `somnopose_loader.cpp` | SomnoPose | Phone-app positional-therapy data import (distinct from CPAP-card formats — not a true SD-card-fingerprinting target, but worth confirming with a sample) |
| `vrem_loader.cpp` | **vTitan vREM** — CPAP/APAP by vTitan (Indian medical-device manufacturer, distribution mainly in India / Southeast Asia). Directory matches `VREM…` per `vrem_loader.cpp:83` | **First real-world sample.** The loader was contributed to OSCAR by a vTitan employee — apparently to get vendor coverage — and as far as the project is aware, OSCAR has **never seen actual user data** from a vREM device. Any first sample is worth validating carefully against the loader's behaviour, since the code path hasn't been exercised by independent users. |

## Oximetry and other importers (manual file import, not CPAP-card auto-detect)

These are **not part of the CPAP-card fingerprinting effort** — they import individual
data files via the user picking them, and their `Detect()` methods return `false` to
opt out of the CPAP-card auto-detection path. Listed here so that future contributors
don't confuse them with missing CPAP loaders.

| Loader file | Device class |
|---|---|
| `viatom_loader.cpp` | Viatom CheckMe / O2Ring pulse oximeters — oximetry only, **not a CPAP loader** |
| `cms50_loader.cpp` | Contec CMS50X pulse oximeter |
| `cms50f37_loader.cpp` | Contec CMS50F-37 pulse oximeter |
| `md300w1_loader.cpp` | ChoiceMMed MD300W1 pulse oximeter |
| `zeo_loader.cpp` | Zeo sleep monitor (defunct company) |
| `dreem_loader.cpp` | Dreem sleep-tracking headband |

## Formats with no OSCAR loader at all (documented for future work)

These cards have been fingerprinted but no existing OSCAR loader handles them.
Implementing each is a multi-week reverse-engineering effort, not a table-entry
addition.

| Format | Sample reference | Notable structural traits |
|---|---|---|
| Löwenstein Prisma VENT V50-C | "AKLERK Lowenstein Prisma Vent V50C" | `P34A11` firmware platform; `prismaVENT.sdpvdat` sentinel; per-day ZIPs |
| VentMed DreamSleep DS6 | "Jonathan Cameron - VentMed-DreamSleep-DS6" | Per-day `.ds1` files; 4-byte fixed TLV records starting `80 16` |
| Philips BiPAP A40 (`BIPAP-A/` tree) | "AKLERK 20240205_Philips_BiPAP_A40" | Standard EDF+D in `BIPAP-A/A*.EDF`/`D*.EDF`/`W*.EDF`; parallel `P-SERIES/` stub triggers PRS1 detect but `(F3,V4)` is unsupported. Hospital NIV family (A-Series — Trilogy/A30/A40 adjacency) |

## Stopping point

The current round of fingerprinting has exhausted the available SD cards on hand.
The CPAP-loader gaps above are the natural next-round targets if those samples
become available. None of the gaps are blocking active OSCAR work — they're catalogue
completeness items, useful when matching an unknown card against an existing loader.
