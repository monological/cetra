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

# This module sits with the generators, one level inside the asset tree, so a
# generated file resolves against the PARENT. Deriving it rather than naming it
# is what keeps the sandbox working: a mirror that reproduces the tree gets its
# own assets root out of the same expression, where a literal "assets" would
# reach the committed corpus from inside the scratch copy.
HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.dirname(HERE)


KIND = {".gltf": "models", ".glb": "models", ".fbx": "models",
        ".ies": "ies", ".cube": "lut",
        ".png": "textures", ".jpg": "textures", ".jpeg": "textures", ".hdr": "textures"}


def asset_ref(filename):
    """How one asset NAMES another from inside a committed file.

    Not a path this process opens -- it is stored in a .cscn or a .gltf and
    resolved later, by whoever loads it. Three consumers, all spelling
    "../<kind>/", and the uniformity is a coincidence worth knowing because
    they resolve through different machinery:

      a scene's model / profile / lut   against the SCENE's own directory, so
                                        ../models/x.gltf counts from scenes/
      a glTF image uri                  through the texture pool, whose
                                        directory is dirname(model), so
                                        ../textures/x.png counts from models/
      a scene's material texture        the same pool, and so the same ../ --
                                        counted from models/ and not from the
                                        scene that named it

    Relative rather than a -t argument on every render: gates.py has 38 inline
    render sites against 4 that pass -t, and -t would have to be suppressed for
    the self-contained bundles, whose models and images stay together.
    """
    kind = KIND.get(os.path.splitext(filename)[1].lower())
    return f"../{kind}/{filename}" if kind else filename


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
    return os.path.join(root or ASSETS, filename)
