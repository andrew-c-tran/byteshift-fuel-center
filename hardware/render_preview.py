#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ByteShift Labs LLC
"""Render kicad/preview/pcb-front.png and pcb-back.png as the assembled-looking bare board:
green solder mask, ENIG pads, white silkscreen, drilled holes.

Needs kicad-cli and KiCad's python (pcbnew), ImageMagick `convert`, Pillow and NumPy.
Usage: python3 hardware/render_preview.py   (export.sh calls it)"""
import os, re, subprocess, tempfile
import numpy as np
import pcbnew
from PIL import Image, ImageDraw

KICAD = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kicad')
PCB = os.path.join(KICAD, 'fuel-center.kicad_pcb')
PX_PER_MM = 20
MARGIN_MM = 2
MASK, MASK_ON_CU = (22, 84, 48), (38, 114, 66)
GOLD, FR4, SILK = (214, 176, 96), (190, 170, 120), (242, 242, 236)


def layer_mask(layer, tmp):
    """Plot one layer black-on-white in page coordinates, return a boolean array (True = drawn)."""
    svg, png = os.path.join(tmp, 'l.svg'), os.path.join(tmp, 'l.png')
    subprocess.run(['kicad-cli', 'pcb', 'export', 'svg', '--layers', layer, '--black-and-white',
                    '--page-size-mode', '1', '--exclude-drawing-sheet', '--drill-shape-opt', '0',
                    '-o', svg, PCB], check=True, stdout=subprocess.DEVNULL)
    s = open(svg).read()
    # KiCad adds an invisible copy of every text for searching; ImageMagick would draw it
    open(svg, 'w').write(re.sub(r'<text\b[^>]*>.*?</text>', '', s, flags=re.S))
    subprocess.run(['convert', '-density', str(PX_PER_MM * 25.4), '-background', 'white', '-flatten',
                    svg, '-colorspace', 'Gray', png], check=True)
    return np.asarray(Image.open(png).convert('L')) < 128


def main():
    board = pcbnew.LoadBoard(PCB)
    bb = board.GetBoardEdgesBoundingBox()
    mm = pcbnew.ToMM
    x0, y0, x1, y1 = (mm(v) for v in (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom()))
    holes = [(p.GetPosition(), p.GetDrillSize().x) for p in board.GetPads() if p.GetDrillSize().x > 0]
    holes += [(v.GetPosition(), v.GetDrillValue()) for v in board.GetTracks() if v.GetClass() == 'PCB_VIA']
    with tempfile.TemporaryDirectory() as tmp:
        for side, pre in (('front', 'F'), ('back', 'B')):
            cu, opening, silk = (layer_mask(f'{pre}.{n}', tmp) for n in ('Cu', 'Mask', 'Silkscreen'))
            c = [round((v - m) * PX_PER_MM) for v, m in ((y0, 0), (y1, 0), (x0, 0), (x1, 0))]
            cu, opening, silk = (a[c[0]:c[1], c[2]:c[3]] for a in (cu, opening, silk))
            img = np.zeros(cu.shape + (4,), np.uint8)
            img[...] = MASK + (255,)
            img[cu] = MASK_ON_CU + (255,)
            img[opening & ~cu] = FR4 + (255,)
            img[opening & cu] = GOLD + (255,)
            img[silk & ~opening] = SILK + (255,)
            im = Image.fromarray(img, 'RGBA')
            draw = ImageDraw.Draw(im)
            for pos, drill in holes:
                cx, cy, r = (mm(pos.x) - x0) * PX_PER_MM, (mm(pos.y) - y0) * PX_PER_MM, mm(drill) / 2 * PX_PER_MM
                draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(0, 0, 0, 0))
            if side == 'back':
                im = im.transpose(Image.FLIP_LEFT_RIGHT)
            pad = MARGIN_MM * PX_PER_MM
            out = Image.new('RGBA', (im.width + 2 * pad, im.height + 2 * pad), (0, 0, 0, 0))
            out.paste(im, (pad, pad))
            out.save(os.path.join(KICAD, 'preview', f'pcb-{side}.png'), optimize=True)


if __name__ == '__main__':
    main()
