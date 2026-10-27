#!/usr/bin/env python3
# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
"""Check that every macro argument which is an expression is parenthesised.

docs/CODING.md, section 0: a function-like macro uses its arguments as
text, so one that doesn't parenthesise a parameter regroups an argument
such as "n + 1" ("max_msgs * size" becomes "n + 1 * size"). Zephyr's own
macros don't all do it. So every argument with a binary, ternary or
assignment operator outside brackets is written in its own parentheses:
K_MSEC((ms / 2)), not K_MSEC(ms / 2). An argument that is one operand,
however written (a name, a literal, a call, a member, an element, a cast,
a unary operator on one of those, a type such as "struct foo *"), binds
tighter than any binary operator and needs none; nor does a designator
(".x = 1"). Nor do the arguments of a macro that stands in for a function
of the same name (a fortified C library's memcpy() or snprintf()): it is
called as the function and is a macro only in some builds.

Checked: each macro call in the repository's own files, and the calls to
other macros in its own macro definitions. (bugprone-macro-parentheses
checks how a definition uses its parameters.) The build runs this on each
of the repository's sources as it compiles (scripts/clang-tidy-cross.py).
By hand, with --fix to add the parentheses:

    scripts/check-macro-args.py [--fix] <build dir>
"""

import importlib.util
import json
import os
import shlex
import sys

import clang.cindex as ci

HERE = os.path.dirname(os.path.realpath(__file__))

## Operators that join two operands (or three, "?"), or assign.
BINARY = {"+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^", "<", ">", "<=", ">=", "==",
          "!=", "&&", "||", "?", ":", "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=",
          "&=", "|=", "^="}

## Operators that are also unary prefixes: binary only after an operand.
AMBIGUOUS = {"+", "-", "*", "&"}

OPEN = {"(": ")", "[": "]", "{": "}"}
CLOSE = {")", "]", "}"}

K = ci.TokenKind


## @brief Load another of the repository's scripts as a module.
#
# @param name Its file name, in scripts/.
# @return The module.
def load_script(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_")[:-3],
                                                  os.path.join(HERE, name))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


## Shared with the global state check: which files are ours, compile flags.
GLOBALS = load_script("check-globals.py")

## libclang's arguments for a compile command, as check-globals.py's.
libclang_args = GLOBALS.libclang_args


## @brief Tell whether a token ends an operand.
#
# @param tok A token.
# @return True for a name, literal or closing bracket, or a postfix ++/--.
def ends_operand(tok):
    if tok.kind in (K.IDENTIFIER, K.LITERAL):
        return True
    return tok.spelling in CLOSE or tok.spelling in ("++", "--")


## @brief Tell whether an operator in an argument joins two operands.
#
# @param arg The argument's tokens.
# @param i Index of the operator.
# @return True for a binary, ternary or assignment operator; False for a
#         unary prefix or a pointer declarator ("struct foo *").
def is_binary(arg, i):
    op = arg[i].spelling
    if op not in BINARY:
        return False
    if op not in AMBIGUOUS:
        return True
    if i == 0 or not ends_operand(arg[i - 1]):
        return False
    return i + 1 < len(arg) and arg[i + 1].spelling not in CLOSE | {"*", "const"}


## @brief Tell whether an argument needs parentheses.
#
# @param arg The argument's tokens.
# @return True if it has a binary operator outside brackets and isn't a
#         designator or already one parenthesised group.
def needs_parens(arg):
    if not arg or arg[0].spelling in (".", "["):
        return False
    depth = 0
    for i, tok in enumerate(arg):
        if tok.spelling in OPEN:
            depth += 1
        elif tok.spelling in CLOSE:
            depth -= 1
        elif depth == 0 and is_binary(arg, i):
            return True
    return False


## @brief Split a macro call's tokens into its arguments.
#
# @param toks Tokens from the "(" after the macro's name.
# @return The arguments' token lists; [] if the call isn't complete.
def split_args(toks):
    args, cur, depth = [], [], 0
    for tok in toks[1:]:
        if tok.spelling in OPEN:
            depth += 1
        elif tok.spelling in CLOSE:
            if depth == 0:
                args.append(cur)
                return args
            depth -= 1
        elif tok.spelling == "," and depth == 0:
            args.append(cur)
            cur = []
            continue
        cur.append(tok)
    return []


## @brief Tell whether two tokens touch, as a function-like macro's name
#         and its "(" do in a definition.
#
# @param a First token.
# @param b Second token.
# @return True if @p b starts where @p a ends.
def adjacent(a, b):
    return a.extent.end.offset == b.extent.start.offset


## @brief Check one macro call's arguments.
#
# @param toks Tokens from the "(" after the macro's name.
# @return (argument number, argument tokens) for each one needing parentheses.
def check_call(toks):
    return [(n + 1, arg) for n, arg in enumerate(split_args(toks)) if needs_parens(arg)]


