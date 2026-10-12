# Runs the recompiler repeatedly and applies the two known fixes:
#   Unhandled branch       -> size override in scripts\size_overrides.txt
#   Unhandled instruction  -> stub in scripts\extra_stubs.txt (OS range only)
# Stops on anything else. Log: scripts\auto_loop.log
import os
import re, struct, subprocess, sys, datetime, shutil

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)
ROM = BASE + r"\rom\game.z64"
SYM = BASE + r"\symbols.toml"
OVR = BASE + r"\scripts\size_overrides.txt"
STUBS = BASE + r"\scripts\extra_stubs.txt"
LOG = BASE + r"\scripts\auto_loop.log"
OS_LO, OS_HI = 0x800B0000, 0x800CB000
MAX_GROW = 0x4000
MAX_ROUNDS = 300

rom = open(ROM, "rb").read()

def log(msg):
    print(msg, flush=True)
    with open(LOG, "a") as f:
        f.write(f"{datetime.datetime.now():%Y-%m-%d %H:%M:%S} {msg}\n")

def run(args):
    return subprocess.run(args, cwd=BASE, capture_output=True, text=True)

def rebuild():
    r = run([sys.executable, r"scripts\splat_to_toml.py"])
    if r.returncode != 0:
        return 99, r.stdout + r.stderr
    shutil.copyfile(BASE + r"\symbols.splat.toml", SYM)
    r = run([sys.executable, r"scripts\update_stubs.py"])
    if r.returncode != 0:
        return 99, r.stdout + r.stderr
    r = run([BASE + r"\N64Recomp.exe", "config.toml"])
    return r.returncode, r.stdout + r.stderr

def load_funcs():
    out = []
    for p in re.split(r"(?m)^\[\[section\]\]\s*$", open(SYM).read())[1:]:
        r = int(re.search(r"rom = (0x[0-9A-Fa-f]+)", p).group(1), 16)
        v = int(re.search(r"vram = (0x[0-9A-Fa-f]+)", p).group(1), 16)
        sz = int(re.search(r"(?m)^size = (0x[0-9A-Fa-f]+)", p).group(1), 16)
        for n, a, b in re.findall(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}', p):
            out.append((n, int(a, 16), int(b, 16), r, v, sz))
    return out

def btarget(pc, w):
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if not (op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and rt in (0, 1, 2, 3)) or (op == 17 and rs == 8)):
        return None
    imm = w & 0xFFFF
    return pc + 4 + ((imm - 0x10000) if imm & 0x8000 else imm) * 4

def real_size(name):
    funcs = load_funcs()
    starts = {f[1] for f in funcs}
    for n, a, s, r, v, sz in funcs:
        if n != name:
            continue
        top, need, p = v + sz, a + s, a
        while p < top:
            w = struct.unpack(">I", rom[r + (p - v):r + (p - v) + 4])[0]
            t = btarget(p, w)
            if t is not None and p < t < top:
                need = max(need, t + 4)
            end_here = (w == 0x03E00008)
            if w >> 26 == 2:            # j: tail call only if it lands on a function start
                jt = (p & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if jt in starts and jt != a:
                    end_here = True
                elif p < jt < top:
                    need = max(need, jt + 4)
            if p + 8 >= need and end_here:
                return a, s, p + 8 - a
            p += 4
        return a, s, None
    return None, None, None

log("---- auto_loop start ----")
seen = set()
for rnd in range(1, MAX_ROUNDS + 1):
    code, out = rebuild()
    if code == 99:
        log(f"round {rnd}: a helper script failed, stopping")
        print(out); break
    count = re.search(r"Function count: (\d+)", out)
    if code == 0:
        log(f"round {rnd}: SUCCESS ({count.group(1) if count else '?'} functions)")
        break
    err = re.search(r"Error in recompiling (\S+)", out)
    fn = err.group(1).rstrip(",") if err else None
    lines = [l for l in out.strip().splitlines() if l.strip()]
    key = (fn, lines[-3] if len(lines) >= 3 else out)
    if key in seen:
        log(f"round {rnd}: same error repeated for {fn}, stopping")
        print(out); break
    seen.add(key)

    m = re.search(r"Unhandled branch in (\S+) at", out)
    if m:
        name = m.group(1)
        a, old, new = real_size(name)
        if a is None:
            log(f"round {rnd}: {name} not found in symbols, stopping")
            print(out); break
        if new is None or new <= old or new - old > MAX_GROW:
            log(f"round {rnd}: {name} needs a manual look (no end found, no growth, or growth too large)")
            print(out); break
        kept = [l for l in open(OVR) if l.split("#")[0].split()[:1] != [name]]
        orig = old
        for l in open(OVR):
            m2 = re.search(r"was (0x[0-9A-Fa-f]+)", l)
            if l.split("#")[0].split()[:1] == [name] and m2:
                orig = int(m2.group(1), 16)
        with open(OVR, "w") as f:
            f.writelines(kept)
            f.write(f"{name} 0x{new:X}  # auto, was 0x{orig:X}\n")
        log(f"round {rnd}: override {name} 0x{old:X} -> 0x{new:X}")
        continue

    if fn and ("Unhandled instruction" in out or "Unhandled cop0" in out):
        addr = next((a for n, a, *_ in load_funcs() if n == fn), None)
        if addr is not None and OS_LO <= addr < OS_HI:
            with open(STUBS, "a") as f:
                f.write(f"{fn}  # auto: unhandled instruction in OS range\n")
            log(f"round {rnd}: stub {fn} ({addr:08X})")
            continue
        log(f"round {rnd}: unhandled instruction in {fn} outside OS range, stopping")
        print(out); break

    log(f"round {rnd}: unknown error type, stopping")
    print(out); break
else:
    log("hit MAX_ROUNDS, stopping")