"""The ui group: the game UI layer (layout, navigation, capture, settings; spec 12.2)."""

import functools
import math
import re
import os
import random
import shutil
import subprocess
import sys

import numpy as np

from gatelib.arms import Arms
from gatelib.harness import ROOT, SCRIPTS
from gatelib.images import _ppm_array
from gatelib.probes import (
    _canvas_run, _canvas_shot, _gametest_probe, _gametest_probe_text, _scribbled,
)


# "ui <case> <label> <key> <numbers...>" from gametest --ui-probe: the four-field
# shape _gametest_probe reads, which the anim, ik and save probes print too. Audio
# does NOT -- it carries a literal rms token and a fixed pair of numbers -- and keeps
# its own reader beside that helper.
_UI_PROBE = re.compile(r"^ui ([\w-]+) (\w+) (\w+)((?:\s+-?[\d.]+)+)$", re.M)

# The element constructors, counted by their FIRST PARAMETER and not by return
# type: ui_screen_root also returns a UIElement*, so counting returns makes the
# closed list seven and turns the arm below into a magic number.
_UI_CTOR = re.compile(r"^UIElement\* (ui_\w+)\(UIElement\* parent", re.M)

_UI_HEADER = os.path.join(ROOT, "cetra", "src", "ui.h")

# What ui.h is allowed to export, in its order. Spelled out rather than counted, so
# adding an element fails by NAME and the failure says which one arrived. The pane and
# the field are spec 14.2's, which argued for them, the code editor spec 14.4's and the
# table spec 14.30's.
_UI_KINDS = ["ui_panel", "ui_label", "ui_button", "ui_toggle", "ui_slider", "ui_selector",
             "ui_scroll", "ui_table", "ui_text_field", "ui_editor"]


def _ui_probe(case, env=None):
    """{(label, key): [numbers]} from one gametest --ui-probe run, or None.

    No window, no GPU and no pad: every case here is layout, measurement or a
    plain struct of input values, which is what lets the group run inside the
    suite's CPU budget.
    """
    return _gametest_probe("--ui-probe", _UI_PROBE, case, env=env)


# ---- The vim mode held to vim itself (spec 14.5)

# Vim, where the oracle's arms find it; they skip without it.
_VIM = shutil.which("vim")

# The indent and format rules the layout oracle sources on vim's side (spec 14.9).
_VIM_LAYOUT = os.path.join(SCRIPTS, "vim_layout.vim")

# The settings the vim mode keeps: autoindent, spaces for a tab at the text's width of 4, a search
# that ignores case unless the query has a capital, Backspace over anything, and J one space.
_VIM_SET = ("set nocompatible autoindent expandtab tabstop=4 shiftwidth=4 softtabstop=0 "
            "ignorecase smartcase backspace=indent,eol,start nojoinspaces")

# The texts the oracle runs on: pcb's own shape, the same with CRLF, words and punctuation with
# blank lines and tabs, and letters past ASCII. None lacks a final line break: vim keeps whether a
# file had one apart from its lines, and once such a text's last line is emptied, the bytes alone
# no longer say whether an empty line follows (the spec's "what differs from vim").
_VIM_FIXTURES = {
    "sexpr": (b"(block memory-bank (count)\n"
              b"  (port addr :width 17)\n"
              b"  (port dq :width (* count 16))\n"
              b"\n"
              b"  ; one chip a lane\n"
              b"  (for (i 0 count)\n"
              b"    (inst (mem i) mt40a512m16\n"
              b"      (connect A addr)\n"
              b"      (connect DQ (slice dq (* i 16) 16)))))\n"),
    "crlf": (b"(part u1 \"lm317\"\r\n"
             b"  (pin 1 adj)\r\n"
             b"\r\n"
             b"  (pin 2 out) (pin 3 in))\r\n"),
    "words": (b"alpha beta-gamma delta_epsilon\n"
              b"    zeta  eta\n"
              b"\ttheta, iota; kappa.\n"
              b"\n"
              b"\n"
              b"lambda mu (nu) [xi] {omicron}\n"),
    "utf8": "café naïve résumé\nΩmega über straße\n".encode(),
}


# The fixed corpus, in vim's notation: every motion, operator and edit the vim mode has, alone, with
# counts, and in the combinations whose rules interact -- an operator over each motion, `.` after a
# change, undo and redo after one. A sequence never begins with a space, which vim would read as
# nothing; "1 " is a space.
_VIM_SEQUENCES = [
    # Motions.
    "h", "3h", "l", "4l", "j", "2j", "k", "3k", "w", "3w", "W", "2W", "b", "3b", "B", "e", "3e",
    "E", "ge", "2ge", "gE", "0", "^", "$", "2$", "gg", "G", "2G", "3gg", "+", "-", "_", "2_",
    "<CR>", "<BS>", "1 ", "3 ", "g_", "5|", "f(", "2f)", "F(", "t)", "T(", "f(;", "f(;;", "t);",
    "f(,", "%", "{", "}", "2}", "jjk", "$j", "j$k",
    # Deletes, over every kind of motion.
    "x", "3x", "X", "2X", "D", "dd", "2dd", "3dd", "9dd", "dj", "dk", "dG", "dgg", "d}", "d{",
    "dw", "2dw", "d2w", "de", "dE", "db", "dB", "dge", "d$", "d2$", "d0", "d^", "dt)", "df(",
    "dT(", "dF(", "d%", "dh", "dl", "d ", "d2l", "d_", "d2_", "d+", "d-", "d<CR>", "d<BS>",
    # Changes, which end in insert mode.
    "cwZ<Esc>", "cWZ<Esc>", "ceZ<Esc>", "ccZ<Esc>", "c$Z<Esc>", "cbZ<Esc>", "CZ<Esc>", "sZ<Esc>",
    "SZ<Esc>", "3sZ<Esc>", "c2wZ<Esc>", "cjZ<Esc>",
    # Yanks and puts.
    "yyP", "yyp", "ywP", "yep", "y$p", "yjp", "ykP", "2yyp", "ddp", "ddP", "xp", "Xp", "dwp",
    "yy3p", "yw2P", "y2wP",
    # Joins, replaces, case and shifts.
    "J", "3J", "gJ", "2gJ", "9J", "rx", "3rx", "~", "5~", "g~w", "g~~", "guw", "gUU", "gU$",
    "g~g~", "gugu", ">>", "3>>", "<<", ">j", "<k", ">G", "<gg", ">w", "3<<",
    # Inserts.
    "iZ<Esc>", "aZ<Esc>", "IZ<Esc>", "AZ<Esc>", "oZ<Esc>", "OZ<Esc>", "o<Esc>", "O<Esc>",
    "3iab<Esc>", "2ofoo<Esc>", "3aX<Esc>", "A<CR>x<Esc>", "i<CR><Esc>", "o<CR>x<Esc>",
    "i<BS><BS><Esc>", "A<BS>x<Esc>", "i<Tab>x<Esc>", "ifoo<Left>X<Esc>", "A<CR><CR>x<Esc>",
    "o<CR><Esc>", "3o<Esc>",
    # Repeats, undo and redo.
    "dw.", "dd.", "x..", "cwZ<Esc>w.", "x3.", "AZ<Esc>j.", "iab<Esc>2.", "Afoo<Esc>u",
    "ihello<Esc>u<C-r>", "dwu", "ddu", "xxu", "xxuu", "dwu<C-r>", "3dd.u", "yku", "dju", "dku",
    "cwZ<Esc>u", "Jxu", ">>u", "g~~u", "ddu<C-r>", "xpu<C-r>", "d0u", "x<Esc>u", "jxu",
    # Text objects after an operator, and repeated.
    "diw", "daw", "diW", "daW", "d2aw", "d3iw", "c2iwZ<Esc>", "di(", "da(", "di)", "di[", "da[",
    "di{", "da{", "di<", "da<", "dib", "dab", "diB", "daB", "2di(", "2da(", "ci(Z<Esc>", "yi(P",
    "ya(p", 'di"', 'da"', "di'", "da'", "di`", 'ci"Z<Esc>', "dip", "dap", "d2ap", "d2ip", "yapP",
    "cipZ<Esc>", ">ip", "gUiw", "g~aw", "guap", "diw.", "daw.", "di(u", "ciwZ<Esc>w.",
    # Visual and visual line mode: moving, switching, and every operator over the area.
    "v", "vl", "vj", "vjj", "vk", "v$", "v0", "vw", "vb", "ve", "vG", "vgg", "v<Esc>", "vj<Esc>",
    "vjo", "vjv", "vjV", "V", "Vj", "Vk", "vd", "vjd", "vkd", "v$d", "vwd", "vjjd", "vx", "vjX",
    "vjD", "Vd", "Vjd", "Vkd", "VGd", "Vggd", "vy", "vjy", "vky", "Vjy", "Vky", "vjY", "vjyP",
    "Vjyp", "vjcZ<Esc>", "VjcZ<Esc>", "vjCZ<Esc>", "vjSZ<Esc>", "vjRZ<Esc>", "vjsZ<Esc>", "vj>",
    "vj<", "Vj>", "V3>", "vj~", "vjU", "vju", "Vg~", "vjgU", "vjJ", "vJ", "VjjJ", "vjgJ", "vrx",
    "vjrx", "Vrx", "vjIZ<Esc>", "vjAZ<Esc>", "vkAZ<Esc>", "v$AZ<Esc>", "VIZ<Esc>", "VAZ<Esc>",
    "vj$AZ<Esc>", "vj0AZ<Esc>",
    # Puts over an area, repeats of a visual change, undo, and counts before v and V.
    "yiwvp", "yiwvep", "yiwveP", "yyvep", "yyVp", "yiwVp", "yiwVjp", "yyVjP", "ywvjp", "yy3Vp",
    "yiwv$p", "yiwvj$p", "yyv$p", "vepu", "yiwvepu<C-r>", "yiwvepp", "yiwvePp", "vjdj.", "vlld.",
    "Vjd.", "vj>.", "vj~j.", "vjrxj.", "VjcZ<Esc>j.", "vjcZ<Esc>j.", "vjJj.", "vjUu", "vjdu",
    "Vjdu<C-r>", "vjcZ<Esc>u", "vjrxu", "vjJu", "vj>u", "3vd", "3Vd", "2vy", "vjy2v", "vly3vd",
    "Vjy2Vd", "vjjd2.", "V2>", "vj3<",
    # Text objects in visual mode, made and grown.
    "viw", "vaw", "viW", "vaW", "v2iw", "v3aw", "viwiw", "vawaw", "viwiwd", "vawawd", "vi(", "va(",
    "vi(d", "va(d", "vi(i(", "va(a(", "vi(i(d", "vib", "vab", "vi[", "va[", "vi{", "va{", "vi<",
    "va<", "v2i(", "v2a(d", "vip", "vap", "vipd", "vapd", "vipip", "vapap", "v2ip", "v2ap",
    "vipipd", "Vip", "Vap", "Vipd", "viwy", 'vi"', 'va"', 'vi"d', 'va"d', "vi'", "viwcZ<Esc>",
    "Viw", "Viwd", "Vi(", "vjiw", "vjiwd", "vkiw", "vkiwd", "vjip", "vkap", "vlaw", "vhiw", "vhaw",
    "vbiw", "vbaw", "vkip", "vjjip",
    # Visual block mode.
    "<C-v>", "<C-v>j", "<C-v>jl", "<C-v>jjl", "<C-v>j$", "<C-v>jo", "<C-v>jlO", "<C-v>j<Esc>",
    "<C-v>jld", "<C-v>jjld", "<C-v>jlx", "<C-v>jlX", "<C-v>jD", "<C-v>jly", "<C-v>jlY",
    "<C-v>jlyP", "<C-v>jlyp", "<C-v>jlyjp", "<C-v>jly$p", "<C-v>jlcZ<Esc>", "<C-v>jCZ<Esc>",
    "<C-v>jSZ<Esc>", "<C-v>jIZ<Esc>", "<C-v>jlAZ<Esc>", "<C-v>j$AZ<Esc>", "<C-v>jjIZ<Esc>",
    "<C-v>jjlAZ<Esc>", "<C-v>jlrx", "<C-v>jl~", "<C-v>jlU", "<C-v>jlu", "<C-v>j>", "<C-v>j<",
    "<C-v>j2>", "<C-v>jJ", "<C-v>jgJ", "<C-v>jldu", "<C-v>jlIZ<Esc>u", "<C-v>jldj.",
    "<C-v>jIZ<Esc>j.", "<C-v>jlrxj.", "<C-v>jlyGp", "<C-v>jlyGP", "2<C-v>", "<C-v>jlv",
    "<C-v>jlV", "vj<C-v>", "Vj<C-v>d", "<C-v>jlyjj<C-v>jp", "<C-v>jly3p", "<C-v>jlp",
    "<C-v>jlyjj<C-v>jP", "yiw<C-v>jp", "yiw<C-v>jlP", "yy<C-v>jp", "yy<C-v>jP", "<C-v>jlpu",
    "yiw<C-v>jpu", "yiw<C-v>j2p", "yy<C-v>jPu",
    # A block's I and A that type nothing end as any insert does, not at the corner (spec 14.7).
    "<C-v>jI<Esc>", "<C-v>jlA<Esc>", "<C-v>j$A<Esc>", "<C-v>jjlA<Esc>",
    # What the random sequences found, held here where no seed has to happen on it: a closer before
    # the next opener using it up, a put from nothing undone, Esc taking an autoindent, a change
    # made again that fails, and P made again.
    "Goq) (z)<Esc>^da(", "Goq) (z)<Esc>^di(", "Goq (z)<Esc>^da(", "Jpuu", "o<Esc>k", "A<CR><Esc>k",
    "cca<CR>b<Esc>G2.", "ywwvP.p", "<C-v>2jly$p",
    # A change on an empty line, its step one with what it typed: two undos take back the change
    # before it too.
    "x4GSfoo<Esc>uu", "x4Gccfoo<Esc>uu", "x4GCfoo<Esc>uu",
    # Search, and the command line. A line let go ends its sequence: fed as typeahead, vim drops
    # the keys after it, where typed ones go on.
    "/a<CR>", "/a<CR>n", "/a<CR>N", "?a<CR>", "?a<CR>n", "?a<CR>N", "2/a<CR>", "/a<CR>3n",
    "/xyzzy<CR>", "/xyzzy<CR>x", "d/a<CR>", "d?a<CR>", "c/a<CR>Z<Esc>", "y/a<CR>P", "/e<CR>",
    "/E<CR>", "/A<CR>", "*", "#", "2*", "*n", "*N", "#n", "d*", "d#", "/e<CR>dn", "/e<CR>dN",
    "d/e<CR>.", ":3<CR>", ":$<CR>", ":0<CR>", ":99<CR>", ":2<CR>", ":1<CR>j", ":noh<CR>", "/<CR>",
    "/a<CR>/<CR>", "/a<CR>?<CR>", "v/a<CR>d", "V/a<CR>d", "v?a<CR>y", "/ab<BS><BS>e<CR>",
    "/x<C-u>e<CR>", "/ab<Esc>", "/<BS>", ":<Esc>", "w*", "b#", "$*", "0#", "3*", "/eta<CR>",
    "/éta<CR>", "/Ω<CR>", "/über<CR>", "/Über<CR>", "/ÜBER<CR>", "/(<CR>", "/)<CR>n", "d/)<CR>",
    "/a<CR>u", "/a<CR>x", ":noh<CR>n", "/a<CR>:noh<CR>n", "*:2<CR>n",
    # gd and gD with no host (spec 14.9): vim's own search for a declaration, its counts -- 1 is
    # this block only -- the search it leaves for n and N, under operators and in visual mode, and
    # from the end of the text, where every word has an earlier use. An operator over gd comes
    # first: vim's gd leaves the operator the kind of the motion before it, linewise after a j,
    # which the mode does not imitate.
    "gd", "gD", "1gd", "1gD", "3gd", "gdn", "gdN", "gDn", "gDN", "dgd", "dgD", "ygDP", "ygdp",
    "cgdZ<Esc>", "cgDZ<Esc>", "vgdd", "vgDy", "VgDd", "gd.", "xgd.", "gdx", "Ggd", "GgD", "G$gd",
    "jgd", "2jgdn", "wgd", "wgD", "<C-v>gdd",
]

# The fold oracle (spec 14.16): the host's folds, made closed by vim's :A,Bfold on vim's side and
# ui_editor_set_folds on the mode's, on the two texts long enough for them -- one fold over lines 2
# to 4, or two, 2 to 3 and 5 to 6 -- and the keys over them: moves by lines taking a closed fold as
# one, the z keys, the moves along a line, the searches and the undo that open one, the operators
# that take one whole, and the inserts and puts beside one.
_VIM_FOLD_FIXTURES = {name: _VIM_FIXTURES[name] for name in ("sexpr", "words")}
_VIM_FOLD_KEYS = [
    "j", "k", "2j", "2k", "3j", "jj", "kk", "jk", "kj", "+", "-", "<CR>", "_", "2_", "2$",
    "G", "gg", "<Down>", "<Up>",
    "zo", "zO", "zv", "zc", "za", "zR", "zM", "zoj", "zok", "zozc", "zozcj", "zaj", "zazaj",
    "zRj", "zMj", "zRzMj", "zOj", "zvj", "zcj", "zRk", "zozMj",
    "lj", "hj", "wj", "bj", "ej", "$j", "0j", "^j", "fej", "%j", "}j", "{j",
    "/e<CR>j", "/e<CR>nj", "*j", "#j",
    "dd", "2dd", "x", "X", "dj", "dk", "D", "dw", "d_", "dG", "dgg", "ddj", "jdd", "kdd",
    "yyp", "yyP", "ddp", "ddP", "jyyp", "jyyP", "cwZ<Esc>", "ccZ<Esc>", ">>", "J",
    "ddu", "dduj", "xu", "zoxzcuj", "jddu", "u",
    "iZ<Esc>j", "AZ<Esc>j", "oZ<Esc>", "OZ<Esc>", "joZ<Esc>", "jOZ<Esc>", "i<Esc>j",
    "zodd", "zoddj", "zRddj", "zozcx", "/a<CR>zcj", "?e<CR>j", "3x", "d$", "yyjp", "zMgg2dd",
    "gej", "gEj", "3|j", "g_j",
    # Visual areas take a closed fold at either end whole.
    "Vd", "Vjd", "Vkd", "vjd", "vkd", "vjjd", "Vy4jp", "v<Esc>k", "V<Esc>k", "vj<Esc>j", "vjy",
    # Where o and O leave the folds, seen by the moves after them.
    "oZ<Esc>k", "oZ<Esc>kk", "oZ<Esc>ggjj", "zooZ<Esc>zMgg2j", "OZ<Esc>zMggj", "joZ<Esc>ggjj",
    # Insert mode's arrows take a closed fold as one line.
    "i<Down>Z<Esc>", "i<Down><Down>Z<Esc>", "i<Up>Z<Esc>", "A<Down><Down>Z<Esc>",
]
# A fold of one line is never closed, as vim's 'foldminlines' has it.
_VIM_ONE_LINE_FOLD_KEYS = ["x", "D", "dw", "wj", "j", "k", "dd", "zcj", "jk", "Vd"]
_VIM_FOLD_SEQUENCES = ([f"<fold 2 4>{k}" for k in _VIM_FOLD_KEYS]
                       + [f"<fold 2 3><fold 5 6>{k}" for k in _VIM_FOLD_KEYS]
                       + [f"<fold 3 3>{k}" for k in _VIM_ONE_LINE_FOLD_KEYS])

# The layout oracle (spec 14.9): every fixture, and one indented badly on purpose -- brackets
# opening and closing over lines, tabs, comment lines the rule keeps (one with a tab, which a kept
# indent rebuilds of spaces), a line of blanks -- with the rules of _VIM_LAYOUT on both sides. =
# over every kind of range, in visual modes and with counts; the new lines o, O and Enter make;
# changes of whole lines; gq over ranges; and ., u and Ctrl+R after them.
_VIM_LAYOUT_FIXTURES = dict(_VIM_FIXTURES, crooked=(
    b"(block bad\n(port a\n      :width 4)\n   (net b)\n; kept\n\t  ; also kept\n"
    b"\t(inst (u 1)\nx))\n  \n  (tail one two three four five)\n"))
_VIM_LAYOUT_SEQUENCES = [
    "==", "=j", "=k", "=G", "=gg", "3==", "=2j", "=ip", "=ap", "=i(", "=a(", "=}", "V=", "Vj=",
    "vj=", "VG=", "<C-v>j=", "==.", "=j.", "j==j.", "==u", "=Gu", "=Gu<C-r>", "=G.",
    "oZ<Esc>", "OZ<Esc>", "o<Esc>", "O<Esc>", "o(<CR>x<Esc>", "A<CR>x<Esc>", "A(<CR>x<Esc>",
    "i<CR><Esc>", "i<CR>x<Esc>", "3oZ<Esc>", "2O(<Esc>", "oZ<Esc>u", "oZ<Esc>j.",
    "ccZ<Esc>", "SZ<Esc>", "cc<Esc>", "cjZ<Esc>", "c}Z<Esc>", "VjcZ<Esc>", "VjSZ<Esc>",
    "ccZ<Esc>j.", "3SZ<Esc>",
    "gqq", "gqj", "gqk", "gqap", "gqip", "gqG", "gqgg", "3gqq", "gqgq", "Vjgq", "vjgq", "V2jgq",
    "<C-v>jgq", "gqj.", "gqq.", "gqju", "gqju<C-r>", "gqGu",
]

