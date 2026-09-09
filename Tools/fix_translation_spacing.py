#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fix spacing and newlines in existing unfinished translations.

For type="unfinished" strings where the source has leading/trailing spaces or
embedded newlines, adjusts the existing translation to match — without
re-translating. English and French are skipped.

The file is parsed with ElementTree to decide what to change, then edited in the
raw text via ts_splice; writing a .ts file out through ElementTree destroys the
DOCTYPE and every XML comment. See Tools/ts_splice.py.
"""
import xml.etree.ElementTree as ET
import os
import glob

import ts_splice

TRANSLATIONS_DIR = r'C:\OSCAR\OSCAR-code\Translations'
SKIP_FILES = {'English.en_UK.ts', 'Francais.fr.ts'}


def corrected(source, translation):
    """Return *translation* with the source's spacing mirrored, or None if unchanged."""
    has_leading_space  = source != source.lstrip(' ')
    has_trailing_space = source != source.rstrip(' ')
    has_newline        = '\n' in source

    if not (has_leading_space or has_trailing_space or has_newline):
        return None

    new_translation = translation

    # Mirror source leading spaces onto translation
    if has_leading_space:
        n = len(source) - len(source.lstrip(' '))
        if len(new_translation) - len(new_translation.lstrip(' ')) != n:
            new_translation = ' ' * n + new_translation.lstrip(' ')

    # Mirror source trailing spaces onto translation
    if has_trailing_space:
        n = len(source) - len(source.rstrip(' '))
        if len(new_translation) - len(new_translation.rstrip(' ')) != n:
            new_translation = new_translation.rstrip(' ') + ' ' * n

    # Replace literal \n (two chars) with actual newline
    if has_newline and '\\n' in new_translation:
        new_translation = new_translation.replace('\\n', '\n')

    return new_translation if new_translation != translation else None


def fix_file(ts_file):
    filename = os.path.basename(ts_file)
    root = ET.parse(ts_file).getroot()
    all_messages = root.findall('.//message')

    resolved = {}

    for index, message in enumerate(all_messages):
        if message.get('numerus') == 'yes':
            continue

        source_elem = message.find('source')
        translation_elem = message.find('translation')

        if source_elem is None or translation_elem is None:
            continue
        if translation_elem.get('type') != 'unfinished':
            continue
        # A translation with children holds its text in <lengthvariant> elements
        # and its own text is only whitespace; leave those alone.
        if len(translation_elem):
            continue

        source = source_elem.text or ''
        translation = translation_elem.text or ''

        if not source or not translation:
            continue

        new_translation = corrected(source, translation)
        if new_translation is not None:
            resolved[index] = (False, new_translation)

    if not resolved:
        print(f"{filename}: nothing to fix")
        return 0

    text, newline = ts_splice.read_text(ts_file)
    text, fixed = ts_splice.splice_translations(text, resolved, len(all_messages), newline)
    ts_splice.write_text(ts_file, text)
    print(f"{filename}: fixed {fixed}")
    return fixed


def main():
    ts_files = sorted(glob.glob(os.path.join(TRANSLATIONS_DIR, '*.ts')))
    total = 0
    for ts_file in ts_files:
        if os.path.basename(ts_file) in SKIP_FILES:
            print(f"Skipping {os.path.basename(ts_file)}")
            continue
        total += fix_file(ts_file)
    print(f"\nTotal: {total} fixes")


if __name__ == '__main__':
    main()
