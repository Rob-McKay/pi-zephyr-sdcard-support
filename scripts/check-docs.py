#!/usr/bin/env python3
# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
"""Check that every function in the repository's own code is documented.

Runs Doxygen (docs/Doxyfile) on the C and Python code and reads its XML
output. Each function needs a Doxygen comment with a brief description, an
@param for every parameter and, when it returns a value, an @return or
@retval. A function declared in one of our headers is documented there:
each definition of it has a plain comment naming the header
("/* Interface: wifi.h. */"), not a Doxygen block. Shell functions, which Doxygen doesn't read, need a "## @brief"
comment block with an "@param $N" for each positional parameter they use.
Prints one line per problem and exits non-zero if there are any.

    scripts/check-docs.py
"""

import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

## Parameters that are never documented (Python's implicit first argument).
IMPLICIT_PARAMS = {"self", "cls"}


## @brief Run Doxygen afresh on docs/Doxyfile and return its XML output directory.
#
# @param root Repository root.
# @return Path of the XML directory.
def run_doxygen(root):
    # Stale XML from files since removed would be checked too
    shutil.rmtree(os.path.join(root, "build", "doxygen"), ignore_errors=True)
    subprocess.run(["doxygen", "docs/Doxyfile"], cwd=root, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.join(root, "build", "doxygen", "xml")


## @brief Concatenate the text inside an XML element.
#
# @param elem Element, or None.
# @return Its text with whitespace collapsed; "" for None.
def text_of(elem):
    if elem is None:
        return ""
    return " ".join("".join(elem.itertext()).split())


## @brief Collect the parameter names a function's documentation covers.
#
# @param member Doxygen memberdef element.
# @return Set of documented parameter names.
def documented_params(member):
    names = set()
    for plist in member.iter("parameterlist"):
        if plist.get("kind") != "param":
            continue
        for name in plist.iter("parametername"):
            names.add(text_of(name))
    return names


## @brief Tell whether a function's documentation describes its result.
#
# @param member Doxygen memberdef element.
# @return True if it has an @return section or @retval entries.
def documents_return(member):
    for sect in member.iter("simplesect"):
        if sect.get("kind") == "return":
            return True
    return any(p.get("kind") == "retval" for p in member.iter("parameterlist"))


## @brief List a function's parameter names from its declaration.
#
# @param member Doxygen memberdef element.
# @return Parameter names, without unnamed, variadic and implicit ones.
def declared_params(member):
    names = []
    for param in member.findall("param"):
        name = text_of(param.find("declname"))
        if name and name not in IMPLICIT_PARAMS and text_of(param.find("type")) != "...":
            names.append(name)
    return names


## @brief Tell whether a Python function's body returns or yields a value.
#
# @param path Source file.
# @param start First line of the body (1-based).
# @param end Last line of the body.
# @return True if any line returns an expression or yields.
def python_returns_value(path, start, end):
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()[start - 1:end]
    return any(re.match(r"\s*(return\s+\S|yield\b)", line) for line in lines)


## @brief Tell whether a function returns a value.
#
# @param member Doxygen memberdef element.
# @param path Source file the function is defined in.
# @return True for a non-void C function or a Python function that returns.
def returns_value(member, path):
    if path.endswith(".py"):
        loc = member.find("location")
        start, end = loc.get("bodystart"), loc.get("bodyend")
        if not start or not end or int(end) < 0:
            return False
        return python_returns_value(path, int(start), int(end))
    rtype = text_of(member.find("type"))
    rtype = re.sub(r"\b(static|inline|extern|const)\b", "", rtype).strip()
    return rtype not in ("", "void")


## @brief Check one function's documentation.
#
# @param member Doxygen memberdef element of kind "function".
# @param root Repository root, for relative paths in messages.
# @return Problems found, as "file:line: message" strings.
def check_function(member, root):
    loc = member.find("location")
    path = loc.get("bodyfile") or loc.get("file")
    line = loc.get("bodystart") or loc.get("line")
    where = f"{os.path.relpath(path, root)}:{line}: {text_of(member.find('name'))}()"
    if not text_of(member.find("briefdescription")):
        return [f"{where}: no Doxygen comment with a brief description"]
    problems = []
    documented = documented_params(member)
    for name in declared_params(member):
        if name not in documented:
            problems.append(f"{where}: parameter '{name}' not documented")
    if returns_value(member, path) and not documents_return(member):
        problems.append(f"{where}: return value not documented")
    return problems


## @brief Find the comment just before a line, skipping blank lines.
#
# @param lines File lines.
# @param index Index of the line.
# @return The comment's lines, or [] if the line before isn't the end of
#         a comment.
def comment_before(lines, index):
    end = index - 1
    while end >= 0 and not lines[end].strip():
        end -= 1
    if end < 0 or not lines[end].rstrip().endswith("*/"):
        return []
    start = end
    while start > 0 and "/*" not in lines[start]:
        start -= 1
    return lines[start:end + 1]


## @brief List the C sources git tracks.
#
# @param root Repository root.
# @return Absolute paths.
def c_sources(root):
    out = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "*.c"], cwd=root,
                         check=True, capture_output=True, text=True).stdout
    return [os.path.join(root, p) for p in out.split()]


