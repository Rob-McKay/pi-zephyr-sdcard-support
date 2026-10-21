#!/usr/bin/env python3
"""Run clang-tidy on a source file using the build's compile command.

CMake's <LANG>_CLANG_TIDY hook (see cmake/strict.cmake) runs this before
each compile as

    clang-tidy-cross.py [--clang-tidy=<exe>] [clang-tidy options] <source> -- <compiler> <args>

<exe> is the clang-tidy to run (cmake/strict.cmake passes the one it
checked); without it, the first clang-tidy on PATH.

Every build compiles with clang (cmake/toolchain.cmake): the Pi firmware
with Arm Toolchain for Embedded (ATfE), a cross compiler that finds its C
library's headers in its per-target runtimes, and the native_sim tests
with the host's clang. clang-tidy's own clang knows neither ATfE's
runtimes nor its configuration files, so for a cross compile this wrapper
adds the C library's include directories as the real compiler finds them,
then runs clang-tidy and returns its exit status. Findings are reported in
this repository's headers as well as its sources.
"""

import os
import re
import shutil
import subprocess
import sys

## A clang's own built-in headers (stddef.h, ...): clang-tidy's clang has its own.
BUILTIN_HEADERS = re.compile(r"/lib/clang/[^/]+/include$")


## @brief Ask a compiler for its system include directories.
#
# The compiler's own built-in headers are left out; what remains is the C
# library's.
#
# @param compiler_cmd The compiler and the options that select headers
#        (--sysroot, --target=, --config=, -m...).
# @return Directories, in search order.
def compiler_system_includes(compiler_cmd):
    out = subprocess.run(compiler_cmd + ["-E", "-Wp,-v", "-x", "c", os.devnull],
                         capture_output=True, text=True).stderr
    dirs, inside = [], False
    for line in out.splitlines():
        if line.startswith("#include <...>"):
            inside = True
        elif line.startswith("End of search list"):
            break
        elif inside:
            d = os.path.normpath(line.strip())
            if not BUILTIN_HEADERS.search(d):
                dirs.append(d)
    return dirs


## @brief Translate a compile command into one for clang-tidy's clang.
#
# @param cmd The compile command: compiler first.
# @return The command for clang-tidy, after its "--".
def clang_command(cmd):
    compiler, args = cmd[0], cmd[1:]
    # Zephyr's ztest macros (zassert_true(cond), ...) are called without
    # their optional message, which clang 19 and later report under
    # -Wpedantic before C23; the host clang 18 that builds the tests does not.
    out = ["clang", "-Wno-unknown-warning-option"] + args + \
        ["-Wno-variadic-macro-arguments-omitted"]
    if not any(a.startswith("--target=") for a in args):
        return out
    header_opts = [a for a in args if a.startswith(("--sysroot", "--target=", "--config", "-m"))]
    for d in compiler_system_includes([compiler] + header_opts):
        out += ["-isystem", d]
    return out


## @brief Entry point.
#
# @return clang-tidy's exit status.
def main():
    argv = sys.argv[1:]
    if "--" not in argv:
        print("usage: clang-tidy-cross.py [--clang-tidy=<exe>] [clang-tidy args] <source> -- "
              "<compile command>",
              file=sys.stderr)
        return 2
    tidy = shutil.which("clang-tidy")
    if argv[0].startswith("--clang-tidy="):
        tidy = argv.pop(0).split("=", 1)[1]
    sep = argv.index("--")
    # Report findings in this repository's headers, not in other modules'.
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    header_filter = f"--header-filter=^{re.escape(root)}/(hal|tests)/"
    cmd = [tidy, header_filter] + argv[:sep] + ["--"] + clang_command(argv[sep + 1:])
    return subprocess.run(cmd).returncode


if __name__ == "__main__":
    sys.exit(main())
