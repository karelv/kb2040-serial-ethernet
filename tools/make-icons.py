#!/usr/bin/env python3
"""Render the app icons in web/ from the favicon design (board outline, LED, data lines).

    python3 tools/make-icons.py

icon-192.png / icon-512.png   "any" purpose: rounded tile on transparent corners
icon-maskable-512.png         "maskable": full-bleed background, artwork inside the 80% safe zone
apple-touch-icon.png (180)    opaque square: iOS applies its own rounded mask
"""
from pathlib import Path

from PIL import Image, ImageDraw

BG = (13, 17, 23)        # --bg
PANEL = (28, 35, 48)     # --panel2
ACCENT = (63, 185, 160)  # --accent
BLUE = (88, 166, 255)    # --accent2
LED = (0, 255, 128)      # --led
OUT = Path(__file__).resolve().parent.parent / "web"
SS = 4  # supersampling factor for smooth edges


def artwork(d: ImageDraw.ImageDraw, size: int, scale: float) -> None:
    """The 32x32 favicon drawing, scaled to `scale` of `size` and centred."""
    u = size * scale / 32
    o = (size - 32 * u) / 2
    p = lambda x, y: (o + x * u, o + y * u)
    d.rounded_rectangle([p(3, 6), p(29, 26)], radius=4 * u, fill=PANEL, outline=ACCENT, width=round(2 * u))
    d.ellipse([p(6, 12), p(14, 20)], fill=LED)
    for y, x2 in ((12, 25), (16, 25), (20, 22)):
        d.line([p(17, y), p(x2, y)], fill=BLUE, width=round(2 * u))
        for x in (17, x2):  # round caps
            cx, cy = p(x, y)
            d.ellipse([cx - u, cy - u, cx + u, cy + u], fill=BLUE)


def render(size: int, *, tile: bool, scale: float) -> Image.Image:
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0) if tile else BG + (255,))
    d = ImageDraw.Draw(img)
    if tile:
        d.rounded_rectangle([0, 0, s - 1, s - 1], radius=s * 0.22, fill=BG)
    artwork(d, s, scale)
    return img.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    render(192, tile=True, scale=0.92).save(OUT / "icon-192.png", optimize=True)
    render(512, tile=True, scale=0.92).save(OUT / "icon-512.png", optimize=True)
    render(512, tile=False, scale=0.72).save(OUT / "icon-maskable-512.png", optimize=True)
    render(180, tile=False, scale=0.86).convert("RGB").save(OUT / "apple-touch-icon.png", optimize=True)
    for f in ("icon-192.png", "icon-512.png", "icon-maskable-512.png", "apple-touch-icon.png"):
        print(f, (OUT / f).stat().st_size, "bytes")


if __name__ == "__main__":
    main()
