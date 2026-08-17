# Apex Medical XT Auto loader — design spec

**Status:** implemented; contributor-validated against private device data.
**Format reference:** `Notes/loaders/Apex/APEX_XT_AUTO_CARD_ANALYSIS.md` — the file
layout, ring-buffer arithmetic, and field encodings are decoded and validated
there. This document covers only how that format becomes an OSCAR loader.

---

## 1. Scope

Import the raw summary file (`00000000.APF`) and, when available, its optional
minute-detail file (`00000000.APE`) directly from the SD
card's `APAPDATA/00000000/` directory — the same path any other OSCAR loader
reads a device from, not a vendor application's save file. There is only one
known model, the XT Auto, so there is no per-model validation matrix the way
SEFAM has one.

Out of scope for this version: Easy Compliance's own `.paf` legacy project-file
format (a possible future addition for users who only have an old exported
project and not the raw SD card — no design work has been done for it here).

## 2. Files

| File | Purpose |
|---|---|
| `oscar/SleepLib/loader_plugins/apexDataParsing.h/.cpp` | Pure decode. No OSCAR types. |
| `oscar/SleepLib/loader_plugins/apex_loader.h/.cpp` | OSCAR integration. |

Both pairs added to `oscar.pro` (`SOURCES` and `HEADERS`). `main.cpp` gains the
`#include` and an `ApexLoader::Register();` call alongside the other loaders.

