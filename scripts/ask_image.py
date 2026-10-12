# Asks the local LM Studio vision model about an image and prints its text answer. The image only goes to
# LM Studio on this PC (http://127.0.0.1:1234), so screenshots stay local; whoever runs this sees text only.
# Usage: python scripts\ask_image.py <image.png> "<question>" [model]   (default model: google/gemma-4-e4b)
# Local vision models miss fine details (1-pixel lines, small rendering bugs): ask concrete yes/no questions
# and have a person confirm anything important.
import base64, json, sys, urllib.request

URL = "http://127.0.0.1:1234/v1/chat/completions"

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__ or "usage: ask_image.py <image.png> <question> [model]")
    path, question = sys.argv[1], sys.argv[2]
    model = sys.argv[3] if len(sys.argv) > 3 else "google/gemma-4-e4b"
    image = base64.b64encode(open(path, "rb").read()).decode("ascii")
    body = {
        "model": model,
        "temperature": 0,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": question},
                {"type": "image_url", "image_url": {"url": "data:image/png;base64," + image}},
            ],
        }],
    }
    req = urllib.request.Request(URL, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as resp:
        answer = json.load(resp)["choices"][0]["message"]["content"]
    sys.stdout.reconfigure(encoding="utf-8")  # the Windows console's default code page can't print all symbols
    print(answer.strip())

if __name__ == "__main__":
    main()
