# Apex XT Auto loader — code review

**Reviewed:** commit `3e3c54c0` "Add Apex XT format support"
**Date:** 2026-08-08
**Scope:** `oscar/SleepLib/loader_plugins/apex_loader.{h,cpp}`,
`oscar/SleepLib/loader_plugins/apexDataParsing.{h,cpp}`,
`oscar/tests/apextests.{h,cpp}`, the three notes under `Notes/loaders/Apex/`,
and the integration edits to `main.cpp`, `oscar.pro`, `Resources.qrc`.

**Method:** static review only. The loader was not run and no Apex sample data
was available. Every finding below was traced through the OSCAR code it depends
on (`Day`, `Session`, `EventList`, `Machine`, `gLineChart`); line references are
to the reviewed commit.

> **Follow-up:** the response to this review is reviewed in
> [Follow-up review](#follow-up-review--commit-785f8606) at the end of this
> document. Twelve of the fourteen findings below were resolved there; the fix
> for finding 3 introduced a regression (F1), since fixed. Finding 13, the
> release-notes entry, has since been written.

---

## Summary

| # | Severity | Area | Finding |
|---|---|---|---|
| 1 | High | Settings | Fixed-pressure sessions never set `CPAP_Pressure`; Daily shows "Fixed n/a", Welcome prints `-3.4e+38` |
| 2 | Medium | Detection | The `.APE`-optional import path is unreachable, and detection is stricter than the parser |
| 3 | Medium | Session extent | Minute detail can run past the session end; waveform duration disagrees with its own samples |
| 4 | Low | Logging | `findDataDir()` logs on every miss, during a 20-second all-drives scan |
| 5 | Low | Efficiency | `findDataDir()` runs three times per `Open()`; `Detect()` re-reads both files |
| 6 | Low | Decode | Optional `00 00 00 00` prefix is ambiguous with a real first minute record |
| 7 | Low | Decode | `cursor + 2` is range-checked instead of wrapped — the one place ring arithmetic is not centralised |
| 8 | Low | Decode | Duplicate `.APE` start timestamps collapse silently |
| 9 | Low | Qt style | Deprecated `QDateTime(QDate, QTime, Qt::TimeSpec)` (deprecated since Qt 6.9) |
| 10 | Low | Redundancy | `setSummaryOnly(false)` is already the default |
| 11 | Low | Style | `Q_OBJECT` is not the first entry in the class body |
| 12 | Docs | Design note | §11 presents contributor validation as project validation; §9 does not match the code |
| 13 | Docs | Release notes | No Apex entry in `Htmldocs/release_notes.html` |
| 14 | Docs | Expectations | OSCAR reports wall-clock hours where the vendor software reports utilization |

---

## 1. (High) Fixed-pressure sessions never set `CPAP_Pressure`

`apex_loader.cpp:356-358` writes only:

```cpp
session->settings[CPAP_Mode] = rec.isApap() ? MODE_APAP : MODE_CPAP;
session->settings[CPAP_PressureMin] = rec.minPressure;
session->settings[CPAP_PressureMax] = rec.maxPressure;
```

When the mode is `MODE_CPAP`, OSCAR reads the *fixed* pressure setting, not
min/max:

* `day.cpp:1629-1632` — `Day::getPressureSettings()` returns
  `Fixed <settings_min(CPAP_Pressure)>`. `Day::settings_min()`
  (`day.cpp:282-297`) returns `std::numeric_limits<EventDataType>::max()` when
  the key is absent, which `Day::validPressure()` (`day.cpp:1612-1617`) renders
  as `"n/a"`. Daily and Statistics therefore display
  **`Fixed n/a (cmH2O)`**.
* `welcome.cpp:281-287` — uses `settings_max(CPAP_Pressure)` and does **not**
  pass it through `validPressure()`, so the Welcome page prints
  **`Your CPAP device used a constant -3.40282e+38 cmH2O of air`**.

Every other loader sets this for fixed-pressure modes: `resmed_loader.cpp:2872`,
`prs1_loader.cpp:1917`, `bmc_loader.cpp:277`, `icon_loader.cpp:856`,
`yuwell_loader.cpp:290`, `prisma_loader.cpp:343`, `resvent_loader.cpp:701`.

**Suggested fix** — in `ApexLoader::Open()`:

```cpp
session->settings[CPAP_Mode] = rec.isApap() ? MODE_APAP : MODE_CPAP;
if (rec.isApap()) {
    session->settings[CPAP_PressureMin] = rec.minPressure;
    session->settings[CPAP_PressureMax] = rec.maxPressure;
} else {
    session->settings[CPAP_Pressure] = rec.minPressure;   // == maxPressure
}
```

Keeping min/max on a CPAP-mode session is harmless (nothing reads them in that
mode), so writing all three is also acceptable if that is preferred; the
essential part is `CPAP_Pressure`.

This only affects an APAP-capable device running in fixed mode, which is why it
may not appear in the data the loader was developed against.

---

## 2. (Medium) The `.APE`-optional import path is unreachable, and detection is stricter than the parser

`APEX_LOADER_DESIGN.md` §9 states:

> `.APE` missing, unreadable, or wrong size → `Open()` proceeds with `.APF`-only
> summary import for every session; no error, no partial detail

`apex_loader.cpp:325-329` implements exactly that fallback. It cannot be
reached:

* `Open()` calls `Detect(path)` first (`apex_loader.cpp:304`).
* `hasApexFiles()` requires **both** `00000000.APF` and `00000000.APE` to exist
  (`apex_loader.cpp:33-37`), and `findDataDir()` returns empty otherwise
  (`:99, :105, :117`).
* `Detect()` then requires `.APE` to be exactly `kApeSize` bytes
  (`apex_loader.cpp:134`) and to read back at that size (`:151-154`).

The only route into the fallback is a file changing on disk between `Detect()`
and `Open()` — a removed card, not the documented scenario.

A second, sharper aspect of the same problem: `Detect()` requires
**session-table entry 0 specifically** to carry the `0xFE` marker
(`apex_loader.cpp:155-156`):

```cpp
const quint8 firstMarker = static_cast<quint8>(ape.at(ApexParsing::kApeSessionTableOffset));
if (firstMarker != ApexParsing::kApeSessionMarker) { return false; }
```

That is stricter than `parseApe()`, whose entire design is that individual
entries may be stale or unused and are skipped one at a time
(`apexDataParsing.cpp:191-204`, and `APEX_XT_AUTO_CARD_ANALYSIS.md` §3, which
lists "marker byte mismatch" as a normal stale condition). A card whose first
ring slot is not live is rejected outright, discarding a perfectly good `.APF`.

**Suggested fix** — either bring the code in line with the design:

* let `findDataDir()` key on `.APF` alone and treat `.APE` as optional;
* in `Detect()`, apply the `.APE` size and marker checks only when the file is
  present, and accept **any** of the 18 table entries carrying the marker rather
  than entry 0;

…or, if `.APE` really should be mandatory, delete the dead fallback at
`apex_loader.cpp:325-329` and correct §9 of the design note. The first option is
preferable: the `.APF` alone yields settings plus session-average pressure and
leak, which is worth importing.

---

## 3. (Medium) Minute detail can run past the session end; waveform duration disagrees with its own samples

`apex_loader.cpp:258-263`:

```cpp
EventList *pressureEvents = session->AddEventList(
    CPAP_Pressure, EVL_Waveform, 0.1f, 0.0f, 0.0f, 0.0f, kMinuteMs);
const qint64 sampleDuration = static_cast<qint64>(pressure.size()) * kMinuteMs;
const qint64 sessionDuration = rec.end.toMSecsSinceEpoch() - startMs;
pressureEvents->AddWaveform(startMs, pressure.data(), pressure.size(),
                            qMax(sampleDuration, sessionDuration));
```

Sample positions come from the **rate**, not the duration —
`EventList::time()` returns `m_first + i * m_rate` (`event.cpp:48-55`), and
`gLineChart` reads `el.rate()` for the sample step (`gLineChart.cpp:646`). The
`duration` argument only sets `m_last` (`event.cpp:143-159`). So when
`sessionDuration` is the larger of the two, `EventList::last()` claims data
past the final sample.

The opposite case matters more. If a decoded run holds more minutes than the
APF start/end span covers, both the waveform and the per-minute events at
`startMs + i * 60000` (`apex_loader.cpp:272`) land after the session's end.
Nothing corrects this: `really_set_first()` / `really_set_last()`
(`apex_loader.cpp:353-354`) are the only writers of `s_first`/`s_last` outside
database load (`session.cpp:70`, `session.cpp:3093-3094`), and
`Session::UpdateSummaries()` does not adjust them (`session.cpp:1328-1388`).
Those events still contribute to AHI but fall outside the session's displayed
span.

**Suggested fix** — pass the honest sample duration and let the session grow to
fit the data:

```cpp
pressureEvents->AddWaveform(startMs, pressure.data(), pressure.size(), sampleDuration);
```

and, in `Open()` after the detail import, either

```cpp
session->really_set_last(qMax(rec.end.toMSecsSinceEpoch(),
                              startMs + qint64(minutes.size()) * 60000));
```

or, if the APF timestamps are to be treated as authoritative, truncate the
minute run to the session span and `qWarning()` when that happens — so a
mismatch is visible rather than silent. Either is defensible; the current code
does neither.

---

## 4. (Low) `findDataDir()` logs on every miss

`apex_loader.cpp:121-122`:

```cpp
qDebug() << "ApexLoader::findDataDir found no Apex file pair beneath"
         << selected.absolutePath();
```

`MainWindow::detectCPAPCards()` (`mainwindow.cpp:1259-1266`) runs every
registered loader against every drive in a loop that repeats roughly once a
second for up to 20 seconds. Every user who imports from any other device will
see dozens of these lines per import. `SefamLoader::findSerialDir()` returns
quietly for the same reason. Drop the message, or move it into `Detect()`'s
positive path only.

---

## 5. (Low) Repeated directory walks and file reads

`Open()` resolves `findDataDir()` three times — once inside `Detect()`
(`apex_loader.cpp:304` → `:128`), once inside `backupData()` (`:310` → `:171`),
and once directly (`:312`) — and `Detect()` reads both files in full. At 42 KB
total this costs nothing measurable, but caching the resolved path in a member
(or passing it down) would remove three directory enumerations per import.

---

## 6. (Low) The optional zero prefix is ambiguous

`apexDataParsing.cpp:143-150` skips a 4-byte `00 00 00 00` block if it follows
the `FE FE FE` marker. A legitimate first minute record of `00 00 00 00`
(raw pressure 0, no events, second field 0) is byte-for-byte identical. If that
ever occurs, the record is consumed as padding and **every** minute in that
session shifts one minute earlier, silently — pressure trace and event
timestamps alike.

The probability is low: a session's first sample should be at the initial
pressure (`0x0F`, typically raw 40 = 4.0 cmH2O), not zero. This is an inherent
ambiguity in the format rather than a coding error, but it is worth an explicit
comment at that site so a future reader does not assume the skip is certain.

---

## 7. (Low) `cursor + 2` is range-checked rather than wrapped

`apexDataParsing.cpp:125-129`:

```cpp
const int startPos = static_cast<int>(cursorRaw) + 2;
if (startPos < kApeRingStart || startPos >= kApeRingEnd) {
    error = QStringLiteral("Apex .APE: session cursor out of ring range - stale table entry");
    return false;
}
```

This is the one place in the decoder that does not go through `ringAdvance()`.
Under the ring model, a cursor of `0x5000` or `0x5001` should wrap to `0x102` /
`0x103`; here it is declared stale instead. Two of 20,480 possible positions, so
the practical impact is negligible, and `APEX_XT_AUTO_CARD_ANALYSIS.md` §3
describes an out-of-range cursor as a stale marker — flagged only because
`APEX_LOADER_DESIGN.md` §3 states that ring arithmetic is "centralised, never
ad hoc", and this is the exception.

---

## 8. (Low) Duplicate `.APE` start timestamps collapse silently

`apexDataParsing.cpp:203` uses `out.insert(entry.start, minutes)`. Two live
table entries sharing a start timestamp would leave only the later one, with no
diagnostic. Not expected in real data, but a `qWarning()` on collision would
make the assumption testable.

---

## 9. (Low) Deprecated `QDateTime` constructor

`apexDataParsing.cpp:30` uses `QDateTime(date, time, Qt::LocalTime)`, deprecated
since Qt 6.9 (`qdatetime.h:361-362`,
`QT_DEPRECATED_VERSION_X_6_9("Pass QTimeZone instead")`). Seven further uses in
`apextests.cpp` (lines 135, 136, 190, 359, 360, 387, 388).

This compiles today: `oscar.pro`'s generated `CXXFLAGS` include `-w`, which
suppresses the warning that `-Werror` would otherwise promote, and the same
constructor already appears in `machine.cpp:273`, `bmcDataParsing.cpp:24` and
`profile_backup.cpp:1197`. It is a forward-compatibility and house-style point
only — new code should prefer `QDateTime(date, time, QTimeZone::LocalTime)`.

Behaviourally the deprecated form is fine here: it resolves DST transitions with
`TransitionResolution::LegacyBehavior` (`qdatetime.cpp:4003-4006`), which
returns a *valid* datetime for a spring-forward gap rather than an invalid one.
That matters because `decodeApfRecord()` (`apexDataParsing.cpp:43`) aborts the
whole `.APF` table on an invalid timestamp — an invalid result there would have
truncated every later session. It does not, so there is no bug; noted so the
reasoning is on record if the constructor is ever changed.

---

## 10. (Low) Redundant `setSummaryOnly(false)`

`apex_loader.cpp:222` and `:244`. `Session`'s constructor already sets
`s_noSettings = s_summaryOnly = false` (`session.cpp:73`). Harmless and arguably
self-documenting; noted only for completeness.

---

## 11. (Low) `Q_OBJECT` placement

`apex_loader.h:29-33` places the `UNITTEST_MODE` `friend` declaration before
`Q_OBJECT`. moc handles this correctly, but every other loader in the tree puts
`Q_OBJECT` first.

---

## 12. (Docs) `APEX_LOADER_DESIGN.md` §11

The section is headed **"Status: implemented and validated"** and carries an
all-Pass verification table, including "**Pass:** manually confirmed by the
user" for the backup round-trip and "every snapshot matched" for end-to-end
validation against paired sample data.

That validation was performed by the contributor against their own device data
and the companion `apex-xt-oscar-convert` project. No Apex sample data exists in
this project, and none of it has been reproduced here. Recommend re-labelling
the table as contributor-reported so it is not later read as OSCAR-team
verification, and adding a line stating what remains unverified in-project.

§9's row for a missing or wrong-size `.APE` does not describe the code's actual
behaviour — see finding 2.

---

## 13. (Docs) Release notes

`Htmldocs/release_notes.html:20` carries the SEFAM entry for the v2.0.2 draft
but has no Apex entry. Release notes are author-written, so this is a reminder
rather than a proposed edit.

---

## 14. (Docs) Usage-hours expectation

The loader creates no mask-on/mask-off slices, so `Session::hours()` falls back
to the full APF start→end wall-clock span. Any mid-session pause therefore
counts as therapy time. `APEX_LOADER_DESIGN.md` §11 already notes that Easy
Compliance's `Duration` column is utilization-oriented; the practical
consequence is that OSCAR will report **more** hours than the vendor software
for sessions containing pauses. Worth stating in §12 (Known limitations) so it
is not later reported as a bug. The `.APF` byte `0x0B` ("Duration − Util") is
the only on-card hint at utilization and is deliberately unused because it is
unreliable on newer records.

---

## What holds up well

Recorded so a later reader does not re-derive it:

* **Decode/loader split.** `ApexParsing` contains no OSCAR types and is testable
  in isolation, mirroring `sefamDataParsing` / `bmcDataParsing`.
* **Ring arithmetic.** `ringAdvance()` (`apexDataParsing.cpp:93-101`) normalises
  correctly for negative offsets and multi-lap counts; the double-modulo is
  right. Aside from finding 7, every ring read goes through `ringBytes()`.
* **Tests.** `apextests.cpp` builds its fixtures with the same `ringAdvance()`
  the decoder uses, so the helper and the code under test agree on ring geometry
  by construction, and it covers the wrap-across-ring-end case explicitly.
  Registration via `DECLARE_TEST` in `apextests.h` plus the `oscar.pro` `test {}`
  additions is correct. Mixed-type `QCOMPARE(out.size(), 2)` compiles on Qt 6.10
  via the integer-comparison branch of `QTest::qCompare`.
* **Detection layering.** Cheap checks first — directory shape, then exact file
  sizes, then a content check — so the 20-second auto-scan does not read files
  on non-Apex drives.
* **Backup.** `backupData()` compares `QDir` objects rather than strings for the
  self-copy guard (`apex_loader.cpp:180`), and reconstructs
  `APAPDATA/00000000` under the backup root, so re-import from the backup
  folder resolves through the same `findDataDir()` path.
* **Empty serial is safe.** `Profile::CreateMachine()` keys
  `MachineList[loadername][serial]` (`profiles.cpp:1185-1190, 1252`); an empty
  serial is a valid hash key, so repeated imports resolve to the same machine
  rather than creating duplicates.
* **Session cleanup.** `delete session` after a failed `AddSession()`
  (`apex_loader.cpp:370`) is safe — `Machine::AddSession()` returns false before
  taking ownership on every rejection path (`machine.cpp:296-330`).
* **AHI wiring.** `CPAP_Apnea` is registered in `ahiChannels`
  (`schema.cpp:425`), so mapping the device's undifferentiated apnea marker
  there contributes to AHI without further wiring, as §7 of the design claims.
* **Machine identity and icon.** The constructor's `m_pixmap_paths` /
  `m_pixmaps` population (`apex_loader.cpp:63-70`) matches `BmcLoader`
  (`bmc_loader.cpp:254-262`), and `ApexLoader::Register()` runs from `main()`
  after the `QApplication` exists, so constructing the `QPixmap` there is valid.
* **Build integration.** Sources and headers are in both the main and `test {}`
  blocks of `oscar.pro`, the icon is in `Resources.qrc`, and `main.cpp`
  registers the loader alongside the others. Object files for both new
  translation units are present in the MinGW build tree, so it compiles.

---
---

# Follow-up review — commit `785f8606`

**Reviewed:** commit `785f8606` "Address Apex loader code review", the response
to everything above. Reached this project as a merge request; the branch is two
commits on top of master, the second being this one.
**Date:** 2026-08-09
**Method:** as before — static review, no Apex sample data. Line references in
this section are to `785f8606`.

## Disposition of the original findings

| # | Severity | Status |
|---|---|---|
| 1 | High | **Fixed.** Extracted into `importSettings()`; covered by a new test |
| 2 | Medium | **Fixed.** The first of the two suggested options — `.APE` is now genuinely optional |
| 3 | Medium | **Fixed**, but the fix introduced a regression — see F1 |
| 4 | Low | **Fixed.** The per-miss `qDebug()` is gone |
| 5 | Low | **Fixed.** `findDataDir()` now runs once per `Open()` |
| 6 | Low | **Fixed.** The ambiguity is documented at the decode site |
| 7 | Low | **Fixed.** Now `ringAdvance(cursorRaw, 2)` |
| 8 | Low | **Fixed.** `qWarning()` on a duplicate key |
| 9 | Low | **Fixed** in the loader and at all seven test sites |
| 10 | Low | **Fixed** |
| 11 | Low | **Fixed** |
| 12 | Docs | **Fixed.** Status re-labelled contributor-validated, §9 rewritten to match the code, and an explicit not-reproduced-in-project paragraph added |
| 13 | Docs | **Done since.** The Apex entry was added to `Htmldocs/release_notes.html` for the v2.0.2 draft |
| 14 | Docs | **Fixed.** The wall-clock-hours limitation is now in §12 |

The supporting refactor is sound. `validateDataDir()` and `backupDataDir()`
correctly separate "resolve the path" from "work on the resolved path", which is
what removes the repeated directory walks, and `Open()` no longer re-reads the
`.APF` it already validated.

---

## F1. (Medium) Derived unintentional leak collapses to a single point

The finding 3 fix passes the honest `sampleDuration` to `AddWaveform()`
(`apex_loader.cpp:269`), so the pressure waveform's `last()` is
`startMs + n * 60000`. The total-leak trace is still built across the full
session span (`apex_loader.cpp:254`):

```cpp
importLeakChannel(session, rec, startMs, rec.end.toMSecsSinceEpoch());
```

`calcLeaks()` (`calcs.cpp:1334`) resolves a pressure value for each
`CPAP_LeakTotal` sample through `TimeSeries::valueAt()`, and
`TimeSeries::findEventListContaining()` (`calcs.cpp:1256-1264`) accepts a
timestamp only when `eventlist->first() <= time && time <= eventlist->last()`.
A leak sample beyond the final minute sample produces no derived event at all.

The `.APF` timestamps carry no seconds field — `decodeTimestamp5()` builds
`QTime(hour, minute, 0)` (`apexDataParsing.cpp:29`) — so a session span is
always a whole number of minutes, and the shortfall is not a rounding effect.
The device simply writes fewer minute records than the span it reports. Decoded
across a real XT Auto card, eight of nine sessions were one to three records
short and the ninth was exactly equal.

So for any profile with *calculate unintentional leaks* enabled — which is the
default (`profiles.h:653`) — `CPAP_Leak` becomes a one-point event list on
almost every session carrying `.APE` detail, where the `qMax()` before this
commit kept both points. `CPAP_LeakTotal` itself is unaffected, which is why
Total Leak still draws its flat line.

The user-visible result is that **no Leak Rate graph is drawn at all**:
`gLineChart.cpp:664` skips any event list of one sample or fewer
(`if (siz <= 1) { continue; }`). The single session whose minute count matched
its span did render, because the bound in `findEventListContaining()` is
inclusive.

This did not go unnoticed: `apextests.cpp:492` was changed from
`QCOMPARE(detailedDerivedCount, 2)` to `1`. The expectation was updated rather
than the behaviour, and without a comment recording the coupling.

Nothing is gained by the loss. `TimeSeries::findValueAtOrBefore()` would have
held the last sample's value over that final partial minute, which is the
normal convention.

**Suggested fix** — end both synthetic traces together. Hoist `sampleDuration`
above the leak call in `importMinuteDetail()` and clamp:

```cpp
importLeakChannel(session, rec, startMs,
                  qMin(rec.end.toMSecsSinceEpoch(), startMs + sampleDuration));
```

`qMin` is correct in both directions: when the decoded minutes outrun the APF
span, `rec.end` is already the smaller value and nothing changes. The cost is
at most the final partial minute of the leak trace. The alternative — padding
the waveform out to `rec.end` — invents a sample and should be avoided.

Whichever way it goes, `apextests.cpp:492` deserves a comment stating that the
derived-leak count is a function of the pressure waveform's extent, so the
number is not silently adjusted again later.

**Resolved.** Applied as described, with the clamp guarded against an empty
`minutes` vector. The test now holds the derived count at 2 and carries a
comment explaining that a relaxed count means an invisible graph — which is how
this got through the first time. Confirmed against a real card: before the
change the Leak Rate graph appeared on exactly one of nine sessions, the one
with no shortfall; after it, on all nine. Logged in
`Notes/Developer Notes/BUG_FIXES.md`.

---

## F2. (Docs) Two header comments were not updated with the code

The design note was revised for both of these changes; the Doxygen was not.

* `apexDataParsing.h:155-158` — `decodeApeSessionRun()`'s `\return` still
  documents the failure mode removed by the finding 7 fix ("the cursor+2
  position falls outside `[kApeRingStart, kApeRingEnd)`") and still says "All
  three conditions mean the table entry is stale". There are two conditions now:
  no `FE FE FE` marker at the wrapped position, and no terminator within
  `kApeMaxMinutes`.
* `apex_loader.h:55` — `findDataDir()` is still described as locating "the
  directory containing 00000000.APF and .APE". After the finding 2 fix it keys
  on `.APF` alone.

**Resolved.** Both corrected. `decodeApeSessionRun()`'s `\param cursorRaw` also
now states that any value is accepted and wrapped rather than range-checked,
since the old `\return` text was the only place that contract was written down.

---

## F3. (Trivial) Stray blank line

`apex_loader.cpp:172` — a blank line immediately after `backupDataDir()`'s
opening brace, left behind when the body was extracted from `backupData()`.

**Resolved.**

---

## Notes, not defects

* **The staleness check changed identity, not strength.** Dropping the cursor
  range check means a stale entry's out-of-range cursor is now wrapped into the
  ring and rejected by the `FE FE FE` check instead of by an explicit bounds
  test. A false accept needs three specific bytes at the wrapped position —
  roughly 1 in 16 million — and both paths end in `parseApe()` skipping the
  entry, so behaviour is equivalent. Recorded so the changed error text is not
  later mistaken for a different problem.
* **Detection is deliberately weaker.** The gate is now a file named
  `00000000.APF`, exactly 21,250 bytes, whose first record decodes. The
  filename carries most of the specificity, and §9 of the design note argues the
  trade-off correctly: refusing an otherwise-good card because its rolling
  detail file is absent or stale costs more than the residual false-positive
  risk.

## Verified sound in this commit

* **Session extent ordering.** `Open()` sets `really_set_last()` from `rec.end`
  before calling `importMinuteDetail()`, which then widens it via `qMax()`
  (`apex_loader.cpp:271`). The per-minute event timestamps are
  `startMs + i * 60000` for `i < n`, so they always fall inside the widened
  extent.
* **The `.APE`-optional fallback is now genuinely reachable.** A missing or
  unreadable file fails `readFile()`, and a wrong-size file fails `parseApe()`;
  both route to the summary-only path with one warning. That is what §9 of the
  design note has always claimed.
* **The new tests are safe.** `QVector<ApeMinuteRecord> minutes(3)` relies on
  the struct's default member initialisers, which are present
  (`apexDataParsing.h:116-122`), so the values are zeroed rather than
  indeterminate.
* **Removing `setSummaryOnly(false)` is safe.** `Session`'s constructor sets it
  (`session.cpp:73`), and nothing between construction and import changes it.
* **`Q_OBJECT` placement.** Moving it above the `UNITTEST_MODE` `friend` leaves
  the class in `private:` access for that declaration, which is where it was
  before.
