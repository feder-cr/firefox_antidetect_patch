"""Every release gate on a staged tree, all of them, before one verdict.

    python3 scripts/release_gates.py <staged tree>

The legs of release.yml used to call the gates in one line, so the first that
refused hid what the others would have said: with the page-surface gate red on
the branding, the runtime gate never ran. Each gate here runs, says what it
found, and the exit code is 1 if any refused.
"""
from __future__ import annotations

import sys
from pathlib import Path

import stock_parity
import validate_release as v


def gates_for(tree: Path) -> list:
    out = [v.assert_juggler_provenance, v.assert_page_surface_is_stock,
           v.assert_no_updater, v.check_no_leaks_tree]
    if stock_parity.platform_of(v._juggler_root(tree)).startswith("win"):
        out.append(v.assert_windows_runtime_is_shipped)
    return out


def main(argv) -> int:
    if len(argv) != 2:
        print("usage: release_gates.py <staged tree>")
        return 2
    tree = Path(argv[1])
    refused = []
    for gate in gates_for(tree):
        print(f"== {gate.__name__}")
        try:
            gate(tree)
        except v.ValidationError as exc:
            refused.append(gate.__name__)
            print(f"   REFUSED: {exc}")
    print(f"release gates: {len(refused)} refused" + (f": {refused}" if refused else ""))
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
