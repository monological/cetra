"""The gate-docs group: gate docstrings (the documented arm list is the one that runs)."""

import ast
import glob
import inspect
import os
import re

from gatelib.harness import SCRIPTS


# A line-initial two-space name, which is the shape an arm's printed line and a
# docstring's arm list BOTH use -- one pattern, because the check is that they agree.
# inspect.getdoc, not __doc__: Python 3.13 strips a docstring's common indentation at
# compile time and earlier versions do not, so the raw attribute puts the arm list at a
# different column depending on the interpreter.
_ARM_NAME = r"([a-z][a-z0-9]*(?:-[a-z0-9]+)*)"
_ARM_DOCUMENTED = re.compile(r"^  " + _ARM_NAME + r"(?=\s)", re.M)
# The verdict line specifically, not any line an arm prints. A SKIP or an ERROR prelude
# carries the same name, and several are emitted from a guard ABOVE the arm they belong
# to -- reading those as the arm's position reports a false order mismatch.
_ARM_PRINTED = re.compile(r'print\(f?"  ' + _ARM_NAME + r'[^"]*PASS')
# The same shape at ANY other indent. Only consulted when the strict form found nothing,
# and only against names the function is known to PRINT -- prose is full of lines that
# begin with a word and a space, so an indent-only test reports every docstring in the
# file. See run_gate_docs_gate for what it is telling apart.
_ARM_ASTRAY = re.compile(r"^ *" + _ARM_NAME + r"(?= +\S)", re.M)


def run_gate_docs_gate(workdir):
    """A group's documented arm list is the list it actually runs.

      gate-arm-docs   every group whose docstring lists its arms lists all of them, in
                      run order. Static -- it reads the source rather than running
                      anything, so it covers all groups whatever --only selected, and
                      costs no render.
      gate-names      no top-level name in any of the suite's files is bound twice, by a
                      definition or an import: scripts/gates.py and everything under
                      scripts/gatelib. Python lets the second binding replace the first
                      without a word, so a group whose helper shares a name with another's,
                      or with one it imports, runs against whichever came last in its file.

    A docstring naming a different set than the function runs is what a reviewer reads to
    decide what is covered, which makes a stale one worse than none. This is not a
    hypothetical: run_water_gate documented 25 arms while running 29, and the four it
    omitted were shoaling, the shore foam band, the crack test and the draw count. Eleven
    more were listed in an order the function had stopped following.

    A group that lists no arms is not required to start -- only a list that EXISTS is held
    to being true, so this adds no documentation debt to the 30 groups without one. A
    group that lists arms but builds their names at runtime cannot be read this way, and
    is reported as unverifiable rather than quietly passed or wrongly failed.
    """
    del workdir # static: nothing is rendered and nothing is written
    # Here and not at the top: the table imports this module, so it is complete only once the
    # gates run.
    from gatelib.groups import GATE_GROUPS
    problems, checked, listless = [], 0, 0
    for selector, _banner, fn in GATE_GROUPS:
        doc = inspect.getdoc(fn) or ""
        documented = _ARM_DOCUMENTED.findall(doc)
        printed, seen = [], set()
        for name in _ARM_PRINTED.findall(inspect.getsource(fn)):
            if name not in seen:
                seen.add(name)
                printed.append(name)
        if not documented:
            # A list at the WRONG INDENT reads as no list at all -- the silent-pass
            # direction, since the group then looks like one that never claimed anything.
            # Easy to land on: getdoc strips the COMMON indent, so a docstring whose
            # every line is indented equally delivers its arm names at column 0 however
            # deeply they were written, and indenting the block FURTHER does not help.
            # It needs a shallower prose line to anchor the strip. Two groups shipped
            # this way, one of them the arm list this check exists to hold.
            #
            # Tested against names the function actually PRINTS rather than by indent
            # alone: prose is full of lines beginning with a word and a space, so the
            # looser test reports every docstring in the file.
            astray = [n for n in _ARM_ASTRAY.findall(doc) if n in seen]
            if astray:
                problems.append(f"{selector}: names arms {sorted(set(astray))} but at an "
                                "indent this cannot verify -- the list wants a shallower "
                                "prose line to anchor getdoc's common-indent strip")
                continue
            listless += 1
            continue
        if not printed:
            problems.append(f"{selector}: lists arms but names none in a literal")
            continue
        checked += 1
        undocumented = [n for n in printed if n not in set(documented)]
        stale = [n for n in documented if n not in set(printed)]
        if undocumented or stale:
            problems.append(f"{selector}: undocumented {undocumented}, documented but "
                            f"not run {stale}")
        elif documented != printed:
            at = next(i for i in range(len(documented)) if documented[i] != printed[i])
            problems.append(f"{selector}: same set, but the list leaves run order at #{at} "
                            f"({documented[at]} listed where {printed[at]} runs)")
    ok = not problems
    detail = (f"{checked} groups list their arms and match; {listless} list none"
              if ok else "; ".join(problems))
    print(f"  gate-arm-docs {'PASS' if ok else 'FAIL'}  {detail}")
    failures = [] if ok else ["gate-arm-docs"]

    # Spec 13.24 met exactly that: the lighting-stream group's run helper took the name the
    # terrain-stream group's already had, terrain-stream called it with its own arguments and
    # stopped the whole suite on a TypeError, and no scoped run reached both groups to see it.
    # Split into files (spec 14.29), two groups' helpers no longer share a namespace, but a file
    # that imports a name it also defines, or imports one name from two modules, is the same
    # replacement inside one file.
    paths = [os.path.join(SCRIPTS, "gates.py")] + sorted(
        glob.glob(os.path.join(SCRIPTS, "gatelib", "**", "*.py"), recursive=True))
    count, twice = 0, []
    for path in paths:
        with open(path) as f:
            tree = ast.parse(f.read(), path)
        lines = {}
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                names = [node.name]
            elif isinstance(node, ast.Assign):
                names = [n.id for t in node.targets for n in ast.walk(t)
                         if isinstance(n, ast.Name)]
            elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
                names = [node.target.id]
            elif isinstance(node, (ast.Import, ast.ImportFrom)):
                names = [(a.asname or a.name).split(".")[0] for a in node.names]
            else:
                names = []
            for name in names:
                lines.setdefault(name, []).append(node.lineno)
        count += len(lines)
        rel = os.path.relpath(path, SCRIPTS)
        twice += [f"{name} at {rel}:{at}" for name, at in sorted(lines.items()) if len(at) > 1]
    ok = not twice
    print(f"  gate-names    {'PASS' if ok else 'FAIL'}  {count} top-level names in {len(paths)} "
          "files, " + ("each bound once" if ok else "bound twice: " + ", ".join(twice)))
    if not ok:
        failures.append("gate-names")
    return failures
