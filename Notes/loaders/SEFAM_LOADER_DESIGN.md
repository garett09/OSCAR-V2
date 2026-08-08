# SEFAM loader — design spec

**Date:** 2026-08-03
**Status:** approved design, not yet implemented.
**Format reference:** `Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md` — the container,
waveform scalings, event taxonomy and settings records are all decoded there and
validated against the manufacturer's *Sefam Analyze* report for the same card.
This document covers only how that format becomes an OSCAR loader.

---

## 1. Scope

Import **any Sefam card** matching `<digits><letter>/<digits>/DATA_nnn/`, not just
the one validated model.

Everything that can vary between models is read from the data rather than
hardcoded: sample rates and channel names come from the `.INI`, header length is
detected, and record size is derived from the declared rate. Only one model
(`1279R`, Rêve Auto) has been validated end to end against a vendor report, so
`Open()` emits the existing `deviceIsUntested(info)` signal for any other model
code. Unknown Sefam devices therefore import, but announce that the format has
not been confirmed for them.

Out of scope for this version: the `.RAM`/`.BKP` images (encrypted), the `DET`,
`NSD` and `Y17` channels, and oximetry/polygraph channels (never populated on a
CPAP-only device).

## 2. Files

| File | Purpose |
|---|---|
| `oscar/SleepLib/loader_plugins/sefamDataParsing.h/.cpp` | Pure decode. No OSCAR types. |
| `oscar/SleepLib/loader_plugins/sefam_loader.h/.cpp` | OSCAR integration. |

Both pairs added to `oscar.pro` (`SOURCES` and `HEADERS`). `main.cpp` gains the
`#include` and a `SefamLoader::Register();` call alongside the other loaders.

The split mirrors `bmc_loader.cpp` / `bmcDataParsing.cpp`. The reason is
verification: the format is already specified and validated on paper, so the
risk in this project is transcription error, not misunderstanding. A decode
layer that takes bytes and returns plain structs can be checked directly against
known-correct numbers without constructing a `Machine` or `Profile`.

## 3. Parser layer — `SefamParsing`

```cpp
struct FileHeader {
    QString   serial;        // trimmed of pad spaces
    QDateTime localStart;    // from YYMMDDhhmmss — device local time
    qint64    utcEpoch;      // 0 when absent (short header variant)
    int       length;        // 71 (channel files) or 38 (.LOG)
};

struct ChannelSpec { QString name; int freq, bits, min, max; };

struct LogRecord {
    qint64  utcSeconds;
    quint8  code;
    quint16 arg;
    QByteArray payload;      // bytes 11..48 — settings live here for codes 2/13
};

struct Settings {
    bool  valid = false;
    float minPressure = 0, maxPressure = 0, rampPressure = 0;
    int   rampMinutes = 0;
};

struct SessionData {
    QString dirName;
    FileHeader header;
    int recordCount = 0;                          // 10-second records
    QHash<QString, ChannelSpec>     channels;     // schema from the .INI
    QHash<QString, QVector<quint8>> samples;      // raw bytes per channel
    QVector<LogRecord>              log;
    Settings                        settings;
};

void descramble(QByteArray &);                    // XOR 0xBF in place — HEADER ONLY
bool parseHeader(const QByteArray &decoded, FileHeader &out);
bool parseIni(const QString &path, QHash<QString,ChannelSpec> &, QDateTime &start);
bool readChannel(const QString &path, const ChannelSpec &,
                 QVector<quint8> &out, int &records, QString &error);
bool readLog(const QString &path, QVector<LogRecord> &out);
bool parseSettings(const LogRecord &, Settings &);
```

`readChannel` carries the real logic: derive record size as
`freq × 10 × (bits/8) + 3`, require `(fileSize − headerLen) % recordSize == 0`,
then per record verify the 8-bit sum checksum and the big-endian sequence
number before concatenating sample bytes.

The `bits` field must be honoured in the record-size formula even though every
channel this version imports is 8-bit. The `.INI` declares `.PLS` as 16-bit, so a
parser that assumed 8 bits would compute the wrong record size and wrongly reject
the file as malformed. `readChannel` sizes 16-bit channels correctly; §6 simply
does not map any of them to an OSCAR channel.

