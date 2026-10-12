# Usage: python scripts\disasm.py <vram_hex> [count] [section]   (default 32 instructions)
#        python scripts\disasm.py <function_name>
# Disassembles straight from the ROM using the section layout in splat_to_toml.py,
# including addresses splat marked as data. Uses rabbitizer (installed with splat).
# The overlays .ovt0-.ovt6 (.text2 = ovt2) share VRAM 0x80025C00: pass the section (e.g. ovt4 or .ovt4)
# for an address there; without it .text2 is used. Function names already say their section.
import os
import ast, re, struct, sys
import rabbitizer

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
ROM = BASE + r"\rom\game.z64"
SYM = BASE + r"\symbols.toml"
CONV = BASE + r"\scripts\splat_to_toml.py"

rom = open(ROM, "rb").read()
m = re.search(r"(?ms)^SECTIONS = (\[.*?\])\s*$", open(CONV).read())
secs = [t[:4] for t in ast.literal_eval(m.group(1))]   # (name, rom, vram, size[, prefix])
funcs = {}   # name -> (section, vram, size)
for block in open(SYM).read().split("[[section]]")[1:]:
    sname = re.search(r'(?m)^name = "([^"]+)"', block).group(1)
    for n, a, sz in re.findall(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}', block):
        funcs[n] = (sname, int(a, 16), int(sz, 16))

arg = sys.argv[1]
want = None
if arg in funcs:
    want, addr, size = funcs[arg]
    count = size // 4
else:
    addr = int(arg, 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    if len(sys.argv) > 3:
        want = sys.argv[3] if sys.argv[3].startswith(".") else "." + sys.argv[3]

hits = [x for x in secs if x[2] <= addr < x[2] + x[3]]
if want:
    hits = [x for x in hits if x[0] == want]
elif len(hits) > 1:
    hits = [x for x in hits if x[0] == ".text2"]
    print(f"(0x{addr:08X} is in the shared overlay slot; showing .text2 - pass a section like ovt4 for another)")
if not hits:
    sys.exit(f"0x{addr:08X} is not inside {want or 'any section'}: {[(n, hex(v)) for n, r, v, sz in secs]}")
sec = hits[0]
starts = {a: n for n, (sn, a, sz) in funcs.items() if sn == sec[0]}
name, srom, svram, ssize = sec
for i in range(count):
    a = addr + i * 4
    if a >= svram + ssize:
        break
    o = srom + (a - svram)
    w = struct.unpack(">I", rom[o:o + 4])[0]
    if a in starts:
        print(f"\n{starts[a]}:")
    text = rabbitizer.Instruction(w, vram=a).disassemble()
    print(f"  {a:08X} {w:08X}  {text}")