## @brief Check that each definition of a public function names its header.
#
# Every source is searched: an interface such as pcm_out.h has several
# implementations, and a function can be defined more than once under #if.
#
# @param name Function name.
# @param header The header declaring it.
# @param sources C sources to search.
# @param root Repository root, for relative paths in messages.
# @return Problems found, as "file:line: message" strings.
def check_interface_comments(name, header, sources, root):
    want = f"Interface: {os.path.basename(header)}"
    problems = []
    definition = re.compile(r"^[A-Za-z_][^;()]*\b" + re.escape(name) + r"\(")
    for path in sources:
        with open(path, encoding="utf-8") as f:
            lines = f.read().split("\n")
        problems += check_definitions(lines, definition, want, name, os.path.relpath(path, root))
    return problems


## @brief Check the comments before one file's definitions of a function.
#
# @param lines The file's lines.
# @param definition Pattern matching a definition's first line.
# @param want Text the comment must contain ("Interface: x.h").
# @param name Function name.
# @param rel The file's path, for messages.
# @return Problems found, as "file:line: message" strings.
def check_definitions(lines, definition, want, name, rel):
    problems = []
    for i, line in enumerate(lines):
        if not definition.match(line) or line.rstrip().endswith(";"):
            continue
        comment = comment_before(lines, i)
        where = f"{rel}:{i + 1}: {name}()"
        if comment and comment[0].lstrip().startswith("/**"):
            problems.append(f"{where}: document it in its header, not at the definition")
        elif not any(want in c for c in comment):
            problems.append(f"{where}: no '/* {want}. */' comment")
    return problems


## @brief Check a function declared in a header: documented there, and
#         each definition naming the header.
#
# @param member Doxygen memberdef element of kind "function".
# @param sources C sources to search for its definitions.
# @param root Repository root.
# @return Problems found, as "file:line: message" strings; [] for a
#         function not declared in one of our headers apart from its body.
def check_public(member, sources, root):
    loc = member.find("location")
    header, body = loc.get("file"), loc.get("bodyfile")
    if not header or not body or not header.endswith(".h") or header == body:
        return []
    return check_interface_comments(text_of(member.find("name")), header, sources, root)


## @brief Check every function in Doxygen's XML output.
#
# Each function is checked once, at whichever of its declaration and
# definition carries the documentation (Doxygen merges the two).
#
# @param xml_dir Doxygen XML directory.
# @param root Repository root.
# @return All problems found, sorted.
def check_all(xml_dir, root):
    problems = set()
    seen = set()
    sources = c_sources(root)
    for name in os.listdir(xml_dir):
        if not name.endswith(".xml") or name in ("index.xml", "Doxyfile.xml"):
            continue
        for member in ET.parse(os.path.join(xml_dir, name)).iter("memberdef"):
            if member.get("kind") != "function" or member.get("id") in seen:
                continue
            seen.add(member.get("id"))
            problems.update(check_function(member, root))
            problems.update(check_public(member, sources, root))
    return sorted(problems)


## @brief List the shell scripts git tracks.
#
# @param root Repository root.
# @return Paths relative to @p root.
def shell_scripts(root):
    out = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "*.sh"], cwd=root,
                         check=True, capture_output=True, text=True).stdout
    return out.split()


## @brief Find a shell function's body: the lines up to its closing brace.
#
# @param lines The script's lines.
# @param start Index of the line declaring the function.
# @return The body's lines.
def shell_body(lines, start):
    for end in range(start + 1, len(lines)):
        if lines[end].startswith("}"):
            return lines[start + 1:end]
    return lines[start + 1:]


## @brief Check the comments of one shell script's functions.
#
# @param root Repository root.
# @param path Script, relative to @p root.
# @return Problems found, as "file:line: message" strings.
def check_shell(root, path):
    with open(os.path.join(root, path), encoding="utf-8") as f:
        lines = f.readlines()
    problems = []
    for i, line in enumerate(lines):
        m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\)\s*\{", line)
        if not m:
            continue
        where = f"{path}:{i + 1}: {m.group(1)}()"
        comment = []
        for prev in reversed(lines[:i]):
            if not prev.lstrip().startswith("#"):
                break
            comment.insert(0, prev)
        doc = "".join(comment)
        if "@brief" not in doc:
            problems.append(f"{where}: no '## @brief' comment")
            continue
        used = set(re.findall(r"\$\{?([1-9])", "".join(shell_body(lines, i))))
        for n in sorted(used):
            if f"@param ${n}" not in doc:
                problems.append(f"{where}: parameter ${n} not documented")
    return problems


## @brief Check every C, Python and shell function, and print the problems.
#
# @return Exit status: 0 if every function is documented.
def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    problems = check_all(run_doxygen(root), root)
    for path in shell_scripts(root):
        problems += check_shell(root, path)
    for p in problems:
        print(p)
    print(f"{len(problems)} documentation problem(s)" if problems
          else "All functions documented")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
