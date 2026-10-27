"""Where the Python SDK that drives the browser lives, decided in ONE place.

⛔ THE DEFECT THIS REPLACES, and it is the shape that hides best. Each of the
four smoke tests carried its own copy of

    SDK_PATH = os.environ.get("STEALTHFOX_PYTHONPATH",
                              r"c:/src/firefox-stealth/release/stealthfox/src")
    sys.path.insert(0, SDK_PATH)

and that directory has not existed since the package was renamed to
`invisible_playwright`. `sys.path.insert` of a path that does not exist is a
silent no-op, so all four kept passing - not because the declaration worked, but
because an editable install happened to be importable anyway. The line that
claims to decide WHICH SDK is under test decided nothing, and nothing said so.

That is worse than a broken test: a smoke test exists to tell you which build
of which SDK talks to which binary, and this one could not have told you.

**So this refuses instead of pretending.** A path that is asked for and does not
exist is an error, never a shrug. And the fact lives in one module because four
copies of a machine-specific path drift the moment one of them is edited.
"""
from __future__ import annotations

import importlib.util
import os
import sys


def load_sdk(module: str = "invisible_playwright") -> str:
    """Make `module` importable, SAY where it came from, and return that.

    Order, and it matters: an explicit `STEALTHFOX_PYTHONPATH` WINS over an
    installed package, because the reason to set it is to test a checkout
    instead of whatever pip last installed. Without that precedence the
    variable would be honoured only when it was not needed.

    It prints the resolved directory because that is the thing a smoke test is
    for: saying which build of which SDK talked to which binary. The old code
    named a directory in a variable and never told anyone whether it had been
    used, which is how it went four renames without being noticed.
    """
    asked = os.environ.get("STEALTHFOX_PYTHONPATH")
    if asked:
        if not os.path.isdir(asked):
            raise SystemExit(
                "STEALTHFOX_PYTHONPATH points at %r, which is not a directory. "
                "Refusing to continue: inserting a path that does not exist "
                "would silently fall back to whatever else is importable, and "
                "this test would then report on an SDK you did not choose."
                % asked)
        sys.path.insert(0, asked)

    spec = importlib.util.find_spec(module)
    if spec is None or not spec.origin:
        raise SystemExit(
            "cannot import %s. Set STEALTHFOX_PYTHONPATH to the `src` "
            "directory of an invisible_playwright checkout, or install the "
            "package." % module)
    where = os.path.dirname(os.path.dirname(spec.origin))
    print("[sdk] %s from %s" % (module, where), file=sys.stderr)
    return where