**Header length is detected, never assumed.** Parse the fixed fields
(`#03/`, 20-char serial, `/`, 12-char timestamp, `/`), then test whether 32 hex
digits followed by `/` come next. Present → 71-byte channel header; absent →
38-byte `.LOG`-style header. Do **not** locate the end of the header by scanning
for the fourth `/` — a payload byte can decode to `/` and shift the parse (this
occurs in the sample card).

The parser returns error strings rather than logging, keeping it free of UI
dependencies.

## 4. Loader layer

```cpp
class SefamLoader : public CPAPLoader {
    bool Detect(const QString &path) override;
    int  Open(const QString &path) override;
    int  Version() override { return sefam_data_version; }   // = 1
    const QString &loaderName() override;                    // "SefamLoader"
    MachineInfo newInfo() override;                          // brand "Sefam"
    bool backupData(Machine *mach, const QString &path);
    static void Register();
};
```

No `initChannels()` override — every event maps to an existing OSCAR channel, so
there are no custom channel IDs to register (see §7).

`Detect()` walks for `<digits><letter>/<digits>/DATA_nnn/DATA_nnn.INI` and also
verifies the `#03/` tag in a channel header, so a coincidentally-shaped
directory tree from another vendor cannot produce a false positive.

**Machine identity.** Brand is `"Sefam"`. Model is taken from the `.INI`
`Created By` field with its trailing padding trimmed (`REVE_AUTO` → shown as
`Rêve Auto`), falling back to the model code when that field is absent or
unrecognised. Series is the model code (`1279R`); serial is the full
`<modelcode><digits>` string from the header, which matches the `.INI` `Serial
Number` field. The card carries no brand string of its own, so `"Sefam"` is a
naming choice: the same device is distributed in some markets under a
distributor's label (see the format notes), and users may report it under either
name.

## 5. Session assembly and time handling

One `DATA_nnn/` directory → one OSCAR `Session`. `SessionID` is the header's UTC
epoch: unique, stable across re-imports, and cheap to test against
`mach->SessionExists()`. When the header carries no UTC epoch (short-header
variant), `SessionID` falls back to `localStart.toSecsSinceEpoch()`. That is
stable for a given machine and timezone, which is sufficient for the dedup test;
it is not comparable across timezones, but neither is any other identifier such a
card offers.

Short mask-off restarts remain separate sessions. OSCAR's existing day grouping
merges them into an OSCAR day, as it does for other multi-session devices.

**Session start uses the local wall-clock fields**, so a 22:03 bedtime displays
as 22:03 regardless of the importing machine's timezone.

**Events are placed relatively**, which needs no timezone arithmetic:

```
eventTimeMs = sessionStartMs + (logRecord.utcSeconds − header.utcEpoch) × 1000
```

This requires the 32-hex field, which the short header variant lacks. A card
whose headers carry no UTC epoch imports **waveforms only**, with a warning, and
skips events. That is the correct conservative default: no non-`1279R` device
has had its event codes validated.

## 6. Waveforms

| `.INI` name | OSCAR channel | stored value | gain | rate |
|---|---|---|---|---|
| `FLW` − `LK` | `CPAP_FlowRate` | patient flow × 10 | 0.1 | 100 ms (10 Hz) |
| `PRE` | `CPAP_MaskPressure` | raw byte | 0.1 | 200 ms (5 Hz) |
| `PRE` smoothed | `CPAP_Pressure` | 10 s moving average | 0.1 | 200 ms (5 Hz) |
| `LK` | `CPAP_LeakTotal` | raw byte | 0.6 | 1000 ms (1 Hz) |

Rates come from the `.INI`, not the table; the table records what the validated
model declares. Values are stored as `qint16` via the `qint16 *` overload of
`EventList::AddWaveform`.

`LK` is **total** leak including the intentional mask vent, hence
`CPAP_LeakTotal` rather than `CPAP_Leak`.

### Therapy pressure is derived, because the card carries only mask pressure

`PRE` is a **mask** pressure: it carries the breathing ripple, about 1.0 cmH₂O
peak to peak. The card has no separate therapy-pressure signal anywhere:

- `PRE` is the only channel declared with `Unit=cmH20`.
- The other populated channels are bit fields, not analogue signals. Across all
  5.72 M samples per channel on the validated card, `NSD` takes **4** distinct
  values (0, 1, 4, 255) and `Y17` takes **11** (0, 1, 4, 8, 16, 32, 33, 36, 40,
  128, 255). `DET` takes 93, but they are plainly bit-structured — `0x02` alone
  is 55% of samples, `0x22` 19%, `0x62` 11%, and the whole set is
  {low 2 bits} × {`0x00`, `0x10`, `0x20` … `0xF0`}.
