#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
from PIL import Image
import string
import sys


def build_symbols() -> list[str]:
    return list(string.ascii_letters + string.digits + "!$%&()*+,-./:;<=>?@[]^_{|}~")


def generate_xpm(src_png: Path, out_pm: Path, size: int = 48, colors: int = 64, alpha_threshold: int = 128) -> None:
    img = Image.open(src_png).convert("RGBA")
    img = img.resize((size, size), Image.LANCZOS)

    alpha = img.getchannel("A")
    mask = alpha.point(lambda a: 255 if a >= alpha_threshold else 0)

    rgb = img.convert("RGB")
    quant = rgb.quantize(colors=colors, method=Image.MEDIANCUT)
    palette = quant.getpalette()

    pixels = quant.load()
    mask_px = mask.load()

    used = set()
    for y in range(size):
        for x in range(size):
            if mask_px[x, y] != 0:
                used.add(pixels[x, y])

    used = sorted(used)
    colors_needed = len(used) + 1

    symbols = build_symbols()
    cpp = 1
    if colors_needed > len(symbols):
        cpp = 2
        symbols = [a + b for a in string.ascii_letters + string.digits
                   for b in string.ascii_letters + string.digits]
    if colors_needed > len(symbols):
        raise RuntimeError("Too many colors for symbol set")

    sym_iter = iter(symbols)
    none_sym = next(sym_iter)
    color_map: dict[int | str, str] = {"None": none_sym}
    for idx in used:
        color_map[idx] = next(sym_iter)

    lines: list[str] = []
    lines.append("/* XPM */")
    lines.append("static char *ck_grab_camera_pm[] = {")
    lines.append(f'"{size} {size} {colors_needed} {cpp}",')

    lines.append(f'"{none_sym} c None",')
    for idx in used:
        r = palette[idx * 3]
        g = palette[idx * 3 + 1]
        b = palette[idx * 3 + 2]
        lines.append(f'"{color_map[idx]} c #{r:02X}{g:02X}{b:02X}",')

    for y in range(size):
        row = []
        for x in range(size):
            if mask_px[x, y] == 0:
                row.append(none_sym)
            else:
                row.append(color_map[pixels[x, y]])
        lines.append('"' + ''.join(row) + '",')

    lines.append("};")
    out_pm.write_text("\n".join(lines) + "\n")


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: generate_xpm.py <input.png> <output.pm> [size] [colors]", file=sys.stderr)
        return 1
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 48
    colors = int(sys.argv[4]) if len(sys.argv) > 4 else 64
    generate_xpm(src, dst, size=size, colors=colors)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
