#!/usr/bin/env python3
"""Check that every function in the repository's own code is documented.

Runs Doxygen (docs/Doxyfile) and reads its XML output. Each function needs
a Doxygen comment with a brief description, an @param for every parameter
and, when it returns a value, an @return or @retval. Prints one line per
problem and exits non-zero if there are any.

    scripts/check-docs.py
"""

import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

## Parameters that are never documented (Python's implicit first argument).
IMPLICIT_PARAMS = {"self", "cls"}


## @brief Run Doxygen on docs/Doxyfile and return its XML output directory.
#
# @param root Repository root.
# @return Path of the XML directory.
def run_doxygen(root):
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
    for name in os.listdir(xml_dir):
        if not name.endswith(".xml") or name in ("index.xml", "Doxyfile.xml"):
            continue
        for member in ET.parse(os.path.join(xml_dir, name)).iter("memberdef"):
            if member.get("kind") != "function" or member.get("id") in seen:
                continue
            seen.add(member.get("id"))
            problems.update(check_function(member, root))
    return sorted(problems)


## @brief Entry point.
#
# @return Process exit status: 0 if every function is documented.
def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    problems = check_all(run_doxygen(root), root)
    for p in problems:
        print(p)
    print(f"{len(problems)} documentation problem(s)" if problems
          else "All functions documented")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
