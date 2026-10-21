#!/usr/bin/env python3
"""Check that a test run executed every line of the code it covers.

Reads, through `llvm-cov gcov`, the gcov-format data that a native_sim test
built with CONFIG_COVERAGE (clang --coverage) leaves after running, and
requires 100% line coverage of each source file matching the given
patterns. Prints the coverage of each file and exits
non-zero if any is short of 100%, or if no file matches.

    scripts/check-coverage.py <build dir> <source glob>...
"""

import fnmatch
import os
import re
import subprocess
import sys

## gcov's per-file summary: the file name, then its line coverage.
SUMMARY = re.compile(r"File '([^']+)'\nLines executed:([0-9.]+)% of (\d+)")


## @brief Find the coverage data files of this repository's code.
#
# Our code is built into the app and into this module's libraries, so
# those object directories are searched; Zephyr's own are not.
#
# @param build Build directory.
# @return Paths of the .gcda files.
def gcda_files(build):
    roots = [os.path.join(build, "CMakeFiles", "app.dir"), os.path.join(build, "modules")]
    found = []
    for top in roots:
        for root, _, files in os.walk(top):
            found += [os.path.join(root, f) for f in files if f.endswith(".gcda")]
    return found


## @brief Run llvm-cov gcov on one data file and read its per-source summaries.
#
# @param gcda Path of a .gcda file.
# @return List of (source path, percent of lines run, line count).
def gcov_summaries(gcda):
    out = subprocess.run(["llvm-cov", "gcov", "-n", gcda], cwd=os.path.dirname(gcda),
                         capture_output=True, text=True, check=True).stdout
    return [(f, float(p), int(n)) for f, p, n in SUMMARY.findall(out)]


## @brief Collect the coverage of the sources matching any pattern.
#
# A source compiled into several objects keeps its best result.
#
# @param build Build directory.
# @param patterns Glob patterns, matched against the end of the source path.
# @return Dict of source path to (percent, line count).
def coverage(build, patterns):
    result = {}
    for gcda in gcda_files(build):
        for src, pct, lines in gcov_summaries(gcda):
            path = os.path.normpath(src)
            if not any(fnmatch.fnmatch(path, "*/" + p) for p in patterns):
                continue
            if path not in result or pct > result[path][0]:
                result[path] = (pct, lines)
    return result


## @brief Entry point.
#
# @return Process exit status: 0 if every matching file is fully covered.
def main():
    if len(sys.argv) < 3:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    build, patterns = os.path.abspath(sys.argv[1]), sys.argv[2:]
    result = coverage(build, patterns)
    if not result:
        print(f"no coverage data for {' '.join(patterns)} in {build}")
        return 1
    short = 0
    for path, (pct, lines) in sorted(result.items()):
        print(f"{pct:6.2f}% of {lines:4d} lines  {path}")
        short += pct < 100.0
    return 1 if short else 0


if __name__ == "__main__":
    sys.exit(main())
