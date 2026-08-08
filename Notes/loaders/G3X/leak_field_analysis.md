# BMC G3X Leak Field Analysis
# Can Unintentional Leak Be Derived for SC.74+ Firmware?

**Date:** 2026-03-26
**Author:** Reverse-engineering investigation
**Status:** Open — awaiting decision on OSCAR implementation
**Updated:** 2026-07-29 — third machine added (E5 B25A Plus); see "Update" below

---

## Update 2026-07-29 — E5 B25A Plus: 0x52A is live, this analysis does not apply

A third machine is now available: **E5 B25A Plus**, firmware `E5-1.02.05.02`. It is a
bilevel (AutoS) device, not a G3.

**Its 0x52A field is populated** — 18.7% of packets non-zero across the first five nights
(range 6.0%–46.0% per night). So this machine *does* provide unintentional leak and is
**not** subject to the "unrecoverable for SC.74+" conclusion below. Its statistics match
PAP-Link closely once zeros are included: 2026-07-18 gives avg 0.56 / median 0.00 /
95th 3.20 L/min against PAP-Link's 0.70 / 0.00 / 3.00.

**Correction (same day):** an earlier revision of this section claimed the E5 card
"confirms the 0.16 L/min-per-raw-unit scale". That was wrong. It rested on the medians
agreeing, and both medians are 0.00 — which holds under *any* scale, so it constrains
nothing. The E5 scale is in fact **~0.10, not 0.16**; see below.

Its 0x568 field reproduces the behaviour documented below for the G3 machines: a
pressure-independent ~15.7 L/min baseline, `r(pressure, 0x568) = −0.206`, with the median
per pressure bucket flat at 15.84 / 15.68 / 15.20 / 17.12 L/min from 7 to 11 cmH₂O. That
is a third machine supporting the conclusion that **0x568 is not unintentional leak**.

### E5 leak scale is ~0.10 L/min per raw unit, not 0.16

The 0.16 factor was calibrated against PAP-Link on a Luna G3X (config `110A40113`) and
applied to every G3X device. It **overstates leak on the E5 by roughly 60%**.

Calibration for one night, against a PAP-Link readout of the same night:

| Measure | PAP-Link | Raw units | Implied scale |
|---|---:|---:|---:|
| Mean | 0.70 L/min | 6.881 | **0.1017** |
| Stable graph section | ~2 L/min | ~21.9 | **~0.091** |
| 95th percentile | 3.0 L/min | 25 | 0.1200 |
| Maximum | 11.2 L/min | 220 | 0.0509 |

Only the first two are usable. The mean uses every sample and is independent of how the
two programs define percentiles; the stable-section reading is a direct point comparison
with no statistics at all. Both give ~0.10.

The 95th percentile and maximum are **not** usable for calibration, because PAP-Link's tail
statistics are demonstrably not computed the way OSCAR's are: its reported maximum of
11.2 L/min is below the raw peak under any single linear scale, implying smoothing or
mask-off exclusion on its side. Expect OSCAR's 95th percentile to read slightly *low* and
its maximum distinctly *high* against PAP-Link even after the scale fix.

Smoothing alone cannot explain the discrepancy: a moving average preserves the mean, and
the means differ (1.10 vs 0.70). Verified across 5 s–300 s windows.

**Confidence: medium.** One card, one night, one PAP-Link readout. A second E5 sample would
establish whether 0.10 is exact or merely close. Implemented as a platform-scoped constant
in `G3xLeakScaleTenthsPerRawUnit()`, keyed on the `E5` firmware prefix, so G3 devices keep
the confirmed 0.16.

---

Two OSCAR bugs were found and fixed while investigating this card — see
`Notes/Developer Notes/BUG_FIXES.md` (2026-07-29). Note that the second one interacts with
this document:

1. Zero-valued leak samples were discarded before storage, inflating every leak statistic
   (66.9% of samples on this card read zero).
