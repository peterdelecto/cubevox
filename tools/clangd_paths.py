#!/usr/bin/env python3
"""Points the two .clangd files at this machine.

clangd has no variable expansion, so both files hold absolute paths under two prefixes. They are
the Arduino data dir (the STM32 core and the xpack compiler) and the checkout. This script rewrites
each prefix to the one arduino-cli and the script's own location report, and leaves every other
line alone. It also moves a third prefix, the Arduino user dir (libraries), which no include line
holds now because no library header is on the include path. Run it after a clone on another
machine, and after moving the checkout or the Arduino dirs.

  python3 -I tools/clangd_paths.py
"""

import os
import re
import subprocess
import sys

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CLANGD_FILES = ("firmware/cubevox/.clangd", "firmware/bootloader/.clangd")

# One regex per prefix. Each captures the prefix a file holds now, from a path that always follows it.
# A prefix may hold spaces, so the capture runs to the first match of the fixed part that follows.
DATA_PREFIX = re.compile(r"-(?:I|isystem)(/.*?)/packages/STMicroelectronics/")
USER_PREFIX = re.compile(r"-I(/.*?)/libraries/U8g2/")
REPO_PREFIX = re.compile(r"-I(/.*?)/(?:firmware/cubevox/src|firmware/bootloader/boards/cubevox|tools/font-editor/fonts)\b")


def swap_prefix(text, pattern, new):
    """Replaces the prefix `pattern` finds with `new` wherever a path starts with it. Returns the text."""
    found = pattern.search(text)
    if not found or found.group(1) == new:
        return text
    return re.sub(re.escape(found.group(1)) + r"(?=/|\s|$)", lambda _: new, text, flags=re.MULTILINE)


def rewrite(text, data, user, repo):
    """The .clangd text with its three path prefixes replaced by this machine's."""
    for pattern, new in ((DATA_PREFIX, data), (USER_PREFIX, user), (REPO_PREFIX, repo)):
        text = swap_prefix(text, pattern, new)
    return text


def arduino_dir(key):
    try:
        run = subprocess.run(["arduino-cli", "config", "get", key], capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f"[ERROR] arduino-cli config get {key} failed: {e}")
    return run.stdout.strip()


def main():
    data, user = arduino_dir("directories.data"), arduino_dir("directories.user")
    for name in CLANGD_FILES:
        path = os.path.join(ROOT, name)
        with open(path) as f:
            old = f.read()
        new = rewrite(old, data, user, ROOT)
        if new != old:
            with open(path, "w") as f:
                f.write(new)
        print(f"[clangd] {name}: {'rewritten' if new != old else 'already points here'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