- No `.LOG` record reports a pressure. Of the 17 record codes present, only
  codes 2 and 13 carry non-zero payloads, and both are settings records.
- The manufacturer's own report agrees: it publishes only an **average**
  pressure per session, with `Prescribed pressure` shown as `-` in A-PAP.

So `CPAP_Pressure` is derived from `PRE` by averaging the ripple away. It has to
exist as its own channel: `CPAP_Pressure`, not `CPAP_MaskPressure`, is what
drives the Pressure graph (`daily.cpp`), the overview trend (`overview.cpp`) and
the pressure statistics. Mapping `PRE` to `CPAP_MaskPressure` alone would cost
all three — the state `prisma_loader.cpp` is currently in, with its
`AddWaveform(CPAP_Pressure, "CPAPPressure")` commented out.

**A moving average, not a median.** A median preserves edges better, which
matters because the device slews up to 2.7 cmH₂O in ten seconds. But it is
biased. Measured against a breath-synchronous reference (a boxcar exactly one
ripple period wide, which is unbiased and local) over 24 sessions:

| filter | mean error | bias | error during slews |
|---|---|---|---|
| **mean, 10 s** | **0.053** | **+0.000** | 0.222 |
| mean, 20 s | 0.050 | +0.000 | 0.534 |
| median, 10 s | 0.092 | +0.031 | 0.149 |
| median, 20 s | 0.090 | +0.033 | 0.227 |

The median's systematic +0.03 cmH₂O bias nearly doubles its overall error and
would shift the reported session average away from the manufacturer's figure.
The 10 s average leaves it untouched — measured shift raw → smoothed is
−0.0008 cmH₂O mean, 0.0066 worst case, far below the 0.1 cmH₂O display step.

Ten seconds is about two and a half breaths, which cuts the ripple to roughly
0.1 cmH₂O peak to peak — the channel's own quantisation step. Twenty seconds
halves the residual ripple again but more than doubles the error across pressure
ramps, which is exactly when a user is reading the graph.

#### A finer, smoother variant was built, measured and rejected

The graph still looks slightly coarse, and the cause is understood. Zoomed in it
draws a small square wave — that is **quantisation, not leftover ripple**. The
derived channel is stored at gain 0.1, the same step the card itself uses, so
the sub-0.1 detail that averaging genuinely recovers is rounded away and the
value toggles between two adjacent tenths.

A version fixing both halves of that was implemented and measured:

- **Kernel:** three passes of a 6 s boxcar instead of one 10 s pass. Cascading
  approximates a Gaussian, whose stopband rejection is far better, so unlike
  widening a single boxcar it improves *both* axes at once — residual ripple
  0.026 vs 0.099, ramp error 0.089 vs 0.098. Cascading must be done in floating
  point; rounding between passes undoes most of the benefit.
- **Gain:** 0.01 on the derived channel, which removes the square wave entirely.

**Rejected on cost/benefit.** The visible improvement was slight, while the
finer gain took `Session::m_timesummary` — one entry per distinct stored value,
persisted to `session_channel_values` — from ~56 to ~396 rows per session, 7.1×
on the validated card. Revisit only if a user complains about the pressure graph
specifically; the measurements above are the whole argument, so there is no need
to re-derive them.

Two facts about this card worth keeping either way. Ripple **grows with
pressure** — measured peak-to-peak 0.69 cmH₂O at 4.0 but 2.17 at 7.5 — so
high-pressure stretches of a night are always the roughest, whatever the filter.
And sharp downward notches that survive even heavy smoothing are **real**: they
are single deep inspirations pulling mask pressure down by 2-3 cmH₂O, and the
manufacturer's own graph shows them too.

The smoothed channel **keeps its source's validity mask exactly**, so both
pressure channels cover the same span and split at the same gaps. Letting the
window bridge a drop-out would extend `CPAP_Pressure` half a window past each
end of every gap in `CPAP_MaskPressure`, and the two graphs would not line up.

### Blower-off stretches are excluded from waveforms and from usage time

