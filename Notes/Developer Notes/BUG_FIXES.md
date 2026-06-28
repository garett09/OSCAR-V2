# OSCAR Bug Fix Log

Notable bugs found and fixed during development/investigation.

---

## 2026-06-25 — BMC legacy loader: session after circular-buffer boundary fails to import

**File:** `oscar/SleepLib/loader_plugins/bmcDataParsing.cpp` — `FindValidSessions()`

**Symptom:** When a BMC session follows a region of old circular-buffer data in the waveform
file (i.e., the previous session ended and the write pointer left stale data at the next
position), the following session fails to import at all. Reported as 6/23 truncation and
6/24 showing no data on the Daily page.

**Root cause:** `FindValidSessions()` selected a waveform crumb (a coarse seek position)
that placed `ReadWaveforms` before the old-data boundary. `ReadWaveforms` hit the stale
circular-buffer data (backward timestamp jump > 2 hours), correctly stopped, and never
reached the current session's data which lay just past the boundary.

Specifically: 6/23 ended at .010@pkt54083; .010@pkt54084 contained stale April data from
the previous cycle; 6/24 data began at .010@pkt54085. The selected crumb was at pkt53248,
so `ReadWaveforms` hit the old-data at pkt54084 and stopped before pkt54085.

The IDX file already records the exact start packet for each session, but the crumb
selection was ignoring it and relying on time-based crumb matching instead.

**Fix:** When a valid IDX entry is found for a session and its start timestamp falls within
the session's OSCAR-day window, construct `chosenCrumb` directly from the IDX entry's
`StartOffsetPacket` / `StartFileIndex`.  This places `ReadWaveforms` exactly at the
current cycle's data, bypassing any stale circular-buffer data that precedes it.

A window guard is required because the IDX matching loop may assign the *next* day's IDX
entry (via the `nextDay` condition) whose start timestamp lies outside the current session's
window.  Without the guard, using that out-of-window position causes `ReadWaveforms` to
stop immediately (EndTimestamp exceeded) and return no waveforms.

The crumb-based fallback (including the OSCAR-day fallback added 2026-06-17) is retained
for sessions where no IDX entry is available or the IDX start falls outside the window.

**Enhancement:** When waveforms are truncated (circular-buffer boundary reached before the
session ended), OSCAR now extends the session EndTimestamp to the full USR-reported duration
(`actualStart + DurationMinutes`).  This causes respiratory events recorded throughout the
night to be distributed to the session and displayed on the Daily page, matching PapLink's
behaviour of showing the complete session with events even when the waveform graphs end early.

---

## 2026-06-24 — CSR shown in Daily left sidebar with no actual CSR events (AirSense 11)

**File:** `oscar/SleepLib/loader_plugins/resmed_loader.cpp` — `ResmedLoader::LoadCSL()`

**Symptom:** The Daily page left sidebar listed Cheyne-Stokes Respiration (CSR) for an
AirSense 11 AutoSet profile even though no CSR ever occurred — nothing in the Statistics
panel, Overview trend, the CSV respiratory export, or a direct database search. Other
ResMed devices only showed the CSR sidebar item when at least one real CSR span existed
in the profile history.

