#!/usr/bin/env python3
"""oscar_details_csv.py

Standalone reproduction of OSCAR 1.7.1's "Details" CSV export, reading directly
from an OSCAR 2.0 SQLite database. No Qt, no OSCAR build required -- Python
standard library only (sqlite3, zlib, struct/array).

The goal is to MATCH 1.7.1, not to improve on it. Specifically:

  * Columns ............ DateTime, Session, Event, Data/Duration
  * Channel set ........ the 1.7.1 countlist + avglist (see TARGET_CHANNELS).
                         The high-rate waveforms (Flow Rate, Mask Pressure,
                         Snore, etc.) are intentionally NOT included, exactly
                         as in 1.7.1.
  * DateTime ........... local time, whole-second ISO 8601 ("YYYY-MM-DDTHH:MM:SS").
                         1.7.1 divides the millisecond timestamp by 1000 and
                         formats with Qt::ISODate, so sub-second precision is
                         dropped here too -- fast channels produce duplicate
                         timestamps, same as 1.7.1.
  * Data/Duration ...... raw_int16 * gain, formatted to exactly 2 decimals.
                         For apnea-type event channels this value IS the event
                         duration; for waveform/flag channels it is the value.
  * Row order .......... session (by start time) -> channel (list order) ->
                         eventlist (index) -> sample (index). NOT sorted by
                         time -- rows are grouped by channel within a session,
                         exactly as 1.7.1 iterates.

If you want higher-resolution output (millisecond timestamps, full float
precision, additional channels, the secondary data field, etc.) write your own
script -- this one deliberately mirrors 1.7.1's behaviour and limitations.

Format facts verified against the OSCAR 2.0 source:
  * EventStoreType is qint16, stored little-endian            (machine_common.h)
  * value(i) = raw[i] * gain   (gain only, NO offset)         (event.cpp:59)
  * event time(i)    = first_time + uint32 delta[i]  (ms)     (event.cpp:51)
  * waveform time(i) = first_time + int(i * rate)    (ms)     (event.cpp:54)
  * compression is qCompress() == zlib stream prefixed with a
    4-byte big-endian uncompressed-length header               (event_data_repository.cpp)
  * either *_blob (raw) or *_compressed is populated, never both

Usage:
    python oscar_details_csv.py --db path/to/oscar.db \
        [--profile USERNAME | --profile-id N] \
        [--start YYYY-MM-DD] [--end YYYY-MM-DD] \
        [--output out.csv] [--day-split-hour 12]

Copyright (c) 2026 The OSCAR Team. GPL (see COPYING).
"""

import argparse
import array
import sqlite3
import sys
import zlib
from datetime import datetime, timedelta, date


# ---------------------------------------------------------------------------
# 1.7.1 channel set, in the exact order the Details report iterates it:
#   all = countlist + avglist
#   countlist = ahiChannels (schema append order) + extra flag channels
#   avglist   = the pressure family
# (channel_id, fallback_code) -- the code is read from the DB channels table
# when present, falling back to these static schema codes otherwise.
#
# Deliberately frozen at the 1.7.1 channel set. OSCAR 2.0 added
# ObstructiveHypopnea (0x1011) and CentralHypopnea (0x1012), which its own CSV
# export does include -- they are omitted here because the point of this script
# is byte-for-byte parity with 1.7.1. A 2.0 database recorded from a SEFAM, BMC
# Luna G3X or Loewenstein prisma device will therefore have hypopnea events this
# script does not emit. Add the two IDs after Hypopnea(0x1003) if you want the
# 2.0 set instead of the 1.7.1 one.
# ---------------------------------------------------------------------------
TARGET_CHANNELS = [
    # --- countlist: ahiChannels first (order from schema.cpp) ---
    (0x1001, "ClearAirway"),
    (0x1010, "AllApnea"),
    (0x1002, "Obstructive"),
    (0x1003, "Hypopnea"),
    (0x1004, "Apnea"),
    # --- countlist: explicit extras appended in exportcsv.cpp ---
    (0x1007, "VSnore"),
    (0x1008, "VSnore2"),
    (0x1006, "RERA"),
    (0x1005, "FlowLimit"),
    (0x100D, "SensAwake"),
    (0x100B, "NRI"),
    (0x100C, "ExP"),
    (0x100A, "LeakFlag"),
    (0x101E, "UserFlag1"),
    (0x101F, "UserFlag2"),
    (0x1009, "PressurePulse"),
    # --- avglist: pressure family ---
    (0x110C, "Pressure"),
    (0x11A4, "PressureSet"),
    (0x110D, "IPAP"),
    (0x11A5, "IPAPSet"),
    (0x110E, "EPAP"),
    (0x11A6, "EPAPSet"),
    (0x1113, "FLG"),
]

