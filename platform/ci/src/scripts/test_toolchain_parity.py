#!/usr/bin/env python3
"""toolchain_parity.py, driven against the builds it exists to refuse.

  known-bad
    1. rustc of another version than the retail's (the 157 candidates: 1.99.0
       from the CI runner's "stable" against the retail's 1.95.0);
    2. Rust SIMD off where the retail has it on;
    3. another C++ optimisation level;
    4. a declared exception whose difference is gone (a stale declaration);
    5. a page with two compilers instead of three.
    6. thin LTO on x86_64, where the retail's is full;
  known-good
    6. the retail's own page against itself;
    7. a difference that is declared, for its platform only;
    8. a path-valued option compares as "set" (the path names the build machine).

Run it directly, like test_release_plan.py:
`python3 scripts/test_toolchain_parity.py`.
"""
from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import toolchain_parity as t  # noqa: E402


def page(rustc="1.95.0", cxx_flags="-TP -O2 -Oy", options="--enable-rust-simd MOZ_LTO=cross",
         rows=3, target="x86_64-pc-mingw32"):
    tools = [("['clang-cl']", "22.1.8", "-Gy"), ("['clang-cl', '-std:c++20']", "22.1.8", cxx_flags),
             ("/x/rustc", rustc, "")][:rows]
    body = "".join("<tr>\n<td>%s</td>\n<td>%s</td>\n<td>%s</td>\n</tr>\n" % r for r in tools)
    return ("<table><tbody><tr><th>target</th></tr><tr><td>%s</td></tr></tbody></table>\n" % target +
            "<table><tbody><tr><th>Compiler</th><th>Version</th><th>Compiler flags</th></tr>\n%s"
            "</tbody></table>\n<h2>Configure options</h2>\n<p>%s</p>" % (body, options))


RETAIL = page(options="--enable-rust-simd MOZ_LTO=cross --with-pgo-jarlog=/builds/worker/fetches/en-US.log")


def diff(ours, retail=RETAIL):
    return t.differences(t.facts(t.parse(ours)), t.facts(t.parse(retail)))


def stale(declared_keys, ours, platform="win64", declared_for="win64"):
    """(undeclared, stale) judging `ours` on `platform`, with `declared_keys`
    declared for `declared_for`."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "exceptions.json"
        path.write_text(json.dumps({"exceptions": [
            {"key": k, "platforms": [declared_for], "why": "test"} for k in declared_keys]}), "utf-8")
        declared = t.load_exceptions(platform, path)
    d = diff(ours)
    return {k: v for k, v in d.items() if k not in declared}, sorted(k for k in declared if k not in d)


def main() -> int:
    cases = []
    same_jarlog = " --with-pgo-jarlog=/home/runner/en-US.log"
    cases.append(("another rustc", lambda: "rustc version" in diff(page(rustc="1.99.0", options="--enable-rust-simd MOZ_LTO=cross" + same_jarlog))))
    cases.append(("Rust SIMD off", lambda: "option --enable-rust-simd" in diff(page(options="MOZ_LTO=cross" + same_jarlog))))
    cases.append(("another C++ optimisation", lambda: "c++ optimisation" in diff(page(cxx_flags="-TP -O1", options="--enable-rust-simd MOZ_LTO=cross" + same_jarlog))))
    cases.append(("a stale declaration", lambda: stale(["rustc version"], page(options="--enable-rust-simd MOZ_LTO=cross" + same_jarlog))[1] == ["rustc version"]))

    def two_compilers():
        try:
            t.facts(t.parse(page(rows=2)))
        except t.ToolchainError:
            return True
        return False
    cases.append(("two compilers instead of three", two_compilers))
    cases.append(("the retail against itself", lambda: diff(RETAIL) == {}))
    cases.append(("a declared difference, on its platform only", lambda: (
        stale(["lto"], page(options="--enable-rust-simd" + same_jarlog)) == ({}, [])
        and "lto" in stale(["lto"], page(options="--enable-rust-simd" + same_jarlog),
                                      platform="linux-x86_64")[0])))
    retail_x86 = "MOZ_AUTOMATION=1 --enable-profile-use=cross MOZ_LTO=cross --enable-rust-simd"
    cases.append(("the retail's MOZ_LTO=cross on x86_64 is full LTO, as --enable-lto=full,cross asks",
                  lambda: "lto" not in diff(page(options="--enable-profile-use=cross --enable-lto=full,cross --enable-rust-simd"),
                                            page(options=retail_x86))))
    cases.append(("thin LTO on x86_64 against the retail's full",
                  lambda: "lto" in diff(page(options="--enable-profile-use=cross --enable-lto=cross --enable-rust-simd"),
                                        page(options=retail_x86))))
    cases.append(("arm64: the retail's MOZ_LTO=cross is thin, as --enable-lto=cross gives",
                  lambda: "lto" not in diff(page(options="--enable-profile-use=cross --enable-lto=cross --enable-rust-simd",
                                                 target="aarch64-pc-linux-gnu"),
                                            page(options=retail_x86, target="aarch64-pc-linux-gnu"))))
    cases.append(("a path-valued option compares as set", lambda: diff(page(options="--enable-rust-simd MOZ_LTO=cross" + same_jarlog)) == {}))

    failures = 0
    for name, check in cases:
        try:
            ok = bool(check())
        except Exception as exc:  # a crash is a failure of the case, not of the run
            ok, name = False, "%s (raised %r)" % (name, exc)
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
