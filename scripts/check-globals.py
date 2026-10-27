#!/usr/bin/env python3
# Copyright (c) 2026 Rob McKay
# SPDX-License-Identifier: Apache-2.0
"""Check that functions document the global state they change.

docs/CODING.md, section 1: a function that itself changes a variable with
static storage (file scope, extern, or a static local), or sets errno,
names each one: a static function in a "@par Global state" section of its
Doxygen comment, a public one (documented in its header) in a "Global
state:" note in the plain comment at its definition, since the variables
are the implementation's own. What counts as a change:

- an assignment, compound assignment, ++ or -- whose target is the
  variable, a member of it or an element of it;
- passing its address, or the array itself, to a parameter that points to
  non-const (memset(), k_sem_give(), ...);
- for errno, "errno =" in the function's own code.

Mutexes and spinlocks are left out: locking and unlocking leaves them as
found, as waiting on a condition variable does. Changes made through a
pointer parameter are the caller's state (@param[out]), and those made by
called functions are theirs.

Python functions are held to the same rule, in their "##" comment: a
change is an assignment to a name declared "global", or an assignment
into, or a mutating method call on (append, update, ...), a module-level
name the function doesn't define itself.

The build runs this on each of the repository's C sources as it compiles
(scripts/clang-tidy-cross.py), with the compile command; scripts/test.sh
runs it on the Python. By hand:

    scripts/check-globals.py <build dir>     every repository C source in
                                             its compile_commands.json
    scripts/check-globals.py --python        every Python file git tracks
"""

import ast

import json
import os
import re
import shlex
import subprocess
import sys

import importlib.util

import clang.cindex as ci

## Repository root: only functions in files under it are checked.
ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))

## Directories under ROOT that aren't the repository's own code.
NOT_OURS = ("build", "out")

## Types of objects that locking leaves as found.
LOCK_TYPES = re.compile(r"\bstruct k_(spinlock|mutex)\b")

## Calls that wait on an object and leave it as found.
WAIT_ONLY = {"k_condvar_wait"}

ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}

K = ci.CursorKind


## @brief Tell whether a file is the repository's own code.
#
# @param path File path, or None.
# @return True for a file under ROOT, outside the build directories.
def ours(path):
    if not path:
        return False
    rel = os.path.relpath(os.path.realpath(path), ROOT)
    return not rel.startswith("..") and rel.split(os.sep)[0] not in NOT_OURS


## @brief Tell whether a variable has static storage.
#
# @param var A VAR_DECL cursor.
# @return True for file scope, extern and static local variables.
def static_storage(var):
    if var.storage_class in (ci.StorageClass.STATIC, ci.StorageClass.EXTERN):
        return True
    return var.semantic_parent is not None and \
        var.semantic_parent.kind == K.TRANSLATION_UNIT


## @brief Strip parentheses and implicit conversions from an expression.
#
# @param expr Expression cursor.
# @return The expression inside.
def strip(expr):
    while expr.kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR, K.CSTYLE_CAST_EXPR):
        children = list(expr.get_children())
        if not children:
            break
        expr = children[-1]
    return expr


## @brief Tell whether a member access goes through a pointer ("->").
#
# @param member A MEMBER_REF_EXPR cursor.
# @return True for "->".
def is_arrow(member):
    return any(t.spelling == "->" for t in member.get_tokens())


## @brief Find the static-storage variable an lvalue is part of.
#
# Follows "." members and array elements to the variable; stops at "->"
# and "*", whose object is wherever the pointer points.
#
# @param expr Lvalue expression cursor.
# @return The VAR_DECL cursor, or None.
def root_var(expr):
    expr = strip(expr)
    while True:
        if expr.kind == K.MEMBER_REF_EXPR:
            if is_arrow(expr):
                return None
            children = list(expr.get_children())
            if not children:
                return None
            expr = strip(children[0])
        elif expr.kind == K.ARRAY_SUBSCRIPT_EXPR:
            base = strip(next(expr.get_children()))
            if base.type.kind == ci.TypeKind.POINTER:
                return None
            expr = base
        elif expr.kind == K.DECL_REF_EXPR:
            var = expr.referenced
            if var is not None and var.kind == K.VAR_DECL and static_storage(var):
                return var
            return None
        else:
            return None


## @brief Tell whether a parameter type lets the callee change what it
#         points to.
#
# @param ptype Parameter type.
# @return True for a pointer to non-const.
def writable_pointer(ptype):
    ptype = ptype.get_canonical()
    return ptype.kind == ci.TypeKind.POINTER and \
        not ptype.get_pointee().is_const_qualified()


