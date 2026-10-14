"""Claude Code hook: records the session state for agent.py.

Usage in settings.json hooks:  python claude_hook.py <working|tool|waiting|done|idle>
Reads the hook JSON from stdin (PreToolUse carries tool_name). Never fails the hook.
"""
import json
import os
import sys
import tempfile
import time

STATE_FILE = os.path.join(tempfile.gettempdir(), "claude_tdisplay.json")


def main():
    event = sys.argv[1] if len(sys.argv) > 1 else "idle"
    try:
        payload = json.load(sys.stdin)
    except Exception:
        payload = {}
    try:
        with open(STATE_FILE, encoding="utf-8") as f:
            d = json.load(f)
    except Exception:
        d = {}

    now = time.time()
    prev = d.get("state", "idle")
    if event == "working":  # new user prompt
        d.update(state="working", tool="", work_start=now)
    elif event == "tool":
        if prev not in ("working", "waiting"):
            d["work_start"] = now
        d.update(state="working", tool=payload.get("tool_name", ""))
    elif event == "waiting":
        d.setdefault("work_start", now)
        d["state"] = "waiting"
    elif event == "done":
        d.update(state="done", tool="", dur=now - d.get("work_start", now))
    else:
        d.update(state="idle", tool="")
    d["state_since"] = now

    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(d, f)
    os.replace(tmp, STATE_FILE)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