The card keeps writing 10-second records while the blower is stopped, so a
session's files span the time the machine was **powered on**, not the time it
was treating. Left in, those stretches are counted as therapy twice over: they
are averaged into the pressure statistics and they inflate the session length.

On the validated card one 19:40 session contains 269 s of blower-off. That
pulled its mean pressure to 3.22 cmH₂O against the manufacturer's 4.3 — the only
session of 31 whose average did not agree.

**Detection.** While stopped the card records a spread of near-zero pressures —
0.0 to 0.4 cmH₂O across 1,694 s card-wide, not one sentinel value — so a
single-value test does not work. Therapy never goes below the device's 4.0 cmH₂O
minimum setting, leaving a wide empty band (0.5–1.9 totals ~43 s card-wide), and
the threshold sits in it at **2.0 cmH₂O**, sustained for at least 10 s. Samples
with no data at all count as stopped too, which also catches the 10-second
sentinel every session opens with. The minimum duration suppresses the brief
sub-threshold dips seen during ramp-down and ramp-up.

> **The threshold is an assumption, not a value read from the card.** It has
> margin only because these devices bottom out at 4.0 cmH₂O. A model whose
> minimum pressure could be set lower would narrow it.

**Effect.** Pressure, leak and flow all gap out together, and `MaskOn`/`MaskOff`
slices are recorded so `Session::hours()` reports usage instead of span —
`bmc_loader.cpp` does the same, and `prs1_loader.cpp` likewise records both
statuses, which lets `gSessionTimesChart` draw the off periods in black while
`Day` counts only `MaskOn`. Measured over the whole card:

| | before | after | vendor |
|---|---|---|---|
| worst session | 19:40 | **15:01** | 16 min |
| card total | 159.003 h span | **158.557 h usage** | 158 h 50 usage |
| span − usage | — | **26.8 min** | 28 min |
| worst session mean pressure | 3.22 | **4.15** | 4.3 |

It also removes the artefact where the smoothing turned each blower transition
into a ramp: the raw steps in 0.6 s but a centred 10 s average spread that over
7 s *and started 4 s early*. Those transitions are now gap edges instead.

**Scored events are deliberately left alone.** Only 7 of 1568 fall inside a
detected off-span (2 OA, 1 CA, 4 snore) and they sit at the detection
boundaries; dropping them would disturb event counts validated against the
manufacturer's report to within one event.

**A session that is entirely blower-off gets no slices at all**, and a warning.
An empty slice list silently means "no slice information", so `hours()` falls
back to the full span — the opposite of the intent — but the alternative is
worse: `Day::cph()` divides by `hours()` with no guard (`day.cpp:1109`, and
`sph()` likewise), so a zero-usage day would produce inf/nan. One session on the
validated card is in this state and overstates usage by 7 minutes out of 158 h.

Two things this does **not** fix. The vendor's `Mask disconnected` column is a
different measurement — it is non-zero on sessions where the blower never
stopped, and where both are non-zero ours runs 8–18 s shorter — so it is
probably leak-threshold based rather than blower-based. And the vendor appears
to **truncate** its averages to one decimal rather than round, which accounts
for most of the residual 0.03–0.06 offsets across the remaining sessions.

### Flow must be converted to patient flow

The card's `FLW` channel is **total** flow — it includes the intentional mask
vent, so it oscillates around roughly +22 L/min rather than zero. OSCAR cannot
use that directly: `FlowParser::calcPeaks()` detects breaths by crossings of a
hard-coded `zeroline = 0`. Measured over one 8.5 h session:

| signal | mean | zero crossings/min |
|---|---|---|
| total flow (`FLW`) | 26.70 | 7.0 |
| patient flow (`FLW − LK`) | **0.17** | **33.6** |

33.6 crossings/min is ~16.8 breaths/min, a normal respiratory rate. Imported as
total flow, breath detection fails outright and OSCAR produces **no** respiratory
rate, tidal volume, minute ventilation or Ti/Te — all of which it otherwise
derives for free.

So the loader subtracts the device's own leak estimate, sampled at `LK`'s rate
and held across each flow sample. This is principled rather than a fudge: total
flow = patient flow + leak, and the two channels were independently confirmed to
agree at the baseline (23.8 vs 22.8 L/min).

### Offsets must be baked into the stored value

