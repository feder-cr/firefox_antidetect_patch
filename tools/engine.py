"""The engine as a patch series: export it from a Firefox tree, rebuild a tree from it.

    python tools/engine.py status    did the source move since the last export?
    python tools/engine.py export    write the series from the source tree
    python tools/engine.py verify    rebuild the tree from the series, compare
    python tools/engine.py check     the series' own rules, offline (also in CI)
    python tools/engine.py apply --head <branch>   the series as a branch: tag + one commit per folder
    python tools/engine.py materialize --into <dir> CI: fetch the tag, apply, check out the tree
    python tools/engine.py drift --to <tag>   our patched Mozilla files that Mozilla changed
    python tools/engine.py port --to <tag>    every folder merged three-way onto the new tag,
                                              all conflicts in one run, branch port/<tag>

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
INVENTORY = ROOT / "tools" / "inventory.json"
UPSTREAM = ROOT / "UPSTREAM.json"
BUDGET = ROOT / "tools" / "hook-budget.json"
# The workflows live in the series (the tree's tests read them beside scripts/), and
# GitHub runs them from this repository's root: export copies them there and check
# requires the two identical. series.yml is the root's own.
WORKFLOWS_SERIES = ROOT / "platform" / "ci" / "src" / ".github" / "workflows"
WORKFLOWS_ROOT = ROOT / ".github" / "workflows"
ROOT_ONLY_WORKFLOWS = {"series.yml"}


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
        if status in ("M", "D"):   # D: a Mozilla file we delete is ours to own like any change
            if path not in records:
                raise SystemExit("modified file %s is not in the inventory: add its record to %s "
                                 "(feature, category, anchor, hookable, notes)" % (path, INVENTORY.name))
            plan[layout.feature_folder(records[path]["feature"])]["modified"].append(path)
        elif status == "A":
            plan[layout.own_file_folder(path)]["added"].append((path, mode))
        else:
            raise SystemExit("status %s for %s is not handled yet" % (status, path))
    return plan, records


def require_descends(source: Path, base: str, head: str) -> None:
    """The branch must be built on the tag: a diff between unrelated commits would
    export Mozilla's own changes as ours."""
    if subprocess.run(["git", "-C", str(source), "merge-base", "--is-ancestor", base, head],
                      capture_output=True).returncode:
        raise SystemExit("%s does not descend from %s: the series is the difference from the tag, "
                         "so the branch must be built on it (tools/engine.py apply)" % (head, base))


def export(source: Path, base: str, head: str) -> None:
    require_descends(source, base, head)
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
    up["exported_tree"] = git(source, "rev-parse", "%s^{tree}" % head).decode().strip()
    write_json(UPSTREAM, up)
    print("exported %d folders: %d modified files, %d added files, from %s"
          % (len(series), sum(len(plan[f]["modified"]) for f in series),
             sum(len(plan[f]["added"]) for f in series), up["exported_from"][:10]))


def sync_workflows() -> None:
    WORKFLOWS_ROOT.mkdir(parents=True, exist_ok=True)
    for old in WORKFLOWS_ROOT.glob("*.yml"):
        if old.name not in ROOT_ONLY_WORKFLOWS:
            old.unlink()
    for wf in WORKFLOWS_SERIES.glob("*.yml"):
        (WORKFLOWS_ROOT / wf.name).write_bytes(wf.read_bytes())


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


IDENTITY = {"GIT_AUTHOR_NAME": "feder-cr", "GIT_COMMITTER_NAME": "feder-cr",
            "GIT_AUTHOR_EMAIL": "85809106+feder-cr@users.noreply.github.com",
            "GIT_COMMITTER_EMAIL": "85809106+feder-cr@users.noreply.github.com"}


def apply(source: Path, base: str, branch: str, force: bool) -> str:
    """Builds the engine in the source repository as branch `branch`: the Mozilla tag,
    then one commit per folder of SERIES, in order. No checkout: the branch is written
    with git plumbing, so it works in a bare repository and does not touch a worktree.
    The commit of a folder is titled with the folder, so `git log` is the layer stack."""
    ref = "refs/heads/%s" % branch
    exists = subprocess.run(["git", "-C", str(source), "rev-parse", "-q", "--verify", ref],
                            capture_output=True).returncode == 0
    if exists and not force:
        raise SystemExit("%s already exists in %s: pass --force to rebuild it (its commits are lost "
                         "unless exported first)" % (branch, source))
    layers = build_stack(source, base)
    parent, tree = layers[-1][1], layers[-1][2]
    git(source, "update-ref", ref, parent)
    print("%s: %s on top of %s, %d layers (tree %s)"
          % (branch, parent[:10], base, len(series_folders()), tree[:12]))
    return tree


