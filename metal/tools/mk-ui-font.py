#!/usr/bin/env python3
"""Bake DejaVu Sans 10px coverage into a freestanding, proportional UI font.
Usage: python mk-ui-font.py /path/to/DejaVuSans.ttf apps/ui_font.h
Pillow is only required to regenerate the checked-in atlas, not to build the OS.
The font's redistribution terms accompany the generated atlas in apps/.
"""
import pathlib
import sys
from PIL import Image, ImageDraw, ImageFont

font = ImageFont.truetype(sys.argv[1], 10)
advances, glyphs = [], []
for code in range(32, 127):
    ch = chr(code)
    advance = max(2, int(font.getlength(ch) + 0.5))
    image = Image.new('L', (12, 12))
    ImageDraw.Draw(image).text((0, -2), ch, font=font, fill=255)
    advances.append(advance)
    glyphs.append(list(image.tobytes()))
lines = ['/* Generated DejaVu Sans coverage; see UI-FONT-LICENSE.txt. */',
         '#ifndef OUTRUN_UI_FONT_H', '#define OUTRUN_UI_FONT_H',
         '#define UI_FONT_W 12', '#define UI_FONT_H 12',
         'static const unsigned char ui_font_advance[95] = {',
         ','.join(map(str, advances)), '};',
         'static const unsigned char ui_font_coverage[95][144] = {']
for glyph in glyphs:
    lines.append('{' + ','.join(map(str, glyph)) + '},')
lines.extend(['};', '#endif', ''])
pathlib.Path(sys.argv[2]).write_text('\n'.join(lines), encoding='ascii')
print('Generated', len(glyphs), 'antialiased glyphs:', sys.argv[2])
