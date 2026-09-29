"""Write assets/lut/silent_grade.cube, apps/silent's colour grade.

The look of the reference: colour drained out, what is left pulled toward a
sickly green-grey, and blacks that do not quite reach black, the way old film
and old games' fog do. Applied by the tonemap after the display encode, so it
works on display-encoded values in 0..1.

    python3 apps/silent/tools/make_grade.py
"""

import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "assets", "lut", "silent_grade.cube")
SIZE = 33

SATURATION = 0.68           # of what was there
TINT = (0.94, 1.0, 0.93)    # green-grey, applied everywhere
LIFT = (0.018, 0.024, 0.020)  # the floor the blacks stop at
CONTRAST = 1.08             # a gentle S about the middle


def grade(r, g, b):
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    c = [luma + (x - luma) * SATURATION for x in (r, g, b)]
    c = [x * t for x, t in zip(c, TINT)]
    c = [0.5 + (x - 0.5) * CONTRAST for x in c]
    c = [l + x * (1.0 - l) for x, l in zip(c, LIFT)]
    return [min(max(x, 0.0), 1.0) for x in c]


def main():
    lines = ['TITLE "silent green-grey"', "LUT_3D_SIZE %d" % SIZE,
             "DOMAIN_MIN 0.0 0.0 0.0", "DOMAIN_MAX 1.0 1.0 1.0", ""]
    n = SIZE - 1
    # Red varies fastest, then green, then blue: the .cube order.
    for bi in range(SIZE):
        for gi in range(SIZE):
            for ri in range(SIZE):
                r, g, b = grade(ri / n, gi / n, bi / n)
                lines.append("%.6f %.6f %.6f" % (r, g, b))
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(OUT)


if __name__ == "__main__":
    main()
