# Scores local LM Studio vision models on screenshots with known answers (same questions for every model).
# Usage: python scripts\vision_bench.py <bench dir> <model> [model...]
# <bench dir> holds the screenshots (scratchpad, never the repo: they show the game) named as in CASES below;
# derived images (crops, grid overlays, edited copies) are written to <bench dir>\derived\.
# The answers came from how each shot was taken and from scripts\ocr.ps1 (Windows OCR, exact text + positions):
#   menu_general/graphics/controls/sound = settings tab clicked (OCR shows that tab's rows),
#   menu_controls_scrolled = Controls tab scrolled down (Fast-Forward = RT, Rewind = left stick click by default),
#   battle_1/2 = mission 02 battle (OCR reads the HUD labels "EVA01", "ANGEL"), black = menu fade (mean 3 of 255).
# Tab label centers (OCR): General 162, Graphics 353, Controls 548, Sound 720 (x, screenshot pixels).
# Each line: case, model answer, PASS/FAIL, seconds. Ends with the score per model and per idea.
import base64, json, re, sys, time, urllib.request
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

URL = "http://127.0.0.1:1234/v1/chat/completions"

def ask(model, image_path, question, timeout=180):
    image = base64.b64encode(Path(image_path).read_bytes()).decode("ascii")
    body = {"model": model, "temperature": 0, "max_tokens": 300, "messages": [{"role": "user", "content": [
        {"type": "text", "text": question},
        {"type": "image_url", "image_url": {"url": "data:image/png;base64," + image}}]}]}
    req = urllib.request.Request(URL, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        text = json.load(resp)["choices"][0]["message"]["content"] or ""
    return re.sub(r"<think>.*?</think>", "", text, flags=re.S).strip()  # drop reasoning blocks of thinking models

# --- derived images (the "ideas") ---
def crop_zoom(src, box, out, scale=3):
    im = Image.open(src).crop(box)
    im.resize((im.width * scale, im.height * scale), Image.LANCZOS).save(out)

def grid(src, out, step=100):
    # Vertical lines every <step> px labelled with their x value: positions become "read the nearest label".
    im = Image.open(src).convert("RGB")
    d = ImageDraw.Draw(im)
    font = ImageFont.load_default(size=22)
    for x in range(0, im.width, step):
        d.line([(x, 0), (x, im.height)], fill=(255, 0, 255), width=1)
        d.text((x + 3, im.height - 30), str(x), fill=(255, 0, 255), font=font)
    for y in range(0, im.height, step):
        d.line([(0, y), (im.width, y)], fill=(0, 255, 255), width=1)
        d.text((3, y + 3), str(y), fill=(0, 255, 255), font=font)
    im.save(out)

def blackout(src, out):
    Image.new("RGB", Image.open(src).size, (2, 2, 2)).save(out)

def glitch_line(src, out, y=600):
    im = Image.open(src).convert("RGB")
    ImageDraw.Draw(im).line([(0, y), (im.width, y)], fill=(255, 255, 255), width=1)
    im.save(out)

def prepare(d):
    dv = d / "derived"
    dv.mkdir(exist_ok=True)
    crop_zoom(d / "menu_controls_scrolled.png", (90, 380, 1250, 480), dv / "rows_zoom.png")
    grid(d / "menu_general.png", dv / "menu_grid.png")
    blackout(d / "battle_1.png", dv / "black_synthetic.png")
    glitch_line(d / "battle_1.png", dv / "battle_glitch.png")

# --- the cases: (id, idea, image, question, check) ---
def num(text):
    m = re.search(r"-?\d+(\.\d+)?", text)
    return float(m.group()) if m else None

def near(target, tol):
    return lambda a: num(a) is not None and abs(num(a) - target) <= tol

def has(*words):
    return lambda a: any(w.lower() in a.lower() for w in words)

def letter(ch):
    return lambda a: re.match(r"\W*\(?([A-Da-d])\b", a) is not None and re.match(r"\W*\(?([A-Da-d])\b", a).group(1).upper() == ch

YESNO = " Answer only yes or no."
TAB_Q = "This is a settings menu with the tabs General, Graphics, Controls and Sound. Which tab is selected? Answer with one word."
ICON_MC = ("In the row labelled '{row}', which controller input is shown as its binding? "
           "A) right trigger (RT)  B) left stick click (L3)  C) Back/View button  D) a gear icon. Answer with the letter only.")
CASES = [
    ("tab_general", "plain", "menu_general.png", TAB_Q, has("general")),
    ("tab_graphics", "plain", "menu_graphics.png", TAB_Q, has("graphics")),
    ("tab_controls", "plain", "menu_controls.png", TAB_Q, has("controls")),
    ("tab_sound", "plain", "menu_sound.png", TAB_Q, has("sound")),
    ("hud_battle_1", "plain", "battle_1.png", "Does this game screenshot show HUD elements such as text labels or gauges?" + YESNO, has("yes")),
    ("hud_battle_2", "plain", "battle_2.png", "Does this game screenshot show HUD elements such as text labels or gauges?" + YESNO, has("yes")),
    ("black_real", "plain", "black.png", "Is this image almost entirely black?" + YESNO, has("yes")),
    ("black_battle", "plain", "battle_1.png", "Is this image almost entirely black?" + YESNO, has("no")),
    ("black_synth", "plain", "derived/black_synthetic.png", "Is this image almost entirely black?" + YESNO, has("yes")),
    ("glitch_yes", "plain", "derived/battle_glitch.png", "Is there a thin, perfectly straight horizontal line running across the whole image (a rendering glitch)?" + YESNO, has("yes")),
    ("glitch_no", "plain", "battle_1.png", "Is there a thin, perfectly straight horizontal line running across the whole image (a rendering glitch)?" + YESNO, has("no")),
    ("ff_icon_plain", "plain", "menu_controls_scrolled.png", ICON_MC.format(row="Fast-Forward"), letter("A")),
    ("rw_icon_plain", "plain", "menu_controls_scrolled.png", ICON_MC.format(row="Rewind"), letter("B")),
    ("ff_icon_zoom", "zoom", "derived/rows_zoom.png", ICON_MC.format(row="Fast-Forward"), letter("A")),
    ("rw_icon_zoom", "zoom", "derived/rows_zoom.png", ICON_MC.format(row="Rewind"), letter("B")),
    ("x_controls_plain", "plain", "menu_general.png", "At what x pixel (from the left edge of the image) is the center of the tab label 'Controls'? Answer with one number.", near(548, 30)),
    ("x_sound_plain", "plain", "menu_general.png", "At what x pixel (from the left edge of the image) is the center of the tab label 'Sound'? Answer with one number.", near(720, 30)),
    ("x_controls_grid", "grid", "derived/menu_grid.png", "Magenta vertical lines are labelled with their x pixel value at the bottom. Using them, at what x pixel is the center of the tab label 'Controls'? Answer with one number.", near(548, 30)),
    ("x_sound_grid", "grid", "derived/menu_grid.png", "Magenta vertical lines are labelled with their x pixel value at the bottom. Using them, at what x pixel is the center of the tab label 'Sound'? Answer with one number.", near(720, 30)),
    ("y_tabs_grid", "grid", "derived/menu_grid.png", "Cyan horizontal lines are labelled with their y pixel value at the left. Using them, at what y pixel is the vertical center of the row of tab labels? Answer with one number.", near(161, 25)),
]

def main():
    if len(sys.argv) < 3:
        sys.exit("usage: vision_bench.py <bench dir> <model> [model...]")
    d = Path(sys.argv[1])
    prepare(d)
    sys.stdout.reconfigure(encoding="utf-8")
    summary = []
    for model in sys.argv[2:]:
        print(f"=== {model}")
        score, per_idea, total_s = 0, {}, 0.0
        for cid, idea, img, q, check in CASES:
            t = time.time()
            try:
                a = ask(model, d / img, q)
            except Exception as e:
                a = f"ERROR {type(e).__name__}"
            s = time.time() - t
            total_s += s
            ok = check(a)
            score += ok
            p = per_idea.setdefault(idea, [0, 0]); p[0] += ok; p[1] += 1
            print(f"{'PASS' if ok else 'FAIL'} {s:5.1f}s {cid:18} {a[:70]!r}", flush=True)
        ideas = ", ".join(f"{k} {v[0]}/{v[1]}" for k, v in per_idea.items())
        summary.append(f"{model}: {score}/{len(CASES)} ({ideas}), {total_s / len(CASES):.1f} s per question")
    print("\n" + "\n".join(summary))

if __name__ == "__main__":
    main()
