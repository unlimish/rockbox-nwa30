#!/usr/bin/env python3
"""Render a Japanese bitmap font for Rockbox from a scalable font.

Rockbox ships no CJK font above 16px (GNU Unifont) and none of the large
Latin faces cover kanji, so a player with a 480x800 screen has nothing
readable to fall back on. Rasterising the whole of Noto Sans CJK would give
a font far too big to load; restricting it to JIS X 0208 - the kana,
punctuation and the 6,355 level 1+2 kanji - keeps everyday Japanese text
covered while staying a couple of megabytes.

The JIS X 0208 repertoire is derived from Python's own euc_jp codec rather
than a checked-in table: every valid two-byte code is decoded and whatever
maps to a character is kept, so the set is exactly what the codec knows and
needs no maintenance.

The size asked for is the height Rockbox will report, i.e. ascent plus
descent, because that is what its font names mean ("35-Adobe-Helvetica" is
35 pixels tall). That is not the em size a scalable font is rendered at -
Noto CJK's metrics are much taller than its em - so the em size is searched
for rather than assumed.

A weight may be given for a variable font, either as one of its named
instances ("Medium", "Bold") or as a number on the wght axis. Noto Sans JP
defaults to Thin, which is far too light to read on a device, so a font with
a weight axis is set to Regular unless asked otherwise.

    usage: make_cjk_font.py <font.otf/ttf> <pixel-height> <out.bdf> [weight]
"""
import sys
from PIL import Image, ImageDraw, ImageFont


def jis_x0208_codepoints():
    """Every character in JIS X 0208, via the euc_jp codec."""
    chars = set()
    for row in range(1, 95):
        for cell in range(1, 95):
            raw = bytes((0xA0 + row, 0xA0 + cell))
            try:
                chars.add(raw.decode("euc_jp"))
            except UnicodeDecodeError:
                pass
    return chars


def wanted_codepoints():
    cps = set(range(0x20, 0x7F))                 # ASCII
    cps |= {ord(c) for c in jis_x0208_codepoints()}
    cps |= set(range(0xFF61, 0xFFA0))            # halfwidth katakana
    cps |= {0x00A5, 0x203E, 0x2010, 0x2212}      # yen, overline, dashes
    return sorted(cps)


def render(font, cp, ascent):
    """Rasterise one codepoint. Returns (bitmap rows as ints, w, h, xoff, yoff,
    advance), all in pixels, with the origin on the baseline.

    Rendered through FreeType's monochrome mode rather than by thresholding a
    greyscale image: the hinter then snaps stems to the pixel grid, so the two
    uprights of a character like the kanji for "mouth" come out the same
    weight. Thresholding an antialiased render leaves one of them a pixel
    fatter than the other, which is very visible at this size."""
    ch = chr(cp)
    advance = int(round(font.getlength(ch)))
    # A 1-bit canvas makes PIL ask FreeType for a monochrome glyph, so the
    # hinter runs; drawing onto a greyscale canvas and thresholding does not.
    pad = ascent
    size = ascent * 3
    img = Image.new("1", (size, size), 0)
    draw = ImageDraw.Draw(img)
    origin = (pad, pad + ascent)  # baseline
    draw.text(origin, ch, font=font, fill=1, anchor="ls")
    box = img.getbbox()
    if box is None:  # blank glyph, e.g. space
        return [], 0, 0, 0, 0, advance
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    rows = []
    px = img.load()
    for y in range(y0, y1):
        bits = 0
        for x in range(x0, x1):
            bits = (bits << 1) | (1 if px[x, y] else 0)
        # BDF pads each row to a whole number of bytes, on the right
        bits <<= (-w) % 8
        rows.append(bits)
    xoff = x0 - origin[0]
    yoff = origin[1] - y1  # distance from baseline to the bottom of the box
    return rows, w, h, xoff, yoff, advance


def set_weight(font, weight):
    """Pick a weight on a variable font. Named instance or wght number."""
    try:
        names = [n.decode("ascii", "replace") if isinstance(n, (bytes, bytearray))
                 else n for n in font.get_variation_names()]
    except OSError:
        return None  # not a variable font, whatever it is is what we get
    if weight is None:
        weight = "Regular"
    for name in names:
        if name.lower() == str(weight).lower():
            font.set_variation_by_name(name)
            return name
    font.set_variation_by_axes([float(weight)])
    return str(weight)


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    src, size, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    weight = sys.argv[4] if len(sys.argv) == 5 else None

    def load(px):
        font = ImageFont.truetype(src, px)
        return font, set_weight(font, weight)

    # find the em size whose ascent+descent is the requested height
    em = size
    for candidate in range(size, 0, -1):
        a, d = load(candidate)[0].getmetrics()
        if a + d <= size:
            em = candidate
            break
    font, picked = load(em)
    if picked is not None:
        print("weight: %s" % picked, file=sys.stderr)
    ascent, descent = font.getmetrics()
    print("em size %d gives height %d (%d+%d), asked for %d"
          % (em, ascent + descent, ascent, descent, size), file=sys.stderr)
    size = em  # everything below rasterises at the em size
    cps = wanted_codepoints()

    glyphs = []
    for i, cp in enumerate(cps):
        glyphs.append((cp, render(font, cp, ascent)))
        if i % 1000 == 0:
            print("  %d/%d" % (i, len(cps)), file=sys.stderr)

    maxw = max((g[1][1] for g in glyphs), default=size)
    with open(out, "w") as f:
        f.write("STARTFONT 2.1\n")
        xlfd_weight = "bold" if picked and "bold" in picked.lower() else "medium"
        f.write("FONT -rockbox-noto sans cjk jp-%s-r-normal--%d-*-*-*-*-*-iso10646-1\n"
                % (xlfd_weight, size))
        f.write("SIZE %d 75 75\n" % size)
        f.write("FONTBOUNDINGBOX %d %d 0 %d\n" % (maxw, ascent + descent, -descent))
        f.write("STARTPROPERTIES 3\n")
        f.write("FONT_ASCENT %d\n" % ascent)
        f.write("FONT_DESCENT %d\n" % descent)
        f.write("DEFAULT_CHAR %d\n" % ord("?"))
        f.write("ENDPROPERTIES\n")
        f.write("CHARS %d\n" % len(glyphs))
        for cp, (rows, w, h, xoff, yoff, advance) in glyphs:
            f.write("STARTCHAR U+%04X\n" % cp)
            f.write("ENCODING %d\n" % cp)
            f.write("SWIDTH %d 0\n" % int(advance * 1000 / size))
            f.write("DWIDTH %d 0\n" % advance)
            f.write("BBX %d %d %d %d\n" % (w, h, xoff, yoff))
            f.write("BITMAP\n")
            nbytes = (w + 7) // 8
            for bits in rows:
                f.write("%0*X\n" % (nbytes * 2, bits))
            f.write("ENDCHAR\n")
        f.write("ENDFONT\n")
    print("wrote %s: %d glyphs, ascent %d descent %d" % (out, len(glyphs), ascent, descent),
          file=sys.stderr)


if __name__ == "__main__":
    main()
