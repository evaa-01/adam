# Usage: python scripts\llm_test.py [base_url] [max_tokens]   (defaults http://127.0.0.1:8080, 8000)
# Gives a local LLM five real decisions from this project and scores its answers.
# Two cases are traps where the obvious fix is wrong. Nothing in the project is changed.
import json, re, sys, time, urllib.request

BASE = (sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080").rstrip("/")
MAX_TOKENS = int(sys.argv[2]) if len(sys.argv) > 2 else 8000

SYSTEM = """You assist with a static recompilation of an N64 game using N64Recomp.
Code sections: boot 0x80096400-0x800CA8B0, main 0x80025C00-0x80068C78, overlay 0x8038F800-0x803A9200.
OS/libultra code lives in 0x800B0000-0x800CB000.
You are given one problem. Reply with ONE JSON object and nothing else:
{"action": "<one of: override, stub, add_function, split_data, human>", "target": "<function name or hex address>", "value": "<hex size if override, else empty>", "reason": "<one sentence>"}
Meaning of actions:
- override: make a function larger so its own branches stay inside it.
- stub: replace a function with an empty one. Only safe for OS/hardware code, or code proven to never run.
- add_function: tell splat a new function starts at an address. Only correct if the previous code ends with jr $ra (plus delay slot).
- split_data: mark everything from an address onward as data, not code.
- human: you are not sure; a human should decide.
A wrong stub or add_function silently breaks the game. Choosing human when unsure is always acceptable."""

CASES = [
    {
        "name": "routine branch override",
        "prompt": """N64Recomp error:
Unhandled branch in func_8002DF18 at 0x8002DF2C to 0x8002E198
inspect_func.py output:
func_8002DF18: start 8002DF18, current size 0x114, real end 8002E310, new size 0x3F8
  overlaps func_8002E030 @ 8002E030: not called elsewhere""",
        "good": {"override"},
        "bad": {"stub", "add_function", "split_data"},
        "check_value": "0x3F8",
    },
    {
        "name": "cache instruction in OS code",
        "prompt": """N64Recomp error:
Unhandled instruction: cache
Error in recompiling func_800BD310
func_800BD310 is at 0x800BD310, size 0xB0. Disassembly start:
  blez a1, ... ; li t3,0x2000 ; sltu at,a1,t3 ; ... cache 0x11,0(t0) ... jr ra
It has 7 callers in OS-range functions.""",
        "good": {"stub"},
        "bad": {"add_function", "split_data", "override"},
    },
    {
        "name": "TRAP: jal into middle of a function",
        "prompt": """N64Recomp error:
No function found for jal target: 0x8002ADA0
Error in recompiling func_8039CA84
Disassembly of main section around 0x8002ADA0:
  8002AD90  addiu  $a2, $a2, -0xB
  8002AD94  sll    $a2, $a2, 18
  8002AD98  jal    func_800A3624
  8002AD9C   sra   $a2, $a2, 16
  .L8002ADA0:
  8002ADA0  lhu    $a2, 0x90($s0)
  8002ADA4  lw     $a0, 0x5C($s0)
  8002ADA8  addiu  $a1, $zero, 0x1BC
Should we add a function at 0x8002ADA0?""",
        "good": {"human", "stub"},
        "bad": {"add_function"},
    },
    {
        "name": "TRAP: vector instruction near end of main block",
        "prompt": """N64Recomp error:
Unhandled instruction: lwc2
Error in recompiling func_80068F44
func_80068F44 is at 0x80068F44, size 0x18, in the main section (0x80025C00-0x8006A600 before trimming).
who_calls.py for 0x80064000-0x8006A600:
func_80064144 size 0x5B8: jal callers none, pointer refs none
func_80064700 size 0x2954: jal callers none, pointer refs ['800671B8']
func_80068C78 size 0xC: jal callers none, pointer refs none
func_80068F44 size 0x18: jal callers none, pointer refs none
func_80069ED8 size 0x18: jal callers none, pointer refs none
func_80069EF0 size 0x18: jal callers none, pointer refs none
Known data nearby: jump table at 0x80069AD0, variables at 0x800671E0 and 0x8006A600.""",
        "good": {"split_data", "human"},
        "bad": {"stub", "add_function", "override"},
        "check_target": "80068C78",
    },
    {
        "name": "dead code in overlay",
        "prompt": """N64Recomp error:
No function found for jal target: 0x8002ADA0
Error in recompiling func_8039CA84
Facts:
- 0x8002ADA0 is in the middle of a main-section function (previous instruction is a jal, code uses $s0 set earlier).
- RAM during gameplay at 0x8002ADA0 matches the main section in ROM, so main is loaded.
- who_calls.py for the overlay tail:
  func_8039C93C: jal callers none, pointer refs none
  func_8039CA84: jal callers ['func_8039C93C']
  func_8039CBB4: jal callers ['func_8039C93C']
  func_8039D280: jal callers ['func_8039C93C']
- func_8039CBB4, func_8039D280, func_8039DF80 also jal into the middle of main-section functions.""",
        "good": {"stub", "human"},
        "bad": {"add_function", "split_data", "override"},
    },
]


def post(path, body, timeout=600):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def get_model():
    try:
        with urllib.request.urlopen(BASE + "/v1/models", timeout=10) as r:
            data = json.load(r).get("data", [])
            return data[0]["id"] if data else "local-model"
    except Exception as e:
        sys.exit(f"Cannot reach {BASE}/v1/models ({e}). Is the server running, and is the port right?")


def ask(model, prompt):
    t = time.time()
    r = post("/v1/chat/completions", {
        "model": model, "temperature": 0.1, "max_tokens": MAX_TOKENS,
        "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}]})
    ch = r["choices"][0]
    msg = ch.get("message", {})
    text = msg.get("content") or ""
    reasoning = msg.get("reasoning_content") or msg.get("reasoning") or ""
    used = (r.get("usage") or {}).get("completion_tokens", "?")
    return text, time.time() - t, ch.get("finish_reason", "?"), reasoning, used


model = get_model()
print(f"model: {model}  @ {BASE}\n")
score, results = 0, []
for c in CASES:
    try:
        raw, secs, finish, reasoning, used = ask(model, c["prompt"])
    except Exception as e:
        print(f"[ERROR] {c['name']}: {e}\n")
        results.append((c["name"], "error"))
        continue
    clean = re.sub(r"<think>.*?</think>", "", raw, flags=re.S)
    m = re.search(r"\{.*\}", clean, flags=re.S)
    try:
        ans = json.loads(m.group(0)) if m else None
    except json.JSONDecodeError:
        ans = None
    if not ans:
        verdict = "FAIL (no valid JSON)"
    else:
        act = str(ans.get("action", "")).lower()
        if act in c["bad"]:
            verdict = "FAIL (harmful choice)"
        elif act in c["good"]:
            verdict = "PASS"
            if "check_value" in c and c["check_value"].lower() not in str(ans.get("value", "")).lower():
                verdict = "PARTIAL (right action, wrong size)"
            if "check_target" in c and act == "split_data" and c["check_target"].lower() not in str(ans.get("target", "")).lower():
                verdict = "PARTIAL (right action, wrong address)"
        else:
            verdict = f"FAIL (unknown action '{act}')"
    if verdict == "PASS":
        score += 1
    results.append((c["name"], verdict))
    print(f"[{verdict}] {c['name']}  ({secs:.0f}s, {used} tokens, finish={finish})")
    if not ans:
        if finish == "length":
            print(f"   hit the token limit while still thinking; rerun with a higher limit, e.g. 16000")
        tail = (reasoning or raw).strip()[-400:]
        if tail:
            print(f"   end of its output: ...{tail}")
    print(f"   answer: {json.dumps(ans) if ans else clean.strip()[:300]}\n")

print(f"score: {score}/{len(CASES)}")
for n, v in results:
    print(f"  {v:38} {n}")
