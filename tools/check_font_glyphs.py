#!/usr/bin/env python3
"""Structural validator for LVGL ``lv_font_conv`` output.

Parses a generated ``.c`` font, performs the *same* cmap lookup LVGL's
``get_glyph_dsc_id()`` performs (FORMAT0_TINY / FORMAT0_FULL / SPARSE_TINY /
SPARSE_FULL), and then proves for every requested code point that the glyph
descriptor and the bitmap bytes behind it look like real rasterised pixels
instead of zeros or garbage.

Run with:  python3 tools/check_font_glyphs.py <font.c> --symbols "A0"
"""

from __future__ import annotations

import argparse
import math
import re
import sys

CMAP_FORMAT0_TINY = "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY"
CMAP_FORMAT0_FULL = "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL"
CMAP_SPARSE_TINY = "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY"
CMAP_SPARSE_FULL = "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def numbers(body: str) -> list[int]:
    return [int(tok, 0) for tok in re.findall(r"0[xX][0-9a-fA-F]+|\d+", strip_comments(body))]


class GlyphDsc:
    __slots__ = ("bitmap_index", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y")

    def __init__(self, bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y):
        self.bitmap_index = bitmap_index
        self.adv_w = adv_w
        self.box_w = box_w
        self.box_h = box_h
        self.ofs_x = ofs_x
        self.ofs_y = ofs_y


class Cmap:
    def __init__(self, block: str, unicode_lists, ofs_lists):
        def grab(name, default="0"):
            m = re.search(r"\.%s\s*=\s*([^,}]+)" % name, block)
            return m.group(1).strip() if m else default

        self.range_start = int(grab("range_start"), 0)
        self.range_length = int(grab("range_length"), 0)
        self.glyph_id_start = int(grab("glyph_id_start"), 0)
        self.list_length = int(grab("list_length"), 0)
        self.type = grab("type").replace(" ", "")
        self.unicode_list = unicode_lists.get(grab("unicode_list"), None)
        self.glyph_id_ofs_list = ofs_lists.get(grab("glyph_id_ofs_list"), None)

    def lookup(self, cp: int):
        """Return (glyph_id, how) mirroring lv_font_get_glyph_dsc_fmt_txt()."""
        if cp < self.range_start or cp > self.range_start + self.range_length:
            return None
        if self.type == CMAP_FORMAT0_TINY:
            return self.glyph_id_start + cp - self.range_start, "format0_tiny"
        if self.type == CMAP_FORMAT0_FULL:
            if self.glyph_id_ofs_list is None:
                return None, "format0_full-NULL-LIST(crash risk)"
            idx = cp - self.range_start
            if idx >= len(self.glyph_id_ofs_list):
                return None, "format0_full-out-of-range"
            return self.glyph_id_start + self.glyph_id_ofs_list[idx], "format0_full"
        if self.type in (CMAP_SPARSE_TINY, CMAP_SPARSE_FULL):
            if self.unicode_list is None:
                return None, "sparse-NULL-LIST(crash risk)"
            target = cp - self.range_start
            lst = self.unicode_list
            lo, hi = 0, min(self.list_length, len(lst)) - 1
            while lo <= hi:
                mid = (lo + hi) // 2
                v = lst[mid]
                if v == target:
                    if self.type == CMAP_SPARSE_TINY:
                        return self.glyph_id_start + mid, "sparse_tiny"
                    if self.glyph_id_ofs_list is None:
                        return None, "sparse_full-NULL-LIST(crash risk)"
                    return self.glyph_id_start + self.glyph_id_ofs_list[mid], "sparse_full"
                if v < target:
                    lo = mid + 1
                else:
                    hi = mid - 1
            return None, "sparse-miss"
        return None, "unknown-type"


class LvglFont:
    def __init__(self, path: str):
        self.path = path
        with open(path, "r", encoding="utf-8", errors="strict") as fh:
            self.text = fh.read()

        m = re.search(r"\*\s*Size:\s*(\d+)\s*px", self.text)
        self.size = int(m.group(1)) if m else 0
        m = re.search(r"\*\s*Bpp:\s*(\d+)", self.text)
        self.bpp = int(m.group(1)) if m else 0
        m = re.search(r"const lv_font_t\s+(\w+)\s*=", self.text)
        self.font_name = m.group(1) if m else "?"
        m = re.search(r"\.line_height\s*=\s*(\d+)", self.text)
        self.line_height = int(m.group(1)) if m else 0
        m = re.search(r"\.bitmap_format\s*=\s*(\d+)", self.text)
        self.bitmap_format = int(m.group(1)) if m else 0

        # lv_font_conv <= 1.5.2 emitted the legacy misspelling "gylph_bitmap".
        m = re.search(r"\bg[ly]{1,2}ph_bitmap\[\]\s*=\s*\{(.*?)\n\};", self.text, re.S)
        if not m:
            raise SystemExit("FATAL: no glyph_bitmap[] found in %s" % path)
        self.bitmap = bytes(b & 0xFF for b in numbers(m.group(1)))

        m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", self.text, re.S)
        if not m:
            raise SystemExit("FATAL: no glyph_dsc[] found in %s" % path)
        entries = re.findall(r"\{(.*?)\}", m.group(1), re.S)
        self.glyph_dsc: list[GlyphDsc] = []
        for e in entries:
            def f(name):
                mm = re.search(r"\.%s\s*=\s*(-?\d+)" % name, e)
                return int(mm.group(1)) if mm else 0

            self.glyph_dsc.append(
                GlyphDsc(f("bitmap_index"), f("adv_w"), f("box_w"), f("box_h"), f("ofs_x"), f("ofs_y"))
            )
        if not self.glyph_dsc or (self.glyph_dsc[0].box_w, self.glyph_dsc[0].box_h) != (0, 0):
            raise SystemExit("FATAL: glyph_dsc[0] is not the reserved zero glyph")

        self.unicode_lists = {}
        self.ofs_lists = {}
        for name, body in re.findall(r"static const uint16_t (\w+)\[\]\s*=\s*\{(.*?)\};", self.text, re.S):
            self.unicode_lists[name] = numbers(body)
        for name, body in re.findall(r"static const uint8_t (\w+)\[\]\s*=\s*\{(.*?)\};", self.text, re.S):
            self.ofs_lists[name] = numbers(body)
        # any other integer list (defensive: different widths for huge ranges)
        for name, body in re.findall(r"static const uint(?:16|32)_t (\w+)\[\]\s*=\s*\{(.*?)\};", self.text, re.S):
            self.unicode_lists.setdefault(name, numbers(body))

        m = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", self.text, re.S)
        if not m:
            raise SystemExit("FATAL: no cmaps[] found in %s" % path)
        self.cmaps = [Cmap(b, self.unicode_lists, self.ofs_lists) for b in re.findall(r"\{(.*?)\}", m.group(1), re.S)]
        if not self.cmaps:
            raise SystemExit("FATAL: empty cmaps[] in %s" % path)

    def lookup(self, cp: int):
        for cmap in self.cmaps:
            res = cmap.lookup(cp)
            if res is None:
                continue
            gid, how = res
            if gid == 0:
                return None, how
            return gid, how
        return None, "no-cmap-match"

    def glyph_bytes(self, gd: GlyphDsc) -> bytes:
        nbytes = (gd.box_w * gd.box_h * self.bpp + 7) // 8
        return self.bitmap[gd.bitmap_index:gd.bitmap_index + nbytes]

    def pixels(self, gd: GlyphDsc):
        bpp = self.bpp
        raw = self.glyph_bytes(gd)
        vals = []
        bitpos = 0
        mask = (1 << bpp) - 1
        for _ in range(gd.box_w * gd.box_h):
            byte = raw[bitpos // 8] if bitpos // 8 < len(raw) else 0
            shift = 8 - bpp - (bitpos % 8)
            vals.append((byte >> shift) & mask)
            bitpos += bpp
        rows = [vals[y * gd.box_w:(y + 1) * gd.box_w] for y in range(gd.box_h)]
        return rows

    def art(self, gd: GlyphDsc, ramp=" .:-=+*#%@") -> list[str]:
        top = (1 << self.bpp) - 1
        rows = self.pixels(gd)
        out = []
        for row in rows:
            out.append("".join(ramp[min(len(ramp) - 1, v * len(ramp) // (top + 1))] for v in row))
        return out


def parse_symbols(text: str) -> list[int]:
    """Accept literal characters, U+XXXX / 0xXXXX tokens and comma/space separators."""
    cps: list[int] = []
    i = 0
    while i < len(text):
        tok = re.match(r"\s*U\+([0-9a-fA-F]{2,6})|0[xX]([0-9a-fA-F]{2,6})", text[i:])
        if tok:
            cps.append(int(tok.group(1) or tok.group(2), 16))
            i += tok.end()
            continue
        ch = text[i]
        i += 1
        if ch in ", \t\r\n":
            continue
        cps.append(ord(ch))
    seen = []
    for c in cps:
        if c not in seen:
            seen.append(c)
    return seen


def parse_range_spec(text: str) -> list[int]:
    """Parse lv_font_conv's --range syntax: ``0x20-0x7E,0x4E2D,0x3000-0x303F``."""
    cps: list[int] = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        m = re.fullmatch(r"0[xX]([0-9a-fA-F]+)\s*-\s*0[xX]([0-9a-fA-F]+)", part)
        if m:
            lo, hi = int(m.group(1), 16), int(m.group(2), 16)
            if hi < lo:
                raise SystemExit("FATAL: inverted range in --range spec: %s" % part)
            cps.extend(range(lo, hi + 1))
            continue
        m = re.fullmatch(r"0[xX]([0-9a-fA-F]+)", part)
        if m:
            cps.append(int(m.group(1), 16))
            continue
        raise SystemExit("FATAL: bad --range item: %s" % part)
    return cps


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate LVGL font glyph bitmaps.")
    ap.add_argument("font_c")
    ap.add_argument("--symbols", help="characters (or U+XXXX tokens) that must be present")
    ap.add_argument("--symbols-file",
                    help="read the required set from a file (one U+XXXX token or literal "
                         "per line); appended to --symbols")
    ap.add_argument("--range", dest="range_spec", default="",
                    help="lv_font_conv --range syntax (0x20-0x7E,0x4E2D) added to the required set")
    # Default negative control: U+FE0F (VARIATION SELECTOR-16) has no glyph in
    # Noto Sans SC, and it is the code point real text lists most often drag in
    # (emoji sequences), so a font built from such a list will not contain it.
    ap.add_argument("--expect-missing", default="U+FE0F",
                    help="comma separated code points that must NOT be present "
                         "(default U+FE0F, absent from Noto Sans SC)")
    ap.add_argument("--blank", default="U+0020,U+00A0,U+3000",
                    help="code points that are legitimately rendered as an empty bitmap")
    ap.add_argument("--margin", type=int, default=5,
                    help="allowed slack for box_w/box_h over the nominal size (accented Latin glyphs "
                         "legitimately exceed the nominal pixel size by a few pixels)")
    ap.add_argument("--limit", type=int, default=64, help="max code points drawn as ASCII art")
    ap.add_argument("--no-art", action="store_true")
    args = ap.parse_args()
    if args.symbols_file:
        try:
            with open(args.symbols_file, "r", encoding="utf-8") as fh:
                args.symbols = (args.symbols or "") + fh.read()
        except OSError as exc:
            ap.error("cannot read --symbols-file %s: %s" % (args.symbols_file, exc))
    if not args.symbols and not args.range_spec:
        ap.error("at least one of --symbols / --symbols-file / --range is required")

    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    font = LvglFont(args.font_c)
    present = parse_symbols(args.symbols or "")
    if args.range_spec:
        seen = set(present)
        for cp in parse_range_spec(args.range_spec):
            if cp not in seen:
                seen.add(cp)
                present.append(cp)
    missing = parse_symbols(args.expect_missing)
    blank_cps = set(parse_symbols(args.blank))

    print("font file      : %s" % args.font_c)
    print("font symbol    : %s" % font.font_name)
    print("size / bpp     : %d px / %d bpp" % (font.size, font.bpp))
    print("line_height    : %d   bitmap_format: %d" % (font.line_height, font.bitmap_format))
    print("bitmap bytes   : %d" % len(font.bitmap))
    print("glyph_dsc[0]   : bitmap_index=%d box=%dx%d (reserved zero glyph)"
          % (font.glyph_dsc[0].bitmap_index, font.glyph_dsc[0].box_w, font.glyph_dsc[0].box_h))
    print("cmap subtables : %d" % len(font.cmaps))
    for i, c in enumerate(font.cmaps):
        print("  [%d] type=%-38s range=0x%X..0x%X glyph_id_start=%d list_length=%d"
              % (i, c.type, c.range_start, c.range_start + max(c.range_length - 1, 0),
                 c.glyph_id_start, c.list_length))

    # Structural: bitmap_index must be monotonically non-decreasing and within the array.
    failures: list[str] = []
    prev = -1
    for gid, gd in enumerate(font.glyph_dsc):
        need = (gd.box_w * gd.box_h * font.bpp + 7) // 8
        if gd.bitmap_index < prev:
            failures.append("glyph id %d: bitmap_index %d went backwards (prev %d)" % (gid, gd.bitmap_index, prev))
        if gd.bitmap_index + need > len(font.bitmap):
            failures.append("glyph id %d: bitmap_index %d + %d bytes overruns bitmap[] (%d)"
                            % (gid, gd.bitmap_index, need, len(font.bitmap)))
        prev = gd.bitmap_index

    header = "%-8s %-3s %-13s %-6s %-8s %-7s %-5s %-5s %-5s %-5s %-5s %-7s %-8s %s" % (
        "cp", "ch", "cmap", "glyph", "bitmap_i", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y",
        "bytes", "nonzero", "distinct", "ink%")
    print("\n== coverage / bitmap checks ==")
    print(header)
    print("-" * len(header))

    art_rows: list[tuple[int, list[str]]] = []
    for cp in present:
        gid, how = font.lookup(cp)
        ch = chr(cp)
        if gid is None:
            failures.append("U+%04X (%s): no glyph via cmap (%s)" % (cp, ch, how))
            print("%-8s %-3s %-13s %-6s %s" % ("U+%04X" % cp, ch, how, "-", "FAIL: not covered"))
            continue
        if gid >= len(font.glyph_dsc):
            failures.append("U+%04X: cmap yields glyph id %d, out of range" % (cp, gid))
            print("%-8s %-3s %-13s %-6d %s" % ("U+%04X" % cp, ch, how, gid, "FAIL: glyph id out of range"))
            continue
        gd = font.glyph_dsc[gid]
        raw = font.glyph_bytes(gd)
        nbytes = (gd.box_w * gd.box_h * font.bpp + 7) // 8
        nonzero = sum(1 for b in raw if b)
        distinct = len(set(raw))
        pixels = [v for row in font.pixels(gd) for v in row]
        ink = (sum(1 for v in pixels if v) / len(pixels) * 100.0) if pixels else 0.0

        problems = []
        notes = []
        # The reserved zero/placeholder entry is glyph id 0 with a 0x0 box and
        # bitmap_index 0. Any other glyph with an empty box is either a genuine
        # blank character (space-like, listed in --blank) or a silent conversion
        # failure. A bitmap_index of 0 is legitimate for whichever glyph comes
        # first once the blank glyphs before it have contributed no bytes, so it
        # is not used on its own as the placeholder signal.
        blank = (gd.box_w == 0 or gd.box_h == 0)
        if blank and cp in blank_cps:
            notes.append("blank glyph (whitespace)")
        elif blank:
            problems.append("empty box %dx%d (placeholder glyph)" % (gd.box_w, gd.box_h))
        if gd.box_w > font.size + args.margin or gd.box_h > font.size + args.margin:
            problems.append("box %dx%d exceeds size %d + margin %d" % (gd.box_w, gd.box_h, font.size, args.margin))
        if not 0 < gd.adv_w <= 2 * font.size * 16:
            problems.append("implausible adv_w %d" % gd.adv_w)
        if len(raw) != nbytes:
            problems.append("truncated bitmap: %d of %d bytes" % (len(raw), nbytes))
        if nonzero == 0 and not blank:
            problems.append("bitmap is all zero")
        if distinct <= 1 and not blank:
            if ink >= 99.0:
                # A glyph whose ink box is fully covered (a stem, "|", "I", a
                # solid bar) is legitimately one repeated byte value.
                notes.append("solid-block glyph")
            else:
                problems.append("bitmap has %d distinct byte value(s) (degenerate)" % distinct)
        if abs(gd.ofs_x) > font.size or abs(gd.ofs_y) > font.size:
            problems.append("implausible offsets ofs_x=%d ofs_y=%d" % (gd.ofs_x, gd.ofs_y))

        status = "OK" if not problems else "FAIL: " + "; ".join(problems)
        if not problems and notes:
            status = "OK (%s)" % ", ".join(notes)
        print("%-8s %-3s %-13s %-6d %-8d %-7d %-5d %-5d %-5d %-5d %-5d %-7d %-8d %-6.1f %s"
              % ("U+%04X" % cp, ch, how, gid, gd.bitmap_index, gd.adv_w, gd.box_w, gd.box_h,
                 gd.ofs_x, gd.ofs_y, len(raw), nonzero, distinct, ink, status))
        if problems:
            failures.append("U+%04X (%s): %s" % (cp, ch, "; ".join(problems)))
        if not args.no_art and len(art_rows) < args.limit:
            art_rows.append((cp, font.art(gd)))

    print("\n== negative control (must be absent) ==")
    for cp in missing:
        gid, how = font.lookup(cp)
        if gid is None:
            print("U+%04X %s: OK absent (%s) -> LVGL falls back / draws placeholder" % (cp, chr(cp), how))
        else:
            failures.append("U+%04X (%s): unexpectedly covered by glyph id %d" % (cp, chr(cp), gid))
            print("U+%04X %s: FAIL unexpectedly covered by glyph id %d" % (cp, chr(cp), gid))

    if art_rows and not args.no_art:
        print("\n== per-pixel readback (decoded exactly as LVGL PLAIN %d bpp) ==" % font.bpp)
        for cp, rows in art_rows:
            print("U+%04X %s" % (cp, chr(cp)))
            for row in rows:
                print("    |%s|" % row)
            print()

    print("== result ==")
    if failures:
        print("FAILED (%d problem(s)):" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("PASS: %d code point(s) covered with non-degenerate bitmaps; %d negative-control code point(s) absent."
          % (len(present), len(missing)))
    return 0


if __name__ == "__main__":
    sys.exit(main())