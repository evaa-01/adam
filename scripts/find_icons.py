# Identifies the controller/keyboard symbols in a screenshot of RecompFrontend's menus exactly, without a vision model:
# the menus draw them with promptfont (runtime\assets\promptfont\promptfont.ttf), so each white symbol found in the given
# band is compared with every promptfont glyph drawn at the same size (the glyph names come from RecompFrontend's
# promptfont.h, e.g. ANALOG_L_CLICK, XBOX_RIGHT_TRIGGER).
# Usage: python scripts\find_icons.py <image.png> <y0> <y1> [x0] [x1]
#   e.g. the Controls tab row "Rewind" (y from scripts\ocr.ps1: the label's top - 10 to its bottom + 10).
# Prints for each symbol (left to right): x range, the best 3 glyphs with their scores (1.0 = identical).
# Text (the row label) is matched too and comes out as some glyph with a low score: look at the binding's x range.
import re, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "runtime" / "assets" / "promptfont" / "promptfont.ttf"
HEADER = Path(r"C:\dev\recomp\RecompFrontend\recompinput\include\recompinput\promptfont.h")
SIZE = 32  # comparison size (square)

def glyphs():
    names = {}
    for name, code in re.findall(r"#define PF_(\w+)_INT 0x([0-9A-Fa-f]+)", HEADER.read_text(encoding="utf-8")):
        names.setdefault(int(code, 16), name)  # first name wins for codes with aliases
    font = ImageFont.truetype(str(FONT), 96)
    out = []
    for code, name in names.items():
        im = Image.new("L", (160, 160), 0)
        ImageDraw.Draw(im).text((20, 10), chr(code), fill=255, font=font)
        a = np.array(im)
        if a.max() < 128:
            continue  # not in the font
        out.append((name, normalize(a)))
    return out

def normalize(a):
    # Crop to the ink, pad to a square (keeps the aspect ratio), scale to SIZE, zero mean / unit norm.
    ys, xs = np.where(a > 100)
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1].astype(np.float32)
    h, w = a.shape
    n = max(h, w)
    sq = np.zeros((n, n), np.float32)
    sq[(n - h) // 2:(n - h) // 2 + h, (n - w) // 2:(n - w) // 2 + w] = a
    v = np.array(Image.fromarray(sq).resize((SIZE, SIZE), Image.BILINEAR), np.float32)
    v -= v.mean()
    return v / (np.linalg.norm(v) + 1e-6)

def symbols(band, gap=8):
    # Left-to-right groups of bright columns (gaps > <gap> px split them).
    cols = np.where((band > 180).sum(0) > 0)[0]
    runs, start, prev = [], None, None
    for c in cols:
        if start is None:
            start = prev = c
        elif c - prev > gap:
            runs.append((start, prev)); start = prev = c
        else:
            prev = c
    if start is not None:
        runs.append((start, prev))
    return runs

def main():
    if len(sys.argv) < 4:
        sys.exit("usage: find_icons.py <image.png> <y0> <y1> [x0] [x1]")
    img = np.array(Image.open(sys.argv[1]).convert("L"))
    y0, y1 = int(sys.argv[2]), int(sys.argv[3])
    x0 = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    x1 = int(sys.argv[5]) if len(sys.argv) > 5 else img.shape[1]
    band = img[y0:y1, x0:x1]
    table = glyphs()
    for a, b in symbols(band):
        piece = band[:, a:b + 1]
        if (piece > 100).sum() < 10:
            continue
        v = normalize(piece)
        scores = sorted(((float((v * g).sum()), name) for name, g in table), reverse=True)[:3]
        print(f"x {x0 + a}-{x0 + b}: " + ", ".join(f"{name} {s:.2f}" for s, name in scores))

if __name__ == "__main__":
    main()
