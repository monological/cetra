"""Where a generated asset lives, asked for by filename.

Every generator in this directory resolves its inputs and outputs through
asset_path. The resolution is relative to THIS FILE and never to the working
directory, which is what lets run_fixture_gen_gate sandbox a generator: it
copies the scripts into a scratch tree, and the writes follow them there rather
than landing on the committed corpus. A generator that joined an absolute path
would overwrite the very assets the gate is comparing against.

The corpus is split by KIND, and asset_subpath below is the ONE statement of
that taxonomy: asset_path joins it onto a root, asset_ref spells it relative,
and gates.py imports it for the suite's own lookups. The hundred-odd call sites
name a file and never a directory, which is the whole reason this module exists
rather than each generator spelling the join itself.

That the one statement is one statement is not decoration. Spec 12.8 shipped
with the map written twice -- here and in gates.py -- and the two had already
drifted apart before either was used in anger: this copy was missing .cscn and
.r16, the other was missing .glb and the image formats. A value two consumers
must agree on is the half that drifts.
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
        ".cscn": "scenes", ".ies": "ies", ".cube": "lut", ".r16": "data",
        ".png": "textures", ".jpg": "textures", ".jpeg": "textures", ".hdr": "textures"}


def asset_subpath(filename):
    """Where one asset sits under the corpus root: "<kind>/<name>".

    The one statement of the taxonomy, and the only thing that changes if the
    corpus is ever re-split. A name that already carries a directory is a
    self-contained bundle (abandoned_window, ivy_arcade, raiden) and passes
    through, as does a kind this map has no entry for.
    """
    if os.sep in filename or "/" in filename:
        return filename
    kind = KIND.get(os.path.splitext(filename)[1].lower())
    return f"{kind}/{filename}" if kind else filename


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
    sub = asset_subpath(filename)
    if sub == filename:
        raise ValueError(
            f"asset_ref({filename!r}): no kind for this extension. A bare name "
            "reaches no loader from inside a committed file -- it would render "
            "a plausible frame with the texture missing, so this refuses rather "
            "than passing the name through.")
    return "../" + sub


def asset_path(filename, root=None):
    """Absolute path to a generated asset, read or written.

    One function for both directions on purpose: a generator that reads back a
    texture it painted must look where it wrote it, so a separate reader would
    be a second place to keep the routing in step.

    `root` is an alternate corpus ROOT, not an alternate directory: the kind
    subpath is appended to it exactly as it is to ASSETS, so a scratch tree is
    shaped like the real one. Two generators take one from the command line
    (water and beach), and gates.py drives water with a scratch directory and
    compares what lands there against the committed pair -- an override that got
    dropped would not fail, it would write over the committed asset and the arm
    would compare that file against itself, passing while testing nothing.
    """
    return os.path.join(root or ASSETS, *asset_subpath(filename).split("/"))
