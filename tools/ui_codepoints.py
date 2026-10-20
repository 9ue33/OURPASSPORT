#!/usr/bin/env python3
"""Collect the code points a C/C++ source tree can actually display.

Why this exists
---------------
A subset LVGL font is only as good as its code point list.  Taking "all Chinese
characters anywhere in the tree" wastes hundreds of KB of Flash on comment text
that can never reach a label, while taking "only the file I remember" silently
loses a glyph that LVGL renders as a blank or a placeholder box -- a failure the
compiler cannot see.

So the list is derived mechanically from the sources: every character that
appears inside a *string literal* (or a char literal) is displayable, and every
character outside one is not.  Comments and #include paths are dropped.

Usage
-----
    ui_codepoints.py OUT.txt main/*.c main/*.h
    ui_codepoints.py OUT.txt --report --asc-range 0x20-0x7E main/*.h
    ui_codepoints.py OUT.txt --exclude app_font_16.c main/*.c

Output is one `U+XXXX` per line, ascending, so the file can be diffed and
regenerated deterministically.  `--report` additionally prints, per source file,
how many code points it contributes and which code points are new to it.
"""

from __future__ import annotations

import argparse
import glob
import os
import sys

# Escape sequences we may meet inside a literal.  Only the ones that can yield a
# displayable character matter; the rest (\n, \t, ...) are control codes and get
# filtered out by the "printable" rule below anyway.
SIMPLE_ESCAPES = {
    "n": 0x0A, "t": 0x09, "r": 0x0D, "0": 0x00, "\\": 0x5C,
    '"': 0x22, "'": 0x27, "?": 0x3F, "a": 0x07, "b": 0x08,
    "f": 0x0C, "v": 0x0B,
}


def strip_and_collect(text: str) -> tuple[str, set[int]]:
    """Return (code containing only literals, set of code points in them).

    A single forward pass with an explicit state machine: comments become
    whitespace, everything outside a literal is dropped from `code`, and the
    contents of literals are decoded (including \\xNN, \\uNNNN, \\UNNNNNNNN and
    octal escapes) into code points.
    """
    kept: list[str] = []
    cps: set[int] = set()
    i = 0
    n = len(text)
    state = "code"  # code | line_comment | block_comment | string | char

    while i < n:
        ch = text[i]

        if state == "code":
            if ch == "/" and i + 1 < n and text[i + 1] == "/":
                state = "line_comment"
                i += 2
                continue
            if ch == "/" and i + 1 < n and text[i + 1] == "*":
                state = "block_comment"
                i += 2
                continue
            if ch == '"':
                state = "string"
                kept.append('"')
                i += 1
                continue
            if ch == "'":
                state = "char"
                kept.append("'")
                i += 1
                continue
            i += 1
            continue

        if state == "line_comment":
            if ch == "\n":
                state = "code"
            i += 1
            continue

        if state == "block_comment":
            if ch == "*" and i + 1 < n and text[i + 1] == "/":
                state = "code"
                i += 2
                continue
            i += 1
            continue

        # Inside a literal.
        if ch == "\\":
            keep, consumed, cp = _decode_escape(text, i)
            if consumed == 0:  # unterminated literal
                state = "code"
                i += 1
                continue
            if keep:
                kept.append(keep)
            if cp is not None:
                cps.add(cp)
            i += consumed
            continue

        closer = '"' if state == "string" else "'"
        if ch == closer:
            kept.append(closer)
            state = "code"
            i += 1
            continue
        if ch == "\n":  # unterminated literal: bail out instead of eating the file
            state = "code"
            i += 1
            continue
        if ch == "\\" + "\n":
            i += 1
            continue

        kept.append(ch)
        cps.add(ord(ch))
        i += 1

    return "".join(kept), cps


