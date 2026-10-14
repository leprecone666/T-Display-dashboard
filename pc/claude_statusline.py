"""Claude Code statusLine command: stores plan limits / context fill for agent.py.

Claude Code pipes a JSON document on stdin and shows whatever we print.
Output is a minimal one-liner; never fails.
"""
import json
import os
import sys
import tempfile
import time

TMP = tempfile.gettempdir()
USAGE_FILE = os.path.join(TMP, "claude_tdisplay_usage.json")
RAW_FILE = os.path.join(TMP, "claude_statusline_raw.json")


def pick(d, *paths):
    for path in paths:
        cur = d
        for k in path.split("."):
            if isinstance(cur, dict) and k in cur:
                cur = cur[k]
            else:
                cur = None
                break
        if cur is not None:
            return cur
    return None


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        return
    try:  # keep the latest raw payload for debugging field names
        with open(RAW_FILE, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1)
    except Exception:
        pass

    five = pick(data, "rate_limits.five_hour.used_percentage")
    five_reset = pick(data, "rate_limits.five_hour.resets_at")
    week = pick(data, "rate_limits.seven_day.used_percentage")
    week_reset = pick(data, "rate_limits.seven_day.resets_at")
    ctx = pick(data, "context_window.used_percentage")

    out = {"ts": time.time(), "five": five, "five_reset": five_reset,
           "week": week, "week_reset": week_reset, "ctx": ctx}
    tmp = USAGE_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(out, f)
    os.replace(tmp, USAGE_FILE)

    bits = []
    if five is not None:
        bits.append(f"5h {five:.0f}%")
    if week is not None:
        bits.append(f"wk {week:.0f}%")
    if ctx is not None:
        bits.append(f"ctx {ctx:.0f}%")
    print(" | ".join(bits))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
