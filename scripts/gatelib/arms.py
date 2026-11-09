"""Choosing which of a group's arms run (spec 14.29): the arms a group can be asked for, the ones
--arms chose, and the decorator that runs a chosen arm and prints its verdict."""

import ast
import functools
import inspect
import re
import textwrap


# An arm's name, which an arm's verdict line and a docstring's arm list BOTH begin with, two
# spaces in -- one pattern, because gate-docs' check is that they agree.
ARM_NAME = r"([a-z][a-z0-9]*(?:-[a-z0-9]+)*)"
# The verdict line specifically, not any line an arm prints. A SKIP or an ERROR prelude
# carries the same name, and several are emitted from a guard ABOVE the arm they belong
# to -- reading those as the arm's position reports a false order mismatch.
ARM_PRINTED = re.compile(r'print\(f?"  ' + ARM_NAME + r'[^"]*PASS')

_chosen = None  # the arms --arms chose; None runs every arm
_ran = set()


class Arms:
    """A group's arms, each a function under `@arm("name")` where `arm = Arms(width)`. A chosen
    arm runs as it is defined; the `(ok, detail)` it returns is printed as its verdict line, the
    name in a column `width` wide, and a failure kept in `failed`. `ok` None is a SKIP. Each arm
    being a function, one that reads another's names fails on every run, not only when it runs
    alone."""

    def __init__(self, width):
        self.width = width
        self.failed = []

    def __call__(self, name):
        def run(fn):
            if _chosen is None or name in _chosen:
                _ran.add(name)
                ok, detail = fn()
                verdict = "SKIP" if ok is None else "PASS" if ok else "FAIL"
                print(f"  {name:<{self.width}} {verdict}  {detail}")
                if ok is not None and not ok:
                    self.failed.append(name)
            return fn
        return run


@functools.cache
def group_source(fn):
    """A group's function's source, read once a run however often it is asked for."""
    return inspect.getsource(fn)


@functools.cache
def _parsed(fn):
    return ast.parse(textwrap.dedent(group_source(fn))).body[0]


def _arm_name(stmt):
    """The arm a statement defines, when it is a function under `@arm("name")`; else None."""
    for d in getattr(stmt, "decorator_list", ()):
        if (isinstance(d, ast.Call) and isinstance(d.func, ast.Name) and d.func.id == "arm"
                and d.args and isinstance(d.args[0], ast.Constant)):
            return d.args[0].value
    return None


def guards(fn):
    """Whether a group's arms are functions under @arm, so it can run some and not others."""
    return any(_arm_name(s) is not None for s in _parsed(fn).body)


def arms_printed(fn):
    """A group's arms in run order, the names --arms can choose: its decorators' in a group that
    guards, else read from its verdict lines. A name built at run time cannot be read."""
    names = [n for n in map(_arm_name, _parsed(fn).body) if n]
    if names:
        return names
    for name in ARM_PRINTED.findall(group_source(fn)):
        if name not in names:
            names.append(name)
    return names


def select(groups, wanted):
    """The arms `wanted` chooses, by group selector. An arm's whole name chooses that arm alone,
    or ui-layout could never run without ui-layout-resize; anything else chooses every arm whose
    name contains it."""
    named = {selector: arms_printed(fn) for selector, _, fn in groups}
    every = {n for names in named.values() for n in names}

    def picked(n):
        return any(n == w if w in every else w in n for w in wanted)

    return {s: hits for s, names in named.items() if (hits := [n for n in names if picked(n)])}


def choose(names):
    """Runs only these arms from here on, in every group that guards its arms."""
    global _chosen
    _chosen = set(names)


def unrun(groups, chosen):
    """The chosen arms a guarding group never ran: a function its group never reached."""
    return [n for selector, _, fn in groups if selector in chosen and guards(fn)
            for n in chosen[selector] if n not in _ran]
