#!/usr/bin/env python3
"""Render HVIDEO boot artwork from type/curves, not an enlarged old bitmap.

Asset generation only. ROMs are built exclusively by build_hsvj_rom.ps1.
Requires Pillow, numpy and an Arial font file supplied with --font.
"""
import argparse
import io
import math
from pathlib import Path
import zipfile

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1920, 512
DESIGN = (3840, 2160)
FRAMES, FPS = 48, 24


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--font', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    ss = 4
    font = ImageFont.truetype(str(args.font), 260 * ss)
    bounds = font.getbbox('HVIDEO')
    glyphs = Image.new('L', (bounds[2] - bounds[0], bounds[3] - bounds[1]))
    ImageDraw.Draw(glyphs).text((-bounds[0], -bounds[1]), 'HVIDEO', fill=255, font=font)
    glyphs = glyphs.resize((980 * ss, 190 * ss), Image.Resampling.LANCZOS)
    mask = Image.new('L', (W * ss, H * ss))
    mask.paste(glyphs, ((W - 980) * ss // 2, 100 * ss))
    mask = mask.resize((W, H), Image.Resampling.LANCZOS)
    alpha = np.asarray(mask, dtype=np.float32) / 255
    glow = np.asarray(mask.filter(ImageFilter.GaussianBlur(10)), dtype=np.float32) / 255
    y, x = np.mgrid[:H, :W].astype(np.float32)
    tone = np.interp(y, [100, 160, 200, 240, 290], [130, 210, 246, 160, 125])
    text = alpha[..., None] * (tone[..., None] * np.array([.90, .96, 1.0]))
    text += glow[..., None] * np.array([5., 9., 14.])
    curve = 338 + 130 * ((x - W / 2) / 900) ** 2
    distance = y - curve
    edge = np.maximum(0, 1 - ((x - W / 2) / 910) ** 4)
    core = np.exp(-.5 * (distance / 1.15) ** 2) * edge
    halo = np.exp(-.5 * (distance / 10) ** 2) * edge

    def render(phase):
        spot_x = 180 + 1560 * (1 - math.cos(phase)) / 2
        spot = np.exp(-.5 * ((x - spot_x) / 70) ** 2)
        rgb = text.copy()
        rgb += core[..., None] * np.array([70., 155., 210.])
        rgb += halo[..., None] * np.array([1., 7., 15.])
        rgb += (spot * core)[..., None] * np.array([175., 100., 45.])
        rgb += (spot * np.exp(-.5 * (distance / 5) ** 2))[..., None] * np.array([4., 15., 25.])
        return Image.fromarray(np.clip(rgb, 0, 255).astype('uint8'), 'RGB')

    first = render(0)
    for name in ('logo.bmp', 'logo_kernel.bmp'):
        first.save(args.output / name)  # uncompressed 24-bit, native pixels
    with zipfile.ZipFile(args.output / 'bootanimation.zip', 'w', zipfile.ZIP_STORED) as z:
        def add(name, data):
            info = zipfile.ZipInfo(name, (2026, 9, 30, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            z.writestr(info, data)
        add('desc.txt', f'{DESIGN[0]} {DESIGN[1]} {FPS}\np 0 0 part0\n')
        trim = f'{W}x{H}+{(DESIGN[0]-W)//2}+{(DESIGN[1]-H)//2}\n'
        add('part0/trim.txt', trim * FRAMES)
        for n in range(FRAMES):
            image = render(2 * math.pi * n / FRAMES)
            buf = io.BytesIO()
            image.save(buf, format='PNG', compress_level=7)
            add(f'part0/{n:04d}.png', buf.getvalue())
    # Human-reviewable preview; not a boot input.
    canvas = Image.new('RGB', DESIGN)
    canvas.paste(first, ((DESIGN[0]-W)//2, (DESIGN[1]-H)//2))
    canvas.resize((1920, 1080), Image.Resampling.LANCZOS).save(args.output / 'preview.png')
    print(f'24-bit crop {W}x{H}, design {DESIGN}, {FRAMES} frames at {FPS} fps')


if __name__ == '__main__':
    main()
