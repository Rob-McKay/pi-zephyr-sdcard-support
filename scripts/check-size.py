#!/usr/bin/env python3
# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
"""Check that no function is too long or too complex: a backstop for the
single responsibility rule (docs/CODING.md, section 2).

Measures every C and Python function git tracks with lizard, and fails any
over MAX_NLOC lines of code or cyclomatic complexity MAX_CCN, unless it is
an exception listed with its reason in EXCEPTIONS. Prints one line per
function over a limit and exits non-zero if there are any.

    scripts/check-size.py
"""

import os
import subprocess
import sys

import lizard

## Most lines of code (not counting blanks and comments) in a function.
MAX_NLOC = 50
## Most cyclomatic complexity in a function.
MAX_CCN = 15

## Functions allowed over a limit, as "path:function", with the reason.
EXCEPTIONS = {}

## Generated data, not code.
SKIP = set()


## @brief List the C and Python files git tracks.
#
# @param root Repository root.
# @return Paths relative to @p root.
def source_files(root):
    out = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "*.c", "*.h", "*.py"],
                         cwd=root, check=True, capture_output=True, text=True).stdout
    return [p for p in out.split() if p not in SKIP]


## @brief Check one file's functions against the limits.
#
# @param root Repository root.
# @param path File, relative to @p root.
# @return Problems found, as "file:line: message" strings.
def check_file(root, path):
    problems = []
    for fn in lizard.analyze_file(os.path.join(root, path)).function_list:
        if f"{path}:{fn.name}" in EXCEPTIONS:
            continue
        if fn.nloc > MAX_NLOC or fn.cyclomatic_complexity > MAX_CCN:
            problems.append(f"{path}:{fn.start_line}: {fn.name}(): {fn.nloc} lines, "
                            f"complexity {fn.cyclomatic_complexity} (limits {MAX_NLOC}, "
                            f"{MAX_CCN}): split it")
    return problems


## @brief Check every function and print the problems.
#
# @return Exit status: 0 if every function is within the limits.
def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    problems = []
    for path in source_files(root):
        problems += check_file(root, path)
    for p in problems:
        print(p)
    print(f"{len(problems)} function(s) too long or complex" if problems
          else f"All functions within {MAX_NLOC} lines and complexity {MAX_CCN}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
