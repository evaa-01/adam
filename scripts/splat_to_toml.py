# Converts splat/spimdisasm output into an N64Recomp symbols file.
# Reads: splat\asm\*.s, scripts\size_overrides.txt   Writes: symbols.splat.toml
import re, glob, os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
ASM = BASE + r"\splat\asm"
OUT = BASE + r"\symbols.splat.toml"
OV = BASE + r"\scripts\size_overrides.txt"
# (name, rom, vram, size, function-name prefix). ovt0-ovt6 come from the game's overlay table at
# 0x800D4F50 and all share VRAM 0x80025C00; ovt2 is ".text2" and keeps unprefixed names.
SECTIONS = [(".text", 0x1000, 0x80096400, 0x68EE0, ""),
            (".ovt0", 0x069EE0, 0x80025C00, 0x00ACD0, "ovt0_"),
            (".ovt1", 0x074BB0, 0x80025C00, 0x05A560, "ovt1_"),
            (".text2", 0x0CF110, 0x80025C00, 0x044A00, ""),
            (".ovt3", 0x113B10, 0x80025C00, 0x003EF0, "ovt3_"),
            (".ovt4", 0x117A00, 0x80025C00, 0x037F60, "ovt4_"),
            (".ovt5", 0x14F960, 0x80025C00, 0x025CE0, "ovt5_"),
            (".ovt6", 0x175640, 0x80025C00, 0x02C240, "ovt6_"),
            (".ovl1", 0x1A1880, 0x8038F800, 0x19A00, "")]

HEAD = re.compile(r"^\s*nonmatching\s+([\w.$]+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)")
INSN = re.compile(r"/\*\s*[0-9A-Fa-f]+\s+([0-9A-Fa-f]{8})\s+[0-9A-Fa-f]{8}\s*\*/")

def section_of(path):
    # splat names each asm file after its ROM offset (e.g. 74BB0.s); sections sharing VRAM are told apart by it.
    try:
        rom = int(os.path.splitext(os.path.basename(path))[0], 16)
    except ValueError:
        return None
    for sec in SECTIONS:
        if sec[1] <= rom < sec[1] + sec[3]:
            return sec
    return None

funcs = {}   # (section name, vram) -> (name, size)
for path in glob.glob(os.path.join(ASM, "**", "*.s"), recursive=True):
    sec = section_of(path)
    if sec is None:
        continue
    pending = None
    for line in open(path, errors="ignore"):
        m = HEAD.match(line)
        if m:
            name = m.group(1)
            if sec[4] and not name.startswith(sec[4]):
                name = sec[4] + name
            pending = (name, int(m.group(2), 0))
            continue
        if pending:
            m = INSN.search(line)
            if m:
                funcs[(sec[0], int(m.group(1), 16))] = pending
                pending = None

# Split overlay functions at real entry points splat missed. In the shared overlay slot splat often starts
# a function on the nop padding before the real start (and names it D_...), hiding the entry point. Rule:
# an address that is a jal target (from the same overlay, .text or .ovl1), lies inside a function, and
# follows "jr $ra; delay slot; [nop padding]" is a function start. A wrong split would show up as an
# "Unhandled branch" and get grown back by auto_loop (overlaps are allowed).
ROMDATA = open(BASE + r"\rom\game.z64", "rb").read()
TEXT_END = {".ovt0": 0x8002EAA0, ".ovt1": 0x80069B70, ".text2": 0x80067060, ".ovt3": 0x800294B0,
            ".ovt4": 0x800518F0, ".ovt5": 0x8003E4D0, ".ovt6": 0x80040DA0}   # from the overlay table

def rom_word(sec, va):
    o = sec[1] + (va - sec[2])
    return int.from_bytes(ROMDATA[o:o + 4], "big")

def jal_targets(sec):
    out = set()
    for (sn, v), (n, s) in funcs.items():
        if sn != sec[0]:
            continue
        for k in range(s // 4):
            w = rom_word(sec, v + 4 * k)
            if w >> 26 == 3:
                out.add(0x80000000 | ((w & 0x3FFFFFF) << 2))
    return out

secs = {s[0]: s for s in SECTIONS}
shared = jal_targets(secs[".text"]) | jal_targets(secs[".ovl1"])
for sname, text_end in TEXT_END.items():
    sec = secs[sname]
    starts = sorted(v for (sn, v) in funcs if sn == sname)
    for t in sorted(shared | jal_targets(sec)):
        if not (sec[2] <= t < text_end) or (sname, t) in funcs:
            continue
        owner = max((v for v in starts if v < t), default=None)
        if owner is None or t >= owner + funcs[(sname, owner)][1]:
            continue
        # Walk back over nop padding (may cross into the previous function). Then either we sit on the
        # jr $ra (its delay slot was a nop too) or on a non-nop delay slot right after the jr $ra.
        a = t - 4
        while a > sec[2] and rom_word(sec, a) == 0:
            a -= 4
        if rom_word(sec, a) != 0x03E00008 and not (a - 4 >= sec[2] and rom_word(sec, a - 4) == 0x03E00008):
            continue
        oname, osize = funcs[(sname, owner)]
        funcs[(sname, owner)] = (oname, t - owner)
        funcs[(sname, t)] = (f"{sec[4]}func_{t:08X}", owner + osize - t)
        starts.append(t); starts.sort()
        print(f"split: {oname} at {t:08X} -> {sec[4]}func_{t:08X}")

if os.path.exists(OV):
    for line in open(OV):
        parts = line.split("#")[0].split()
        if len(parts) != 2:
            continue
        hit = False
        for v, (n, s) in list(funcs.items()):
            if n == parts[0]:
                funcs[v] = (n, int(parts[1], 0))
                hit = True
                print(f"override: {n} size 0x{s:X} -> {parts[1]}")
        if not hit:
            print(f"WARNING: override for unknown function {parts[0]}")

with open(OUT, "w") as fh:
    for name, rom, vram, size, prefix in SECTIONS:
        fs = sorted((v, n, s) for (sn, v), (n, s) in funcs.items() if sn == name and vram <= v < vram + size)
        bad = [f for f in fs if f[2] < 4 or f[2] % 4]
        fh.write(f'[[section]]\nname = "{name}"\nrom = 0x{rom:08X}\nvram = 0x{vram:08X}\nsize = 0x{size:X}\nfunctions = [\n')
        for v, n, s in fs:
            fh.write(f'    {{ name = "{n}", vram = 0x{v:08X}, size = 0x{s:X} }},\n')
        fh.write("]\n\n")
        print(f"{name}: {len(fs)} functions, {len(bad)} with odd sizes")
