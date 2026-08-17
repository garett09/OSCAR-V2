# Apex Medical XT Auto — SD card format analysis

**Status:** Reverse-engineered; not vendor-documented.
**Validation:** Validated against actual export files.
**Source:** ported from the companion project `apex-xt-oscar-convert`
(`docs/apex_format/apf.md`, `docs/apex_format/ape.md`), which reverse-engineered
this format via static byte-diffing (`.APF`) and dynamic disassembly of the
vendor's Easy Compliance 3.4.1 Windows software (`.APE`), cross-checked against
several years of real device exports and Easy Compliance's own CSV/PDF reports.
This document covers only the raw device format. See
`Notes/loaders/Apex/APEX_LOADER_DESIGN.md` for
how it becomes an OSCAR loader.

---

## 1. Device storage layout

The device SD card exposes two fixed-size files side by side:

```
APAPDATA/00000000/00000000.APF
APAPDATA/00000000/00000000.APE
```

Both are rewritten in place on each connection — they are rolling tables, not
append logs. There is no per-connection dated folder on the device itself (any
dated folder naming seen in sample data reflects when a researcher happened to
export the card, not anything the device itself writes).

No checksum or CRC exists anywhere in either file. No compression is used
anywhere — every field is a fixed-size, direct-byte-encoded value, a packed
nibble, or part of a circular buffer.

## 2. `.APF` — session summary table

Fixed size: **21,250 bytes**. Two-byte header (unparsed, unknown), then up to
`(21250 − 2) / 29 ≈ 732` packed 29-byte records starting at offset `0x02`, with
no gaps between records. There is no record-count field: the table ends at the
first record that is entirely `0xFF` (`EMPTY_SLOT_BYTE`). Wraparound behaviour
once all ~732 slots are full has never been observed in real data — no sample
card has reached capacity — so it is untested against a real device, only
against a synthetic full table.

### Record layout (29 bytes, all single unsigned bytes — no endianness concerns)

| Offset | Size | Field | Encoding | Status |
|---|---|---|---|---|
| 0x00 | 1 | Start year | `year − 2000` | KNOWN |
| 0x01 | 1 | Start month | 1–12 | KNOWN |
| 0x02 | 1 | Start day | 1–31 | KNOWN |
| 0x03 | 1 | Start hour | 0–23 | KNOWN |
| 0x04 | 1 | Start minute | 0–59 | KNOWN |
| 0x05 | 1 | End year | `year − 2000` | KNOWN |
| 0x06 | 1 | End month | | KNOWN |
| 0x07 | 1 | End day | | KNOWN |
| 0x08 | 1 | End hour | | KNOWN |
| 0x09 | 1 | End minute | | KNOWN |
| 0x0A | 1 | Unknown | always `0x00` | UNKNOWN |
| 0x0B | 1 | Possibly "Duration − Util" (minutes) | matches on older records, fails on newer ones after a multi-year gap in one dataset — theorized firmware behaviour change | INFERRED, unreliable |
| 0x0C | 1 | Unknown constant | always `0x0B` (11) | UNKNOWN |
| 0x0D | 1 | Unknown constant | always `0x08` (8) | UNKNOWN |
| 0x0E | 1 | Unknown constant | always `0x08` (8) | UNKNOWN |
| 0x0F | 1 | Initial Pressure | `raw / 2.0` cmH2O | KNOWN |
| 0x10 | 1 | Max Pressure | `raw / 2.0` cmH2O | KNOWN |
| 0x11 | 1 | Min Pressure | `raw / 2.0` cmH2O | KNOWN |
| 0x12 | 1 | Possibly Ramp Time | always observed `0x00` | UNKNOWN, do not import |
| 0x13 | 1 | Average Pressure | `raw / 10.0` cmH2O | KNOWN |
| 0x14 | 1 | Average Leak | raw LPM, no scaling | KNOWN |
| 0x15–0x1C | 8 | — | exhaustively tested against event counts and against a checksum/CRC of the rest of the record; both ruled out across 8 investigation phases | UNKNOWN |

**No event counts (apnea/hypopnea/snore) exist anywhere in `.APF`.** They exist
only in `.APE`, and only for the most recent ≤18 sessions (§3).

## 3. `.APE` — per-minute detail ring buffer

Fixed size: **20,482 bytes** (`0x5002`). This is the actual finest-grained data
source the device exposes; found via dynamic disassembly of the vendor app
rather than static analysis (`.APE`'s own layout gave no purchase to
byte-diffing — the working decoder came from tracing the vendor app's live
memory access pattern, confirming the process indexes a `base + i × 0x276C`
struct array where `0x276C == 10092` decimal, matching a related legacy format's
per-session slot size exactly).

