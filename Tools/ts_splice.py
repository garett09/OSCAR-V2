#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Edit the <translation> bodies of a Qt .ts file without disturbing anything else.

A .ts file cannot be round-tripped through ElementTree.  Its writer discards the
<!DOCTYPE TS> declaration and every XML comment, and re-indents the document in
its own style -- two-space, "'" in the XML declaration, "line=\"35\" />".  Writing
through it stripped the DOCTYPE from all 31 catalogues, would have deleted the
translator attribution comment at the top of Ukrainska.uk.ts (the only catalogue
carrying one), and turned any one-string change into a whole-file diff.

So the callers parse with ElementTree to decide *what* to change, and use
splice_translations() to make the change in the raw file text.  Only the body of
each <translation> element -- or of its first <lengthvariant> -- is replaced;
attributes, indentation, comments, the DOCTYPE and the line endings survive
untouched.

Copyright (c) 2026 The OSCAR Team
"""

import re

# "&" has to be substituted first or it would corrupt the entities that follow.
# Qt's own tools escape all five of these in element text; matching them keeps a
# later lupdate run from rewriting the lines we touched.
XML_ESCAPES = (("&", "&amp;"), ("<", "&lt;"), (">", "&gt;"),
               ('"', "&quot;"), ("'", "&apos;"))

MESSAGE_RE = re.compile(r'<message\b[^>]*>.*?</message>', re.S)
TRANSLATION_RE = re.compile(r'(<translation\b[^>]*>)(.*?)(</translation>)', re.S)
LENGTHVARIANT_RE = re.compile(r'(<lengthvariant\b[^>]*>)(.*?)(</lengthvariant>)', re.S)


def xml_escape(text):
    """Escape text for an XML element body, the way Qt's own tools do."""
    for raw, entity in XML_ESCAPES:
        text = text.replace(raw, entity)
    return text


def message_count(root):
    """Number of <message> elements in a parsed tree, in document order.

    Pass the result to splice_translations() as *total_messages*; the indices in
    *resolved* are positions in this same ordering.
    """
    return len(root.findall('.//message'))


def splice_translations(text, resolved, total_messages, newline='\n'):
    """Replace translation bodies in *text*; return (new_text, count).

    *resolved* maps a message's document-order index to (is_variant, new_text).
    *new_text* is given unescaped and is escaped here.  A variant translation
    holds its text in <lengthvariant> children and its own text is whitespace
    that must stay as it is, so only the first variant is written.

    Edits are applied back to front so the offsets of the earlier ones stay
    valid.  If the number of <message> blocks found in the text does not match
    *total_messages* the text and the parsed tree have diverged, and rather than
    write to a guessed offset this raises.
    """
    blocks = list(MESSAGE_RE.finditer(text))
    if len(blocks) != total_messages:
        raise RuntimeError(
            "found %d <message> blocks in the file text but %d in the parsed "
            "tree; refusing to edit" % (len(blocks), total_messages))

    edits = []
    for index, (is_variant, new_text) in resolved.items():
        block = blocks[index]
        target = (LENGTHVARIANT_RE if is_variant
                  else TRANSLATION_RE).search(block.group(0))
        if target is None:
            print("  warning: no place to put translation for message %d" % index)
            continue

        replacement = xml_escape(new_text)
        if newline != '\n':
            replacement = replacement.replace('\n', newline)
        edits.append((block.start() + target.start(2),
                      block.start() + target.end(2), replacement))

    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text, len(edits)


def read_text(ts_file):
    """Read a .ts file as text, returning (text, newline) with its own line ending."""
    text = open(ts_file, 'rb').read().decode('utf-8')
    return text, ('\r\n' if '\r\n' in text else '\n')


def write_text(ts_file, text):
    """Write text back as bytes, so nothing re-encodes the line endings."""
    open(ts_file, 'wb').write(text.encode('utf-8'))
