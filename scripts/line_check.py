# Counts thin horizontal glitch lines in a window screenshot, without anyone having to look at it. The game draws
# at 320x240 and is scaled up (2x in a 640x480 window, 4x in 1280x960), so every real detail of the picture is at
# least 2 screen pixels tall. A single pixel row that is clearly darker (or brighter) than both rows next to it can't
# come from the game's picture: it's a gap or seam between drawn strips.
# Usage: python scripts\line_check.py <screenshot.png> [more.png ...]
# Prints per image: rows where more than 2% of the pixels form such a line, grouped into bands, plus a total.
import sys
import numpy as np
from PIL import Image

TITLE_BAR = 80      # skip the window's title bar
THRESHOLD = 24      # brightness step (0-255) that counts as "clearly" darker/brighter
MIN_FRACTION = 0.02 # share of a row's pixels that must be part of a line

def line_rows(path):
    lum = np.asarray(Image.open(path).convert("L"), dtype=np.int16)[TITLE_BAR:]
    above, row, below = lum[:-2], lum[1:-1], lum[2:]
    dark = np.minimum(above - row, below - row) > THRESHOLD
    bright = np.minimum(row - above, row - below) > THRESHOLD
    fraction = (dark | bright).mean(axis=1)
    return [(y + 1 + TITLE_BAR, f) for y, f in enumerate(fraction) if f > MIN_FRACTION]

for path in sys.argv[1:]:
    rows = line_rows(path)
    bands, start, prev = [], None, None
    for y, _ in rows:
        if start is None or y > prev + 8:
            if start is not None:
                bands.append((start, prev))
            start = y
        prev = y
    if start is not None:
        bands.append((start, prev))
    worst = max((f for _, f in rows), default=0.0)
    print(f"{path}: {len(rows)} line rows, worst row {worst:.0%} of its width; bands (y ranges): "
          + (", ".join(f"{a}-{b}" for a, b in bands) if bands else "none"))