# Map channel_id -> position in the 1.7.1 iteration order (for row sorting).
CHANNEL_ORDER = {cid: i for i, (cid, _code) in enumerate(TARGET_CHANNELS)}

# Map channel_id -> the Event-column code. These are the compiled-in,
# untranslatable schema codes that 1.7.1's schema::channel[key].code() returns,
# so we use them directly rather than the per-profile channels.channel_code
# (which is normally identical but could diverge).
CHANNEL_CODE = {cid: code for cid, code in TARGET_CHANNELS}

EVL_WAVEFORM = 0
EVL_EVENT = 1


def _decompress(raw, comp):
    """Return the uncompressed bytes for a (blob, compressed_blob) column pair.

    Mirrors EventDataRepository load logic: prefer the compressed column when
    present; qCompress prepends a 4-byte big-endian length header before the
    raw zlib stream, so strip those 4 bytes before zlib.decompress().
    """
    if comp is not None and len(comp) > 0:
        return zlib.decompress(bytes(comp)[4:])
    if raw is not None and len(raw) > 0:
        return bytes(raw)
    return None


def _to_int16_array(buf):
    """Little-endian qint16 bytes -> array of Python ints."""
    a = array.array("h")            # 'h' == signed 16-bit, itemsize 2 everywhere
    a.frombytes(buf)
    if sys.byteorder == "big":
        a.byteswap()
    return a


def _to_uint32_array(buf):
    """Little-endian quint32 bytes -> array of Python ints."""
    typecode = "I" if array.array("I").itemsize == 4 else "L"
    a = array.array(typecode)
    a.frombytes(buf)
    if sys.byteorder == "big":
        a.byteswap()
    return a


def resolve_profile_id(con, username, profile_id):
    """Determine which profile to export. Errors out with guidance if ambiguous."""
    cur = con.cursor()
    if profile_id is not None:
        cur.execute("SELECT id, username FROM profiles WHERE id = ?", (profile_id,))
        row = cur.fetchone()
        if not row:
            sys.exit(f"No profile with id {profile_id}")
        return row[0], row[1]
    if username is not None:
        cur.execute("SELECT id, username FROM profiles WHERE username = ?", (username,))
        row = cur.fetchone()
        if not row:
            sys.exit(f"No profile named '{username}'")
        return row[0], row[1]
    cur.execute("SELECT id, username FROM profiles ORDER BY id")
    rows = cur.fetchall()
    if len(rows) == 1:
        return rows[0][0], rows[0][1]
    listing = "\n".join(f"  id={r[0]}  {r[1]}" for r in rows)
    sys.exit("Multiple profiles found; specify --profile or --profile-id:\n" + listing)


def oscar_day(start_ms, split_hour):
    """OSCAR 'day' date for a session: a day runs split_hour -> split_hour+24h.

    A session starting before split_hour (default noon) belongs to the previous
    calendar day's night.
    """
    st = datetime.fromtimestamp(start_ms / 1000.0)
    if st.hour < split_hour:
        return (st - timedelta(days=1)).date()
    return st.date()


