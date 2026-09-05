#!/usr/bin/env python3
"""Install or remove the World Clock row without rewriting user JSONC."""

import json
import os
from pathlib import Path
import re
import sys

BEGIN = "// BEGIN spacexrace.worldclock (managed)"
END = "// END spacexrace.worldclock (managed)"
TARGET = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "omarchy/extensions/omarchy-menu.jsonc"


def remove_block(text):
    pattern = re.compile(r"\n?[ \t]*" + re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n?", re.S)
    return pattern.sub("\n", text)


def install():
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    text = TARGET.read_text() if TARGET.exists() else "{}\n"
    text = remove_block(text)
    close = text.rfind("}")
    if close < 0:
        raise SystemExit(f"Refusing to modify invalid menu extension: {TARGET}")
    before = text[:close].rstrip()
    significant = re.sub(r"//[^\n]*|/\*.*?\*/", "", before, flags=re.S).rstrip()
    prefix = "" if significant.endswith("{") or before.endswith(",") else ","
    entry = {
        "icon": "󰥔",
        "label": "World Clock Wallpaper",
        "description": "Toggle the animated Earth background",
        "action": '"$HOME/.config/omarchy/plugins/spacexrace.worldclock/bin/world-clock" toggle',
        "checked": "omarchy plugin list --json | jq -e 'any(.[]; .id == \"spacexrace.worldclock\" and .enabled)' >/dev/null"
    }
    block = (f"\n  {BEGIN}\n  {prefix}\"trigger.toggle.world-clock-wallpaper\": "
             f"{json.dumps(entry, ensure_ascii=False, separators=(',', ':'))},\n  {END}\n")
    TARGET.write_text(text[:close].rstrip() + block + text[close:])


def uninstall():
    if TARGET.exists():
        TARGET.write_text(remove_block(TARGET.read_text()))


if __name__ == "__main__":
    {"install": install, "uninstall": uninstall}[sys.argv[1]]()
