# Checks the project setup before running auto_loop.py. Changes nothing.
import os, re, glob

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
ok = True
def bad(msg):
    global ok
    ok = False
    print("PROBLEM:", msg)

need = [r"rom\game.z64", "N64Recomp.exe", r"splat\adam.yaml",
        r"scripts\splat_to_toml.py", r"scripts\update_stubs.py", r"scripts\auto_loop.py",
        r"scripts\inspect_func.py", r"scripts\rebuild.ps1",
        r"scripts\extra_stubs.txt", r"scripts\size_overrides.txt"]
for f in need:
    if not os.path.exists(os.path.join(BASE, f)):
        bad(f"missing {f}")

rom = os.path.join(BASE, r"rom\game.z64")
if os.path.exists(rom):
    head = open(rom, "rb").read(16)
    if head[:4] != b"\x80\x37\x12\x40":
        bad("ROM is not big-endian .z64")
    elif head[8:12] != b"\x80\x09\x64\x00":
        bad("ROM entry point is not 0x80096400 (wrong ROM?)")

y = os.path.join(BASE, r"splat\adam.yaml")
if os.path.exists(y):
    t = open(y).read()
    for s in ("[0x1000, asm]", "[0x354B0, bin]", "[0xCF110, asm]", "[0x112188, bin]",
              "[0x1A1880, asm]", "vram: 0x80096400", "vram: 0x80025C00", "vram: 0x8038F800"):
        if s not in t:
            bad(f"adam.yaml is missing {s}")

asm = glob.glob(os.path.join(BASE, r"splat\asm\*.s"))
if len(asm) < 2:
    bad("splat\\asm has no output, run scripts\\rebuild.ps1 once")

sym = os.path.join(BASE, "symbols.toml")
names = set()
if os.path.exists(sym):
    t = open(sym).read()
    names = set(re.findall(r'name = "(func_[0-9A-F]+)"', t))
    print(f"symbols.toml: {t.count('[[section]]')} sections, {len(names)} functions")
    if t.count("[[section]]") != 3:
        bad("symbols.toml should have exactly 3 sections (boot, main, ovl1)")
    if "FUN_" in t:
        bad("symbols.toml still has Ghidra names (FUN_), run rebuild.ps1")

for f, label in ((r"scripts\extra_stubs.txt", "stub"), (r"scripts\size_overrides.txt", "override")):
    p = os.path.join(BASE, f)
    if not os.path.exists(p):
        continue
    for i, line in enumerate(open(p), 1):
        parts = line.split("#")[0].split()
        if not parts:
            continue
        if label == "override" and (len(parts) != 2 or not re.fullmatch(r"0x[0-9A-Fa-f]+", parts[1])):
            bad(f"{f} line {i} is malformed: {line.strip()}")
        if names and parts[0] not in names:
            bad(f"{f} line {i}: {parts[0]} is not in symbols.toml")

cfg = os.path.join(BASE, "config.toml")
if os.path.exists(cfg):
    t = open(cfg).read()
    for s in ("entrypoint = 0x80096400", 'symbols_file_path = "symbols.toml"', "[patches]"):
        if s not in t:
            bad(f"config.toml is missing {s}")

print("ALL OK" if ok else "fix the problems above before running auto_loop.py")