## @brief Find the variable whose address an argument passes.
#
# @param arg Argument expression cursor.
# @return The VAR_DECL cursor for "&var..." or an array "var", or None.
def addressed_var(arg):
    arg = strip(arg)
    if arg.kind == K.UNARY_OPERATOR:
        tokens = [t.spelling for t in arg.get_tokens()]
        if tokens and tokens[0] == "&":
            return root_var(next(arg.get_children()))
        return None
    var = root_var(arg)
    if var is not None and var.type.get_canonical().kind in (
            ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
        return var
    return None


## @brief Find the variables a call can change through its arguments.
#
# @param call A CALL_EXPR cursor.
# @return VAR_DECL cursors.
def call_changes(call):
    callee = call.referenced
    if callee is None or callee.spelling in WAIT_ONLY:
        return []
    ftype = callee.type.get_canonical()
    if ftype.kind != ci.TypeKind.FUNCTIONPROTO:
        return []
    ptypes = list(ftype.argument_types())
    changed = []
    for i, arg in enumerate(call.get_arguments()):
        if i < len(ptypes) and writable_pointer(ptypes[i]):
            if LOCK_TYPES.search(arg.type.get_canonical().get_pointee().spelling):
                continue  # a lock, even a member of a struct
            var = addressed_var(arg)
            if var is not None:
                changed.append(var)
    return changed


## @brief Find the variable an assignment or increment changes.
#
# @param node An operator cursor.
# @return The VAR_DECL cursor, or None.
def operator_change(node):
    children = list(node.get_children())
    if not children:
        return None
    if node.kind in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR):
        if operator_spelling(node, children[0]) in ASSIGN_OPS:
            return root_var(children[0])
        return None
    if node.kind == K.UNARY_OPERATOR:
        tokens = [t.spelling for t in node.get_tokens()]
        if tokens and (tokens[0] in ("++", "--") or tokens[-1] in ("++", "--")):
            return root_var(children[0])
    return None


## @brief Find the operator between a binary expression's operands.
#
# @param node A binary operator cursor.
# @param lhs Its left operand.
# @return The operator's spelling.
def operator_spelling(node, lhs):
    lhs_tokens = len(list(lhs.get_tokens()))
    tokens = [t.spelling for t in node.get_tokens()]
    return tokens[lhs_tokens] if lhs_tokens < len(tokens) else ""


## @brief Collect the static-storage variables a function changes itself.
#
# @param fn A FUNCTION_DECL definition cursor.
# @return Variable names, in order of first change.
def changed_globals(fn):
    names = []
    for node in fn.walk_preorder():
        if node.kind == K.CALL_EXPR:
            changed = call_changes(node)
        elif node.kind in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR,
                           K.UNARY_OPERATOR):
            var = operator_change(node)
            changed = [var] if var is not None else []
        else:
            continue
        for var in changed:
            if not LOCK_TYPES.search(var.type.spelling) and var.spelling not in names:
                names.append(var.spelling)
    return names


## @brief Tell whether a function assigns errno in its own code.
#
# @param fn A FUNCTION_DECL definition cursor.
# @return True for "errno =".
def sets_errno(fn):
    tokens = [t.spelling for t in fn.get_tokens()]
    return any(tokens[i] == "errno" and tokens[i + 1] == "="
               for i in range(len(tokens) - 1))


## @brief Get the plain comment just before a definition.
#
# @param fn A FUNCTION_DECL definition cursor.
# @return The comment's text, or "" if there is none.
def comment_before(fn):
    with open(fn.extent.start.file.name, encoding="utf-8") as f:
        lines = f.read().split("\n")
    end = fn.extent.start.line - 2
    while end >= 0 and not lines[end].strip():
        end -= 1
    if end < 0 or not lines[end].rstrip().endswith("*/"):
        return ""
    start = end
    while start > 0 and "/*" not in lines[start]:
        start -= 1
    return "\n".join(lines[start:end + 1])


## @brief Get what a function says about the global state it changes.
#
# @param fn A function definition cursor.
# @return The "@par Global state" section of its Doxygen comment, or the
#         "Global state:" note in the comment at its definition; None if
#         it has neither.
def global_state_doc(fn):
    m = re.search(r"@par Global state(.*?)(?=\n\s*\*\s*@(?![cpe]\b)|\*/|$)", fn.raw_comment or "",
                  re.S)
    if m:
        return m.group(1)
    m = re.search(r"Global state:(.*?)(?=\*/|$)", comment_before(fn), re.S)
    return m.group(1) if m else None


