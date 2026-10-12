# Sends a task (prompt file) plus context files to the local LM Studio model and writes its answer to a file.
# Everything stays on this PC (http://127.0.0.1:1234). Use it for bounded drafting/analysis tasks and review the
# result: the local model is fast but inconsistent on judgment calls (see CLAUDE.md "Local LLM notes").
# Usage: python scripts\ask_llm.py <prompt.txt> <answer.txt> [context files...] [--model <id>] [--max-tokens N]
import json, re, sys, time, urllib.request

URL = "http://127.0.0.1:1234/v1/chat/completions"

def main():
    args = sys.argv[1:]
    model, max_tokens = "qwen3.6-35b-a3b", 32000
    if "--model" in args:
        i = args.index("--model"); model = args[i + 1]; del args[i:i + 2]
    if "--max-tokens" in args:
        i = args.index("--max-tokens"); max_tokens = int(args[i + 1]); del args[i:i + 2]
    if len(args) < 2:
        sys.exit("usage: ask_llm.py <prompt.txt> <answer.txt> [context files...] [--model id] [--max-tokens N]")
    prompt_path, answer_path, context_paths = args[0], args[1], args[2:]
    parts = [open(prompt_path, encoding="utf-8").read()]
    for path in context_paths:
        parts.append(f"\n\n===== FILE: {path} =====\n" + open(path, encoding="utf-8", errors="replace").read())
    body = {
        "model": model,
        "temperature": 0.2,
        "max_tokens": max_tokens,
        "messages": [{"role": "user", "content": "".join(parts)}],
    }
    start = time.time()
    req = urllib.request.Request(URL, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=7200) as resp:
        data = json.load(resp)
    answer = data["choices"][0]["message"]["content"]
    answer = re.sub(r"(?s)<think>.*?</think>", "", answer).strip()  # drop the reasoning part, if any
    open(answer_path, "w", encoding="utf-8", newline="\n").write(answer + "\n")
    usage = data.get("usage", {})
    print(f"{answer_path}: {len(answer)} chars in {time.time() - start:.0f} s, tokens {usage}")

if __name__ == "__main__":
    main()
