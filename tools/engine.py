"""The engine as a patch series: export it from a Firefox tree, rebuild a tree from it.

    python tools/engine.py status    did the source move since the last export?
    python tools/engine.py export    write the series from the source tree
    python tools/engine.py verify    rebuild the tree from the series, compare
    python tools/engine.py check     the series' own rules, offline (also in CI)

The Mozilla tag, the source branch and the commit last exported are in UPSTREAM.json;
--source / --base / --head override them (the source repository defaults to
C:/ff/source, or $ENGINE_SOURCE).

export   writes one folder per feature (tools/layout.py says which):
           hooks.patch    our changes to Mozilla's files, as a git diff against the tag
           src/<path>     the files we add to the tree, byte for byte
           MANIFEST.json  what the folder owns (regenerated on every export)
           FEATURE.md     the feature's description, created once and then edited by hand
         SERIES, the order the folders are applied in, and UPSTREAM.json's
         "exported_from". A folder that no longer owns anything is removed.

verify   rebuilds the tree from the series on top of the tag, in a throwaway git index
         (no checkout of Firefox, works in a bare repository), and compares it with the
         source head's tree. Identical trees mean the series IS the engine.

check    needs no Firefox: SERIES lists exactly the folders that exist, every folder has
         its MANIFEST and FEATURE.md, every file of ours is listed and present, each
         hooks.patch touches only the Mozilla files its MANIFEST owns, and no Mozilla file
         carries more of our lines than tools/hook-budget.json allows. The budget only
         goes down: logic belongs in our own files, Mozilla's files get hooks.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import layout  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
INVENTORY = ROOT / "tools" / "inventory-157.json"
UPSTREAM = ROOT / "UPSTREAM.json"
BUDGET = ROOT / "tools" / "hook-budget.json"


def git(source: Path, *args: str, env=None, input_bytes=None) -> bytes:
    return subprocess.run(["git", "-C", str(source), *args], check=True,
                          capture_output=True, env=env, input=input_bytes).stdout


def upstream() -> dict:
    return json.loads(UPSTREAM.read_text(encoding="utf-8"))


def write_json(path: Path, data) -> None:
    path.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")


def series_folders() -> list:
    return (ROOT / "SERIES").read_text(encoding="utf-8").split()


def existing_folders() -> list:
    return sorted(str(p.parent.relative_to(ROOT)).replace("\\", "/")
                  for p in ROOT.glob("*/**/MANIFEST.json")
                  if p.relative_to(ROOT).parts[0] in layout.GROUP_ORDER and "src" not in p.relative_to(ROOT).parts)


def changed_files(source: Path, base: str, head: str):
    """(status, mode, path) for every file the head changes against the tag."""
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
                raise SystemExit("modified file %s is not in the inventory: add its record to %s "
                                 "(feature, category, anchor, hookable, notes)" % (path, INVENTORY.name))
            plan[layout.feature_folder(records[path]["feature"])]["modified"].append(path)
        elif status == "A":
            plan[layout.own_file_folder(path)]["added"].append((path, mode))
        else:
            raise SystemExit("status %s for %s is not handled yet" % (status, path))
    return plan, records


def export(source: Path, base: str, head: str) -> None:
    plan, records = owners(source, base, head)
    series = sorted(plan, key=layout.folder_sort_key)
    for gone in sorted(set(existing_folders()) - set(series)):
        shutil.rmtree(ROOT / gone)
        print("removed %s: it owns nothing any more" % gone)
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
        if (out / "src").exists():
            shutil.rmtree(out / "src")
        for path, mode in owned["added"]:
            target = out / "src" / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(git(source, "cat-file", "blob", "%s:%s" % (head, path)))
        manifest = {
            "modified": sorted(owned["modified"]),
            "added": {path: mode for path, mode in sorted(owned["added"])},
            "inventory": [records[p] for p in sorted(owned["modified"])],
        }
        write_json(out / "MANIFEST.json", manifest)
        feature_md = out / "FEATURE.md"
        if not feature_md.exists():
            feature_md.write_text(describe(folder, manifest), encoding="utf-8", newline="\n")
    (ROOT / "SERIES").write_text("\n".join(series) + "\n", encoding="utf-8", newline="\n")
    up = upstream()
    up["exported_from"] = git(source, "rev-parse", head).decode().strip()
    write_json(UPSTREAM, up)
    print("exported %d folders: %d modified files, %d added files, from %s"
          % (len(series), sum(len(plan[f]["modified"]) for f in series),
             sum(len(plan[f]["added"]) for f in series), up["exported_from"][:10]))


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
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
        git(source, "read-tree", base, env=env)
        for folder in series_folders():
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


PATCH_FILE = re.compile(r"^diff --git a/(\S+) b/")


def added_lines(patch: Path) -> dict:
    """Mozilla file -> lines we add to it, read from a hooks.patch."""
    counts, current = {}, None
    for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
        m = PATCH_FILE.match(line)
        if m:
            current = m.group(1)
            counts[current] = 0
        elif current and line.startswith("+") and not line.startswith("+++"):
            counts[current] += 1
    return counts


def problems() -> list:
    """Every rule of the series that is broken now, as sentences."""
    found = []
    listed, present = series_folders(), existing_folders()
    if sorted(listed) != present:
        found.append("SERIES and the folders differ: only in SERIES %s, only on disk %s"
                     % (sorted(set(listed) - set(present)), sorted(set(present) - set(listed))))
    if listed != sorted(listed, key=layout.folder_sort_key):
        found.append("SERIES is not in layout.GROUP_ORDER order")
    budget = json.loads(BUDGET.read_text(encoding="utf-8"))
    seen = {}
    for folder in present:
        d = ROOT / folder
        if not (d / "FEATURE.md").exists():
            found.append("%s has no FEATURE.md" % folder)
        manifest = json.loads((d / "MANIFEST.json").read_text(encoding="utf-8"))
        for path in manifest["added"]:
            if not (d / "src" / path).is_file():
                found.append("%s: MANIFEST lists %s, src/ does not have it" % (folder, path))
        if (d / "src").exists():
            on_disk = {str(p.relative_to(d / "src")).replace("\\", "/")
                       for p in (d / "src").rglob("*") if p.is_file()}
            for extra in sorted(on_disk - set(manifest["added"])):
                found.append("%s: src/%s is not in its MANIFEST" % (folder, extra))
        patch = d / "hooks.patch"
        touched = added_lines(patch) if patch.exists() else {}
        if sorted(touched) != sorted(manifest["modified"]):
            found.append("%s: hooks.patch touches %s, MANIFEST owns %s"
                         % (folder, sorted(touched), manifest["modified"]))
        for path, n in touched.items():
            if path in seen:
                found.append("%s is patched by both %s and %s" % (path, seen[path], folder))
            seen[path] = folder
            allowed = budget.get(path)
            if allowed is None:
                found.append("%s (%s) is a Mozilla file with no hook budget: a new hook is at most 5 "
                             "lines; add it to tools/hook-budget.json with its real count" % (path, folder))
            elif n > allowed:
                found.append("%s (%s) carries %d of our lines, its budget is %d: move the logic "
                             "into a file of ours and leave a hook" % (path, folder, n, allowed))
    for path in sorted(set(budget) - set(seen)):
        found.append("tools/hook-budget.json still lists %s, which no patch touches: remove it" % path)
    return found


def check() -> int:
    found = problems()
    for p in found:
        print("FAIL", p)
    if not found:
        total = sum(json.loads(BUDGET.read_text(encoding="utf-8")).values())
        print("ok: %d folders, %d of our lines in Mozilla files" % (len(series_folders()), total))
    return 1 if found else 0


def shrink_budget() -> None:
    """Lowers each budget to the lines the patches carry now. Never raises one."""
    budget = json.loads(BUDGET.read_text(encoding="utf-8"))
    for folder in existing_folders():
        patch = ROOT / folder / "hooks.patch"
        for path, n in (added_lines(patch) if patch.exists() else {}).items():
            if path in budget and n < budget[path]:
                print("%s: %d -> %d" % (path, budget[path], n))
                budget[path] = n
    write_json(BUDGET, dict(sorted(budget.items())))


def status(source: Path, head: str) -> int:
    up = upstream()
    now = git(source, "rev-parse", head).decode().strip()
    if now == up.get("exported_from"):
        print("in step: the series is %s at %s" % (head, now[:10]))
        return 0
    behind = git(source, "log", "--oneline", "%s..%s" % (up["exported_from"], head)).decode()
    print("the source moved: %s is %s, the series was exported from %s\n%s\nrun export, then verify"
          % (head, now[:10], up["exported_from"][:10], behind))
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["status", "export", "verify", "check", "shrink-budget"])
    ap.add_argument("--source", type=Path, default=Path(os.environ.get("ENGINE_SOURCE", "C:/ff/source")))
    ap.add_argument("--base")
    ap.add_argument("--head")
    a = ap.parse_args()
    up = upstream()
    base, head = a.base or up["mozilla_tag"], a.head or up["source_branch"]
    if a.command == "check":
        return check()
    if a.command == "shrink-budget":
        shrink_budget()
        return 0
    if a.command == "status":
        return status(a.source, head)
    if a.command == "export":
        export(a.source, base, head)
        return 0
    return verify(a.source, base, head)


if __name__ == "__main__":
    sys.exit(main())