def _decode_escape(text: str, i: int) -> tuple[str, int, int | None]:
    """Decode the escape at text[i] ('\\\\').  Returns (literal text, chars
    consumed, code point or None).  (\"\", 0, None) means end of input."""
    n = len(text)
    if i + 1 >= n:
        return "", 0, None
    c = text[i + 1]

    if c in SIMPLE_ESCAPES:
        cp = SIMPLE_ESCAPES[c]
        lit = _escape_literal(cp)
        return lit, 2, cp

    if c == "x":
        j = i + 2
        h = ""
        while j < n and len(h) < 2 and text[j] in "0123456789abcdefABCDEF":
            h += text[j]
            j += 1
        if not h:
            return "\\x", 2, None
        cp = int(h, 16) & 0xFF
        return _escape_literal(cp), j - i, cp

    if c == "u":
        j = i + 2
        h = text[j:j + 4]
        if len(h) == 4 and all(d in "0123456789abcdefABCDEF" for d in h):
            cp = int(h, 16)
            return _escape_literal(cp), 6, cp
        return "\\u", 2, None

    if c == "U":
        j = i + 2
        h = text[j:j + 8]
        if len(h) == 8 and all(d in "0123456789abcdefABCDEF" for d in h):
            cp = int(h, 16)
            return _escape_literal(cp), 10, cp
        return "\\U", 2, None

    if c in "01234567":
        j = i + 1
        o = ""
        while j < n and len(o) < 3 and text[j] in "01234567":
            o += text[j]
            j += 1
        cp = int(o, 8) & 0xFF
        return _escape_literal(cp), j - i, cp

    return text[i:i + 2], 2, None


def _escape_literal(cp: int) -> str:
    """Render a decoded code point so the output file is still valid text."""
    if cp == 0x5C:
        return "\\\\"
    if cp == 0x22:
        return '\\"'
    if cp == 0x27:
        return "\\'"
    return chr(cp)


def parse_ranges(spec: str) -> set[int]:
    out: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            out.update(range(int(lo, 0), int(hi, 0) + 1))
        else:
            out.add(int(part, 0))
    return out


def is_displayable(cp: int) -> bool:
    """Control codes are never rendered as glyphs; C0/C1 and the surrogate and
    noncharacter ranges only add noise (and lv_font_conv may reject them)."""
    if cp < 0x20 or cp == 0x7F:
        return False
    if 0x80 <= cp <= 0x9F:  # C1 controls
        return False
    if 0xD800 <= cp <= 0xDFFF:  # surrogates
        return False
    if 0xFDD0 <= cp <= 0xFDEF:
        return False
    if (cp & 0xFFFE) == 0xFFFE:
        return False
    if cp > 0x10FFFF:
        return False
    return True


def describe(cp: int) -> str:
    ch = chr(cp)
    if 0x20 <= cp < 0x7F:
        return f"U+{cp:04X} {ch}"
    if 0x4E00 <= cp <= 0x9FFF or 0x3400 <= cp <= 0x4DBF:
        return f"U+{cp:04X} {ch}"
    return f"U+{cp:04X}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", help="path of the generated U+XXXX list")
    ap.add_argument("sources", nargs="+", help="source files (globs allowed)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="basename to skip (repeatable); use for generated fonts")
    ap.add_argument("--asc-range", default="",
                    help="extra raw range spec forced in, e.g. 0x20-0x7E")
    ap.add_argument("--report", action="store_true",
                    help="print a per-file contribution report")
    args = ap.parse_args()

    files: list[str] = []
    for pattern in args.sources:
        matched = sorted(glob.glob(pattern)) or [pattern]
        for path in matched:
            if os.path.basename(path) in args.exclude:
                continue
            if not os.path.isfile(path):
                print(f"skip (not a file): {path}", file=sys.stderr)
                continue
            files.append(path)
    if not files:
        print("no source files matched", file=sys.stderr)
        return 1

    forced = parse_ranges(args.asc_range) if args.asc_range else set()
    forced = {cp for cp in forced if is_displayable(cp)}

    total: dict[int, str] = {}
    for path in files:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        _code, cps = strip_and_collect(text)
        cps = {cp for cp in cps if is_displayable(cp)}
        new = sorted(cp for cp in cps if cp not in total)
        for cp in cps:
            total.setdefault(cp, path)
        if args.report:
            han = sum(1 for cp in cps if 0x3400 <= cp <= 0x9FFF)
            print(f"{path}: {len(cps):5d} code points ({han} Han), "
                  f"{len(new)} new")
            if new and han <= 400:
                print("        new: " + " ".join(describe(cp) for cp in new))

    for cp in sorted(forced):
        total.setdefault(cp, "<--asc-range>")

    lines = [f"U+{cp:04X}" for cp in sorted(total)]
    with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")

    han = sum(1 for cp in total if 0x3400 <= cp <= 0x9FFF)
    ascii_n = sum(1 for cp in total if cp < 0x80)
    print(f"{args.output}: {len(total)} code points "
          f"({han} Han, {ascii_n} ASCII, {len(total) - han - ascii_n} other) "
          f"from {len(files)} file(s)")
    if args.report:
        print("\nprovenance of non-ASCII code points:")
        for cp in sorted(total):
            if cp >= 0x80:
                print(f"  {describe(cp):12s} {total[cp]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())