`EventList` has an `offset` field, but OSCAR applies it **inconsistently**:

- `gLineChart.cpp` renders `(raw + offset) * gain` — offset applied *before* gain
- `EventList::data()` returns `raw * gain` — offset ignored
- `FlowParser::openFlow()` likewise applies gain alone

A channel needing an offset would therefore graph differently from how it is
summarised, and would break breath detection. The loader passes `offset = 0`
everywhere and pre-scales the stored value instead. This is why flow is stored
in tenths of a L/min with gain 0.1 rather than as raw bytes with gain 460/255 and
offset −180.

Channels not imported:

- `DET` — breath-phase bitfield. `Session::UpdateSummaries()` already calls
  `calcRespRate()`, which derives respiratory rate, tidal volume, minute
  ventilation, Ti and Te from the flow waveform. Importing `DET` would duplicate
  that and diverge from every other loader.
- `Y17` — undecoded bit field. Not declared in the `.INI` at all, yet written on
  every session. 11 distinct values card-wide.
- `NSD` — near-constant bit field: 99.64% zero, with 17,610 samples of `1` and
  120 of `4` across the card. Not empty, but nothing is decoded from it yet.
- `ABD`, `HRT`, `PLS`, `POS`, `SPO`, `STS`, `THO` — header-only stubs. Skipped by
  the size test: file size equal to header size means the channel was not
  recorded.

### The 255 sentinel

`0xFF` marks invalid/no-data on all three waveforms. Imported naively it reads as
**153 L/min leak** and **25.5 cmH₂O**, which would corrupt both statistics. It is
not rare enough to ignore — 1,324 s of `LK` and 310 s of `PRE` across the sample
card, and one session is 412 of 420 seconds sentinel.

Each channel is therefore split into contiguous runs of valid samples, and **each
run becomes its own `EventList`** via a separate `AddEventList()` call, the same
way ResMed handles EDF gaps. One `EventList` holds one contiguous span, so this
is the only correct mechanism.

Splitting is at **sample** granularity, not record granularity: `PRE` sentinel
runs are all record-aligned but `LK` runs are not (28 of 36 in the sample), so
record-level splitting would leak sentinel values into the data.

## 7. Events

| Log code | OSCAR channel | Event value |
|---|---|---|
| 3 | `CPAP_Obstructive` | `arg / 10.0` seconds |
| 4 | `CPAP_ClearAirway` | `arg / 10.0` seconds |
| 5 | `CPAP_Hypopnea` | 0 |
| 6 | `CPAP_Hypopnea` | 0 |
| 7 | `CPAP_VSnore` | 0 |
| 8 | `CPAP_FlowLimit` | 0 |

**Codes 5 and 6 are obstructive and central hypopnea.** OSCAR has no central
hypopnea channel, and both existing loaders that face this — BMC G3X
(`bmcG3xDataParsing.cpp`) and Prisma (`prisma_loader.cpp:414`) — fold both into
`CPAP_Hypopnea`. Sefam follows that precedent for consistency. See
`Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md` for the taxonomy, which records the
distinction even though OSCAR discards it. Revisiting this across all three
loaders, probably by adding a general central-hypopnea channel, is a known
deferred decision and is deliberately **not** attempted here — a Sefam-only
channel would make the three loaders inconsistent with each other.

**Hypopneas record zero duration.** The log argument is zero in 97% of code 5/6
records. The vendor reports 15–16 s averages, so the duration exists somewhere —
most likely `Y17` — but it is not in the log, and storing a nominal value would
put a fabricated number in the database as though it had been measured.

### Event placement — direction established, exact alignment unverified

**The log timestamp is the event END.** Two independent lines of evidence agree:
reading it as a start produces impossible overlaps across 362 consecutive event
pairs (zero overlaps when read as an end), and apnea flags placed at the raw
timestamp render visibly late against the manufacturer's own waveform report,
by an amount that varies with each event's duration.

**But flags are placed at the event START.** OSCAR draws a `FLAG` channel as a
bare vertical line at the event timestamp — `gFlagsLine`'s FLAG branch ignores
the stored duration entirely, surfacing it only in the tooltip. ResMed, the
reference loader, passes the EDF+ annotation *onset*, so OSCAR's convention is
flag-at-start. (`bmc_loader` passes `EndTime` and is the outlier here.) The
loader therefore subtracts the duration before adding the event.

