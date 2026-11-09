"""Which arms a run chose, and which of them ran (spec 14.29)."""

import ast
import inspect
import re
import textwrap


# An arm's name, which an arm's printed line and a docstring's arm list BOTH begin with, two
# spaces in -- one pattern, because gate-docs' check is that they agree.
_ARM_NAME = r"([a-z][a-z0-9]*(?:-[a-z0-9]+)*)"
# The verdict line specifically, not any line an arm prints. A SKIP or an ERROR prelude
# carries the same name, and several are emitted from a guard ABOVE the arm they belong
# to -- reading those as the arm's position reports a false order mismatch.
_ARM_PRINTED = re.compile(r'print\(f?"  ' + _ARM_NAME + r'[^"]*PASS')

_chosen = None  # the arms --arms chose; None runs every arm
_ran = set()


def arms_printed(fn):
    """A group's arms, in the order it prints their verdicts: the names --arms can choose."""
    out = []
    for name in _ARM_PRINTED.findall(inspect.getsource(fn)):
        if name not in out:
            out.append(name)
    return out


def guard_names(stmt):
    """The arms a statement guards, when it is `if arm(...)` with names spelt out; else None."""
    t = stmt.test if isinstance(stmt, ast.If) else None
    if not (isinstance(t, ast.Call) and isinstance(t.func, ast.Name) and t.func.id == "arm"):
        return None
    return [a.value for a in t.args if isinstance(a, ast.Constant) and isinstance(a.value, str)]


def group_body(fn):
    """A group's source and its function's statements, parsed."""
    src = textwrap.dedent(inspect.getsource(fn))
    return src, ast.parse(src).body[0]


def guards(fn):
    """Whether a group puts its arms under arm(), so it can run some and not others. Read from
    the code, not its text, which a docstring could mention arm() in."""
    _, node = group_body(fn)
    return any(guard_names(s) is not None for s in node.body)


def choose(names):
    """Runs only these arms from here on, in every group that guards its arms."""
    global _chosen
    _chosen = set(names)


def arm(*names):
    """Whether to run the block below: true when no arms were chosen, or when any of `names` was.
    Arms that print from one run name each other here and run together."""
    if _chosen is not None and not _chosen.intersection(names):
        return False
    _ran.update(names)
    return True


def arms_ran():
    """Every arm a guard let run."""
    return set(_ran)