```
0x0000  +----------------------------------------------------+
        | header / misc (82 bytes) — unknown, never parsed      |
0x0052  +----------------------------------------------------+
        | session table: 18 entries × 8 bytes (circular)         |
0x0102  +----------------------------------------------------+
        | circular payload: per-minute detail records              |
        | (itself circular, wraps within this region)              |
0x5002  +----------------------------------------------------+
```

### Session table (offset `0x52`, 18 entries × 8 bytes)

A circular buffer of the most recent 18 sessions — a new session overwrites the
physically-oldest slot, not necessarily the file-order-last one.

| Byte(s) | Field | Notes |
|---|---|---|
| 0 | Marker | always `0xFE` |
| 1–5 | Session start timestamp | same 5-byte `(year−2000, month, day, hour, minute)` encoding as `.APF` |
| 6–7 | Payload cursor | little-endian u16 offset into the circular payload region where this session's minute records begin |

### Circular payload (`[0x102, 0x5002)`)

A session's detail run starts at `cursor + 2` (the `+2` skips a 2-byte unknown
sub-header), framed by an `FE FE FE` start marker and an `FF FF FF` end marker.
An optional 4-byte `00 00 00 00` block sometimes follows the start marker
(present or absent, both observed in real data — skip it if present). Between
the markers, 4-byte minute records are packed back-to-back with no gaps,
wrapping around the ring past `0x5002` back to `0x102` if the run crosses the
end of the file.

### Minute record (4 bytes — one per minute of therapy)

| Byte | Bits | Field | Status |
|---|---|---|---|
| 0 | 7:0 | Pressure sample, `raw / 10.0` cmH2O (same scale as `.APF`'s Average Pressure) | KNOWN |
| 1 | 7:0 | Event seconds-within-minute; values 0–59 validate the event-count nibbles, while values ≥60 mark those nibbles invalid/stale | KNOWN |
| 2 | 7:4 | Apnea count (0–15 events in this minute) | KNOWN |
| 2 | 3:0 | Hypopnea count (0–15 events in this minute) | KNOWN |
| 3 | 7:4 | Unexercised in all real data seen — always 0 | INFERRED, do not import |
| 3 | 3:0 | Vibratory Snoring count (0–15 events in this minute) | KNOWN |

This is a **count-per-minute record, not a discrete event log**. Event counts
are accepted only when byte 1 is in the valid seconds range 0–59; this rule
reproduced the Easy Compliance totals for every paired ground-truth session
evaluated. The nibble
preserves how many events occurred within the minute, but not their distinct
second-level timestamps or durations. Pressure is similarly one sample per minute, far
coarser than devices that log at ~1 sample/5–10 s.

### Session matching

`.APE` session-table timestamps are matched against already-parsed `.APF`
records by **exact 5-byte value equality**. A stale table entry whose ring
payload has since been overwritten by a newer session (marker byte mismatch,
an out-of-range cursor, or no `FF FF FF` terminator found within 1,440 minutes
of scanning) is a normal, expected condition — not corruption — and should be
skipped silently; that session simply has no per-minute detail.

**Only the most recent ≤18 sessions can ever have `.APE` detail.** This is a
hard ring-buffer capacity limit, confirmed against real Easy Compliance
Detail-tab screenshots, which report "Session Number: 18" once the device has
that much history regardless of the covered date range — it is a fixed session
count, not a time window. Every session older than that is summary-only
forever, by design of the device, not a parsing gap.

## 4. What is deliberately not decoded

- `.APF` bytes 0x15–0x1C (8 bytes per record) — fully uncharacterized.
- `.APE` minute-record byte 3's high nibble — inferred-unused, not sensor data.
- `.APF` byte 0x0B ("Duration − Util") — unreliable on newer records, not used
  by the loader (session duration is computed from start/end timestamps
  instead, which are fully reliable).
- `.APF` byte 0x12 ("possibly Ramp Time") — always observed `0x00`, unverified,
  not imported.
- No serial number or firmware version exists anywhere in either raw file. The
  `00000000` directory/file name is a fixed device constant, not an identifier.
- A separate `0x53`-stride sparse 8-byte table elsewhere on the card has been
  structurally identified as a circular buffer with its own sequence counter,
  believed to be a device alarm/event log unrelated to respiratory events —
  out of scope, not documented further here.