def gather_eventlists(con, profile_id, start_date, end_date, split_hour):
    """Return target EventList metadata rows, filtered by date and sorted to
    match 1.7.1's iteration order (session start, then channel list order,
    then eventlist index)."""
    placeholders = ",".join("?" for _ in TARGET_CHANNELS)
    cur = con.cursor()
    cur.execute(
        f"""
        SELECT el.id, el.channel_id, el.event_type, el.first_time, el.count,
               el.rate, el.gain, el.eventlist_index,
               s.session_id, s.start_time
        FROM event_lists el
        JOIN sessions s ON el.session_id = s.id
        WHERE el.profile_id = ?
          AND el.channel_id IN ({placeholders})
          AND el.count > 0
        """,
        [profile_id] + [cid for cid, _ in TARGET_CHANNELS],
    )

    rows = []
    for r in cur.fetchall():
        (elid, cid, etype, first_time, count, rate, gain,
         evidx, session_id, start_time) = r
        day = oscar_day(start_time, split_hour)
        if start_date and day < start_date:
            continue
        if end_date and day > end_date:
            continue
        rows.append({
            "elid": elid, "cid": cid, "etype": etype,
            "first_time": first_time, "count": count,
            "rate": rate, "gain": gain if gain is not None else 1.0,
            "evidx": evidx, "session_id": session_id,
            "start_time": start_time,
        })

    rows.sort(key=lambda x: (x["start_time"],
                             CHANNEL_ORDER.get(x["cid"], 1 << 30),
                             x["evidx"]))
    return rows


def emit_eventlist(con, meta, write):
    """Decode one EventList's data and write its rows. Returns row count."""
    cur = con.cursor()
    cur.execute(
        """SELECT data_blob, data_compressed, time_blob, time_compressed
           FROM event_data WHERE eventlist_id = ?""",
        (meta["elid"],),
    )
    row = cur.fetchone()
    if row is None:
        return 0  # summary-only session or missing blob; nothing to dump

    data_blob, data_comp, time_blob, time_comp = row
    data_bytes = _decompress(data_blob, data_comp)
    if data_bytes is None:
        return 0
    data = _to_int16_array(data_bytes)

    gain = meta["gain"]
    first_time = meta["first_time"]
    n = min(meta["count"], len(data))

    if meta["etype"] == EVL_EVENT:
        time_bytes = _decompress(time_blob, time_comp)
        times = _to_uint32_array(time_bytes) if time_bytes is not None else None

        def t_ms(q):
            return first_time + (times[q] if times is not None else 0)
    else:  # waveform: time(i) = first_time + int(i * rate)
        rate = meta["rate"] or 0.0

        def t_ms(q):
            return first_time + int(q * rate)

    code = CHANNEL_CODE.get(meta["cid"], str(meta["cid"]))
    session_id = meta["session_id"]

    count = 0
    for q in range(n):
        # 1.7.1: fromSecsSinceEpoch(time/1000) -> local time, ISODate (seconds)
        secs = t_ms(q) // 1000
        dt = datetime.fromtimestamp(secs)
        stamp = dt.strftime("%Y-%m-%dT%H:%M:%S")
        value = data[q] * gain
        write(f"{stamp},{session_id},{code},{value:.2f}\n")
        count += 1
    return count


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Reproduce OSCAR 1.7.1's Details CSV export from an OSCAR 2.0 database.")
    ap.add_argument("--db", required=True, help="Path to the OSCAR SQLite database file")
    ap.add_argument("--profile", help="Profile username to export")
    ap.add_argument("--profile-id", type=int, help="Profile id (overrides --profile)")
    ap.add_argument("--start", help="Start date YYYY-MM-DD (OSCAR day, inclusive)")
    ap.add_argument("--end", help="End date YYYY-MM-DD (OSCAR day, inclusive)")
    ap.add_argument("--output", help="Output CSV path (default: stdout)")
    ap.add_argument("--day-split-hour", type=int, default=12,
                    help="Hour an OSCAR day starts (default 12 = noon; match your "
                         "profile's Day Split Time setting)")
    args = ap.parse_args(argv)

    start_date = date.fromisoformat(args.start) if args.start else None
    end_date = date.fromisoformat(args.end) if args.end else None

    # Read-only connection so we never touch the live database.
    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    try:
        profile_id, username = resolve_profile_id(con, args.profile, args.profile_id)
        metas = gather_eventlists(con, profile_id, start_date, end_date,
                                  args.day_split_hour)

        out = open(args.output, "w", newline="", encoding="utf-8") if args.output else sys.stdout
        try:
            write = out.write
            write("DateTime,Session,Event,Data/Duration\n")
            total = 0
            for meta in metas:
                total += emit_eventlist(con, meta, write)
        finally:
            if out is not sys.stdout:
                out.close()
    finally:
        con.close()

    print(f"Exported {total} rows for profile '{username}' "
          f"({len(metas)} eventlists).", file=sys.stderr)


if __name__ == "__main__":
    main()
