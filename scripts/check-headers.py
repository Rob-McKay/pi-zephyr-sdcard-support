#!/usr/bin/env python3
# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
"""Check that every source file carries the copyright and licence lines.

Every file git tracks needs, in its first lines, "Copyright (c) <year>
Rob McKay" and "SPDX-License-Identifier: Apache-2.0", except patches to
other projects, logs, documents, media and the SBOM (EXEMPT). Prints one
line per file without them and exits non-zero if there are any. With
--fix, adds the missing lines in the file's own comment style instead.

    scripts/check-headers.py [--fix]
"""

import fnmatch
import os
import re
import subprocess
import sys

## Owner and year written by --fix.
COPYRIGHT = "Copyright (c) 2026 Rob McKay"
SPDX = "SPDX-License-Identifier: Apache-2.0"

## Files that need no header: other projects' changes (patches, and commit
# messages for them), records and data.
EXEMPT = ["*.patch", "*commit-message.txt", "*.log", "*.md", "*README.txt", "LICENSE",
          "*.jpg", "*.png", "*.svg", "*.aac", "*.mp3", "*.json"]

## Files whose comments are C block comments; all others take "#" lines.
C_STYLE = ["*.c", "*.h", "*.S", "*.dts", "*.overlay"]

## How far into a file the lines may be.
HEAD_LINES = 15

COPYRIGHT_RE = re.compile(r"Copyright \(c\) 20\d\d(-20\d\d)? Rob McKay")


## @brief List the files git tracks.
#
# @param root Repository root.
# @return Paths relative to @p root.
def tracked_files(root):
    out = subprocess.run(["git", "-c", "safe.directory=*", "ls-files"], cwd=root, check=True,
                         capture_output=True, text=True).stdout
    return out.split()


## @brief Tell whether a path matches any of some patterns.
#
# @param path Relative path.
# @param patterns fnmatch patterns, matched against the whole path.
# @return True if one matches.
def matches(path, patterns):
    return any(fnmatch.fnmatch(path, p) for p in patterns)


## @brief Tell whether a file's first lines have both header lines.
#
# @param lines The file's lines.
# @return True if they do.
def has_header(lines):
    head = "".join(lines[:HEAD_LINES])
    return bool(COPYRIGHT_RE.search(head)) and SPDX in head


## @brief Find the line holding the licence identifier, if near the top.
#
# @param lines The file's lines.
# @return Its index, or None.
def spdx_line(lines):
    for i, line in enumerate(lines[:HEAD_LINES]):
        if "SPDX-License-Identifier" in line:
            return i
    return None


## @brief Add the copyright line, and the licence line if missing.
#
# The copyright goes just before an existing licence line, in that line's
# comment style; a file with neither gets both at the top, after any "#!"
# line.
#
# @param path Relative path, for its comment style.
# @param lines The file's lines.
# @return The new lines.
def add_header(path, lines):
    i = spdx_line(lines)
    if i is not None and lines[i].lstrip().startswith("/*"):
        # A one-line "/* SPDX... */": make it a block holding both lines
        indent = lines[i][:lines[i].index("/*")]
        block = [indent + "/*\n", indent + " * " + COPYRIGHT + "\n",
                 indent + " * " + SPDX + "\n", indent + " */\n"]
        return lines[:i] + block + lines[i + 1:]
    if i is not None:
        prefix = lines[i][:lines[i].index("SPDX")]
        return lines[:i] + [prefix + COPYRIGHT + "\n"] + lines[i:]
    if matches(path, C_STYLE):
        block = ["/*\n", " * " + COPYRIGHT + "\n", " * " + SPDX + "\n", " */\n"]
    else:
        block = ["# " + COPYRIGHT + "\n", "# " + SPDX + "\n"]
    start = 1 if lines and lines[0].startswith("#!") else 0
    return lines[:start] + block + lines[start:]


## @brief Check, or with fix set repair, every tracked source file.
#
# @param root Repository root.
# @param fix Add missing lines instead of reporting them.
# @return Paths still without a header.
def check_all(root, fix):
    missing = []
    for path in tracked_files(root):
        if matches(path, EXEMPT):
            continue
        full = os.path.join(root, path)
        with open(full, encoding="utf-8") as f:
            lines = f.readlines()
        if has_header(lines):
            continue
        if fix:
            with open(full, "w", encoding="utf-8") as f:
                f.writelines(add_header(path, lines))
        else:
            missing.append(path)
    return missing


## @brief Run the check on the repository this script is in.
#
# @return Exit status: 0 if every file has its header.
def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    missing = check_all(root, "--fix" in sys.argv[1:])
    for path in missing:
        print(f"{path}: no '{COPYRIGHT}' and '{SPDX}' in its first {HEAD_LINES} lines")
    if missing:
        print("Run scripts/check-headers.py --fix to add them.")
        return 1
    print("All source files have copyright and licence headers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