Hypopneas have no duration to subtract. Left at the raw timestamp they would sit
about 15 s late on this patient's most frequent event type, so their flags are
shifted by the manufacturer's published mean durations — 16 s for OH, 15 s for
CH. This is a **display-placement heuristic, not measured data**; real durations
vary, so individual events are approximate in both directions rather than
uniformly late. The stored duration remains zero. Both constants should be
removed once per-event extent is recovered (open problem 1).

> **Not verified to the sample.** The correction's *direction* is well founded —
> apnea flags at the raw timestamp were observed rendering late by an amount that
> varied with each event's duration, which is exactly the signature of a
> flag-at-end placement. Whether they now land precisely right is unconfirmed:
> the manufacturer's waveform PDFs lack the resolution to compare against, and
> they cannot be zoomed.
>
> This is a general limitation rather than a SEFAM quirk. Devices commonly record
> event timestamps at a coarser resolution than the flow waveform, so a flag can
> only ever be placed to within that quantisation regardless of how the loader
> interprets the field. Treat exact flag alignment as an open item across
> loaders, not something this one can settle.

Codes 9–13 and the administrative codes are parsed but not imported. The loader
computes no AHI; `calcAHIGraph()` derives it from the flag channels.

## 8. Settings

Taken from `.LOG` code 2 where present, falling back to code 13:

```
CPAP_Mode         = MODE_APAP
CPAP_PressureMin  = payload byte 11 × 0.1
CPAP_PressureMax  = payload byte 15 × 0.1
CPAP_RampTime     = payload byte 12
CPAP_RampPressure = payload byte 16 × 0.1
SEFAM_HumidLevel  = payload byte 22          (code 2 only)
```

The comfort-level, patient-circuit and mask-leak candidates identified in the
format notes are **not** written: they are positional guesses that no card so
far can vary, and an unverified byte displayed as a therapy setting is worse
than showing nothing.

The humidifier level is written because it is no longer a guess — a second card
from the same device with only that setting changed moved byte 22 by exactly
the amount the setting moved, and nothing else. It goes to a SEFAM-specific
`SETTING` channel (`0xe500`) rather than `CPAP_HumidSetting`, because that
generic id resolves through `schema::channel["HumidSet"]` and no channel of that
name is ever registered — it is an empty channel, so anything written to it is
silently invisible. Registered `LOOKUP` with only `0 = Off` named; Daily prints
the raw number for any value with no option, so the numbered levels need no
entries and an out-of-range level still displays honestly.

Only code 2 carries it. A code 13 snapshot leaves the field at −1 and the
session then shows no humidifier level rather than an inherited one.

Mode is set to APAP because A-PAP is the only mode observed.

