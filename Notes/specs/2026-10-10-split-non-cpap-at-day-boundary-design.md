# Split oximetry and other non-CPAP sessions at the day boundary: design

**Date:** 2026-10-10
**Status:** approved design, scope decisions recorded 2026-10-10; not implemented
**Tracking:** GitLab #216 (open); #314 closed as a duplicate of #216

## Problem

A user who sleeps past noon with a ResMed CPAP and an oximeter (O2Ring, etc.) cannot see
the data after noon on one day. The ResMed session after noon is on day N+1, but the oximeter
session is entirely on day N because it started before noon. So the Daily view for N+1 shows
CPAP data with no oximetry, and day N shows oximetry running past the end of the CPAP data.

## How OSCAR assigns sessions to days today

- `Machine::AddSession()` (`SleepLib/machine.cpp`) puts each **whole** session on one day,
  chosen by the session's start time against `profile->session->daySplitTime()`. OSCAR never
  cuts a session at the split time. This is the same for CPAP and non-CPAP machines.
- ResMed machines end their session at noon themselves. For profiles with a ResMed machine,
  OSCAR forces the split time to 12:00 and turns off "combine close sessions"
  (`mainwindow.cpp`, ResMed preference lock).
- Most other CPAPs (PRS1 and others) keep a session that crosses noon whole, the same as
  oximetry. For those users the CPAP and oximetry days already line up.

So the mismatch is between a CPAP that **splits its own sessions** and an oximeter that does
not. It is not a general rule that OSCAR splits CPAP data at noon.

## Proposal

Add an opt-in profile preference, e.g. *"Split oximetry and other non-CPAP sessions at the day
split time"*, on the Preferences page that holds the day split time.

- **Split time:** use `daySplitTime()`, not a fixed noon. For ResMed profiles that is always
  12:00; for other profiles it follows the user's setting.
- **Default: off, for every profile.** For non-ResMed CPAP users, turning it on would *create*
  a mismatch: the CPAP session stays whole on day N while the oximetry is cut. The preference
  text should say it is meant for CPAPs that split at noon (ResMed). It is not turned on
  automatically for ResMed profiles, because guessing from the machines in a profile is
  unreliable. The user decides.
- **Scope:** machine types `MT_OXIMETER`, `MT_SLEEPSTAGE` and `MT_POSITION`, all controlled
  by the one option. Exclude `MT_JOURNAL` and `MT_CPAP`.
- **Forward-looking only:** the option applies to sessions imported after it is turned on.
  There is no tool to split sessions already imported (see pitfall 1).
- **When:** at import, before `Machine::AddSession()`. The pieces are then ordinary sessions,
  so Daily graphs, Overview, `session_summaries` and `daily_summaries` (SpO2 and pulse
  averages, etc.) are correct per day with no schema change and no special handling in the
  display code.

### Why at import rather than at display time

Changing the display instead (letting day N+1 show the part of day N's oximetry session that
lies inside its window) would leave per-day statistics computed over the whole session. It
would also have to be added to every graph and to the Overview and Statistics code. Splitting
the data keeps one meaning for "the sessions of a day".

## Implementation outline

### 1. Shared splitting helper (new)

A SleepLib helper, e.g. `QList<Session*> splitAtDayBoundaries(Session*)`, returns the
original session cut to end at the first split time, plus one new session for each later
day. A recording longer than 24 h crosses more than one boundary, so the helper must loop.

`EventList` (`SleepLib/event.h`) has no slicing method, so one must be written. It has to
handle:
- **Waveform lists** (`EVL_Waveform`, fixed rate, no per-sample timestamps), as Viatom uses.
  Slice the data by sample index and set `first`/`last` on each piece.
- **Event lists** (`EVL_Event`, timestamped), as the oximeter import wizard uses. Those lists
  store only *changes* of value, so the second piece needs a sample at the boundary carrying
  the last value. Otherwise its graph starts blank until the next change.
- Session-level `first`/`last` and per-channel first/last on every piece.

### 2. Call sites (non-CPAP import paths)

| Path | Where it adds the session |
|---|---|
| Viatom / Wellue (incl. POD2) | `ViatomLoader::SaveSessionToDatabase()` |
| Oximeter import wizard: CMS50/MD300W1 files and live recording | `OximeterImport` save handler, `mach->AddSession(session)` |
| Somnopose | `somnopose_loader.cpp` |
| Zeo | `zeo_loader.cpp` |
| Dreem | `dreem_loader.cpp` |
| Apple Health (sleep stages and oximetry) | `applehealth_loader.cpp` (see pitfall below) |

Don't put the split inside `Machine::AddSession()` as it stands. Profile load also calls it
with `allowOldSessions=true` (`machine.cpp`, summary load), and at that point the events are
not loaded. If the split is centralised there, it must be limited to new imports.

### 3. Session IDs and duplicate detection

- The first piece keeps the original session ID, so the loaders' `SessionExists(sid)` checks
  still recognise a file that was already imported (Viatom, Zeo, Somnopose and Dreem all use
  them).
- Later pieces need new IDs; the split time in seconds is the obvious choice. Check
  `SessionExists()` and add a second on a collision.

### 4. Summaries and derived events

`Session::UpdateSummaries()` runs `calcSPO2Drop()` and `calcPulseChange()`, and the import
wizard also calls them directly before `AddSession()`. After a split, each piece computes them
over its own data, so a desaturation or pulse event that straddles the boundary may be
lost or detected differently. This needs checking against real data before deciding whether
computing on the whole session and dividing the results by time is worth the extra work.

## Pitfalls found during evaluation

1. **Existing data is not affected, and Rebuild can't fix that.** The non-CPAP loaders keep
   no backup of the source files (their file headers say so), so Rebuild cannot apply the
   split to sessions already imported. **Decision:** the change is forward-looking. There is
   no one-time tool to split existing sessions. A user who wants older data split deletes
   those sessions and re-imports the files. The preference text or help should say so.

   Turning the option off later does not merge pieces back together.
2. **Apple Health's check for already-imported nights.** It builds its set of existing nights
   from `nightDate(session->first())` over the sessions of *every* `MT_OXIMETER` and
   `MT_SLEEPSTAGE` machine, with a fixed noon split. A piece from another device starting at
   12:00 on day N+1 would mark night N+1 as already imported, and that night's Apple Health
   data would be skipped. That check would need to ignore pieces made by the split, for
   example by using only the loader's own machine or only the piece that holds the session's
   original ID.
3. **One machine per type per day.** `Day::addSession()` refuses a session from a second
   machine of the same `MachineType` on a day. If oximeter A's piece after noon lands on day
   N+1 and that night was recorded with oximeter B, B's session is refused for that day. This
   is rare (two oximeters used on consecutive nights), but the result is silent, so the import
   should at least log it.
4. **Short pieces.** A piece only a few minutes long after the split is hidden by "Ignore
   short sessions", as for CPAP. That is consistent, but users may ask about it.
5. **Combine close sessions.** For non-ResMed profiles, `combineCloseSessions` can move a
   session to the previous day. The split should use the same boundary that `AddSession()`
   will use, so a piece doesn't end up on a different day than the one it was cut for.

## Decisions (2026-10-10)

- Default **off** for all profiles; not turned on automatically for ResMed profiles.
- **Forward-looking only**; no one-time tool to split sessions already imported.
- Applies to **oximetry, sleep-stage and position** data, under one option.