**Root cause:** `LoadCSL()` ran for every session whose day contained a `*_CSL.edf` file
(`CSLlist`, called per session at the EVE/CSL loop) and unconditionally created an empty
CSR event list up front ("Always create CSR event list so that overview always finds
something"). An event list with zero events still registers `CPAP_CSR` in the session's
`m_availableChannels` (`session.cpp` summary rebuild), and the Daily sidebar unions
`m_availableChannels` across the whole profile history (`daily.cpp`). The AirSense 11
emits CSL.edf files even on nights with no "CSR Start"/"CSR End" annotations, so CSR was
listed permanently. The code path is identical for all ResMed machines; the only variable
is whether the card contains CSL.edf files.

**Fix:** Create the CSR event list lazily — initialise `EventList *CSR = nullptr` and only
call `sess->AddEventList(CPAP_CSR, EVL_Event)` when a valid, in-session CSR span is about
to be added (restoring the original deferred-creation pattern that was present, commented
out, in the "CSR End" branch). Days/profiles with no real CSR no longer register the
channel, so the sidebar behaves consistently across all ResMed devices.

---

## 2026-06-20 — Disabled sessions in OSCAR 1.7.1 become enabled after import into 2.0

**Files:** `oscar/profileimporter.h`, `oscar/profileimporter.cpp`

**Symptom:** Any session that was disabled (unchecked) on the Daily page in OSCAR 1.7.1
appeared enabled after importing the profile into OSCAR 2.0.

**Root cause (two parts):**
1. OSCAR 1.7.1 stores the per-session enabled/disabled flag in `Sessions.info` (binary
   file, machine folder), written on every OSCAR exit via `Machine::~Machine()`. The `.000`
   binary summary files do not store this flag. `Session::LoadSummaryFromFile()` always
   sets `s_enabled = 1`, so disabled state was lost.
2. A first fix attempt read `Summaries.xml.gz` / `Summaries.xml` instead. That file is
   only written during SD card imports (`SaveSummaryCache()`) and can be stale — a session
   disabled after the last import appears as `enabled="1"` in the XML. `loadSessionInfo()`
   in 1.7.1 reads `Sessions.info` after `LoadSummary()` and overrides whatever the XML
   says, making `Sessions.info` the authoritative source.

**Fix:** `ProfileImporter::readSummariesEnabledMap()` now reads `Sessions.info` (binary,
`magic=0xC73216AB`, `filetype=5`, one `quint32 sid + quint8 enabled` record per session),
with fallback to `Summaries.xml` / `Summaries.xml.gz` for very old profiles that predate
`Sessions.info`. `loadMachineSessions()` calls this before the `.000` loop and calls
`session->setEnabled(false)` after `LoadSummaryFromFile()` for any disabled session.
`setEnabled()` guards its DB write with `m_sessionrow_id > 0`, so calling it before
`StoreToDatabase()` is safe — only the in-memory flag is changed, which `StoreToDatabase()`
then persists.

---

## 2026-06-18 — WebDAV mapped drive not visible in "Find your CPAP data card" dialog

**File:** `oscar/mainwindow.cpp` — `selectCPAPDataCards()`

**Symptom:** On Windows 11 with a network drive (e.g. Z:) mapped via WebDAV, the drive
appeared in Windows File Explorer but was absent from OSCAR's "Find your CPAP data card"
`QFileDialog`.

**Root cause:** The `QFileDialog` was opened without a `setSidebarUrls()` call. Qt's
non-native dialog sidebar (used when the OSCAR UI language differs from the Windows UI
language) is populated only from default system bookmarks — drive letters are not added
automatically. The native IFileDialog sidebar is similarly limited to Windows-default
locations without an explicit `AddPlace()` call, which Qt maps from `setSidebarUrls()`.
WebDAV drives are also excluded from the FAT32-only auto-scan in `getDriveList()`, so the
user always reaches the manual dialog when importing from WebDAV.

**Fix:** Added a `setSidebarUrls()` call that merges the dialog's existing sidebar URLs
with all entries from `QDir::drives()`. On Windows, `QDir::drives()` calls
`GetLogicalDriveStrings()` which reliably includes all logical drive letters — local,
removable, and network (WebDAV). For the non-native dialog, this populates the sidebar
widget directly; for the native dialog, Qt maps each URL to `IFileDialog::AddPlace()`.

---

## 2026-06-20 — Up-arrow buttons unclickable on spinboxes in Edit User Profile dialog

**File:** `oscar/newprofile.cpp` — constructor `NewProfile::NewProfile()`

**Symptom:** On the Edit User Profile dialog, the up-arrow buttons on QDoubleSpinBox
widgets (Height and RX Pressure fields) could not be clicked; the cursor did not change
to a pointer over them. Down-arrows worked. The issue did not appear with Fusion style.

**Root cause:** The light-mode stylesheet applied in the constructor included
`border: 1px solid #adadad; border-radius: 2px;` for `QDoubleSpinBox` (and `QDateEdit`).
When any `border` or `border-radius` property is set on a QAbstractSpinBox derivative via
QSS, Qt's style engine takes over sub-control geometry calculation instead of delegating
to the native Windows Vista style. Qt's QSS engine renders the arrows using UxTheme but
calculates hit-test rects independently. Without explicit `::up-button` QSS rules, the
up-button's hit-test rect is miscalculated and does not match the rendered position.
The down-button incidentally worked due to its bottom position. Fusion style implements
its own correct sub-control geometry so was unaffected.

**Fix:** Removed `border` and `border-radius` from the `QDoubleSpinBox` and `QDateEdit`
stylesheet rules, keeping only `background-color` and `color`. The native style then
handles sub-control geometry correctly.

---

## 2026-06-17 — Window clipped off-screen on Windows 11 4K display

**File:** `oscar/mainwindow.cpp` / `oscar/mainwindow.h`

**Symptom:** On Windows 11 with a 3840×2160 display, the OSCAR window opens with content
clipped at the bottom and blank space at the top. Workaround: maximize then restore.

**Root cause:** The Windows frame-position correction (fixing `restoreGeometry()` frame vs.
client-area confusion) was queued via `QTimer::singleShot(0)` in the `MainWindow`
constructor. It fired during `SetupGUI()`'s `QApplication::processEvents()` call — before
`mainwin->show()`. At that point `screen()` returns null and `frameGeometry()` is
unreliable, so the correction was a no-op.

**Fix:** Moved the correction into `showEvent()`, guarded by `m_geometryCorrected`, so it
fires after the window manager has applied the frame. Closes #219.

---

## 2026-06-17 — BMC legacy loader: 6/16 session skipped; 6/15 EVT data discarded

**File:** `oscar/SleepLib/loader_plugins/bmcDataParsing.cpp` — `FindValidSessions()`, `ReadDateSession()`

**Symptom:** On a BMC legacy card where the user fell asleep after noon (session started
~15:19), the most recent session was silently dropped. Older sessions whose waveform data
had been overwritten by the circular buffer were also dropped entirely, losing their
respiratory event records.

**Root cause (6/16 — waveform skipped):** `FindValidSessions()` selects the last waveform
crumb strictly before the session's `StartTimestamp`. Because `DecodeDate()` always sets the
session start to noon, any session that actually began after noon has no qualifying crumb
— even though the waveform data is present on the card. The session was discarded at the
`if (!chosenCrumb.Timestamp.isValid()) continue;` guard.

**Root cause (6/15 — EVT data lost):** When the circular waveform buffer has overwritten
older data, no crumb exists at all for those sessions. The same guard caused the session to
be completely skipped, including its USR summary and respiratory events.

**Root cause (memory leak):** When `dateSession.Waveforms` is empty, the `BmcSession*`
created before the waveform-splitting loop was never appended and never deleted.

**Fix:**
- `FindValidSessions()`: After the primary crumb search fails, fall back to the first crumb
  within the session's OSCAR day (noon → next noon). This handles sessions starting after
  noon. Sessions still without a crumb continue to be linked (with an invalid crumb) for
  EVT-only import.
- `ReadDateSession()`: Added a fallback at the end that creates an EVT-only `BmcSession`
  from the USR summary when no waveform sessions were produced. Start/end timestamps are
  derived from the first/last respiratory event (with `DurationMinutes` as minimum width),
  or from noon if there are no events. Fixed the memory leak with an `else { delete session; }`.

---

## 2026-06-14 — Integrity check on restart after declining loader data rebuild

**File:** `oscar/SleepLib/profiles.cpp` — `Profile::DataFormatError()`

**Symptom:** If the user clicked "No" when prompted to rebuild a machine's data after a loader
version change, OSCAR called `QApplication::exit(-1)`. This bypassed `closeEvent()` and
`DatabaseManager::markCleanShutdown()`, leaving the dirty-shutdown flag set. On the next
startup, OSCAR ran a full database integrity check unnecessarily.

**Root cause:** The `exit(-1)` path skipped the normal shutdown sequence. The "file manager
launch" secondary dialog that preceded it was also unhelpful boilerplate.

**Fix:** Removed the secondary info dialog and `showInGraphicalShell()` call. Call
`DatabaseManager::markCleanShutdown()` explicitly before `QApplication::exit(0)`.

---

## 2026-06-13 — Daily page: right-click menu checkboxes invisible when checked

**File:** `oscar/Graphs/gGraphView.cpp` — `gGraphView::populateMenu()`

**Symptom:** In the right-click context menu's Plots, CPAP Overlays, Oximeter Overlays, and
Dotted Lines submenus, checked items showed no visual indicator at all; only unchecked items
showed the empty-box outline.

**Root cause:** In Qt6, applying any stylesheet to a `QCheckBox` (even just a hover
background rule) activates QStyleSheetStyle for the entire widget. Without explicit
`::indicator` rules in that stylesheet, QStyleSheetStyle cannot properly delegate the
checked-state indicator to the native platform renderer inside a styled `QMenu`. The
unchecked border outline still rendered (it's a simple CSS border), but the checked-state
checkmark/fill was invisible.

**Fix:** Added explicit `QCheckBox::indicator` stylesheet rules to all three
`chbox->setStyleSheet(...)` calls in `populateMenu()`, using the already-existing
`:/icons/empty_box.png` and `:/icons/checkmark.png` resource icons.

---

## 2026-06-13 — Daily page: Selection Length not updating on zoom

**File:** `oscar/Graphs/gFlagsLine.cpp` — `gFlagsGroup::paint()`

**Symptom:** On the Daily page, the "Selection Length" text at the top of the Event Flags
graph always showed the full night's duration even after zooming in on another graph.

**Root cause:** The Event Flags graph has `blockZoom = true` so that flags remain visible
across the full day regardless of zoom. In `gFlagsGroup::paint()`, the same
`blockZoom`-aware bounds (`g.rmin_x`/`g.rmax_x`) were used for both drawing the flag bars
AND computing the Selection Length text. `rmin_x`/`rmax_x` are the full-day immutable
bounds and are never updated by `SetXBounds`, so the Selection Length never reflected the
zoomed window. In 1.7.1 the text used `g.graphView()->GetXBounds()` which always returns
the current view bounds.

**Fix:** Introduced separate `selminx`/`selmaxx`/`seldur` variables in `paint()` that always
read `g.min_x`/`g.max_x`. These are updated by `SetXBounds` when any graph in the group
zooms. Flag drawing continues to use the `blockZoom`-aware full-day bounds.

---

## 2026-06-11 — Add Version 1 CSV export (1.7.x-style) to File > Export Data menu

**Files:** `exports/exportcsv.h`, `exports/exportcsv.cpp`, `exports/exportcsv.ui`,
`oscar.pro`, `mainwindow.ui`, `mainwindow.h`, `mainwindow.cpp`

**Change:** Ported the 1.7.x `ExportCSV` dialog to OSCAR 2.0 and wired it to a new
File > Export Data > Version 1 CSV Reports... menu item (positioned after CSV Export Wizard).
Offers daily summary, per-session summary, and raw event detail CSV exports matching
the 1.7.x layout. All SleepLib APIs (`Day`, `Session`, `ahiChannels`, etc.) are compatible
with 2.0; resource path in the .ui updated to `../Resources.qrc`.

**GitLab:** Closes #214

---

## 2026-06-10 — BMC G3X: EVT-only import for PapLink-synced cards

**Files:** `bmcDataParsing.h`, `bmcG3xDataParsing.cpp`, `bmc_loader.cpp`

**Symptom:** OSCAR showed "no data found" for G3X cards that use BMC's PapLink cloud sync;
the SD card retains only the current session's waveform file (16 KB) but stores
148+ nights of IT summary statistics and EVT events in the .idx/.evt files.

**Root cause:** `ParseIdxRecords` rejected any IDX record with `waveLen==0`; all 148 historical
records had `waveLen=0` (waveform data is on BMC's server, not the card).

**Fix:** Accept `waveLen==0` records that have IT-block duration>0 or EVT data. Add EVT-only
session-building path that splits on 0x40/0x41 markers, populates PressureSnapshots
from 0x42 records (30s steps), and emits CPAP_Pressure/IPAP/EPAP without a flow
waveform. IT-only days (no EVT) get a synthetic session using the IT-block duration.

---

## 2026-06-10 - Daily CSV report: "95th" percentile columns were actually 90th

**Files:** `oscar/docs/system_reports.orf` (Daily Summaries/by Day)

**Symptom:** The `by Day` CSV report's `Pressure_95th` and `Leak_95th` columns are
labelled as the 95th percentile but contain the 90th.

**Root cause:** `daily_summaries.pressure_95th` / `leak_total_95th` are populated by
`day->p90(...)` (`daily_summary_repository.cpp:302/308`), and `Day::p90()` returns
`percentile(code, 0.90F)` (`day.cpp:483`) — a hardcoded 90th percentile. The column
name (and report header) were never reconciled with the stored value. The session-level
equivalents (`session_summaries`) genuinely store the 95th (`session.cpp:3389/3401`),
so the two reports disagreed.

**Fix:** Relabelled the `by Day` headers to `Pressure_90th` / `Leak_90th` to match the
stored value. (Done as part of surfacing additional stored fields in the by-Day and
by-Session reports.) The deeper day-vs-session percentile-level inconsistency, and the
day/session leak-channel difference (`CPAP_Leak` vs `CPAP_LeakTotal`), are left for a
separate fix — see GitLab issue #212.

## 2026-06-10 - Export CSV: date formatting inconsistency and stale filename on custom range

**Files:** `oscar/exports/report_exporter.cpp`

**Symptom 1:** In the CSV Export dialog, start date and end date pickers displayed dates
with different formats — start showed e.g. "5/1/2026" (no leading zeros) while end showed
"05/01/2026" (with leading zeros).

**Root cause:** `m_endDateEdit->setDisplayFormat("MM/dd/yyyy")` was hardcoded while
`m_startDateEdit->setDisplayFormat(QLocale().dateFormat(...))` used the locale format.
Simple copy-paste error; the two widgets used different literal format strings.

**Fix:** Compute a single `dateDisplayFormat` string from the locale and apply it to both
date edits.

**Symptom 2:** When Custom date range was selected, manually changing start/end dates did
not update the filename field.

**Root cause:** `dateChanged` on the two date edits was not connected to
`updateFilenameField()`. The filename was only refreshed when the quick-range combo or
profile changed.

**Fix:** Connected both `m_startDateEdit::dateChanged` and `m_endDateEdit::dateChanged`
to `updateFilenameField()` at widget construction time.

**Also:** Deleted the dead `ExportCSV` class (`exportcsv.cpp/.h/.ui`) — it was the
predecessor to `ReportExporter` and had been unreachable since `ReportExporter` replaced
it. Removed the stale `#include "exportcsv.h"` from `reportvarietyeditor.cpp`.

---

## 2026-06-10 - Application font change not propagating to most UI widgets on Qt6/Windows

**Files:** `oscar/SleepLib/common.cpp` (`setApplicationFont`)

**Symptom:** Changing the application font or point size in File → Preferences did not
update most UI widgets: the main tab bar labels (Profile/Daily/Overview/Statistics), the
daily page sidebar (text and tab names), the welcome page title labels, and the options
frames at the bottom of the Overview and Statistics pages.

**Root cause (all platforms):** Qt6's `QApplication::setFont()` sends `ApplicationFontChange`
to all widgets via `allWidgets()`, but in arbitrary order. Each child widget resolves its
font against its parent's `data.fnt`, which may not yet be updated when the child
processes the event. Children end up resolving against a stale parent font and don't
visually update. Qt5 did not have this ordering problem.

**Root cause (Windows/Qt6 additionally):** OSCAR 2.0 calls `qApp->setStyleSheet()` for
QComboBox padding. This installs `QStyleSheetStyle` as the application style, which caches
each widget's font at polish-time. Even after `setFont()` updates the logical font,
`QStyleSheetStyle` renders with its stale cached font.

**Fix:** Two-part fix in `setApplicationFont()` (`SleepLib/common.cpp`):
1. Call `mainwin->setFont(font)` — uses ordered top-down propagation (mainwindow first,
   then each level of children in turn), so every widget always resolves against an
   already-updated parent. Fixes macOS and Linux.
2. Iterate `QApplication::allWidgets()` and unpolish+polish every widget that has
   `WA_StyleSheet` set. This covers both app-level stylesheets (Windows: all widgets are
   marked) and widget-level stylesheets (e.g. `ProfileSelector::setStyleSheet()` in its
   constructor). Without this, `QStyleSheetStyle` renders with stale cached fonts even
   after the logical font has been updated.

---

## 2026-06-07 - Profile import: X button on progress dialog hides dialog, leaving OSCAR invisible

**Files:** `oscar/SleepLib/progressdialog.{h,cpp}`, `oscar/mainwindow.cpp`
(`on_action_Import_OSCAR_Data_triggered`)

**Symptom:** Clicking the title-bar X on the import progress dialog made the dialog
disappear while the import continued running with no visible window. OSCAR appeared gone
but was still alive in Task Manager.

**Root cause:** `ProgressDialog` had no `closeEvent` override, so the default
`QDialog::reject()` → `hide()` fired, removing the only visible window. The import was
still running on the main thread via `processEvents()`, and no abort signal was wired up
to `ProfileImporter::cancel()`.

**Fix:**
- Added `closeEvent` override to `ProgressDialog`: if `m_allowClose` is false, ignores
  the event and calls `onAbortClicked()` (treats X as Cancel). Added `allowClose()` method
  so the programmatic `progress.close()` at the end of the import can still close it.
- Added `progress.addAbortButton()` and connected `progress.abortClicked` →
  `importer.cancel()` in `on_action_Import_OSCAR_Data_triggered`.
- `progress.allowClose()` called before `progress.close()` at end of import.
- Added `importer.wasCancelled()` check to show "Import Cancelled" info message instead
  of the "Import Failed" error message when the user deliberately stops the import.

---

## 2026-06-07 - Profile import: Backup folder copy slow due to per-file processEvents()

**Files:** `oscar/profileimporter.{h,cpp}` (`copyDirectoryRecursively`)

**Symptom:** Copying the Backup folder during profile import from OSCAR 1.7.1 takes a
very long time — far longer than the actual I/O would require.

**Root cause:** `copyDirectoryRecursively` called `QApplication::processEvents()` after
every single file. A ResMed Backup folder with years of EDF files can contain thousands
of files, so the Qt event queue was drained thousands of times — dominating the copy time.

**Fix:** Added `QElapsedTimer m_copyTimer` to `ProfileImporter`. Started before each
Backup folder copy. In `copyDirectoryRecursively`, replaced the unconditional per-file
`processEvents()` with a timer-guarded call that fires at most once per 100ms.

---

## 2026-06-07 - Profile import: oversized event list blob causes cascade failure

**Files:** `oscar/database/event_data_repository.cpp` (`storeEventListData`),
`oscar/SleepLib/session.cpp` (`Session::StoreEventsToDatabase`)

**Symptom:** Profile import fails with multiple "Failed to store event data" warnings:
first "string or blob too big Unable to bind parameters", then "Parameter count mismatch"
on subsequent channels, resulting in `totalSaved < totalEventLists` and the whole session
being counted as failed.

**Root causes (two):**

1. **Corrupted count in legacy .001 file** — one event list (channel 6144, index 23) has a
   count field that is garbage-large, causing `serializeInt16Array` to produce a blob
   exceeding SQLite's `SQLITE_MAX_LENGTH` (1 GB default). Compression is already applied
   in `storeEventListData`; at sizes of 500M+ samples it cannot reduce the blob enough.

2. **Prepared-statement cascade** — after the first `m_insertQuery.exec()` failure,
   `m_statementsPrepared` remains `true`. The next call to `storeEventListData` reuses the
   now-invalid SQLite prepared statement and fails with "Parameter count mismatch", causing
   additional spurious failures.

**Fixes:**

1. `event_data_repository.cpp`: call `resetPreparedStatements()` after a failed `exec()`
   so subsequent calls get a fresh statement instead of inheriting the broken one.

2. `session.cpp`: before incrementing `totalEventLists`, compute the estimated blob size
   (`count × sizeof(EventStoreType)`, doubled for second field, plus time array for events).
   If it exceeds 100 MB — far beyond any legitimate CPAP night even at 250 Hz — skip the
   event list with a `qWarning` (channel, index, count, estimated size) and `continue`
   without incrementing `totalEventLists`. The rest of the session then imports cleanly.

---

## 2026-06-06 - Profile import: "Session persistence failure: 0 session(s) and 1 event set(s) failed to store"

**Files:** `oscar/SleepLib/session.cpp` (`Session::StoreEventsToDatabase`),
`oscar/profileimporter.{h,cpp}` (`ProfileImporter::loadMachineSessions`)

**Symptom:** Importing an OSCAR 1.x profile fails with the error "Session persistence failure:
0 session(s) and 1 event set(s) failed to store". The failure affected exactly one session per
import attempt.

**Root cause:** `Session::StoreEventsToDatabase()` calls
`EventListRepository::deleteBySession(m_sessionrow_id)` to clear stale `event_lists` rows
before re-inserting. The return value was silently ignored. If the DELETE fails for any reason
(e.g., a prior SQL error leaving the connection in an error state), stale rows remain for that
session. The subsequent `INSERT INTO event_lists` then hits the
`UNIQUE(session_id, channel_id, eventlist_index)` constraint, `create()` returns -1,
`totalSaved < totalEventLists`, and `StoreEventsToDatabase()` returns false.

**Fix:**
1. `session.cpp`: Added return-value check on `deleteBySession()` with a `qWarning()` when it
   fails, so the failure is surfaced rather than silently swallowed.
2. `profileimporter.h`: Added `m_lastEventFailFile` and `m_lastEventFailDbError` member strings.
3. `profileimporter.cpp`: On event-store failure, capture the session filename and SQL error text.
   Improved the user-visible error message to include the failing session filename and SQL error
   so future occurrences can be diagnosed from the dialog alone.

---

## 2026-06-06 - ResMed: second session silently dropped / time-shifted when sessions are close together

**File:** `oscar/SleepLib/loader_plugins/resmed_loader.cpp`
(`ResDayTask::run`, `repairEDFStartFromSession`)

**Symptom:** When two sessions are separated by less than ~60 seconds, the second session's
EDF data is time-shifted (placed at the wrong session's mask-on time) in the flow charts.

**Root cause:** `repairEDFStartFromSession` used `ovr.start` (the STR mask-on of the session
the EDF file was assigned to) as the repair target. For devices whose EDF headers store local
time (labeled UTC), the header and filename time agree after `localNoDST` conversion —
`edf.startdate ≈ filenameMs`. The repair should therefore be a no-op. But because the STR's
1-minute resolution causes `maskon[1] = maskoff[0]` when sessions are <60 s apart, the second
session's EDF files end up in the first session's or second session's overlap depending on the
exact sub-second timing, and `ovr.start` is the wrong session's mask-on. The tolerance check
`|edf.startdate − ovr.start| > 6 h` then fires spuriously (difference was ~6h22min in the
observed case), forcing all the second session's data to the first session's start time.

**Fix:** `repairEDFStartFromSession` now uses the filename-derived time as the primary repair
target instead of `ovr.start` (STR maskon). The filename records when the device RTC created
the EDF file — which is what `edf.startdate` should contain — whereas `ovr.start` is when the
mask was applied (0–60 s earlier), and is simply wrong when the file was mismatched to a
different session. The STR session time is used as a fallback only if the filename cannot be
parsed.

Also adds a `fileNearSession` guard to the duration-overlap fallback in `ResDayTask::run` to
prevent a file starting more than 10 minutes before a session's mask-on from being claimed by
that session via duration overlap (secondary defense).

---

## 2026-06-02 - Profile rename always restarted, even for non-open profiles

**File:** `oscar/newprofile.cpp` (`NewProfile::on_nextButton_clicked`)

**Symptom:** Renaming any profile triggered a full application restart, even when the
renamed profile was not the currently open one.

**Additional bugs in non-open case:**
- `Profiles::profiles[newProfileName] = p_profile` stored the *open* profile's pointer
  instead of the renamed profile's pointer, and never removed the old key.
- `AppSetting->setProfileName()` was called unconditionally, incorrectly changing the
  active profile name when renaming a different profile.
- `CloseProfile()` was called unconditionally, closing whatever profile was open.

**Fix:** Split the post-rename path on `profile == p_profile`.
- Open profile: AppSetting update + CloseProfile + DB update + RestartApplication (unchanged).
- Non-open profile: DB update + `profileSelector->updateProfileList()` + `accept()`.
- Both paths: fix `Profiles::profiles` map (remove old key, insert new with correct pointer).

---

## 2026-06-02 - Planned restart triggers spurious integrity check

**File:** `oscar/mainwindow.cpp` (`MainWindow::RestartApplication`)

**Symptom:** After a profile rename (or language change), OSCAR restarts and runs a
full database integrity check on the new instance, even though the shutdown was clean.

**Root cause:** `RestartApplication()` calls `QProcess::startDetached()` then
`QApplication::exit()`. The new process races ahead and reads `CleanShutdown = false`
(set at startup) before `main()`'s cleanup path writes `markCleanShutdown()`.
`switchToDatabase()` already fixed this same race explicitly, but `RestartApplication()`
was never updated to match.

**Fix:** Call `DatabaseManager::markCleanShutdown()` before `QProcess::startDetached()`
in `RestartApplication()`, same pattern as `switchToDatabase()`.

---

## 2026-06-02 - Profile rename: no explicit conflict check (issue #198)

**File:** `oscar/newprofile.cpp` (`NewProfile::on_nextButton_clicked`)

**Symptom:** Renaming a profile to a name already in use by another profile relied on
`QDir::rename()` returning false to detect the conflict. This meant non-conflict OS
failures (permissions, locked files) showed a misleading "Profile Name Already In Use"
message. Additionally, a DB username conflict without a matching directory was not
detected at all.

**Root cause:** No explicit pre-checks before attempting `QDir::rename()`.

**Fix:** Added explicit checks for both directory (`profilesDir.exists(newProfileName)`)
and database (`profileRepo.findByUsername(newProfileName).id != 0`) conflicts before
attempting the rename. The `ProfileRepository` instance is now shared between the
pre-check and the subsequent DB update. The fallback error message (OS rename failure)
now gives an accurate description rather than falsely claiming a name conflict.

---

## 2026-06-01 - Edit Profile dialog all-dark on KDE Plasma dark mode

**File:** `oscar/newprofile.cpp` (`NewProfile` constructor)

**Symptom:** On Kubuntu 26.04 in system dark mode, the Edit Profile (NewProfile) wizard
dialog appeared entirely dark — dark background, white text, dark input fields.

**Root cause:** Qt's `styleHints()->setColorScheme(Light)` hint (set in `main.cpp`) is not
guaranteed on KDE Plasma; the KDE platform theme injects its own dark palette colors into
individual windows regardless of the application-level hint. `NewProfile` had no explicit
light-mode styling, unlike `ProfileSelector` which already had a protective stylesheet
(see commit `1fc4110c` for the prior SQL editor instance of the same issue).

**Fix:** Added `setAutoFillBackground(true)`, a white `QPalette::Window`, and an explicit
stylesheet covering all widget types used in the dialog (QLabel, QLineEdit, QTextEdit,
QComboBox, QDateEdit, QDoubleSpinBox, QCheckBox, QGroupBox, QPushButton, QTextBrowser,
QPlainTextEdit) — matching the pattern already in `ProfileSelector`.

---

## 2026-06-01 - Compress Database blocked UI thread and used excessive memory

**Files:** `oscar/mainwindow.cpp`

**Symptom:** Compressing a large database (20+ GB) caused OSCAR to use all available system
memory and block the UI for an extended period with no feedback to the user.

**Root causes (two):**

1. **`VACUUM` used WAL mode internally**, creating a copy of the entire database in the WAL
   file before merging it back. This effectively doubled peak memory usage.

2. **No wait dialog**, so the application appeared frozen to the user.

**Fix:** Replaced `VACUUM` with `VACUUM INTO 'oscar_new.db'`, which writes the compacted copy
directly to a new file without WAL involvement. After completion, the old file is swapped out
and OSCAR restarts. Both the integrity check and the VACUUM now run on background threads
(`QThread` + `QEventLoop`) with plain wait dialogs so the UI remains responsive throughout.

---

## 2026-06-01 - SQLite corruption errors not surfaced to user

**Files:** `oscar/database/database_manager.{h,cpp}`, all 27 repository `.cpp` files

**Symptom:** If a query failed due to database corruption (`SQLITE_CORRUPT` code 11 or
`SQLITE_IOERR` code 10), OSCAR logged a warning and silently continued, potentially showing
wrong or missing data with no explanation.

**Fix:** Added `DatabaseManager::checkQueryError()` which inspects the native SQLite error code
(low byte of `QSqlError::nativeErrorCode()`). If corruption or I/O error is detected it emits
`databaseError()` — already wired to a `QMessageBox::critical` in `main.cpp` — with recovery
guidance. A `m_corruptionReported` flag prevents stacking multiple dialogs on a cascade of
failures. Added call sites to all exec() failure paths across all 27 repository files.

---

## 2026-06-01 - Compress Database blocked UI thread and used excessive memory

**Files:** `oscar/mainwindow.cpp`

**Symptom:** Compressing a large database (20+ GB) caused OSCAR to use all available system
memory and block the UI for an extended period with no feedback to the user.

**Root causes (two):**

1. **`VACUUM` used WAL mode internally**, creating a copy of the entire database in the WAL
   file before merging it back, effectively doubling peak memory usage.

2. **No wait dialog**, so the application appeared frozen to the user.

**Fix:** Replaced `VACUUM` with `VACUUM INTO 'oscar_new.db'`, which writes the compacted copy
directly to a new file without WAL involvement. After completion the old file is swapped out
and OSCAR restarts. Both the integrity check and the VACUUM now run on background threads
(`QThread` + `QEventLoop`) with plain wait dialogs so the UI remains responsive throughout.

---

## 2026-06-01 - SQLite corruption errors not surfaced to user

**Files:** `oscar/database/database_manager.{h,cpp}`, all 27 repository `.cpp` files

**Symptom:** If a query failed due to database corruption (`SQLITE_CORRUPT` code 11 or
`SQLITE_IOERR` code 10), OSCAR logged a warning and silently continued, potentially showing
wrong or missing data with no explanation.

**Fix:** Added `DatabaseManager::checkQueryError()` which inspects the native SQLite error
code (low byte of `QSqlError::nativeErrorCode()`). If corruption or I/O error is detected it
emits `databaseError()` — already wired to a `QMessageBox::critical` in `main.cpp` — with
recovery guidance. A `m_corruptionReported` flag prevents stacking multiple dialogs on a
cascade of failures. Added call sites to all exec() failure paths across all 27 repository files.

---

## 2026-05-31 - Dirty-shutdown integrity check ran on wrong database after database switch

**Files:** `oscar/main.cpp`, `oscar/database/database_manager.cpp`, `oscar/database/database_manager.h`,
`oscar/mainwindow.cpp`

**Symptom:** Switching databases via File → Database → Recent triggered a lengthy integrity check
on the newly opened database even though the previous session had closed cleanly.

**Root causes (three):**

1. **Flag not path-scoped.** The dirty-shutdown flag (`db/CleanShutdown`) was a single global
   QSettings key with no association to a database path. A crash on database A set the flag dirty;
   opening any other database B on the next launch triggered an integrity check on B.

2. **Race condition on database switch.** `switchToDatabase()` spawned the new OSCAR process via
   `startDetached()` before the old process had written the clean-shutdown flag. The new process
   started (after its 1-second `-p` delay) and read the still-dirty flag before the old process
   finished cleanup.

3. **Current database not added to Recent list.** `main.cpp` only called `RecentDatabases::add()`
   when the list was empty (first run). If the list already had entries, the current database was
   never recorded, so it was absent from the Recent menu after switching away.

**Fixes:**

1. Added `db/LastShutdownPath` QSettings key alongside `db/CleanShutdown`. Startup now skips the
   integrity check if the stored path doesn't match the database being opened.

2. Added `DatabaseManager::markCleanShutdown(dbPath)` static method (writes both keys and calls
   `settings.sync()`). Called in `switchToDatabase()` **before** `startDetached()` so the flag
   is on disk before the new process is created. Also called from `main()` cleanup as before.

3. Changed `RecentDatabases::add(GetAppData())` in `main.cpp` to run unconditionally on every
   launch, promoting the current database to the top of the Recent list.

---

## 2026-05-31 - Integrity check dialog showed "(Not Responding)" on large databases

**Files:** `oscar/database/database_manager.cpp`, `oscar/main.cpp`, `oscar/mainwindow.cpp`

**Symptom:** The "Checking database integrity, please wait..." dialog displayed "(Not Responding)"
in its title bar during the check on large databases, alarming users into thinking OSCAR had hung.

**Root cause:** `checkIntegrity()` ran on the UI thread, blocking the Windows message pump.
Windows marks any window that stops processing messages for ~5 seconds as "Not Responding".

**Fix:** Rewrote `checkIntegrity()` to open a private temporary SQLite connection (named per
calling thread) so it is safe to call from any thread — Qt SQL connections are per-thread and
`m_database` belongs to the main thread. Both call sites (startup check in `main.cpp` and manual
check in `mainwindow.cpp`) now run the check on a `QThread` with a `QEventLoop` keeping the main
thread responsive. The wait dialog shows plain text only (no progress bar, which looked frozen).

---

## 2026-05-29 - BMC legacy loader: G3 B20A imports no data (1-byte packet offset)

**Files:** `oscar/SleepLib/loader_plugins/bmcDataParsing.cpp`, `bmcDataParsing.h`

**Symptom:** OSCAR detects a BMC G3 B20A SD card but imports 0 sessions.

**Root cause:** Some `.nnn` waveform files begin with a 255-byte legacy tail packet (the
oldest un-overwritten circular-buffer entry from a previous firmware era), followed by
standard 256-byte packets starting at byte 0xFF.  The loader assumed all packets were
256-byte aligned from byte 0.  Crumb timestamps sampled at multiples of 0x100000 fell
1 byte into the wrong field, producing QDateTime objects with invalid hours (e.g. hour=56)
whose `toMSecsSinceEpoch()` = INT64_MIN.  These invalid crumbs were selected as "last crumb
before session start" (INT64_MIN < any valid epoch), and `ReadWaveforms` stopped immediately
on the first garbage packet because the backward-jump guard fired against the initial
`lastPacketTimestamp = QDateTime(2000,1,1)`.

**Fix:** Added `DetectFileDataOffset()` which checks whether bytes 0xFF–0x100 form 0xAAAA
(i.e. the Terminator of the 255-byte packet followed by the header of the first data packet).
`BuildWaveformCrumbs` uses this offset when computing crumb byte positions and now also
requires `timestamp.isValid()`.  `ReadWaveforms` seeks past the 255-byte packet when opening
a successor file.  `ReadWaveformPacketTimestamp` signature changed from `quint16 packetOffset`
to `quint64 packetStartByte`.

---

## 2026-05-28 - ResMed loader: second session not imported on mid-night re-import

**File:** `oscar/SleepLib/loader_plugins/resmed_loader.cpp` (`checkSummaryDay()`)

**Symptom:** When a user sleeps part of the night, imports to OSCAR, sleeps the rest of the
night, then imports again, OSCAR reports everything up to date and the second session is never
imported. The user must purge the day and reimport everything to see both sessions.

**Root cause:** `checkSummaryDay()` decides whether to reimport a day by comparing existing
database sessions against mask-on/off pairs in the STR.edf (`maskevents / 2`). On a mid-night
re-import the device may not yet have written the second session to the STR, so `maskevents`
still shows 1 pair. With 1 DB session and 1 STR pair, the skip condition
`sessions.length() >= numPairs` (`1 >= 1`) is satisfied and the day is skipped, even though
the EDF files for the second session are already on the card.

**Fix:** Also count distinct EDF session groups on the card (unique timestamp prefix among
non-EVE/CSL files in `resday.files`). Use `qMax(numPairs, edfGroups)` as the expected session
count. The EDF file list is already in memory from `ScanFiles()` so this requires no
additional I/O. Closes GitLab #195.

---

## 2026-05-28 - ResMed loader: false positive EDF corruption warnings for CSL and EVE files

**File:** `oscar/SleepLib/loader_plugins/resmed_loader.cpp` (`LoadCSL()`, `LoadEVE()`)

**Symptom:** Log filled with "ResMed: repaired corrupt EDF startdate" warnings for CSL and EVE
files on days with multiple sessions. The EDF header timestamps were valid but were being
silently overwritten with incorrect values.

**Root cause:** `repairEDFStartFromSession()` validated an EDF header timestamp by comparing
it against the current session's mask-on time (from STR.edf), rejecting headers more than
6 hours from that time. CSL and EVE files are day-wide — they are loaded for every session
in the OSCAR day, not just the one whose mask-on time matches. On multi-session days, the
first session's CSL/EVE (e.g. starting at 8:20 PM) was being compared against the second
session's mask-on time (e.g. 3:22 AM), producing a ~7-hour difference that triggered the
corruption check even though the EDF header was correct.

**Fix:** Removed the `repairEDFStartFromSession()` call from `LoadCSL()` and `LoadEVE()`.
The check remains in place for session-specific file types (BRP, SAD, PLD) where the
session mask-on time is a valid reference. Closes GitLab #194.

---

## 2026-05-28 - Purge all device data: spurious "Could not delete" warnings for .000/.001 files

**File:** `oscar/SleepLib/session.cpp` (`Session::Destroy()`)

**Symptom:** Debug log filled with "Could not delete …/Summaries/xxxxxxxx.000" and
"…/Events/xxxxxxxx.001" warnings whenever "Data > Advanced > Purge all device data" was used.

**Root cause:** `Session::Destroy()` unconditionally called `QDir::remove()` on the `.000`
and `.001` legacy session files without first checking whether they exist. OSCAR 2.0 no
longer writes these files (session data is stored in SQLite), so every session generated a
spurious warning.

**Fix:** Removed the file deletion code entirely. `Machine::Purge()` already calls
`removeRecursively()` on the Events and Summaries directories, covering any legacy files.
Also removed the now-unused local `QDir dir` variable.

---

## 2026-05-25 - Export Journal Notes dialog: wrong profile pre-selected; button always shows Close

**Files:** `oscar/exports/journalnotesdialog.{cpp,ui}`

**Symptom 1:** When no profile was open, the Profile combo defaulted to the first profile
in the database instead of being blank.

**Root cause:** `populateProfiles()` only called `setCurrentIndex()` when a match was
found; when no profile was open (`preSelectIndex == -1`), Qt auto-selected index 0.

**Fix:** Added `else { ui->profileCombo->setCurrentIndex(-1); }` so the combo is
explicitly blank when no profile is open.

**Symptom 2:** The Cancel/Close button always showed "Close", even before any export
had been performed.

**Root cause:** The `.ui` file initialised `closeButton` with text "Close" and nothing
changed it after an export completed.

**Fix:** Changed initial text in `.ui` to "Cancel". Added
`ui->closeButton->setText(tr("Close"))` at the end of `on_exportButton_clicked()` so
the label changes to "Close" only after a successful export completes.

---

## 2026-05-25 - Backup dialog closeButton labelled "Close" before any operation runs

**Files:** `oscar/backupdialog.{cpp,ui}`

**Symptom:** The Backup dialog's Cancel/Close button showed "Close" on first open and
reverted to "Close" after a failed or cancelled backup, even though no backup had
completed successfully.

**Root cause:** `backupdialog.ui` initialised `closeButton` with text "Close", and
`onBackupFailed()` reset it to "Close" instead of "Cancel".

**Fix:** Changed the initial text in `.ui` to "Cancel". Changed `onBackupFailed()` to
set the label back to "Cancel". On success the dialog calls `accept()` and closes, so
no "Close" state is needed. Matches the same fix applied to Share and Restore dialogs.

---

## 2026-05-24 - Restore dialog: rename label shows wrong text; dialog hidden by taskbar (#190)

**Files:** `oscar/restoredialog.{h,cpp,ui}`

**Symptom 1:** The conflict-resolution group showed the rename radio as "Rename — import
as <profile name>_restored", which is wrong since the actual rename now produces
"(copy N)" names. The static label was never updated to match the real proposed name.

**Symptom 2:** After pressing Download (cloud link) or Validate (local file), the
infoGroup and nameGroup become visible, growing the dialog. On Windows, if the dialog
was near the bottom of the screen, the expanded portion was hidden behind the taskbar.

**Root cause 1:** The rename radio text was a static string in the .ui file. Nothing
updated it when the conflict group was shown.

**Root cause 2:** `onValidationFinished()` showed groups and grew the dialog but never
checked or corrected the dialog's position relative to the available screen area.

**Fix 1:** Added `computeRenameSuggestion(baseName)` helper (strips existing " (copy N)"
suffix, finds lowest free copy number). Called in `updateConflictForName()` to set the
rename radio label to the exact proposed name each time a conflict is shown.

**Fix 2:** Added `ensureOnScreen()` using `QTimer::singleShot(0, ...)` to run after the
layout engine has computed the new dialog size, then clamps `frameGeometry()` to
`QScreen::availableGeometry()` (which excludes the taskbar). Called at the end of
`onValidationFinished()`.

---

## 2026-05-24 - Restore dialog: rename suggestion stacks suffixes on repeated restores (#189)

**File:** `oscar/restoredialog.cpp` (rename radio toggled handler)

**Symptom:** Restoring the same profile a second time presents a conflict. Clicking the
rename radio appended "_restored" to whatever was in the name field. If that candidate
also conflicted, clicking rename again produced "_restored_restored". For share packages
the suffix also looks wrong: "John (Shared)_restored" (underscore abuts parenthetical).

**Root cause:** The handler blindly appended `"_restored"` to the current name field
text without checking uniqueness, and `setText` → `updateConflictForName` would detect
another conflict on the freshly-appended name, requiring the user to click Rename again.

**Fix:** Replace the append with a loop: strip any existing `" (copy N)"` suffix from
the base name to get the root, then try "root (copy 2)", "root (copy 3)", ... until a
non-conflicting candidate is found. One click on Rename always produces the correct
unique name regardless of how many copies already exist.

---

## 2026-05-24 - Restore dialog: share package not detected when downloaded from cloud (#188)

**Files:** `oscar/restoredialog.cpp` (`onValidationFinished`),
`oscar/database/backup/backup_manifest.{h,cpp}`,
`oscar/database/backup/profile_backup.{h,cpp}`,
`oscar/sharedialog.cpp`, `oscar/backupdialog.cpp`

**Symptom:** When a share .oscar file is downloaded via a cloud link (Dropbox, Google
Drive, etc.) and restored from the Restore Profile dialog, the proposed profile name
is the bare original username — not `"username (Shared)"`. If that username already
exists locally, the conflict-rename path proposes `"username_restored"` instead. The
disk path (browsing for the `share_*.oscar` file) showed `"username (Shared)"` correctly.

**Root cause:** Share-package detection in `onValidationFinished()` checked only whether
`QFileInfo(path).fileName().startsWith("share_")`. The `CloudDownloader` saves to a temp
file named `oscar_download_XXXXXX.oscar`, which does not carry the `share_` prefix, so
the detection always returned `false` for cloud-originated packages.

**Fix:** Added a `package_type` field ("share" or "backup") to the manifest.
`BackupManifest::setPackageType()` writes it; `ProfileBackup::setPackageType()` carries
the value through from the calling dialog. `ShareDialog` sets "share",
`BackupDialog` sets "backup". `RestoreDialog::onValidationFinished()` reads
`package_type` from the manifest first and falls back to the filename check for
old packages that predate this field.

---

## 2026-05-24 - Share dialog Close button stuck as "Cancel" after successful share (#187)

**File:** `oscar/sharedialog.cpp` (`onBackupCompleted`, `onUploadFinished`)

**Symptom:** After a successful share operation (File destination or cloud upload), the
Close button retains the label "Cancel". Clicking it triggers the cancellation branch,
which disables the button as "Cancelling..." — making the dialog impossible to dismiss.

**Root cause:** `on_shareButton_clicked` calls `setUiLocked(true)`, which sets
`m_operationActive = true` and renames the button to "Cancel". Both completion handlers
(`onBackupCompleted` for the File path and `onUploadFinished` for cloud) only called
`closeButton->setEnabled(true)` but never reset `m_operationActive` or the button text.
The click handler checks `m_operationActive` first, so the button always took the cancel
branch rather than calling `reject()`.

**Fix:** Added `m_operationActive = false` and `closeButton->setText(tr("Close"))` in
both completion handlers. The other input controls remain locked intentionally so the
user sees the result before the dialog can be reused.

---

## 2026-05-24 - Window drifts by frame border width on each restart (follow-up to #186)

**File:** `oscar/mainwindow.cpp` (`SetupGUI`, `closeEvent`, `RestartApplication`)

**Symptom:** After fixing #186 (geometry save on restart), the window still opened
one title-bar-height lower than its previous position on every restart triggered
by a profile rename.  The horizontal position could also drift by the thin side-border
width, though that was less noticeable.

**Root cause:** `saveGeometry()` records the client-area origin; `restoreGeometry()`
on Windows reapplies it as if it were the frame origin, shifting the client area down
(and slightly right) by the frame decoration sizes.  The existing Windows-only clamping
code in `SetupGUI` only corrected when the window went off-screen, so the drift was
silently accepted for windows fully on-screen.

**Fix:**
- Both save sites (`closeEvent` and `RestartApplication`) now also write
  `MainWindow/frameTopLeft` (a `QPoint`) from `frameGeometry().topLeft()` when the
  window is not maximized.
- The Windows `QTimer::singleShot` correction pass at startup reads that saved
  frame origin and calls `move()` with the appropriate client-area offset so the
  frame lands at exactly the saved position, then clamps to the available screen area
  as before.

---

## 2026-05-24 - Window geometry not preserved on restart after profile rename (GitLab #186)

**File:** `oscar/mainwindow.cpp` (`RestartApplication`)

**Symptom:** When OSCAR restarts after a profile rename, the main window reopens
with the geometry from the previous normal session exit rather than the geometry
at the time of the rename/restart.

**Root cause:** `closeEvent()` is responsible for saving window geometry to
`QSettings`, but `RestartApplication()` exits via `QApplication::exit()` + `::exit(0)`,
bypassing `closeEvent()` entirely. The geometry saved was therefore stale — from
whatever the last normal session exit recorded.

**Fix:** Added an explicit geometry save block at the top of `RestartApplication()`,
before launching the new process. Uses the same `QSettings` group and key as
`closeEvent()` so the restarted instance reads the correct value on startup.

Closes #186

---

## 2026-05-24 - Profile selector not shown after import or restore

**Files:** `oscar/mainwindow.cpp` (`on_action_Import_OSCAR_Data_triggered`,
           `on_actionRestore_Profile_triggered`)

**Symptom:** After successfully importing or restoring a profile, the new profile
did not appear to the user. The profile list was correctly refreshed in the
background, but OSCAR stayed on whichever tab was active (e.g. Daily), so the
user never saw the updated profile selector. Reported as "didn't show up until
after a restart" — the rename-triggered auto-restart coincidentally landed on
the profile selector tab, which is why the profile appeared after that restart.

**Root cause:** `profileSelector->updateProfileList()` was called correctly, but
`ui->tabWidget->setCurrentWidget(profileSelector)` was never called afterward.

**Fix:** Added `ui->tabWidget->setCurrentWidget(profileSelector)` after
`updateProfileList()` in both import and restore handlers.

Closes #184

---

## 2026-05-24 - Empty Journal/Summaries directory created unnecessarily

**Files:** `oscar/SleepLib/machine.cpp` (`Machine::Load`),
           `oscar/profileimporter.cpp` (`ProfileImporter::copyJournalFolders`)

**Symptom:** After creating a new profile and importing CPAP data, an empty
`Journal_xxx/Summaries/` directory appeared in the profile data folder.  Same
empty directory was also created during a 1.7.1→2.0 profile migration.  In
OSCAR 2.0 all journal data lives in the database; the legacy `Summaries/`
subdirectory is never read or written, so it should not be created.

**Root cause (two paths):**

1. `Machine::Load()` file-based fallback (for machines not yet loaded from DB):
   line 808 unconditionally called `dir.mkpath(summarypath)` before it knew
   whether any `.000` files existed to populate that directory.  For the Journal
   machine this always produced an empty `Summaries/` on the next profile open.

2. `ProfileImporter::copyJournalFolders()` explicitly created the empty
   `Journal_xxx/Summaries/` subdirectory when copying the journal folder
   structure during a 1.7.1 import.

**Fix:**
- `machine.cpp`: Removed the entire "move old files to correct locations" block
  (SleepyHead-era dead code that relocated `.000`/`.001` files from the machine
  root to `Summaries/`/`Events/`).  OSCAR 2.0 never writes `.000` files, so the
  block was permanently unreachable — and its unconditional `mkpath(summarypath)`
  was what created the empty `Summaries/` folder.  The file-based fallback now
  goes straight to reading from the existing `Summaries/` subdirectory; if it
  does not exist, `entryList()` returns empty and no directory is created.
- `profileimporter.cpp`: Removed the three lines that created the empty
  `Summaries/` subdirectory; only the `Journal_xxx/` directory itself is created.

---

## 2026-05-23 - HTML files not copied to build output on plain Build (GitLab #182)

**File:** `oscar/oscar.pro`

**Symptom:** Changes to `Htmldocs/*.html` (e.g. `release_notes.html`) were not reflected
in the Help/About dialog after a plain Build. A Rebuild or manual qmake run was required.

**Root cause:** The copy of `Htmldocs/*.html` → `build/Html/` was done via `system(xcopy)`
/ `system(cp)` calls inside the `.pro` file, which execute only at qmake time — not at
compile time.

**Fix:** Replaced the platform-specific `system()` loops for HTML files with a `COPIES`
entry (`html_copies.files` / `html_copies.path`). qmake's `COPIES` mechanism generates
proper Makefile dependency rules so a plain Build copies any changed HTML files automatically.

---

## 2026-05-22 - Backup success message box appears behind main window (GitLab #180)

**File:** `oscar/backupdialog.cpp` (`BackupDialog::onBackupCompleted`)

**Symptom:** After a successful backup, the QMessageBox confirmation sometimes appeared
behind the main window rather than on top of the backup dialog. The .oscar file was
created correctly, but users saw no notification and were left with a dialog showing
only an active Cancel button and no way to proceed.

**Root cause:** Qt/Windows z-order issue. `createBackup()` runs synchronously in the
main thread, using `QCoreApplication::processEvents()` for UI responsiveness. During
that processing, window focus could shift to the main window. When `onBackupCompleted`
subsequently created the QMessageBox, Qt parented it to the dialog but painted it
behind the already-focused main window.

**Fix:** Call `raise()` and `activateWindow()` on the dialog immediately before
showing the QMessageBox, ensuring the dialog has focus before the box is created.

---

## 2026-05-21 - Yuwell YH-825 BiPAP pressure chart flat at zero (Format C decode targeted CPAP byte)

**File:** `oscar/SleepLib/loader_plugins/yuwell_loader.cpp` (`YuwellFormatC::OpenSession`)

**Symptom:** Yuwell YH-825 BiPAP (model `YH825A`, mode S/T) imports cleanly via the
existing Format C path — hours, tidal volume, respiratory rate, leak, and AHI counts all
populate — but the pressure chart shows a flat zero line for every session. Affects any
Yuwell Format C device running BiPAP mode (S / T / ST / VGPS / AUTOS — modes `0x01-0x05`).
GitLab issue #178.

**Root cause:** `YuwellFormatC::OpenSession` was written against the YH-830 CPAP/APAP and
only handled `mode == YUWELL_FORMATC_CPAP` (`0x00`) and `mode == YUWELL_FORMATC_APAP` (`0x06`).
The other five mode constants in `yuwell_loader.h:12-18` (S / T / ST / VGPS / AUTOS) fell
through to `MODE_UNKNOWN`. More importantly, the per-record decode read `pressure` from
record offset `0x0C`, which is unused (always zero) in BiPAP records — the device stores
IPAP and EPAP as little-endian `u16 * 10` at record offsets `0x02-0x03` and `0x04-0x05`,
which the loader was skipping over via `skipRawData(10)`.

**Fix:**
1. Read IPAP / EPAP from record offsets `0x02-0x05` (replaces the 10-byte skip with two
   `u16` reads and a 6-byte skip — net byte arithmetic unchanged, so CPAP/APAP reads at
   `0x0C` / `0x0D` still hit the right offsets).
2. Allocate `CPAP_IPAP` / `CPAP_EPAP` event lists alongside the existing `CPAP_Pressure`.
3. Branch the per-record emission on `mode`: CPAP/APAP keep emitting `CPAP_Pressure`
   unchanged; the five BiPAP-class modes emit to `CPAP_IPAP` and `CPAP_EPAP` instead.
4. Map the five unhandled BiPAP modes to existing `CPAPMode` enum values: S/T/ST →
   `MODE_BILEVEL_FIXED`, VGPS → `MODE_AVAPS`, AUTOS → `MODE_BILEVEL_AUTO_VARIABLE_PS`.

**Verification:** Candidate decoding confirmed end-to-end against the Yuwell BreathCare
vendor app for the YH-825 sample: ramp from IPAP 8.5 / EPAP 4.5 to IPAP 16 / EPAP 12 over
15 minutes, exact match to the device's actual therapy settings. CPAP/APAP execution path
is byte-identical to the original code when `mode == 0x00` or `mode == 0x06`, preserving
YH-830 behaviour.

---

## 2026-05-21 - DV6 pressure graph invisible on full-night sessions

**File:** `oscar/SleepLib/loader_plugins/intellipap_loader.cpp` (`load6HighResData`)

**Symptom:** CPAP_Pressure graph blank on full-night DV6 sessions with R.BIN high-res data;
same sessions rendered correctly when zoomed in far enough to trigger non-accelerated rendering.
Sessions without R.BIN data (L.BIN only) showed correct pressure via EVL_Event.

**Root cause:** `load6HighResData` created the CPAP_Pressure EventList as `EVL_Waveform`
(1 Hz). The accelerated rendering path in `gLineChart::paint()` maps samples to pixel columns
and draws a vertical line from `min_py` to `max_py` per column. At full-night scale (~43 s
per pixel), slowly-changing pressure produces the same `py` for every sample in a column,
so `min_py == max_py` → zero-length line → invisible. The non-accelerated path (active when
zoomed in) draws actual line segments and works correctly. All other CPAP loaders use
`EVL_Event` for pressure, which always uses the non-accelerated rendering path.

**Fix:** Changed `CPAP_Pressure` in `load6HighResData` from `EVL_Waveform` to `EVL_Event`
with one `AddEvent` per pressure byte (`pressure1` at `ti`, `pressure2` at `ti + 1000`),
matching the BMC loader pattern.

**Note:** Existing sessions imported with the old `EVL_Waveform` format must be reimported
from backup or SD card to display correctly.

---

## 2026-05-20 - Time Alignment code review: 14 issues fixed (oscar2-align-clocks)

**Files:** `oscar/devicetimecorrectiondialog.cpp`, `oscar/driftanalysisdialog.cpp`,
`oscar/SleepLib/machine.cpp`, `oscar/SleepLib/machine.h`,
`oscar/SleepLib/machine_common.h`, `oscar/SleepLib/profiles.cpp`,
`oscar/daily.cpp`, `oscar/database/device_time_correction_repository.cpp`,
`oscar/database/database_schema.cpp`

**Issue 1 (Critical):** Clicking a drift row in Corrections dialog, then nudging + Save,
would silently destroy the drift model. Fix: `populateControlsFromRow` now returns early
for drift rows, leaving `m_staged.id = 0` and showing a read-only mode label.

**Issue 2 (Critical):** Bookmark drift used `sessions.first()->correctionMs()` on a
`Day` returned by `GetDay(date, MT_CPAP)`. Since `Day::sessions` holds all session types,
`first()` could be an oximeter. Fix: loop to find the first MT_CPAP session at all three
call sites in `daily.cpp`.

**Issue 3 (Critical):** Legacy `clockDrift` migration wrote one offset row per existing
CPAP day, so future imports got no correction. Fix: write a single open-ended offset row
from the earliest CPAP date.

**Issue 5 (Important):** Open-ended rows stored as `""` for timezone type but
`"2099-12-31"` for all others. Fix: `effectiveDateRange` now uses `""` for all
open-ended rows; timezone special case retained only for the non-advanced (single-night)
path where timezone is inherently open-ended.

**Issue 7 (Important):** Navigating Daily while a staged correction was in progress
silently discarded it. Fix: `setDate` prompts Save/Discard/Cancel when the date changes.

**Issue 8 (Minor):** Comment said "schema version 16" for the device_time_corrections
table; corrected to version 17.

**Issue 9 (Minor):** Drift model stored slope as `c1 = slope + 1.0` to distinguish from
constant rows. Fix: store raw slope; discriminate on `row.type == "drift"` in
`correctionMs()`; backward-compat in `reloadCorrectionsFromDb` strips the old sentinel.

**Issue 10 (Minor):** SQL used integer literal `c1 = 0` to exclude drift rows in upsert
predicates. Fix: removed the redundant `c1 = 0` predicate — `type = :type` already
excludes drift rows when called with non-drift types.

**Issue 11 (Minor):** Linear regression in `onFitDrift` used the uncentered formula,
losing precision for short windows. Fix: mean-centered formula.

**Issue 13 (Minor):** `rebuildMachine` was duplicated identically in both dialogs. Fix:
added `Machine::reloadCorrectionsFromDb(Machine*)` static helper; both dialogs delegate
to it. Handles old drift row sentinel encoding (`c1 - 1.0` backward compat).

**Issue 15 (New, not in original review):** "Date Added" column in Corrections history
showed UTC date from `applied_at`; adding a correction at 11 pm local time appeared as
the next calendar day. Fix: convert to local time before truncating to date.

---

## 2026-05-20 - Time Alignment Codex review: 3 remaining bugs fixed (oscar2-align-clocks)

**Files:** `oscar/devicetimecorrectiondialog.cpp`, `oscar/SleepLib/profiles.cpp`,
`oscar/SleepLib/machine.cpp`

**Bug A (P1 — Stale preview after device switch):** `onDeviceChanged` discarded the
`previous` parameter, so `clearStagedAndRevert()` rebuilt the *new* machine rather than
the one that held stale preview rows. Fix: named the `previous` parameter and, when a
staged preview exists, call `resetToNewMode()` + `rebuildMachine(prevMach)` using the
machine extracted from `previous`.

**Bug B (P2 — Corrections loaded after day bucketing):** `LoadMachineData` loaded
correction rows after `mach->Load()` and `calculateDailySummaries()`, so `AddSession`
bucketed sessions by raw (uncorrected) timestamps. Fix: moved `reloadCorrectionsFromDb`
into the existing machine loop before `mach->Load()`; removed the now-redundant
post-load loop. `Machine::AddSession` now computes a corrected first-time
(`rawFirst + correctionMs(rawDate)`) for split-time and day-bucketing comparisons.

**Bug C (P3 — Offset display wraps above 24 h):** `QTime(0,0,0).addMSecs()` silently
wraps at 24 h, so a stored offset > 86 400 000 ms displayed as a smaller value. Fix:
clamp the displayed milliseconds to 86 399 999 and show "Offset exceeds 24 hours and
cannot be displayed precisely." in the warning label.

---

## 2026-05-20 - Dreem import gives no warning when user selects Excel file instead of CSV (#176)

**Files:** `oscar/SleepLib/loader_plugins/dreem_loader.cpp`

**Symptom:** The Dreem web portal exports data as .xlsx (sometimes with a .csv extension).
Importing such a file produced 0 sessions and only a generic "There was a problem
opening" notification with no explanation.

**Fix:** Detect ZIP/XLSX magic bytes (`PK`) at the start of the file in `openCSV()`.
If found, show a QMessageBox directing the user to use Apple2Dreem instead.

---

## 2026-05-20 - Event Flags graph shown with misleading rebuild message on sleep-stage-only days (#174)

**Files:** `oscar/Graphs/gFlagsLine.cpp`

**Symptom:** On a day with only Dreem sleep stage data and no CPAP data, the Event Flags
graph appeared and displayed "Database Outdated. Please Rebuild CPAP Data", which is
irrelevant — no CPAP data exists for that day.

**Root cause:** `m_rebuild_cpap` was set to true whenever no FLAG/SPAN channels were
found, regardless of whether any CPAP sessions existed. The `m_empty` fallback and
`isEmpty()` also treated any day events (including sleep stage events) as sufficient to
show the graph.

**Fix:** Guard all three checks on `!m_sessions.isEmpty()` so the graph is hidden and
the rebuild message suppressed when no CPAP sessions are present for the day.

---

## 2026-05-20 - Dreem import does not remember last used directory (#175)

**Files:** `oscar/SleepLib/common.h`, `oscar/mainwindow.h`, `oscar/mainwindow.cpp`

**Symptom:** The Dreem CSV import dialog opened in the last oximetry directory instead
of the last Dreem import directory.

**Fix:** Added `STR_PREF_LastDreemPath` preference key; added optional `folderPrefKey`
parameter to `importNonCPAP()`; Dreem import action passes the new key.

---

## 2026-05-19 - Remove ANGLE/Qt5 dead code from graphics engine selection (#173)

**Files:** `oscar/main.cpp`, `oscar/mainwindow.h`, `oscar/mainwindow.cpp`,
`oscar/newprofile.cpp`, `oscar/preferencesdialog.cpp`,
`oscar/SleepLib/common.h`, `oscar/SleepLib/common.cpp`

**Changes:**
1. Shift-key at launch now toggles between OpenGL and Software engines (previously always forced Software).
2. Added `--OpenGL` command-line option (case-insensitive) to force OpenGL engine.
3. Made `--legacy` command-line option case-insensitive.
4. Removed `GFX_ANGLE` from the engine enum and all supporting code — ANGLE is not supported in Qt6.
5. Removed `--hires`/`--hiresoff` command-line stubs — Qt5 leftovers with no effect.
6. Removed `-l` (force-login) command-line option and `force_login` parameter from
   `RestartApplication()` — obsolete since password protection was removed.

---

## 2026-05-18 - Create ZIP of OSCAR database — memory, UI, and progress bugs (#172)

**Files:** `oscar/zip.cpp`, `oscar/zip.h`, `oscar/mainwindow.cpp`, `oscar/mainwindow.ui`,
`oscar/SleepLib/progressdialog.cpp`

**Symptom:** Zipping a large database (28 GB) exhausted all system RAM and froze OSCAR.
Progress bar stayed at 0%, abort button showed as "Ab" and did nothing.

**Root causes:**
1. `ZipFile::AddFile` called `f.readAll()` — loaded the entire file into a `QByteArray`
   before passing to miniz. Fix: replaced with `mz_zip_writer_add_read_buf_callback`
   streaming (64 KB chunks via callback).
2. Read callback had no `processEvents()` — event loop never spun during compression,
   freezing the UI and preventing abort. Fix: `notifyReadProgress()` called every 4 MB.
3. Abort button added to `hlayout` after dialog was already shown — squeezed to "Ab".
   Fix: moved to its own row in `vlayout`.

---

## 2026-05-16 - Per-database dialog preferences migrated from QSettings to app_preferences

**Files:** `database/reports_initializer.cpp`, `exports/report_exporter.cpp`,
`importprofile.cpp`, `backupdialog.cpp`, `restoredialog.cpp`, `sharedialog.cpp`,
`exports/journalnotesdialog.cpp`

**Symptom/issue:** Several dialog preferences (last export folder, last backup dir,
CSV report version, report tree state, etc.) were stored in the system-wide QSettings
registry. Users with multiple databases saw settings bleed across databases.

**Root cause:** QSettings stores to a single system-wide registry key regardless of
which database is open. The correct store for per-database UI preferences is the
`app_preferences` table in the current database.

**Fix:** Replaced `QSettings` save/load with `AppPreferencesRepository` in all
affected dialogs. Categories used: `Reports`, `ReportExporter`, `ImportProfile`,
`BackupDialog`, `RestoreDialog`, `ShareDialog`, `JournalNotesDialog`. The `csv_reports_version`
key was a functional bug — it caused reports to not be re-seeded correctly when
switching databases.

---

## 2026-05-16 - Add Purge Range of Days feature (#166)

**Files:** `oscar/mainwindow.ui`, `oscar/mainwindow.h`, `oscar/mainwindow.cpp`,
`oscar/purgerangedaysdialog.h`, `oscar/purgerangedaysdialog.cpp`, `oscar/oscar.pro`

**Summary:** Added "Purge Range of Days..." under Data → Advanced. New `PurgeRangeDaysDialog`
collects start/end date (pre-filled with current day) and data-type selection (6 radio buttons
matching single-day purge). A `QProgressDialog` loop calls new helper `purgeDayData()` per day
with cancel support; `purgeDay()` refactored to reuse the same helper.

---

## 2026-05-16 - Profile selector sort order not remembered across restarts (#165)

**File:** `oscar/profileselector.cpp`

**Symptom:** Clicking a column header in the profile selector to sort the list worked
during the session, but the sort reset to "Profile name, ascending" on every restart.

**Root cause:** `updateProfileList()` always called `sortByColumn(0, Qt::AscendingOrder)`
unconditionally, discarding any user-chosen sort.

**Fix:** Connected `QHeaderView::sortIndicatorChanged` in the constructor to save the
chosen column and order via `AppPreferencesRepository` under category `"ProfileSelector"`
(keys `sortColumn`, `sortOrder`). `updateProfileList()` now restores those values
instead of hard-coding column 0. Stored in the database's `app_preferences` table, so
the preference is per-database — users with multiple databases get a different
remembered sort for each.

---

## 2026-05-16 - VREM loader: data not saved to database (Plots Disabled + no machine save)

**File:** `oscar/SleepLib/loader_plugins/vrem_loader.cpp`

**Symptoms:** Same "Plots Disabled" symptom as ResVent (#162) and Yuwell (#163), but
with an additional failure mode: no data was ever committed at all because
`machine->Save()` was entirely absent from `VREMLoader::Open()`.

**Root causes (two):**
1. `OscarDataParser()` called `session->Store()` before `Machine::Save()`, same race
   as ResVent/Yuwell — on first import the machine has no DB ID so Store() skips all
   database writes but clears the changed flag.
2. `Machine::Save()` was never called anywhere in `VREMLoader::Open()`, so even for
   re-imports the machine record and its sessions were never finalized in the database.

**Fix:**
- Removed the premature `session->Store(machine->getDataPath())` call from
  `OscarDataParser()`.
- Added `machine->Save()` in `Open()` after `OscarDataParser()` returns, inside the
  per-OD-folder loop so each machine is saved when its sessions are complete.

---

## 2026-05-16 - Yuwell loader: all graphs show "Plots Disabled" after navigating between days

**File:** `oscar/SleepLib/loader_plugins/yuwell_loader.cpp`

**Symptoms:** Same as ResVent (#162). Import data via Yuwell loader; daily page looks
correct for the last day, but navigating to any earlier day shows "Plots Disabled" on
all graphs. Navigating back to last day also then shows "Plots Disabled".

**Root cause:** Identical to ResVent bug. All four Yuwell format variants (FormatA,
FormatB, FormatC, FormatD) called `sess->Store()` before `Machine::Save()`, so on
first import the machine had no database ID. Store() skipped all database writes but
cleared the changed flag, leaving events permanently absent from the database.

**Fix:** Removed the four premature `sess->Store(mach->getDataPath())` calls (one per
format variant). Machine::Save() → SaveTask::run() handles the full save after the
machine is in the database.

---

## 2026-05-16 - ResVent loader: all graphs show "Plots Disabled" after navigating between days

**File:** `oscar/SleepLib/loader_plugins/resvent_loader.cpp`

**Symptoms:**
1. Import data via ResVent loader; last day displayed on daily page looks correct.
2. Navigate to any earlier day → all graphs show "Plots Disabled".
3. Navigate back to last day → also shows "Plots Disabled".

**Root cause:**
`LoadSession()` called `session->Store()` directly before `Machine::Save()` was called.
`Session::Store()` checks `s_machine->getDatabaseId() > 0` before saving to DB; on a
first import the machine has no DB ID yet, so the `else` branch fires and database
storage is **skipped entirely**. However, `s_changed` is still cleared (`s_changed = false`).

When `Machine::Save()` subsequently runs it:
1. Saves the machine to the database (giving it a DB ID).
2. Skips `SaveTask` for these sessions because `IsChanged() == false`.
3. Calls only `StoreToDatabase()` (session metadata) for sessions with `sessionRowId == 0`.
   `StoreEvents()` is **never called** — waveform/event data is never written to the DB.

The daily page initially shows the last day correctly because sessions are still in memory
with `s_events_loaded = true`. When the user navigates to another day, `daily.cpp`
calls `d->CloseEvents()` on all loaded days, purging in-memory events. Subsequent
display of any day tries to reload events from the DB, finds none, and shows
"Plots Disabled".

**Fix:**
Removed the direct `session->Store(machine->getDataPath())` call from `LoadSession()`.
`Machine::Save()` → `SaveTask::run()` already calls `Store()` correctly: it executes
*after* `Machine::SaveToDatabase()` has given the machine a DB ID, so both
`StoreToDatabase()` and `StoreEvents()` succeed and events reach the database.

**Note:** Existing profiles that were imported with the broken loader will have session
metadata in the database but no event data. Those sessions need to be deleted and
re-imported (delete the machine's data folder and re-import from SD card).

---

## 2026-05-16 - Weight graph: wrong units in legend, "lb oz" format on Y-axis

**Files:** `oscar/SleepLib/common.h`, `oscar/SleepLib/common.cpp`,
`oscar/Graphs/gYAxis.cpp`, `oscar/Graphs/gOverviewGraph.cpp`

**Symptoms:**
1. The upper-right legend of the Weight graph showed the raw stored value (kg) as a plain
   decimal (e.g. "80.37") regardless of the user's selected unit system.
2. Y-axis tick labels displayed "176lb 6oz" — the ounces component adds length without
   useful precision for axis scale marks.
3. After editing the profile to change the unit system, Y-axis labels kept the old unit
   system until OSCAR was closed and re-opened; the legend updated immediately because it
   already read directly from `p_profile`.

**Root causes:**
1. The legend value was computed with `QString::number(f, 'f', 2)` for all non-time
   channels, with no special case for `Journal_Weight`.
2. `weightString()` (used by `gYAxisWeight::Format`) always returned full precision
   including ounces in English mode, but Y-axis values are in raw kg so they never round
   to whole pounds after conversion.
3. `gYAxisWeight` stored `m_unitsystem` at construction time. Editing the profile via
   "Edit Profile" does not call `RebuildGraphs`, so the axis object kept the old unit
   system while the legend (which reads `p_profile->general->unitSystem()` at paint time)
   updated correctly.

**Fix:**
- Added `bool rounded = false` parameter to `weightString()`. When `true` in English mode,
  rounds to the nearest pound and omits ounces (`"176lb"`); in metric, rounds to whole kg.
- `gYAxisWeight::Format` now reads unit system live from `p_profile->general->unitSystem()`
  at paint time (falling back to `m_unitsystem` only when no profile is open), and passes
  `rounded = true`.
- The legend section in `gOverviewGraph::paint` checks for `code == Journal_Weight` and
  calls `weightString(f, p_profile->general->unitSystem())` instead of raw `%2f`.

---

## 2026-05-16 - Overview page tooltips obscured by large mouse pointer (GitLab #160)

**Files:** `oscar/Graphs/layer.h`, `oscar/Graphs/gGraphView.cpp`,
`oscar/Graphs/gSummaryChart.cpp`, `oscar/Graphs/gSessionTimesChart.cpp`,
`oscar/Graphs/gOverviewGraph.cpp`

**Symptom:** Graph tooltips were positioned with their upper-left corner at the mouse
hotspot, causing a large cursor to overlap and obscure part of the tooltip content.

**Root cause:** `gToolTip::calculateRect` always placed the tooltip at `moveTo(m_pos)`,
ignoring the stored `m_alignment` value. All Overview chart tooltips originate in
`gSummaryChart::draw` (shared by `gAHIChart`, `gUsageChart`, `gTTIAChart`, etc.), not
`gOverviewGraph`.

**Fix:** Added `TT_AlignBottomLeft` (and `TT_AlignBottomRight`) to the `ToolTipAlignment`
enum and handled them in `calculateRect` via `moveBottomLeft`/`moveBottomRight`. Updated
all Overview tooltip call sites to use `TT_AlignBottomLeft` with the anchor offset 20px
right of the mouse hotspot, placing the tooltip above and to the right of the cursor.

---

## 2026-05-15 - Statistics: right-align Days/AHI/FL columns in Changes to Device Settings (GitLab #158)

**File:** `oscar/statistics.cpp` — `GenerateRXChanges()`

**Symptom:** Days, AHI, and FL columns in the Changes to Device Settings table were
left-aligned; numbers looked untidy and FL values crowded the adjacent Machine column.

**Fix:** Right-aligned all three numeric columns with `padding-right` (16px for Days and
AHI, 32px for FL) in both the `<th>` header and `<td>` data cells.

---

## 2026-05-15 - Bookmarks search field: visibility, clear button, no-profile crash (Mantis #201, GitLab #157)

**Files:** `oscar/mainwindow.ui`, `oscar/mainwindow.cpp`

**Symptoms:**
1. Bookmarks panel search field had white text on light blue background — poor visibility.
2. Clear button showed a refresh/reload icon — wrong semantic.
3. Clicking the clear button (or pressing Enter in the filter field) with no profile open
   crashed OSCAR with a null pointer dereference.

**Root causes:**
1. & 2. Cosmetic choices in the original UI design.
3. `updateFavourites()` dereferenced `p_profile` on its first line with no null guard.

**Fixes:**
- Search field: black text on `rgb(220,232,255)` background; magnifying glass icon
  (`edit-find.png`) shown via `QLineEdit::addAction(LeadingPosition)`, hidden as soon as
  the user starts typing.
- Clear button: replaced `refresh.png` icon with bold `✕` text; turns red on hover.
- Added `if (!p_profile) return;` guard at the top of `updateFavourites()`.

---

## 2026-05-14 - New profile inherits channel settings from currently-open profile

**File:** `oscar/SleepLib/profiles.cpp` — `Profiles::Create()`

**Symptom:** Channel settings (enabled state, colours, thresholds) for a newly-created
profile were copied from the profile that was open at the time, instead of using the
schema-defined defaults.

**Root cause:** `schema::channel` is a global `ChannelList` that is mutated in place by
`Profile::loadChannels()` when a profile opens. `Profiles::Create()` never reset this
global before building the new profile, so `initializeChannelsFromSchema()` (called on
the new profile's first open, when no DB row exists) wrote the previously-open profile's
values instead of true defaults.

**Fix:** At the start of `Profiles::Create()`, just before constructing the new `Profile`
object, save the current profile's channel state (so it can be reloaded later) and then
call `schema::resetChannels()` to restore hardcoded defaults. The new profile is then
initialized from a clean schema.

---

## 2026-05-15 - Session bar on Event Flags graph shifts when other graphs are zoomed

**File:** `oscar/Graphs/gFlagsLine.cpp` — `gFlagsGroup::paint()`

**Symptom:** When the user zooms in on any other graph (Flow Rate, Pressure, etc.), the
session-boundary bar at the top of the Event Flags graph shifts position while the flag
lines themselves stay fixed — causing the bar to be misaligned with the events it brackets.

**Root cause:** `gFlagsGroup::paint()` derived its `minx`/`maxx` from
`g.graphView()->GetXBounds()`, which returns the graphview's current *zoomed* bounds
(`m_minx`/`m_maxx`). But the Event Flags graph has `blockZoom() = true`, so its flag
lines (`gFlagsLine::paint()`) correctly ignore the zoom and use `g.rmin_x`/`g.rmax_x`
(full-day bounds). On every zoom-triggered repaint the session bar used zoomed coords
while the flags used full-day coords, producing a visible mismatch.

**Fix:** Applied the same `blockZoom()` guard used in `gFlagsLine::paint()`: when
`g.blockZoom()` is true, use `g.rmin_x`/`g.rmax_x`; otherwise use `g.min_x`/`g.max_x`.
This makes the session bar, the time-range text, and the line cursor all consistent
with the flags.

---

## 2026-05-14 - Session bar misaligned on Event Flags graph when oximeter data widens day range (#155)

**Files:** `oscar/Graphs/gFlagsLine.cpp`, `oscar/Graphs/gFlagsLine.h`

**Symptom:** When a day includes both CPAP and oximeter data, the gray session-boundary
bars at the top of the Event Flags graph appeared at wrong positions — shifted and
incorrectly scaled — producing an on/off pattern that missed session transitions.
On CPAP-only days the bar was correct (coincidentally).

**Root cause:** `gFlagsGroup::paint` computed session bar positions using
`m_start`/`m_duration` (CPAP-only time bounds set at `SetDay` time), but the graph's
X axis spans the full day range including non-CPAP data. When oximeter sessions
extended beyond the CPAP window, the mapping was wrong.

**Fix:** Replaced `m_start`/`m_duration` with the already-computed `minx`/`maxx`/`dur`
variables (from `g.graphView()->GetXBounds()`) used by all other content in the same
paint function. Removed the now-unused `m_start` and `m_duration` members.

---

## 2026-05-14 - Window drifts down by title-bar height on restart when sized to fill work area (#153)

**Files:** `oscar/mainwindow.cpp` (constructor)

**Symptom:** On Windows 11, if the user manually resizes the main window to fill the
available desktop area (top of screen to top of taskbar), then exits and restarts OSCAR,
the window is restored approximately one title-bar height lower, hiding the bottom
behind the taskbar.

**Root cause:** Qt's `restoreGeometry()` on Windows misplaces a manually-maximised
(but not Qt-maximised) window by one title-bar height on restore.

**Fix:** After the window is shown, clamp `frameGeometry()` to `screen()->availableGeometry()`
using a `QTimer::singleShot(0, ...)`. Guarded with `#ifdef Q_OS_WIN` because Linux/X11
WM decorations arrive asynchronously after `show()`, making `frameGeometry()` unreliable
at that point.

---

## 2026-05-14 - Regression: Fusion theme restart fails with lock error (#152)

**Files:** `oscar/preferencesdialog.cpp` (`PreferencesDialog::Save()`)

**Symptom:** Changing the Fusion theme in Preferences/Appearance triggers a restart.
The new OSCAR instance shows "This OSCAR database folder is already open in another
instance of OSCAR." The old OSCAR window briefly remains visible then disappears.

**Root cause:** `Save()` calls `RestartApplication()` which calls `CloseProfile()`
(setting `p_profile = nullptr`) then spawns the new process and calls
`QApplication::exit()`. `Save()` still returned `true`, so `on_okButton_clicked()`
called `accept()`, `pd.exec()` returned `Accepted`, and
`on_actionPreferences_triggered()` executed post-dialog code that dereferenced the
now-null `p_profile`. The crash killed the old process before `lockFile.unlock()`
ran, leaving `oscar.lock` on disk. The new process found the stale lock and aborted.

**Fix:** Return `false` from `Save()` immediately after calling `RestartApplication()`.
This prevents `accept()` from being called, skips the post-dialog UI rebuilds, and
allows the process to exit cleanly via `QApplication::exit()` with `lockFile.unlock()`
properly called.

---

## 2026-05-14 - Popout graph window does not repaint until mouse is moved over it (#151, Mantis #316)

**Files:** `oscar/Graphs/gGraphView.cpp` (`gGraphView::popoutGraph()`)

**Symptom 1:** When a graph is popped out (right-click → Pop out Graph), the new dock
window opens but the graph is not painted correctly until the user moves the mouse over
the window.

**Symptom 2:** When a graph in the popout window is closed (X button on dock widget),
the remaining graphs in the window are not repainted until the user moves the mouse over
the window.

**Root cause:** For Qt6 `QOpenGLWidget`, calling `update()` immediately after showing a
new dock window posts a deferred paint event, but the FBO content may not be composited
back to the screen until the window finishes settling (show/activation events, focus
changes). The result sits in the FBO unseen until user interaction (mouse move) triggers
another paint cycle. On close, no repaint was triggered at all for remaining graphs.

**Fix:** Two changes in `popoutGraph()`:
1. Added a `QTimer::singleShot(50ms)` callback that calls `gv->update()` and
   `dock->update()` after the window has fully settled (show/activation/focus events done).
2. Connected `newDockWidget::visibilityChanged` to a lambda that calls `update()` on all
   remaining `gGraphView`s and the dock window whenever a dock widget is shown or hidden.

---

## 2026-05-13 - Records panel shows AHI instead of RDI when RDI preference is set (#150)

**Files:** `oscar/statistics.cpp`

**Symptom:** In the Records panel right sidebar, the Best/Worst AHI records and Best/Worst
Device Settings sections always displayed AHI values and labels, even when the user had
selected RDI in Preferences > CPAP. Mantis #298.

**Root cause:** `UpdateRecordsBox()` hardcoded `day->calcAHI()`, `tr("AHI: %1")`, and
`rx.ahi` throughout, with no check of `p_profile->general->calculateRDI()`. The
`rxAHILessThan` sort comparator also always used `rx.ahi`, so the best/worst device
settings were selected on the wrong metric when RDI mode was active.

**Fix:** Added `bool rdi` / `QString ahitxt` variables derived from the preference at
the top of `UpdateRecordsBox()`. AHI Records now use `day->calcRDI()` vs `day->calcAHI()`
for the stored value, and all labels use `ahitxt`. Best/Worst Device Settings use
`rx.rdi / rx.hours` vs `rx.ahi / rx.hours`, and `rxAHILessThan` sorts by `rdi` when
the preference is set.

---

## 2026-05-13 - Right sidebar does not restore last-used panel on startup (#149)

**Files:** `oscar/SleepLib/appsettings.h`, `oscar/SleepLib/appsettings.cpp`, `oscar/mainwindow.h`,
`oscar/mainwindow.cpp`

**Symptom:** On startup, the right sidebar always showed the Records panel, regardless of
which panel (Navigation, Bookmarks, or Records) was last selected by the user. Mantis #16.

**Root cause:** `mainwindow.cpp` hardcoded `ui->toolBox->setCurrentIndex(2)` and no preference
was saved when the user switched panels.

**Fix:** Added `RightSidebarPanel` preference (default 2) to `AppSettings`. Startup now restores
the saved index. New `on_toolBox_currentChanged(int)` slot persists the index whenever the user
switches panels.

---

## 2026-05-13 - Print report omits last graph when it is the only graph on the last page (#148)

**Files:** `oscar/reports.cpp`

**Symptom:** When printing a Daily (or Overview) report, the last graph is silently omitted
if it would be the only graph on the final page.

**Root cause:** `graph_slots` (line 393) was computed as
`int(6 - (virt_height - top) / (full_graph_height + normal_height))`, which is
`floor(6 * top / virt_height)`. For a typical header height of ~400 px on a ~2896 px
virtual page, this evaluates to `floor(0.83) = 0`. However the drawing loop increments
`top` by `full_graph_height + normal_height/2` per graph — slightly less than `virt_height/6`
— so only 5 graphs actually fit on page 1, not 6. With `graph_slots = 0`, the formula
`pages = ceil(N / 6)` gives 1 page for N=6, but 2 are actually needed. When graph 6 triggers
a page break, `page(2) > pages(1)` fires and the loop breaks before drawing the last graph.

**Fix:** Replaced `floor` with `ceil`:
`graph_slots = (int)ceilf((float)graphs_per_page * (float)top / virt_height)`.
Mathematically proven that `ceil(6*top/H) >= 6 - k_max` always, so `pages` is never
under-estimated. At worst, `pages` over-estimates by 1 (footer says "Page 1 of 2" when
there is only 1 page), which only occurs for very small headers.

---

## 2026-05-13 - Crash on F12 screenshot when no profile is open (#147)

**Files:** `oscar/mainwindow.cpp`

**Symptom:** Pressing F12 to take a screenshot with no profile open causes OSCAR to crash
immediately after the screenshot file is saved successfully.

**Root cause:** `MainWindow::saveProfilePath()` dereferences `p_profile` unconditionally
via `(*p_profile)[folderProfileName] = pathName`. When no profile is open, `p_profile` is
null, causing a null pointer dereference.

**Fix:** Added `if (p_profile)` guard in `saveProfilePath()`, consistent with the adjacent
`profilePath()` function which already handles the null case correctly.

---

## 2026-05-12 - Journal data (notes, feelings, weight) not saved after a session is deleted (#135)

**Files:** `oscar/daily.cpp`, `oscar/daily.h`

**Symptom:** After clearing all content from a Daily Notes page and navigating away
(which correctly deletes the empty journal session), returning to that date and entering
new data (notes, feelings, weight) would not be saved. Intermittent for notes; consistent
for feelings and weight on dates with no CPAP data.

**Root cause (new — this fix):** `deleteJournalSession` removes the session from the
machine's sessionlist and the Day's sessions list but leaves the Day object in
`profile->daylist`. On the next visit, `CreateJournalSession` calls `p_profile->GetDay(date)`
which finds the empty Day, then calls `cday->first()` which returns 0 (since `Day::first()`
skips journal sessions and there are no other sessions). `Machine::AddSession` rejects any
session with `first == 0`, so the new session is never added to the Day, and `GetJournalSession`
cannot find it in `Unload` — nothing is saved.

**Root cause (original — this fix also):** When all journal content was cleared, the empty
journal session was persisted to the database with only a `LastUpdated` timestamp, causing
the calendar to show a bold date with no actual data.

**Fix:**
- Added `isJournalSessionEmpty()` static helper and `Daily::deleteJournalSession()` to
  remove the session from both memory and DB when all meaningful fields are cleared.
- In `CreateJournalSession`, added `cday->first() > 0` guard so that an empty Day
  (no non-journal sessions) falls back to the default 20:00 timestamp rather than
  passing 0 to `Machine::AddSession`.

---

## 2026-05-11 - System Information dialog missing database schema version (#144)

**File:** `oscar/main.cpp`

**Symptom:** Help > System Information showed OSCAR version, Qt version, OS, graphics engine,
and data directory, but did not include the database schema version.

**Root cause:** No entry for schema version was added to the build info list.

**Fix:** Added `#include "database/database_schema.h"` to `main.cpp` and called
`addBuildInfo()` with `DatabaseSchema::CURRENT_SCHEMA_VERSION` immediately after
the data directory line.

---

## 2026-05-11 - Import from 1.7.1: graph height (and other app settings) not applied until restart (#142)

**File:** `oscar/profileimporter.cpp` — `ProfileImporter::migrateAppSettings()`

**Symptom:** After importing a profile from OSCAR 1.7.1, settings such as Graph Height on
the Preferences > Appearance page show a stale value from the previous OSCAR 2.0 session
instead of the value from the imported 1.7.1 profile.

**Root cause:** `migrateAppSettings()` correctly copies keys from 1.7.1's `Preferences.xml`
into `p_pref`, but `AppSetting`'s in-memory member variables (e.g. `m_graphHeight`) are
only populated at construction and are not updated when `p_pref`'s values change at runtime.

**Fix:** After saving the migrated settings, delete `AppSetting` and reconstruct it from
the updated `p_pref` so the in-memory cache immediately reflects the imported values.

---

## 2026-05-11 - File > Database > Delete hangs UI when database is on slow storage (NAS)

**File:** `oscar/database/database_delete_dialog.cpp` — `DatabaseDeleteDialog::onDeleteClicked()`

**Symptom:** OSCAR becomes unresponsive during deletion when the database folder is on a
NAS or other slow storage, because `QDir::removeRecursively()` was called on the main thread.

**Root cause:** Synchronous recursive directory removal on the main (UI) thread.

**Fix:** Moved `removeRecursively()` to a background thread via `QtConcurrent::run()`.
A `QFutureWatcher` + `QEventLoop` keeps the UI pumping while waiting. A 2-second
`QTimer` defers showing a frameless "Deleting, please wait…" dialog, so fast
local-SSD deletions complete silently with no extra UI.

---

## 2026-05-11 - File > Database > Open accepts any folder, not just ones with a database

**File:** `oscar/mainwindow.cpp` — `MainWindow::on_actionDatabaseOpen_triggered()`

**Symptom:** User could select any arbitrary folder via File > Database > Open and OSCAR
would attempt to open it as a database, even if it contained no oscar.db file.

**Root cause:** No validation that the selected folder actually contains `oscar.db` before
calling `switchToDatabase()`.

**Fix:** Added a check for `oscar.db` in the selected folder; shows a warning and returns
early if the file is absent.

---

## 2026-05-11 - Profile import does not migrate channel customizations from 1.7.1

**Files:** `oscar/profileimporter.cpp`, `oscar/SleepLib/profiles.cpp`, `oscar/SleepLib/profiles.h`

**Symptom:** When importing a 1.7.1 profile, user-modified channel settings (colors, labels,
enabled state, thresholds) set via the Preferences dialog were not preserved — the imported
profile always had schema defaults.

**Root cause:** The importer called `initializeChannelsFromSchema()` unconditionally after
import, ignoring `channels.dat` in the source profile folder, which is where 1.7.1 stores
per-profile channel customizations.

**Fix:** Added an optional `channelsDatDir` parameter to `loadChannelsFromDat()` and
`migrateChannelsToDatabase()`. The importer now calls `migrateChannelsToDatabase(sourcePath)`
to read `channels.dat` directly from the source profile without copying the file.
Falls back to schema defaults when no `channels.dat` is present.

---

## 2026-05-11 - File > Edit Profile opens wrong profile when none is open

**File:** `oscar/mainwindow.cpp` — `on_action_Edit_Profile_triggered()`

**Symptom:** When no profile was open and a profile was selected in the profile list,
File > Edit Profile opened the editor for the last-used profile instead of the selected one.

**Root cause:** The menu handler always used `AppSetting->profileName()` (last opened
profile) rather than the currently selected profile in the list.

**Fix:** When no profile is open (`p_profile == nullptr`), use `selectedProfileName()`
(delegates to `profileSelector->selectedProfileName()`) to get the selected row's profile
name. Also added early return if the name is empty (nothing selected).

---

## 2026-05-11 - Show disk usage fails when no profile is open (#138)

**File:** `oscar/profileselector.cpp` — `on_diskSpaceInfo_linkActivated()`

**Symptom:** Clicking "Show disk usage information" on the Profiles page showed nothing
if no profile was currently open. Also showed data for the open profile rather than the
selected profile.

**Root cause:** Handler passed `p_profile` (the open profile, which is null when no
profile is open) instead of the selected profile.

**Fix:** Use `selectedProfileName()` to look up the selected profile in
`Profiles::profiles` and pass that pointer to `getProfileDiskInfo()`.

---

## 2026-05-10 - Edit Profile dialog layout unified (#137)

**Files:** `oscar/newprofile.ui`

**Symptom:** Group boxes on the CPAP, Doctor Info, and Profile pages did not align with the
bottom of the group boxes on the Personal Info page. Address/notes text areas had excessive
vertical expansion. On the Profile page, the Profile Info and Locale Settings group boxes
split the page evenly instead of the Profile Info box being sized to its content.

**Fix:** Removed forced 40 px spacers from the Contact Info, Doctor/Clinic Info, and Locale
Settings group boxes. Added minimum height (55 px) to address and notes text areas so they
stay compact by default but can grow. Added internal expanding spacers to group boxes with
only fixed-height fields (Personal Info, Locale Settings) so extra page height is absorbed
cleanly within each box. Set Locale Settings group box to Expanding vertical size policy so
it takes all remaining height on the Profile page. Reduced dialog height from 450 to 440 px.

---

## 2026-05-10 - Remove password support (#136)

**Files:** `oscar/newprofile.ui`, `oscar/newprofile.h`, `oscar/newprofile.cpp`,
`oscar/profileselector.h`, `oscar/profileselector.cpp`, `oscar/mainwindow.h`,
`oscar/mainwindow.cpp`, `oscar/SleepLib/profiles.h`,
`oscar/database/user_info_repository.h`, `oscar/database/user_info_repository.cpp`,
`oscar/database/backup/profile_backup.h`, `oscar/database/backup/profile_backup.cpp`

**Symptom:** OSCAR supported weak SHA1-based profile passwords with no recovery mechanism;
forgotten passwords required manual database editing beyond most users' abilities.

**Fix:** Removed all password support. The `password_hash` column is left as a dead column
in the `user_info` table (no schema migration). Old backups restoring into the current
schema will simply ignore the column value since the application never reads it.

---

## 2026-05-09 - Edit SQL window illegible on KDE dark mode (#134)

**Files:** `oscar/sqleditor.cpp`

**Symptom:** On Kubuntu 24.04 LTS with dark mode enabled, the SQL text in the Edit SQL
window was white on white (unreadable).

**Root cause:** Qt's `setColorScheme(Light)` hint is not guaranteed on KDE Plasma; the
KDE platform theme can still supply a dark-palette text color (white) to individual
widgets. `QPlainTextEdit` had no explicit text color, so white text appeared against the
default light background. The read-only path also set `background-color: #f0f0f0` without
a matching `color`, compounding the issue.

**Fix:** Set explicit `background-color: white; color: black` on `queryEdit` in the
constructor and in both branches of `setReadOnly()`.

---

## 2026-05-07 - Feelings: range button invisible and wrong value/format in print report (#132)

**Files:** `oscar/daily.cpp`, `oscar/daily.ui`, `oscar/reports.cpp`

**Symptom:** The Feelings (ZombieMeter) range toggle in the Daily Notes panel was
styled as plain text with no border, making it invisible as a button. The Print Daily
report showed the internal storage value (user-entered × 10) with a "/10" suffix
instead of the user-facing value.

**Root cause:** The `Units10_100` QPushButton had `border: none` and zero padding,
rendering it as flat text. The report code passed the raw stored value (0–100 scale)
to `QString::arg()` without dividing by 10 and appended "/10" literally.

**Fix:** Removed the flat styling so the button renders natively; changed label text
to "0..10" / "0..100" (range format); added `margin-left` to separate it from the
spinbox. Report now divides by 10 (normal mode) or shows as-is (zombie mode), with
"(0 .. 10)" / "(0 .. 100)" appended for clarity. No "/10" suffix.

---

## 2026-05-07 - Channel names persist in wrong language after language change (#130)

**Files:** `oscar/SleepLib/schema.h`, `oscar/SleepLib/profiles.cpp`

**Symptom:** After switching OSCAR's language and back, graph legends, Statistics
labels, and Preferences event/waveform lists remained in the previous language for
the affected profile. Only profiles that had been open during a non-English session
were affected.

**Root cause (two parts):**
1. `saveChannelsToDatabase()` never wrote a language tag to `profile_preferences`,
   so stored channel names had no language stamp. The tag in `profile_preferences`
   was stale (left over from the old `channels.dat` era).
2. `loadChannelsFromDatabase()` compared the stale `profile_preferences.Language`
   tag against QSettings — both happened to be `en_US` — so `changing_language`
   stayed false and the German/Dutch names were loaded unchanged.

**Fix:**
- Added `defaultFullname()` / `defaultLabel()` / `defaultDescription()` accessors
  to `Channel` in `schema.h` (expose schema-init defaults set by `tr()` at startup).
- `saveChannelsToDatabase()`: writes `STR_PREF_Language = currentLanguage()` before
  saving, stamping the language the names are actually in.
- `loadChannelsFromDatabase()`: added a secondary check that counts how many stored
  `fullname` values differ from the current-language schema defaults. If more than
  half differ, the stored data is treated as being in the wrong language and schema
  defaults are used instead. Threshold logic preserves legitimate user customizations
  (which affect only a small subset of channels).

---

## 2026-05-07 - oscar.lock not released before teardown, blocking restart (#131)

**Files:** `oscar/main.cpp`

**Symptom:** When OSCAR restarted for a language change, the new process (which
waits 1 second via `-p`) sometimes failed to acquire oscar.lock and exited, leaving
no OSCAR running.

**Root cause:** The `QLockFile lockFile` destructor runs after `mainapp.exec()`
returns, but only after `delete mainwin`, `Profiles::Done()`, and
`DatabaseManager::close()` complete. On a slow machine or large database this
teardown can take more than 1 second, causing a race with the new process.

**Fix:** Call `lockFile.unlock()` immediately after `mainapp.exec()` returns,
before any teardown, so the new instance can always acquire the lock in time.

---

## 2026-05-07 - Feelings: /10 suffix shown in UI and wrong value printed (#129)

**Files:** `oscar/daily.cpp`, `oscar/reports.cpp`

**Symptom:** The Feelings (ZombieMeter) spinbox in the Daily page Notes panel showed
a "/10" label after the value. The Print Daily report printed the internal storage value
(user-entered × 10, e.g. 70) with a "/10" suffix instead of the user-facing value (e.g. 7.0).

**Root cause:** `setup_ZombieUIWidgets()` set the `Units10_100` button text to `"/10"` in
normal mode. The report code passed the raw stored value (0–100 scale) to `QString::arg()`
without dividing by 10, and appended `"/10"` literally in the format string.

**Fix:** `daily.cpp`: changed `/10` label text to empty string in normal mode.
`reports.cpp`: divide stored value by 10 and format with 1 decimal in normal mode;
show as-is (0 decimals) in zombiemeter mode. Removed `/10` suffix from format string.

---

## 2026-05-05 - Early startup log messages missing from debug.txt (#126)

**Files:** `oscar/logger.h`, `oscar/logger.cpp`

**Symptom:** Log messages from approximately the first 0.6 seconds of startup were
absent from debug.txt, though they appeared on stderr and in the UI debug pane.

**Root cause:** `initializeLogger()` installs the message handler but `logToFile()` is
not called until after the data directory and lock file are confirmed (~220 lines later
in main.cpp). During that window `m_logStream` was null, so `appendClean()` silently
skipped the file write.

**Fix:** Added `m_preFileBuffer` to `LogThread`. Messages are stashed there when the
file is not yet open. `logToFile()` flushes the buffer to the file before proceeding.

---

## 2026-05-06 - Lock file not released on early exit (#128)

**Files:** `oscar/main.cpp`

**Symptom:** If OSCAR exits early (e.g. database version too new), `oscar.lock` remains
on disk. The next OSCAR startup sees the stale lock and refuses to open the database,
reporting it is already open in another instance.

**Root cause:** `QLockFile` was heap-allocated (`new`) and never `delete`d. The destructor
(which calls `unlock()` and removes the lock file) was never invoked on early `return`.

**Fix:** Changed to stack allocation so the destructor fires automatically on any exit
path from `main()`.

---

## 2026-05-06 - Journal note "0" during startup migration (#125, follow-up)

**Files:** `oscar/profileimporter.cpp`

**Symptom:** Even after the 2026-05-04 fix, testers who triggered the 1.7.1 import via
the startup migration flow (fresh install with no 2.0 data) still saw journal notes as "0".
Users who imported via the menu after a normal startup were unaffected.

**Root cause:** `schema::init()` was called at line 218 of `importProfile()` — after
`migrateMetadata()` at line 103. During startup migration `main.cpp`'s `schema::init()`
(at line 971) had not yet run, so `Journal_Notes` was still 0 (C++ global default).
`sess->settings.contains(0)` returned false even though the key `0xd000` was in the
QHash, so the note was not stored correctly.

**Fix:** Moved `schema::init()` to before the `migrateMetadata()` call in `importProfile()`.
Removed diagnostic-only logging added in commits 65e1b870 and da0ef95d.

---

## 2026-05-04 - Journal note imported from 1.7.1 displayed as "0" (#125)

**Files:** `oscar/SleepLib/session.cpp`, `oscar/profileimporter.cpp`

**Symptom:** After importing a profile from OSCAR 1.7.1, a journal note could appear
as the text "0" instead of the original note content.
**Root cause:** Two bugs: (1) `StoreToDatabase()` stored a `Journal_Notes` row even when
the note text was empty (e.g. if the QVariant from the 1.7.1 binary file failed to
deserialise as a QString), resulting in `data_type="text"` with an empty `json_value`.
(2) `LoadFromDatabase()` required `!json_value.isEmpty()` for the "text" branch; an
empty `json_value` fell through to the numeric fallback and loaded `setting.value` (= 0),
which displayed as "0".
**Fix:** `StoreToDatabase` now skips writing `Journal_Notes` when the text is empty.
`LoadFromDatabase` no longer requires non-empty `json_value` for the "text" branch
(empty note loads as "", not 0); logs a `qWarning` when `json_value` is empty.
`migrateJournalFromSource()` added a post-load check: if `Journal_Notes` is present
but not a non-empty QString, it is discarded and a warning logged with the session ID.

---

## 2026-05-04 - Two instances on same folder cause data corruption (#124)

**Files:** `oscar/main.cpp`

**Symptom:** Two OSCAR instances opened on the same database folder simultaneously
would cause SQLite write conflicts and profile state corruption.
**Root cause:** No exclusion mechanism prevented concurrent access to the same folder.
**Fix:** Added `QLockFile` at `<datadir>/oscar.lock`. On startup, after the data folder
is confirmed writable, OSCAR acquires the lock. If the lock is already held, the user
is warned and OSCAR exits. `setStaleLockTime(0)` prevents auto-recovery of stale locks.

---

## 2026-05-04 - Database switch: process not started; Preferences.xml created (#123)

**Files:** `oscar/mainwindow.cpp`, `oscar/SleepLib/preferences.cpp`

**Symptom 1:** Switching databases via File ▸ Database ▸ Recent sometimes failed to
start the new OSCAR process.
**Root cause:** The new process read `Settings/AppData` from QSettings immediately after
the current process wrote it, with no guarantee the registry write was visible in time.
**Fix:** Pass `--datadir <path>` on the command line so the new process receives the
target database path directly, bypassing QSettings entirely for the initial open.

**Symptom 2:** `Preferences.xml` was created in the data folder on each database switch.
**Root cause:** `switchToDatabase()` wrote `Settings/AppData` (the new path) to QSettings
before the current process exited. During shutdown, `GetAppData()` returned the new path
while `p_pref->p_filename` still pointed at the old database. `Preferences::Save()` saw
the mismatch and fell through to its XML fallback.
**Fix:** The current process no longer updates `Settings/AppData` — the new process sets
it via `--datadir`. Also hardened `Preferences::Save()` to never write XML for the
`"Preferences"` object under any condition; logs a warning and returns if the DB path
guard fails.

---

## 2026-05-02 - File > Database menu: Preferences.xml re-creation and empty Recent list (#120)

**Files:** `oscar/mainwindow.cpp`, `oscar/SleepLib/appsettings.h`, `oscar/SleepLib/appsettings.cpp`

**Symptom 1:** `Preferences.xml` was re-created after a database switch.
**Root cause:** `switchToDatabase()` called `RecentDatabases::setActive()` before `p_pref->Save()`.
`setActive()` updates `Settings/AppData` immediately, so `GetAppData()` returns the new
path. `Preferences::Save()` guards with `p_filename.startsWith(GetAppData())`; once the
path changed, that check failed and Save() fell through to writing the XML file instead
of saving to the database.
**Fix:** `switchToDatabase()` now calls `CloseProfile()` and `p_pref->Save()` first
(while `GetAppData()` still points at the old database), then writes `Settings/AppData`
and adds to the Recent list.

**Symptom 2:** Database menu disappeared (and Recent list appeared empty) after switching
to a different database.
**Root cause:** `showDatabaseMenu` was stored in the `app_preferences` database table.
A new or different database defaults the preference to `false`, hiding the Database menu.
**Fix:** `showDatabaseMenu` getter/setter now use `QSettings` directly, so the preference
persists across all database switches.

---

## 2026-05-02 - File > Database menu (#120)

**Files:** `oscar/mainwindow.cpp` / `.h` / `.ui`, `oscar/SleepLib/appsettings.h` / `.cpp`,
`oscar/preferencesdialog.ui` / `.cpp`, `oscar/main.cpp`, `oscar/oscar.pro`,
new: `oscar/database/recent_databases.{h,cpp}`, `oscar/database/database_delete_dialog.{h,cpp}`

**Feature:** Added `File ▸ Database ▸ New / Open / Recent / Delete` submenu enabling
support staff and developers to create, switch between, and delete OSCAR databases
in arbitrary locations. Visibility controlled by a new "Add database menu items"
checkbox in Preferences ▸ General (default: off).

**Also removed** orphan `on_actionChange_Data_Folder_triggered` (and its UI action)
which had been hidden and unreachable since the data-folder feature was reworked.

---

## 2026-05-02 - Calendar: hasoxi not set for CPAP machines with built-in oximeter

**File:** `oscar/daily.cpp` — `Daily::UpdateCalendarDay()`

**Symptom:** Calendar day was not shown in green (CPAP + Oxi color) for CPAP machines
that have a built-in oximeter (SpO2/Pulse data stored in the CPAP day rather than
a separate MT_OXIMETER day).

**Root cause:** `hasoxi` was only set when `FindDay(date, MT_OXIMETER)` returned
a non-null pointer. CPAP machines with integrated oxi store their data in the CPAP
day's channels (OXI_SPO2, OXI_Pulse, POS_Movement) rather than in a separate
MT_OXIMETER day object.

**Fix:** Reused the `FindDay(MT_CPAP)` pointer and added a `channelHasData()` check
for OXI_SPO2, OXI_Pulse, and POS_Movement channels on the CPAP day.

---

## 2026-05-01 - BMC loader: Default y-axis mode identical to Auto-Fit

**File:** `oscar/SleepLib/session.cpp` — `Session::StoreDB()`

**Symptom:** On the Daily page, toggling between Default and Auto-Fit y-axis scaling modes
produced no visible change for graphs loaded from the BMC loader (e.g. Pressure, Flow Rate,
Tidal Volume). The correct behavior was present in OSCAR 1.7.1 and for ResMed profiles
in OSCAR 2.0.

**Root cause:** Three-step failure chain:
1. The BMC loader never calls `setPhysMin`/`setPhysMax` for its channels (only `CPAP_FLG`
   gets explicit values).
2. `Session::StoreDB()` read physMin/physMax using `m_physmin.value(id, 0)` — raw map
   access with 0 as default — rather than calling `Session::physMin(id)`. For BMC channels
   the maps were empty, so 0/0 was stored in the database for all non-FLG channels.
3. On reload from DB, `m_physmin[id] = 0` and `m_physmax[id] = 0` were cached. When
   `gGraph::physMinY()`/`physMaxY()` iterated layers, the gLineChart layer was skipped
   (`tmp == 0 && physMaxy() == 0`), returning 0 for both. Default mode's early-return
   guard (`if (maxy > miny)`) failed (0 is not > 0), causing it to fall through to the
   same Auto-Fit rounding path. In OSCAR 1.7.1 this did not manifest because there was
   no database layer — `physMin(id)` was always lazily evaluated from live event data.

**Fix:** Changed `StoreDB` to call `physMin(id)` / `physMax(id)` (the accessor functions)
instead of the raw map read. The accessors trigger the lazy `floor(Min)` / `ceil(Max+0.5)`
computation for channels that never had `setPhysMin`/`setPhysMax` called, and return the
correct explicit value for channels (like CPAP_FLG) that did.

**Note:** Existing BMC sessions already in the database have physMin=0/physMax=0 stored and
will continue to show the old behaviour until the profile is re-imported.

---

## 2026-05-01 - Graph title overflows into adjacent graphs when title is too long

**File:** `oscar/Graphs/gGraph.h`, `oscar/Graphs/gGraph.cpp` — `gGraph::paint()`

**Symptom:** On the Daily page, the vertical graph title text could overflow into adjacent
graphs when the graph panel was too short to fit the title at the default font size.

**Root cause:** The title was always drawn with `mediumfont` (12pt bold) regardless of
the available graph height. No clipping was applied, so long titles spilled into
neighbouring panels.

**Fix:** Before queuing the title text, compare `fm.horizontalAdvance(title)` (which
becomes the visual height after 90° rotation) against the graph height. If it exceeds
the height, reduce the font point size by 1pt steps down to a 7pt minimum. The adjusted
font is stored in a new `QFont m_titleFont` member on `gGraph` so the `QFont *` stored
in the text queue remains valid until it is consumed by `DrawTextQue`. (Closes #113)

---

## 2026-05-01 - BMC loader: "Plots Disabled" for SpO2/Pulse when no Oxi accessory attached

**File:** `oscar/SleepLib/loader_plugins/bmc_loader.cpp` — `BmcLoader::ExportSession()`

**Symptom:** After importing BMC data without an Oxi accessory, the Daily graph page
immediately showed "Plots Disabled" for SpO2 and Pulse Rate charts. Navigating away and
back to the same day made the charts disappear (correct behaviour), but they should not
have appeared at all on the first view.

**Root cause:** The loader unconditionally called `AddEventList()` for `OXI_SPO2` and
`OXI_Pulse` before the waveform loop, creating empty `EventList` objects. The graph code
renders "Plots Disabled" for any channel present with zero data points. The empty lists
are never persisted to the database (session save skips empty EventLists), so they
vanish on reload — ResMed's loader never exhibits this because it only creates a channel
after confirming it has at least one valid sample.

**Fix:** Changed to lazy initialisation — `wSpO2` and `wPulse` start as `nullptr` and are
only created (via `AddEventList`) on the first packet with a non-zero SpO2 or pulse value.
If no Oxi data is present in the session, neither EventList is created.

---

## 2026-05-01 - BMC legacy loader: missing first N minutes of session

**File:** `oscar/SleepLib/loader_plugins/bmcDataParsing.cpp` — `BmcData::FindValidSessions()`

**Symptom:** OSCAR displayed BMC sessions missing a variable number of minutes from the
beginning, compared to BMC's PAP-Link software.

**Root cause:** The December 2025 commit (e3834b9c) inverted the waveform crumb selection
logic. Crumbs are sparse index timestamps spaced 0x1000 (4096) packets apart in each
waveform file. The old code selected the **last crumb strictly before** `StartTimestamp`,
then `ReadWaveforms` read forward from that point and kept only packets within the session
window. The new code instead selected the **first crumb at-or-after** `StartTimestamp`,
causing `ReadWaveforms` to begin reading up to ~68 minutes into the session (the crumb
interval), silently discarding all waveform packets before that crumb.

**Fix:** Restored the original direction of the crumb search: iterate crumbs and keep
updating `chosenCrumb` while `crumb.Timestamp < usrSession.StartTimestamp`, then break.
The `!chosenCrumb.Timestamp.isValid()` guard from the new code was retained.

---

## 2026-04-30 - Warn when importing from a different CPAP machine (#107)

**Files:** `oscar/mainwindow.cpp` — `MainWindow::selectCPAPDataCards()`;
`oscar/SleepLib/profiles.h` — `CPAPSettings`; `oscar/preferencesdialog.ui/cpp`;
`oscar/database/preferences_repository.cpp`

**Symptom / Feature:** No warning was shown when a user imported from an SD card belonging
to a different CPAP machine than the one previously imported into the profile. This could
silently mix data from two machines into one profile.

**Root cause:** The import flow had no serial-number cross-check before proceeding.

**Fix:** In `selectCPAPDataCards()`, after `PeekInfo()` retrieves the card's serial:
1. Check if the path is on a physical removable (FAT/VFAT) drive via `getDriveList()`.
2. If so, and if the new `WarnOnDifferentSDCard` CPAP preference is enabled (default: true),
   compare the card's serial against the most-recently-imported CPAP machine in the profile
   (`p_profile->GetMachine(MT_CPAP)`).
3. If both serials are non-empty and differ, show a `QMessageBox::Warning` naming both
   machines and require the user to click **Continue**; **Cancel** aborts the import.
The check is skipped for folder/hard-drive/network imports (Use Case #3).
A new "Warn when SD card is from a different machine" checkbox in Preferences → Import lets
users with two machines on one profile (Use Case #2/4) disable the check.

---

## 2026-04-30 - Pop-out Time at Pressure graph shows incorrect X-axis (#109)

**Files:** `oscar/Graphs/gGraphView.cpp` — `gGraphView::popoutGraph()`,
`oscar/Graphs/MinutesAtPressure.h` — `MinutesAtPressure::CloneInto()`

**Symptom:** On the Daily page, popping out the Time at Pressure graph produces a window
where the X-axis (pressure axis) is enlarged/cut off and only part of the graph is shown.

**Root cause:** In `popoutGraph()`, the new pop-out `gGraphView` (`gv`) has `m_minx = 0`
and `m_maxx = 0` throughout its lifetime. `setDay()` is called before any graphs are added
to `gv`, so `ResetBounds()` finds an empty graph list and never sets `m_minx`/`m_maxx`.
When `MinutesAtPressure::RecalcMAP::setSelectionRange()` calls `gv->GetXBounds()`, it
receives `(0, 0)` instead of the day's actual time range. The recalculation then uses
`minTime = 0, maxTime = 0`, producing an incorrect pressure distribution. An existing
workaround in `updateTimesForEventList` partially compensated for the zero minTime but
failed entirely when clock drift was non-zero.

**Fix 1:** After setting `newgraph->min_x`/`max_x` in `popoutGraph()`, immediately copy
those values into `gv->m_minx`/`gv->m_maxx`. This ensures `GetXBounds()` returns the
correct day range for the recalculation.

**Fix 2:** Copy `initialized` in `CloneInto()` so the pop-out renders immediately with
the cloned pressure distribution data rather than showing a blank first frame.

---

## 2026-04-30 - Overview graph heights reset when File/Preferences OK clicked (#108)

**File:** `oscar/overview.cpp` — `Overview::RebuildGraphs()`

**Symptom:** After the user manually adjusts graph heights on the Overview page, opening
File/Preferences and clicking OK (even with no changes) resets all heights to the default.

**Root cause:** `mainwindow.cpp` unconditionally calls `overview->RebuildGraphs(true)` after
the Preferences dialog closes. Inside `RebuildGraphs`, `GraphView->LoadSettings("Overview")`
restored the correct heights, but the `if (reset)` block that follows immediately called
`GraphView->resetLayout()` (twice), overwriting every graph height with the global default.

**Fix:** Moved `LoadSettings("Overview")` and `settingsLoaded = true` to after the
`if (reset)` block, so saved heights are applied last and are not clobbered by `resetLayout()`.

---

## 2026-04-30 - Overview pressure graph scaled to setting instead of observed pressure (#104)

**File:** `oscar/Graphs/gPressureChart.cpp` / `gPressureChart.h`

**Symptom:** On the Overview page, the pressure bar chart's top bar and Y-axis maximum
were always driven by the machine's IPAPHi (or equivalent) pressure setting ceiling,
not by the pressure actually observed during therapy. For bilevel auto machines the top
bar showed the setting even when observed peak pressure was meaningfully lower.

**Root cause:** `populate()` called `addSlice(CPAP_IPAPHi)` for all auto/variable bilevel
modes, which reads `day->settings_max(CPAP_IPAPHi)` — the setting, not observed data.

**Fix:** Added `addObservedIPAPMax()` which reads `Day::Max(CPAP_Pressure)` (peak of the
combined pressure waveform stored in the session summary), falling back to
`Day::Max(CPAP_IPAP)` then to the setting if neither has data. All five
`addSlice(CPAP_IPAPHi)` calls in `populate()` replaced with `addObservedIPAPMax()`.
Added `Maxy()` override returning `m_maxy + 1` for 1 cmH2O headroom above the bars.

---

## 2026-04-29 - Extend line cursor through Event Flags graph (#102)

**File:** `oscar/Graphs/gFlagsLine.cpp` (`gFlagsGroup::paint`)

**Symptom:** The green vertical line cursor on the Daily page did not extend through the
Event Flags graph, breaking visual continuity.

**Root cause:** `gFlagsGroup::paint()` never implemented the cursor line drawing that
`gLineChart` and `gOverviewGraph` both have. No architectural barrier — simply an omission.

**Fix:** Added the standard 10-line cursor block (same pattern as `gLineChart::paint`)
after the outline rect is drawn, so the green line renders on top of the graph border.

---

## 2026-04-29 - Show calendar day in italic if it has a bookmark (#99)

**File:** `oscar/daily.cpp` (`Daily::UpdateCalendarDay`)

**Feature:** Calendar days that have at least one bookmark are now displayed in italic,
consistent with the existing visual encoding (bold = journal data, underline = position
data, colour = CPAP/oximeter presence).

**Implementation:** Retained the `Day*` pointer from `FindDay(date, MT_JOURNAL)` and
added a `hasbookmarks` check using `settingExists(Bookmark_Start)` plus a non-empty
list test on the journal session. `setFontItalic(true)` is applied to the
`QTextCharFormat` when the check passes.

---

## 2026-04-29 - Welcome page hides Oximetry block when oxi data comes from CPAP

**File:** `oscar/welcome.cpp` (`Welcome::GenerateOxiHTML`)

**Symptom:** The Welcome page's oximetry info block (icon, frame, "Most recent
Oximetry data...") was hidden for users whose CPAP machine has built-in oximetry,
because no dedicated `MT_OXIMETER` machine exists.

**Root cause:** `GetMachines(MT_OXIMETER)` returned an empty list, so
`haveoximeterdata` stayed false and the block was hidden. `LastDay(MT_OXIMETER)`
likewise returned an invalid date for the report link.

**Fix:** When no dedicated oximeter machine has data, fall back to checking
`channelAvailable("SPO2"|"Pulse")`. If oxi channels are present, set
`haveoximeterdata = true` with `oxiSourceType = MT_CPAP`. For the displayed date,
walk back from the last CPAP day to find one with SPO2/Pulse data; otherwise use
`LastDay(MT_CPAP)`. Mirrors the fix to `Statistics::GenerateCPAPUsage`.

---

## 2026-04-29 - Statistics page hides Oximeter section when oxi data comes from CPAP

**Files:** `oscar/statistics.h`, `oscar/statistics.cpp`
(`StatisticsRow::value`, `Statistics::GenerateCPAPUsage`)

**Symptom:** The "Oximeter Statistics" block on the Statistics page was not shown for
users whose CPAP machine has built-in oximetry (oxi channels stored in MT_CPAP
sessions rather than a dedicated MT_OXIMETER machine).

**Root cause:** The section-skip guard called `countDays(MT_OXIMETER, ...)` which
returns 0 when there is no dedicated oximeter machine, so `skipsection = true` hid
the entire block. All downstream `calcWavg/calcMin/etc.(channel, MT_OXIMETER, ...)`
calls also returned zero/null because `GetGoodDay(date, MT_OXIMETER)` cannot find
days whose only sessions are of type MT_CPAP.

**Fix:** Before the row-rendering loop in `GenerateCPAPUsage`, detect which machine
type actually holds oximetry data: `MT_OXIMETER` if a dedicated oximeter has data in
the report range, `MT_CPAP` if the SPO2/Pulse channels are available in CPAP sessions
but no dedicated oximeter data exists. This `oxiSourceType` is then substituted for
`MT_OXIMETER` in the `SC_HEADING` day-count check, the `SC_DAYS_HEADER` data lookups,
and passed as a new `typeOverride` parameter to `StatisticsRow::value()`, which uses
it instead of the row's nominal `type` for all Profile-level calc calls.

---

## 2026-04-29 - Viatom import silently fails after 1.7.1 migration (#97)

**Files:** `oscar/SleepLib/profiles.cpp` (`Profile::CreateMachine`),
`oscar/SleepLib/machine.cpp` (`Machine::SaveToDatabase`)

**Symptom:** After migrating a profile from OSCAR 1.7.1, importing Viatom oximeter
data appeared to succeed ("Imported 5 sessions") but all sessions were silently
discarded. All oximeter plots on the Daily page showed as disabled. Debug log
contained `UNIQUE constraint failed: machines.profile_id, machines.machine_id`
followed by "Machine not in database yet, skipping database storage" for every session.

**Root cause:** The 1.7.1 migration stored the Viatom machine with `serial_number=""`
in the DB (machines.xml had no serial element). `loadMachinesFromDatabase()` indexed
it in `MachineList["Viatom"][""]`. The Viatom loader called `CreateMachine()` with
`serial="25C2303495"` — not found under the empty key — so a new `Machine` object was
created with the same `m_id` but `m_database_id=0`. `SaveToDatabase()` tried to INSERT,
hitting `UNIQUE(profile_id, machine_id)`. With `m_database_id==0`, `Session::Store()`
logged a warning and returned without saving any events.

**Fix 1 (`CreateMachine`):** After the folder-scan determines `id` is non-zero, scan
the loader's existing `MachineList` entries by `m_id`. If found under a different serial
key, re-index it to the new serial and return the existing machine (preserving
`m_database_id`).

**Fix 2 (`SaveToDatabase`):** When `findBySerialLoaderAndProfile()` returns no match,
fall back to `findByProfileAndMachineId()`. If that finds a machine with the same
loader, adopt its DB ID and update the serial in the DB.

---

## 2026-04-28 - viatom_loader: SpO2=100 triggers spurious "Untested Data" warning

**File:** `oscar/SleepLib/loader_plugins/viatom_loader.cpp`

**Symptom:** Importing from a WellUe/Viatom OsRing_S oximeter triggered the
"Your Viatom device generated data that OSCAR has never seen before" warning
dialog, even though the data was valid.

**Root cause:** The SpO2 valid range check was `61–99%`. The OsRing_S can
record `SpO2=100`, which is a legitimate reading but fell outside the check,
producing an `UNEXPECTED_VALUE` warning.

**Fix:** Raised the upper bound from 99 to 100 in both `ParseFileViatom` (the
main Viatom path) and `ParseFilePOD2` (the POD2 path).

---

## 2026-04-27 - daily_summaries: drop bogus per-machine dimension (schema v16, #95)

**Files:** `oscar/database/database_schema.{h,cpp}`,
`oscar/database/daily_summary_repository.{h,cpp}`,
`oscar/database/backup/profile_restore.cpp`,
`oscar/SleepLib/profiles.cpp`, `oscar/mainwindow.cpp`,
`Notes/DATABASE_SCHEMA_REFERENCE.md`

**Symptom:** None visible to users — but the schema was internally
inconsistent and three repository finders were unreachable from any call
site that used their default arguments.

**Root cause:** `daily_summaries` was defined with `machine_id` as part of
its natural key (`UNIQUE(profile_id, date, machine_id)`), even though a
row is a profile-day rollup that already aggregates across every machine
contributing sessions to the OSCAR day. Every caller passed `machineId =
0` intending "combined", but `calculateFromDay()` silently rewrote that
to the first enabled CPAP session's machine id; `create()` then stored
the row with that id rather than NULL. Meanwhile `findByProfileAndDate`,
`findRange`, and `exists` translated `machineId = 0` into
`WHERE machine_id IS NULL`, so they could never read back what the writer
had stored. The mismatch was invisible because none of those three
finders had any in-tree callers.

**Fix (schema v15 → v16):**
- Dropped `machine_id` column, its FK to `machines`, and
  `idx_daily_summaries_profile_machine`.
- Changed `UNIQUE(profile_id, date, machine_id)` to
  `UNIQUE(profile_id, date)`.
- `migrateV15ToV16` rebuilds the table (SQLite cannot drop a column
  inside a UNIQUE constraint without a rebuild). The data copy is
  `INSERT OR REPLACE` ordered to prefer machine-bound rows over NULL ones
  on the (profile_id, date) key — defensive only; current databases have
  exactly one row per (profile_id, date) already.
- `DailySummaryRepository` API simplified: the optional `machineId`
  parameter is gone from `findByProfileAndDate`, `findRange`,
  `calculateAndStore`, `calculateAndStoreFromDay`, and `exists`.
  `DailySummaryData::machineId` removed.
- Backup compatibility: v15 backups restore cleanly because
  `executeSqlFile()`'s existing `validColumns` filter (built from
  `PRAGMA table_info`) silently drops columns absent from the v16
  schema. The dead FK-remap special case for
  `daily_summaries.machine_id` was removed.

**Side effect:** Deleting a machine no longer touches `daily_summaries`
(previously the FK was `ON DELETE SET NULL`). This is consistent with
the new "rollup is a profile-day, not a machine-day" semantics.

---

## 2026-04-27 - CSV Export Wizard: rename/description not persisted to database (#94)

**Files:** `oscar/database/report_tree_model.{h,cpp}`,
`oscar/exports/report_exporter.cpp`

**Symptom:** Renaming a user report/folder or editing a description in the
CSV Export Wizard appeared to succeed, but changes were lost on close/reopen.

**Root cause (rename):** `ReportTreeModel` did not override `setData()`.
When Qt commits an inline edit (F2 or right-click → Rename), it calls
`QAbstractItemModel::setData()` with `Qt::EditRole`. Without the override,
the base `QStandardItemModel::setData()` ran — updating only the in-memory
item text — while `renameNode()` (which writes to DB) was never called.

**Root cause (description):** `onEditDescription()` discarded the bool
return value of `updateDescription()`, so silent DB failures showed no error.

**Fix:** Override `setData()` in `ReportTreeModel` to detect `Qt::EditRole`
changes on user non-root nodes, persist the new name to the database, then
fall through to the base class to update the model text. Added
`QMessageBox::warning` in `onEditDescription()` when `updateDescription()`
returns false.

---

## 2026-04-26 - Purge oximetry: stale stats remain in daily_summaries (#91)

**Files:** `oscar/mainwindow.cpp` — `MainWindow::purgeDay()`,
`MainWindow::on_actionPurgeCurrentDaysOximetry_triggered()`

**Symptom:** After purging oximetry data for a day, `daily_summaries.spo2_avg`,
`pulse_avg`, `pulse_min`, `pulse_max`, and `has_oximetry` retain the old values.
Statistics pages continue to show oximetry stats that no longer exist.

**Root cause:** `Session::Destroy()` calls `SessionRepository::remove()` which
cascade-deletes `session_summaries` (has `ON DELETE CASCADE` from sessions).
However, `daily_summaries` has no FK relationship to sessions — it is only linked
to profiles and machines — so deleting sessions has no effect on it. Neither purge
code path called `DailySummaryRepository::calculateAndStoreFromDay()` or
`invalidateDate()` after destroying the sessions.

**Fix:** After the session-destroy loop in both purge paths, look up the updated
`Day*` for the affected date. If sessions remain (e.g. CPAP data still present),
call `calculateAndStoreFromDay()` to recalculate the row via INSERT OR REPLACE.
If no sessions remain (day was entirely purged), call `invalidateDate()` to delete
the now-stale row.

---

## 2026-04-26 - SpO2 and pulse data missing from daily_summaries after oximetry import (#89)

**Files:** `oscar/mainwindow.cpp` — `MainWindow::importNonCPAP()`;
`oscar/oximeterimport.cpp`

**Symptom:** After importing oximetry data (via the oximeter wizard or file-based
loaders), `daily_summaries.spo2_avg`, `pulse_avg`, `pulse_min`, `pulse_max`, and
`has_oximetry` remain 0/false even though data is correctly in `session_channels`
and `session_summaries`.

**Root causes:**

1. `calculateDailySummaries()` is called only once at profile load when
   `existingCount == 0`. After any subsequent import no code calls it again, so
   the `daily_summaries` rows for affected dates are never updated with oximetry
   stats. (`mach->day[date]` and `profile->daylist[date]` point to the same Day
   object, so a single call sees combined CPAP+oxi sessions; `INSERT OR REPLACE`
   updates existing rows idempotently.)

2. `oximeterimport.cpp` called `count()`, `Min()`, and `Max()` for OXI_SPO2 and
   OXI_Pulse but not `avg()` or `wavg()`, leaving those fields as 0 in both
   `session_channels` and, consequently, `daily_summaries.spo2_avg`/`pulse_avg`.

**Fixes:**

- `importNonCPAP()`: call `p_profile->calculateDailySummaries()` after a
  successful import (`res > 0`). Covers Viatom, Dreem, Zeo, Somnopose.
- `oximeterimport.cpp`: add `session->avg()` and `session->wavg()` for
  OXI_Pulse and OXI_SPO2 before `mach->Save()`, then call
  `p_profile->calculateDailySummaries()` after `StoreMachines()`.

---

## 2026-04-25 - session_channels p95/median stored as 0 for OXI_Pulse and other channels (#88)

**Files:** `oscar/SleepLib/machine.cpp` — `Machine::Save()`;
`oscar/SleepLib/session.cpp` — `Session::StoreToDatabase()`, `Session::updateCountSummary()`

**Symptom:** `session_channels.p95` and `session_channels.median` contain 0 for OXI_Pulse
(channel_id 6144) and potentially other channels even though data was correctly imported.

**Root causes:**

1. **Primary — overwrite on re-save:** `Machine::Save()` called `StoreToDatabase()` on
   every session unconditionally. When `Profile::Save()` triggered `Machine::Save()` (e.g.
   user changes preferences), events were not loaded in memory. `StoreToDatabase()` deleted
   the existing `session_channels` rows and re-inserted them with p95=0.

2. **Missing fallback in `StoreToDatabase()`:** When events weren't loaded, the code
   unconditionally set p95=0 even when `m_valuesummary`/`m_timesummary` were already
   populated (loaded from the DB). No attempt was made to use these loaded summaries.

3. **EVL_Event single-event list:** For change-only compressed channels where the value
   never changed during the session, `ToTimeDelta()` creates a 1-event EventList. The loop
   in `updateCountSummary()` never runs for cnt==1, leaving `valsum` empty.
   `calculatePercentiles()` then returns `valid=false` → p95=0.

4. **EVL_Waveform multi-EventList time accumulation:** When a channel has multiple
   EventLists (e.g. separated by recording gaps), `updateCountSummary()` iterated the
   global cumulative `valsum` for the timesum calculation on each EventList. Counts from
   earlier EventLists were double-counted into subsequent EventLists' time weights,
   giving inflated (wrong) p95 values when gaps exist.

**Fixes:**

- `Machine::Save()`: skip `StoreToDatabase()` for sessions where `sessionRowId() > 0`
  and `!changed()`. New sessions (rowId==0) and sessions explicitly marked changed still
  always save.
- `StoreToDatabase()`: added `hasSummaryData` check — if events aren't loaded but
  `m_valuesummary` and `m_timesummary` are populated (DB-loaded), use them via
  `calculatePercentiles()` instead of writing 0.
- `updateCountSummary()` EVL_Event: after the loop, if `valsum` is still empty (cnt==1),
  add `lastraw` to valsum with a time weight of 1 so `calculatePercentiles()` can return
  the single constant value as the percentile.
- `updateCountSummary()` EVL_Waveform: use a per-EventList `localValsum` for the timesum
  calculation so each EventList contributes only its own sample counts to the time weights.

---

## 2026-04-25 - importNonCPAP() ran without a database transaction (#87)

**File:** `oscar/mainwindow.cpp` — `MainWindow::importNonCPAP()`

**Symptom:** Imports of Zeo, Dreem, Somnopose, and Viatom data ran outside any
database transaction. A failure mid-import would leave the database in a partially
written state with no way to roll back.

**Root cause:** `importCPAP()` wraps the entire import in `dbMgr.transaction()` /
`dbMgr.commit()` with rollback on failure, but `importNonCPAP()` had no equivalent
wrapping — it called `loader.Open()` and `ctx->Commit()` with no transaction guard.

**Fix:** Added `dbMgr.transaction()` before `loader.Open()` in `importNonCPAP()`,
with the same failure handling as `importCPAP()`: error dialog + early return on
transaction-start failure, rollback + critical dialog on commit failure. Also added
the `lastImported` timestamp update (inside the transaction) that `importCPAP()` already
performs, which was also missing.

---

## 2026-04-24 - nightly-build.bat corrupts itself when git stash runs mid-execution (#83)

**File:** `Building/Windows/nightly-build.bat`, `Building/Windows/nightly-notify.ps1`

**Symptom:** Running `nightly-build.bat` when the file had local modifications produced
garbled commands (e.g. `l buildall-qt6.bat`) and failed immediately.

**Root cause:** `git stash` inside the script restored the old version of the file on
disk. Since cmd.exe reads batch files by byte offset, the interpreter then read from
the wrong offset in the restored (differently-sized) file.

**Fix:** Moved `git stash`/`pop` to before the fetch step so the script is never
modified mid-execution. Added a self-modification guard (skipped when running a copy
from outside the repo). Added `--ignore-cr-at-eol` to guard's `git diff` to avoid
false positives from CRLF/LF differences. Added logging to `C:\OSCAR\nightly-build.log`
and a modal failure notification via `nightly-notify.ps1`.

---

## 2026-04-23 - layoutSettings folder not removed after import when source has no saved layouts

**File:** `oscar/main.cpp` — `importLegacyNamedLayouts()`

After importing from OSCAR 1.7.1, the `layoutSettings` folder persisted in OSCAR 2.0's app
data when the source had a `.descriptions.txt` file but no `.shg` layout files. The function
removes `.descriptions.txt` only inside the `if (allOk)` block that runs after committing
`.shg` entries to the DB. When no `.shg` files exist for a view, `entries.isEmpty()` causes
an early `continue`, skipping `descFile.remove()`. The file remains, the folder is non-empty,
and the `dir.rmdir()` cleanup at the end cannot remove it.

Fix: call `descFile.remove()` in the `entries.isEmpty()` path before `continue` so the
descriptions file is always cleaned up.

---

## 2026-04-23 - Bulk import creates empty layoutSettings folder in app data

**File:** `oscar/profileimporter.cpp` — `copyLayoutSettings()`

After a bulk import from OSCAR 1.7.1, an empty (or near-empty) `layoutSettings` folder
was left in the OSCAR 2.0 app data directory. The source folder existed but contained only
a `.txt` file with no saved layouts. `QDir().mkpath(destLayoutPath)` was called
unconditionally after confirming the source folder exists, before checking whether any
files actually needed to be copied. The destination folder was created even when no files
required copying.

Fix: moved `mkpath()` inside the copy loop, guarded by a `destCreated` flag so the
destination directory is created only when the first file actually needs to be copied there.

---

## 2026-04-23 - Crash deleting a profile whose directory is missing

**File:** `oscar/profileselector.cpp` — `on_buttonDestroyProfile_clicked()`

A cancelled (or otherwise failed) profile import can leave a row in the `profiles` table while
the profile directory is absent. `Profiles::Scan()` skips such profiles (marks them `missing` and
does not add them to the in-memory `Profiles::profiles` map). `updateProfileList()` still shows
them because it queries the DB directly. Clicking Delete on such a profile called
`Profiles::profiles[name]`, which — because `QHash::operator[]` inserts a default value on a
missing key — silently returned `nullptr`. The immediately following `profile->Get(...)` call
dereferenced that null pointer → crash (`QHash::find` via `Preferences::Get` at
`profileselector.cpp:487`).

Fix: use `Profiles::profiles.value(name, nullptr)` to avoid inserting a null entry. When the
returned pointer is null, resolve the profile path directly from the database via
`ProfileRepository::resolvePath()`. Guard the password-check block with `if (profile && ...)`.
`removeDirWithProgress()` already handles a non-existent directory safely (returns true), so the
rest of the deletion flow works unchanged.

---

## 2026-04-23 - Fix profile import failing with "cannot commit - no transaction is active"; cancellation leaves incomplete DB data

**Files:** `oscar/SleepLib/preferences.cpp`, `oscar/profileimporter.cpp`

`Preferences::Save()` and `Preferences::Open()` called `db.transaction()` / `db.commit()` /
`db.rollback()` directly on the raw `QSqlDatabase` object, bypassing
`DatabaseManager::m_inTransaction` tracking. When `ProfileImporter::importProfile()` called
`migrateAppSettings()` → `p_pref->Save()` while the outer metadata transaction was active, the
direct `db.commit()` committed the outer transaction. `DatabaseManager` still believed
`m_inTransaction = true`, so when `importProfile()` then tried its own commit, SQLite correctly
reported "cannot commit - no transaction is active". The code path for a failed commit did not
call `ProfileRepository::remove()`, so the already-committed metadata (profile record, machines,
user info) remained in the DB with no directory and no sessions. This manifested as two user-
visible bugs: (1) an error message on every import attempt, and (2) after cancellation of a bulk
import, the first profile appeared in the DB with incomplete data.

Fix 1 (`preferences.cpp`): use `ownTransaction = !dbMgr.inTransaction()` in both `Save()` and
`Open()`. When an outer transaction is already active the writes join it; when called standalone
the function starts and commits its own transaction as before.

Fix 2 (`profileimporter.cpp`): move the `profileId` lookup to BEFORE the metadata commit so the
ID is available on the failure path. Add a defensive `cleanupRepo.remove(profileId)` call in the
commit-failure path to remove any metadata that may have been committed via a bypassed path.

---

## 2026-04-22 - Fix restore failure for pre-v15 backups containing removed columns

**Files:** `oscar/database/backup/profile_restore.cpp`

`executeSqlFile()` reconstructed INSERT statements verbatim from backup SQL files and
passed them to SQLite unchanged. When a backup from schema v12–v14 contained a column
that was subsequently removed (e.g. `user_info.dst_enabled` dropped in v15,
`sessions.events_file`/`summary_file` dropped in v12), SQLite rejected the INSERT with
"table has no column named X", aborting the entire restore. The existing table-level
guard (`sqlite_master` check) handled missing *tables* but not missing *columns*.
Fix: query `PRAGMA table_info(<table>)` once per file to build a valid-column set; skip
any column in the backup INSERT that is absent from the set. This closes the latent bug
for all past and future column removals without needing per-migration restore patches.

---

## 2026-04-22 - Remove dead DST Zone field (schema v15)

**Files:** `oscar/newprofile.ui`, `oscar/newprofile.cpp`, `oscar/SleepLib/profiles.h`,
`oscar/database/user_info_repository.{h,cpp}`, `oscar/database/database_schema.{h,cpp}`,
`Notes/DATABASE_SCHEMA_REFERENCE.md`

The "DST Zone" checkbox in the New Profile dialog was stored and persisted to the
`user_info.dst_enabled` database column, but was never read by any loader, timestamp
calculation, or display code. `EDFInfo::localNoDST` (the only DST-related mechanism)
derives its offset from the system timezone at startup, independently of this field.
Removed the checkbox, `STR_UI_DST` constant, `daylightSaving()`/`setDaylightSaving()`
accessors, `dstEnabled` struct member, all SQL references, and the database column.
Schema bumped from v14 to v15; migration drops the column via `ALTER TABLE DROP COLUMN`.

---

## 2026-04-21 - Eight issues from Claude code-review of schema v14 additions

**Files:** `oscar/database/app_preferences_repository.{h,cpp}`, `oscar/database/database_manager.cpp`, `oscar/database/database_schema.cpp`, `oscar/SleepLib/preferences.cpp`, `oscar/Graphs/gGraphView.cpp`, `oscar/main.cpp`

**Bug 1 — Stale sibling column after scalar ↔ blob type switch (`app_preferences`)**
Symptom: `save()` upserted `value`/`data_type` but left `blob_value` from any prior `saveBlob()` call intact (and vice-versa). Although `data_type` identifies the authoritative column on read, the stale column wastes row width and surprises direct SQL queries.
Fix: Added `blob_value = NULL` to `save()`'s UPDATE clause; added `value = NULL` to `saveBlob()`'s UPDATE clause.

**Bug 2 — `importLegacyNamedLayouts` aborted entire view on single unreadable file**
Symptom: If one `.shg` file couldn't be opened, `readOk = false` propagated to `allOk`, causing DB rollback for the entire view — all successfully-read layouts were discarded and left permanently in legacy form.
Fix: Skip unreadable files individually with a `qWarning`, removing `readOk`. Also skip version-0 files (corrupt or under 6 bytes) rather than importing garbage that silently fails to deserialize.

**Fix 3 — Dead migration code after `return true` in `upgradeSchema`**
~285 lines of v3–v11 migration code were unreachable after the `return true` at the end of active migration logic. Deleted.

**Fix 4 — Qt5 `#if QT_VERSION` guards in `Preferences::Save()` XML path**
Three conditional blocks using `QVariant::Type` / `QVariant::Invalid` / `ts.setCodec` were unnecessary in the Qt6-only build. Removed; kept Qt6 branch inline.

**Fix 5 — `gGraphView::SaveSettings` silently swallowed DB save failures**
No warning was emitted when `repo.saveCurrentLayout()` returned false; `openOk` was set but callers had no console signal. Added `qWarning` on failure.

**Fix 6 — `PRAGMA journal_mode = WAL` result not checked**
`exec()` succeeds even if SQLite stays in delete mode (e.g. on a network filesystem). The pragma returns the resulting mode as a row. Now reads the result and emits `qWarning` if it isn't "wal".

**Style 7 — `dataTypeFromVariant`/`variantFromString` were non-static instance methods**
Neither method accesses instance state. Declared `static` in the header.

**Style 8 — `hasData()` used `toInt()` for a `COUNT(*)` result**
Changed to `toLongLong()` to match the id type and other count sites.

---

## 2026-04-19 - Four bugs from data-migration code review

**Files:** `oscar/SleepLib/preferences.cpp`, `oscar/main.cpp`, `oscar/saveGraphLayoutSettings.cpp`

**Bug 1 — Crash during XML→DB migration leaves preferences permanently half-seeded**
Symptom: A power failure mid-import caused the `app_preferences` table to be partially populated. On the next launch `rows.isEmpty()` returned false so the code loaded the partial data and never looked at `Preferences.xml` again, silently losing the remaining keys.
Fix: Wrapped the `Open()` seeding loop in a `db.transaction()`/`db.commit()`. A crash now rolls back to an empty table so the next launch falls through to XML seeding again.

**Bug 2 — Erased app preferences resurrected from DB on next launch**
Symptom: `Preferences::Erase()` removed a key from the in-memory hash, but `Save()` only UPSERTed — it never deleted rows. Erased keys (e.g. `STR_AppName`, `STR_GEN_SkipLogin`) were reloaded from the DB on the next `Open()`.
Fix: Changed the DB path in `Save()` to delete-then-reinsert the whole `"general"` category inside a transaction, so erased keys are not carried forward.

**Bug 3 — No transactions around legacy layout import loops**
Symptom: `importLegacyNamedLayouts()` and `importLegacyProfileLayouts()` wrote to the DB one row per file with no transaction; a crash left partial DB state with some source files already deleted.
Fix: Restructured both functions to two phases — read all files into memory first, then write all to DB inside a single `db.transaction()`; delete source files only after `db.commit()`.

**Bug 4 — Layout description truncation appended "..." beyond the allowed length**
Symptom: In `SaveGraphLayoutSettings::itemChanged()`, a description exactly one character over `maxDescriptionLen` (80) had `"..."` appended, yielding 84 characters instead of being capped at 80.
Fix: Changed `desc.append("...")` to `desc = desc.left(maxDescriptionLen - 3) + "..."`.

---

## 2026-04-19 - Restore dialog blocked UI for several minutes during Validate

**Files:** `oscar/restoredialog.{h,cpp}`, `oscar/oscar.pro`

**Symptom:** Clicking Validate in the Restore Profile dialog caused the UI to freeze for several minutes (extraction + SHA-256 hash of all .sql files ran on the main thread).

**Root cause:** `ProfileRestore::validatePackage()` was called synchronously in `on_validateButton_clicked()`, blocking the Qt event loop until the ZIP extraction and checksum computation finished.

**Fix:** Moved `validatePackage()` to a thread-pool worker via `QtConcurrent::run`. The dialog immediately shows an indeterminate progress bar and disables all controls. `QFutureWatcher<bool>::finished` fires on the main thread when done, where the rest of the validation UI logic runs unchanged. Added `concurrent` to `QT +=` in oscar.pro.

---

## 2026-04-20 - Six issues from Gemini code-review of schema v14 migration

**Files:** `oscar/database/database_schema.cpp`, `oscar/database/graph_layouts_repository.cpp`, `oscar/main.cpp`, `oscar/Graphs/gGraphView.cpp`, `oscar/saveGraphLayoutSettings.{h,cpp}`

**Issue 1 — `migrateV13ToV14` had no transaction wrapper**
Symptom: If any of the four DDL/DML steps succeeded but a later step failed, the DB was left with v13 schema version but v14 tables already created — relying on idempotency to recover on retry.
Fix: Wrapped the migration body in `db.transaction()` / `db.commit()` / `db.rollback()`.

**Issue 2 & 3 — `peekVersion` lambda duplicated; importers lacked `static`**
Symptom: Identical lambda duplicated in `importLegacyNamedLayouts()` and `importLegacyProfileLayouts()`; both functions had external linkage.
Fix: Extracted to a `static int peekShgVersion(const QByteArray&)` file-scope helper; added `static` to both importer functions.

**Issue 4 — `loadNamedLayout` selected `is_current` but silently skipped it**
Symptom: SELECT included `is_current` at column index 3 but the mapping code jumped directly to `q.value(4)` for `description`, reading the wrong column if ordering ever changed.
Fix: Removed `is_current` from the SELECT; shifted `description`/`format_version`/`data` to indices 3/4/5.

**Issue 5 — `openOk` global not set in DB branches of `SaveSettings`/`LoadSettings`**
Symptom: Callers reading `openOk` after a DB-path call would see stale state from the previous file operation.
Fix: Set `openOk` to the DB operation result in both `SaveSettings` and `LoadSettings` DB branches.

**Issue 6 (style) — Misleading comment and vestigial stub**
Fix: Corrected `saveCurrentLayout` comment (removed "preserve description" which doesn't apply to current-layout upserts); removed no-op `createSaveFolder()` definition from `saveGraphLayoutSettings.cpp` and declaration from `.h`; added `extern` linkage explanation to `gVversion` declaration; added `qWarning` to DB-not-open paths in load methods.

---

## 2026-04-19 - Four bugs from Codex code-review of schema v14 migration

**Files:** `oscar/database/database_schema.cpp`, `oscar/database/app_preferences_repository.cpp`, `oscar/database/backup/profile_backup.cpp`, `oscar/database/backup/profile_restore.cpp`

**Bug 1 — Fresh v14 databases missing `blob_value` column in `profile_preferences`**
Symptom: `createProfilePreferencesTable()` did not include `blob_value BLOB`, but the v13→v14 migration adds it via `ALTER TABLE`. Any code writing `blob_value` to a fresh install would fail with "no such column".
Fix: Added `blob_value BLOB` to the `CREATE TABLE` DDL in `createProfilePreferencesTable()`.

**Bug 2 — QDateTime app preferences saved in Qt::TextDate format, read back with Qt::ISODate**
Symptom: `AppPreferencesRepository::save()` used `value.toString()` which for QDateTime produces Qt::TextDate (e.g. "Sat Apr 19 12:00:00 2026"). `variantFromString()` parsed with `Qt::ISODate`, which fails, returning an invalid QDateTime. Affected `UpdatesLastChecked` and any other datetime pref.
Fix: Added explicit ISO 8601 serialization for QDateTime/QDate/QTime types in `save()`.

**Bug 3 — `graph_layouts` not included in profile backup/restore**
Symptom: Per-profile current daily/overview layouts (rows with `profile_id = this profile`) were silently dropped on backup and not restored, losing the user's layout customizations.
Fix: Added `graph_layouts` export to `exportProfileMetadata()` in `profile_backup.cpp` and added it to the `restoreOrder` list in `profile_restore.cpp`.

**Bug 4 — `createGraphLayoutsTable()` returns true even if unique indexes fail**
Symptom: Both partial unique index creations used `qWarning()` + fall-through on failure. The schema version could advance to 14 without the indexes. Repository upserts using `ON CONFLICT` on those indexes would then insert duplicate rows instead of updating.
Fix: Changed both index creation failure paths to `qCritical()` + `return false`.

---

## 2026-04-19 - Schema v14: Preferences.xml and graph layouts migrated to DB

**Files:** `oscar/database/database_schema.{h,cpp}`, `oscar/database/database_manager.cpp`, `oscar/database/app_preferences_repository.{h,cpp}`, `oscar/database/graph_layouts_repository.{h,cpp}`, `oscar/SleepLib/preferences.cpp`, `oscar/Graphs/gGraphView.{h,cpp}`, `oscar/saveGraphLayoutSettings.{h,cpp}`, `oscar/main.cpp`, `oscar/oscar.pro`

**Change:** Schema bumped from v13 to v14. Two new tables replace filesystem files:
- `app_preferences` — replaces `Preferences.xml` (global app preferences singleton)
- `graph_layouts` — replaces `layoutSettings/*.shg` (named layouts) and per-profile `daily.shg`/`overview.shg` (current layouts)
Also: `blob_value BLOB` column added to `profile_preferences` for future use.

**Migration:** `DatabaseManager` now calls `DatabaseSchema::upgradeSchema()` (formerly a stub) when the DB version is below current. `migrateV13ToV14()` is additive-only (CREATE TABLE IF NOT EXISTS + ALTER TABLE ADD COLUMN).

**Legacy file import:** Preferences.xml is imported once on first launch (in `Preferences::Open()` fallthrough) then deleted. Named `.shg` files in `layoutSettings/` are imported by `importLegacyNamedLayouts()` in `main.cpp` then deleted. Per-profile `daily.shg`/`overview.shg` are imported lazily in `gGraphView::LoadSettings()` on first load then deleted.

**Initialization order fix:** DB initialization in `main.cpp` moved to before `p_pref->Open()` so the DB routing in `Preferences::Open()` sees an open database.

---

## 2026-04-19 - Debug log flushed to disk on every message (crash-safe logging)

**Files:** `oscar/logger.cpp` — `LogThread::appendClean()`, `LogThread::run()`

**Symptom:** On crash, recent debug messages were lost because they were buffered in `LogThread::buffer` (a QList) and in the `QTextStream` — neither was flushed before the process died.

**Root cause:** `appendClean()` only enqueued messages; `run()` dequeued them asynchronously. Any messages not yet dequeued at crash time were lost. Additionally, the `QTextStream` write had no `QFile::flush()` call to push data past the C library buffer.

**Fix:** Moved the file write into `appendClean()`, under the existing `strlock`, with `Qt::endl` (flushes stream) plus `m_logFile->flush()` (flushes C library buffer to kernel). The `run()` thread now only emits `outputLog()` for UI display. The OS writes kernel-buffered data to disk on process exit, so a normal crash loses nothing.

---

## 2026-04-17 - OpenProfile rollback on late failure now fully cleans partial state

**Files:** `oscar/mainwindow.cpp` — `OpenProfile()`

**Symptom:** A defensive failure path could still leave a partially opened profile in memory if `OpenProfile()` aborted after `p_profile` assignment and data load had already started. This was most visible in the active-page guard checks.

**Root cause:** Although `ProgressDialog` cleanup had been added, late returns in `OpenProfile()` did not consistently roll back loaded profile data and partially created pages.

**Fix:** Added a pre-open sanity guard to abort before assigning `p_profile` when page objects are unexpectedly still active, and introduced a single rollback helper (`abortOpenProfile`) for late failures. The rollback now:
- closes/deletes progress dialog,
- deletes any partially created Welcome/Daily/Overview pages,
- unloads machine data, removes lock, and nulls `p_profile`,
- and calls `ensureCleanDatabaseState()`.

---

## 2026-04-17 - Daily constructor: skip graphs for channels absent from loaded data

**Files:** `oscar/daily.cpp` — `Daily::Daily()`

**Symptom:** Daily page constructor always created graph objects for every possible channel (Prisma, BMC, PRS1-specific, Zeo, Position, Oximeter, etc.) regardless of whether the loaded profile contained any data for those channels. This wasted memory and had latent null-pointer risk if any channel's graph entry was ever missing from the creation loops but referenced in the hardcoded AddLayer calls.

**Root cause:** The `cpapcodes[]` and `oximetercodes[]` creation loops had no availability check. All ~25 hardcoded `graphlist[schema::channel[CODE].code()]->AddLayer(...)` calls after the loops used `operator[]` (which inserts null on miss) rather than `.value()` (which returns null without inserting), so a missing graph entry would crash.

**Fix:** Before the creation loops, build a `QSet<ChannelID>` from all machines' `availableChannels(0)`. Skip graph creation in both loops for channels not in the set (empty set = no data loaded = create all). Changed all hardcoded `graphlist[...]->AddLayer(...)` calls to use `graphlist.value(...)` with null guards so absent graphs are safely skipped.

---

## 2026-04-17 - Short/ignored sessions leaked in UnloadMachineData

**Files:** `oscar/SleepLib/profiles.cpp` — `Profile::UnloadMachineData()`

**Symptom:** Memory allocated for short sessions (under the "ignore sessions shorter than" threshold, default 5 minutes) was never freed when closing a profile. Each open/close cycle leaked one Session object per short session in the profile.

**Root cause:** `Machine::AddSession()` adds every session to `sessionlist` unconditionally (to prevent re-importing), but returns early without adding it to a `Day` when the session is below the ignore threshold. `UnloadMachineData()` called `sessionlist.clear()` which empties the QHash without deleting the Session pointers. Sessions in Days were freed correctly via `Day::~Day()`, but these day-less sessions had no other owner.

**Fix:** Before clearing `sessionlist`, collect all Session pointers that are in a Day (via `mach->day`), then explicitly delete any sessions in `sessionlist` not in that set.

---

## 2026-04-17 - Remove High Resolution Mode (Qt5-only feature)

**Files:** `oscar/highresolution.h`, `oscar/highresolution.cpp` (deleted), `oscar/main.cpp`, `oscar/oscar.pro`, `oscar/preferencesdialog.cpp`, `oscar/preferencesdialog.ui`

**Symptom:** Preferences > Appearance showed a disabled "Enable High Resolution Mode" checkbox with no function in Qt6.

**Root cause:** The feature was Qt5-only — in Qt6, high DPI scaling is always on. The checkbox was already disabled/forced-checked in Qt6 builds, but the entire module and its UI element remained.

**Fix:** Deleted `highresolution.h`/`.cpp`, removed the checkbox from `preferencesdialog.ui`, and removed all `#if QT_VERSION < 6` guarded references in `main.cpp` and `preferencesdialog.cpp`. The setting was stored in a separate file (`hiResolutionMode.txt`), not in `Preferences.xml`, so profile import is unaffected.

---

## 2026-04-17 - ProgressDialog left open on error returns in OpenProfile

**Files:** `oscar/mainwindow.cpp` — `OpenProfile()`

**Symptom:** Two defensive checks in `OpenProfile()` (active `daily` or `overview` object detected) called `return false` without closing or deleting the `ProgressDialog` that had already been shown via `progress->open()`. The dialog would remain visible on screen with no way to dismiss it.

**Root cause:** Both early-return paths at the `if (daily)` and `if (overview)` guards branched out before the normal `progress->close(); delete progress;` at the end of the function.

**Fix:** Added `progress->close(); delete progress;` before each `return false`.

---

## 2026-04-17 - Daily::leakchart member variable never initialised

**Files:** `oscar/daily.cpp` — `Daily::Daily()`, `oscar/daily.h`

**Symptom:** The class member `gLineChart *leakchart` (intended to allow preference-driven threshold resets) was never assigned in the constructor. A local variable of the same name shadowed it, leaving the member holding an indeterminate pointer for the life of the object.

**Root cause:** Refactoring introduced `gLineChart *leakchart = new gLineChart(...)` as a local variable in the constructor, shadowing the class member declared in the header. The class member was never assigned or initialised.

**Fix (option 2B):** Removed the type prefix from the constructor declaration, turning it into an assignment to `this->leakchart`. Also added `leakchart = nullptr` near the other member initialisations at the top of the constructor body for safety.

---

## 2026-04-17 - Redundant repaint() calls in Welcome::refreshPage()

**Files:** `oscar/welcome.cpp` — `refreshPage()`

**Symptom:** Five explicit `repaint()` calls forced immediate synchronous redraws of the Welcome page toolbar buttons after each call to `setEnabled()`. Qt already schedules a deferred paint event when widget state changes, making these calls redundant overhead on every profile open.

**Root cause:** Unnecessary calls added; Qt handles repainting automatically when widget state changes.

**Fix:** Removed all five `repaint()` calls.

---

## 2026-04-17 - CProgressBar heap-allocated on every Overview range change

**Files:** `oscar/overview.cpp` — `on_rangeCombo_activated()`

**Symptom:** `CProgressBar` was heap-allocated with `new` and explicitly deleted at the end of every range-change handler, adding unnecessary heap churn on a frequently called code path.

**Root cause:** No reason for heap allocation; `CProgressBar` is not a QWidget subclass and has no parent-ownership requirements.

**Fix:** Changed to stack allocation. Removed `new`/`delete`; replaced `->` member access with `.`.

---

## 2026-04-16 - Purge machine reappears after restart

**Files:** `oscar/mainwindow.cpp` — `purgeMachine()`

**Symptom:** After purging a device via Data/Advanced/Purge all device data, the machine reappeared in the list after restarting OSCAR (session data was gone, but the machine record persisted).

**Root cause:** `purgeMachine()` called `p_profile->DelMachine(mach)` which removes the machine from the in-memory list only. It never called `MachineRepository::remove()` to delete the machine record from the SQLite database.

**Fix:** Capture `mach->getDatabaseId()` before deleting the object, then call `MachineRepository::remove(dbId)` to delete the database record.

---

## 2026-04-16 - Feelings (ZombieMeter) imported with wrong values / displayed incorrectly

**Files:** `oscar/profileimporter.cpp` — `migrateJournalFromSource()`; `oscar/daily.cpp` — `setup_ZombieUIWidgets()`, `on_ZombieSlider_valueChanged()`, `on_Units10_100_clicked()`

**Symptom:** After a 1.7.1 → 2.0 import, Feelings values on the Daily Notes tab showed wrong numbers.

**Root cause:** OSCAR 1.x stored the Feelings field (`Journal_ZombieMeter`) in two mixed encodings: an old 0–10 scale (where 5 was a "not set" sentinel) and a newer 0–100 scale stored as value+20. The importer wrote these raw values into the 2.0 database without normalising them. The display code in `setup_ZombieUIWidgets` applied a runtime decoding (< 20 → ×10, ≥ 20 → −20) to handle both formats on the fly, but this was fragile and produced confusing results for the user.

**Fix (import):** In `migrateJournalFromSource()`, after loading each journal session, normalise `Journal_ZombieMeter` to a clean 0–100 range before storing: values ≥ 20 have 20 subtracted (strip the offset); value 5 is converted to 0 (old "not set" sentinel); all other values (0–4, 6–10) are multiplied by 10. A result of 0 removes the key entirely (not set).

**Fix (display):** Removed the runtime decoding from `setup_ZombieUIWidgets` — values are now stored as 0–100 directly, so the function uses `qBound(0, zombieValue, 100)` unchanged. Removed the `+ZombieModeOffset` offset from `on_ZombieSlider_valueChanged()` and `on_Units10_100_clicked()` so newly entered values are also stored as 0–100.

**Note:** Users with existing OSCAR 2.0 feelings data must reimport from 1.7.1 to correct stored values.

---

## 2026-04-15 - Overview BMI graph missing or unpopulated

**File:** `oscar/daily.cpp` — `Daily::set_JournalWeightValue()`

**Symptom:** The BMI graph on the Overview page either did not appear at all, or appeared but showed no data points.

**Root cause:** `set_JournalWeightValue()` saved `Journal_Weight` to the journal session but never saved `Journal_BMI`. BMI was only calculated and displayed in the UI label (`set_BmiUI`). Since `Journal_BMI` was never persisted, `day->settingExists(Journal_BMI)` always returned false in `gOverviewGraph::SetDay()`, so `m_goodcodes` stayed all-false, `m_empty` stayed true, and `gGraphView` skipped the graph entirely. Users with older data where BMI had been stored saw the graph appear but all new days had no data points.

**Fix:** In `set_JournalWeightValue()`, after saving weight, also calculate and save `Journal_BMI` (using `user_height_cm`) when weight > 0 and height is available. When weight is cleared, also erase `Journal_BMI` from journal settings.

Also fixed in `profileimporter.cpp::migrateJournalFromSource()`: during the 1.7.1 → 2.0 import, the importer now reads the source user's height and backfills `Journal_BMI` into each journal session that contains `Journal_Weight` before storing it to the database. This ensures historical weight data produces a populated BMI graph immediately after import.

---

## 2026-04-13 - Blank progress dialog during Resync Device Detected Events

**File:** `oscar/mainwindow.cpp` — `MainWindow::doReprocessEvents()`

**Symptom:** When enabling Custom CPAP User Event flagging and selecting "Resync Device Detected Events", a blank popup appeared and stayed on screen for the ~1 minute duration of the operation.

**Root cause:** `doReprocessEvents()` created and opened a `ProgressDialog` but never called `QApplication::processEvents()` inside the processing loop, so Qt's event loop had no opportunity to paint the dialog or update its progress bar. `doRecompressEvents()` (the equivalent function for recompression) correctly called both `progress.setProgressValue(++idx)` and `QApplication::processEvents()` per day.

**Fix:** Added `int idx = 0;` before the loop and `progress.setProgressValue(++idx); QApplication::processEvents();` at the end of each day iteration, matching the pattern in `doRecompressEvents()`.

---

## 2026-04-14 - Statistics end-date drift and purge-all notes recreation

**Files:** `oscar/SleepLib/profiles.cpp`, `oscar/daily.h`, `oscar/daily.cpp`, `oscar/mainwindow.cpp`

**Symptom 1 (Statistics):** On the Statistics page, the reporting period end date (and "Most Recent" period end) could be later than the CPAP database range end date. In affected cases, the extra day had no CPAP data.

**Root cause 1:** `Profile::FirstGoodDay(MT_*)` / `Profile::LastGoodDay(MT_*)` treated no-data typed ranges as valid by falling back through `FirstDay(mt)`/`LastDay(mt)` behavior. For types with no data (notably oximeter), this let overall profile bounds (which can be extended by Journal days) leak into statistics report-date calculations.

**Fix 1:** Added inverted-range guards in both methods:
- `FirstGoodDay(MT_*)` now returns invalid when `FirstDay(mt) > LastDay(mt)`.
- `LastGoodDay(MT_*)` now returns invalid when `LastDay(mt) < FirstDay(mt)`.

**Symptom 2 (Purge):** Daily page "Data -> Advanced -> Purge selected day -> Delete all include Notes" could appear to delete notes, but notes reappeared immediately.

**Root cause 2:** After purge deleted the journal session, Daily reload called `Unload(previous_date)`. The notes editor still contained pre-purge text, so unload re-saved/recreated the journal entry.

**Fix 2:** Added `Daily::clearJournalNotesEditor()` and call it from `MainWindow::purgeDay()` when purging `MT_JOURNAL`, before `LoadDate(date)`, so reload cannot re-create the deleted note.

## 2026-04-14 - Statistics report-date stale across profile switches

**Files:** `oscar/statistics.h`, `oscar/statistics.cpp`, `oscar/mainwindow.cpp`

**Symptom:** For users with multiple profiles, the statistics report end date could show a stale value when switching profiles, if both profiles' `lastGoodDay()` happened to match the cached value from the previous profile.

**Root cause:** `Statistics::updateReportDate()` uses two file-scope `QDate` statics (`lastdate`, `firstdate`) to skip redundant recalculations. These statics are process-global and not reset between profile switches, so the early-return could fire on the first `GenerateStatistics()` call for a newly-opened profile, leaving the new profile's `statReportDate` unrefreshed.

**Fix:** Added `Statistics::resetReportDate()` which invalidates both statics, and call it in `MainWindow::OpenProfile()` immediately after `p_profile = prof`, ensuring the first `updateReportDate()` call for each new profile always does a full refresh.

---

## 2026-04-09 - Code review fixes: mainwindow.cpp

**Files:** `oscar/mainwindow.cpp`

**Fix 1 — ProgressDialog leak in zip exporters:** `on_actionCreate_Card_zip_triggered()`, `on_actionCreate_Log_zip_triggered()`, and `on_actionCreate_OSCAR_Data_zip_triggered()` all allocated a `ProgressDialog` on the heap but never deleted it. Added `prog->close(); delete prog;` after `z.Close()` in each function.

**Fix 2 — Dead code in `finishCPAPImport()`:** Removed a large block of commented-out session-save code with confusing stale comments (`(?)savesession`). Session saving is now handled by `ctx->Commit()` inside `importCPAP()`.

**Fix 3 — Syntax noise in `SetupGUI`:** Removed a stray extra semicolon (`; ;`) and trailing `};` in the `m_clinicalMode` check block.

**Fix 4 — Removed Win32 fallback in `purgeMachine()`:** The `#ifdef Q_OS_WIN` block used `SetFileAttributes`/`RemoveDirectory` to attempt removal of the machine directory. Since the intent is to leave the directory intact if it is non-empty (i.e. has unexpected contents), the Win32 code added no value over the cross-platform `QDir::rmdir()`. Replaced with a single `qWarning()` on failure.

---

## 2026-04-09 - Bulk import progress bar stayed at 0; single import caused OS "hung" reports

**Files:** `oscar/main.cpp`, `oscar/profileimporter.cpp`

**Symptom 1:** During bulk startup import, the progress bar value never advanced until an entire profile was
complete, making it appear frozen (especially for single-profile users where it went 0→100 with no
intermediate movement).

**Root cause:** The `progressChanged` signal lambda in `migrateFromOSCAR()` updated only the label text;
it never called `progress.setValue()`. The dialog range was `0..profileList.size()*100`, so only the
coarse per-profile increments at `i*100` and `(i+1)*100` moved the bar.

**Fix:** Changed dialog range to `0..100` (per-profile). The lambda now calls `progress.setValue(current)`
so the bar advances through all import stages (0→10→20…→100) for each profile. The label still shows
"n of n profiles" for overall context. Bar resets to 0 at the start of each new profile.

**Symptom 2:** During single-profile import (File → Profiles → Import Profile from OSCAR) the UI could
appear hung to the OS, because `QApplication::processEvents()` was only called every 10 sessions and
there were no UI yields before the `calculateSummaries()`, `profile->Save()`, or `dbMgr.commit()` calls.

**Fix (first pass):** Reduced session update interval from 10→5. Added `reportProgress()` calls at key
phases.

**Further fix:** Added `QApplication::processEvents()` per file in `copyDirectoryRecursively` to keep
the UI alive while the Backup folder is being copied. Split the single large import transaction into
three smaller ones: (1) profile metadata only (committed before session loading), (2) one transaction
per machine for session data (so each commit is small), (3) calculateSummaries + profile->Save(). On
failure after partial commits `ProfileRepository::remove()` cascade-deletes the partial profile from
the database before filesystem cleanup.

---

## 2026-04-09 - Statistics page shows labels but no data columns (regression from 4bbbf9b8)

**Files:** `oscar/statistics.cpp`

**Symptom:** Statistics page shows row headings (Total Days, AHI, Leak, etc.) but all data columns are absent. Reported against imported 1.7.1 profiles but affects all profiles.

**Root cause:** Commit `4bbbf9b8` changed `if (row.calc == SC_HEADING)` to `if (row.calc == SC_HEADING && summaryInfo.size() > 0)` to guard against a crash on zero-session profiles. However, `summaryInfo.size()` returns `numDisabledsessions` (count of disabled sessions), which is 0 for any profile with all sessions enabled (the normal case). The heading block — which builds the `periods` list of column time-ranges — was therefore never entered, leaving `periods` empty and all data rows with no columns.

**Fix:** Restored `if (row.calc == SC_HEADING)` and added a proper date-validity guard: if `summaryInfo.first()` or `summaryInfo.last()` is invalid (true only for a zero-data profile), set `skipsection = true` and `continue`. This preserves the crash protection while restoring column display for normal profiles.

---

## 2026-04-09 - OAuth2 crash: delete-sender-in-slot in OAuth2Handler::onNewConnection

**Files:** `oscar/network/oauth2_handler.cpp`

**Symptom:** OSCAR crashed immediately after the user completed both Google OAuth web pages. Stack trace showed `doActivate<false>` / `QAbstractSocketPrivate::emitReadyRead`.

**Root cause:** `onNewConnection()` connected a lambda to `QTcpSocket::readyRead`. The lambda called `stopServer()`, which does `delete m_server`. Because `QTcpServer::nextPendingConnection()` returns a socket whose parent is the server, deleting the server also deleted the socket. This happened while Qt's signal machinery was still executing the `readyRead` emission on that socket — destroying the sender mid-emission crashes `doActivate`.

**Fix:** Moved `stopServer()` and `exchangeCodeForToken()` (and all validation) into a `QTimer::singleShot(0, ...)` callback so they execute after the `readyRead` slot returns and signal emission is complete.

---

## 2026-04-09 - OSCAR 1.x → 2.0 import: missing layoutSettings and preferences ✓ tested

**Files:** `oscar/profileimporter.cpp`, `oscar/profileimporter.h`

**Symptoms:**
1. After importing an OSCAR 1.x profile, the `layoutSettings` folder (saved graph layouts) was missing in OSCAR 2.0.
2. Session settings such as "do not import sessions before date" (`IgnoreOlderSessions`/`IgnoreOlderSessionsDate`) reverted to defaults after import.
3. App-level tab preferences (`OpenTabAtStart`, `OpenTabAfterImport`) were not carried over.

**Root causes:**

1. **layoutSettings not copied** — The `layoutSettings` folder lives in the OSCAR 1.x data root (two levels above the profile folder), not inside the profile folder. The importer only handled files within the profile folder. Fix: new `copyLayoutSettings()` method computes the source data root and copies files not already present in the destination.

2. **Profile preferences overwritten on save** — `profileimporter.cpp` correctly saved source preferences to the "session"/"cpap"/etc. DB categories via `prefRepo.saveAllPreferences()`. However, the subsequent `profile->Save()` call invoked `saveProfilePreferencesToDatabase()` on the *destination* profile (which held defaults), writing those defaults to the "profile" DB category. Since `loadExtendedDataFromDatabase()` treats the "profile" category as authoritative (it runs last and overwrites), source settings were lost on next open. Fix: copy `sourceProfile->p_preferences` into `profile->p_preferences` (skipping `DataFolder`, `UserName`, `VersionString`) before `profile->Save()`.

3. **App-level preferences not migrated** — All user-configurable settings stored in `AppWideSetting` / `p_pref` (Preferences.xml at the data root) — including `AutoOpenLastUsed`, `OpenTabAtStart`, `OpenTabAfterImport`, graph appearance settings, etc. — are not inside the profile and were not migrated. Fix: new `migrateAppSettings()` method opens the source `Preferences.xml` and bulk-copies all keys to the live `p_pref`, skipping OSCAR 2.0-specific tracking values (`VersionString`, `Profile`, `Skipped*Version`, `UpdatesLastChecked`).

---

## 2026-04-08 - Qt5→Qt6 locale regression in date/time display

**Files:** `oscar/overview.cpp`, `oscar/daily.cpp`, `oscar/Graphs/gGraphView.cpp`

**Symptom:** In Qt6, `QDateTime::toString(format)` uses the C locale, so month/day names (MMM, MMMM, dddd) always appear in English regardless of the user's system locale.

**Findings (audit 2026-04-08):**

1. **overview.cpp:445** — `dt.toString("dd MMM yyyy (dddd)")` — date label on Overview page.
2. **daily.cpp:2482** — `dt.toString("MMM dd HH:mm:ss.zzz")` — date display on Daily page.
3. **gGraphView.cpp:885** — `dt.toString("MMM dd yyyy")` — log panel entry.
4. **gGraphView.cpp:1755,1757** — `st/et.toString("d MMM...")` — date range label (same-year and different-year cases).
5. **gGraphView.cpp:1771,1773** — `st/et.toString("d MMM...")` — range string with time (same/different year).
6. **gGraphView.cpp:1785** — `st.toString(tr("d MMM yyyy [ %1 - %2 ]")...)` — single-day range string.

**Fix:** Replaced all `dt.toString(fmt)` calls with `QLocale().toString(dt, fmt)`. `QLocale()` respects the default locale set by `QLocale::setDefault()` in `translation.cpp`.

---

## 2026-04-08 - i18n fixes from codebase audit

**Files:** `oscar/SleepLib/profiles.cpp`, `oscar/mainwindow.cpp`, `oscar/profileselector.cpp`, `oscar/overview.cpp`, `oscar/exports/report_exporter.cpp`, `oscar/translation.cpp`

**Findings (audit 2026-04-08):**

1. **profiles.cpp:842** — `QMessageBox::warning` string not wrapped in `tr()` (inside commented-out block; fixed for if it is re-enabled).
2. **mainwindow.cpp:554** — `"Opening " + profileName` not translated; changed to `tr("Opening %1").arg(profileName)`.
3. **profileselector.cpp:770** — `"Something went wrong"` not wrapped in `tr()`.
4. **overview.cpp:152** — `"[Date Widget]"` placeholder not wrapped in `tr()`.
5. **report_exporter.cpp:222,228** — Date format hardcoded as `"MM/dd/yyyy"` (US-only); replaced with `QLocale().dateFormat(QLocale::ShortFormat)`.
6. **translation.cpp:158** — Multi-language window title intentionally not `tr()`-wrapped (dialog appears before any language is loaded); added comment documenting this.

---

## 2026-04-08 - DST timestamp fixes from codebase audit

**Files:** `oscar/SleepLib/loader_plugins/bmcDataParsing.cpp`, `oscar/SleepLib/loader_plugins/bmcG3xDataParsing.cpp`, `oscar/SleepLib/loader_plugins/resmed_loader.cpp`, `oscar/SleepLib/machine.cpp`

**Findings (audit 2026-04-08):**

1. **bmcDataParsing.cpp:24** (`DecodeDate`) — `QDateTime(date, QTime(12,0,0))` had no explicit timezone; added `Qt::LocalTime`.
2. **resmed_loader.cpp:1453** — Mask-off end time used `QDateTime::addDays(1)` (adds exactly 86 400 s); on spring-forward night this overshoots into the next OSCAR day. Fixed by using `QDate::addDays(1)` then reconstructing the QDateTime with `EDFInfo::localNoDST`.
3. **machine.cpp:373, 275** — `AddSession` and `pickDate` compared `QTime time` against `split_time`; during fall-back the same local time appears twice and cannot be disambiguated. Replaced with epoch comparison: `s->first() < QDateTime(d2.date(), split_time, Qt::LocalTime).toMSecsSinceEpoch()`. Removed now-unused `QTime time` local variable; surviving reference at line 385 replaced with `d2.time()`.
4. **bmcDataParsing.cpp:69, 126** — In-progress and historic session `EndTimestamp` used `StartTimestamp.addDays(1)` (adds 86 400 s). Fixed with `QDate::addDays(1)` then reconstruct preserving same time-of-day and timeSpec.
5. **bmcG3xDataParsing.cpp:1902** — Same `addDays(1).addSecs(-1)` pattern on QDateTime. Fixed using `QDate::addDays(1)` then construct noon on next date minus 1 s. Also added explicit `Qt::LocalTime` to StartTimestamp construction.

**Root cause (all):** `QDateTime::addDays(N)` adds exactly N×86 400 seconds; on DST transition nights the local noon-to-noon window is 82 800 s (spring-forward) or 90 000 s (fall-back), so the computed boundary falls in the wrong OSCAR day. `QDate::addDays(N)` performs calendar arithmetic and is DST-safe.

---

## 2026-04-08 - Crash/stale-pointer fixes from codebase audit

**Files:** `oscar/SleepLib/loader_plugins/bmcDataParsing.cpp`, `oscar/SleepLib/deviceconnection.cpp`, `oscar/statistics.cpp`

**Findings (audit 2026-04-08):**

1. **bmcDataParsing.cpp:590** — `session->Waveforms.first()` called when `Waveforms` could be empty on a mid-loop split. Added `&& !session->Waveforms.isEmpty()` guard.
2. **bmcDataParsing.cpp:603** — Same crash at loop exit; added same guard to the post-loop `if`.
3. **bmcDataParsing.cpp:567–580** — `foundSettings` stored a raw pointer into `AllMachineSettings` (via `&msettings` in range-for and `&AllMachineSettings.last()`), which is invalidated by container reallocation. Replaced with copy-by-value.
4. **deviceconnection.cpp:426,438** — `SetValueEvent::id()` and `GetValueEvent::id()` called `m_keys.first()` unconditionally; default constructor leaves `m_keys` empty. Added isEmpty() guard returning `QString()`.
5. **statistics.cpp:1441** — `summaryInfo.first()/last()` called inside `SC_HEADING` branch with no check for empty; crashes on a profile with zero recorded nights. Added `summaryInfo.size() > 0` guard.

**Fix:** Guards added at each call site; `bmcDataParsing` settings lookup converted from pointer to value.

---

## 2026-04-05 - Import: Summary-only sessions show AHI/H = 0.00 after import from OSCAR 1.7.1

**Files:** `oscar/SleepLib/session.cpp` (`LoadSummaryFromFile`)

**Symptom:** After importing OSCAR 1.7.1 data, summary-only ResMed sessions (those without a `.001` events file) displayed AHI = 0.00 and Hypopnea = 0.00. Sessions with full event data imported correctly.

**Root cause (two interacting issues):**
1. `StoreSummaryStatistics()` in `resmed_loader.cpp` stores fractional event counts for summary-only sessions: `setCount(CPAP_Hypopnea, R.hi * hours)` produces e.g. 0.997 (not an integer). `SessionChannelData::count` and `SessionSummaryData::hypopneaCount` are `int`, so 0.997 truncates to 0 on storage.
2. OSCAR 1.7.1 wrote `.000` files at version ≤ 14. `LoadSummaryFromFile()` only reads `m_availableChannels` for version ≥ 15 (version 14 had a serialization bug). With an empty `m_availableChannels`, `StoreToDatabase()` skips the session_channels block entirely — `m_cph` (event rates, stored correctly) is never written to the database.
3. `LoadFromDatabase()`'s summary-only restore block can recover counts from `m_cph × hours`, but only if session_channels was stored. With no session_channels for CPAP_Hypopnea, the condition `m_cph.contains(CPAP_Hypopnea)` is false, the block is skipped, and the count stays 0.

**Fix:** At the end of `LoadSummaryFromFile()`, if `m_availableChannels` is still empty after parsing, rebuild it from the keys in `m_cnt` and `m_cph`. This ensures `StoreToDatabase()` creates session_channels records (with correct `cph` = 0.10), enabling the existing restore block in `LoadFromDatabase()` to compute `qRound(cph × hours)` = 1 on reload.

**Note:** Sessions already imported with broken data must be re-imported.

---

## 2026-04-02 - AirSense 11: Wrong icon shown on Welcome page

**Files:** `oscar/SleepLib/loader_plugins/resmed_loader.cpp`, `oscar/SleepLib/machine.cpp`

**Symptom:** Welcome page showed the default CPAP icon instead of the AirSense 11 icon.

**Root cause (3 parts):**
1. `scanProductObject()` (JSON path, `Identification.json`): used `indexOf("11")/left(idx+2)` to derive `info.series`, producing `"AirSense11"` (no space). The icon hash is keyed on `"AirSense 11"` (with space), so lookup failed and the default icon was returned.
2. `parseIdentLine()` (`.tgt` path): had no checks for `STR_ResMed_AirSense11` or `STR_ResMed_AirCurve11`; these models fell through to the S9 `else` branch.
3. `Machine::SaveToDatabase()`: when an existing machine record was found, only `machineId` was updated — `series` and other info fields were never refreshed, so stale values from old loader bugs persisted across imports.

**Fix:**
- `scanProductObject()`: replaced the `indexOf` hack with explicit `contains()` checks (case-insensitive) for each series string, matching the same constants used in the icon hash.
- `parseIdentLine()`: added `contains()` checks for AirSense 11 and AirCurve 11 before the AirSense 10 checks; all checks made case-insensitive; also handle no-space form (`"AirSense11"`).
- `SaveToDatabase()`: now updates `series`, `model`, and `modelNumber` in the existing record if any have changed, so a re-import from SD card self-corrects stale database entries.

---

## 2026-03-30 - G3X: Periodic breathing duration wrong (uint16 vs uint32)

**Files:** `bmcG3xDataParsing.cpp`

**Symptom:** PB episode durations were ~28s and ~23s instead of the correct ~159s and ~154s as reported by PAP-Link.

**Root cause:** EVT type 0x09 (PB marker) encodes duration as a uint32 at offset 0x1C (low 16 bits) combined with offset 0x1E (high 16 bits).  The code was reading only the low 16 bits (`value2`) and dividing by 1000, giving 28034/1000≈28s and 23390/1000≈23s.  The correct uint32 LE values are 159106 ms and 154462 ms (≈2:39 and ≈2:34 matching PAP-Link exactly), confirmed via raw byte dump: bytes [0x82,0x6D,0x02,0x00] = 0x00026D82 = 159106 and [0x5E,0x5B,0x02,0x00] = 0x00025B5E = 154462.

**Fix:** In `kG3xEvtTypePBMarker` case, compute `durationMs = static_cast<quint32>(value2) | (static_cast<quint32>(ReadUInt16LEPtr(rec, 0x1E)) << 16)` before dividing by 1000.

---

## 2026-03-27 - G3X: Flow limitation bars misaligned and wrong width

**Files:** `bmcDataParsing.h`, `bmcG3xDataParsing.cpp`, `bmc_loader.cpp`

**Symptom:** FL events displayed as a near-invisible line when zoomed in (200 ms wide), with no visual correlation to individual breaths in the flow waveform.

**Root cause (two bugs):**
1. **Wrong width:** Bar was bookended at ±100 ms, giving a 200 ms display width.  The EVT value2 field (which for respiratory events carries duration in milliseconds) was read but not stored or used for FL events.  Confirmed via Python analysis of Kavolodin EVT data (5145 FL events): value2 range 0.36–3.98 s, mean 1.88 s — the device-reported inspiration duration.
2. **Wrong direction:** The bar was initially plotted forward from `ts` (correct), then changed to backward (`ts − dur` to `ts`) based on a misread of the flow waveform.  With backward plotting the bar covered expiration instead of inspiration.  Confirmed visually: `ts` is the trough at end-of-expiration / start-of-inspiration; the bar must run forward to cover the inspiratory peak where FL occurs.

**Fix:**
- Added `DurationMs` field to `BmcFlowLimitEvent`; EVT parser stores `value2` there.
- `bmc_loader.cpp` plots each FL event from `ts` to `ts + DurationMs` (forward), with a zero bookend 100 ms before `ts` to isolate from the previous event.
- Validated: FL bars now align with the inspiratory peak; severity 1/2/3 visually correlates with degree of inspiratory flattening in the flow waveform.

---

## 2026-03-25 - G3X: EVT scan misses all records when EventStartOffset is not 32-byte aligned

**Files:** `bmcG3xDataParsing.cpp`

**Symptom:** For at least one known device (Lijunjun G3 B20A, SC.75), all respiratory events (apneas, hypopneas, flow limits) are absent from the imported day because the EVT parse loop finds zero valid records.

**Root cause:** The IDX `EventStartOffset` is not guaranteed to fall on a 32-byte record boundary. When it falls mid-record (e.g., 16 bytes in), the parse loop starting exactly at `EventStartOffset` and stepping in 32-byte increments hits the second half of every record — none of which start with the required AE AA magic bytes. All records are silently skipped.

**Fix:** Round `EventStartOffset` down to the nearest 32-byte boundary before seeking. The existing AE AA magic check discards the partial record fragment at the start.

---

## ~~2026-03-25 - G3X: Replace 0x44-based PB with AASM computed PB from 0x0C breath markers~~

~~**Files:** `bmcG3xDataParsing.cpp`, `bmcg3x_loader.h`~~

~~**Symptom:** Periodic breathing was never shown in OSCAR for any G3X device. Firmware SC.72 (JCCPAP/Luna G3X) emits no EVT 0x44 records at all. On SC.74+/SC.75 the 0x44 records had not been validated and PB output was explicitly suppressed.~~

~~**Root cause:** PB detection depended entirely on firmware-emitted 0x44 event records, which are absent on SC.72. No universal PB signal existed.~~

~~**Fix:** Replaced 0x44-based detection with an AASM algorithm applied to device-classified CSA (central apnea) events from `rawRespEvents`. Each event carries a device-measured duration; startTime is derived as endTime − duration. The algorithm groups consecutive central apneas (≥3 s each) separated by ≤20 s of normal breathing into PB episodes (≥3 qualifying apneas per episode). Works on all firmware. `ExportPeriodicBreathing()` in `BmcG3xLoader` changed from `false` to `true`.~~

---

## 2026-03-25 - G3X: EVT 0x42 IPAP/EPAP fields reversed; bilevel devices showed wrong IPAP

**Files:** `bmcG3xDataParsing.cpp`

**Symptom:** For bilevel G3X devices (e.g. G3 B20A with 1 cmH2O pressure support), OSCAR displayed the same value for both `CPAP_IPAP` and `CPAP_EPAP`, equal to EPAP. IPAP was never shown.

**Root cause:** The EVT 0x42 record carries two pressure fields: `value2` (offset 0x1C) = EPAP, `unk1e` (offset 0x1E) = IPAP. The parser was treating `value2` as both IPAP and EPAP (CPAP assumption). Confirmed by comparing the Lijunjun G3 B20A (1 cmH2O PS): every 0x42 record has `unk1e` = `value2` + 100 (= +1.00 cmH2O).

**Fix:** Updated both the main EVT parse loop and the fallback scan in `ReadDateSession()` to read EPAP from `value2` and IPAP from `unk1e`. IPAP falls back to EPAP if `unk1e` is outside the plausible pressure range (handles pure CPAP devices where `unk1e` might be zero or absent).

---

## 2026-03-26 - G3X: Periodic breathing (EVT 0x44) suppressed; unknown-firmware warning added

**Files:** `bmc_loader.h`, `bmc_loader.cpp`, `bmcg3x_loader.h`, `bmcg3x_loader.cpp`

~~**Periodic breathing:** EVT type 0x44 ("PB") was being emitted to the CPAP_PB channel for all G3X sessions. The flag has not been validated against PAP-Link for any G3X firmware, and on SC.74+ firmware it produces clearly nonsensical output. Added `ExportPeriodicBreathing()` virtual method to `BmcLoader` (base returns `true` for legacy BMC). `BmcG3xLoader` overrides to `false`, which suppresses creation of the CPAP_PB event list entirely. The raw 0x44 records are still collected in the parser for future analysis.~~

**Unknown-firmware warning:** BMC G3X firmware version is read from the companion `.log` file (first 6 KB scanned for a null-terminated ASCII string starting with `"G3-2."`; offset varies by device — ~0x0420 in small G3 A20 logs, ~0x1420 in larger G3 B20A ring-buffer logs). Falls back to IDX offset `0x0345` (internal SC build string, e.g. `"G3-2.SC.72.01"`) if the `.log` is unavailable. The version is stored in `MachineInfo.properties["firmware"]` (added in `PeekInfo()`). In `Open()`, if the firmware string is non-empty and does not start with a known user-facing prefix (`"G3-2.11."` or `"G3-2.12."`, or contain `"SC.72"` / `"SC.74"` as IDX-fallback identifiers), a `QMessageBox::information` dialog is shown asking the user to send their SD card .zip to the OSCAR team. Import then continues normally.

---

## 2026-03-26 - G3X: Leak graph wrong/empty on firmware SC.74+ (Kavolodin / Patient 2); model string and firmware version also fixed

**Files:** `bmcG3xDataParsing.cpp`, `bmcDataParsing.h`

**Symptom:** On BMC G3 A20 machines with firmware SC.74+ (part-code prefix `880`, e.g. Kavolodin and Patient 2), the OSCAR leak graph showed a near-zero sparse line with only a handful of data points — clearly wrong. On the same machines, the machine model string displayed the internal part/config code (`"880A40383"`) instead of the product name (`"G3 A20"`). Firmware version was not recorded at all.

**Root cause (three bugs):**
1. **Leak field**: `kG3xOffsetLeak = 0x52A` is populated only in firmware SC.72 (prefix `110`). In firmware SC.74+ (prefix `880`) this field is always zero (0.33% non-zero, clearly noise). The correct leak field for SC.74+ is `0x568` (total mask leak; continuously populated at ~15–17 L/min baseline, scale 0.16 L/min per raw unit).
2. **Model string**: IDX offset `0x048` holds the internal part/config code (`"110A40113"` or `"880A40383"`), not the product name. The product name (`"G3 A20"`) is at IDX offset `0x100`.
3. **Firmware version**: Not read at all. The user-facing version string (e.g. `"G3-2.11.02.33"` or `"G3-2.12.54.13"`) is in the companion `.log` file; the internal SC build string (e.g. `"G3-2.SC.72.01"`) is at IDX offset `0x0345` as a fallback.

**Fix:**
- Added `FirmwareVersion` field to `BmcMachineInfo` in `bmcDataParsing.h`.
- Updated `ParseMachineInfo()` to read model from IDX `0x100` (with fallback to `0x048` if absent) and firmware version from the `.log` file (scan first 6 KB for `"G3-2."` prefix; offset varies by device). Falls back to IDX `0x0345` if `.log` is unavailable.
- Added `kG3xOffsetAlternateLeak = 0x568` constant.
- Added Phase 3.5 adaptive leak-field probe in `ReadDateSession()`: samples first 200 waveform packets; if fewer than 10% have non-zero `0x52A`, selects `0x568` for the entire day. This is robust to future firmware versions without needing hard-coded part-code lookup.
- Updated waveform loop to use `leakFieldOffset` (determined by Phase 3.5) instead of always using `kG3xOffsetLeak`.
- Scale factor `0.16 L/min per raw unit` applies to both fields.

---

## 2026-03-25 - G3X: Respiratory event duration corrected to value2 (milliseconds)

**Files:** `bmcG3xDataParsing.cpp`
**Root cause:** All respiratory event types (0x01–0x09, 0x0A) were using `value1` as duration in seconds. `value1` is actually a wrapping counter (0–999) with no duration information; `value2` encodes duration in milliseconds. Confirmed 2026-03-25 by extracting value2/1000 for a full OSCAR day and comparing against OSCAR waveform graphs: OSA durations of 27.7 s, 15.6 s; hypopnea 10.9 s; UA 11.5 s; RERA 11.3 s — all clinically plausible. `0x0B` value2/1000 = 0.4–1.1 s (sub-second, not event duration).
**Fix:** `G3xRawRespEvent.Value1` renamed to `Value2Millis`; raw event collection changed to store `value2`; duration = `round(value2 / 1000.0)` seconds, clamped to [10, 180]. RERA fixed-duration override removed.

---

## 2026-03-25 - G3X: Unclassified hypopnea (0x01) added to CPAP_Hypopnea channel

**Files:** `bmcG3xDataParsing.cpp`
**Feature:** EVT message type `0x01` confirmed as unclassified hypopnea (2026-03-25, Patient 2 B33BF114508). Added `kG3xEvtTypeUH = 0x01` constant; added to EVT loop and Phase 2 mapping → `BmcRespiratoryEventType::HYP`. Duration uses value2/1000 s clamped to 10–180 s. 58 records observed across Patient 2 nights; absent from JCCPAP.

---

## ~~2026-03-26 - G3X: PB detection expanded to include CH (central hypopnea) events~~

~~**Files:** `bmcG3xDataParsing.cpp`~~
~~**Symptom:** JCCPAP-2 (SC.72 firmware) periodic breathing episode missed. SC.72 emits one CSA plus generic/central hypopneas during a PB event; CSA-only clustering cannot form a ≥3-apnea cluster from one CSA.~~
~~**Root cause:** Phase 2b PB clustering filtered only on `kG3xEvtTypeCSA` (0x04). On CPAP devices CH (0x08, central hypopnea) also indicates absent/reduced central drive and is part of the same periodic-breathing cluster.~~
~~**Fix:** Expanded Phase 2b filter to include `kG3xEvtTypeCH` (0x08) alongside CSA. CH events continue to be reported as hypopneas in OSCAR (unchanged); only the PB episode detection input changes.~~

---

## 2026-03-24 - G3X: RERA (Respiratory Effort Related Arousal) added to CPAP_RERA channel

**Files:** `bmcDataParsing.h`, `bmcG3xDataParsing.cpp`, `bmc_loader.cpp`
**Feature:** EVT message type `0x0A` confirmed as RERA by PAP-Link correlation on five independent events across four JCCPAP nights (2026-02-12, 2026-02-21, 2026-02-22 ×2, 2026-02-24). All five PAP-Link RERA timestamps matched a `0x0A` record within ±1 minute; two RERAs on 2026-02-22 corresponded exactly to the two `0x0A` records on that night.
**Implementation:** `RERA` added to `BmcRespiratoryEventType` enum. `kG3xEvtTypeRERA = 0x0A` constant added. Phase 2 maps `0x0A` → `BmcRespiratoryEventType::RERA`. Written to `CPAP_RERA` (EVL_Event) in `bmc_loader`. Duration initially set to a fixed 10 s; updated 2026-03-25 to use value2/1000 s (observed ~11.3 s, consistent with PAP-Link's 10-second display).

---

## 2026-03-23 - G3X: Flow Limitation (Mild/Moderate/Severe) added to CPAP_FLG channel

**Files:** `bmcDataParsing.h`, `bmcG3xDataParsing.cpp`, `bmc_loader.cpp`
**Feature:** EVT message types 0x0E (Mild), 0x0F (Moderate), 0x10 (Severe) confirmed as flow limitation events by PAP-Link alignment on two independent sessions (G3X-2 2026-02-20: 376/22/6 vs PAP-Link many/21/5; JCCPAP 2025-12-20: 181/11/5). These types are absent from devices with no FL.
**Implementation:** `BmcFlowLimitEvent` struct (Timestamp + Grade 1/2/3) collected in EVT loop, assigned to sessions, written to `CPAP_FLG` (EVL_Event, gain=1.0) with ±100 ms zero bookends to create isolated bars. `setPhysMin/Max(0.0/3.0)` anchors the y-axis so mild (grade 1) bars are visible. Statistics (min=0, median=0, 95th=0, max=3) are correct — FL is sparse (~2% of session time).

---

## 2026-03-23 - G3X: CPAP_Pressure artificially quantised to 0.5 cmH2O steps

**Files:** `bmcG3xDataParsing.cpp`, `bmc_loader.cpp`, `bmc_loader.h`, `bmcg3x_loader.h`
**Symptom:** The G3X Pressure graph had only 0.5 cmH2O resolution despite the machine recording pressure at 0.01 cmH2O precision.
**Root cause:** `Raw.IPAP`/`Raw.EPAP` were written using `PressureHundredthsToRawHalfCm()`, which rounds to the nearest half-cmH2O to match the legacy BMC packet format. The `bmc_loader` then applied a fixed gain of 0.5. This was appropriate for the legacy machine (which genuinely only records half-cmH2O steps) but threw away the G3X's finer precision.
**Fix:** Added virtual `PressureChannelGain()` to `BmcLoader` (default 0.5 for legacy). G3X overrides to 0.01. G3X now stores raw hundredths of cmH2O directly in `Raw.IPAP`/`Raw.EPAP`, bypassing `PressureHundredthsToRawHalfCm()`. `bmc_loader` uses `PressureChannelGain()` for all three pressure event lists.

---

## 2026-03-22 - BMC Legacy: IPAP/EPAP fields mis-labelled in packet struct

**Files:** `bmcDataParsing.h`, `bmcDataParsing.cpp`
**Symptom:** `BmcWaveformPacketStruct` had `IPAP` at offset 0x04 and `EPAP` at 0x06, which was backwards. A runtime `std::swap` was papering over the error.
**Root cause:** The documentation was incorrect — 0x04 is always the smaller value (EPAP) and 0x06 is always the larger (IPAP), confirmed across all packets on all three test machines.
**Fix:** Swapped the field names in the struct declaration in `bmcDataParsing.h` (EPAP at 0x04, IPAP at 0x06). Removed the `std::swap` calls and their comment from `bmcDataParsing.cpp`.

---

## 2026-03-20 - BMC Legacy: MaskPressure waveform channel showed garbage data

**Files:** `bmc_loader.h`, `bmc_loader.cpp`
**Symptom:** CPAP_MaskPressure waveform channel showed unrecognizable data. Separate BMC_PressureWave chart was also displayed.
**Root cause:** `Raw.MaskPressure` (kBmcExtendedWaveformSamples = 50 elements) is never initialized in the legacy BMC packet constructor — it contains garbage. The actual mask pressure waveform is in `Raw.PressureWave` (25 samples at packet offset 0x08, ÷10 = cmH2O). The separate BMC_PressureWave chart is redundant; mask pressure belongs in the standard CPAP_MaskPressure channel.
**Fix:** Changed `wMaskPressure->AddWaveform` in `bmc_loader.cpp` to use `bmcWaveform.Raw.PressureWave`. Changed `ExportPressureWaveform()` in `bmc_loader.h` base class to return `false` (no separate chart).

---

## 2026-03-20 - BMC Legacy: PressureWaveform gain wrong by factor of 10

**Files:** `bmc_loader.h`
**Symptom:** The pressure waveform displayed values 10× too high (e.g., 85 cmH2O instead of 8.5 cmH2O).
**Root cause:** `PressureWaveformGain()` returned `1.0`. Raw waveform values (int16) store pressure in 0.1 cmH2O units; gain must be `0.1` to display cmH2O. Confirmed by cross-referencing raw PressureWave values against known EPAP/IPAP settings across three SD card images (BMC Luna, BMC Luna G3, BMC G3).
**Fix:** Changed `PressureWaveformGain()` return value in `bmc_loader.h` from `1.0` to `0.1`.

---

## 2026-03-20 - BMC Legacy: Pulse rate / SpO2 from optional oximeter accessory

**Files:** `bmcDataParsing.cpp`
**Note:** SpO2 (0xCC) and pulse rate (0xCE) are zero on all currently tested SD images because those machines did not have the oximeter accessory installed. The fields are correctly mapped; data will appear when an oximeter-equipped machine's card is loaded. Both channels are expected to be present or absent together. Offset 0xBE was briefly tried as an alternate pulse source but is mechanical, not physiological.

---

## 2026-03-20 - BMC Legacy: Ti/Te computation produced values ~10× too small

**Files:** `bmc_loader.cpp`
**Symptom:** CPAP_Ti showed 95% of values below 0.21 seconds — physiologically implausible for normal breathing (~0.8–1.2 s expected inspiratory time at 12–16 BPM).
**Root cause:** `IERatioMapped` is stored as a percentage (0–100) by the `BmcWaveformPacket` constructor, but the Ti/Te block divided by 1000 (treating it as permille). For example, `IERatioMapped = 23` at RR = 12 produced Ti = (60/12) × (23/1000) = 0.115 s instead of the correct 1.15 s.
**Fix:** Changed divisors in `bmc_loader.cpp` lines 519–520 from `1000.0` to `100.0`, and `(1000 - IERatioMapped)` to `(100 - IERatioMapped)`.

---

## 2026-03-20 - BMC G3X: CPAP_Ti / CPAP_Te / I:E channels based on unconfirmed offsets

**Files:** `bmcG3xDataParsing.cpp`, `bmc_loader.cpp`, `bmc_loader.h`, `bmcg3x_loader.h`
**Symptom:** G3X exported CPAP_Ti, CPAP_Te, CPAP_IE, BMC_IE_Ratio channels derived from waveform packet offsets 0x074 and 0x07E, which were assumed to be inspiration/expiration time in centiseconds.
**Root cause:** Analysis showed the sum of 0x074 + 0x07E is near-constant (~565 cs) regardless of respiratory rate, and neither field correlates with RR, tidal volume, minute ventilation, or pressure. They are not patient Ti/Te. The original label was incorrect.
**Fix:** Added `ExportTimingChannels()` virtual method to `BmcLoader` (default true). Overridden to false in `BmcG3xLoader`. Guarded creation and population of `wInspTime`, `wExpTime`, `wIEValue`, `wIERatio` event lists in `bmc_loader.cpp`. Suppressed IERatioMapped computation from 0x074/0x07E in `bmcG3xDataParsing.cpp`. Also consolidated duplicate Ti/Te write block into one guarded block.

---

## 2026-03-18 - Daily calendar navigation bar doesn't highlight on window focus

**Files:** `oscar/daily.ui`
**Symptom:** In OSCAR 1.7.1 (Qt 5), the calendar's month/year navigation bar changed from black-on-gray to white-on-blue when OSCAR had window focus. In 2.0 (Qt 6), it stays black-on-gray regardless of focus.
**Root cause:** Qt 5 applied the system highlight color to the navigation bar automatically. Qt 6 no longer does this, and the explicit stylesheet in `daily.ui` overrides any default behavior. Qt stylesheets don't support a `:focus` pseudo-state on parent widgets, so restoring this would require handling `QEvent::WindowActivate`/`WindowDeactivate` in code to swap the stylesheet.
**Status:** Noted; no fix applied.

---

## 2026-03-18 - Preferences dialog more vertically spread out than Qt5/Windows 10

**Files:** Preferences `.ui` file
**Symptom:** Preferences dialog is noticeably taller on Qt 6 / Windows 11 compared to Qt 5 / Windows 10.
**Root cause:** Not a bug. Qt 6 uses larger default layout margins, spacing, and widget padding. Windows 11's Segoe UI font renders with slightly different metrics, and native controls are taller for touch-friendly sizing. The combined effect makes the same `.ui` layout taller.
**Status:** Noted; no fix applied. Could be tightened via layout margins/spacing or stylesheet, but that risks affecting other platforms.

---

## 2026-03-18 - Crash on application exit

**Files:** `oscar/main.cpp`, `oscar/mainwindow.cpp`
**Symptom:** OSCAR crashes during shutdown in `QGuiApplication::~QGuiApplication` every time the application exits.
**Root cause:** Two `QApplication` instances existed simultaneously. A `QApplication app` was created at the top of `main()` (inside a Qt 6.5+ guard) to force light-mode color scheme, and then a second `QApplication mainapp` was created later for the actual event loop. Qt requires exactly one QApplication per process. When `mainapp` was destroyed on exit, it cleaned up global Qt state; then `app`'s destructor tried to clean the same state again, crashing in `QGuiApplication::~QGuiApplication`.
**Fix:** Removed the first `QApplication app` and moved the `setColorScheme(Qt::ColorScheme::Light)` call to `mainapp` immediately after its construction. Also restructured shutdown: (1) moved `Profiles::Done()` and `DestroyGraphGlobals()` from `closeEvent` to `main.cpp` after explicit `delete mainwin` so globals outlive all widgets; (2) un-parent loaders from MainWindow in `closeEvent` to avoid dual-ownership double-free; (3) removed `QCoreApplication::quit()` from `MainWindow::~MainWindow()`.

---

## 2026-03-18 - Crash when importing/exporting journal with no profile open

**Files:** `oscar/mainwindow.cpp`
**Symptom:** OSCAR crashes when user selects Journals/Import Journal (or Export) with no profile/database open.
**Root cause:** `profilePath()` (line 2528) dereferences `p_profile` without a null check. When no profile is loaded, `p_profile` is null, causing a crash in `QHash::contains`.
**Fix:** Added null guard for `p_profile` in `profilePath()`. Added early-return with warning message in both `on_actionImport_Journal_triggered()` and `on_actionExport_Journal_triggered()` when `p_profile` is null.

---

## 2026-03-18 - BMC G3X: mask-on hours now separate from hours-used

**Files:** `oscar/SleepLib/loader_plugins/bmcg3x_loader.h`, `oscar/SleepLib/loader_plugins/bmc_loader.h`, `oscar/SleepLib/loader_plugins/bmc_loader.cpp`, `oscar/SleepLib/session.cpp`

**Symptom:** `session_summaries.hours_used` and `session_summaries.mask_on_hours` were identical; mask-on hours did not reflect the fact that the patient removed the mask before the machine was turned off.

**Root cause (two separate bugs):**
1. `Session::StoreSummaryToDatabase()` computed `hoursUsed = hours()`, but `hours()` returns MaskOn-slice time when slices are populated, making `hoursUsed == maskOnHours`.
2. The mask-off detector in `BmcG3xLoader::findStableEndMs` used a trailing high-leak scan; after mask removal the BMC G3X ramps pressure to minimum so leak returns to zero — the last packet has `Raw.Leak = 0` and the detector never triggered.

**Fix:**
- `session.cpp`: Changed `hoursUsed` to use `(s_last - s_first) / 3600000.0` (raw session span) instead of `hours()`.
- `bmcg3x_loader.h`: Replaced leak-based mask-off detection with flow-based detection. Scans backward through `Raw.Flow[50]` samples to find the last packet with any sample exceeding the noise floor (±25 raw). If the trailing no-flow period is ≥ 300 seconds, that packet's timestamp is the mask-off point. Validated on reference night: mask-off detected at 08:37:43, giving maskOnHrs = 8.16 vs totalHrs = 8.60 (≈ 27-minute difference).

---

## 2026-03-17 - Dark mode illegibility fixed in three windows

**Files:** `oscar/profileselector.cpp`, `oscar/daily.ui`, `oscar/oximeterimport.cpp`

**Symptom:** In system dark mode on Qt 6.2/6.4, three windows were illegible: the profile selector had a dark background, the daily page calendar widget had a dark background, and the oximeter import wizard showed white-on-white text.

**Root cause:** Qt 6.5+ provides `QApplication::styleHints()->setColorScheme()` to force light mode per-widget, but this API is unavailable on Qt 6.2/6.4. Without explicit palette overrides, these widgets inherited the system dark palette.

**Fix:**
- `profileselector.cpp`: Apply an explicit light `QPalette` (white base/window, black text) to `this` in the constructor.
- `daily.ui`: Extend the `QCalendarWidget` stylesheet with rules targeting `QAbstractItemView` (white background, black text) and `qt_calendar_navigationbar` (light grey background, black text).
- `oximeterimport.cpp`: Apply `WindowText`, `Text`, and `ButtonText` = black to `this->palette()` in the constructor so text is legible over the existing light gradient background.

---

## 2026-03-17 - Qt dialog/button translations added via `oscar_qt_*.ts` supplemental catalogs

**Files:** `oscar/translation.cpp`, `Translations/qt/oscar_qt_*.ts`, `Tools/generate_qt_dialog_translations.py`

**Symptom:** Standard Qt dialog strings such as `Open`, `Save`, `Cancel`, `Close`, and `QFileDialog` labels remained untranslated even when OSCAR itself was running in a translated language.

**Root cause:** OSCAR already had runtime support for loading supplemental `oscar_qt_*.qm` files, but the corresponding `.ts` source files did not exist. The runtime loader also truncated the language code to two letters, which prevented region-specific catalogs such as `zh_CN`, `pt_BR`, `es_MX`, and `en_UK` from being loaded by name.

**Fix:** Added trimmed Qt supplemental translation catalogs under `Translations/qt/` for every current OSCAR language, generated primarily from the local Qt 6.10.2 translation sources and supplemented with OSCAR-local strings/manual fallbacks where Qt did not ship a matching source catalog. Updated `initTranslations()` to try the full OSCAR language code first (for example `oscar_qt_pt_BR.qm`) and then fall back to the base language code if needed.

**Note:** Languages where Qt does not provide a matching source catalog (for example Afrikaans, Filipino, Greek, Norwegian Bokmal, Romanian, Thai) use best-effort supplemental translations and should be reviewed by native speakers when possible.

---

## 2026-03-17 — QMessageBox custom buttons not translated (Yes/Cancel in CPAP Data Located dialog)

**Files:** `oscar/mainwindow.cpp`, `Translations/*.ts` (27 language files)

**Symptom:** The "Yes" and "Cancel" buttons in the "CPAP Data Located" QMessageBox were not translated when OSCAR was set to a non-English language on an English OS.

**Root cause:** `tr("Yes")` and `tr("Cancel")` called in `mainwindow.cpp` look up translations in the `MainWindow` context only. These strings existed in other contexts in the `.ts` files (e.g. `QObject`, `RestoreDialog`) but were absent from the `MainWindow` context. Qt does not fall back across contexts.

**Fix:** Added "Yes" and "Cancel" to the `MainWindow` context in all 27 `.ts` files that had existing translations for those strings. Three files were not updated due to no existing translation: Czech, English (UK), Thai.

**Note:** QFileDialog button labels ("Open"/"Save"/"Cancel") remain untranslated — those are Qt-internal strings requiring `oscar_qt_xx.qm` files, which do not yet exist.

---

## 2026-03-17 — "Last Imported" column in profile selector not updating after SD card import

**Files:** `oscar/mainwindow.cpp`, `oscar/SleepLib/importcontext.cpp`

**Symptom:** The "Last Imported" column in the profile selector showed a stale date and was never updated after importing new data from an SD card.

**Root Cause:** `MachineInfo::lastimported` is set to `QDateTime::currentDateTime()` only when a `MachineInfo` is first constructed (new machine). For existing machines it is loaded from the database and never updated on subsequent imports. `Machine::SaveToDatabase()` returns early for already-registered machines without touching `lastImported`. Additionally, `ResmedLoader` calls `mach->AddSession()` directly (not via `new_sessions` or `ImportContext`), so there was no hook in the loader itself to catch the timestamp.

**Fix:** In `MainWindow::importCPAP()`, after `Open()` returns `c > 0` (sessions were imported) and inside the open DB transaction, iterate all machines in the profile matching the loader's name, set `m->info.lastimported = QDateTime::currentDateTime()`, and persist via `MachineRepository::update()`. This is the one place in the import flow guaranteed to run for all loaders regardless of how they add sessions. Also added the same fix in `ImportContext::Commit()` for the context-based path used by PRS1.

---

## ~~2026-03-15 — QFileDialog: DontUseNativeDialog required for button translation (design note)~~

~~**Files:** All call sites using `QFileDialog` throughout OSCAR.~~

~~**Background:** `QFileDialog::DontUseNativeDialog` was added to every `QFileDialog` call so that button labels (Open, Save, Cancel, etc.) are translated to the user's selected OSCAR language.~~

~~**Investigation:** We considered whether there was an alternative that would allow native OS dialogs to be used while still translating the buttons. There is not. Native dialogs are rendered entirely by the OS platform layer (e.g. COMDLG32 on Windows) and always use the OS locale. Qt has no mechanism to inject translated text into native dialogs on any supported platform. `QFileDialog::setLabelText()` is documented to have no effect on native dialogs on most platforms.~~

~~**Conclusion:** `DontUseNativeDialog` is the only correct cross-platform solution when the app language may differ from the OS language. The trade-off is that Qt-rendered dialogs have a slightly different appearance and may be marginally slower than native dialogs on some platforms.~~

---

## 2026-03-16 — BMC G3X: startup artifacts on Flow Rate, Pressure, and Pressure Trend graphs

**Files:** `oscar/SleepLib/loader_plugins/bmcg3x_loader.h`, `oscar/SleepLib/loader_plugins/bmc_loader.h`, `oscar/SleepLib/loader_plugins/bmc_loader.cpp`

**Symptom:** At the left edge of the session view, Flow Rate showed a spike from y=0 to the first sample, Pressure showed a ramp from the APAP minimum (~4.0 cmH2O) up to the therapeutic value, and Pressure Trend showed the same ramp.

**Root cause:** The BMC G3X machine starts recording waveform packets immediately when powered on, before the patient puts on the mask. During this idle period, `PressureTrend` (0x76C) is held at the minimum APAP pressure (400 hundredths = 4.0 cmH2O). When the patient dons the mask, the APAP algorithm ramps pressure upward over several seconds to reach the therapeutic target. The session's `s_first` was set to the very first waveform packet, so all idle and ramp data was visible from the left graph edge.

**Fix:** Added virtual `findStableStartMs(BmcSession*)` to `BmcLoader` (base returns `StartTimestamp` unchanged). `BmcG3xLoader` overrides it with a two-phase scan: (1) skip the initial idle packets where `PressureTrend` is at the startup minimum, (2) from the first packet where pressure begins rising, advance until pressure stops rising (ramp peak). The resulting timestamp is passed to `session->really_set_first()` instead of `StartTimestamp`. The graph renderers naturally crop all earlier data: the waveform renderer skips samples before `s_first`, and the event renderer primes from the last pre-`s_first` event so step-function channels also start cleanly.

---

## 2026-03-15 — Viatom/Wellue import: SpO2 and Pulse Rate plots show "Plots Disabled" / empty after restart

**Files:** `oscar/SleepLib/loader_plugins/viatom_loader.h`

**Symptom:** After importing Viatom/Wellue oximeter data via Data → Import Viatom Data, the session appeared in the session list but SpO2 and Pulse Rate graphs showed "Plots Disabled". After restarting OSCAR, the same plots were simply empty. Debug log showed: `Session::LoadFromDatabase(): Session XXXXXXXXXX not found in database`.

**Root cause:** `Machine::SaveToDatabase()` rejects machines where both `info.serial` and `info.model` are empty (`"Cannot save machine without serial or model"`). In `ViatomLoader::newInfo()`, `model` was `QString()` (empty). `serial` is only set if the file's enclosing folder name is ≥9 characters long with the last 4 numeric (a Viatom device serial number pattern). When a user imports files from a non-serial-named folder (e.g. Downloads), both remain empty. The machine is never saved to the database, so `m_database_id` stays 0. `Session::Store()` skips all DB writes when the machine has no DB ID. After `TrashEvents()` clears in-memory data, `OpenEvents()` fails (machine not in DB), yielding 0 plot points → "Plots Disabled". On restart, `LoadSessionsFromDatabase()` finds no machine/sessions and nothing is shown.

**Fix:** Changed `newInfo()` in `viatom_loader.h` to provide a default `model` of `QObject::tr("Viatom Oximeter")`. This ensures the machine is always saved to the database even when no serial can be determined from the folder name, so session data is correctly stored and loaded.

---

## 2026-03-15 — MD300W1, Dreem, Somnopose, ZEO loaders: same empty model/serial bug as Viatom

**Files:** `oscar/SleepLib/loader_plugins/md300w1_loader.h`, `dreem_loader.h`, `somnopose_loader.h`, `zeo_loader.h`

**Symptom:** Same as the Viatom bug above — sessions would not persist to database after import.

**Root cause:** Same root cause. `newInfo()` returned empty `model` and empty `serial` in all four loaders. MD300W1 goes through the oximeter wizard; Dreem, Somnopose, and ZEO go through `importNonCPAP`. None populate serial from file data, so `Machine::SaveToDatabase()` always failed for them.

**Fix:** Added default model names: MD300W1 → `"MD300W1 Oximeter"`, Dreem → `"Dreem Headband"`, Somnopose → `"Somnopose"`, ZEO → `"Zeo Sleep Manager"`.

---

## 2026-03-12 — Statistics page month names not translated in Qt6

**Files:** `oscar/statistics.cpp`

**Symptom:** In Monthly report mode, column headers showing month names (e.g., "January", "February") were always displayed in English regardless of the active language. Worked correctly under Qt5.

**Root cause:** `QDate::toString(format)` behaviour changed between Qt5 and Qt6. In Qt5 it used the application's default locale for month/day names. In Qt6 it always uses the C locale (English). This is a documented Qt6 breaking change.

**Fix:** Changed `s.toString("MMMM<br>yyyy")` to `QLocale().toString(s, "MMMM<br>yyyy")` on line 1460, which explicitly uses the application locale and works correctly in both Qt5 and Qt6.

---

## 2026-03-12 — QDateEdit ignoreOlderSessionsDate displays incorrectly under translation

**Files:** `oscar/preferencesdialog.ui`, `Translations/Francais.fr.ts`

**Symptom:** The "Do not import sessions older than" date field in Preferences displayed strange/uneditable content when a non-English language (specifically French) was active.

**Root cause:** The `displayFormat` property (`dd MMMM yyyy`) of the `QDateEdit` widget was a plain `<string>` in the `.ui` file, so Qt's `uic` wrapped it in `tr()`. The French translation mapped it to `jj MMMM aaaa` (using French abbreviations jour/an), which are not valid Qt date format codes. Qt treated `jj` and `aaaa` as literal text, causing the widget to display literal "jj" and "aaaa" instead of numeric day and year.

**Fix:** Added `notr="true"` to the `displayFormat` string in `preferencesdialog.ui` so the format is never translated. Qt's `MMMM` token already renders month names in the application locale's language, so no translation of the format string is needed. Also corrected the French translation from `jj MMMM aaaa` to `dd MMMM yyyy`; this entry will become obsolete after the next `lupdate` run.

---

## 2026-03-12 — Purged data reappears after OSCAR restart

**File:** `oscar/SleepLib/session.cpp`, `Session::Destroy()`

**Symptom:** Purging a day (or all data) via Data → Advanced appeared to work, but after closing and restarting OSCAR the purged sessions were still present.

**Root cause:** `Session::Destroy()` only deleted the legacy `.000` summary and `.001` event files from disk, but never removed the session row from the SQLite database. Since summary file storage was moved to the database (`.000` writes are disabled), nothing was actually deleted — the files didn't exist and the DB rows remained intact. On restart OSCAR loaded sessions from the database, restoring all "purged" data.

**Fix:** Added `SessionRepository::remove(m_sessionrow_id)` call in `Session::Destroy()` before `unlinkSession()`. The DB schema uses `ON DELETE CASCADE` on all child tables (session_channels, session_slices, session_settings, event_lists, respiratory_events, etc.), so a single delete of the sessions row cleans up everything.

---

## 2026-03-12 — Rebuild-from-backup erases backup directory in BMC loaders

**Files:** `oscar/SleepLib/loader_plugins/bmc_loader.cpp`, `oscar/SleepLib/loader_plugins/bmcg3x_loader.cpp`, `Open()`

**Symptom:** "Rebuild from backup" for BMC and BMC G3x machines silently destroyed the backup data it was trying to import from, leaving the machine with no backup after the rebuild.

**Root cause:** Both `BmcLoader::Open()` and `BmcG3xLoader::Open()` unconditionally erase and recreate `mach->getBackupPath()` then copy `dirpath` into it. When rebuilding from backup, `dirpath` IS the backup directory, so the backup was wiped before any data was read.

**Fix:** Before the erase-and-copy block in both loaders, compare `QDir::cleanPath(dirpath)` to `QDir::cleanPath(backupPath)`. If they are equal, skip the backup creation entirely. `QDir::cleanPath()` normalises trailing slashes so the comparison is reliable.

---

## 2026-03-12 — Profile selector name column not translatable (locale name order)

**File:** `oscar/profileselector.cpp`, `oscar/profileselector.h`

**Symptom:** Names in the profile list and detail panel were always displayed in "Last, First" order regardless of locale. French users expect "First Last" order.

**Root cause:** Three of the four name-formatting call sites used bare `QString("%1, %2")` with no `tr()` wrapper, making them untranslatable. The fourth used `tr("Name: %1, %2")` but conflated the label and the name format in a single translation string, making it impossible for translators to change only the name order.

**Fix:** Added `ProfileSelector::formattedName(lastName, firstName)` helper that returns `tr("%1, %2").arg(lastName, firstName)`. All four call sites now use the helper. Translators can override `"%1, %2"` to `"%2 %1"` in their `.ts` file to get "First Last" order with no further code changes.

---

## 2026-03-12 — Profile selector name column sorts by display string, not by last name

**File:** `oscar/profileselector.cpp`, `oscar/profileselector.h`

**Symptom:** Clicking the name column header to sort would sort by the displayed string. In French ("First Last" order) this sorts by first name, not last name — inconsistent with English behaviour and user expectation.

**Root cause:** `MySortFilterProxyModel2::lessThan` compared `Qt::DisplayRole` data for the name column. When the display format is locale-dependent (e.g. "First Last" in French), the sort order follows the display format rather than a stable key.

**Fix:** Added `MySortFilterProxyModel2::NameSortRole` (`Qt::UserRole+3`) to carry a locale-independent `"lastname, firstname"` sort key. All three `setData` calls for column 5 now also store this key. `lessThan` detects the name column and compares by `NameSortRole` instead of `DisplayRole`. Added `ProfileSelector::nameSortKey(lastName, firstName)` static helper to produce the key consistently.

---

## 2026-03-12 — gOverviewGraph crash on mouse-over (Overview page)

**File:** `oscar/Graphs/gOverviewGraph.cpp`, `mouseMoveEvent()`

**Symptom:** OSCAR crashes when moving the mouse over the Feelings plot on the Overview page, if Feelings has only been set for one day.

**Root cause:** `d.value()` was called on a `QHash` iterator (`d = m_values.find(hl_day)`) before the guard `d != m_values.end()` was checked. When hovering over any day without a Feelings entry, `d` is the end iterator — dereferencing it is undefined behaviour and crashes.

**Stack trace frame:** `gOverviewGraph::mouseMoveEvent` at line 1079 (original), `QHash::iterator::value()`.

**Fix:** Moved `QMap<short, EventDataType> &valhash = d.value();` to inside the `if ((d != m_values.end()) && (day != nullptr))` block, so it is only evaluated when the iterator is valid. `valhash` is only used within that block, so no other changes were needed.

---

## 2026-03-16 — BMC G3X: oximetry channels continue recording after mask removal; CPAP waveforms do not

**Files:** No code change — investigation note only.

**Observation:** On the 2026-03-16 night, the Flow Rate waveform and most CPAP channels (Pressure, Mask Pressure, Tidal Volume, etc.) appear to stop at approximately 08:27, while the OSCAR session timeline and the Oximetry channels (SpO2, Pulse Rate) continue to 09:04.

**Root cause (data, not code):** The patient removed the mask at approximately 08:25–08:27. Binary evidence in the waveform file (B33BF114508.000):
- 08:25:34: Leak jumps from 0 to 15 raw units (mask seal breaking).
- 08:25:54: Flow spikes to 1159 raw units, leak reaches 1199 (mask coming off).
- 08:26:28 onward: Flow drops to 0–5 raw units (noise floor); leak stabilises at ~178–193 raw units (ambient air escaping from the running machine with no mask).
- 08:27–09:04: Flow amplitude never exceeds 18 raw units; all 60 waveform packets per minute are present and valid.

The SpO2 finger probe remained on the patient's finger, so OXI_SPO2 and OXI_Pulse continue recording valid data until 09:04. The CPAP machine also continued running (and recording) with no mask until 09:04, producing a constant-leak, near-zero-flow signature.

**Conclusion:** OSCAR is displaying the data correctly. The flow waveform is not missing — it is genuinely flat because the mask was off. The session end time of 09:04 reflects when the machine was switched off, not when therapy ended. No code change required.

---

## 2026-03-15 — "Find your CPAP data card" dialog Open/Cancel buttons not translated

**File:** `oscar/mainwindow.cpp`, `MainWindow::importCPAPData()` (around line 1285)

**Symptom:** When a non-English language was active, the Open and Cancel buttons in the "Find your CPAP data card" `QFileDialog` remained in English.

**Root cause:** `QFileDialog` defaults to the native OS file picker. Native dialogs render their own buttons outside Qt's widget and translation system, so `tr()` has no effect on them.

**Fix:** Added `w.setOption(QFileDialog::DontUseNativeDialog, true)` so Qt renders the dialog itself. Qt's own dialog widgets are fully subject to the translation system, and the buttons are translated correctly.

---

## 2026-04-09 - Network module: QTemporaryFile destructor deletes downloaded file; missing size checks and cleanup helpers

**Files:** `oscar/network/cloud_downloader.{h,cpp}`, `oscar/network/dropbox_uploader.{h,cpp}`, `oscar/network/onedrive_uploader.{h,cpp}`, `oscar/network/oauth2_handler.cpp`

**Bug 1 — CloudDownloader deleted the downloaded file on destruction**
On successful download, `onReplyFinished()` left `m_tempFile` non-null. The destructor called `m_tempFile->remove()` on it, deleting the temp file from disk even though the caller still held the path. Fixed by adding `delete m_tempFile; m_tempFile = nullptr;` in the success path (without calling `remove()`, since `autoRemove` is false).

**Bug 2 — Timestamp-based temp file name**
`start()` constructed the temp file path using `QDateTime::currentMSecsSinceEpoch()`, leaving a dead XXXXXX-template assignment above it. Replaced with `QTemporaryFile` (template `oscar_download_XXXXXX.oscar`, `setAutoRemove(false)`) for a secure, unique name.

**Bug 3 — Redundant Qt version guards in startRequest()**
The `#if QT_VERSION >= Qt6` and `#elif >= Qt5.9` branches in `startRequest()` were identical. Collapsed to a single `#if >= 5.9` / `#else` guard.

**Bug 4 — Duplicated manual cleanup in Dropbox/OneDrive uploaders**
Manual `m_reply->deleteLater(); m_reply = nullptr; delete m_file; m_file = nullptr;` was repeated across abort, error, and success branches. Added a `cleanupReply()` helper to both uploaders (matching the pattern already in `CloudUploader`) and replaced all call sites.

**Bug 5 — OneDrive uploader had no file-size guard**
`createUploadSession()` sent the entire file with no size check, despite Microsoft Graph API's 60 MiB per-chunk limit. Added a 60 MiB hard limit with a user-facing error, mirroring the Dropbox uploader's 150 MB guard.

**Bug 6 — OAuth2 token expiry persisted as ISO date string**
`saveTokens()` wrote `m_tokenExpiry.toString(Qt::ISODate)`; `loadTokens()` parsed it back with `QDateTime::fromString`. Changed to `toMSecsSinceEpoch()` / `fromMSecsSinceEpoch()` (stored as `qint64`) for unambiguous UTC round-tripping.


---

## 2026-04-19 - No way to cancel Restore, Backup, or Share Profile operations

**Files:** `oscar/database/backup/profile_backup.{h,cpp}`, `oscar/database/backup/profile_restore.{h,cpp}`, `oscar/restoredialog.{h,cpp}`, `oscar/backupdialog.{h,cpp}`, `oscar/sharedialog.{h,cpp}`

**Symptom:** Once Restore / Backup / Share was started, the user had no way to stop it. The Close button was disabled for the duration, which could be many minutes for large profiles.

**Root cause:** `ProfileBackup::createBackup()` and `ProfileRestore::restoreProfile()` ran synchronously on the main thread with no cancellation mechanism. All three dialogs disabled the Close button during the operation.

**Fix:**
- Added `requestCancel()` + `std::atomic<bool> m_cancelRequested` to both `ProfileBackup` and `ProfileRestore`.
- Added `QCoreApplication::processEvents()` calls between each major phase in `createBackup()`, and after each table-import emit in `restoreInTransaction()`, so the Cancel click is processed promptly.
- Changed all three dialogs to keep the Close button enabled during operation, relabelled "Cancel". Clicking it calls `requestCancel()` on the active object (and `abort()` on the cloud uploader if an upload is in progress). The button becomes "Cancelling..." and is disabled once clicked.
- Cancellation is treated as a non-error: no QMessageBox is shown; status label shows "Cancelled." and the UI resets normally.
- For restore: if cancelled mid-transaction, `restoreInTransaction()` rolls back cleanly before returning.

---

## 2026-05-22 - Profile backup fails with "Failed to add manifest.json to package" (#179)

**Files:** `oscar/zip.cpp`, `oscar/zip.h`

**Symptom:** Profile backup always failed at the packaging step with the error
"Failed to add manifest.json to package". The Qt Application Output showed:
`unable to add "manifest.json" : 1827 bytes file read failed`.

**Root cause:** `ZipFile::m_abort`, `m_progress`, and `m_lastNotified` were never
initialized in the constructor — only in `AddFiles()`. `createPackage()` calls
`AddFile()` directly, bypassing `AddFiles()`. With the original stack layout
`m_abort` happened to land on a zero byte; adding a new `SqlExporter` object in
`exportMachinesAndSessions()` (for the `device_time_corrections` backup) shifted
the stack layout enough that `m_abort` now landed on a truthy byte.
`zip_file_read()` immediately returned 0, miniz reported "file read failed", and
the backup aborted.

**Fix:** Initialize `m_abort(false)`, `m_progress(0)`, `m_lastNotified(0)` in the
`ZipFile` constructor member-initializer list.

**Also fixed (same session):** `SqlExporter::exportBlobTable()` called `file.close()`
before the `QTextStream` destructor could flush its internal buffer. For small
result sets (e.g. a 0-row `device_time_corrections` export) the entire file content
remained in the stream buffer and was never written to disk. Fixed by calling
`stream.flush()` before `file.close()`.

---

## 2026-06-01 - Backup restore fails for archives > 4 GB (Zip64 bug)

**Files:** `oscar/zip.{h,cpp}`, `oscar/SleepLib/thirdparty/miniz.c`

**Symptom:** Kubuntu 26.04 user backs up with beta 5 and cannot restore with beta 5 or RC1.
OSCAR reports "is not a valid backup package." ARK reports "entry 6: invalid Zip64 extra
field."

**Root causes (two):**

1. **miniz 10.1.0 Zip64 writing bug** (in `oscar/SleepLib/thirdparty/miniz.c`, **patched**):
   When a ZIP archive grows past 4 GB, miniz sets an internal `m_zip64` flag and thereafter
   writes Zip64 extended information extra fields in local file headers for entries at
   offset >= 4 GB. These local-header extras incorrectly include `local_header_offset` — a
   field the ZIP spec reserves for central directory headers only. libarchive (used by ARK)
   correctly rejects such entries. The same bug was confirmed present in a newer version of
   miniz (unfixed upstream). Patched in four spots across `mz_zip_writer_add_mem_ex_v2` and
   `mz_zip_writer_add_read_buf_callback`: local-header Zip64 extras now only fire when file
   sizes overflow (passing NULL for offset), while the CDH Zip64-extra blocks were changed
   from `if (pExtra_data != NULL)` to an explicit size-or-offset condition so the central
   directory still records the correct 64-bit offset independently.

2. **`UnzipFile::Open()` loaded the entire archive into RAM** (`zip.cpp`):
   The old implementation called `QFile::readAll()` and passed the resulting `QByteArray` to
   `mz_zip_reader_init_mem`. For a > 4 GB backup this requires > 4 GB of contiguous RAM; when
   unavailable, `readAll()` returns a truncated buffer, causing `mz_zip_reader_init_mem` to
   fail (ECDH not found) and OSCAR to report "not a valid backup package."

**Fix (root cause 2):** Replaced the in-memory approach with a seek+read callback using
`mz_zip_reader_init`. A static `unzip_qfile_read()` callback seeks `QFile` to `file_ofs`
and reads `n` bytes, so miniz performs random-access I/O directly against the file without
ever loading it all into RAM. The `QFile m_file` member (replacing `QByteArray m_fileData`)
stays open from `Open()` to `Close()`. This allows OSCAR to restore backups of any size,
including existing archives with the miniz Zip64 writing bug (miniz's reader is lenient about
local-header Zip64 extras; it skips them by stated length without validation).