# The random sequences' vocabulary: whole commands, composed at random. It leaves out what the corpus
# leaves out as known differences, and so whatever they would combine into: visual I and A, $
# inside a character or line area, a line let go; and a quote object in visual mode, which vim
# grows over an area wider than a character where the mode makes it afresh. Every insert ends in
# Esc: where vim leaves a cursor still inserting at the end of its keys is its harness's, not vim's.
_VIM_MOTIONS = ["h", "j", "k", "l", "w", "b", "e", "W", "B", "E", "ge", "0", "^", "$", "gg", "G",
                "2G", "f(", "t)", "F(", ";", ",", "%", "{", "}", "+", "-", "_", "2j", "3w", "<CR>",
                "/a<CR>", "?e<CR>", "n", "N", "*", "#"]
_VIM_AREA_MOTIONS = [m for m in _VIM_MOTIONS if m not in ("$", "<CR>")]
_VIM_OBJECTS = ["iw", "aw", "iW", "i(", "a(", "ip", "ap", 'i"']
_VIM_AREA_OBJECTS = [o for o in _VIM_OBJECTS if o != 'i"']
# What is typed in an insert. A change whose motion fails types it in normal mode instead, so it
# holds only keys the mode has: vim reads Z, q and ( as commands it does not.
_VIM_TEXTS = ["k", "ab", "x y", "é", "a<CR>b", "w<BS>", "<CR>"]


def _vim_random_command(rnd):
    """One command of the random sequences, in vim's notation, where a '<' is "<lt>"."""
    kind = rnd.randrange(10)
    if kind <= 2:
        return rnd.choice(_VIM_MOTIONS)
    if kind == 3:
        op = rnd.choice(["d", "y", "c", ">", "<lt>", "g~", "gU"])
        target = rnd.choice(_VIM_MOTIONS + _VIM_OBJECTS + ["<lt>" if op == "<lt>" else op[-1]])
        return op + target + (rnd.choice(_VIM_TEXTS) + "<Esc>" if op == "c" else "")
    if kind == 4:
        return rnd.choice(["x", "3x", "X", "D", "J", "gJ", "rx", "~", "p", "P", "u", "<C-r>", ".",
                           "dd", "yy", ">>", "Y", ":2<CR>", "2."])
    if kind == 5:
        return rnd.choice(["i", "a", "I", "A", "o", "O", "s", "S", "C", "cc", "cw"]) + \
            rnd.choice(_VIM_TEXTS) + "<Esc>"
    if kind <= 7:
        area = rnd.choice(["v", "V"]) + "".join(rnd.choice(_VIM_AREA_MOTIONS)
                                                for _ in range(rnd.randint(1, 2)))
        if rnd.random() < 0.3:
            area += rnd.choice(_VIM_AREA_OBJECTS)
        op = rnd.choice(["d", "x", "y", "c", ">", "<lt>", "~", "u", "U", "J", "rx", "p", "P",
                         "o<Esc>"])
        return area + op + (rnd.choice(_VIM_TEXTS) + "<Esc>" if op == "c" else "")
    block = "<C-v>" + "".join(rnd.choice(["j", "k", "l", "h", "w", "2j", "e", "$"])
                              for _ in range(rnd.randint(1, 3)))
    op = rnd.choice(["d", "y", "c", ">", "<lt>", "~", "U", "rx", "p", "I", "A", "D", "C"])
    return block + op + (rnd.choice(_VIM_TEXTS[:4]) + "<Esc>" if op in "cIAC" else "")


def _vim_random_cases(seed, count):
    """`count` random sequences of two to six commands from `seed`, each on one fixture at one of
    its starts: the same cases for the same seed on every run."""
    import random
    rnd = random.Random(seed)
    names = sorted(_VIM_FIXTURES)
    cases = []
    for _ in range(count):
        text = _VIM_FIXTURES[rnd.choice(names)]
        line, col = rnd.choice(_vim_starts(text))
        keys = "".join(_vim_random_command(rnd) for _ in range(rnd.randint(2, 6)))
        cases.append((text, line, col, keys))
    return cases


# What the corpus leaves out on a fixture, each a place the vim mode differs from vim on purpose:
# vim's A after an area ending on a character of several bytes inserts inside that character.
_VIM_NOT_ON = {
    "utf8": {"vjAZ<Esc>", "vkAZ<Esc>", "VAZ<Esc>", "vj0AZ<Esc>"},
}


def _vim_corpus(sequences, fixtures=None):
    """The cases a list of sequences makes: each on every fixture -- _VIM_FIXTURES unless others
    are given -- at three of its starts, turned by the sequence's place in the list so every start
    is used."""
    cases = []
    for j, keys in enumerate(sequences):
        for name, text in (fixtures or _VIM_FIXTURES).items():
            if keys in _VIM_NOT_ON.get(name, ()):
                continue
            starts = _vim_starts(text)
            for k in (0, 3, 7):
                line, col = starts[(j + k) % len(starts)]
                cases.append((text, line, col, keys))
    return cases


