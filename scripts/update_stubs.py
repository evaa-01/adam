# Rebuilds config.toml: stubs every function with cache/COP0 instructions,
# plus the names listed in scripts\extra_stubs.txt, and renames (to <name>_recomp) the names in
# scripts\extra_renames.txt (game functions whose names clash with the C library, e.g. fabs).
import re, struct, os, json

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
ROM = BASE + r"\rom\game.z64"
SYM = BASE + r"\symbols.toml"
CFG = BASE + r"\config.toml"
EXTRA = BASE + r"\scripts\extra_stubs.txt"
RENAMES = BASE + r"\scripts\extra_renames.txt"
HOOKS = BASE + r"\scripts\extra_hooks.txt"
NAMES = BASE + r"\scripts\n64recomp_names.json"

rom = open(ROM, "rb").read()

# N64Recomp renames every function on its built-in lists (to <name>_recomp) before applying stubs.
# Reimplemented/ignored ones are never recompiled, so they need no stub; renamed ones are stubbed
# under their new name.
lists = json.load(open(NAMES)) if os.path.exists(NAMES) else {"reimplemented": [], "ignored": [], "renamed": []}
skip = set(lists["reimplemented"]) | set(lists["ignored"])
renamed = set(lists["renamed"])
handled = []
text = open(SYM).read()

def risky(w):
    op = w >> 26
    if op == 0x2F: return "cache"
    if op == 0x10:
        rs = (w >> 21) & 31
        if rs == 0: return "mfc0"
        if rs == 4: return "mtc0"
        if rs >= 16: return "eret/tlb"
    return None

names, stubs = set(), []
for p in re.split(r"(?m)^\[\[section\]\]\s*$", text)[1:]:
    rom_off = int(re.search(r"rom = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    vram = int(re.search(r"vram = (0x[0-9A-Fa-f]+)", p).group(1), 16)
    for m in re.finditer(r'\{ name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}', p):
        name, v, s = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        names.add(name)
        o = rom_off + (v - vram)
        ops = {risky(w) for w in struct.unpack(">%dI" % (s // 4), rom[o:o + s])} - {None}
        if ops:
            if name in skip:
                handled.append(name)
            elif name in renamed:
                stubs.append((name + "_recomp", s, sorted(ops)))
            else:
                stubs.append((name, s, sorted(ops)))

have = {n for n, _, _ in stubs}
if os.path.exists(EXTRA):
    for line in open(EXTRA):
        n = line.split("#")[0].strip()
        if not n or n in have:
            continue
        if n not in names:
            print(f"WARNING: extra stub {n} is not in symbols.toml, skipped")
            continue
        stubs.append((n, 0, ["manual"]))
        have.add(n)

renames = []
if os.path.exists(RENAMES):
    for line in open(RENAMES):
        n = line.split("#")[0].strip()
        if not n:
            continue
        if n not in names:
            print(f"WARNING: rename {n} is not in symbols.toml, skipped")
            continue
        if n in have:
            print(f"WARNING: rename {n} is also stubbed; not supported, skipped")
            continue
        renames.append(n)

# Hooks: "<func> <vram> <C code>" -> C code inserted before the instruction at <vram>.
hooks = []
if os.path.exists(HOOKS):
    for line in open(HOOKS):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        func, vram, text = line.strip().split(None, 2)
        if func not in names:
            print(f"WARNING: hook function {func} is not in symbols.toml, skipped")
            continue
        hooks.append((func, int(vram, 16), text))

print("functions to stub:", len(stubs))
if handled:
    print("  (not stubbed, N64Recomp already skips them:", ", ".join(handled) + ")")
for name, s, ops in stubs:
    print(f"  {name} size=0x{s:X}: {', '.join(ops)}")

with open(CFG, "w") as fh:
    fh.write('[input]\nentrypoint = 0x80096400\nrom_file_path = "rom/game.z64"\n'
             'symbols_file_path = "symbols.toml"\noutput_func_path = "RecompiledFuncs"\n'
             'single_file_output = false\nfunctions_per_output_file = 50\n\n'
             '[patches]\nstubs = [\n')
    for name, s, ops in stubs:
        fh.write(f'    "{name}",  # {", ".join(ops)}\n')
    fh.write("]\n")
    if renames:
        fh.write("renamed = [\n")
        for n in renames:
            fh.write(f'    "{n}",\n')
        fh.write("]\n")
    for func, vram, text in hooks:
        fh.write(f'\n[[patches.hook]]\nfunc = "{func}"\nbefore_vram = 0x{vram:08X}\ntext = {json.dumps(text)}\n')
print("functions renamed:", ", ".join(renames) if renames else "none")