## @brief Check one function definition.
#
# @param fn A FUNCTION_DECL definition cursor in the repository's code.
# @return Problems found, as "file:line: message" strings.
def check_function(fn):
    changed = changed_globals(fn)
    if sets_errno(fn):
        changed.append("errno")
    if not changed:
        return []
    where = f"{os.path.relpath(fn.location.file.name, ROOT)}:{fn.location.line}: " \
            f"{fn.spelling}()"
    doc = global_state_doc(fn)
    if doc is None:
        return [f"{where}: changes {', '.join(changed)}: no '@par Global state' section "
                f"(or 'Global state:' note at a public definition)"]
    missing = [n for n in changed if not re.search(rf"\b{re.escape(n)}\b", doc)]
    if missing:
        return [f"{where}: '@par Global state' doesn't name {', '.join(missing)}"]
    return []


## @brief Parse a source and check the functions defined in our files.
#
# @param source Source file.
# @param args Compiler arguments, without the compiler and the source.
# @param seen Locations already checked (a header's inline functions are
#        parsed with every source that includes it); updated.
# @return Problems found.
def check_source(source, args, seen):
    index = ci.Index.create()
    tu = index.parse(source, args=args)
    fatal = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
    if fatal:
        return [f"{source}: can't parse: {fatal[0].spelling}"]
    problems = []
    for fn in tu.cursor.walk_preorder():
        if fn.kind != K.FUNCTION_DECL or not fn.is_definition():
            continue
        loc = fn.location
        if loc.file is None or not ours(loc.file.name):
            continue
        # By name too: a macro such as ZTEST defines two functions on one line
        key = (os.path.realpath(loc.file.name), loc.line, fn.spelling)
        if key in seen:
            continue
        seen.add(key)
        problems += check_function(fn)
    return problems


## @brief Turn a compile command into libclang arguments.
#
# Drops the compiler, the output and the source, and adds what the build's
# compiler finds on its own: the C library's headers (a cross compiler)
# and the built-in headers (the host clang's, matching libclang).
#
# @param cmd The command's words: compiler first.
# @param source The source it compiles.
# @return Arguments for libclang.
def libclang_args(cmd, source):
    spec = importlib.util.spec_from_file_location(
        "tidy_cross", os.path.join(os.path.dirname(os.path.realpath(__file__)),
                                   "clang-tidy-cross.py"))
    tidy = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tidy)
    args = tidy.clang_command(cmd)[1:]
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-c", source) or os.path.realpath(a) == os.path.realpath(source):
            continue
        out.append(a)
    resource = subprocess.run(["clang", "-print-resource-dir"], capture_output=True,
                              text=True).stdout.strip()
    if resource:
        out += ["-resource-dir", resource]
    return out


## @brief Check every repository source in a build's compile database.
#
# @param build_dir Build directory with compile_commands.json.
# @return Problems found.
def check_build(build_dir):
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as f:
        entries = json.load(f)
    problems, seen = [], set()
    for e in entries:
        source = os.path.realpath(os.path.join(e["directory"], e["file"]))
        if not ours(source) or not source.endswith(".c"):
            continue
        cmd = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        problems += check_source(source, libclang_args(cmd, source), seen)
    return problems


## Methods that change the object they are called on.
MUTATORS = {"append", "extend", "insert", "update", "clear", "pop", "popitem", "remove",
            "add", "discard", "setdefault", "sort", "reverse"}


## @brief Find the name at the root of a target or call receiver.
#
# @param node An expression node: Name, or Subscript or Attribute on one.
# @return The name, or None for anything else.
def py_root(node):
    while isinstance(node, (ast.Subscript, ast.Attribute)):
        node = node.value
    return node.id if isinstance(node, ast.Name) else None


## @brief List the names an assignment target binds.
#
# @param target Target node.
# @return Names bound: a bare name, or those in a tuple or list; not the
#         names inside a subscript or attribute target, which it changes.
def bound_names(target):
    if isinstance(target, ast.Name):
        return [target.id]
    if isinstance(target, (ast.Tuple, ast.List)):
        return [n for e in target.elts for n in bound_names(e)]
    return []