def build_stack(source: Path, base: str) -> list:
    """The series on `base`, as [(folder, commit, tree)] in SERIES order (the first entry
    is the tag itself, folder None). Writes objects only, moves no ref."""
    parent = git(source, "rev-parse", "%s^{commit}" % base).decode().strip()
    layers = [(None, parent, git(source, "rev-parse", "%s^{tree}" % parent).decode().strip())]
    # Every layer is dated with the tag's own commit date: the same series on the same
    # tag gives the same commits, byte for byte, on any machine. That makes the commit
    # a seal records (source_commit) reproducible by anyone who has main and the tag,
    # although it lives on no remote.
    when = git(source, "log", "-1", "--format=%cI", parent).decode().strip()
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"), **IDENTITY,
                   GIT_AUTHOR_DATE=when, GIT_COMMITTER_DATE=when)
        git(source, "read-tree", parent, env=env)
        for folder in series_folders():
            patch = ROOT / folder / "hooks.patch"
            if patch.exists():
                done = subprocess.run(["git", "-C", str(source), "apply", "--cached", "--whitespace=nowarn",
                                       str(patch)], capture_output=True, env=env)
                if done.returncode:
                    raise SystemExit("%s does not apply on %s:\n%s"
                                     % (folder, base, done.stderr.decode(errors="replace")))
            manifest = json.loads((ROOT / folder / "MANIFEST.json").read_text(encoding="utf-8"))
            for path, mode in manifest["added"].items():
                data = (ROOT / folder / "src" / path).read_bytes()
                sha = git(source, "hash-object", "-w", "--stdin", input_bytes=data).decode().strip()
                git(source, "update-index", "--add", "--cacheinfo", "%s,%s,%s" % (mode, sha, path),
                    env=env)
            tree = git(source, "write-tree", env=env).decode().strip()
            parent = git(source, "commit-tree", tree, "-p", parent, "-m", folder, env=env).decode().strip()
            layers.append((folder, parent, tree))
    return layers


def port(source: Path, old: str, new: str, branch: str, force: bool) -> int:
    """Moves the series from Mozilla tag `old` to tag `new` in one run, every folder
    merged three-way, and reports every conflict instead of stopping at the first.

    The series is applied to `old`, where it applies cleanly, giving layers L1..Ln.
    Then each layer is replayed on `new`: Ni = merge(base L(i-1), ours N(i-1), theirs Li)
    with `git merge-tree`, so each folder's own change meets what Mozilla changed.
    A conflicted folder still gets its commit (files with conflict markers) and the
    run goes on. The result is branch `branch` in the source repository: resolve the
    markers there, commit, then export. Both tags must be in the source repository
    (the blobs of `old` are the base of every merge)."""
    for tag in (old, new):
        if subprocess.run(["git", "-C", str(source), "rev-parse", "-q", "--verify", "%s^{commit}" % tag],
                          capture_output=True).returncode:
            raise SystemExit("%s is not in %s: fetch it first, e.g. git -C %s fetch --depth 1 %s "
                             "refs/tags/%s:refs/tags/%s" % (tag, source, source, upstream()["mozilla_repository"], tag, tag))
    ref = "refs/heads/%s" % branch
    if subprocess.run(["git", "-C", str(source), "rev-parse", "-q", "--verify", ref],
                      capture_output=True).returncode == 0 and not force:
        raise SystemExit("%s already exists: pass --force to rebuild it" % branch)
    stack = build_stack(source, old)
    current = git(source, "rev-parse", "%s^{commit}" % new).decode().strip()
    when = git(source, "log", "-1", "--format=%cI", current).decode().strip()
    env = dict(os.environ, **IDENTITY, GIT_AUTHOR_DATE=when, GIT_COMMITTER_DATE=when)
    report = []
    for (_, base_commit, _), (folder, layer, _) in zip(stack, stack[1:]):
        done = subprocess.run(["git", "-C", str(source), "merge-tree", "--write-tree", "--name-only",
                               "--no-messages", "--merge-base=%s" % base_commit, current, layer],
                              capture_output=True)
        lines = done.stdout.decode(errors="replace").split("\n")
        if done.returncode not in (0, 1) or not lines[0].strip():
            raise SystemExit("merge-tree failed on %s: %s" % (folder, done.stderr.decode(errors="replace")))
        tree, conflicts = lines[0].strip(), sorted({l for l in lines[1:] if l.strip()})
        title = folder if not conflicts else "%s (CONFLICT: %s)" % (folder, ", ".join(conflicts))
        current = git(source, "commit-tree", tree, "-p", current, "-m", title, env=env).decode().strip()
        report.append((folder, conflicts))
    git(source, "update-ref", ref, current)
    bad = [(f, c) for f, c in report if c]
    for folder, conflicts in bad:
        print("CONFLICT %s: %s" % (folder, ", ".join(conflicts)))
    print("%s: %d folders moved from %s to %s, %d clean, %d with conflicts (%d files)"
          % (branch, len(report), old, new, len(report) - len(bad), len(bad), sum(len(c) for _, c in bad)))
    return 1 if bad else 0


