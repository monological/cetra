"""Where a generated asset lives, asked for by filename.

Every generator in this directory resolves its inputs and outputs through
asset_path. The resolution is relative to THIS FILE and never to the working
directory, which is what lets run_fixture_gen_gate sandbox a generator: it
copies the scripts into a scratch tree, and the writes follow them there rather
than landing on the committed corpus. A generator that joined an absolute path
would overwrite the very assets the gate is comparing against.

Today every asset sits beside this module, so asset_path is a join onto its own
directory. When the corpus is split by kind, the routing changes HERE and the
hundred-odd call sites do not -- that separation is the whole reason this
module exists rather than each generator spelling the join itself.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))


def asset_path(filename, root=None):
    """Absolute path to a generated asset, read or written.

    One function for both directions on purpose: a generator that reads back a
    texture it painted must look where it wrote it, so a separate reader would
    be a second place to keep the routing in step.

    `root` overrides the destination for the two generators that accept an
    output directory on the command line (water and beach). gates.py drives
    those with a scratch directory and compares what lands there against the
    committed pair, so an override that got dropped would not fail -- the
    generator would write over the committed asset and the arm would compare
    that file against itself, passing while testing nothing and corrupting the
    corpus on the way through.
    """
    return os.path.join(root or HERE, filename)
