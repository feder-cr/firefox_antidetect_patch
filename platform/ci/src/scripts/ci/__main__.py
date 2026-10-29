"""python3 scripts/ci <command>

  matrix                          the build matrix (JSON), for the plan job
  asset --leg L                   the asset name of leg L for this tree
  mozconfig --leg L --compile-cache gha|off [--sccache PATH] [--pgo-dir DIR]
                                  extend ./.mozconfig with the CI layers, print it
  phase NAME -- CMD...            run CMD as phase NAME, recording its events
  report DIR                      the run's summary from the legs' events
  assets | version                every leg's asset name | the version the tree builds
  payload-guard --tag T --repo R --seal F
                                  refuse a seal that differs from T's published one
  symbols-match --package P --symbols Z
                                  refuse crash symbols that describe other binaries than P's
  input-watch --file F --log L [--verify [--strict]]
                                  log who moves build input F; after the build, refuse a changed F
  sccache-build --tag T --patches DIR --out DIR
                                  the sccache the candidates use, built from T and our patches
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))  # upstream_toolchain.py

import legs  # noqa: E402
import mozconfig  # noqa: E402
import phase  # noqa: E402
import report  # noqa: E402


def main(argv) -> int:
    if argv[:1] == ["phase"]:
        if len(argv) < 4 or argv[2] != "--":
            print("usage: phase NAME -- CMD...", file=sys.stderr)
            return 2
        return phase.run(argv[1], argv[3:])

    ap = argparse.ArgumentParser(prog="scripts/ci")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("matrix")
    a = sub.add_parser("asset")
    a.add_argument("--leg", required=True)
    m = sub.add_parser("mozconfig")
    m.add_argument("--leg", required=True)
    m.add_argument("--compile-cache", required=True, choices=("gha", "off"))
    m.add_argument("--sccache", default=os.environ.get("SCCACHE_PATH", ""))
    m.add_argument("--pgo-dir", type=Path, default=Path.home() / "pgo")
    r = sub.add_parser("report")
    r.add_argument("dir", type=Path)
    sub.add_parser("assets")
    sub.add_parser("version")
    g = sub.add_parser("payload-guard")
    g.add_argument("--tag", required=True)
    g.add_argument("--repo", required=True)
    g.add_argument("--seal", type=Path, required=True)
    s = sub.add_parser("symbols-match")
    s.add_argument("--package", type=Path, required=True)
    s.add_argument("--symbols", type=Path, required=True)
    w = sub.add_parser("input-watch")
    w.add_argument("--file", type=Path, required=True)
    w.add_argument("--log", type=Path, required=True)
    w.add_argument("--verify", action="store_true")
    w.add_argument("--strict", action="store_true")
    b = sub.add_parser("sccache-build")
    b.add_argument("--tag", required=True)
    b.add_argument("--patches", type=Path, required=True)
    b.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)

    if args.cmd == "matrix":
        print(legs.matrix(legs.tree_version()))
    elif args.cmd == "assets":
        print("\n".join(l.asset(legs.tree_version()) for l in legs.LEGS))
    elif args.cmd == "version":
        print(legs.tree_version())
    elif args.cmd == "payload-guard":
        import payload_guard
        try:
            print(payload_guard.check(args.tag, args.repo, args.seal))
        except payload_guard.GuardError as exc:
            print("ERROR: %s" % exc, file=sys.stderr)
            return 1
    elif args.cmd == "symbols-match":
        import symbols_match
        try:
            print("\n".join(symbols_match.run(args.package, args.symbols)))
        except symbols_match.MismatchError as exc:
            print("ERROR: %s" % exc, file=sys.stderr)
            return 1
    elif args.cmd == "input-watch":
        import input_watch
        if not args.verify:
            input_watch.watch(args.file, args.log)
            return 0
        try:
            print(input_watch.verify(args.file, args.log, strict=args.strict))
        except ValueError as exc:
            print("ERROR: %s" % exc, file=sys.stderr)
            return 1
    elif args.cmd == "sccache-build":
        import sccache_build
        print(sccache_build.build(args.tag, args.patches, args.out))
    elif args.cmd == "asset":
        print(legs.by_name(args.leg).asset(legs.tree_version()))
    elif args.cmd == "mozconfig":
        import upstream_toolchain
        path = Path(".mozconfig")
        if not path.is_file():
            print("ERROR: no .mozconfig in the source", file=sys.stderr)
            return 1
        plan = mozconfig.Plan(args.compile_cache, args.sccache, args.pgo_dir, upstream_toolchain.pgo)
        text = mozconfig.compose(path.read_text(encoding="utf-8"), legs.by_name(args.leg), plan)
        path.write_text(text, encoding="utf-8")
        print("----- final .mozconfig -----")
        print(text)
    elif args.cmd == "report":
        text = report.render(args.dir)
        print(text)
        phase.summary(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