2. The 0x52A-vs-0x568 probe ran **per day** on only 200 packets, so it selected 0x568 on
   4 of 19 nights on this card. **Any prior OSCAR-side leak observation on a machine whose
   0x52A non-zero fraction sits near the 10% threshold may have been reading a mix of the
   two fields across nights.** The raw-data analysis in this document read the fields
   directly and is unaffected.

---

## Background

OSCAR reports and computes based on **unintentional (mask-fit) leak** — the air escaping
around the mask seal, not through the mask's intentional vent holes. This is the standard
for all other CPAP machines supported by OSCAR.

For BMC G3X devices, two firmware generations have been observed:

| Config code (IDX 0x48) | SC firmware (IDX 0x0345) | User version | Machines |
|------------------------|--------------------------|--------------|----------|
| `110A40113` | `G3-2.SC.72.01` | G3-2.11.x | JCCPAP (Luna G3X) |
| `880A40383` | `G3-2.SC.74.03` | G3-2.12.x | Kavolodin (G3 A20), Patient 2 |

In SC.72, OSCAR reads unintentional leak from waveform packet offset **0x52A** (scale
0.16 L/min per raw unit), confirmed against PAP-Link. In SC.74+, 0x52A is always zero
and the Phase 3.5 adaptive probe falls back to **0x568**.

The question is: does 0x568 give unintentional leak for SC.74+, or is it something else?
And if not, can unintentional leak be derived from what is available?

---

## What 0x568 Actually Represents

### Both firmware versions: identical baseline

| Machine | Firmware | 0x568 baseline mean | 0x568 baseline median |
|---------|----------|--------------------:|----------------------:|
| JCCPAP | SC.72 | 16.7 L/min | ~16 L/min |
| Kavolodin | SC.74+ | 16.5 L/min | ~16 L/min |

The baseline distributions are statistically indistinguishable. 0x568 carries the **same
signal** in both firmware versions.

### No correlation with pressure

Real vent flow through mask holes increases with pressure. 0x568 shows:
- JCCPAP: r(pressure, 0x568) = **0.036** (essentially zero)
- Kavolodin: r(pressure, 0x568) = **0.001** (essentially zero)

This rules out 0x568 being intentional vent flow.

### No correlation with 0x52A in JCCPAP

In JCCPAP where both fields are populated:
- r(0x52A, 0x568) = **−0.067** (essentially zero)
- 0x52A mean = 12.9 L/min (unintentional, confirmed vs PAP-Link)
- 0x568 mean = 16.6 L/min

These two fields measure **independent quantities**, not two views of the same leak.

### Implication for "total leak" hypothesis

If 0x568 were total leak (vent + unintentional), then in JCCPAP:
- vent = 0x568 − 0x52A = 16.6 − 12.9 = **3.7 L/min** mean

That is far too low for any real mask at typical CPAP pressures. Therefore
**0x568 is not total leak**.

### What 0x568 probably is

Its pressure-independence and constant ~16 L/min baseline suggest it is the machine's
estimate of **intentional vent flow** — which in practice is approximately constant across
the 8–12 cmH₂O operating range for the mask type used in both test machines. It does
spike to 100–350 L/min during mask repositioning or removal, because at that point the
"vent" calculation becomes meaningless and raw flow dominates.

### Consequence for SC.74+

In SC.74+, 0x52A is dead and 0x568 carries the vent signal — not unintentional leak.
OSCAR's current Phase 3.5 selection (falling back to 0x568 when 0x52A is absent) results
in displaying the **vent component** as the patient's "Leak", with a baseline of ~16 L/min
even with a perfectly fitting mask. This is misleading.

---

## Can Unintentional Leak Be Derived for SC.74+?

### A) Direct field scan (offsets 0x52A–0x5A0 in Kavolodin)

A full survey found no offset with a distribution consistent with unintentional leak
(expected: 0–30 L/min, mostly near zero with occasional spikes). Every offset in this
range is either zero, a known clinical scalar (RR at 0x530, minute ventilation at 0x52E),
or a machine-internal counter with an implausible range.

