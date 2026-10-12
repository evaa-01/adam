# Usage: python scripts\add_overlay.py <name> <rom_hex> <vram_hex> <size_hex>
# Adds a code segment to splat\adam.yaml and a matching section to splat_to_toml.py.
# Run scripts\rebuild.ps1 afterwards.
import os
import re, sys, shutil

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
YAML = BASE + r"\splat\adam.yaml"
CONV = BASE + r"\scripts\splat_to_toml.py"

name = sys.argv[1]
rom, vram, size = (int(x, 16) for x in sys.argv[2:5])
end = rom + size

y = open(YAML).read()
if f"name: {name}\n" in y:
    sys.exit(f"{name} is already in adam.yaml, nothing changed")

segs = [(m.start(), m.group(1), m.group(2), int(m.group(3), 16)) for m in
        re.finditer(r"(?m)^  - name: (\S+)\n    type: (\S+)\n    start: (0x[0-9A-Fa-f]+)\n", y)]
tail = re.search(r"(?m)^  - \[(0x[0-9A-Fa-f]+)\]\s*$", y)
nxt = [s for s in segs if s[3] > rom]
pos = nxt[0][0] if nxt else tail.start()
limit = nxt[0][3] if nxt else int(tail.group(1), 16)
prev = [s for s in segs if s[3] <= rom][-1]
if prev[2] != "bin":
    sys.exit(f"ROM 0x{rom:X} falls inside '{prev[1]}' ({prev[2]}), not a bin segment; refusing")
if end > limit:
    sys.exit(f"overlay end 0x{end:X} runs past the next segment at 0x{limit:X}; refusing")

block = (f"  - name: {name}\n    type: code\n    start: 0x{rom:X}\n    vram: 0x{vram:08X}\n"
         f"    subsegments:\n      - [0x{rom:X}, asm]\n")
if end < limit:
    block += f"  - name: {name}_after\n    type: bin\n    start: 0x{end:X}\n"

c = open(CONV).read()
m = re.search(r"(?m)^SECTIONS = \[(.*)\]$", c)
if not m:
    sys.exit("could not find SECTIONS in splat_to_toml.py; nothing changed")

shutil.copyfile(YAML, YAML + ".bak")
shutil.copyfile(CONV, CONV + ".bak")
open(YAML, "w").write(y[:pos] + block + y[pos:])
entry = f'(".{name}", 0x{rom:X}, 0x{vram:08X}, 0x{size:X})'
open(CONV, "w").write(c[:m.end(1)] + ", " + entry + c[m.end(1):])
print(f"added {name}: ROM 0x{rom:X}-0x{end:X} -> vram 0x{vram:08X} (size 0x{size:X})")
print("backups: adam.yaml.bak, splat_to_toml.py.bak")