def drift(source: Path, old: str, new: str) -> int:
    """Which Mozilla files we patch did Mozilla change between the two tags: the files
    a port has to look at, whether or not they conflict."""
    ours = sorted(r["file"] for r in json.loads(INVENTORY.read_text(encoding="utf-8")))
    out = git(source, "diff", "--name-status", "--no-renames", old, new, "--", *ours).decode()
    rows = [l.split("\t", 1) for l in out.splitlines() if l.strip()]
    for status, path in rows:
        print("%s %s" % ({"M": "changed", "D": "DELETED by Mozilla", "A": "added"}.get(status, status), path))
    print("%d of our %d patched Mozilla files changed between %s and %s" % (len(rows), len(ours), old, new))
    return 0


def materialize(into: Path, branch: str = "engine") -> str:
    """For CI: makes `into` the engine's source tree, exactly as a checkout of the old
    full-tree branch was. Fetches the Mozilla tag one commit deep from Mozilla's
    repository, applies the series as `branch`, checks it out, and requires the tree to
    be the one UPSTREAM.json declares. Prints the built commit."""
    up = upstream()
    into.mkdir(parents=True, exist_ok=True)
    if not (into / ".git").exists():
        git(into, "init", "-q")
    exclude = into / ".git" / "info" / "exclude"
    exclude.parent.mkdir(parents=True, exist_ok=True)
    rel = os.path.relpath(ROOT, into).replace("\\", "/")
    if not rel.startswith(".."):
        exclude.write_text("/%s/\n" % rel, encoding="utf-8")
    tag = up["mozilla_tag"]
    git(into, "fetch", "-q", "--depth", "1", "--no-tags", up["mozilla_repository"],
        "refs/tags/%s:refs/tags/%s" % (tag, tag))
    tree = apply(into, tag, branch, force=True)
    expected = up.get("exported_tree")
    if expected and tree != expected:
        raise SystemExit("the series applied to %s gives tree %s, UPSTREAM.json declares %s"
                         % (tag, tree, expected))
    # The working tree follows the machine's git config (core.autocrlf), exactly as the
    # actions/checkout this replaces did: the Windows legs build what they built before.
    git(into, "checkout", "-q", "-f", branch)
    head = git(into, "rev-parse", "HEAD").decode().strip()
    print("materialized %s: %s (tree %s)" % (into, head, tree[:12]))
    return head


def verify(source: Path, base: str, head: str) -> int:
    require_descends(source, base, head)
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
    series_wf = {p.name: p.read_bytes() for p in WORKFLOWS_SERIES.glob("*.yml")}
    root_wf = {p.name: p.read_bytes() for p in WORKFLOWS_ROOT.glob("*.yml")}
    for name in sorted(set(series_wf) | set(root_wf) - ROOT_ONLY_WORKFLOWS):
        if series_wf.get(name) != root_wf.get(name):
            found.append(".github/workflows/%s differs from the series' copy (platform/ci/src): "
                         "edit it in the source, then export" % name)
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
    ap.add_argument("command", choices=["status", "apply", "materialize", "export", "verify", "check",
                                        "shrink-budget", "drift", "port"])
    ap.add_argument("--to", help="drift/port: the Mozilla tag to move to (a stable release, never a beta)")
    ap.add_argument("--into", type=Path, default=Path("."), help="materialize: the directory to make the tree")
    ap.add_argument("--source", type=Path, default=Path(os.environ.get("ENGINE_SOURCE", "C:/ff/source")))
    ap.add_argument("--base")
    ap.add_argument("--head", help="the source branch (apply: the branch it creates)")
    ap.add_argument("--force", action="store_true", help="apply: rebuild an existing branch")
    a = ap.parse_args()
    up = upstream()
    base, head = a.base or up["mozilla_tag"], a.head or up["source_branch"]
    if a.command in ("drift", "port"):
        if not a.to:
            raise SystemExit("%s needs --to <Mozilla tag>" % a.command)
        if a.command == "drift":
            return drift(a.source, base, a.to)
        return port(a.source, base, a.to, a.head if a.head else "port/%s" % a.to, a.force)
    if a.command == "materialize":
        materialize(a.into)
        return 0
    if a.command == "apply":
        apply(a.source, base, head, a.force)
        return 0
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