**Conclusion: No direct unintentional leak field exists in SC.74+ packets.**

### B) Subtraction approach: unintentional = total − vent

In principle, unintentional = total_leak − intentional_vent_flow. The intentional vent
is a function of pressure and mask type. A regression on JCCPAP's 0x182 "total machine
flow" field produced:

```
vent_raw ≈ 0.128 × pressure_raw(0x76E) + 87.6
```

However, applying this to Kavolodin produces a vent estimate of **~32 L/min** at median
pressure (8.89 cmH₂O), which **exceeds** the total 0x568 value of 16.8 L/min. This is
physically impossible, and results in the derived unintentional being zero 98% of the time.

The regression coefficient was derived in raw units of the 0x182 flow field (scale ÷10
for L/min), which differs from the 0x52A leak field (scale ×0.16 for L/min). The
coefficient cannot be applied directly to 0x568.

More fundamentally, the vent curve is published by the **mask manufacturer**, not BMC,
and varies significantly between mask models within the same type category (full face,
nasal, nasal pillow). The device does not record which mask the patient is using — at
most it records a broad mask type, and even that has not been identified in the G3X
packet format. The correct vent curve is therefore unknowable from the device data alone.

**Conclusion: The subtraction approach fails — the vent curve is not obtainable from
the device data, and estimating it from a different machine's regression is not reliable.**

### C) 0x182 "total machine flow" DC offset

The waveform packet at 0x182 contains total machine output flow = tidal + unintentional
+ vent. The DC offset (mean of 0x182 minus mean of tidal flow at 0x56E) equals the
combined non-tidal flow (vent + unintentional).

| Machine | 0x182 DC offset | Vent component (derived) | Uninten component (derived) |
|---------|----------------|--------------------------|------------------------------|
| JCCPAP  | 273.7 raw | 273.7 − 80.6 = 193.1 raw | 80.6 raw ≈ 0x52A (69.8 raw, coefficient ~1.009) |
| Kavolodin | 219.7 raw | ~202.3 raw (vent estimate) | ~17.4 raw residual |

The Kavolodin residual of 17.4 raw (in 0x182 units) is a rough upper bound on the
unintentional component, but converting to L/min requires knowing the scale factor of
0x182, which differs from 0x52A/0x568. Without calibration against known values, this
cannot be reliably extracted.

**Conclusion: 0x182 provides a weak constraint but not a usable unintentional leak signal.**

### D) Self-calibration (subtract per-session minimum of 0x568)

Using the 5th percentile of 0x568 within a session as a proxy for the vent baseline:

| Night | vent_proxy (p05) | % non-zero after subtraction | Mean non-zero |
|-------|-----------------|------------------------------|---------------|
| 2026-03-01 | 11.4 L/min | 94.9% | 7.4 L/min |
| 2026-03-02 | 10.4 L/min | 94.6% | 8.9 L/min |

For a well-fitting mask, we would expect near-zero unintentional leak >90% of the time.
Getting 94–95% non-zero means the per-session minimum is not a valid vent baseline —
the variability in 0x568 is intrinsic noise, not unintentional leak signal.

**Conclusion: Self-calibration produces clinically implausible results.**

---

## Summary of Findings

| Approach | Result |
|----------|--------|
| Direct field in packet | **Not found** — 0x52A is dead, no other offset qualifies |
| Subtraction (total − vent) | **Fails** — vent curve unknown; JCCPAP-derived coefficient gives vent > total |
| 0x182 DC decomposition | **Too imprecise** — scale factor unknown; needs independent calibration |
| Per-session minimum subtraction | **Clinically implausible** — produces 95% non-zero, not consistent with good mask fit |

**Unintentional leak is not recoverable from SC.74+ waveform data with available information.**

---

## Options for OSCAR Implementation

