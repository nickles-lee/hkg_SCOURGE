#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Boot-message translations for the AppStandbyClock safety disclaimer.

`config/messages.json` maps a firmware locale (the `translation_<locale>.qm`
suffix) to the text shown on the startup safety screen. The patcher locates
the SAFETY_OBEY entries by their source-key hashes in the Qt `.qm` offset
table and replaces each translation payload with the mapped text, padded
with U+0020 to the original length. The `.qm` structure and byte size are
preserved, so the shipping binary can load it unchanged.

Patching by key (not by the old translation text) means an already-patched
`.qm` can be refreshed again: the key hash is stable, the previous message
is irrelevant.

All locales present in the firmware must be mapped. A locale with no entry
is a hard error: silently leaving the OEM disclaimer in one language is the
failure mode this module exists to prevent.

TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
"""
import json
import os
import struct
import xml.etree.ElementTree as ET

QM_DIR = "./app/share/AppStandbyClock/localization/qm"
TS_DIR = "./app/share/AppStandbyClock/localization/ts"

# Qt .qm container (qtranslator.cpp).
QM_MAGIC = b"\x3c\xb8\x64\x18\xca\xef\x9c\x95\xcd\x21\x1c\xbf\x60\xa1\xbd\xdd"
TAG_END = 1
TAG_TRANSLATION = 3
TAG_SOURCETEXT = 6
TAG_CONTEXT = 7
TAG_COMMENT = 8
TAG_OBSOLETE1 = 5
SECTION_HASHES = 0x42
SECTION_MESSAGES = 0x69


def load_messages(path):
    """Load and validate the locale -> text map."""
    with open(path, encoding="utf-8") as f:
        messages = json.load(f)
    if not isinstance(messages, dict) or not messages:
        raise ValueError("%s: expected a non-empty locale -> text object" % path)
    for locale, text in messages.items():
        if not isinstance(text, str) or not text.strip():
            raise ValueError("%s: empty message for locale %r" % (path, locale))
        if text != text.strip():
            raise ValueError("%s: locale %r has leading/trailing whitespace" %
                             (path, locale))
        if any(line != line.rstrip() for line in text.split("\n")):
            raise ValueError("%s: locale %r has trailing line whitespace" %
                             (path, locale))
    return messages


def qm_entry(locale):
    return "%s/translation_%s.qm" % (QM_DIR, locale)


def ts_entry(locale):
    return "%s/translation_%s.ts" % (TS_DIR, locale)


def locales_in_dir(localization_dir):
    """Return the locales of an extracted localization/qm directory."""
    qm_dir = os.path.join(localization_dir, "qm")
    return sorted(
        name[len("translation_"):-len(".qm")]
        for name in os.listdir(qm_dir)
        if name.startswith("translation_") and name.endswith(".qm")
    )


def safety_keys(ts_path):
    """Return the unique (source, comment) SAFETY_OBEY keys in a stock .ts."""
    keys = []
    tree = ET.parse(ts_path)
    for ctx in tree.getroot().findall("context"):
        for msg in ctx.findall("message"):
            source = msg.findtext("source") or ""
            if "SAFETY_OBEY" in source:
                key = (source, msg.findtext("comment") or "")
                if key not in keys:
                    keys.append(key)
    return keys


def _elf_hash(text):
    h = 0
    for byte in text.encode("utf-8"):
        h = ((h << 4) + byte) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h or 1


def _sections(data):
    if data[:len(QM_MAGIC)] != QM_MAGIC:
        raise ValueError("not a Qt .qm file")
    sections = {}
    off = len(QM_MAGIC)
    while off + 5 <= len(data):
        tag = data[off]
        block_len = struct.unpack(">I", data[off + 1:off + 5])[0]
        off += 5
        if not tag or not block_len:
            break
        if off + block_len > len(data):
            raise ValueError("truncated .qm section 0x%02x" % tag)
        sections[tag] = (off, block_len)
        off += block_len
    return sections


def _message_spans(data, start, end):
    """Return (translation spans, source text, comment) for one .qm entry."""
    spans = []
    source = comment = None
    p = start
    while p < end:
        tag = data[p]
        p += 1
        if tag == TAG_END:
            break
        if tag == TAG_OBSOLETE1:
            p += 4
            continue
        if tag not in (TAG_TRANSLATION, TAG_SOURCETEXT, TAG_CONTEXT, TAG_COMMENT):
            raise ValueError("unsupported .qm message tag 0x%02x at %d" % (tag, p - 1))
        if p + 4 > end:
            raise ValueError("truncated .qm message tag 0x%02x" % tag)
        length = struct.unpack(">I", data[p:p + 4])[0]
        p += 4
        if p + length > end:
            raise ValueError("truncated .qm message payload")
        if tag == TAG_TRANSLATION:
            spans.append((p, length))
        elif tag == TAG_SOURCETEXT:
            source = data[p:p + length].decode("utf-8", "replace")
        elif tag == TAG_COMMENT:
            comment = data[p:p + length].decode("utf-8", "replace")
        p += length
    return spans, source, comment


def _matching_spans(data, sections, source, comment):
    """Yield the translation spans for one key, or raise if the key is absent."""
    hash_off, hash_len = sections[SECTION_HASHES]
    msg_off, msg_len = sections[SECTION_MESSAGES]
    msg_end = msg_off + msg_len
    wanted = _elf_hash(source + comment)
    found = False
    for i in range(0, hash_len - 7, 8):
        h, ro = struct.unpack(">II", data[hash_off + i:hash_off + i + 8])
        if h != wanted:
            continue
        start = msg_off + ro
        if not msg_off <= start < msg_end:
            raise ValueError("message offset out of range: %s" % source)
        spans, found_source, found_comment = _message_spans(data, start, msg_end)
        if found_source is not None and found_source != source:
            continue
        if found_comment is not None and found_comment != comment:
            continue
        found = True
        for span in spans:
            yield span
    if not found:
        raise ValueError("key not found in .qm: %s" % source)


def patch_qm(data, keys, new_text):
    """Replace every listed SAFETY_OBEY entry in a .qm bytearray.

    Returns the number of message entries patched. Raises ValueError when a
    key is missing, a block is malformed, or the text does not fit.
    """
    sections = _sections(data)
    if SECTION_HASHES not in sections or SECTION_MESSAGES not in sections:
        raise ValueError("missing .qm hashes/messages section")
    new = new_text.encode("utf-16be")

    patched = 0
    for source, comment in keys:
        spans = list(_matching_spans(data, sections, source, comment))
        if not spans:
            raise ValueError("no translation entry for %s" % source)
        for pos, length in spans:
            if length % 2 or len(new) % 2:
                raise ValueError("odd UTF-16 slot for %s (%d bytes)" %
                                 (source, length))
            if len(new) > length:
                raise ValueError("message too long for %s "
                                 "(%d > %d UTF-16 units)" %
                                 (source, len(new) // 2, length // 2))
            data[pos:pos + length] = new + b"\x00\x20" * ((length - len(new)) // 2)
            if data[pos:pos + len(new)] != new:
                raise ValueError("replacement failed for %s" % source)
            patched += 1
    return patched


def read_qm_messages(data, keys):
    """Return {source: [stored translations]} for the listed keys."""
    sections = _sections(data)
    if SECTION_HASHES not in sections or SECTION_MESSAGES not in sections:
        raise ValueError("missing .qm hashes/messages section")
    out = {}
    for source, comment in keys:
        out[source] = [
            data[pos:pos + length].decode("utf-16be")
            for pos, length in _matching_spans(data, sections, source, comment)
        ]
    return out