The split mirrors `sefamDataParsing.cpp` / `sefam_loader.cpp` (which itself
mirrors `bmcDataParsing.cpp` / `bmc_loader.cpp`) for the same reason: the format
is already specified and validated on paper (against real device data and the
vendor's own Easy Compliance CSV exports), so the risk here is transcription
error, not misunderstanding. A decode layer that takes bytes and returns plain
structs can be checked directly against known-correct numbers without
constructing a `Machine` or `Profile`.

## 3. Parser layer — `ApexParsing`

```cpp
namespace ApexParsing {

constexpr int    kApfRecordSize       = 29;
constexpr int    kApfTableStartOffset = 0x02;
constexpr quint8 kApfEmptySlotByte    = 0xFF;

constexpr int    kApeSize                = 0x5002;   // 20482
constexpr int    kApeSessionTableOffset  = 0x52;
constexpr int    kApeSessionTableEntries = 18;
constexpr int    kApeSessionEntrySize    = 8;
constexpr int    kApeRingStart           = 0x102;
constexpr int    kApeRingEnd             = kApeSize;
constexpr int    kApeMaxMinutes          = 1440;
constexpr quint8 kApeSessionMarker       = 0xFE;

struct ApfRecord {
    QDateTime start;
    QDateTime end;

    float initialPressure = 0.0f;   // cmH2O, raw/2.0
    float maxPressure     = 0.0f;   // cmH2O, raw/2.0
    float minPressure     = 0.0f;   // cmH2O, raw/2.0
    float averagePressure = 0.0f;   // cmH2O, raw/10.0
    float averageLeak     = 0.0f;   // LPM, raw session-average total leak

    quint8 rawMinPressure = 0, rawMaxPressure = 0;   // for exact-integer isApap(), not float compare

    bool isApap() const { return rawMinPressure != rawMaxPressure; }
};

struct ApeMinuteRecord {
    float pressure = 0.0f;   // cmH2O, raw/10.0 — same scale as ApfRecord::averagePressure
    quint8 apnea    = 0;      // event count in this minute, byte 2 high nibble
    quint8 hypopnea = 0;      // event count in this minute, byte 2 low nibble
    quint8 snoring  = 0;      // event count in this minute, byte 3 low nibble
};

struct ApeTableEntry {
    bool      markerOk = false;   // byte 0 == 0xFE
    QDateTime start;               // bytes 1-5
    quint16   cursor    = 0;       // bytes 6-7, little-endian
};

QDateTime decodeTimestamp5(const quint8 *bytes);
bool decodeApfRecord(const quint8 *record, ApfRecord &out);
bool parseApf(const QByteArray &data, QVector<ApfRecord> &out, QString &error);

ApeTableEntry decodeApeTableEntry(const quint8 *entry);
int ringAdvance(int offset, int count = 1);
QByteArray ringBytes(const QByteArray &apeData, int offset, int count);
bool decodeApeSessionRun(const QByteArray &apeData, quint16 cursorRaw,
                          QVector<ApeMinuteRecord> &out, QString &error);
bool parseApe(const QByteArray &apeData, QHash<QDateTime, QVector<ApeMinuteRecord>> &out);

}  // namespace ApexParsing
```

`decodeTimestamp5` is shared by `decodeApfRecord` and `decodeApeTableEntry`.
Both validate via `QDate`/`QTime` construction (rejecting out-of-range
month/day/hour/minute) rather than manual bounds checks — a record that is not
entirely `0xFF` but still decodes to an invalid date is treated as malformed,
not silently accepted with garbage fields.

**Ring arithmetic is centralised, never ad hoc.** `ringAdvance`/`ringBytes` wrap
offsets into `[kApeRingStart, kApeRingEnd)`; every byte read inside
`decodeApeSessionRun` — the `FE FE FE` check, the optional zero-prefix skip,
and each 4-byte minute record — goes through `ringBytes`, never a raw index
into the `QByteArray`. This is the highest-risk code in the whole loader (a
one-off error here silently corrupts every minute's data after the wrap point
rather than crashing), which is why it gets its own dedicated test coverage
before any OSCAR integration exists to obscure a bug (see §11).

`decodeApeSessionRun` control flow: wrap `cursor + 2` into
`[kApeRingStart, kApeRingEnd)` → require `FE FE FE` at that position → skip an
optional `00 00 00 00` block if present → read 4-byte minute records via
`ringBytes` until `FF FF FF` is found or `kApeMaxMinutes` records have been read
with no terminator. Both failure modes (missing marker and unterminated run)
return `false` with a diagnostic `error` string —
they represent a stale, already-overwritten table entry, not a corrupt file,
and the caller (`parseApe`) treats them as "this session has no detail",
never as a hard failure.

Minute-record byte 1 validates the three respiratory-event count nibbles: a
value in the seconds range 0–59 accepts the counts, while a value ≥60 suppresses
them as stale/non-event data. Pressure remains valid either way. This rule
matched every paired ground-truth session evaluated.

`parseApe` does **not** take `.APF` records as input. It is keyed purely by the
`.APE` session-table's own decoded timestamps; matching against `.APF` sessions
happens one layer up, in the loader (`apex_loader.cpp`), via a `QHash` lookup.
This keeps the decode layer testable in complete isolation from `.APF` parsing.

`parseApf`/`parseApe` return `false` only for structural failure (file too
short to contain even a header). A truncated or malformed tail within an
otherwise-valid file ends the result early rather than raising an error — an
empty-slot record stops `.APF` parsing at that point; a per-session failure
inside `.APE` simply omits that one session from the result hash.

## 4. Loader layer

```cpp
class ApexLoader : public CPAPLoader {
    bool Detect(const QString &path) override;
    int  Open(const QString &path) override;
    int  Version() override { return apex_data_version; }   // = 1
    const QString &loaderName() override;                   // "ApexLoader"
    MachineInfo PeekInfo(const QString &path) override;
    MachineInfo newInfo() override;                          // brand "Apex Medical", series "apex-xt-auto"
    bool backupData(Machine *mach, const QString &path);
    static void Register();

  protected:
    QString findDataDir(const QString &path);
    static void importSessionAverages(Session *session, const ApexParsing::ApfRecord &rec);
    static void importLeakChannel(Session *session, const ApexParsing::ApfRecord &rec,
                                  qint64 startMs, qint64 endMs);
    static void importMinuteDetail(Session *session, const ApexParsing::ApfRecord &rec,
                                   const QVector<ApexParsing::ApeMinuteRecord> &minutes,
                                   qint64 startMs);
};
```

No `initChannels()` override — every event maps to an existing OSCAR channel
(§7), so there are no custom channel IDs to register.

`findDataDir()` accepts the card root, `APAPDATA/`, or the `00000000/`
directory itself — walking up to two levels looking for a directory that
directly contains `00000000.APF` — so the entry point
is as forgiving as SEFAM's `findSerialDir()`.

`Detect()` layers three checks, cheapest first, matching SEFAM's precedent of
never trusting directory shape alone:
1. `findDataDir()` finds the `.APF` summary file.
2. Its size matches exactly (21,250 bytes) — strong but not sufficient on its
   own, since a coincidentally-sized unrelated file must not false-positive.
3. Positive content check: the first `.APF` record decodes successfully
   (in-range date fields).

`.APE` is deliberately not part of detection: it is an optional rolling-detail
file, and a missing, unreadable, or malformed copy falls back to `.APF` summary
import rather than hiding otherwise usable sessions.

An all-empty card (zero sessions ever recorded) makes step 3 fail on the
`.APF` side, and `Detect()` correctly declines — there is nothing importable on
such a card regardless.

**Machine identity.** Brand is `"Apex Medical"`, model and model number are the
fixed string `"XT Auto"` — there is only one supported model and the raw
format carries no model/firmware field to read instead. `series` is the fixed
string `"apex-xt-auto"`, matching `oscar/icons/apex-xt-auto.png` (already
present in the working tree, wired into `Resources.qrc` and the loader
constructor's `m_pixmap_paths`/`m_pixmaps`, following `BmcLoader`'s pattern in
`bmc_loader.cpp:257-264`). **`serial` is left blank** — unlike SEFAM, nothing in
either raw file identifies an individual device. This means OSCAR cannot tell
two different physical XT Auto units apart if a user somehow imported from
both into the same profile; that is a real, accepted limitation of the device
data itself, not a loader shortcut (see §12).

## 5. Session assembly and time handling

One `.APF` record → one OSCAR `Session`. `SessionID` is
`rec.start.toSecsSinceEpoch()` on a `Qt::LocalTime`-spec `QDateTime` — the only
scheme available, not a fallback, since **no UTC epoch or timezone offset
exists anywhere in this format** (unlike SEFAM, which has a stored UTC epoch
and only falls back to local time for its short-header variant). Session start
uses the device's local wall-clock fields directly, so a 22:03 bedtime displays
as 22:03 regardless of the importing machine's timezone — consistent with
every other loader's handling of naive device timestamps.

**Events are placed at absolute minute boundaries**, needing no relative-offset
arithmetic: `eventTimeMs = startMs + minuteIndex × 60000`. This is simpler than
SEFAM's event-placement problem (§7 of that design doc) because `.APE` data has
no separate "log timestamp vs. session start" relationship to reconcile — each
minute record's position in the decoded array *is* its timestamp offset,
directly.

A session whose `.APF` record decodes but whose end time is not after its
start time (both zero, or corrupted) is skipped — it cannot represent real
therapy time.

## 6. Waveforms and settings scope

Only one measured time-series waveform is available: **`CPAP_Pressure`**, one sample per minute
(`rate = 60000 ms`), stored as `qint16(round(pressure_cmH2O × 10))` with
`gain = 0.1`, `offset = 0`. This is the device's actual resolution — there is
no higher-rate pressure signal anywhere in either file, and no flow or
time-varying leak waveform at all.

**`offset` is always 0.** OSCAR applies `EventList::offset` inconsistently —
`gLineChart` renders `(raw + offset) × gain`, but `EventList::data()` and
`FlowParser` apply gain alone and ignore offset (documented precedent in
`SEFAM_LOADER_DESIGN.md` §6). Pressure has no nonzero baseline requiring a
bake-in here, so this is trivially satisfied, but it is called out explicitly
so a future reviewer does not wonder whether it was considered.

**`.APF` Average Leak is imported as session-average total leak.** The raw
value is written to `CPAP_LeakTotal` at both session boundaries, producing a
flat summary trace. This deliberately communicates the only resolution the
device provides and does not imply measured variation within the session.

OSCAR's existing `calcLeaks()` path derives `CPAP_Leak` from total leak and
`CPAP_Pressure` using the user's configured intentional mask-leak curve. The
pressure channel contains decoded minute pressure when detail is available,
or a flat APF average-pressure trace when it is not. The loader does not create
a separate `CPAP_PressureSet` channel because Apex does not record one.
`CPAP_LeakTotal` remains available even when the user disables derived
unintentional-leak calculation.

**No ramp settings are imported.** `.APF` byte 0x12 ("possibly Ramp Time") is
always observed as `0x00` across all real sample data and its status is
unverified (`APEX_XT_AUTO_CARD_ANALYSIS.md` §2) — an unverified byte displayed
as a therapy setting is worse than showing nothing, same reasoning SEFAM
applied to its own unverified humidifier/comfort candidates.

## 7. Events

| `.APE` minute-record field | OSCAR channel | Event value |
|---|---|---|
| Apnea nibble (byte 2, high) | `CPAP_Apnea` | 0 |
| Hypopnea nibble (byte 2, low) | `CPAP_Hypopnea` | 0 |
| Snoring nibble (byte 3, low) | `CPAP_VSnore` | 0 |

**Apnea maps to `CPAP_Apnea` (unclassified), not `CPAP_Obstructive`.** The
device's own data never distinguishes obstructive from central apnea — there
is a single combined marker. `CPAP_Apnea` is OSCAR's existing channel for
exactly this case (already included in `ahiChannels`, so it contributes to AHI
correctly with no further wiring), and mapping it to `CPAP_Obstructive` would
misrepresent data the device itself does not claim.

**All events record zero duration**, because none exists in the source data —
a minute record carries a count, not start/end pairs. This is the same
"do not fabricate a duration" principle SEFAM applied to its own
duration-less hypopnea codes, except here it applies to every event type, not
just one.

**One `AddEvent` per nibble count**, at that minute's absolute timestamp
(`startMs + minuteIndex × 60000`). A count above one produces repeated markers
at the same minute boundary, preserving the manufacturer's event totals even
though their distinct sub-minute timestamps are unavailable. This is not one
`EventList` split into contiguous runs the way SEFAM splits around its `0xFF`
sentinel, because every
minute in a decoded `.APE` run is real data; there is no invalid/sentinel value
to split around at this granularity. Each of `CPAP_Apnea`/`CPAP_Hypopnea`/
`CPAP_VSnore`'s `EventList` is created lazily, on the first flagged minute for
that channel, so a session with e.g. zero apnea minutes gets no empty
`CPAP_Apnea` `EventList` at all.

**This is coarser than every other OSCAR-supported device**: one marker per
minute with no sub-minute timing, versus a discrete timestamped event log
elsewhere. Multiple real events inside one minute retain their count but share
the same approximate timestamp. This is a firmware/hardware limitation of the
device's on-card storage, not an import shortcut — documented explicitly here and in §12 so it
is never mistaken for a bug later.

The loader computes no AHI; `Session::UpdateSummaries()` / `calcAHIGraph()`
derive it from the flag channels, same as every other loader.

## 8. Settings

```
if APAP:
    CPAP_Mode        = MODE_APAP
    CPAP_PressureMin = rec.minPressure
    CPAP_PressureMax = rec.maxPressure
else:
    CPAP_Mode        = MODE_CPAP
    CPAP_Pressure    = rec.minPressure
```

`isApap()` compares the **raw pressure bytes** (`rawMinPressure != rawMaxPressure`),
not the scaled floats — both are always exact `raw/2.0` half-steps so float
equality would also work reliably here, but comparing the underlying integers
sidesteps float-equality concerns entirely and costs nothing.

No other settings are written. `CPAP_RampTime`/`CPAP_RampPressure` are
intentionally absent (§6). Settings are per-session, taken directly from that
session's own `.APF` record — there is no cross-session forward-fill needed
the way SEFAM's sparse settings-log required, because every `.APF` record
independently carries its own Initial/Max/Min pressure fields.

## 9. Error handling

A malformed session is skipped; it is never fatal — a card is 21,250 + 20,482
bytes total, so partial-write corruption from an interrupted device connection
is plausible and must not abort the whole import.

| Condition | Response |
|---|---|
| `.APF` shorter than one header + one record | `parseApf` returns `false`, `Open()` logs and imports nothing |
| `.APF` record is the all-`0xFF` empty-slot sentinel | Table parsing stops there; earlier records are still imported |
| `.APF` record's timestamp fields are out of range | `decodeApfRecord` returns `false`; `parseApf` stops the table there (same as an empty-slot sentinel — an unparseable record cannot be trusted to be followed by valid ones) |
| `.APE` missing, unreadable, or wrong size | `Open()` proceeds with `.APF`-only summary import for every session; no error, no partial detail |
| `.APE` session-table entry has a bad marker, a cursor whose wrapped location has no start marker, or no terminator within 1,440 minutes | That one session gets no minute detail; its APF settings and session-average channels remain available, and every other session is unaffected |
| Session end time not after start time | Session skipped entirely |

Existing sessions are skipped via `mach->SessionExists(sessionID)` before any
per-session work happens, making re-import idempotent and cheap — same
mechanism every other loader uses.

`isAborted()` is checked once per `.APF` record in the main import loop. With
at most ~732 sessions of a few hundred bytes each, worst-case cancellation
latency is a small fraction of a second.

Progress is `setProgressMax(records.size())` / `setProgressValue(n)` with one
`updateMessage` at the start of the run, not per session (there is nothing
slow enough per session to warrant finer-grained messaging — the entire card
is 42 KB).

## 10. Card backup

A `backupData(Machine*, const QString&)` method, called early in `Open()`,
structurally identical to `SefamLoader::backupData()`:

```cpp
const QString dataDir = findDataDir(path);
QDir src(dataDir);
QDir dst(QDir(mach->getBackupPath()).absoluteFilePath("APAPDATA/00000000"));

if (src == dst) {                      // importing from the backup folder itself
    rebuild_from_backups = true;
    create_backups = false;
} else {
    rebuild_from_backups = false;
    create_backups = p_profile->session->backupCardData();
}
if (rebuild_from_backups || !create_backups) return true;
copyPath(src.absolutePath(), dst.absolutePath(), true);
```

Compare `QDir` objects, not strings — separators differ on Windows (same
precedent as SEFAM).

**The backup reconstructs the card's `APAPDATA/00000000/` layout** rather than
copying whatever the user selected, exactly like SEFAM reconstructs
`<modelcode>/<serial>/` — so the backup's shape does not depend on whether the
user pointed `Detect()` at the card root, `APAPDATA/`, or the `00000000/`
directory itself, and re-import from the backup folder works unchanged.

**The whole dataset is copied** — trivially, since both files together are only
~42 KB, there is no space/time argument for selective backup the way ResMed's
selective backup exists to skip an SD card's irrelevant bulk.

## 11. Verification

**Decode-layer unit tests** (`oscar/tests/apextests.cpp`, QtTest) cover the
highest-risk logic — `.APE` ring-buffer arithmetic — in isolation, before any
OSCAR integration exists to obscure a bug: basic `.APF` record decode
(including `isApap()`), empty-slot table truncation, a full ~732-record
capacity table, ring-offset wraparound, basic `.APE` session-run decode
(pressure + nibble unpacking), wraparound across the ring end, the optional
zero-prefix present/absent, cursor-offset wrapping, and stale-entry failure
modes (missing marker and unterminated run) each returning
`false` without `parseApe` treating it as fatal. A synthetic loader-level test
also verifies total-leak boundary values, detailed pressure support, and the
average-pressure fallback without using patient data.

**Contributor-reported end-to-end validation against anonymized paired sample
data** covered
multiple snapshots spanning the available history. Some snapshots contain
fewer decodable detail entries than the table capacity because stale or
overwritten entries are expected; this is the per-entry fallback described in
§3, not a loss of summary sessions.

| Check | Expected | Result |
|---|---|---|
| Sessions decoded | matches the paired export's session rows | **Pass:** every snapshot matched |
| Per-session start and mode | matches the paired export per row | **Pass:** every compared row matched |
| Decoded initial/max/min/average pressure and average leak | matches the corresponding paired export fields | **Pass:** every compared row matched |
| Apnea/Hypopnea/Vibratory Snoring totals in decodable detail sessions | matches the paired export exactly | **Pass:** every paired detail session matched |
| Sessions beyond the detail-table window | no minute respiratory detail (expected, not a defect) | **Pass:** every session was retained; only decodable entries received minute detail |
| Re-import idempotency | session count does not change | **Pass by code path:** stable start-time IDs are checked with `SessionExists()` before allocation |
| Backup round-trip | `Detect()`/`Open()` against the backup folder itself imports correctly, no self-copy loop | **Pass:** manually confirmed by the user |

The OSCAR project does not contain Apex patient samples, so these end-to-end
results and the backup round-trip have not been independently reproduced by
the OSCAR team. In-project automated coverage uses synthetic fixtures only.

Easy Compliance's `Duration` column is utilization-oriented rather than the
raw wall-clock span between the APF start/end fields. Therapy-utilization gaps
and daylight-saving transitions can therefore produce expected differences.
OSCAR deliberately uses the APF start and end timestamps as documented in §5;
the export duration is not treated as an end-time oracle. Both timestamp fields
decode consistently, and Qt local-time epoch conversion handles DST cases.

## 12. Known limitations

All are inherent to the device's on-card data, not loader shortcuts:

- **Minute-level-only granularity.** Apnea/Hypopnea/Vibratory-Snoring events
  carry no duration and no sub-minute timing; multiple real events within one
  minute retain their count but share the same approximate timestamp. Pressure
  is one sample per minute, far coarser than devices logging at ~1 sample/5–10 s.
- **No time-varying leak waveform.** Leak graphs and statistics use the one
  session-average total-leak value exposed by the device. The flat trace must
  not be interpreted as measured minute-level variation.
- **No ramp settings.** The candidate byte is unverified and always zero in
  every real sample seen.
- **Sessions beyond the most recent ≤18 have no minute detail forever** — a
  hard ring-buffer capacity limit of the device. Their settings and
  session-average pressure/leak channels remain available.
- **No obstructive/central apnea distinction** — the device itself does not
  record one.
- **No serial number or firmware version anywhere in the raw format** — OSCAR
  cannot distinguish two physical XT Auto units by device identity alone.
- **OSCAR reports wall-clock session hours.** `.APF` byte 0x0B
  ("Duration − Util") is unreliable on newer records and is not used; session
  duration is computed from start/end timestamps instead. Sessions containing
  therapy pauses can therefore show more hours than Easy Compliance's
  utilization-oriented duration.
- **8 fully-uncharacterized `.APF` bytes** (0x15–0x1C) are retained, and the
  unused `.APE` minute-record byte 3 high nibble is ignored, never surfaced.
- **No `.paf` (Easy Compliance legacy project file) support** — out of scope
  for this version.
- **No checksum or CRC anywhere in the format** — corruption cannot be
  distinguished from a legitimately short or empty card beyond the structural
  checks already described in §9.