## @brief List the names a Python function binds itself: its parameters
#         and the plain names it assigns, less those declared global.
#
# @param fn A FunctionDef node.
# @param declared Names the function declares global.
# @return The local names.
def py_local_names(fn, declared):
    local = {a.arg for a in fn.args.args + fn.args.kwonlyargs}
    for node in ast.walk(fn):
        if isinstance(node, ast.Assign):
            targets = node.targets
        elif isinstance(node, (ast.AnnAssign, ast.For)):
            targets = [node.target]
        elif isinstance(node, ast.With):
            targets = [i.optional_vars for i in node.items if i.optional_vars is not None]
        else:
            continue
        local |= {n for t in targets for n in bound_names(t)}
    return local - declared


## @brief Find the name an assignment target changes.
#
# @param target Target node.
# @param declared Names the function declares global.
# @return A plain name if declared global; for a subscript or attribute,
#         the name it is rooted at; else None.
def py_target_root(target, declared):
    if isinstance(target, ast.Name):
        return target.id if target.id in declared else None
    return py_root(target)


## @brief Find the names a statement or call may change.
#
# @param node An AST node.
# @param declared Names the function declares global.
# @return The roots of its assignment targets (a plain name only if
#         declared global), or of a mutating method call's receiver.
def py_change_roots(node, declared):
    if isinstance(node, (ast.Assign, ast.AugAssign, ast.AnnAssign)):
        targets = node.targets if isinstance(node, ast.Assign) else [node.target]
        return [py_target_root(t, declared) for t in targets]
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and \
            node.func.attr in MUTATORS:
        return [py_root(node.func.value)]
    return []


## @brief Collect the module-level names a Python function changes.
#
# @param fn A FunctionDef node.
# @param module_names Names assigned at module level.
# @return Names, in order of first change.
def py_changed_globals(fn, module_names):
    declared = {n for node in ast.walk(fn) if isinstance(node, ast.Global) for n in node.names}
    local = py_local_names(fn, declared)
    changed = []
    for node in ast.walk(fn):
        for r in py_change_roots(node, declared):
            if r and (r in declared or (r in module_names and r not in local)) and \
                    r not in changed:
                changed.append(r)
    return changed


## @brief Get the "##" comment block just above a Python function.
#
# @param lines The file's lines.
# @param fn A FunctionDef node.
# @return The block's text.
def py_comment(lines, fn):
    first = min([fn.lineno] + [d.lineno for d in fn.decorator_list]) - 2
    block = []
    while first >= 0 and lines[first].lstrip().startswith("#"):
        block.insert(0, lines[first])
        first -= 1
    return "\n".join(block)


## @brief Check one Python file's functions.
#
# @param path File.
# @return Problems found, as "file:line: message" strings.
def check_python_file(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    tree = ast.parse(text)
    lines = text.split("\n")
    module_names = {t.id for node in tree.body if isinstance(node, (ast.Assign, ast.AnnAssign))
                    for t in (node.targets if isinstance(node, ast.Assign) else [node.target])
                    if isinstance(t, ast.Name)}
    problems = []
    for fn in ast.walk(tree):
        if not isinstance(fn, ast.FunctionDef):
            continue
        changed = py_changed_globals(fn, module_names)
        if not changed:
            continue
        where = f"{os.path.relpath(path, ROOT)}:{fn.lineno}: {fn.name}()"
        m = re.search(r"@par Global state(.*?)(?=\n\s*#\s*@(?![cpe]\b)|$)", py_comment(lines, fn),
                      re.S)
        if m is None:
            problems.append(f"{where}: changes {', '.join(changed)}: no '@par Global state' "
                            f"section")
            continue
        missing = [n for n in changed if not re.search(rf"\b{re.escape(n)}\b", m.group(1))]
        if missing:
            problems.append(f"{where}: '@par Global state' doesn't name {', '.join(missing)}")
    return problems


## @brief Check every Python file git tracks.
#
# @return Problems found.
def check_python():
    out = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "*.py"], cwd=ROOT,
                         check=True, capture_output=True, text=True).stdout
    problems = []
    for p in out.split():
        problems += check_python_file(os.path.join(ROOT, p))
    return problems


## @brief Check a build directory, or the Python, as the command line says.
#
# @return Exit status: 0 if every function documents its changes.
def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    problems = check_python() if sys.argv[1] == "--python" else check_build(sys.argv[1])
    for p in problems:
        print(p)
    print(f"{len(problems)} function(s) with undocumented global state" if problems
          else "All changes to global state documented")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
