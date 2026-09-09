#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Flag Qt .ts translations written in a script the target language does not use.

Two defects this catches, both of which have reached released builds:

1. A whole translation in the wrong language.  Czech.cz.ts was seeded from a copy
   of the Greek catalogue and 27 Greek strings were never translated over; they
   shipped in 2.0.1 and were reported by a user (GitLab #278).

2. One or two characters from an unrelated script wedged into the middle of an
   otherwise correct word, produced by the automatic translation in
   translate_ts_properly.py -- for example Afrikaans "Kopieer Verslагvariëteit"
   (Cyrillic "аг" standing in for Latin "ag") or Danish "sessionskaналdata".
   These are invisible on screen at a glance but render as tofu in many fonts.

Neither is detectable by eye across 60-odd catalogues, and neither is caught by
lupdate, lrelease or Qt Linguist.  Run this before cutting a release.

The check is deliberately one-directional: Latin is allowed in every language,
because product names and abbreviations (OSCAR, CPAP, ResMed, SpO2) legitimately
stay in Latin everywhere.  Only the reverse -- a non-Latin script appearing in a
language that does not use it -- is reported.

Usage:
    python ts_check_scripts.py [translations_dir]

Scans <translations_dir>/*.ts and <translations_dir>/qt/*.ts.  The directory
defaults to the Translations folder beside this script's repository root.

Exit status is 0 when every catalogue is clean and 1 when anything is reported,
so it can be used as a release gate.

Copyright (c) 2026 The OSCAR Team
"""

import os
import sys
import unicodedata
import xml.etree.ElementTree as ET


# Scripts each language uses in addition to Latin.  Every language OSCAR ships is
# listed, so that a code missing from this table is a new language rather than an
# oversight; unknown codes are reported instead of being silently assumed Latin.
EXTRA_SCRIPTS = {
    "af": set(),                                    # Afrikaans
    "ar": {"ARABIC"},                               # Arabic
    "bg": {"CYRILLIC"},                             # Bulgarian
    "cz": set(),                                    # Czech
    "da": set(),                                    # Danish
    "de": set(),                                    # German
    "el": {"GREEK"},                                # Greek
    "en_UK": set(),                                 # English (UK)
    "es": set(),                                    # Spanish
    "es_MX": set(),                                 # Spanish (Mexico)
    "fi": set(),                                    # Finnish
    "fil": set(),                                   # Filipino
    "fr": set(),                                    # French
    "he": {"HEBREW"},                               # Hebrew
    "hu": set(),                                    # Hungarian
    "it": set(),                                    # Italian
    "ja": {"HAN", "HIRAGANA", "KATAKANA"},          # Japanese
    "ko": {"HANGUL", "HAN"},                        # Korean
    "nl": set(),                                    # Dutch
    "no": set(),                                    # Norwegian
    "pl": set(),                                    # Polish
    "pt": set(),                                    # Portuguese
    "pt_BR": set(),                                 # Portuguese (Brazil)
    "ro": set(),                                    # Romanian
    "ru": {"CYRILLIC"},                             # Russian
    "sv": set(),                                    # Swedish
    "th": {"THAI"},                                 # Thai
    "tr": set(),                                    # Turkish
    "uk": {"CYRILLIC"},                             # Ukrainian
    "zh_CN": {"HAN", "BOPOMOFO"},                   # Chinese (Simplified)
    "zh_TW": {"HAN", "BOPOMOFO"},                   # Chinese (Traditional)
}

# Always acceptable, whatever the language.
ALWAYS_ALLOWED = {"LATIN", "MODIFIER", "COMBINING"}

# A character's script is taken from the first word of its Unicode name, which is
# cheap and needs no third-party module.  These first words are not scripts: they
# introduce symbols and punctuation that belong to no single language.  Without
# them the check produces false positives on, for example, Spanish "n.º 1" and the
# Japanese prolonged sound mark in "データ".
NEUTRAL_NAME_PREFIXES = {
    "FEMININE", "MASCULINE",            # ª º  ordinal indicators (Spanish, Portuguese)
    "KATAKANA-HIRAGANA",                # ー ゛ ゜ marks shared by both kana
    "IDEOGRAPHIC",                      # 、 。 CJK punctuation
    "FULLWIDTH", "HALFWIDTH",           # fullwidth Latin and punctuation forms
    "MICRO", "DEGREE", "OHM", "KELVIN", "ANGSTROM",
    "SUPERSCRIPT", "SUBSCRIPT", "VULGAR", "NUMERO",
}


def script_of(char):
    """Return the Unicode script name for *char*, or None if it is language-neutral.

    Punctuation, digits, whitespace and the symbols listed in
    NEUTRAL_NAME_PREFIXES carry no language, so they return None and are never
    reported.  An unassigned or private-use character returns "UNKNOWN", which is
    always reported -- it has no business in a translation.
    """
    if char.isspace() or not char.isalpha():
        return None
    try:
        name = unicodedata.name(char)
    except ValueError:
        return "UNKNOWN"
    first = name.split()[0]
    if first in NEUTRAL_NAME_PREFIXES:
        return None
    # unicodedata names Han characters "CJK UNIFIED IDEOGRAPH-4E00" and the like.
    return "HAN" if first == "CJK" else first


def language_code(path):
    """Extract the language code from a catalogue filename.

    Application catalogues are named "Czech.cz.ts" and the Qt dialog catalogues
    "oscar_qt_cz.ts"; both yield "cz".  Returns None if neither shape matches.
    """
    base = os.path.basename(path)[:-len(".ts")]
    if base.startswith("oscar_qt_"):
        return base[len("oscar_qt_"):]
    _, _, code = base.partition(".")
    return code or None


def describe(char):
    """Return "U+05E9 HEBREW LETTER SHIN" style text for a stray character."""
    try:
        name = unicodedata.name(char)
    except ValueError:
        name = "<unnamed>"
    return "U+%04X %s" % (ord(char), name)


# A translation in which most letters are foreign is a whole string in the wrong
# language; one in which only a few are is a correct string with characters
# corrupted inside it.  The two want different reporting, and different repairs.
WRONG_LANGUAGE_RATIO = 0.5


def check_file(path, allowed):
    """Return a list of findings for one catalogue.

    Each finding is a dict with the context name, source and target text, the
    offending characters and their positions, and whether the string looks wholly
    foreign or merely contains strays.  A parse failure is returned as a finding
    of its own rather than raised, so one damaged file does not stop the scan.
    """
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as err:
        return [{"error": "XML parse error: %s" % err}]

    findings = []
    for context in root.findall("context"):
        name = context.findtext("name") or "?"
        for message in context.findall("message"):
            translation = message.find("translation")
            if translation is None or not translation.text:
                continue
            text = translation.text

            letters = 0        # characters that carry a script at all
            strays = []        # (index, character) for each offending character
            scripts = set()
            for index, char in enumerate(text):
                script = script_of(char)
                if script is None:
                    continue
                letters += 1
                if script not in allowed:
                    strays.append((index, char))
                    scripts.add(script)

            if not strays:
                continue
            findings.append({
                "error": None,
                "context": name,
                "source": message.findtext("source") or "",
                "target": text,
                "type": translation.get("type"),
                "strays": strays,
                "scripts": scripts,
                "wholly_foreign": len(strays) >= letters * WRONG_LANGUAGE_RATIO,
            })
    return findings


def catalogues(directory):
    """Return every .ts file to scan: the application set plus the qt/ subset."""
    found = []
    for folder in (directory, os.path.join(directory, "qt")):
        if not os.path.isdir(folder):
            continue
        found += [os.path.join(folder, f)
                  for f in sorted(os.listdir(folder)) if f.endswith(".ts")]
    return found


def main():
    if len(sys.argv) > 1:
        directory = sys.argv[1]
    else:
        directory = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 os.pardir, "Translations")
    directory = os.path.abspath(directory)

    files = catalogues(directory)
    if not files:
        print("No .ts files found in: %s" % directory)
        return 2

    print()
    print("Script consistency check: %s" % directory)
    print("-" * 78)

    wrong_language = 0
    corrupted = 0
    errors = 0
    unknown = []

    for path in files:
        code = language_code(path)
        if code not in EXTRA_SCRIPTS:
            # Do not guess.  A new language whose script is not listed would
            # otherwise be reported top to bottom as one enormous false positive.
            unknown.append((os.path.basename(path), code))
            continue

        findings = check_file(path, ALWAYS_ALLOWED | EXTRA_SCRIPTS[code])
        if not findings:
            continue

        whole = sum(1 for f in findings if not f["error"] and f["wholly_foreign"])
        part = sum(1 for f in findings if not f["error"] and not f["wholly_foreign"])
        summary = ", ".join(s for s in
                            ("%d wrong-language" % whole if whole else "",
                             "%d corrupted" % part if part else "") if s)
        print("%s  (%s)" % (os.path.relpath(path, directory), summary or "error"))

        for finding in findings:
            if finding["error"]:
                print("    %s" % finding["error"])
                errors += 1
                continue

            flag = "" if finding["type"] is None else "  [%s]" % finding["type"]
            print("    %s: %r%s" % (finding["context"], finding["source"][:60], flag))

            if finding["wholly_foreign"]:
                # The whole string is in another language.  Naming every letter
                # would bury the one fact that matters, which is which language.
                wrong_language += 1
                print("      wholly %s: %r"
                      % ("/".join(sorted(finding["scripts"])), finding["target"][:60]))
            else:
                # A few characters inside an otherwise correct string.  Here the
                # exact characters and their positions are what makes it fixable.
                corrupted += 1
                print("      %r" % finding["target"][:70])
                for index, char in finding["strays"]:
                    print("        at %d: %s" % (index, describe(char)))
        print()

    for name, code in unknown:
        print("SKIPPED %s: language code %r is not in EXTRA_SCRIPTS -- add it"
              % (name, code))

    print("-" * 78)
    print("%d catalogues checked, %d skipped."
          % (len(files) - len(unknown), len(unknown)))
    print("%d translation(s) wholly in the wrong language, "
          "%d with foreign characters inside them, %d unreadable file(s)."
          % (wrong_language, corrupted, errors))
    print()

    return 1 if (wrong_language or corrupted or errors or unknown) else 0


if __name__ == "__main__":
    sys.exit(main())