## @brief Find the macro calls among a run of tokens.
#
# @param toks Tokens: a macro instantiation, or a definition's body.
# @param names Function-like macro names to look for; None to take the
#        first token as the name (an instantiation).
# @return (name, tokens from its "(") for each call.
def calls_in(toks, names):
    if names is None:
        if len(toks) > 1 and toks[1].spelling == "(":
            return [(toks[0].spelling, toks[1:])]
        return []
    return [(t.spelling, toks[i + 1:]) for i, t in enumerate(toks[:-1])
            if t.kind == K.IDENTIFIER and t.spelling in names and toks[i + 1].spelling == "("]


## @brief Collect the function-like macros a translation unit defines.
#
# @param tu Translation unit, parsed with its preprocessing record.
# @return Their names.
def function_like_macros(tu):
    names = set()
    for cur in tu.cursor.get_children():
        if cur.kind == ci.CursorKind.MACRO_DEFINITION:
            toks = list(cur.get_tokens())
            if len(toks) > 1 and toks[1].spelling == "(" and adjacent(toks[0], toks[1]):
                names.add(toks[0].spelling)
    return names


## @brief Collect the functions a translation unit declares.
#
# @param tu Translation unit.
# @return Their names.
def declared_functions(tu):
    return {c.spelling for c in tu.cursor.get_children()
            if c.kind == ci.CursorKind.FUNCTION_DECL}


## @brief Find the arguments needing parentheses in a source's macro calls.
#
# @param source Source file.
# @param args Compiler arguments for libclang.
# @param seen Locations already found (headers are parsed with every source
#        that includes them); updated.
# @return (file, start offset, end offset, line, macro, argument number,
#         text) for each.
def find_in_source(source, args, seen):
    tu = ci.Index.create().parse(
        source, args=args, options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
    fatal = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
    if fatal:
        raise RuntimeError(f"{source}: can't parse: {fatal[0].spelling}")
    macros = function_like_macros(tu)
    functions = declared_functions(tu)
    found = []
    for cur in tu.cursor.get_children():
        loc = cur.location
        if loc.file is None or not GLOBALS.ours(loc.file.name):
            continue
        if cur.kind == ci.CursorKind.MACRO_INSTANTIATION:
            calls = calls_in(list(cur.get_tokens()), None)
        elif cur.kind == ci.CursorKind.MACRO_DEFINITION:
            calls = calls_in(list(cur.get_tokens())[1:], macros)
        else:
            continue
        for name, toks in calls:
            if name in functions:
                continue
            for n, arg in check_call(toks):
                key = (os.path.realpath(loc.file.name), arg[0].extent.start.offset)
                if key in seen:
                    continue
                seen.add(key)
                found.append((key[0], key[1], arg[-1].extent.end.offset,
                              arg[0].extent.start.line, name, n,
                              " ".join(t.spelling for t in arg)))
    return found


## @brief Describe each argument needing parentheses.
#
# @param found find_in_source()'s results.
# @return Problems, as "file:line: message" strings.
def problems_of(found):
    return [f"{os.path.relpath(f, GLOBALS.ROOT)}:{line}: {name}() argument {n} "
            f"'{text}' is an expression: write it in parentheses"
            for f, _, _, line, name, n, text in found]


## @brief Check one source, as check-globals.py's check_source() does.
#
# @param source Source file.
# @param args Compiler arguments for libclang (libclang_args()).
# @param seen Locations already reported; updated.
# @return Problems found.
def check_source(source, args, seen):
    return problems_of(find_in_source(source, args, seen))


## @brief Wrap the found arguments in parentheses, in their files.
#
# @param found find_in_source()'s results.
def fix(found):
    by_file = {}
    for f, start, end, *_ in found:
        by_file.setdefault(f, []).append((start, end))
    for f, spans in by_file.items():
        with open(f, "rb") as fh:
            data = fh.read()
        edits = sorted([(s, b"(") for s, _ in spans] + [(e, b")") for _, e in spans],
                       key=lambda x: x[0], reverse=True)
        for off, text in edits:
            data = data[:off] + text + data[off:]
        with open(f, "wb") as fh:
            fh.write(data)


## @brief Find the arguments needing parentheses in a build's sources.
#
# @param build_dir Build directory with compile_commands.json.
# @return find_in_source()'s results for all of them.
def find_in_build(build_dir):
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as f:
        entries = json.load(f)
    found, seen = [], set()
    for e in entries:
        source = os.path.realpath(os.path.join(e["directory"], e["file"]))
        if not GLOBALS.ours(source) or not source.endswith(".c"):
            continue
        cmd = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        found += find_in_source(source, libclang_args(cmd, source), seen)
    return found


## @brief Check, or with --fix repair, a build directory's sources.
#
# @return Exit status: 0 if every expression argument is parenthesised
#         (or, with --fix, now is).
def main():
    argv = sys.argv[1:]
    do_fix = "--fix" in argv
    dirs = [a for a in argv if a != "--fix"]
    if len(dirs) != 1:
        print(__doc__)
        return 2
    found = find_in_build(dirs[0])
    if do_fix:
        fix(found)
        print(f"Parenthesised {len(found)} macro argument(s)")
        return 0
    for p in problems_of(found):
        print(p)
    print(f"{len(found)} macro argument(s) to parenthesise" if found
          else "All expression macro arguments are parenthesised")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
