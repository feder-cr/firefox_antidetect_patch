"""The engine as a patch series: export it from a Firefox tree, rebuild a tree from it.

    python tools/engine.py export --source C:/ff/source --base FIREFOX_157_0_1_RELEASE --head stealth/157
    python tools/engine.py verify --source C:/ff/source --base FIREFOX_157_0_1_RELEASE --head stealth/157

export   writes one folder per feature (tools/layout.py says which):
           hooks.patch    our changes to Mozilla's files, as a git diff against --base
           src/<path>     the files we add to the tree, byte for byte
           MANIFEST.json  what the folder owns (regenerated on every export)
           FEATURE.md     the feature's description, created once and then edited by hand
         and SERIES, the order the folders are applied in.

verify   rebuilds the tree from the series on top of --base, in a throwaway git index
         (no checkout of Firefox), and compares it with --head's tree. Identical trees
         mean the series IS the engine: nothing was lost or added on the way.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import layout  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
INVENTORY = ROOT / "tools" / "inventory-157.json"


def git(source: Path, *args: str, env=None, input_bytes=None) -> bytes:
    return subprocess.run(["git", "-C", str(source), *args], check=True,
                          capture_output=True, env=env, input=input_bytes).stdout


def changed_files(source: Path, base: str, head: str):
    """(status, mode, path) for every file --head changes against --base."""
    out = git(source, "diff", "--no-renames", "--raw", "-z", base, head)
    fields = out.split(b"\0")
    result = []
    for meta, path in zip(fields[0::2], fields[1::2]):
        if not meta:
            continue
        parts = meta.decode().lstrip(":").split()
        result.append((parts[4], parts[1], path.decode()))
    return result


def owners(source: Path, base: str, head: str):
    """folder -> {"modified": [paths], "added": [(path, mode)]}"""
    records = {r["file"]: r for r in json.loads(INVENTORY.read_text(encoding="utf-8"))}
    plan = defaultdict(lambda: {"modified": [], "added": []})
    for status, mode, path in changed_files(source, base, head):
        if status == "M":
            if path not in records:
                raise SystemExit("modified file %s is not in the inventory" % path)
            plan[layout.feature_folder(records[path]["feature"])]["modified"].append(path)
        elif status == "A":
            plan[layout.own_file_folder(path)]["added"].append((path, mode))
        else:
            raise SystemExit("status %s for %s is not handled yet" % (status, path))
    return plan, records


def export(source: Path, base: str, head: str) -> None:
    plan, records = owners(source, base, head)
    series = sorted(plan, key=layout.folder_sort_key)
    for folder in series:
        out = ROOT / folder
        out.mkdir(parents=True, exist_ok=True)
        owned = plan[folder]
        patch = out / "hooks.patch"
        if owned["modified"]:
            patch.write_bytes(git(source, "diff", "--binary", "--full-index", "--no-renames",
                                  base, head, "--", *sorted(owned["modified"])))
        elif patch.exists():
            patch.unlink()
        for path, mode in owned["added"]:
            target = out / "src" / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(git(source, "cat-file", "blob", "%s:%s" % (head, path)))
        manifest = {
            "modified": sorted(owned["modified"]),
            "added": {path: mode for path, mode in sorted(owned["added"])},
            "inventory": [records[p] for p in sorted(owned["modified"])],
        }
        (out / "MANIFEST.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n",
                                           encoding="utf-8", newline="\n")
        feature_md = out / "FEATURE.md"
        if not feature_md.exists():
            feature_md.write_text(describe(folder, manifest), encoding="utf-8", newline="\n")
    (ROOT / "SERIES").write_text("\n".join(series) + "\n", encoding="utf-8", newline="\n")
    print("exported %d folders: %d modified files, %d added files"
          % (len(series), sum(len(plan[f]["modified"]) for f in series),
             sum(len(plan[f]["added"]) for f in series)))


def describe(folder: str, manifest: dict) -> str:
    """A first FEATURE.md, from the inventory. Edited by hand from then on."""
    recs = manifest["inventory"]
    prefs = sorted({p for r in recs for p in r.get("prefs") or []})
    categories = sorted({r["category"] for r in recs})
    lines = ["# %s" % folder, "",
             "Category: %s" % (", ".join(categories) or "own files only"),
             "Prefs: %s" % (", ".join(prefs) or "none read directly"),
             "Test: TODO", "",
             "## What it does", "", "TODO: one paragraph, from the notes below.", "",
             "## Hooks in Mozilla files", ""]
    for r in recs:
        lines.append("- `%s` (%s, %s): %s" % (r["file"], r.get("anchor") or "?", r["hookable"],
                                             (r.get("notes") or "").replace("\n", " ")[:400]))
    if manifest["added"]:
        lines += ["", "## Our own files", ""] + ["- `%s`" % p for p in manifest["added"]]
    return "\n".join(lines) + "\n"


def verify(source: Path, base: str, head: str) -> int:
    series = (ROOT / "SERIES").read_text(encoding="utf-8").split()
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
        git(source, "read-tree", base, env=env)
        for folder in series:
            patch = ROOT / folder / "hooks.patch"
            if patch.exists():
                git(source, "apply", "--cached", "--whitespace=nowarn", str(patch), env=env)
            manifest = json.loads((ROOT / folder / "MANIFEST.json").read_text(encoding="utf-8"))
            for path, mode in manifest["added"].items():
                data = (ROOT / folder / "src" / path).read_bytes()
                sha = git(source, "hash-object", "-w", "--stdin", input_bytes=data).decode().strip()
                git(source, "update-index", "--add", "--cacheinfo", "%s,%s,%s" % (mode, sha, path),
                    env=env)
        built = git(source, "write-tree", env=env).decode().strip()
    expected = git(source, "rev-parse", "%s^{tree}" % head).decode().strip()
    if built == expected:
        print("identical: the series rebuilds %s exactly (tree %s)" % (head, built))
        return 0
    diff = git(source, "diff", "--stat", expected, built).decode()
    print("DIFFERENT: rebuilt tree %s, %s is %s\n%s" % (built, head, expected, diff))
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["export", "verify"])
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--head", required=True)
    a = ap.parse_args()
    if a.command == "export":
        export(a.source, a.base, a.head)
        return 0
    return verify(a.source, a.base, a.head)


if __name__ == "__main__":
    sys.exit(main())