### Option 1: Display 0x568 as-is with corrected label
Show 0x568 as "Total Leak" or "Mask Leak" (not "Leak"). Accept that the baseline will
be ~16 L/min even for perfect mask fit. Spikes faithfully indicate poor mask fit.

**Pro:** Shows real data. Spikes are clinically meaningful.
**Con:** Baseline misleads users into thinking they always have elevated leak. Statistics
(mean, 95th percentile) will be inflated relative to other CPAP machines.

### Option 2: Suppress leak channel for SC.74+
Do not populate CPAP_Leak for machines where Phase 3.5 selects 0x568.

**Pro:** Honest — no misleading data.
**Con:** Completely loses leak information, including spike detection.

### Option 3: Excess leak = max(0, 0x568 − threshold)
Subtract a fixed threshold representing the expected vent baseline. From both machines,
the vent is approximately 100 raw = 16 L/min. So:

```
excess_leak = max(0, raw_0x568 − 100) × 0.16 L/min
```

This gives near-zero at good baseline and clinically meaningful spikes at poor fit.

**Pro:** Behaviorally similar to what a patient with JCCPAP sees from 0x52A.
**Con:** The threshold 100 raw (16 L/min) is derived from only two machines of the same
firmware family. Vent curves are published by mask manufacturers (not BMC) and vary
significantly across mask models within the same type category. The machine does not
record which mask the patient is using. The threshold is therefore a heuristic with
unknown validity across the SC.74+ device population.

Variant: derive the threshold per-session from the 5th percentile of 0x568 packets
where flow is also non-zero (i.e., during active therapy, not startup/shutdown zeros).
This is adaptive but adds complexity.

### Option 4: Contact BMC / review PAP-Link source
PAP-Link reports a "Leak" value that appears to be processed differently from either
0x52A or 0x568 raw values (PAP-Link showed 0 and 8 L/min for Kavolodin, vs 0x568
median of ~16 L/min). If the PAP-Link processing is known or can be reverse-engineered
from more test data, this may reveal the correct interpretation.

---

## Known Unknowns

1. **What exactly is 0x568?** Its near-constant pressure-independence and identical
   baseline across firmware versions suggest it is an internal machine estimate (possibly
   the firmware's own vent leak model), not a direct sensor reading.

2. **Does SC.74+ firmware record unintentional leak anywhere else in the packet?**
   A focused survey of offsets 0x000–0x5A0 found nothing, but the full 2048-byte packet
   was not exhaustively scanned for Kavolodin with leak-detection heuristics.

3. **What does PAP-Link use for leak on SC.74+ machines?** PAP-Link showed values
   inconsistent with both 0x52A (dead) and 0x568 (raw). Knowing PAP-Link's source
   field and any processing applied would answer the question definitively.

4. **Is the mask vent curve known?** Mask vent curves are published by the **mask
   manufacturer** (ResMed, Fisher & Paykel, etc.), not the CPAP manufacturer — BMC
   does not publish them. CPAP machines typically allow the user to select only a broad
   mask *type* (full face, nasal, nasal pillow), not a specific model. Vent curves vary
   substantially between different masks within the same type category. The G3X IDX
   format does not appear to record even mask type in its per-day records (unlike legacy
   BMC which stored it at IDX 0x140–0x165). In practice, the subtraction approach is
   not viable: neither the exact mask model nor its vent curve is recoverable from the
   device data.

---

## Scripts Used in This Analysis

- `Notes/G3X/kavolodin_uninten_leak.py` — main investigation script
- `Notes/G3X/compare_0x568_both.py` — 0x568 comparison between JCCPAP and Kavolodin
- `Notes/G3X/compare_packet_layouts.py` — earlier packet layout survey
- `Notes/G3X/kavolodin_fw880_leak.py` — initial SC.74+ leak field identification

## Test Files

- JCCPAP: `C:/OSCAR/TestFiles/JCCPAP/A31.....000` (SC.72)
- Kavolodin: `C:/OSCAR/TestFiles/Kavolodin BMC G3 A20/A31....000` (SC.74+)