**Settings are carried forward.** This reverses an earlier decision in this
document ("a session with no settings record carries no settings rather than
inheriting a neighbour's"), which was made before measuring how often the device
writes the record. On the validated card only **4 of 31 sessions carry a code-2
record**, and the code-13 fallback never fires because both sessions containing
code 13 also contain code 2. The original rule would therefore have left 27 of
31 sessions with no settings displayed at all.

The settings did not change over the period — the manufacturer's report lists
identical values on every session row — so the device simply writes the record
occasionally rather than per session. The loader keeps the most recent settings
seen and applies them to subsequent sessions.

Directory names sort chronologically, so iteration order makes this safe even if
settings change mid-card: a session inherits only from a record written at or
before it, never from a later one. Sessions preceding the first settings record
still carry nothing. On the validated card the first session carries a code-2
record, so all 31 are covered.

## 9. Error handling

A malformed session is skipped; it is never fatal. Partial cards are normal on a
device that was unplugged mid-write, so a card with one bad directory must still
import the other thirty.

| Condition | Response |
|---|---|
| `.INI` missing or unparseable | Skip the directory |
| Header tag not `#03/` | Skip the directory |
| `(fileSize − headerLen) % recordSize != 0` | Skip that **channel**, keep the session |
| Checksum or sequence mismatch | Truncate the channel at the last good record, keep the session |
| Zero valid samples after sentinel splitting | Skip the session entirely |

The size-divisibility check is what catches a wrong assumed sample rate, so it
must not be silently tolerated. A torn final record from an interrupted write is
expected and is handled by truncation rather than rejection.

The log-only trailing directory present on the sample card (a session opened but
never recorded) is skipped by the `.INI` rule with no special-casing.

Failures go to `qWarning()` with directory and reason; one summary line at the
end if anything was skipped, not a dialog per failure. Unknown `.INI` channel
names are ignored silently — the schema belongs to the device, and a future model
declaring a channel we do not map is expected rather than erroneous.

Existing sessions are skipped via `mach->SessionExists(sessionID)` before any
file is read, making re-import idempotent and cheap.

`isAborted()` is checked at the top of the session loop. With ~30 sessions of
≤300 KB, worst-case cancellation latency is one session. Progress is
`setProgressMax(sessionCount)` / `setProgressValue(n)` with an `updateMessage`
per session.

## 10. Card backup

A `backupData(Machine*, const QString&)` method on the loader, called early in
`Open()`, following the Yuwell packaging. It is not a base-class virtual; each
loader defines and invokes its own.

```cpp
QDir ipath(path), bpath(mach->getBackupPath());
if (ipath == bpath) {                 // importing from the backup folder itself
    rebuild_from_backups = true;
    create_backups = false;
} else {
    rebuild_from_backups = false;
    create_backups = p_profile->session->backupCardData();
}
if (rebuild_from_backups || !create_backups) return true;
copyPath(ipath.absolutePath(), bpath.absolutePath(), true);
```

Compare `QDir` objects, not strings — separators differ on Windows.

**Compare the real source and destination, not the selected path against the
backup root.** `Detect()` accepts the card root, the model directory or the
serial directory, so a user can import from a path *nested inside* the backup
folder. A root-level comparison passes in that case, and `copyPath()` with
`overwrite = true` removes each destination file before copying — for a
self-copy that destroys the backup. The loader compares the resolved serial
directory against the computed destination instead.

**The backup reconstructs the card's `<modelcode>/<serial>/` layout** rather than
copying whatever the user selected, so the backup's shape does not depend on how
they navigated. This also keeps re-import working: `PeekInfo()` reads the model
code from the serial directory's parent, which would otherwise be `Backup`.

**The whole card is copied, including `.RAM`/`.BKP`.** ResMed backs up
selectively because its cards carry irrelevant bulk; a Sefam card is ~31 MB and
almost entirely relevant. The encrypted images are ~3.2 MB of that. They are not
read, but they are the only place the remaining unknowns (`Y17`, hypopnea
durations, log code 10) could ever be answered from, so a backup that discarded
them would destroy the one artefact a future investigation would need.

Because the copy preserves the `<model>/<serial>/DATA_nnn/` structure, `Detect()`
works unchanged on a backup folder and re-import from backup needs no special
path.

## 11. Verification

No unit tests — the project's QtTest harness is not used in this workflow.

**Golden numbers**, already confirmed against the vendor report, checkable by eye
on first import:

| Check | Expected |
|---|---|
| Sessions imported | 31 (trailing log-only directory skipped) |
| Total recorded time | 159.0 h |
| Event totals | OA 248, CA 191, Hypopnea 309, Snore 1408, FL 961 |
| Apnea mean duration | 16.5 s (OA), 12.8 s (CA) |
| Settings | min 4.0, max 20.0, ramp 45 min, ramp pressure 4.0 |
| AHI for the period | ≈ 4.7 |

**Three checks on the daily graph**, each targeting a specific design risk:

1. **Leak graph continuity** — gaps must be gaps. Spikes to 153 L/min mean
   sentinel splitting failed (§6).
2. **Event placement** against a vendor waveform PDF — resolves the
   start-versus-end question (§7).
3. **AHI ≈ 4.7** — catches event mapping or duration errors.

## 12. Known limitations

Carried forward deliberately, all documented in the format notes:

- Hypopneas have no duration.
- Central hypopnea is folded into `CPAP_Hypopnea`, discarding the device's
  obstructive/central split for hypopneas.
- Humidifier and comfort settings are not shown.
- Log code 10 (1.64/h) is unidentified and not imported.
- Event timestamps are assumed to be event ends pending visual confirmation.
- OSCAR's event counts will run slightly above the vendor's over long periods
  (~2% on the sample card). The vendor analyzer applies ramp and leak inclusion
  filtering that the raw log does not. This is a scoring-policy difference, not a
  defect, and must **not** be "fixed" by inventing filters to match.
