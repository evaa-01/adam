# Usage: python scripts\split_data.py <section> <vram_hex>
# Marks everything in that code segment from <vram> onward as data (a bin subsegment
# in splat\adam.yaml). Run scripts\rebuild.ps1 afterwards.
import os
import re, sys, shutil

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
YAML = BASE + r"\splat\adam.yaml"
CONV = BASE + r"\scripts\splat_to_toml.py"
sec, vram = sys.argv[1], int(sys.argv[2], 16)

m = re.search(r"(?m)^SECTIONS = \[(.*)\]$", open(CONV).read())
secs = {n: (int(r, 16), int(v, 16), int(s, 16)) for n, r, v, s in
        re.findall(r'\("([^"]+)", (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)', m.group(1))}
if sec not in secs:
    sys.exit(f"{sec} not in SECTIONS ({list(secs)})")
srom, svram, ssize = secs[sec]
if not (svram < vram < svram + ssize) or vram % 4:
    sys.exit(f"0x{vram:08X} is not inside {sec} (0x{svram:08X}-0x{svram + ssize:08X})")
rom = srom + (vram - svram)

y = open(YAML).read()
seg = re.search(rf"(?m)^  - name: \S+\n    type: code\n    start: 0x{srom:X}\n    vram: [^\n]+\n    subsegments:\n((?:      - \[[^\n]+\n)+)", y)
if not seg:
    sys.exit(f"no code segment starting at ROM 0x{srom:X} in adam.yaml")
subs = re.findall(r"      - \[(0x[0-9A-Fa-f]+), (\w+)\]", seg.group(1))
if any(int(o, 16) == rom for o, _ in subs):
    sys.exit(f"a subsegment already starts at ROM 0x{rom:X}; nothing changed")
subs.append((f"0x{rom:X}", "bin"))
subs.sort(key=lambda t: int(t[0], 16))
new = "".join(f"      - [{o}, {k}]\n" for o, k in subs)
shutil.copyfile(YAML, YAML + ".bak")
open(YAML, "w").write(y[:seg.start(1)] + new + y[seg.end(1):])
print(f"{sec}: data from vram 0x{vram:08X} (ROM 0x{rom:X}) onward; backup adam.yaml.bak")