def _vim_starts(text):
    """Where a case starts in a text, as vim's cursor() counts: the first character, the
    middle and the last of a few lines spread through it, each on a character's first byte."""
    lines = text.decode("utf-8").replace("\r\n", "\n").rstrip("\n").split("\n")
    picks = sorted({0, 1, len(lines) // 2, len(lines) - 1})
    starts = []
    for i in picks:
        if i >= len(lines):
            continue
        cols = [0]
        for ch in lines[i][:-1]:
            cols.append(cols[-1] + len(ch.encode()))
        for at in sorted({cols[0], cols[len(cols) // 2], cols[-1]}):
            starts.append((i + 1, at + 1))
    return starts


def _vim_exe_keys(keys):
    """Keys in vim's notation as the inside of a double-quoted vim string."""
    out, i = [], 0
    while i < len(keys):
        if keys[i] == "<":
            j = keys.find(">", i)
            if j > i:
                name = keys[i + 1:j]
                out.append("<" if name == "lt" else "\\<" + name + ">")
                i = j + 1
                continue
        out.append({"\\": "\\\\", '"': '\\"'}.get(keys[i], keys[i]))
        i += 1
    return "".join(out)


def _vim_reference(case, path, read_only=False, layout=False):
    """(line, column, bytes) vim leaves `case` at -- its text, the cursor's line and column, and
    its keys -- or None when vim did not run. The text is the buffer as vim writes it, in the
    file's own line endings. The keys are fed as typed, so each command is its own step of undo
    and a key after one that failed is still read, as when a person types them; normal! would
    make every command one step and let go of the keys after a failure. `read_only` sets
    'nomodifiable' on the buffer, which is what a read-only text is held to; `layout` sources the
    indent and format rules a host's hooks answer with (spec 14.9). Keys beginning with
    "<fold A B>" have lines A to B folded first, closed, by :A,Bfold (spec 14.16)."""
    text, line, col, keys = case
    folds = re.match(r"((?:<fold \d+ \d+>)*)", keys).group(1)
    keys = keys[len(folds):]
    fold_cmds = [arg for a, b in re.findall(r"<fold (\d+) (\d+)>", folds)
                 for arg in ("-c", f"{a},{b}fold")]
    with open(path, "wb") as f:
        f.write(text)
    out, pos = path + ".out", path + ".pos"
    for p in (out, pos):
        if os.path.exists(p):
            os.remove(p)
    cmd = [_VIM, "-Nu", "NONE", "-i", "NONE", "-n", "-es", "-c", _VIM_SET,
           *(["-c", "execute 'source ' . fnameescape('" + _VIM_LAYOUT.replace("'", "''") + "')"]
             if layout else []),
           *(["-c", "set nomodifiable"] if read_only else []), *fold_cmds,
           "-c", f"call cursor({line}, {col})",
           "-c", f"call feedkeys(\"{_vim_exe_keys(keys)}\", 'xt')",
           "-c", f"call writefile([line('.') . ' ' . col('.')], '{pos}')",
           "-c", f"silent w! {out}", "-c", "qa!", path]
    try:
        subprocess.run(cmd, capture_output=True, timeout=20, stdin=subprocess.DEVNULL)
        with open(pos, "rb") as f:
            l, c = f.read().split()
        with open(out, "rb") as f:
            data = f.read()
    except (subprocess.TimeoutExpired, OSError, ValueError):
        return None
    return int(l), int(c), data


def _vim_cetra(cases, workdir, tag, read_only=False, layout=False):
    """[(line, column, bytes, emptied)] the vim mode leaves each case at, from one gametest run --
    `emptied` whether the text was emptied whole on the way; None for a case it printed nothing
    for. `read_only` makes every case's text read-only; `layout` installs the rules of
    _VIM_LAYOUT as the host's indent and format hooks."""
    path = os.path.join(workdir, f"vim_cases_{tag}.txt")
    with open(path, "w", encoding="ascii") as f:
        for text, line, col, keys in cases:
            f.write(f"{text.hex()} {line} {col} {keys.encode().hex()}\n")
    env = dict(os.environ, CETRA_VIM_CASES=path)
    if read_only:
        env["CETRA_VIM_READ_ONLY"] = "1"
    if layout:
        env["CETRA_VIM_LAYOUT"] = "1"
    out, _ = _gametest_probe_text("--ui-probe", "vim-run", env=env, timeout=1800)
    got = {}
    for m in re.finditer(r"^vim (\d+) (\d+) (\d+) ([0-9a-f]*) ([01])$", out, re.M):
        got[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), bytes.fromhex(m.group(4)),
                                m.group(5) == "1")
    return [got.get(i) for i in range(len(cases))]


def _vim_agrees(ref, got):
    """Whether the vim mode left a case where vim did. Two differences are known and let pass. Vim
    keeps a file's final line break apart from its lines, so once a text is emptied whole, the two
    may differ by that break alone. And a block shift whose left column is past its top line's end
    leaves vim's cursor past that end, in normal mode, where vim's next command finds it on the
    line's last character -- where the mode puts it at once."""
    if got is None:
        return False
    if ref == got[:3]:
        return True
    if ref[0] == got[0] and ref[2] == got[2] and got[1] >= 1:
        line = ref[2].split(b"\n")[ref[0] - 1].rstrip(b"\r")
        last = line[got[1] - 1:].decode("utf-8", "replace")
        if ref[1] == len(line) + 1 and len(last) == 1:
            return True
    brk = b"\r\n" if b"\r\n" in ref[2] + got[2] else b"\n"
    return (got[3] and ref[:2] == got[:2]
            and (ref[2] == got[2] + brk or got[2] == ref[2] + brk))


def _vim_oracle(cases, workdir, tag, read_only=False, layout=False):
    """The cases on which the vim mode and vim itself differ, as (case, vim's, the mode's), and
    how many vim ran. The mode's one run goes beside vim's many. `read_only` runs every case on a
    read-only text beside vim's 'nomodifiable'; `layout` gives both sides the rules of
    _VIM_LAYOUT."""
    from concurrent.futures import ThreadPoolExecutor
    folder = os.path.join(workdir, f"vim_{tag}")
    os.makedirs(folder, exist_ok=True)
    with ThreadPoolExecutor(max_workers=9) as pool:
        mode = pool.submit(_vim_cetra, cases, workdir, tag, read_only, layout)
        ref = list(pool.map(lambda ic: _vim_reference(ic[1], os.path.join(folder, f"{ic[0]}.txt"),
                                                      read_only, layout),
                            enumerate(cases)))
        got = mode.result()
    ran = sum(r is not None for r in ref)
    return [(c, r, g) for c, r, g in zip(cases, ref, got)
            if r is not None and not _vim_agrees(r, g)], ran


def _vim_shown(diffs, label):
    """The first three differences as an arm reports them, `label` naming each case."""
    return "; ".join(f"{label(case)}: vim {r[0]}:{r[1]}, the mode "
                     + (f"{g[0]}:{g[1]}{'' if g[2] == r[2] else ', text differs'}" if g
                        else "nothing")
                     for case, r, g in diffs[:3])


def _probe_wrong(p, want):
    """The probe keys whose first number is not the one `want` names, as {"label key": got}:
    empty when every one holds. A key the probe never printed reads as None."""
    return {f"{a} {b}": p.get((a, b), [None])[0] for (a, b), v in want.items()
            if p.get((a, b), [None])[0] != v}


def run_ui_gate(workdir):
    """The game UI layer (spec 12.2), asserted where it is a pure function: layout
    is (tree, width, height) with no GL and no clock, the input pass takes a
    struct of values rather than a device, and the settings file is a table
    walked both ways. So the arms run with no window and no GPU, through
    gametest's probe -- all but the ones spec 14.2 added END TO END, which run
    apps/canvas headless, since what they assert is the path from the engine's
    input and the font's atlas to the UI's picture.

      ui-layout      a column at 1280x720 lands on its closed form: the panel
                     centred, its height the padding plus three rows plus two
                     gaps, and every row inset by the padding and grown to the
                     panel's width. The row HEIGHT is read rather than asserted,
                     since it comes from the font's metrics.
      ui-layout-resize the same tree at 640x360 and 2560x1440 centres at both, and
                     laying it out at the first size AGAIN reproduces the first
                     rects exactly -- layout derives its answer rather than
                     accumulating it.
      ui-wrap        a paragraph at 400 points breaks into lines that each fit,
                     and the first line must be most of the way full: breaking at
                     every word also never exceeds the limit.
      ui-nav         down moves through the column and wraps off the end; up wraps
                     the other way. Nothing is focused to begin with, so the first
                     press ACQUIRES and the wrap is the fourth press, not the
                     third.
      ui-focus-set   a screen opened on its middle row is focused there before any
                     press and says so, a press moves on from it as from a row
                     navigated to, and clearing it puts the first press back to
                     acquiring the first row (spec 13.49).
      ui-hit         a control claims its own centre and neither neighbour's; a
                     point in the panel's padding, and one in the gap between two
                     rows, belong to the parent and to no control. The pointer
                     walk then reaches the element under the cursor.
      ui-capture     a modal screen captures input and a non-modal HUD does not,
                     and popping gives it back. Then what suppression DOES, driven
                     through the pad reader seam rather than asked of the flag: a
                     held stick and button reach move_x and ui_accept, raising the
                     switch takes move_x to zero EXACTLY while ui_accept -- flagged
                     `ui` -- keeps reading, and lowering it hands move_x back. The
                     free reads are asserted too, or the rest would pass over a
                     layer that reads nothing at all.
      ui-stack       push, pop, pop_all and a pop of the empty stack each leave the
                     top where they should, including the underflow that must be a
                     no-op rather than a crash.
      ui-theme-identity a zeroed style emits vertices identical to no style at all,
                     which is what makes "zero means inherit" a resolution step
                     instead of a second drawing path -- and a style that DOES say
                     something must differ, or the comparison proves nothing.
      ui-textured-uv the whole-picture textured quad emits vertices identical to
                     the UV quad over {0, 0, 1, 1}, so it is that quad and not a
                     second path (spec 13.43) -- and a quad over part of a picture
                     must differ, or the UVs never reached the vertices.
      ui-on-stack    a screen pushed over another leaves the one under it on the
                     stack though not on top, and a pop takes the top one off and
                     nothing else (spec 13.50).
      ui-opaque      one screen over another draws both while it is plain, and draws
                     to the vertex as it does alone once it is opaque; popped, the
                     screen under it draws as it did (spec 13.49). The two screens
                     look different, or the last comparison would prove nothing.
      ui-elements-closed ui.h exports exactly the element constructors the
                     list names, six since spec 12.2, the pane and the field
                     since 14.2, the code editor since 14.4 and the table since
                     14.30. The scope ratchet, asserted rather than promised.
      ui-settings-roundtrip write, read back into a ZEROED struct, and every field
                     survives; the window mode rides the file as a name; an absent
                     file leaves defaults rather than zeros; and apply reaches the
                     mixer.
      ui-wrap-label  a label with `wrap` set, stretched across a fixed 300-point
                     panel, breaks into the lines ui_text_wrap_point gives at its
                     280-point width, measures that many line heights plus its
                     padding, and draws no glyph past its width; given no width,
                     it measures as its longest word (spec 14.2).
      ui-wrap-cache  laying a wrapped label out again at the same width breaks
                     nothing again; a new width, new text and a new size each break it
                     once. Its text is measured across once, and again only for new
                     text or a new size: a new width keeps the measure, which is the
                     label's longest word in its face.
      ui-runs-draw   a styled run colours exactly its bytes' glyphs, the rest
                     keep the label's colour, and a run in another face draws that
                     face's glyph.
      ui-runs-wrap   a run in a wider face takes more of a wrapped line, so the
                     label breaks into more lines, and nothing it draws reaches
                     past the label.
      ui-runs-refused runs out of order, overlapping, past the text or inside a
                     UTF-8 sequence are each refused and leave the runs set before;
                     setting the text clears them.
      ui-scroll-layout a pane's content measures its rows and gaps exactly, its
                     rows start at its top, and the wheel moves every row up by the
                     same scroll.
      ui-scroll-wheel a wheel unit scrolls one step, the theme's font size; the
                     scroll stops at either end; a wheel off the pane moves nothing,
                     and the inner of two panes takes it.
      ui-scroll-clip a row in view takes a click and one laid out below the pane,
                     under the same point, does not; the pointer is over the UI only
                     where the pane shows it; only the rows in view are emitted.
      ui-scroll-into-view navigating onto a row half out of view scrolls by exactly
                     what shows it, back up returns to the top, and a focus the app
                     sets is revealed -- but the pointer moving onto that half row
                     focuses it and scrolls nothing; ui_scroll_into_view shows a row
                     above the view by its top and moves no focus.
      ui-scroll-follow a pane following its end begins there, stays there as rows
                     are added, and stays where it was once scrolled up.
      ui-scroll-bar  the thumb is the share of the content in view, and as far
                     down the track as the content is scrolled; dragging it 50
                     points scrolls by that share of the track's room, and a press
                     on the track below it pages down by nine tenths of the view.
      ui-rows-built  100,000 virtual rows in a 200-point pane: the content is every
                     row at its estimate and the gaps, only the rows in view and one
                     either side are built, at the top and at row 50,000, which is
                     shown, and laying out again builds nothing.
      ui-rows-measure a row's height is measured when it is built and kept: the
                     content holds the built rows at their measure, laying out again
                     builds nothing, a new width measures again without building
                     the rows again, and a row marked changed is built again.
      ui-rows-anchor a row scrolled to is shown at the pane's bottom though the rows
                     above it measured twice their estimate; the row above the view
                     built again half as tall again leaves the first row in view
                     where it was.
      ui-rows-focus  the focused row stays built and focused when the pane is
                     scrolled far from it, beside the rows in view.
      ui-table-columns a table's columns (spec 14.30), as exact rects at 900 and 701
                     points: a FIT column as wide as its widest cell, which is row
                     90,000 of 100,000, far past the view; a FIT column narrower
                     than its title as wide as the title; a FIXED column its
                     length; a SHARE of 0.3 that share of the width less the gaps,
                     rounded down; two GROW columns halving the rest; each column
                     the spacing past the last, its start on a whole point of the
                     content.
      ui-table-measure 100,000 rows of 4 columns measure 400,000 cells once, laying
                     out again measures none, a row marked changed measures 4, the
                     widest cell shrunk narrows its column to the next widest, and
                     ten rows added measure 40.
      ui-table-heights a row of one line and a row of three are their lines at the
                     line height and 2 points of padding above and below; a
                     source's heights win; the rows meet with no gap; the first row
                     starts under the header, one line and its padding tall, or at
                     the content's top with no titles; scrolled 1,000 points by the
                     wheel, the header's rule stays a pixel above the rows and the
                     rows move up by the scroll; the source's heights doubled and
                     asked again alone (spec 14.32) are taken with no cell measured.
      ui-table-view  of 100,000 rows only those in view are drawn, counted by the
                     source's calls in a draw: at most the rows the view holds and
                     one more, by the columns; row 50,000 scrolled to is shown with
                     its bottom at the view's.
      ui-table-refused no columns, 33 columns, a drawn FIT column and an element
                     made under a table are each refused and leave the table's
                     columns where they stood, and a cursor past the rows, below
                     none or on a row that cannot hold it leaves the cursor; 32
                     columns are taken.
      ui-table-cursor no cursor at first; Down puts it on the first row that holds
                     it, three Downs in one pass skip the row that cannot and call
                     on_cursor once, Up skips it back, End and Home reach the last
                     and the first and End again calls nothing, Page Down moves a
                     view's rows; ui_table_set_cursor calls nothing; a press on a
                     row that holds it moves it there and one on a row that cannot
                     does not; Up from none lands on the last; a host's move is
                     told at once; a key hook taking Down leaves the cursor. Every
                     cursor's row is in view.
      ui-table-pane  a table in a pane (spec 14.32), as tall as its rows beside a
                     panel: the wheel over the table, whose rows all show, scrolls
                     the pane, and over the panel too; scrolled 300, the header is
                     held at the pane's top, a row is in view exactly when some of
                     it is below the header, and only the rows shown are drawn;
                     the cursor keyed down to row 30 is shown whole under the
                     header by the pane, and Page Down moves by the rows the pane
                     shows; the focus let go and given back leaves the pane where
                     the cursor's row shows; a press on the pane's bar over the
                     table pages the pane and leaves the cursor; a pane centres a
                     table shorter than it.
      ui-max-size    a fitting pane of 30 rows held to 200 points is that tall and
                     scrolls; a growing child held to 100 takes it and its siblings
                     share the rest; two held and centred leave the rest to the
                     alignment; a fixed element ignores its most.
      ui-share       UI_SHARE (spec 14.9), as exact rects: a share of 0.7 is that
                     share of the row's inner length less its gap, rounded down to
                     a whole point, and its GROW sibling the rest exactly, at
                     widths whose 0.7 is whole and is not, and with the GROW side a
                     row measuring its own gap; a share across the main axis; one
                     held at its most; two halves of an odd length leaving a point;
                     two of 0.7 overflowing as FIXED lengths; half of a parent that
                     fits its children, which it adds nothing to; and a share of 0.
      ui-measure     a panel measured by its app (spec 14.33): its answer across
                     plus its padding, a label after it the row's gap on; down, the
                     answer it gives for the width it was given, FIXED or fitted,
                     and a FIXED axis never asked; a child or text larger than the
                     answer wins; a GROW panel keeps its answer in a row whose even
                     share is less; its most holds; an answer below 0 or not a
                     number is none; refused on a label, which keeps its text's
                     width; and NULL clears it.
      ui-padding-none a negative padding and spacing are none, a zero one the
                     style's and the theme's, and the padding goes per side.
      ui-label-padding a label's text is drawn inside its padding: its first line's
                     baseline its top padding and its face's ascent below its top,
                     its pen its left padding in (spec 14.13). Drawn from its rect's
                     corner, every label's text sat its top padding high, 4 points
                     by default, with all its padding under it.
      ui-baseline    a row on UI_ALIGN_BASELINE (spec 14.13): labels at 14 and 28
                     points, one at 10 in a padded column, two in a row on the
                     baseline of its own, one after a textless icon in another and
                     one with 40 points of padding under it all stand on one
                     baseline, and the row's own baseline is that line; a panel
                     with no text stands at the row's top; and the row is as tall
                     as its lowest child reaches, the padded label moved down onto
                     the line. Within a framebuffer pixel, the grid a glyph's
                     corner is snapped to.
      ui-line-ascent ui_line_ascent (spec 14.31) at 14 and 28 points is the face's
                     ascent scaled to each, read from the face and not the
                     function; a label of each size with no padding stands its
                     glyph that far below its top, within half a pixel; with no
                     face it is 0. Returning the line height fails on the face,
                     the glyphs moving with it; the UI's own baseline a point off
                     fails on the glyphs alone.
      ui-control-lines a button and a slider whose text is two lines draw it through
                     the lines their height was measured from: the button's two
                     rows sit on its centre exactly as one row does, and the
                     slider's second row lies above its track. Centred as one
                     line, a two-line button's text sat half a line low and a
                     slider's track crossed its second line.
      ui-text-mesh   a text mesh's per-character colour counts characters, not
                     bytes: "héllo" is 5, its é is character 1 and only that quad
                     takes the colour.
      ui-field-type  typed characters make the field's text, the caret at its end,
                     and on_change fires once for a pass that changed it and not
                     for one that did not.
      ui-field-edit  arrows, a word left, Home, End, Backspace, a word's Backspace
                     and Delete each leave the text and the caret they should.
      ui-field-utf8  é, Ω and a four-byte codepoint: the caret steps a whole
                     codepoint, Backspace takes one whole, and the text decodes
                     with no U+FFFD.
      ui-field-select Shift+arrows, select all, one press, two (a word) and three
                     (the line); a read-only field takes runs and copies plain
                     text, and an editable one refuses runs.
      ui-field-clipboard copy, paste, a single-line paste flattening a newline,
                     cut, and a read-only field that copies but neither cuts nor
                     pastes nor takes typing -- all through the engine's
                     clipboard, which headless is its own.
      ui-field-undo  typing undoes and redoes a word at a time.
      ui-presses     a run of presses as an app counts one: three close in time
                     and place 1, 2, 3; a late one and a far one 1; a cycle of
                     three wrapping to 1; a run ended beginning again.
      ui-field-caret-x at every index of a line with kerning pairs, the caret is
                     where the glyph there is drawn, to the pixel snap.
      ui-field-scroll-x a line longer than its field keeps the caret inside it at
                     the end, and Home scrolls back to the start.
      ui-field-multiline a multi-line field wraps into more rows than its most,
                     stops growing at three rows' height, keeps the caret's x going
                     up a row, Home goes to the wrapped row's start, and End on a
                     line ending in spaces goes past them to its newline; with no
                     most it grows to every row, and Page Down pages by the three
                     rows a field held to three shows.
      ui-field-placeholder a hint far wider than its field sizes nothing, draws dim
                     while the field is empty and goes once it is typed into;
                     ui_set_text on a field is refused.
      ui-field-append ui_scroll_find_row finds a built row and none past it; text
                     appended to a read-only field joins it with its run placed after
                     the old text, the selection kept, and the row grown without
                     being built again; an append past the most is refused whole,
                     and one with runs refused on an editable field. Runs set in a
                     wider face and an append made before a layout leave the field
                     broken into the rows a field made beside it with the same text
                     and runs takes from scratch: the runs' change breaks it again at
                     once, where it used to wait for a layout that the append's own
                     break could hide it from.
      ui-field-keys  Enter submits; the key hook takes Up before the field; Tab
                     and Shift+Tab move the focus in tree order, and a character
                     after two Tabs in one pass reaches the field they reach; Esc
                     lets go in a screen that is not modal, a page key after it in
                     the same pass finding nothing focused, and closes a modal one;
                     the keys of the pass the app focused a field in are not its;
                     Shift+Enter is a new line in a field that submits on Enter.
      ui-field-focus the pointer passing over a button leaves a field focused; a
                     press on nothing lets go; a press on the field or the button
                     takes the focus there; ui_wants_keyboard follows the field.
      ui-match       fuzzy_match: no match is -1; word starts outrank letters
                     inside words ("rn": "route net" over "print"); case does not
                     matter; the best placement is found, not the first ("st" in
                     "fast stop" at 5 and 6); positions are byte offsets in a UTF-8
                     name; an empty query matches at 0; the positions say how many
                     they are, none for no match or an empty query.
      ui-utf8        text.c's UTF-8: every length's first and last codepoint
                     encodes and decodes back; surrogates and values past
                     U+10FFFF encode to nothing; overlong forms, a surrogate, bad
                     lead bytes and a cut sequence decode to one U+FFFD a byte; prev
                     mirrors next at every step over good and bad bytes, and does
                     not move at the start.
      ui-doc-roundtrip the code editor's text (spec 14.4) gives back exactly the
                     bytes it was made from -- LF, CRLF, mixed, a lone CR, a
                     byte-order mark, no final newline, nothing -- loaded, with a
                     line break put in and taken out again; the line ending is
                     found from the first line.
      ui-doc-random  3000 seeded replaces of random bytes, line breaks and
                     characters past ASCII, each checked against a flat string:
                     the text, its length and line count, every line's start and
                     content, line_of and copies; every step undone gives the
                     first text and redone the last.
      ui-doc-map     an offset held at an old version lands where the changes
                     since put it: kept before an insertion at it, moved after,
                     sent to the end of bytes that replaced it. A range is cut
                     where a change cuts into it, dropped when one replaces it
                     whole, grown round one inside it, and follows the changes
                     after; a version too new or too far back is refused.
      ui-doc-set-text a whole new text lands as the one change between the two,
                     the shared ends on characters' edges, a caret moved through
                     it; the same text again is no change; the host's own edit
                     inside a character is refused.
      ui-doc-undo    typing joins into words, a space starting the next; a sealed
                     caret splits typing; two carets' typing is one step and joins
                     as one caret's does; undo and redo restore the text and the
                     carets as they were; with keep_undo off nothing is kept. A
                     host's edit joined to the latest step (spec 14.35) is undone
                     with it, the caret back where the step began, and redone with
                     the caret moved through it; after an undo it is a step of its
                     own, typing after it is never merged into its bytes, and a
                     group opened before it starts where its first action finds
                     the caret.
      ui-doc-read-only a read-only text (spec 14.7): typing at two carets refused as
                     one action, the version unchanged and nothing told to
                     on_action; an editor's undo and redo refused; the host's edit
                     and its whole new text landing, and its own undo and redo
                     taking them; each refusal told once to on_refused; writable
                     again, nothing differs.
      ui-doc-highlight a line is coloured once and again only once it changes; a
                     stateful line waits on the walk from the top, and a changed
                     line asks again of the lines after it only while the state
                     it leaves has changed.
      ui-glyph-cache the code editor's glyphs (spec 14.4): every glyph of JetBrains
                     Mono from ASCII through Cyrillic, packed as asked for until
                     the atlas grows, is stb_truetype's own bitmap at that size
                     byte for byte where the atlas holds it; the cell is the
                     advance rounded; a codepoint the face lacks is missing; the
                     same size again keeps the atlas, another makes the glyphs
                     again at its scale.
      ui-editor-wrap the code editor's rows: a line breaks after its last space
                     that fits, at the edge for a word longer than a row, tabs and
                     characters past ASCII at their columns; a wrapped line's later
                     rows hang at its indent, unless that is over half the row.
      ui-editor-hit  every glyph an editor drew, clicked in its cell, lands on the
                     offset drawn there, and every character's caret, clicked,
                     lands back on itself -- across a tab, a wrapped line,
                     characters past ASCII and an empty line.
      ui-editor-anchor three lines put in above the view, every line above it
                     measured where its rows were guessed, and a new width all keep
                     the line at the view's top; the wheel moves it.
      ui-editor-keys each of the editor's standard bindings on a fixture: a
                     character, a word (to its end on macOS, the next one's start
                     elsewhere), a row, the line's first character then its start,
                     the text's start, the deletes, Enter indenting, Tab and
                     Shift+Tab, select all, copy, cut and paste, typing past ASCII,
                     undo and redo; and every command listed once, named and
                     described.
      ui-editor-carets typing at five carets as one action; selections that overlap
                     merged; five lines pasted one to each caret; Cmd+D taking the
                     word and then its next matches; Esc back to the primary; a
                     caret added a row below twice at its column; Option+click
                     adding one; a double click taking a word, a triple its line
                     and a fourth a caret again; and find's next match selected.
                     Run with freed
                     memory scribbled over: the bar's place is summed from the
                     rows above the first line, which a new editor once left unset,
                     and the drag on the bar read that garbage (spec 14.6's review).
      ui-editor-mode a vim-like mode made through the editor's public calls and
                     nothing else: x, dw, a count before j, i into an insert mode
                     that types and takes the standard mode's arrows, and Esc back
                     to a normal mode that types nothing. What makes a vim mode an
                     addition.
      ui-editor-host-undo with the text keeping no undo, a host's mode over the
                     standard one binds Cmd+Z to its own command: the command runs,
                     the text is untouched, the editor's own undo refuses.
      ui-editor-layer a layer over a mode takes the mode's look (spec 14.5): its
                     block caret drawn a cell wide, and nothing typed where the
                     mode types nothing; its own key runs, and its name is its own.
      ui-editor-decline a layer's command that does nothing hands the key to the
                     mode under it, whose binding for it runs.
      ui-editor-paired-char a press is a key and then its character: a binding that
                     takes Space takes the space with it; a space arriving alone,
                     as an input method's would, is typed; a key nothing binds
                     types its character; a used key with no character in its pass
                     -- a script's -- takes none from the next; a key whose command
                     moves the focus to another editor, as a split bound to `|`
                     does, types its character into neither (spec 14.6); and a text
                     field's letter, key and character, typed, the field using no
                     key it types from its character.
      ui-element-keys a panel takes keys with a hook (spec 14.10): focused, it
                     is handed every key and character in order, and a key it takes
                     moving the focus to a field takes its character with it, the
                     field typing nothing; the pointer passing over a field and a
                     button leaves it focused, a press on the field moves the
                     focus, Tab moves it on and Esc lets go; a press focuses it
                     only when it is focusable, and ui_set_focus does either way; a
                     field's hook keeps a / out of the text and the letters it
                     declines are typed; a focusable panel with none takes no keys
                     and gives the focus to a passing pointer; a button, toggle,
                     slider, selector and editor refuse a hook, the editor typing
                     as before.
      ui-panel-focus a panel taking the keys is drawn focused (spec 14.11): with a
                     key hook and a draw hook, the theme's FOCUS border drawn over
                     the hook's painting while it is focused and while it is
                     pressed, none unfocused; with no draw hook, its own quad
                     carries the border, and with an image in its style the border
                     is drawn over the image; the app's colour in theme.panel[FOCUS]
                     is the one drawn; a focusable panel with no key hook -- a
                     list's row, which the focus follows the pointer over -- and
                     a label with both hooks draw no border.
      ui-border-fade a fading screen fades a quad's border with its fill (spec
                     14.12): a bordered rect drawn at a draw list's alpha of one
                     half carries half its fill's alpha and half its border's.
      ui-editor-shapes a selection of three characters drawn by each shape: three
                     cells, four with the character at its far end (a vim mode's
                     visual mode), and its line whole with the line break (visual
                     line mode); the block caret over the character at the head.
      ui-editor-line-numbers the code editor's line numbers in theme.line_numbers
                     (spec 14.14): with it zeroed, every number glyph is the
                     text's colour at 38% of its alpha; set to red, every number
                     is red and the code's glyphs keep their colour; zeroed once
                     more, the numbers are at 38% again.
      ui-editor-whole-lines a range whose style has whole_lines (spec 14.15): one
                     quad on each row of each line it reaches, from the text's
                     left edge to the editor's right, the empty line's and the
                     wrapped line's every row included; none on the line its end
                     starts; a selection drawn over it; a style without the flag
                     two cells behind two characters; zeroed, no such quad.
      ui-editor-scroll-line the exact scroll (spec 14.15): after a wheel's 32.5
                     rows, ui_editor_top_line gives the top line and its row with
                     half a row more; ui_editor_scroll_line puts a second editor
                     there with three rows' starts level with the first's; put
                     before a layout, it is taken at it; past the last line it
                     holds where a wheel run to the end does.
      ui-editor-folds a closed fold (spec 14.16) is one row: one quad in
                     theme.editor_fold from the gutter's left, its label's glyphs
                     in the line numbers' colour and no number, line 40 the next
                     row numbered 41 as when open, an open fold's lines each on
                     its own row; a row move from line 9 lands on its first line
                     and the next on line 40, a wheel's row puts it at the top,
                     its row is its first line's start and an offset inside it is
                     drawn there; a click opens it, the caret on its first line,
                     and it closes again by line, its label a copy; a line put in
                     among its lines joins it, taking them all out takes it, and
                     typing or a Delete in a closed fold opens it, typing on a
                     read-only text does not; Home and End from a line a fold
                     hides stay on that line; an editor shown another
                     text and back keeps its folds, a second has none; folds
                     that overlap are refused; the standard mode's open-fold,
                     toggle-fold and close-fold act on the caret's fold, and do
                     nothing with no fold there.
      ui-editor-fillers filler rows (spec 14.16): two above line 5 and one above a
                     closed fold's first line are three quads in
                     theme.editor_filler from the text's left edge to the
                     editor's right, with no glyph on their rows; one above a
                     line the fold hides is not drawn; a row move down from line
                     4 lands on line 5 and from line 9 on the fold, up from line
                     5 on line 4; a wheel's row from line 4 puts line 5 at the
                     top at row 0, and line 5 put at 2 rows is its text's first
                     row; a click on a filler lands at its line's start, and on
                     the one above the fold leaves the fold closed; the
                     long line's rows as ui_editor_line_rows gives them are its
                     rows as drawn.
      ui-editor-gutter gutter marks (spec 14.16): a style with a red gutter fill and
                     a + over a line wrapping to several rows draws one red quad a
                     row from left of the numbers to the text's left edge, the
                     first before the line's number; the + in the gap after the
                     number on its first row only, in the text's colour; a style
                     with a - alone puts it on its line, shorter than the +, and
                     no fill; the - style's sign_colour green (spec 14.26), the -
                     is green and the + still the text's colour; with both styles
                     zeroed, no quad ends at the text's edge and no glyph stands
                     in the gap.
      ui-editor-bar-room a bar is drawn only inside its element, its insets
                     included (spec 14.17): of two editors in a row over a
                     200-line text, the second 300 by 150, every fill a bar wide
                     stands inside the second less the insets, its thumb among
                     them, with the first sized 0 by 0 and again 4 by 150; a press
                     inside the narrow one below where a thumb would stand
                     scrolls nothing.
      ui-editor-top-row the view's top as rows from the text's start (spec 14.19):
                     0 at the start; line 100 half a row in is the rows above it,
                     3 fillers and a closed fold's one row among them, and 0.5;
                     ui_editor_scroll_row reads back as put on a filler, on the
                     fold's row, on a wrapped line's second row and with a part;
                     held past the end where a wheel run there stops, and at 0;
                     asked before a layout, taken at it; two editors side by
                     side, a line wrapping on one and fillers making it up on the
                     other, put at the same row, drawing the next lines level.
      ui-editor-gutter-digits the gutter as wide as the digits asked (spec 14.19): a
                     text of 9 lines asked for 4 has the columns of one of 1000 in
                     the same width, its line 1 number where that text's stands;
                     0, or fewer than its own, gives its own columns; asked for 7
                     on a text too long for one frame's budget, its rows guesses
                     again, settled after.
      ui-editor-filler-end fillers under the last line (spec 14.19): on a text of 20
                     lines with no last line ending, 2 on the line count drawn as 2
                     fills after line 19 with no glyph on their rows; counted to
                     the scroll's end, by a row put past it and by a wheel; G and a
                     j stand on the last line, two rows down from the line before
                     stop at the text's end, and a click on them lands there; the
                     last line taken out, they stand under the new last; one past
                     the line count left out; with a closed fold ending the text,
                     the rows summed again when an edit or a new set of fillers
                     moves the line counting them, a fold closed over the top
                     putting it on the fold's row, and Home on a wrapped row of
                     the fold's first line staying on that line.
      ui-editor-rows-kept a line's rows answered from the count the editor keeps
                     (spec 14.20): on 200 lines in 300 points, settled, every line
                     asked twice wraps none; on a text just shown, laid out and
                     not drawn, the first ask of a line wraps it and the second
                     does not; at 240 points the asked line wrapped again to its
                     new count; at 8 columns a tab stop, the rows guesses again
                     and a line of tabs wrapped at 8.
      ui-editor-fillers-read the fillers read back (spec 14.20): given out of order, a
                     line named twice and one of no rows, read back sorted, the
                     last for the line, the empty one left out; moved with their
                     lines by two put in above; the one on a line pasted over with
                     the same text gone; one on the line count last; none, and
                     an element not an editor, 0 and NULL.
      ui-editor-exact-rows exact rows (spec 14.20): on 600 lines whose words each take
                     a row, guessed shorter than they wrap, an editor laid out on
                     the text and not drawn counts the rows above line 300 on its
                     first ask as one settled does, and ui_editor_scroll_row at
                     the settled one's row puts its top on the same line and row;
                     the bar dragged half way wraps 8 lines at most; a closed
                     fold's 100 lines left guesses, the rest above wrapped; the
                     fold cut to its first line by a change below it, that
                     line's own rows counted; the fold opened after a walk passed
                     it, its lines wrapped by the next ask; a second ask wraps
                     none.
      ui-editor-scrolls the scrolls asked of the view counted (spec 14.20): one for
                     each of ui_editor_reveal, _scroll_place, _scroll_line and
                     _scroll_row; one for a wheel's step, each pass of the bar
                     pressed or held and a key moving the caret; one asked before
                     a layout counted then and not when taken; none for an edit
                     above the view, a fold closed over the top and opened, a
                     layout at another width and the budget settling.
      ui-editor-link two editors linked (spec 14.21), side by side on 200 lines,
                     the left's line 40 wrapping to three rows and two fillers on
                     the right making them up: the right wheeled 100.5 rows, one
                     pass leaves the left at its row, lines 101 and 102 level; a
                     row asked of the right before its first layout on a text
                     just shown taken by one layout, the left placed before it
                     and after; the leader's rows above its top changed with no
                     scroll asked, by fillers set and by a narrower width, the
                     follower following; ui_editor_scroll_row, a wheel, the
                     bar's thumb and a key on the left each make it lead; a line
                     asked of
                     the left before its layout, then a row of the right, both at
                     the right's; the left moved counts no scroll and does not
                     lead; the left shown another text follows the right's row;
                     hidden, each holds the other to nothing until shown; the
                     right freed lets the left go, and a third linked then a
                     fourth lets the third go; linked to itself or a panel,
                     refused; NULL lets both sides go.
      ui-editor-link-folds a fold turned on one of a linked pair turns the fold of
                     the same index on the other (spec 14.21), of three closed on
                     each: open-fold and close-fold run at a caret on the left, a
                     ui_editor_set_fold_open on the right, a press on the left's
                     fold row, typing at a caret in the right's fold, and vim's
                     zM and zo; none with two folds on one side, from either side,
                     the right's third among them; a set of folds on one leaves
                     the other's; a fold closed above the leader's top by a set
                     of its own, the follower at its row after one pass.
      ui-editor-steps each action told with its step: typing joined into words
                     shares one; a group holds one over typing, a Backspace and
                     more typing, one undo takes the group back and one redo makes
                     it again; typing after keep_undo was off is a step of its own,
                     never added to one from before; the steps are the same with
                     keep_undo off.
      ui-vim-oracle  the vim mode held to vim itself (spec 14.5): every sequence
                     of a fixed corpus -- each motion, operator and edit, with
                     counts, operators over every kind of motion and text object,
                     the three visual modes and every operator over them, puts
                     over an area, `.`, undo and redo -- run on pcb-like texts,
                     CRLF, tabs and letters past ASCII among them, through the mode
                     and through vim 9 fed the keys as typed; the text, byte for
                     byte, and the cursor must agree. Skips without vim on the
                     path.
      ui-vim-folds   the same over the host's folds (spec 14.16): one fold or two,
                     closed by :A,Bfold in vim and ui_editor_set_folds in the mode,
                     on the two fixtures long enough for them; moves by lines that
                     take a closed fold as one line, zo zO zv zc za zR zM, the moves
                     along a line, searches, % and undo that open one, operators
                     that take one whole, and inserts and puts beside one. Skips
                     without vim on the path.
      ui-vim-random  the same, on 1600 sequences no one wrote: two to six whole
                     commands each -- motions, operators over motions and objects,
                     edits, inserts, undo, `.`, searches, areas in the three visual
                     modes with operators over them, blocks run to their lines'
                     ends -- drawn from four fixed seeds, so every run tries the
                     same ones; a difference names its seed and its keys. Skips
                     without vim on the path.
      ui-vim-state   the vim mode's state as a host shows it: NORMAL and INSERT
                     named, a block caret and a bar, the keys of a half-typed
                     command ("2d") shown and dropped by Esc, a click past a line's
                     end held on its last character, VISUAL, V-LINE and V-BLOCK
                     named and drawn inclusive, as lines and as pieces, a drag
                     entering visual mode with its head on the last character it
                     reached, and a put 999,999,999 times refused.
      ui-vim-undo    undo by vim's units: an insert from i to Esc, an Enter and a
                     Backspace in it, is one step, and cw with what it typed is
                     one -- undone and redone whole, and told to a host as one step
                     with keep_undo off; an insert of 5,000 keys made again whole by
                     ., in normal mode after.
      ui-vim-host    a host's layer over the vim mode: its Space acts in normal mode
                     and takes the space its press makes, in insert mode hands the
                     key back and the space is typed; the host's undo takes u and
                     Ctrl+R; / and : ask for the host's line, its answers search
                     and go to line 3, "w" is not vim's, and d/ answered by the host
                     is made again by . without asking; one vim over two editors,
                     an insert left in the first let go when keys reach the second;
                     a paste in insert mode made again by . as the text it typed
                     (macOS, where the paste key is).
      ui-vim-lookup  a host answering what a name is (spec 14.9), recording what it
                     is asked: gd, gD, Ctrl+] and K in normal mode each asked once,
                     of its kind; g, Esc and d asking nothing and leaving d waiting;
                     dgd and gd in visual mode vim's motions, asking nothing; Ctrl+]
                     and K in visual mode asked with the area in place and leaving
                     visual mode at its head, and an area over two lines asking
                     nothing and kept, as vim keeps it; a host finding nothing
                     giving vim's own gd and leaving the caret for Ctrl+] and K; .
                     after a lookup making the change before it again; a read-only
                     text still asking; a host going to a definition from a block,
                     visual mode ending where it went; with no host, dgd after j
                     characterwise, the one difference from vim kept; and a host
                     showing the editor another text from visual mode leaving the
                     mode normal there.
      ui-vim-carets  several carets, which vim has none of: Ctrl+V and I puts a
                     caret on each line, Z typed at every one, Esc and then u each
                     leaving one cursor at the block's top left; A after the block;
                     three carets each taking 3x, dw and vlld; two areas sharing a
                     character deleted as one; and two carets whose i( ranges nest
                     out of their order, > and = taking each line of the outer once.
      ui-editor-script apps/canvas --editor on a file, driven by a script: a caret
                     added below, "XY" typed at both, undone, Esc to one caret, the
                     word at it taken by Cmd+D and found again round the text's end
                     (end to end).
      ui-vim-script  apps/canvas --editor --vim, driven by a script: dw, a search
                     typed in the app's own line under the editor, x, :1 there, .
                     made again, and a search finding nothing said on the line
                     (end to end); and with --vim-lookup --vim-layout (spec 14.9),
                     == re-indenting a line by its bracket and gd saying itself on
                     the line.
      ui-editor-host-edit the host's edits through an editor: a selection moved by an
                     edit before it, kept by one after it, grown by one inside it; a
                     caret under replaced bytes at their end, and before bytes the
                     host put in at it; a whole new text changing past the caret
                     keeping it; a second editor of the same text moved by typing in
                     the first.
      ui-remove      an element taken out (spec 14.6): the focus inside it, as the
                     last input pass saw it, left on nothing and the next layout
                     putting the element after it where it stood; a held press let
                     go, its release activating nothing; a button whose own action
                     takes it out, its second ask refused as the element is out
                     already, and an editor its own key takes out, freed as the
                     pass ends, the text forgetting the editor; a held bar let go; an
                     editor taken out mid-insert closing its text's group; a root and
                     a pane's row refused, and the focus refused an element of
                     another screen. Run with freed memory scribbled over, so an
                     element freed inside the pass that took it out crashes the run
                     rather than passing.
      ui-move        an element moved (spec 14.6): an editor under a new row
                     keeping its carets, its view and the focus, typing still
                     reaching it; a field keeping its text; placed in front of the
                     child named, at the end for none, and in front of itself where
                     it is; a root, a child to go in front of that is not the
                     parent's, a move into itself or under it, a pane's row, a move
                     into a pane with rows or into one of its rows, and a move to
                     another screen refused.
      ui-hidden      a hidden element (spec 14.18): the middle of three panels 100
                     wide with gaps of 10 hidden, the third 110 after the first and
                     the row 210; of two growing, the other takes the whole row; the
                     hidden panel's rect and its child's 0 by 0, and each standing
                     where it stood once shown; a hidden editor and a filled panel
                     drawing what the screen draws without them; a press and a
                     wheel where a hidden editor stood reaching the pane round it;
                     hiding the focused editor leaving the focus on nothing, a key
                     typed then changing nothing; the focus refused a hidden editor
                     and a field under a hidden panel; a held press and a held bar
                     let go when hidden; an editor moved under a hidden panel
                     letting go of the focus; Tab stepping over a hidden button; an
                     editor scrolled to line 50 with two carets hidden five passes,
                     its columns the same each pass, and shown with its top line,
                     carets and wrapped rows kept, settled at once, where sized to
                     nothing it has one column; a pane hidden mid-ease asking for no
                     frame; a pane of rows hidden five passes building and freeing
                     none and shown with the same rows; a root and a pane's row
                     refused.
      ui-editor-set-doc an editor shown another text (spec 14.6): a text it has not
                     shown from its start; the text left no longer moving its
                     carets -- one at 3 in the new text stays there through an edit
                     at the old one's start -- while a second editor on it keeps its
                     own; typing reaching the new text; the same text again changing
                     nothing; NULL showing nothing; back to the first text where it
                     was, its carets and its top brought through the edit made there
                     while it showed another, by the text itself; a new editor
                     placed before its first layout, placed at that layout; and a
                     text freed while the editor was parked on it, its place let go.
                     Run with freed memory scribbled over.
      ui-vim-set-doc what the vim mode held unfinished in an editor -- a half-typed
                     operator, an insert, whose group on the text left is closed, a
                     visual area -- let go when the editor is shown another text,
                     and an operator let go when the editor is freed and another
                     made in its place. That last case stands for what it names only
                     where the allocator gave the new editor the freed one's address,
                     which the probe says, and the arm says when it did not.
      ui-editor-read-only apps/canvas --editor --read-only, driven by a script (spec
                     14.7): typing, Backspace, cut and paste change nothing; the
                     word Cmd+D takes is cut with nothing copied before, and reaches
                     the clipboard; the caret moves; each action one refusal (end to
                     end).
      ui-vim-read-only the vim mode on a read-only text, as vim with 'nomodifiable'
                     (spec 14.7): a visual delete letting the area go with the cursor
                     at its start, told with the editor; 3dd and 5J one refusal each;
                     . making the refused command again, refused again, and with the
                     text writable making it; a block's I into an insert whose typing
                     is refused; a text made read-only during an insert keeping the
                     insert open until Esc. Against vim 9.2: what . makes of a refused
                     visual put (the delete under it) and a refused 3s (its count)
                     once the text is writable, a J on the last line, an r past the
                     line's end and a ~ on an empty line beeped at rather than
                     refused, and a refused r kept for nothing. Then apps/canvas
                     --editor --vim --read-only by a script: yiw yanks first, x, dd,
                     p, ., cw, i and o change nothing and leave the clipboard as
                     yanked, w and j move, and the refusals are counted (end to end).
      ui-vim-read-only-oracle the oracle's corpus again on a read-only text, beside vim
                     with 'nomodifiable': the text and the cursor must agree. Three
                     sequences are left out by name: with their `o` refused, what
                     they would insert is read as commands, and it holds `q` and
                     `(`, a macro and a sentence motion the mode leaves out. Skips
                     without vim on the path.
      ui-vim-layout-oracle = and gq and the new lines that ask for an indent, held to vim
                     (spec 14.9): the layout corpus on every fixture and a crooked
                     one, vim sourcing scripts/vim_layout.vim as its indentexpr and
                     formatexpr, the mode given the same two rules in C as a host's
                     hooks; then again read-only beside 'nomodifiable', the three
                     sequences that type ( into a refused insert left out, as above.
                     Skips without vim on the path.
      ui-editor-ranges-late apps/canvas --editor's parse on its own thread, taken six
                     frames after it began: the text it read had an unclosed '(' at
                     byte 9, three bytes were typed before it since, and the error is
                     at 12 and underlined there in red (end to end).
      ui-type-palette apps/canvas --palette driven by a script: mod+K opens it and
                     "zo" filters the commands to Zoom in and Zoom out, the first
                     lit at its first two bytes, with the text in the field (end to
                     end).
      ui-click-palette the same palette, and a scripted click on its second row:
                     the press and release the engine read reach the UI through
                     ui_input_read, Zoom out runs and the palette closes (end to
                     end).
      ui-history-clip apps/canvas --history: the column's padding above its pane,
                     and the gap below it down to the input line, are the column's own
                     colour, though the pane is scrolled to the end of 200 messages,
                     and the pane shows text (end to end).
      ui-typed       an input script's typing reaches the UI through
                     ui_input_read, every event in its order: characters
                     past ASCII as their codepoints, a key with the platform's
                     shortcut modifier for `mod`, and a later line's key on its own
                     frame (spec 14.2, end to end).
      ui-glyphs      é, Ω and µ draw from the atlas, which held printable ASCII
                     only until spec 14.2: each puts ink where a space puts none, and
                     é is not e. A codepoint the atlas lacks draws its box rather
                     than a silent gap (end to end).
    """
    arm = Arms(21)

    def close(a, b, tol=0.01):
        return abs(a - b) <= tol

    # ---- layout
    @arm("ui-layout")
    def _():
        p = _ui_probe("layout")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            panel = p[("panel", "rect")]
            rows = [p[(f"row{i}", "rect")] for i in range(3)]
            pad, gap, pw = 10.0, 8.0, 300.0
            h = rows[0][3]
            want_ph = 2 * pad + 3 * h + 2 * gap
            ok = (close(panel[2], pw) and close(panel[3], want_ph)
                  and close(panel[0], (1280.0 - pw) / 2) and close(panel[1], (720.0 - want_ph) / 2)
                  and all(close(r[0], panel[0] + pad) for r in rows)
                  and all(close(r[2], pw - 2 * pad) for r in rows)
                  and all(close(rows[i][1], panel[1] + pad + i * (h + gap)) for i in range(3))
                  and all(close(r[3], h) for r in rows))
            detail = (f"panel {panel[0]:.0f},{panel[1]:.0f} {panel[2]:.0f}x{panel[3]:.0f} "
                      f"(want centred, h = 2*{pad:.0f} + 3*{h:.0f} + 2*{gap:.0f} = {want_ph:.0f}), "
                      f"rows inset to {rows[0][2]:.0f} at y "
                      f"{rows[0][1]:.0f}/{rows[1][1]:.0f}/{rows[2][1]:.0f}")
        return ok, detail

    # ---- layout at other sizes
    @arm("ui-layout-resize")
    def _():
        p = _ui_probe("layout-resize")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            small, large, again = p[("small", "rect")], p[("large", "rect")], p[("again", "rect")]
            ok = (close(small[0], (640.0 - 300.0) / 2) and close(small[1], (360.0 - small[3]) / 2)
                  and close(large[0], (2560.0 - 300.0) / 2)
                  and close(large[1], (1440.0 - large[3]) / 2) and small == again)
            detail = (f"640x360 -> {small[0]:.0f},{small[1]:.0f}; "
                      f"2560x1440 -> {large[0]:.0f},{large[1]:.0f}; back to the first size "
                      f"reproduces it {'exactly' if small == again else 'NOT exactly'}")
        return ok, detail

    # ---- wrapping
    @arm("ui-wrap")
    def _():
        p = _ui_probe("wrap")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            limit = p[("limit", "points")][0]
            count = int(p[("lines", "count")][0])
            widths = [p[(f"line{i}", "width")][0] for i in range(count)]
            ok = count >= 2 and all(w <= limit for w in widths) and widths[0] > limit * 0.75
            detail = (f"{count} lines at {limit:.0f} points, widths "
                      + "/".join(f"{w:.0f}" for w in widths)
                      + f" (want every line <= {limit:.0f} and the first > {limit * 0.75:.0f})")
        return ok, detail

    # ---- navigation
    @arm("ui-nav")
    def _():
        p = _ui_probe("nav")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            seq = [p[(k, "focus")][0] for k in ("start", "down", "twice", "last", "wrapped", "up")]
            ok = seq == [-1.0, 0.0, 1.0, 2.0, 0.0, 2.0]
            detail = ("focus " + " -> ".join(f"{v:.0f}" for v in seq)
                      + " (want -1 0 1 2 0 2: nothing focused, then acquire, two moves, a wrap off "
                        "the end and a wrap back)")
        return ok, detail

    # ---- focus set by the app
    @arm("ui-focus-set")
    def _():
        p = _ui_probe("focus-set")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            seq = [p[(k, "focus")][0] for k in ("set", "down", "cleared", "acquired")]
            same = p[("reported", "same")][0]
            ok = seq == [1.0, 2.0, -1.0, 0.0] and same == 1.0
            detail = ("focus " + " -> ".join(f"{v:.0f}" for v in seq)
                      + f" (want 1 2 -1 0: set, moved on, cleared, acquired), reported back "
                        f"({same:.0f})")
        return ok, detail

    # ---- hit testing
    @arm("ui-hit")
    def _():
        p = _ui_probe("hit")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            ok = (p[("centre", "row1")][0] == 1 and p[("centre", "row0")][0] == 0
                  and p[("centre", "row2")][0] == 0 and p[("padding", "panel")][0] == 1
                  and p[("padding", "rows")][0] == 0 and p[("gap", "rows")][0] == 0
                  and p[("pointer", "row1")][0] == 1 and p[("pointer", "row0")][0] == 0)
            detail = (f"centre claims {p[('centre', 'row1')][0]:.0f} of row1 and "
                      f"{p[('centre', 'row0')][0] + p[('centre', 'row2')][0]:.0f} of its "
                      f"neighbours; padding and gap claim {p[('padding', 'rows')][0]:.0f}/"
                      f"{p[('gap', 'rows')][0]:.0f} rows; the pointer walk reaches row1 "
                      f"({p[('pointer', 'row1')][0]:.0f})")
        return ok, detail

    # ---- capture
    @arm("ui-capture")
    def _():
        p = _ui_probe("capture")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            free_move, free_accept = p[("free", "move")][0], p[("free", "accept")][0]
            held_move, held_accept = p[("held", "move")][0], p[("held", "accept")][0]
            given_move = p[("given", "move")][0]
            ok = (p[("hud", "captures")][0] == 0 and p[("menu", "captures")][0] == 1
                  and p[("popped", "captures")][0] == 0 and p[("cleared", "captures")][0] == 0
                  # The held pad must actually reach both actions, or every
                  # assertion below passes over a layer that reads nothing at all.
                  and free_move > 0.5 and free_accept > 0.5
                  # Zero EXACTLY: suppression returns a zero, it does not attenuate.
                  and held_move == 0.0 and held_accept > 0.5
                  and given_move > 0.5)
            detail = (f"hud {p[('hud', 'captures')][0]:.0f}, modal "
                      f"{p[('menu', 'captures')][0]:.0f}, popped "
                      f"{p[('popped', 'captures')][0]:.0f} (want 0/1/0); with a pad held, move_x "
                      f"{free_move:.2f} -> {held_move:.2f} -> {given_move:.2f} (want it taken to 0 "
                      f"and handed back) while ui_accept holds {free_accept:.2f} -> "
                      f"{held_accept:.2f} (want it unaffected)")
        return ok, detail

    # ---- the screen stack
    @arm("ui-stack")
    def _():
        p = _ui_probe("stack")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            keys = ["empty", "pushed", "second", "popped", "cleared", "underflow"]
            vals = [p[(k, "top")][0] for k in keys]
            ok = all(v == 1 for v in vals)
            detail = ("top is where it should be after "
                      + ", ".join(f"{k}={v:.0f}" for k, v in zip(keys, vals)) + " (want all 1)")
        return ok, detail

    # ---- theme resolution
    @arm("ui-theme-identity")
    def _():
        p = _ui_probe("theme-identity")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            match = p[("styles", "match")][0]
            differs = p[("styled", "differs")][0]
            ok = match == 1 and differs == 1
            detail = (f"a zeroed style hashes identically to no style ({match:.0f}, want 1) and a "
                      f"style that says something does not ({differs:.0f}, want 1)")
        return ok, detail

    # ---- a part of a picture
    @arm("ui-textured-uv")
    def _():
        p = _ui_probe("textured-uv")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            match = p[("whole", "match")][0]
            differs = p[("part", "differs")][0]
            ok = match == 1 and differs == 1
            detail = (f"the whole-picture quad hashes identically to the UV quad over 0..1 "
                      f"({match:.0f}, want 1) and a part of the picture does not ({differs:.0f}, "
                      f"want 1)")
        return ok, detail

    # ---- on the stack, not only on top
    @arm("ui-on-stack")
    def _():
        p = _ui_probe("on-stack")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            keys = [("empty", "a"), ("pushed", "a"), ("pushed", "b"), ("under", "a"), ("over", "b"),
                    ("popped", "b"), ("popped", "a")]
            got = [p[k][0] for k in keys]
            want = [0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0]
            ok = got == want
            detail = ("on the stack " + " ".join(f"{s}-{n}={v:.0f}" for (s, n), v in zip(keys, got))
                      + " (want 0 1 0 1 1 0 1: the one under counts, a pop takes only the top)")
        return ok, detail

    # ---- a screen that hides the ones under it
    @arm("ui-opaque")
    def _():
        p = _ui_probe("opaque")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            plain = p[("plain", "differs")][0]
            covered = p[("covered", "match")][0]
            popped = p[("popped", "match")][0]
            ok = plain == 1 and covered == 1 and popped == 1
            detail = (f"stacked plainly both draw ({plain:.0f}), opaque the top draws as it does "
                      f"alone ({covered:.0f}), popped the one under draws as it did "
                      f"({popped:.0f}); want 1/1/1")
        return ok, detail

    # ---- the closed element list
    @arm("ui-elements-closed")
    def _():
        try:
            with open(_UI_HEADER, "r", encoding="utf-8") as f:
                names = _UI_CTOR.findall(f.read())
            ok = names == _UI_KINDS
            detail = (f"ui.h exports {len(names)} element constructors: {', '.join(names)} "
                      f"(want exactly {len(_UI_KINDS)})")
        except OSError as e:
            ok, detail = False, f"cannot read ui.h ({e})"
        return ok, detail

    # ---- settings
    @arm("ui-settings-roundtrip")
    def _():
        env = dict(os.environ)
        env["CETRA_SETTINGS_DIR"] = os.path.join(workdir, "ui-settings")
        p = _ui_probe("settings", env=env)
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            want_default = {"master": 1.0, "music": 1.0, "sfx": 1.0, "ui": 1.0,
                            "window_mode": 0.0, "vsync": 1.0}
            want_written = {"master": 0.25, "music": 0.5, "sfx": 0.75, "ui": 0.125,
                            "window_mode": 1.0, "vsync": 0.0}
            ok = (all(close(p[("default", k)][0], v, 1e-6) for k, v in want_default.items())
                  and all(close(p[("roundtrip", k)][0], v, 1e-6) for k, v in want_written.items())
                  and all(close(p[("absent", k)][0], v, 1e-6) for k, v in want_default.items())
                  and p[("named", "fullscreen")][0] == 1
                  and all(close(p[("applied", k)][0], want_written[k], 1e-6)
                          for k in ("master", "music", "sfx", "ui")))
            detail = (f"written {p[('roundtrip', 'master')][0]:.3f}/"
                      f"{p[('roundtrip', 'music')][0]:.3f}/{p[('roundtrip', 'sfx')][0]:.3f} "
                      f"survives a read into a zeroed struct, the mode rides the file as a name "
                      f"({p[('named', 'fullscreen')][0]:.0f}), an absent file leaves defaults, and "
                      f"apply reaches the mixer ({p[('applied', 'master')][0]:.3f})")
        return ok, detail

    # ---- wrapped labels (spec 14.2)
    @arm("ui-wrap-label")
    def _():
        p = _ui_probe("wrap-label")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            _, _, w, h = p[("label", "rect")]
            lines, want_lines = p[("lines", "count")][0], p[("lines", "want")][0]
            lh, inner = p[("line", "height")][0], p[("inner", "width")][0]
            right = p[("drawn", "right")][0]
            fit, word = p[("fit", "width")][0], p[("fit", "word")][0]
            # A label's own padding is 4 above and below, from the default style.
            ok = (lines == want_lines and lines > 1 and close(h, lines * lh + 8.0)
                  and close(w, inner) and right <= inner + 0.5 and close(fit, word))
            detail = (f"{lines:.0f} lines (want {want_lines:.0f}, over 1), {h:.2f} high (want "
                      f"{lines * lh + 8.0:.2f}), {w:.2f} wide (want {inner:.0f}), drawn to "
                      f"{right:.2f} (want within {inner:.0f}); with no width it is {fit:.3f} wide, "
                      f"its longest word {word:.3f}")
        return ok, detail

    @arm("ui-wrap-cache")
    def _():
        p = _ui_probe("wrap-cache")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            steps = ("first", "again", "wider", "retext", "restyle")
            seq = [p[(k, "wraps")][0] for k in steps]
            measured = [p[(k, "measures")][0] for k in steps]
            ok = seq == [1, 1, 2, 3, 4] and measured == [1, 1, 1, 2, 3]
            detail = (f"broken {seq[0]:.0f} time(s) first, {seq[1]:.0f} after two more layouts, "
                      f"{seq[2]:.0f} at a new width, {seq[3]:.0f} after new text, {seq[4]:.0f} in "
                      f"a new size (want 1, 1, 2, 3, 4); measured "
                      + ", ".join(f"{m:.0f}" for m in measured) + " (want 1, 1, 1, 2, 3)")
        return ok, detail

    # ---- styled runs
    @arm("ui-runs-draw")
    def _():
        p = _ui_probe("runs-draw")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            colors = [p.get((f"glyph{i}", "color")) for i in range(4)]
            red, own = [1.0, 0.0, 0.0], [0.92, 0.92, 0.92]
            plain_w, face_w = p[("face", "widths")]
            ok = (p[("glyphs", "count")][0] == 4 and colors[0] == own and colors[1] == red
                  and colors[2] == red and colors[3] == own and abs(face_w - plain_w) > 1.0)
            detail = (f"glyph colours {colors} (want own, red, red, own); the run's glyph in "
                      f"another face is {face_w:.2f} wide against {plain_w:.2f}")
        return ok, detail

    @arm("ui-runs-wrap")
    def _():
        p = _ui_probe("runs-wrap")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            plain_lines, face_lines = p[("lines", "plain")][0], p[("lines", "face")][0]
            right, width = p[("drawn", "right")][0], p[("label", "width")][0]
            ok = face_lines > plain_lines and right <= width + 0.5
            detail = (f"{plain_lines:.0f} lines in one face, {face_lines:.0f} with a word in a "
                      f"wider one (want more), drawn to {right:.2f} of {width:.2f}")
        return ok, detail

    @arm("ui-runs-refused")
    def _():
        p = _ui_probe("runs-refused")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            verdicts = {k: p[(k, "ok")][0] for k in ("valid", "order", "overlap", "past", "split")}
            kept, retext = p[("kept", "count")][0], p[("retext", "count")][0]
            ok = (verdicts == {"valid": 1, "order": 0, "overlap": 0, "past": 0, "split": 0}
                  and kept == 3 and retext == 0)
            detail = (f"accepted {verdicts} (want only valid), {kept:.0f} runs kept through the "
                      f"refusals (want 3), {retext:.0f} after new text (want 0)")
        return ok, detail

    # ---- the scrolling pane: 20 rows 30 tall, 8 apart, in a 300x200 pane at (490, 260)
    row_pitch, view, content = 38.0, 200.0, 20 * 30.0 + 19 * 8.0
    @arm("ui-scroll-layout")
    def _():
        p = _ui_probe("scroll-layout")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            px, py, _, _ = p[("pane", "rect")]
            at, moved = p[("rows", "at")], p[("moved", "at")]
            scroll = p[("wheeled", "scroll")][0]
            ok = (close(p[("content", "height")][0], content)
                  and all(close(a, py + i * row_pitch) for a, i in zip(at, (0, 1, 3)))
                  and scroll > 0 and all(close(m, a - scroll) for m, a in zip(moved, at)))
            detail = (f"content {p[('content', 'height')][0]:.1f} (want {content:.0f}), rows at "
                      f"{at} (want pitch {row_pitch:.0f} from {py:.0f}), wheeled {scroll:.1f} and "
                      f"every row moved up by it: {moved}")
        return ok, detail

    @arm("ui-scroll-wheel")
    def _():
        p = _ui_probe("scroll-wheel")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            step = p[("step", "points")][0]
            got = {k: p[(k, "scroll")] for k in ("three", "bottom", "top", "outside", "inner",
                                                 "outer")}
            ok = (got["three"] == [3 * step, 3 * step] and got["bottom"] == [content - view] * 2
                  and got["top"] == [0.0, 0.0] and got["outside"] == [0.0, 0.0]
                  and got["inner"][0] == 2 * step and got["outer"][0] == 0.0)
            detail = (f"three units {got['three'][0]:.1f} (want {3 * step:.0f}), the bottom "
                      f"{got['bottom'][0]:.1f} (want {content - view:.0f}), the top "
                      f"{got['top'][0]:.1f}, off the pane {got['outside'][0]:.1f} (want 0); "
                      f"nested, inner {got['inner'][0]:.1f} (want {2 * step:.0f}) and outer "
                      f"{got['outer'][0]:.1f} (want 0)")
        return ok, detail

    @arm("ui-scroll-clip")
    def _():
        p = _ui_probe("scroll-clip")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            cin, cout = p[("clicks", "in")][0], p[("clicks", "out")][0]
            oin, oout = p[("over", "in")][0], p[("over", "out")][0]
            drawn, outside = p[("rows", "drawn")][0], p[("rows", "outside")][0]
            # Rows 0 to 5 reach into the 200-point view: row 5 starts 190 down.
            ok = cin == 1 and cout == 0 and oin == 1 and oout == 0 and drawn == 6 and outside == 0
            detail = (f"clicks in view {cin:.0f} and below it {cout:.0f} (want 1, 0); over the UI "
                      f"{oin:.0f} and {oout:.0f} (want 1, 0); {drawn:.0f} rows emitted (want 6), "
                      f"{outside:.0f} wholly outside (want 0)")
        return ok, detail

    @arm("ui-scroll-into-view")
    def _():
        p = _ui_probe("scroll-into-view")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            down, up = p[("down", "scroll")][1], p[("up", "scroll")][1]
            to_set = p[("set", "scroll")][1]
            hover_focus, hover_moved = p[("hover", "focused")][0], p[("hover", "moved")][0]
            app = p.get(("app", "scroll"), [None, None])[1]
            app_focus = p.get(("app", "focus"), [None])[0]
            want_down = 5 * row_pitch + 30.0 - view
            want_set = 15 * row_pitch + 30.0 - view
            want_app = 2 * row_pitch
            ok = (p[("focus", "row5")][0] == 1 and close(down, want_down) and up == 0.0
                  and close(to_set, want_set) and hover_focus == 1 and hover_moved == 0.0
                  and app is not None and close(app, want_app) and app_focus == 1)
            detail = (f"down to row 5 scrolls {down:.1f} (want {want_down:.0f}), back up {up:.1f} "
                      f"(want 0), a focus set on row 15 {to_set:.1f} (want {want_set:.0f}); "
                      f"hovering row 5 focuses it ({hover_focus:.0f}) and scrolls "
                      f"{hover_moved:.1f} (want 0); the app showing row 2 scrolls to {app} (want "
                      f"{want_app:.0f}), the focus kept ({app_focus})")
        return ok, detail

    @arm("ui-scroll-follow")
    def _():
        p = _ui_probe("scroll-follow")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            first, grown = p[("first", "scroll")][0], p[("grown", "scroll")][0]
            up, kept = p[("up", "scroll")][0], p[("kept", "scroll")][0]
            h0, h1 = p[("content", "height")][0], p[("grown", "height")][0]
            ok = (close(first, h0 - view) and close(grown, h1 - view) and up < grown
                  and close(kept, up))
            detail = (f"begins at {first:.0f} (want its end {h0 - view:.0f}), grown {grown:.0f} "
                      f"(want {h1 - view:.0f}); scrolled up to {up:.0f}, a row added leaves it at "
                      f"{kept:.0f}")
        return ok, detail

    @arm("ui-scroll-bar")
    def _():
        p = _ui_probe("scroll-bar")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            px, py, pw, ph = p[("pane", "rect")]
            tx, ty, tw, th = p[("thumb", "rect")]
            track = ph - 4.0
            want_h = track * view / content
            dragged = p[("dragged", "scroll")][0]
            want_drag = 50.0 / (track - th) * (content - view)
            paged = p[("paged", "by")][0]
            ok = (close(th, want_h) and close(ty, py + 2.0) and close(tx, px + pw - 8.0)
                  and close(dragged, want_drag) and close(paged, 0.9 * view))
            detail = (f"thumb {th:.2f} tall (want {want_h:.2f}) at {ty:.1f} (want {py + 2:.0f}); "
                      f"dragged 50 points it scrolls {dragged:.2f} (want {want_drag:.2f}); a press "
                      f"below it pages {paged:.1f} (want {0.9 * view:.0f})")
        return ok, detail

    # ---- virtual rows: 30-point rows 8 points apart in a 200-point pane
    row_h, row_gap = 30.0, 8.0
    # The rows in view at most, and one either side.
    held_most = math.ceil(200.0 / (row_h + row_gap)) + 3
    @arm("ui-rows-built")
    def _():
        p = _ui_probe("rows-built")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            near, far = p[("near", "held")][0], p[("far", "held")][0]
            near_builds, far_builds = p[("near", "builds")][0], p[("far", "builds")][0]
            content = p[("near", "content")][0]
            want_content = 100000 * row_h + 99999 * row_gap
            ok = (close(content, want_content) and 0 < near <= held_most and near_builds == near
                  and 0 < far <= held_most and far_builds <= near_builds + far
                  and p[("far", "shown")][0] == 1 and p[("again", "builds")][0] == 0)
            detail = (f"content {content:.0f} (want {want_content:.0f}); {near:.0f} rows held at "
                      f"the top and {far:.0f} at row 50,000 (want at most {held_most}), "
                      f"{far_builds:.0f} built in all (want at most {near_builds + far:.0f}); row "
                      f"50,000 shown {p[('far', 'shown')][0]:.0f}, laid out again "
                      f"{p[('again', 'builds')][0]:.0f} built (want 0)")
        return ok, detail

    @arm("ui-rows-measure")
    def _():
        p = _ui_probe("rows-measure")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            count = p[("built", "count")][0]
            built = p[("built", "height")][0]
            content = p[("content", "height")][0]
            narrow, wide = p[("wider", "content")]
            wider = p[("wider", "builds")][0]
            # Every row is at least its estimate, and the built ones are at their measure.
            floor = built + (50 - count) * row_h + 49 * row_gap
            ok = (content > 50 * row_h + 49 * row_gap and content >= floor - 1e-3
                  and p[("again", "builds")][0] == 0 and close(narrow, content) and wide < narrow
                  and wider <= 2 and p[("changed", "builds")][0] == wider + 1)
            detail = (f"content {content:.0f} (want at least {floor:.0f}, the built rows' "
                      f"{built:.0f} and the rest at the estimate); laid out again "
                      f"{p[('again', 'builds')][0]:.0f} built (want 0); wider, {narrow:.0f} to "
                      f"{wide:.0f} with {wider:.0f} built (want fewer points, at most 2 built); a "
                      f"changed row {p[('changed', 'builds')][0]:.0f} (want {wider + 1:.0f})")
        return ok, detail

    @arm("ui-rows-anchor")
    def _():
        p = _ui_probe("rows-anchor")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            shown = p[("reveal", "shown")][0]
            bottoms = p.get(("reveal", "bottoms"), [0.0, -1.0])
            above, moved = p[("above", "height")][0], p[("first", "moved")][0]
            ok = (shown == 1 and close(bottoms[0], bottoms[1]) and close(above, 90.0)
                  and close(moved, 0))
            detail = (f"row 100 shown {shown:.0f}, its bottom {bottoms[0]:.1f} (want the pane's "
                      f"{bottoms[1]:.1f}); the row above the view built again {above:.0f} tall "
                      f"(want 90), the first row in view moved {moved:.2f} (want 0)")
        return ok, detail

    @arm("ui-rows-focus")
    def _():
        p = _ui_probe("rows-focus")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            held = p[("far", "held")][0]
            ok = (p[("first", "built")][0] == 1 and p[("far", "scroll")][0] > 1000.0
                  and p[("far", "kept")][0] == 1 and held <= held_most + 1)
            detail = (f"focused row built {p[('first', 'built')][0]:.0f}; scrolled to "
                      f"{p[('far', 'scroll')][0]:.0f}, kept {p[('far', 'kept')][0]:.0f} with "
                      f"{held:.0f} rows held (want at most {held_most + 1})")
        return ok, detail

    # ---- the table (spec 14.30)
    @arm("ui-table-columns")
    def _():
        p = _ui_probe("table-columns")
        if not p:
            return False, "the probe produced nothing"
        widest, title = p[("widest", "width")][0], p[("title", "width")][0]
        gap = p[("gap", "points")][0]
        wrong = []
        for width in (900, 701):
            row = p.get((f"w{width}row", "rect"))
            cols = [p.get((f"w{width}c{c}", "rect")) for c in range(6)]
            if not row or not all(cols):
                wrong.append(f"{width}: no rects")
                continue
            inner = row[2]
            share = math.floor(0.3 * (inner - 5 * gap))
            grow = (inner - 5 * gap - widest - title - 80 - share) / 2
            want = [widest, title, 80.0, share, grow, grow]
            at = 0.0
            for c, (got, w) in enumerate(zip(cols, want)):
                start = got[0] - row[0]
                if not close(got[2], w, 1e-3) or start != math.floor(at + 1e-4):
                    wrong.append(f"{width} column {c} at {start:.3f} {got[2]:.3f} wide "
                                 f"(want {math.floor(at + 1e-4):.0f} and {w:.3f})")
                at += w + gap
        ok = not wrong
        detail = (f"FIT {widest:.2f} (its widest cell, row 90,000), FIT {title:.2f} (its "
                  f"title), FIXED 80, SHARE 0.3 rounded down, two GROW halving the rest, "
                  f"{gap:.0f} apart at 900 and 701 points"
                  + (": " + "; ".join(wrong) if wrong else ": every rect as the rule gives it"))
        return ok, detail

    @arm("ui-table-measure")
    def _():
        p = _ui_probe("table-measure")
        if not p:
            return False, "the probe produced nothing"
        want = {("first", "measures"): 400000, ("again", "measures"): 0,
                ("changed", "measures"): 4, ("shrunk", "measures"): 4,
                ("grown", "measures"): 40, ("shrunk", "row7"): 0}
        wrong = _probe_wrong(p, want)
        shrunk, second = p[("shrunk", "width")][0], p[("second", "width")][0]
        widest = p[("widest", "width")][0]
        ok = not wrong and close(shrunk, second, 1e-3) and widest > shrunk
        detail = (f"measured {p[('first', 'measures')][0]:.0f} cells (want 400000), again "
                  f"{p[('again', 'measures')][0]:.0f} (want 0), a row changed "
                  f"{p[('changed', 'measures')][0]:.0f} (want 4); the widest shrunk, "
                  f"{widest:.2f} to {shrunk:.2f} (want the next widest, {second:.2f}) with "
                  f"{p[('shrunk', 'measures')][0]:.0f} measured (want 4); ten rows added "
                  f"{p[('grown', 'measures')][0]:.0f} (want 40); shrunk to 5 rows, row 7 "
                  f"{'still placed' if p.get(('shrunk', 'row7'), [1])[0] else 'gone'} (want gone)")
        return ok, detail

    @arm("ui-table-heights")
    def _():
        p = _ui_probe("table-heights")
        if not p:
            return False, "the probe produced nothing"
        line, pad = p[("line", "height")][0], p[("row", "padding")][0]
        lined, own = p[("lined", "heights")], p[("own", "heights")]
        one, three = line + 2 * pad, 3 * line + 2 * pad
        meets = [p[("lined", "meet")][0], p[("own", "meet")][0]]
        header, bare = p[("lined", "header")][0], p[("own", "header")][0]
        pixel = p[("pixel", "points")][0]
        rule, scrolled_rule = p[("rule", "top")][0], p[("scrolled", "rule")][0]
        scrolled, row37 = p[("scrolled", "by")][0], p[("scrolled", "row37")][0]
        # Row 37's top: rows of one line but row 1, of three.
        want37 = one + (one + three + 35 * one) - 1000.0
        # The source's heights doubled and asked again alone (spec 14.32): no cell measured.
        doubled = p.get(("doubled", "heights"), [0.0, 0.0])
        doubled_measures = p.get(("doubled", "measures"), [-1])[0]
        ok = (close(lined[0], one) and close(lined[1], three) and close(own[0], 50.0)
              and close(own[1], 51.0) and all(close(m, 0.0) for m in meets)
              and close(header, one) and close(bare, 0.0) and close(scrolled, 1000.0)
              and close(rule, one - pixel) and close(scrolled_rule, rule)
              and close(row37, want37) and close(doubled[0], 100.0)
              and close(doubled[1], 102.0) and doubled_measures == 0)
        detail = (f"rows of one and three lines {lined[0]:.2f} and {lined[1]:.2f} (want "
                  f"{one:.2f} and {three:.2f}), the source's {own[0]:.0f} and {own[1]:.0f} "
                  f"(want 50 and 51), meeting {meets[0]:.2f} and {meets[1]:.2f} apart (want 0); "
                  f"the first row {header:.2f} under the content's top (want the header's "
                  f"{one:.2f}), and {bare:.2f} with no titles (want 0); scrolled {scrolled:.0f} "
                  f"(want 1000), the rule at {rule:.2f} then {scrolled_rule:.2f} (want "
                  f"{one - pixel:.2f} both), row 37 at {row37:.2f} (want {want37:.2f}); the "
                  f"source's doubled and asked again alone {doubled[0]:.0f} and {doubled[1]:.0f} "
                  f"(want 100 and 102) with {doubled_measures:.0f} cells measured (want 0)")
        return ok, detail

    @arm("ui-table-view")
    def _():
        p = _ui_probe("table-view")
        if not p:
            return False, "the probe produced nothing"
        calls, far_calls = p[("drawn", "calls")][0], p[("far", "calls")][0]
        columns = p[("text", "columns")][0]
        row_h, view = p[("row", "height")][0], p[("view", "height")][0]
        most = (math.ceil(view / row_h) + 1) * columns
        least = math.floor(view / row_h) * columns
        bottom, view_bottom = p.get(("far", "bottom"), [0.0, -1.0])
        ok = (least <= calls <= most and least <= far_calls <= most
              and p[("far", "shown")][0] == 1 and close(bottom, view_bottom))
        detail = (f"{calls:.0f} cells asked at the top and {far_calls:.0f} at row 50,000 (want "
                  f"{least:.0f} to {most:.0f}: {view:.0f} points of {row_h:.0f}-point rows by "
                  f"{columns:.0f} columns); row 50,000 shown {p[('far', 'shown')][0]:.0f}, its "
                  f"bottom {bottom:.1f} (want the view's {view_bottom:.1f})")
        return ok, detail

    @arm("ui-table-refused")
    def _():
        p = _ui_probe("table-refused")
        if not p:
            return False, "the probe produced nothing"
        want = {("set", "kept"): 1, ("none", "kept"): 1, ("many", "kept"): 1,
                ("drawnfit", "kept"): 1, ("child", "made"): 0, ("child", "count"): 0,
                ("child", "kept"): 1, ("most", "last"): 1, ("past", "cursor"): 3,
                ("negative", "cursor"): 3, ("unheld", "cursor"): 3}
        wrong = _probe_wrong(p, want)
        ok = not wrong
        detail = ("no columns, 33, a drawn FIT column and a child each refused with the "
                  "columns kept, a cursor past the rows, below none and on a row that cannot "
                  "hold it refused with the cursor kept, 32 columns taken"
                  + (f"; wrong: {wrong}" if wrong else ""))
        return ok, detail

    @arm("ui-table-cursor")
    def _():
        p = _ui_probe("table-cursor")
        if not p:
            return False, "the probe produced nothing"
        row_h, view = p[("row", "height")][0], p[("view", "height")][0]
        # Page Down from row 1: the row a view below its top, or the next holding the cursor.
        paged = math.floor((row_h + view) / row_h)
        while paged % 4 == 0:
            paged += 1
        steps = [("none", -1, 0), ("first", 1, 1), ("down3", 5, 1), ("up", 3, 1),
                 ("end", 99, 1), ("endagain", 99, 0), ("home", 1, 1), ("pagedown", paged, 1),
                 ("set", 50, 0), ("press", 49, 1), ("pressno", 49, 0), ("upnone", 99, 1),
                 ("moved", 97, 1), ("hooked", 97, 0)]
        want = {}
        for label, row, calls in steps:
            want[(label, "row")] = row
            want[(label, "calls")] = calls
            want[(label, "shown")] = 0 if row < 0 else 1
        wrong = _probe_wrong(p, want)
        ok = not wrong
        detail = ("rows that are multiples of 4 cannot hold the cursor; "
                  + (f"wrong: {wrong}" if wrong else
                     f"every step lands where it should, Page Down on {paged}, each row shown "
                     f"and on_cursor once a pass the person moved it"))
        return ok, detail

    @arm("ui-table-pane")
    def _():
        p = _ui_probe("table-pane")
        if not p or ("centred", "gaps") not in p:
            return False, "the probe produced nothing"
        tall, rows = p[("table", "height")]
        wheel_pane, wheel_table = p[("wheel", "table")]
        held, wrong, row0 = (p[("held", k)][0] for k in ("top", "wrong", "row0"))
        calls, shown, columns = (p[("drawn", k)][0] for k in ("calls", "shown", "columns"))
        side = p[("wheel", "side")][0]
        cursor, under, foot = (p[("reveal", k)][0] for k in ("cursor", "under", "foot"))
        paged, page_want = p[("page", "cursor")]
        kept, refocused = p[("focus", "kept")]
        bar = p[("bar", "scroll")][0]
        bar_cursor, cursor_before = p[("bar", "cursor")]
        above, below = p[("centred", "gaps")]
        checks = {
            "the table as tall as its header and rows": close(tall, rows, 1e-3),
            "the wheel over the table scrolls the pane 300": close(wheel_pane, 300.0, 1e-3)
            and wheel_table == 0.0,
            "the header held at the pane's top": close(held, 0.0, 1e-3),
            "each row shown exactly when below the header": wrong == 0 and row0 == 0,
            "only the rows shown drawn": 0 < calls <= (shown + 1) * columns,
            "the wheel over the panel scrolls the pane 100": close(side, 100.0, 1e-3),
            "row 30 shown whole under the header": cursor == 30 and under >= -1e-3
            and foot >= -1e-3 and min(under, foot) <= 1e-3,
            "Page Down by the rows shown": paged == page_want,
            "the focus given back keeps the cursor's row": kept > 0.0
            and close(refocused, kept, 1e-3),
            "the pane's bar pressed over the table": bar > 0.0 and bar_cursor == cursor_before,
            "a table shorter than its pane centred": abs(above - below) <= 1.0 and above > 0.0,
        }
        failed = [name for name, passed in checks.items() if not passed]
        detail = (f"held {held:+.2f} from the pane's top, {wrong:.0f} rows shown wrongly; "
                  f"{calls:.0f} cells drawn for {shown:.0f} rows shown of {columns:.0f} columns; "
                  f"the wheel moved the pane {wheel_pane:.0f} over the table and {side:.0f} over "
                  f"the panel; row {cursor:.0f} {under:.2f} under the header and {foot:.2f} above "
                  f"the foot; Page Down to {paged:.0f} (want {page_want:.0f}); the focus given "
                  f"back left the pane at {refocused:.0f} (want {kept:.0f}); the bar pressed "
                  f"paged the pane to {bar:.0f} with the cursor on {bar_cursor:.0f} (want "
                  f"{cursor_before:.0f}); centred {above:.1f} and {below:.1f}"
                  + (f"; failed: {', '.join(failed)}" if failed else ""))
        return not failed, detail

    # ---- the most an element measures, and lengths that are none
    @arm("ui-max-size")
    def _():
        p = _ui_probe("max-size")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            h, content = p[("pane", "height")][0], p[("pane", "content")][0]
            scrolled = p[("pane", "scrolled")][0]
            held, rest = p[("held", "width")][0], p[("rest", "widths")]
            both, offset = p[("centred", "widths")], p[("centred", "offset")][0]
            fixed = p[("fixed", "width")][0]
            want_content = 30 * row_h + 29 * row_gap
            ok = (close(h, 200.0) and close(content, want_content) and scrolled > 0
                  and close(held, 100.0) and all(close(w, 250.0) for w in rest)
                  and all(close(w, 100.0) for w in both) and close(offset, 200.0)
                  and close(fixed, 50.0))
            detail = (f"a fitting pane held to 200 is {h:.0f} tall over {content:.0f} of content "
                      f"(want {want_content:.0f}), and the wheel scrolls it {scrolled:.0f}; a "
                      f"growing child held to 100 takes {held:.0f} and its siblings {rest} (want "
                      f"250 each); two held to 100 and centred {both} at {offset:.0f} (want 200); "
                      f"a fixed one with a most of 10 keeps {fixed:.0f} (want 50)")
        return ok, detail

    # ---- a size as a share of the parent; every length here is exact in float, so == holds
    @arm("ui-share")
    def _():
        p = _ui_probe("share")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            def rect(label, key):
                return p.get((label, key), [float("nan")] * 4)

            def share_of(f, room):
                return max(math.floor(np.float32(f) * np.float32(room)), 0)

            pad, gap, wrong = 8.0, 6.0, []
            for label in ("split0", "split1", "split2", "nested"):
                row, a, b = rect(label, "row"), rect(label, "share"), rect(label, "grow")
                room = row[2] - 2 * pad - gap
                want = share_of(0.7, room)
                if (a[0], a[2], b[0], b[2]) != (row[0] + pad, want, a[0] + want + gap, room - want):
                    wrong.append(f"{label} {a[2]:g} + {b[2]:g} in {room:g} (want {want} + "
                                 f"{room - want:g})")
            row, a = rect("across", "row"), rect("across", "share")
            if (a[1], a[3]) != (row[1], share_of(0.5, row[3])):
                wrong.append(f"across {a[3]:g} of {row[3]:g} (want {share_of(0.5, row[3])})")
            row, a, b = rect("held", "row"), rect("held", "share"), rect("held", "grow")
            room = row[2] - 2 * pad - gap
            if (a[2], b[2]) != (100.0, room - 100.0):
                wrong.append(f"held {a[2]:g} + {b[2]:g} (want 100 + {room - 100:g})")
            row, a, b = rect("halves", "row"), rect("halves", "a"), rect("halves", "b")
            room = row[2] - 2 * pad - gap
            left = row[0] + row[2] - pad - (b[0] + b[2])
            if (a[2], b[2], b[0], left) != (share_of(0.5, room),) * 2 + (a[0] + a[2] + gap, 1.0):
                wrong.append(f"halves {a[2]:g} + {b[2]:g} leaving {left:g} (want 1)")
            row, a, b = rect("over", "row"), rect("over", "a"), rect("over", "b")
            room = row[2] - 2 * pad - gap
            if (a[2], b[2], b[0]) != (share_of(0.7, room),) * 2 + (a[0] + a[2] + gap,):
                wrong.append(f"over {a[2]:g} + {b[2]:g} at {b[0] - row[0]:g}")
            row, a = rect("fit", "row"), rect("fit", "share")
            if (row[2], a[0], a[2]) != (100.0, row[0] + 100.0, 50.0):
                wrong.append(f"fit: the row {row[2]:g} (want 100), its share {a[2]:g} (want 50)")
            row, a, b = rect("zero", "row"), rect("zero", "share"), rect("zero", "grow")
            room = row[2] - 2 * pad - gap
            if (a[2], b[0], b[2]) != (0.0, a[0] + gap, room):
                wrong.append(f"zero {a[2]:g} + {b[2]:g} (want 0 + {room:g})")
            ok = not wrong
            splits = ", ".join(f"{rect(k, 'share')[2]:g} + {rect(k, 'grow')[2]:g}"
                               for k in ("split0", "split1", "split2"))
            detail = ("; ".join(wrong) if wrong else
                      f"0.7 and GROW in 578, 579 and 311.5 points: {splits}; nested, across, held, "
                      f"two halves leaving a point, two past 1, under a FIT parent and of 0 as the "
                      f"rule says")
        return ok, detail

    # ---- a panel measured by its app (spec 14.33): each size from its answer, its child, its text
    # or its most, and its padding
    @arm("ui-measure")
    def _():
        p = _ui_probe("measure")
        if not p or ("label", "kept") not in p:
            return False, "the probe produced nothing"

        def size(label):
            return tuple(p.get((label, "rect"), [float("nan")] * 4)[2:])

        pad, gap = 6.0, 8.0
        # Width and height, None where the height is not the check's; texted and the label as
        # their twins with no measure are.
        want = {"first": (37 + 2 * pad, 3000 / 37 + 2 * pad), "fixed": (112, 3000 / 100 + 2 * pad),
                "holding": (80 + 2 * pad, 20 + 2 * pad), "grow": (55 + 2 * pad, None),
                "held": (200, None), "negative": (2 * pad, 2 * pad), "nan": (2 * pad, 2 * pad),
                "cleared": (2 * pad, 2 * pad), "texted": size("textedtwin"),
                "label": size("twin")}
        asked = {"first": [1, 1], "fixed": [0, 1], "cleared": [0, 0]}
        wrong = [f"{k} {size(k)[0]:g} by {size(k)[1]:g} (want {w:g} by "
                 f"{'any' if h is None else format(h, 'g')})"
                 for k, (w, h) in want.items()
                 if not close(size(k)[0], w, 1e-3)
                 or (h is not None and not close(size(k)[1], h, 1e-3))]
        wrong += [f"{k} asked {p[(k, 'asked')]} (want {n})" for k, n in asked.items()
                  if p[(k, "asked")] != n]
        first, after = p[("first", "rect")], p[("after", "rect")]
        if after[0] != first[0] + first[2] + gap:
            wrong.append(f"the label after the first {after[0] - first[0]:g} on (want "
                         f"{first[2] + gap:g})")
        if p[("label", "kept")][0] != 1:
            wrong.append("a label took a measure")
        detail = ("; ".join(wrong) if wrong else
                  f"{len(want)} sizes as their answers, children, text, most and padding give them, "
                  f"the label after the first {gap:g} on, asked {asked}")
        return not wrong, detail

    @arm("ui-padding-none")
    def _():
        p = _ui_probe("padding-none")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            none, inherit, sides = (p[(k, "rect")] for k in ("none", "inherit", "sides"))
            first = p[("first", "offset")]
            ok = (close(none[2], 20.0) and close(none[3], 40.0) and first == [0.0, 0.0]
                  and inherit[2] > 20.0 and inherit[3] > 40.0
                  and close(sides[2], 32.0) and close(sides[3], 20.0))
            detail = (f"negative padding and spacing: two 20-point children in {none[2]:.0f} x "
                      f"{none[3]:.0f} (want 20 x 40) from its corner {first}; zero inherits, "
                      f"{inherit[2]:.0f} x {inherit[3]:.0f} (want more); none above and below and "
                      f"6 either side, {sides[2]:.0f} x {sides[3]:.0f} (want 32 x 20)")
        return ok, detail

    # A glyph's corner is snapped to the framebuffer's pixels, so a baseline or a pen read back
    # from it lies within half a pixel of where it was drawn.
    @arm("ui-label-padding")
    def _():
        p = _ui_probe("label-padding")
        if not p or ("baseline", "off") not in p:
            ok, detail = False, "the probe produced nothing"
        else:
            snap = p[("pixel", "size")][0] * 0.5 + 0.01
            base, pen = p[("baseline", "off")][0], p[("pen", "off")][0]
            ok = abs(base) <= snap and abs(pen) <= snap
            detail = (f"padding 10 above and 12 left: the baseline {base:+.2f} and the pen "
                      f"{pen:+.2f} points off their place inside it (want within {snap:.2f})")
        return ok, detail

    @arm("ui-baseline")
    def _():
        p = _ui_probe("baseline")
        count = p.get(("glyphs", "count"), [0])[0] if p else 0
        if count != 7:
            ok, detail = False, f"the probe drew {count} glyphs (want 7)"
        else:
            snap = p[("pixel", "size")][0] + 0.02
            lines = [p[(f"glyph{i}", "line")][0] for i in range(7)]
            spread = max(lines) - min(lines)
            own, drawn = p[("row", "own")]
            top = p[("textless", "top")][0]
            height, reach = p[("row", "height")]
            ok = (spread <= snap and abs(own - drawn) <= snap and abs(top) <= 0.01
                  and abs(height - reach) <= 0.01)
            detail = (f"baselines at {', '.join(f'{v:.2f}' for v in lines)}, {spread:.2f} apart "
                      f"(want within {snap:.2f}); the row's own at {own:.2f}; the textless panel "
                      f"{top:+.2f} from the row's top (want 0); the row {height:.2f} tall, its "
                      f"lowest child reaching {reach:.2f}")
        return ok, detail

    @arm("ui-line-ascent")
    def _():
        p = _ui_probe("line-ascent")
        count = p.get(("glyphs", "count"), [0])[0] if p else 0
        if count != 2:
            return False, f"the probe drew {count} glyphs (want 2)"
        snap = p[("pixel", "size")][0] * 0.5 + 0.01
        faces = [p[(f"face{i}", "off")][0] for i in range(2)]
        glyphs = [p[(f"glyph{i}", "off")][0] for i in range(2)]
        none = p[("none", "face")][0]
        ok = (all(abs(v) <= 1e-4 for v in faces) and all(abs(v) <= snap for v in glyphs)
              and none == 0.0)
        detail = (f"at 14 and 28 points, {faces[0]:+.4f} and {faces[1]:+.4f} off the face's ascent "
                  f"(want 0); each label's glyph {glyphs[0]:+.2f} and {glyphs[1]:+.2f} off its top "
                  f"and that (want within {snap:.2f}); with no face {none:.2f} (want 0)")
        return ok, detail

    @arm("ui-control-lines")
    def _():
        p = _ui_probe("control-lines")
        counts = p.get(("rows", "count")) if p else None
        if not counts:
            ok, detail = False, "the probe produced nothing"
        else:
            one = p.get(("one", "offset"), [None])[0]
            two = p.get(("two", "offset"), [None])[0]
            clear = p.get(("slider", "clear"), [None])[0]
            ok = (counts == [1, 2, 2] and one is not None and two is not None
                  and abs(two - one) <= 0.01 and clear is not None and clear > 0)
            detail = (f"rows {counts} (want 1, 2, 2); a row's centre off the button's centre {one} "
                      f"for one line and {two} for two lines' middle (want equal); the slider's "
                      f"track {clear} points below its second row (want over 0)")
        return ok, detail

    @arm("ui-text-mesh")
    def _():
        p = _ui_probe("text-mesh")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            greens = [p.get((f"quad{q}", "green"), [None])[0] for q in range(5)]
            want = {("chars", "count"): 5, ("quads", "count"): 5}
            wrong = _probe_wrong(p, want)
            ok = not wrong and greens == [1, 0, 1, 1, 1]
            detail = (f"5 characters and 5 quads, only the é's coloured (greens {greens})" if ok
                      else f"wrong: {wrong}; greens {greens} (want 1, 0, 1, 1, 1)")
        return ok, detail

    # ---- the text field: each probe line says 1 where its text was the one it wanted
    def flags_ok(p, keys):
        return not _probe_wrong(p, {k: 1 for k in keys})

    @arm("ui-field-type")
    def _():
        p = _ui_probe("field-type")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            caret = p[("first", "caret")]
            changes = [p[(k, "changes")][0] for k in ("first", "second", "idle")]
            ok = (flags_ok(p, [("first", "text"), ("second", "text")]) and caret[0] == 5
                  and changes == [1, 2, 2])
            detail = (f"typed 'hello' then ' world', the caret at {caret[0]:.0f} (want 5), "
                      f"on_change {changes} (want 1, 2, 2)")
        return ok, detail

    @arm("ui-field-edit")
    def _():
        p = _ui_probe("field-edit")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            carets = [p[(k, "caret")][0] for k in ("left", "word", "home", "end", "delete")]
            ok = (carets == [9, 6, 0, 11, 0]
                  and flags_ok(p, [("backspace", "text"), ("wordback", "text"),
                                   ("delete", "text")]))
            detail = (f"carets after left, a word left, Home, End and Delete {carets} (want 9, 6, "
                      f"0, 11, 0); Backspace, a word's Backspace and Delete left the texts they "
                      f"should")
        return ok, detail

    @arm("ui-field-utf8")
    def _():
        p = _ui_probe("field-utf8")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            typed, erased = p[("typed", "caret")], p[("erased", "caret")]
            lefts = [p[("left1", "caret")][0], p[("left2", "caret")][0]]
            cps = p[("erased", "codepoints")]
            ok = (typed[0] == 9 and typed[3] == 9 and lefts == [5, 3] and erased[0] == 1
                  and erased[3] == 7 and cps == [97, 937, 128512])
            detail = (f"typed a, é, Ω and a four-byte codepoint: {typed[3]:.0f} bytes, caret "
                      f"{typed[0]:.0f} (want 9, 9); left twice {lefts} (want 5, 3); Backspace "
                      f"leaves {erased[3]:.0f} bytes and caret {erased[0]:.0f} (want 7, 1), "
                      f"codepoints {cps}")
        return ok, detail

    @arm("ui-field-select")
    def _():
        p = _ui_probe("field-select")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            sel = {k: p[(k, "caret")][1:3] for k in ("shift", "all", "double", "triple")}
            ok = (sel["shift"] == [0, 5] and sel["all"] == [0, 16] and sel["double"] == [6, 10]
                  and sel["triple"] == [0, 16] and p[("runs", "ok")][0] == 1
                  and p[("copied", "plain")][0] == 1 and p[("editable", "runs")][0] == 0)
            detail = (f"selections {sel} (want shift 0-5, all 0-16, double 6-10, triple 0-16); a "
                      f"read-only field took runs and copied plain text; an editable one refused "
                      f"runs")
        return ok, detail

    @arm("ui-field-clipboard")
    def _():
        p = _ui_probe("field-clipboard")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            keys = [("copied", "clip"), ("pasted", "text"), ("flattened", "text"), ("cut", "text"),
                    ("cutcopied", "clip"), ("rocopied", "clip"), ("readonly", "text")]
            wrong = _probe_wrong(p, {k: 1 for k in keys})
            ok = not wrong
            detail = ("copy, paste, a flattened newline, cut, and a read-only field's copy that "
                      "neither cuts nor pastes, each as it should be" if ok else f"wrong: {wrong}")
        return ok, detail

    @arm("ui-field-undo")
    def _():
        p = _ui_probe("field-undo")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            steps = ("undo1", "undo2", "redo1", "redo2")
            wrong = _probe_wrong(p, {(k, "text"): 1 for k in steps})
            ok = not wrong
            detail = ("'abc def' undoes to 'abc' then nothing, and redoes back a word at a time"
                      if ok else f"the text was not the one wanted after {list(wrong)} (want "
                      f"'abc', '', 'abc', 'abc def'; the probe's stderr names each)")
        return ok, detail

    @arm("ui-presses")
    def _():
        p = _ui_probe("presses")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            got = {k: p.get((k, "count")) for k in ("run", "apart", "cycle", "ended")}
            ok = (got["run"] == [1, 2, 3] and got["apart"] == [1, 1]
                  and got["cycle"] == [1, 2, 3, 1] and got["ended"] == [1])
            detail = f"counts {got} (want run 1 2 3, apart 1 1, cycle 1 2 3 1, ended 1)"
        return ok, detail

    @arm("ui-field-caret-x")
    def _():
        p = _ui_probe("field-caret-x")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            worst = p[("worst", "off")][0]
            ok = worst <= 0.5
            detail = (f"the caret at most {worst:.3f} points from where the glyph at its index is "
                      f"drawn (want within the pixel snap, 0.5)")
        return ok, detail

    @arm("ui-field-scroll-x")
    def _():
        p = _ui_probe("field-scroll-x")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            left, right = p[("content", "edges")]
            end, home = p[("end", "caret")][0], p[("home", "caret")][0]
            end_scroll, home_scroll = p[("end", "scroll")][0], p[("home", "scroll")][0]
            ok = (left <= end <= right and end_scroll > 0 and close(home, left)
                  and home_scroll == 0)
            detail = (f"at the end the caret is at {end:.1f} inside {left:.0f}-{right:.0f}, "
                      f"scrolled {end_scroll:.1f}; Home puts it at {home:.1f} with the scroll "
                      f"{home_scroll:.1f}")
        return ok, detail

    @arm("ui-field-multiline")
    def _():
        p = _ui_probe("field-multiline")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            rows, lh = p[("rows", "count")][0], p[("line", "height")][0]
            h = p[("field", "rect")][3]
            x0, x1 = p[("up", "x")]
            dy = p[("up", "dy")][0]
            end = p.get(("end", "caret"), [None])[0]
            nomost_rows = p.get(("nomost", "rows"), [0])[0]
            nomost_h = p.get(("nomost", "height"), [0])[0]
            paged = p.get(("page", "rows"), [None])[0]
            ok = (rows > 3 and close(h, 3 * lh + 16.0) and abs(x0 - x1) < 12.0 and close(dy, lh)
                  and p[("home", "caret")][0] > 0 and p[("home", "space")][0] == 1 and end == 6
                  and nomost_rows > 3 and close(nomost_h, nomost_rows * lh + 16.0)
                  and paged is not None and close(paged, 3.0))
            detail = (f"{rows:.0f} rows (want over 3) in {h:.1f} points (want {3 * lh + 16:.0f}); "
                      f"up a row the caret's x moves {abs(x0 - x1):.2f} (want under a glyph) and "
                      f"its y {dy:.1f} (want {lh:.0f}); Home at {p[('home', 'caret')][0]:.0f}, "
                      f"a word's start; End on 'abc   ' at {end} (want 6, past its spaces); "
                      f"with no most {nomost_rows:.0f} rows in {nomost_h:.0f} points (want "
                      f"{nomost_rows * lh + 16:.0f}); Page Down over three rows shown moves "
                      f"{paged} rows (want 3)")
        return ok, detail

    @arm("ui-field-placeholder")
    def _():
        p = _ui_probe("field-placeholder")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            empty, dim = p[("empty", "glyphs")][0], p[("empty", "dim")][0]
            typed, typed_dim = p[("typed", "glyphs")][0], p[("typed", "dim")][0]
            # The full-strength glyphs on the empty field are the fixture's button; typed, "ok"
            # joins them and the hint goes.
            flags = flags_ok(p, [("size", "same"), ("settext", "refused")])
            ok = flags and dim > 0 and typed_dim == 0 and typed == empty - dim + 2
            detail = (f"the hint sizes nothing and ui_set_text on a field is refused "
                      f"({'yes' if flags else 'no'}); {dim:.0f} of {empty:.0f} glyphs dim while "
                      f"empty, {typed_dim:.0f} once 'ok' is typed (want 0) with {typed:.0f} glyphs "
                      f"(want {empty - dim + 2:.0f})")
        return ok, detail

    @arm("ui-field-append")
    def _():
        p = _ui_probe("field-append")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            want = {("found", "row"): 1, ("found", "none"): 1, ("added", "ok"): 1,
                    ("joined", "text"): 1, ("selection", "kept"): 1, ("run", "start"): 6,
                    ("row", "builds"): 0, ("row", "grew"): 1, ("toolong", "refused"): 1,
                    ("editable", "refused"): 1}
            wrong = _probe_wrong(p, want)
            rows, fresh, was = (p.get(("restyled", "rows")) or [0, 0, 0])[:3]
            restyled = rows == fresh and fresh != was
            ok = not wrong and restyled
            detail = ("the built row found and none past it; two appends joined, the selection and "
                      "the appended run's place (byte 6) kept, the row grown and not built again; "
                      "past the most refused whole; runs refused on an editable field; "
                      if not wrong else f"wrong: {wrong}; ") + (
                      f"restyled and appended to before a layout, {rows:.0f} rows against "
                      f"{fresh:.0f} broken from scratch, {was:.0f} before (want equal, and not "
                      f"{was:.0f})")
        return ok, detail

    @arm("ui-field-keys")
    def _():
        p = _ui_probe("field-keys")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            want = {("enter", "submits"): 1, ("hook", "taken"): 1, ("hook", "caret"): 3,
                    ("tab", "button"): 1, ("backtab", "field"): 1, ("tabcarry", "text"): 1,
                    ("escape", "focus"): 1, ("escape", "onstack"): 1, ("before", "text"): 1,
                    ("newline", "text"): 1, ("chat", "submits"): 1, ("modal", "before"): 1,
                    ("modal", "after"): 0}
            wrong = _probe_wrong(p, want)
            ok = not wrong
            detail = "every key did what it should" if ok else f"wrong: {wrong}"
        return ok, detail

    @arm("ui-field-focus")
    def _():
        p = _ui_probe("field-focus")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            want = {("wants", "editing"): 1, ("hover", "kept"): 1, ("nothing", "focus"): 1,
                    ("wants", "after"): 0, ("click", "field"): 1, ("click", "button"): 1,
                    ("wants", "button"): 0}
            wrong = _probe_wrong(p, want)
            ok = not wrong
            detail = "the focus went where it should" if ok else f"wrong: {wrong}"
        return ok, detail

    # ---- the matcher
    @arm("ui-match")
    def _():
        p = _ui_probe("match")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            s = {k: p[(k, "score")][0]
                 for k in ("none", "starts", "inside", "upper", "best", "utf8", "empty")}
            counts = [p.get((k, "count"), [None])[0] for k in ("none", "starts", "utf8", "empty")]
            ok = (s["none"] == -1 and s["starts"] > s["inside"] > 0 and s["upper"] == s["starts"]
                  and p[("best", "at")] == [5, 6] and p[("utf8", "at")] == [3, 4, 5]
                  and p[("starts", "at")] == [0, 6] and s["empty"] == 0 and counts == [0, 2, 3, 0])
            detail = (f"scores {s}; 'st' in 'fast stop' at {p.get(('best', 'at'))} (want 5, 6), "
                      f"'sum' in 'résumé' at {p.get(('utf8', 'at'))} (want bytes 3, 4, 5); "
                      f"positions written for no match, 'rn', 'sum' and '' {counts} "
                      f"(want 0, 2, 3, 0)")
        return ok, detail

    @arm("ui-utf8")
    def _():
        p = _ui_probe("utf8")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            want = {("roundtrip", "ok"): 7, ("encode", "refused"): 3, ("mirror", "steps"): 15,
                    ("mirror", "off"): 0, ("start", "returns"): 0, ("start", "moved"): 0}
            wrong = _probe_wrong(p, want)
            replaced, length = p[("malformed", "fffd")][0], p[("malformed", "bytes")][0]
            ok = not wrong and replaced == length
            detail = (f"7 edge codepoints round-trip, 3 non-characters refused, {replaced:.0f} "
                      f"U+FFFD from {length:.0f} malformed bytes (want one a byte), prev mirrors "
                      f"next at every step" if ok else f"wrong: {wrong}; {replaced:.0f} U+FFFD "
                      f"from {length:.0f} malformed bytes (want one a byte)")
        return ok, detail

    # ---- the code editor (spec 14.4): every probe line a 1, and as many as the case says
    def doc_flags(case, lines, what, env=None):
        p = _ui_probe(case, env=env)
        if not p:
            return False, "the probe produced nothing"
        wrong = _probe_wrong(p, {k: 1 for k in p})
        if wrong or len(p) != lines:
            return False, f"wrong: {wrong}; {len(p)} lines of {lines}"
        return True, what

    @arm("ui-doc-roundtrip")
    def _():
        ok, detail = doc_flags("doc-roundtrip", 30, "LF, CRLF, mixed, a lone CR, a byte-order "
                               "mark, no final newline and nothing: each byte-exact loaded, edited "
                               "and undone, and the line ending found")
        return ok, detail

    @arm("ui-doc-random")
    def _():
        p = _ui_probe("doc-random")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            steps, wrong = p[("checked", "steps")][0], p[("wrong", "count")][0]
            undone = p[("undone", "steps")][0]
            ok = (steps == 3000 and wrong == 0 and undone > 0 and p[("undone", "first")][0] == 1
                  and p[("redone", "last")][0] == 1)
            detail = (f"{steps:.0f} seeded replaces, {wrong:.0f} disagreeing with a flat string in "
                      f"its text, lines, line starts, line_of or copies; {undone:.0f} steps undone "
                      f"to the first text {p[('undone', 'first')][0]:.0f}, redone to the last "
                      f"{p[('redone', 'last')][0]:.0f}")
        return ok, detail

    @arm("ui-doc-map")
    def _():
        ok, detail = doc_flags("doc-map", 13, "offsets and ranges held at an old version land "
                               "where the changes since put them, cut or dropped inside replaced "
                               "bytes, and follow later changes; a version too new or too old is "
                               "refused")
        return ok, detail

    @arm("ui-doc-set-text")
    def _():
        ok, detail = doc_flags("doc-set-text", 7, "a whole new text lands as the one change "
                               "between the two, on characters' edges, a caret moved through it; "
                               "the same text is no change; the host's edit inside a character is "
                               "refused")
        return ok, detail

    @arm("ui-doc-undo")
    def _():
        ok, detail = doc_flags("doc-undo", 27, "typing joins into words, a sealed caret splits it, "
                               "two carets are one step, undo and redo restore the text and the "
                               "carets, with keep_undo off nothing is kept, a change asked for "
                               "inside on_action is refused, and a host's edit joined to the latest "
                               "step is undone and redone with it, a step of its own after an undo, "
                               "typing after it a step of its own, a group opened before it "
                               "starting where its first action finds the caret")
        return ok, detail

    @arm("ui-doc-read-only")
    def _():
        ok, detail = doc_flags("doc-read-only", 6, "typing at two carets refused as one action and "
                               "told to no one but on_refused, an editor's undo and redo refused, "
                               "the host's edits, undo and redo taken, and nothing different "
                               "writable")
        return ok, detail

    @arm("ui-doc-highlight")
    def _():
        ok, detail = doc_flags("doc-highlight", 8, "a line is coloured once until it changes; a "
                               "stateful line waits on the walk, and a changed line asks again of "
                               "the lines after it only while the state it leaves has changed")
        return ok, detail

    @arm("ui-glyph-cache")
    def _():
        p = _ui_probe("glyph-cache")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            checked, wrong = p[("glyphs", "checked")][0], p[("glyphs", "wrong")][0]
            flags = {k: v[0] for k, v in p.items() if k[0] != "glyphs"}
            ok = (checked > 500 and wrong == 0 and all(v == 1 for v in flags.values())
                  and len(flags) == 6)
            detail = (f"{checked:.0f} glyphs of JetBrains Mono, {wrong:.0f} differing from "
                      f"stb_truetype's own bitmap in the atlas; {flags}")
        return ok, detail

    @arm("ui-editor-wrap")
    def _():
        ok, detail = doc_flags("editor-wrap", 7, "a line breaks after its last space that fits, at "
                               "the edge for a word longer than a row, past tabs and characters "
                               "past ASCII at their columns, its later rows hung at its indent "
                               "unless that is over half the row")
        return ok, detail

    @arm("ui-editor-hit")
    def _():
        p = _ui_probe("editor-hit")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            glyphs, glyphs_wrong = p[("glyphs", "checked")][0], p[("glyphs", "wrong")][0]
            trips, trips_wrong = p[("roundtrip", "checked")][0], p[("roundtrip", "wrong")][0]
            ok = glyphs > 80 and glyphs_wrong == 0 and trips > 100 and trips_wrong == 0
            detail = (f"{glyphs:.0f} glyphs drawn, {glyphs_wrong:.0f} whose cell a click does not "
                      f"land on the offset drawn there; {trips:.0f} carets, {trips_wrong:.0f} a "
                      f"click at does not land back on")
        return ok, detail

    @arm("ui-editor-anchor")
    def _():
        ok, detail = doc_flags("editor-anchor", 4, "lines put in above the view, every line above "
                               "it measured where it was guessed, and a new width all keep the "
                               "line at its top; the wheel moves it")
        return ok, detail

    @arm("ui-editor-keys")
    def _():
        ok, detail = doc_flags("editor-keys", 19, "every standard binding's effect: characters, "
                               "words, rows, the line's first character then its start, the text's "
                               "start, deletes, Enter indenting, Tab and Shift+Tab, select all, "
                               "copy, cut, paste, typing past ASCII, undo and redo; every command "
                               "listed once")
        return ok, detail

    @arm("ui-editor-carets")
    def _():
        ok, detail = doc_flags("editor-carets", 14, "typing at five carets as one action, "
                               "overlapping selections merged, five lines pasted one to each "
                               "caret, Cmd+D's word and next matches, Esc to the primary, a caret "
                               "added a row below twice, Option+click, a double click's word, a "
                               "triple's line and a fourth click's caret, a drag in the text "
                               "selecting after the bar's thumb was dragged, find's next match, "
                               "touching selections typed over each landing after its own text, "
                               "and a caret merged into a selection it starts; freed memory "
                               "scribbled over, so the bar's place reads no memory left unset",
                               env=_scribbled())
        return ok, detail

    @arm("ui-editor-mode")
    def _():
        ok, detail = doc_flags("editor-mode", 6, "a vim-like mode built through the public calls "
                               "alone: x, dw, a count before j, i into an insert mode that types "
                               "and takes the standard mode's arrows, Esc back to a normal mode "
                               "that types nothing")
        return ok, detail

    @arm("ui-editor-host-undo")
    def _():
        ok, detail = doc_flags("editor-host-undo", 3, "with keep_undo off, a host's mode binding "
                               "Cmd+Z to its own command runs it, the text untouched, and the "
                               "editor's own undo refuses")
        return ok, detail

    # ---- the seams a mode stands on (spec 14.5)
    @arm("ui-editor-layer")
    def _():
        ok, detail = doc_flags("editor-layer", 4, "a layer over a mode draws its block caret and "
                               "types nothing where it types nothing, runs its own key, and keeps "
                               "its own name")
        return ok, detail

    @arm("ui-editor-decline")
    def _():
        ok, detail = doc_flags("editor-decline", 1, "a layer's command that does nothing hands its "
                               "key to the mode under it, whose binding runs")
        return ok, detail

    @arm("ui-editor-paired-char")
    def _():
        ok, detail = doc_flags("editor-paired-char", 6, "a binding taking Space takes the space "
                               "its press makes, a space arriving alone is typed, a key nothing "
                               "binds types its character, a used key with no character in its "
                               "pass takes none from the next, a key moving the focus types its "
                               "character nowhere, and a field's letter is typed")
        return ok, detail

    @arm("ui-element-keys")
    def _():
        ok, detail = doc_flags("element-keys", 15, "a focused panel with a key hook is handed "
                               "every key and character in order, a key it takes dropping its "
                               "character wherever the focus went, and keeps the focus under a "
                               "passing pointer; Tab and Esc reach the screen; a press focuses it "
                               "only when it is focusable; a field's hook keeps a / out of its "
                               "text; a panel with no hook is as it was; the four controls and an "
                               "editor refuse a hook")
        return ok, detail

    @arm("ui-panel-focus")
    def _():
        ok, detail = doc_flags("panel-focus", 8, "a panel taking the keys is drawn focused: its "
                               "border over a draw hook's painting, focused and pressed, on its "
                               "own quad with no hook, over its style's image, in the theme's "
                               "colour when the app sets one; a focusable panel with no key hook "
                               "and a label draw none")
        return ok, detail

    @arm("ui-border-fade")
    def _():
        ok, detail = doc_flags("border-fade", 2, "a fading screen fades a quad's border with its "
                               "fill: at a draw list's alpha of one half, both alphas halved")
        return ok, detail

    @arm("ui-editor-shapes")
    def _():
        ok, detail = doc_flags("editor-shapes", 4, "a selection of three characters drawn three "
                               "cells wide, four with the character at its far end, and its line "
                               "whole with the line break; the block caret over the character at "
                               "the head")
        return ok, detail

    @arm("ui-editor-line-numbers")
    def _():
        ok, detail = doc_flags("editor-line-numbers", 3, "the line numbers in the text's colour at "
                               "38% with theme.line_numbers zeroed, all red with it set while the "
                               "code keeps its colour, and at 38% again with it zeroed once more")
        return ok, detail

    @arm("ui-editor-whole-lines")
    def _():
        ok, detail = doc_flags("editor-whole-lines", 5, "a whole-line range fills each row of its "
                               "lines from the text's left edge to the editor's right, an empty "
                               "and a wrapped line included, not the line its end starts; a "
                               "selection over it; a plain style bands its characters; a zeroed "
                               "style draws nothing")
        return ok, detail

    @arm("ui-editor-scroll-line")
    def _():
        ok, detail = doc_flags("editor-scroll-line", 4, "the line at the top and the rows into it "
                               "read after a wheel's 32.5 rows, put on a second editor with its "
                               "rows level with the first's, put before a layout and taken at it, "
                               "and held past the last line as a wheel is")
        return ok, detail

    @arm("ui-editor-folds")
    def _():
        ok, detail = doc_flags("editor-folds", 8, "a closed fold one row in the fold colour with "
                               "its label and no number, the next line numbered as before; a row "
                               "move, a wheel, a click and an offset inside it take it as one row; "
                               "a click opens it; it moves with its lines and goes with them, "
                               "typing or a Delete in it opens it, not on a read-only text; Home "
                               "and End from a line it hides stay on that line; kept in a text "
                               "left, none on a second editor; overlaps refused; open-fold, "
                               "close-fold and toggle-fold by name")
        return ok, detail

    @arm("ui-editor-fillers")
    def _():
        ok, detail = doc_flags("editor-fillers", 4, "filler rows above a line and above a closed "
                               "fold's row in the filler colour with no number or text, none for a "
                               "line the fold hides; a row move steps over them and the top's rows "
                               "count them; a click on one lands at its line's start; a line's "
                               "rows as asked are its rows as drawn")
        return ok, detail

    @arm("ui-editor-gutter")
    def _():
        ok, detail = doc_flags("editor-gutter", 4, "a range's gutter fill on every row of its line "
                               "from the gutter's left to the text, under the number; its sign in "
                               "the gap on the line's first row only, in the text's colour or its "
                               "style's sign_colour, a sign alone drawing no fill; a zeroed style "
                               "neither")
        return ok, detail

    @arm("ui-editor-bar-room")
    def _():
        ok, detail = doc_flags("editor-bar-room", 4, "a bar only inside its element: an editor "
                               "sized to nothing draws none, one 4 points wide none and a press on "
                               "it pages nothing; one with room draws its thumb inside its insets")
        return ok, detail

    @arm("ui-editor-top-row")
    def _():
        ok, detail = doc_flags("editor-top-row", 6, "the top as rows from the text's start, "
                               "fillers and a closed fold's row counted, put back as read, held as "
                               "a wheel is, taken at a layout when asked before, and two sides "
                               "level at one row")
        return ok, detail

    @arm("ui-editor-gutter-digits")
    def _():
        ok, detail = doc_flags("editor-gutter-digits", 4, "a short text asked for 4 digits with a "
                               "long text's columns and numbers, its own for 0 or fewer, and a "
                               "change's rows guessed again and settled")
        return ok, detail

    @arm("ui-editor-filler-end")
    def _():
        ok, detail = doc_flags("editor-filler-end", 6, "fillers on the line count drawn under the "
                               "last line and counted to the scroll's end, no caret on them, a "
                               "click there at the text's end, kept under a new last line, one "
                               "past the count left out, and under a closed fold ending the text")
        return ok, detail

    @arm("ui-editor-rows-kept")
    def _():
        ok, detail = doc_flags("editor-rows-kept", 4, "a line's rows from the count kept: none "
                               "wrapped asking a settled text twice over, a guess wrapped once, "
                               "and wrapped again at new columns and new tab stops")
        return ok, detail

    @arm("ui-editor-fillers-read")
    def _():
        ok, detail = doc_flags("editor-fillers-read", 5, "the fillers read back sorted, one a line "
                               "and none empty, moved with their lines, gone from a line pasted "
                               "over, those under the text last, and none for none")
        return ok, detail

    @arm("ui-editor-exact-rows")
    def _():
        ok, detail = doc_flags("editor-exact-rows", 7, "a text just shown counts the rows above "
                               "its top and above a row put as a settled one does, the bar wraps "
                               "none, a closed fold's lines stay guesses until it is cut to a line "
                               "or opened, and a second ask wraps nothing")
        return ok, detail

    @arm("ui-editor-scrolls")
    def _():
        ok, detail = doc_flags("editor-scrolls", 4, "one scroll counted for each call asked, the "
                               "wheel, the bar and a key, one asked before a layout once, and none "
                               "for an edit, a fold, a layout or the budget")
        return ok, detail

    @arm("ui-editor-link")
    def _():
        ok, detail = doc_flags("editor-link", 10, "two linked editors level at every layout, in "
                               "either order in the tree and with no scroll asked, led by the last "
                               "asked a scroll and never by a "
                               "layout or the hold, another text following, hidden ones held to "
                               "nothing, a freed one let go, and links making no pair refused")
        return ok, detail

    @arm("ui-editor-link-folds")
    def _():
        ok, detail = doc_flags("editor-link-folds", 8, "a fold turned on one side turned on the "
                               "other by a command, a call, a click, an edit and vim's z keys, "
                               "none with counts apart, a set of folds one side's own, and the two "
                               "level after")
        return ok, detail

    @arm("ui-editor-steps")
    def _():
        ok, detail = doc_flags("editor-steps", 7, "typing joined into words shares a step, a group "
                               "holds one over typing, a Backspace and more typing, and one undo "
                               "takes the group back and one redo makes it again; typing after "
                               "keep_undo was off a step of its own; the steps the same with "
                               "keep_undo off")
        return ok, detail

    # ---- the vim mode held to vim itself (spec 14.5)
    @arm("ui-vim-oracle")
    def _():
        if not _VIM:
            return None, "(no vim on the path)"
        cases = _vim_corpus(_VIM_SEQUENCES)
        diffs, ran = _vim_oracle(cases, workdir, "oracle")
        ok = ran == len(cases) and not diffs
        shown = _vim_shown(diffs, lambda case: f"{case[3]!r} at {case[1]}:{case[2]}")
        detail = (f"{len(cases)} cases of {len(_VIM_SEQUENCES)} sequences over "
                  f"{len(_VIM_FIXTURES)} texts, vim ran {ran}, {len(diffs)} differ from it"
                  + (f": {shown}" if shown else ""))
        return ok, detail

    # ---- the vim mode over the host's folds, held to vim's manual folds (spec 14.16)
    @arm("ui-vim-folds")
    def _():
        if not _VIM:
            return None, "(no vim on the path)"
        cases = _vim_corpus(_VIM_FOLD_SEQUENCES, _VIM_FOLD_FIXTURES)
        diffs, ran = _vim_oracle(cases, workdir, "folds")
        ok = ran == len(cases) and not diffs
        shown = _vim_shown(diffs, lambda case: f"{case[3]!r} at {case[1]}:{case[2]}")
        detail = (f"{len(cases)} cases of {len(_VIM_FOLD_SEQUENCES)} sequences over closed "
                  f"folds on {len(_VIM_FOLD_FIXTURES)} texts, vim ran {ran}, {len(diffs)} "
                  f"differ from it"
                  + (f": {shown}" if shown else ""))
        return ok, detail

    # ---- the vim mode held to vim on sequences no one wrote (spec 14.5)
    @arm("ui-vim-random")
    def _():
        if not _VIM:
            return None, "(no vim on the path)"
        seeds = (1, 2, 3, 4)
        cases, seed_of = [], {}
        for seed in seeds:
            for case in _vim_random_cases(seed, 400):
                seed_of.setdefault(case, seed)
                cases.append(case)
        diffs, ran = _vim_oracle(cases, workdir, "random")
        ok = ran == len(cases) and not diffs
        shown = _vim_shown(diffs, lambda case: f"seed {seed_of[case]} {case[3]!r} at "
                                               f"{case[1]}:{case[2]}")
        detail = (f"{len(cases)} sequences from seeds {', '.join(map(str, seeds))}, vim ran "
                  f"{ran}, {len(diffs)} differ from it" + (f": {shown}" if shown else ""))
        return ok, detail

    @arm("ui-vim-state")
    def _():
        ok, detail = doc_flags("vim-state", 11, "NORMAL and INSERT named, the block caret and the "
                               "bar, the keys of a half-typed command shown and dropped by Esc, a "
                               "click past a line's end held on its last character, the three "
                               "visual modes named and drawn their own ways, a drag entering "
                               "visual mode, and a count past the most a put makes refused")
        return ok, detail

    @arm("ui-vim-undo")
    def _():
        ok, detail = doc_flags("vim-undo", 8, "an insert from i to Esc, an Enter and a Backspace "
                               "in it, is one step, and so is cw with what it typed: each undone "
                               "and redone whole, and told as one step with keep_undo off too; an "
                               "insert of 5,000 keys made again whole by .")
        return ok, detail

    @arm("ui-vim-host")
    def _():
        ok, detail = doc_flags("vim-host", 9, "a host's Space over the vim mode acts in normal "
                               "mode and takes its space, hands it back in insert mode where it is "
                               "typed, keeps the host's name over vim's look, and the host's undo "
                               "takes u and Ctrl+R; / and : ask the host's line, whose answers "
                               "search and go to a line, and d/ answered there is made again by . "
                               "asking nothing; one vim over two editors lets the first's insert "
                               "go when keys reach the second, and a paste in insert mode is made "
                               "again as the text it typed")
        return ok, detail

    @arm("ui-vim-lookup")
    def _():
        ok, detail = doc_flags("vim-lookup", 11, "gd, gD, Ctrl+] and K asked of the host once "
                               "each, of their kinds; g Esc d asking nothing; dgd and visual gd "
                               "vim's motions; Ctrl+] and K in a one-line visual area asked with "
                               "it in place and leaving visual mode, over two lines asking "
                               "nothing; a host finding nothing giving vim's gd; . repeating the "
                               "change before a lookup; a read-only text asking; a host's jump "
                               "from a block kept; dgd after j characterwise; another text shown "
                               "from visual mode left normal")
        return ok, detail
    @arm("ui-vim-carets")
    def _():
        ok, detail = doc_flags("vim-carets", 10, "a block's I a caret on each line, what is typed "
                               "on every one, one cursor at the top left after Esc and after undo, "
                               "A after the block; each of three carets taking 3x, dw and a visual "
                               "area; two areas sharing a character deleted as one; and two "
                               "carets' ranges nested out of order shifted and re-indented a line "
                               "once each")
        return ok, detail

    # The text the editor's and the vim mode's end-to-end arms open.
    text_path = os.path.join(workdir, "editor_script.txt")
    with open(text_path, "w", encoding="utf-8", newline="") as f:
        f.write("one two\none four\n")

    # ---- the editor end to end: a file opened, two carets typed at, undone, a word found
    @arm("ui-editor-script")
    def _():
        script = os.path.join(workdir, "editor_script.script")
        find_key = "mod+g" if sys.platform == "darwin" else "f3"
        with open(script, "w", encoding="utf-8") as f:
            f.write(f"3 key=mod+alt+down\n5 type=XY\n7 key=mod+z\n9 key=escape\n11 key=mod+d\n"
                    f"13 key={find_key}\n")
        _, typed = _canvas_run(workdir, "editor_typed", "--editor", text_path, "--input-script",
                               script, frames=7)
        _, done = _canvas_run(workdir, "editor_done", "--editor", text_path, "--input-script",
                              script, frames=16)
        typed, done = _canvas_editor(typed), _canvas_editor(done)
        ok = (typed["text"] == b"XYone two\nXYone four\n" and typed["sel"] == [2, 2, 2, 12, 12]
              and done["text"] == b"one two\none four\n" and done["sel"] == [1, 0, 3])
        detail = (f"typed at two carets {typed['text']!r} {typed['sel']} (want "
                  f"b'XYone two\\nXYone four\\n' [2, 2, 2, 12, 12]); undone, the word at the caret "
                  f"taken and found again {done['text']!r} {done['sel']} (want the file as it "
                  f"was, [1, 0, 3])")
        return ok, detail

    # ---- the vim mode end to end: dw, a search and a line number through the app's own line, x
    # and . made again, and a search finding nothing said so
    @arm("ui-vim-script")
    def _():
        script = os.path.join(workdir, "editor_script.script")
        with open(script, "w", encoding="utf-8") as f:
            f.write("3 type=dw\n5 type=/\n7 type=four\n9 key=enter\n11 type=x\n13 type=:\n"
                    "15 type=1\n17 key=enter\n19 type=.\n21 type=/\n23 type=zzz\n25 key=enter\n")
        _, run = _canvas_run(workdir, "vim_script", "--editor", text_path, "--vim",
                             "--input-script", script, frames=28)
        run = _canvas_editor(run)
        ok = (run["text"] == b"wo\none our\n" and run["sel"] == [1, 0, 0] and run["vim"] == 0
              and run["said"] == "Pattern not found: zzz")
        detail = (f"{run['text']!r} {run['sel']}, normal mode {run['vim'] == 0}, said "
                  f"{run['said']!r} (want b'wo\\none our\\n' [1, 0, 0], 'Pattern not found: zzz')")
        # The app's own lookup and layout (spec 14.9): == re-indents the second line by its bracket,
        # and gd on the first line's head says itself on the vim line.
        sexpr_path = os.path.join(workdir, "vim_layout.ckt")
        with open(sexpr_path, "w", encoding="utf-8", newline="") as f:
            f.write("(block a\n(port b)\n)\n")
        with open(script, "w", encoding="utf-8") as f:
            f.write("3 type=j\n5 type===\n7 type=ggw\n9 type=gd\n")
        _, laid = _canvas_run(workdir, "vim_layout", "--editor", sexpr_path, "--vim",
                              "--vim-lookup", "--vim-layout", "--input-script", script, frames=12)
        laid = _canvas_editor(laid)
        ok = ok and laid["text"] == b"(block a\n  (port b)\n)\n" and laid["said"] == "gd block"
        detail += (f"; with the app's lookup and layout {laid['text']!r}, said {laid['said']!r} "
                   f"(want b'(block a\\n  (port b)\\n)\\n', 'gd block')")
        return ok, detail

    @arm("ui-editor-host-edit")
    def _():
        ok, detail = doc_flags("editor-host-edit", 8, "a selection through the host's edits before "
                               "it, after it and inside it, a caret under replaced bytes and at an "
                               "insertion, a whole new text changing past the caret, a second "
                               "editor of the same text, and both letting go of the text when it "
                               "is freed")
        return ok, detail

    # ---- elements taken out and moved (spec 14.6)
    @arm("ui-remove")
    def _():
        ok, detail = doc_flags("remove", 11, "the focus, the focus last seen, a held press and a "
                               "held bar let go of what goes; the next layout whole; a button's "
                               "action and an editor's key taking their own element out, freed as "
                               "the pass ends, a second ask refused; an insert's group closed; a "
                               "root and a row refused, and the focus refused another screen's "
                               "element; freed memory scribbled over", env=_scribbled())
        return ok, detail

    @arm("ui-move")
    def _():
        ok, detail = doc_flags("move", 10, "an editor moved under a row keeping its carets, its "
                               "view and the focus, a field its text, each in front of the child "
                               "named or at the end, and the refusals")
        return ok, detail

    @arm("ui-hidden")
    def _():
        ok, detail = doc_flags("hidden", 16, "a hidden element takes no room or gap, lays out "
                               "empty and stands where it stood once shown; draws nothing; takes "
                               "no press, wheel, key, Tab or focus, and lets go of a press, a bar "
                               "and the focus; an editor keeps its carets, scroll and wraps, where "
                               "sized to nothing it has one column; a pane neither eases nor "
                               "builds rows; the refusals")
        return ok, detail

    # ---- an editor shown another text (spec 14.6)
    @arm("ui-editor-set-doc")
    def _():
        ok, detail = doc_flags("set-doc", 9, "a text not shown from its start, the one left no "
                               "longer moving its carets while a second editor keeps its own, "
                               "typing reaching the new text, the same text a no-op, NULL nothing, "
                               "back to the first where it was through the edit made since, a new "
                               "editor placed before its first layout, and a text freed while "
                               "parked on; freed memory scribbled over", env=_scribbled())
        return ok, detail

    @arm("ui-vim-set-doc")
    def _():
        p = _ui_probe("vim-set-doc")
        if not p:
            ok, detail = False, "the probe produced nothing"
        else:
            reused = p.pop(("address", "reused"), [0])[0]
            wrong = _probe_wrong(p, {k: 1 for k in p})
            ok = not wrong and len(p) == 4
            detail = (f"an operator, an insert and a visual area let go when the editor is shown "
                      f"another text, and an operator when it is freed and another made "
                      f"in its place "
                      + ("at its address" if reused else
                         "-- not at its address this run, so that case did not stand for its name")
                      + (f"; wrong: {wrong}" if wrong else ""))
        return ok, detail

    # ---- a read-only text (spec 14.7): the standard mode end to end
    # The text both read-only arms open.
    ro_text = b"one two\nthree four\n"
    ro_path = os.path.join(workdir, "editor_read_only.txt")
    with open(ro_path, "wb") as f:
        f.write(ro_text)

    @arm("ui-editor-read-only")
    def _():
        ro_script = os.path.join(workdir, "editor_read_only.script")
        # The word Cmd+D takes is cut with nothing copied before, so the clipboard says the cut
        # copied.
        with open(ro_script, "w", encoding="utf-8") as f:
            f.write("3 type=XY\n5 key=backspace\n7 key=mod+d\n9 key=mod+x\n11 key=mod+v\n"
                    "13 key=right\n")
        _, ro = _canvas_run(workdir, "editor_read_only", "--editor", ro_path, "--read-only",
                            "--input-script", ro_script, frames=16)
        ro = _canvas_editor(ro)
        # The two letters, the cut and the paste: Backspace at the text's start deletes nothing.
        ok = (ro["text"] == ro_text and ro["sel"] == [1, 3, 3] and ro["clip"] == b"one"
              and ro["refused"] == 4)
        detail = (f"the text {'unchanged' if ro['text'] == ro_text else 'changed'}, selections "
                  f"{ro['sel']}, clipboard {ro['clip']!r}, {ro['refused']} refused (want "
                  f"unchanged, [1, 3, 3], b'one', 4)")
        return ok, detail

    # ---- the vim mode on a read-only text: the probe, then end to end
    @arm("ui-vim-read-only")
    def _():
        ok, detail = doc_flags("vim-read-only", 14, "a visual delete letting the area go with the "
                               "cursor at its start, told with the editor, 3dd and 5J one refusal "
                               "each, . refusing the refused command again and making it once "
                               "writable, a block's I into an insert whose typing is refused, an "
                               "insert the text was made read-only in kept open until Esc; and "
                               "against vim, what . makes of a refused visual put and 3s once "
                               "writable, and a J, r and ~ that could change nothing beeped at "
                               "rather than refused, a refused r kept for nothing")
        ro_script = os.path.join(workdir, "editor_read_only.script")
        # The clipboard yanked first, which none of the refused commands after may change.
        with open(ro_script, "w", encoding="utf-8") as f:
            f.write("3 type=yiw\n5 type=x\n7 type=dd\n9 type=p\n11 type=.\n13 type=cw\n15 type=i\n"
                    "17 type=w\n19 type=o\n21 type=j\n")
        _, rv = _canvas_run(workdir, "vim_read_only", "--editor", ro_path, "--vim", "--read-only",
                            "--input-script", ro_script, frames=24)
        rv = _canvas_editor(rv)
        # x, dd, p, the p `.` makes again, cw, i and o.
        script_ok = (rv["text"] == ro_text and rv["sel"] == [1, 12, 12] and rv["clip"] == b"one"
                     and rv["refused"] == 7 and rv["vim"] == 0)
        ok = ok and script_ok
        detail += (f"; by a script, the text "
                   f"{'unchanged' if rv['text'] == ro_text else 'changed'}, "
                   f"selections {rv['sel']}, clipboard {rv['clip']!r}, {rv['refused']} refused, "
                   f"normal mode {rv['vim'] == 0} (want unchanged, [1, 12, 12], b'one' as yanked "
                   f"first, 7, True)")
        return ok, detail

    @arm("ui-vim-read-only-oracle")
    def _():
        if not _VIM:
            return None, "(no vim on the path)"
            # With their `o` refused, what these would insert is read as commands, and it holds `q`
            # and `(`: a macro and a sentence motion, which the mode leaves out.
        absent = {"Goq) (z)<Esc>^da(", "Goq) (z)<Esc>^di(", "Goq (z)<Esc>^da("}
        cases = _vim_corpus([s for s in _VIM_SEQUENCES if s not in absent])
        diffs, ran = _vim_oracle(cases, workdir, "read_only", read_only=True)
        missing = absent - set(_VIM_SEQUENCES)
        ok = ran == len(cases) and not diffs and not missing
        shown = _vim_shown(diffs, lambda case: f"{case[3]!r} at {case[1]}:{case[2]}")
        detail = (f"{len(cases)} cases on a read-only text, vim ran {ran}, {len(diffs)} differ "
                  f"from it" + (f": {shown}" if shown else "")
                  + (f"; left out but no longer in the corpus: {sorted(missing)}" if missing
                     else ""))
        return ok, detail

    # ---- the host's indent and format held to vim's indentexpr and formatexpr (spec 14.9)
    @arm("ui-vim-layout-oracle")
    def _():
        if not _VIM:
            return None, "(no vim on the path)"
            # Refused, these leave ( to be read as a command: vim's sentence motion, which the mode
            # leaves out.
        absent = {"o(<CR>x<Esc>", "A(<CR>x<Esc>", "2O(<Esc>"}
        runs = []
        for read_only in (False, True):
            seqs = [s for s in _VIM_LAYOUT_SEQUENCES if not (read_only and s in absent)]
            cases = _vim_corpus(seqs, _VIM_LAYOUT_FIXTURES)
            diffs, ran = _vim_oracle(cases, workdir, "layout_ro" if read_only else "layout",
                                     read_only=read_only, layout=True)
            runs.append((len(cases), ran, diffs))
        missing = absent - set(_VIM_LAYOUT_SEQUENCES)
        ok = all(ran == n and not diffs for n, ran, diffs in runs) and not missing
        detail = "; ".join(
            f"{label}: {n} cases of {len(_VIM_LAYOUT_SEQUENCES)} sequences over "
            f"{len(_VIM_LAYOUT_FIXTURES)} texts, vim ran {ran}, {len(diffs)} differ"
            + (f": {_vim_shown(diffs, lambda case: f'{case[3]!r} at {case[1]}:{case[2]}')}"
               if diffs else "")
            for label, (n, ran, diffs) in zip(("writable", "read-only"), runs))
        if missing:
            detail += f"; left out but no longer in the corpus: {sorted(missing)}"
        return ok, detail

    # ---- a parse's errors, read a few versions back, underlined where the typing since put them
    @arm("ui-editor-ranges-late")
    def _():
        late_path = os.path.join(workdir, "editor_late.ckt")
        with open(late_path, "w", encoding="utf-8", newline="") as f:
            f.write("(inst a)\n(net b\n(part c)\n")
        late_script = os.path.join(workdir, "editor_late.script")
        with open(late_script, "w", encoding="utf-8") as f:
            f.write("2 type=XYZ\n")
        late_out, late = _canvas_run(workdir, "editor_late", "--editor", late_path, "--editor-lag",
                                     "6", "--input-script", late_script, frames=8)
        errors = [int(v) for v in late.get(("editor", "errors"), [])]
        at = [int(v) for v in late.get(("editor", "error_at"), [])]
        red = 0
        if late_out and len(at) == 2:
            img = _ppm_array(late_out)
            box = img[at[1]:at[1] + 40, at[0]:at[0] + 24]
            red = int(np.count_nonzero((box[..., 0] > 170) & (box[..., 1] < 120)
                                       & (box[..., 2] < 120)))
        ok = errors == [1, 12, 13] and red > 0
        detail = (f"the parse of the text before 'XYZ' was typed, taken six frames on: errors "
                  f"{errors} (want one at bytes 12 to 13, the unclosed '(' moved past the typing; "
                  f"9 to 10 is the version it read), {red} red pixels under it at {at} (want some)")
        return ok, detail

    # ---- the palette, end to end
    # The palette opened and typed into, which the click arm takes its second row from.
    @functools.cache
    def palette_run():
        script = os.path.join(workdir, "ui_palette.script")
        with open(script, "w", encoding="utf-8") as f:
            f.write("2 key=mod+k\n4 type=zo\n")
        return _canvas_run(workdir, "palette", "--sheet", "board", "--palette",
                           "--input-script", script, frames=8)

    @arm("ui-type-palette")
    def _():
        out, probe = palette_run()
        state = [int(v) for v in probe.get(("palette", "state"), [])]
        field = [int(v) for v in probe.get(("palette", "field"), [])[:4]]
        if not out or not state or not field:
            ok, detail = False, "the run failed or printed no palette"
        else:
            x0, y0, x1, y1 = field
            a = _ppm_array(out)[y0 + 6:y1 - 6, x0 + 6:x1 - 6, :3].astype(int)
            ink = int((a.max(axis=2) > 150).sum())
            # Open, two bytes typed, two rows, the first Zoom in (the third command) lit at 0 and 1.
            ok = state == [1, 2, 2, 2, 0, 1] and ink > 30
            detail = (f"open/typed/rows/first/lit {state} (want [1, 2, 2, 2, 0, 1]), {ink} px of "
                      f"text in the field (want over 30)")
        return ok, detail

    # ---- a click on a palette row, the pointer read by the engine (spec 14.3)
    @arm("ui-click-palette")
    def _():
        out, probe = palette_run()
        second = [int(v) for v in probe.get(("palette", "second"), [])[:4]]
        if not out or not second:
            ok, detail = False, "no second row to click: the palette's run failed or built none"
        else:
            # The second row's middle, Zoom out, as the app laid it out; the script's pointer is
            # framebuffer pixels with +Y up.
            fb_h, fb_w = _ppm_array(out).shape[:2]
            x = (second[0] + min(second[2], fb_w)) // 2
            y = fb_h - (second[1] + second[3]) // 2
            script = os.path.join(workdir, "ui_click.script")
            with open(script, "w", encoding="utf-8") as f:
                f.write(f"2 key=mod+k\n4 type=zo\n6 at={x},{y}\n7 at={x},{y} down\n8 at={x},{y}\n")
            _, probe = _canvas_run(workdir, "click", "--sheet", "board", "--palette",
                                   "--input-script", script, frames=12)
            ran = probe.get(("palette", "ran"), [None])[0]
            state = [int(v) for v in probe.get(("palette", "state"), [])]
            ok = ran == "3" and state[:1] == [0]
            detail = (f"a click on the second row ran command {ran} (want 3, Zoom out) and the "
                      f"palette is {'closed' if state[:1] == [0] else 'open'} (want closed)")
        return ok, detail

    # ---- the history column, end to end
    @arm("ui-history-clip")
    def _():
        out, probe = _canvas_run(workdir, "history", "--sheet", "board", "--history")
        hist = probe.get(("history", "column"))
        if not out or not hist:
            ok, detail = False, "the run failed or printed no column"
        else:
            cx0, cy0, cx1, cy1 = (int(v) for v in hist[:4])
            qx0, qy0, qx1, qy1 = (int(v) for v in hist[5:9])
            want = int(hist[10], 16)
            input_y = int(hist[12])
            a = _ppm_array(out)[:, :, :3].astype(int)
            target = np.array([(want >> 16) & 255, (want >> 8) & 255, want & 255])
            # Above the pane, two pixels in from the column's own edges, which its anti-aliasing
            # blends with what is behind it; below it, the gap down to the input line.
            strips = np.concatenate([a[cy0 + 2:qy0, cx0 + 4:cx1 - 4],
                                     a[qy1:input_y, cx0 + 4:cx1 - 4]])
            stray = int((np.abs(strips - target).max(axis=2) > 2).sum())
            ink = int((np.abs(a[qy0:qy1, qx0:qx1] - target).max(axis=2) > 2).sum())
            ok = stray == 0 and ink > 5000 and qy0 > cy0 and qy1 < cy1
            detail = (f"{stray} px off the column's colour in its padding above and below the pane "
                      f"(want 0), {ink} px of text in the pane (want over 5000)")
        return ok, detail

    # ---- typing, from the engine to the UI's input (spec 14.2)
    @arm("ui-typed")
    def _():
        script = os.path.join(workdir, "ui_typed.script")
        with open(script, "w", encoding="utf-8") as f:
            f.write("2 type=aé\\sΩ key=mod+a key=shift+left\n4 key=backspace\n")
        said = _canvas_shot(os.path.join(workdir, "ui_typed.ppm"), 8, "--canvas-probe",
                            "--input-script", script)
        typed = [line.split()[2:] for line in (said or "").splitlines()
                 if line.startswith("canvas typed")]
        mod = 8 if sys.platform == "darwin" else 2  # GLFW_MOD_SUPER, GLFW_MOD_CONTROL
        # GLFW_KEY_A 65, GLFW_KEY_LEFT 263 with GLFW_MOD_SHIFT 1, GLFW_KEY_BACKSPACE 259.
        want = [["c61", "ce9", "c20", "c3a9", f"k65+{mod}", "k263+1"], ["k259+0"]]
        ok = typed == want
        detail = f"the UI was handed {typed} (want {want})"
        return ok, detail

    # ---- glyphs past ASCII, and the box for one the atlas lacks
    def panel_crop(tag, text):
        out, probe = _canvas_run(workdir, f"glyph_{tag}", "--ui-text", text)
        panel = probe.get(("ui", "panel"))
        if not out or not panel:
            return None
        x0, y0, x1, y1 = (int(v) for v in panel[:4])
        return _ppm_array(out)[y0:y1, x0:x1, :3]

    @arm("ui-glyphs")
    def _():
        crops = {tag: panel_crop(tag, text) for tag, text in
                 (("blank", " "), ("e", "e"), ("eacute", "é"), ("omega", "Ω"), ("micro", "µ"),
                  ("missing", "你"))}
        if any(c is None for c in crops.values()):
            ok, detail = False, "a run failed or printed no panel"
        else:
            def differ(a, b):
                return int(np.any(crops[a] != crops[b], axis=2).sum())
            ink = {tag: differ(tag, "blank") for tag in ("eacute", "omega", "micro", "missing")}
            accent = differ("eacute", "e")
            ok = all(v > 40 for v in ink.values()) and accent > 20
            detail = (f"ink over a space: é {ink['eacute']}, Ω {ink['omega']}, µ {ink['micro']}, "
                      f"the missing box {ink['missing']} px (want each over 40); é differs from e "
                      f"by {accent} px (want over 20)")
        return ok, detail

    return arm.failed


def _canvas_editor(probe):
    """The editor's lines from a _canvas_run probe: the focused pane's text as bytes ("text"), its
    selections, the count then each anchor and head ("sel"), the changes the texts refused
    ("refused", -1 for no line), the clipboard as bytes ("clip"), and with --vim its state ("vim",
    -1 for no line) and what its line said ("said")."""
    vim = probe.get(("editor", "vim")) or []
    return {
        "text": bytes.fromhex((probe.get(("editor", "hex")) or [""])[0]),
        "sel": [int(v) for v in probe.get(("editor", "sel"), [])],
        "refused": int((probe.get(("editor", "refused")) or ["-1"])[0]),
        "clip": bytes.fromhex((probe.get(("editor", "clip")) or [""])[0]),
        "vim": int(vim[0]) if vim else -1,
        "said": bytes.fromhex(vim[1]).decode() if len(vim) > 1 else "",
    }
