# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Read our build files in mozbuild's own sandbox, without configure.

A build file error (an unsorted list, a variable assigned twice, an include
that does not exist, a source file that is not there) otherwise surfaces only
when a CI build reaches the backend, an hour of runner time later: the first
candidate of the hook layout stopped on toolkit/components/urlformatter's
EXTRA_JS_MODULES list, which mozbuild wants sorted.

Checked: every moz.build or *.mozbuild that carries a STEALTH-HOOK marker or
names one of our Stealth* files, read once per target (Windows, Linux, macOS)
with the same reader `mach file-info` uses (EmptyConfig: no objdir needed).
Each source, export and module it lists must exist in the tree.

And every source of ours (Stealth*.c/cpp/mm, nr_stealth*) must be named by a
build file or #included by another file. StealthFontFileStream.cpp was split
out of gfxDWriteCommon.cpp and listed nowhere: Linux does not need it, and the
Windows link failed on its symbols an hour and a quarter into the build.

Run: python build/stealth_check_build_files.py   (exit 1 on any problem)
     python build/stealth_check_build_files.py <file>...   (only those)
     python build/stealth_check_build_files.py --self-test
"""

import glob
import os
import re
import subprocess
import sys

TREE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
for p in ["python/mozbuild", "python/mach", "python/mozversioncontrol", "python/mozterm", "python/mozboot"] + [
    os.path.relpath(d, TREE) for d in glob.glob(os.path.join(TREE, "testing/mozbase/*")) + glob.glob(os.path.join(TREE, "third_party/python/*"))
]:
    for sub in ("", "lib", "src"):
        d = os.path.join(TREE, p, sub)
        if os.path.isdir(d):
            sys.path.append(d)

from mozbuild.frontend.context import Path  # noqa: E402
from mozbuild.frontend.reader import BuildReader, EmptyConfig  # noqa: E402

TARGETS = {
    "windows": {"OS_TARGET": "WINNT", "OS_ARCH": "WINNT", "MOZ_WIDGET_TOOLKIT": "windows", "TARGET_CPU": "x86_64", "CC_TYPE": "clang-cl", "TARGET_KERNEL": "WINNT"},
    "linux": {"OS_TARGET": "Linux", "OS_ARCH": "Linux", "MOZ_WIDGET_TOOLKIT": "gtk", "TARGET_CPU": "x86_64", "CC_TYPE": "clang", "TARGET_KERNEL": "Linux", "MOZ_X11": "1", "MOZ_WAYLAND": "1"},
    "macos": {"OS_TARGET": "Darwin", "OS_ARCH": "Darwin", "MOZ_WIDGET_TOOLKIT": "cocoa", "TARGET_CPU": "aarch64", "CC_TYPE": "clang", "TARGET_KERNEL": "Darwin"},
}
# Lists whose entries are files of the source directory (a leading "!" is the
# objdir, a leading "/" the top of the tree).
FILE_LISTS = ("SOURCES", "UNIFIED_SOURCES", "EXPORTS", "EXTRA_JS_MODULES", "XPIDL_SOURCES",
              "WEBIDL_FILES", "IPDL_SOURCES", "MOZ_SRC_FILES", "FINAL_TARGET_FILES", "JS_PREFERENCE_PP_FILES")
OURS = re.compile(r"STEALTH-HOOK|Stealth[A-Z]\w*\.(cpp|h|mm|c|cc|sys\.mjs|js|mozbuild|yaml)")
OUR_SOURCE = re.compile(r"(^|/)((Test)?Stealth[A-Za-z0-9]*|nr_stealth\w*)\.(c|cc|cpp|mm)$")
BUILD_FILES = ("*moz.build", "*.mozbuild", "*.build", "*.gyp", "*.gypi")


def git(*args):
    return subprocess.run(["git", "-C", TREE, *args], capture_output=True).stdout.decode("utf-8", "replace")


def orphans(sources, build_text, include_text):
    """The sources no build file names (in either quote) and no #include
    line pulls in."""
    out = []
    for s in sources:
        b = re.escape(os.path.basename(s))
        if not re.search(r"""["'/]%s["']""" % b, build_text) and not re.search(r'["/]%s"' % b, include_text):
            out.append(s)
    return out


def orphan_problems():
    sources = [p for p in git("ls-files", "-z").split("\0") if OUR_SOURCE.search(p)]
    names = "|".join(sorted({re.escape(os.path.basename(s)) for s in sources}))
    build_text = git("grep", "-hE", names, "--", *BUILD_FILES)
    include_text = git("grep", "-hE", r'#\s*include\s*"[^"]*(%s)"' % names)
    return [f"{s}: no build file lists it and nothing #includes it"
            for s in orphans(sources, build_text, include_text)]


def our_build_files():
    out = []
    for dp, dirs, fs in os.walk(TREE):
        dirs[:] = [d for d in dirs if not d.startswith((".", "obj-")) and d not in ("node_modules",)]
        for f in fs:
            if f == "moz.build" or f.endswith(".mozbuild"):
                p = os.path.join(dp, f)
                try:
                    text = open(p, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                if OURS.search(text):
                    out.append(p)
    return sorted(out)


def entries(value, prefix=""):
    """Every string of a (possibly hierarchical) mozbuild list."""
    if isinstance(value, (list, tuple)):
        for v in value:
            yield from entries(v, prefix)
        return
    if isinstance(value, (str, Path)):  # hierarchical lists answer any attribute: test the type
        yield value
        return
    walk = getattr(value, "walk", None)
    if walk:
        for _, files in walk():
            for f in files:
                yield f
        return
    try:
        for v in value:
            yield from entries(v, prefix)
    except TypeError:
        pass


def check(path, target, substs):
    """Read the moz.build of path's directory with its parents, as `mach
    file-info` does: the parents define the templates (Library,
    GeneratedFile...), and a *.mozbuild is read through the moz.build that
    includes it."""
    problems = []
    build = path if os.path.basename(path) == "moz.build" else os.path.join(os.path.dirname(path), "moz.build")
    rel = os.path.relpath(build, TREE).replace(os.sep, "/")
    cfg = EmptyConfig(TREE, substs=substs)
    try:
        _, contexts = BuildReader(cfg).read_relevant_mozbuilds([rel])
    except Exception as e:  # the reader's errors carry the file and line
        msg = str(e).strip().splitlines()
        hint = [l for l in msg if "Error" in l or "error" in l][-1:] or msg[-1:]
        return [f"{os.path.relpath(path, TREE)} [{target}]: {type(e).__name__}: {' '.join(hint)[:400]}"]
    srcdir = os.path.dirname(build)
    for ctx in contexts:
        if os.path.normcase(os.path.normpath(getattr(ctx, "main_path", ""))) != os.path.normcase(os.path.normpath(build)):
            continue
        for var in FILE_LISTS:
            for e in entries(ctx.get(var, [])):
                full = e.full_path if isinstance(e, Path) else None
                e = str(e)
                if e.startswith("!") or "*" in e or not e or type(e).__name__ == "ObjDirPath":
                    continue
                if full is None:
                    full = os.path.join(TREE, e.lstrip("/")) if e.startswith("/") else os.path.join(srcdir, e)
                if not os.path.exists(full):
                    problems.append(f"{os.path.relpath(path, TREE)} [{target}]: {var} names {e}, which does not exist")
    return problems


def main(argv):
    files = [os.path.abspath(a) for a in argv] or our_build_files()
    if not argv and len(files) < 20:
        print(f"only {len(files)} build files of ours found: the scan is not reading the tree")
        return 1
    problems = []
    for p in files:
        for target, substs in TARGETS.items():
            problems += check(p, target, substs)
    if not argv:
        problems += orphan_problems()
    problems = sorted(set(problems))
    for line in problems:
        print(line)
    print(f"{len(files)} build files, {len(TARGETS)} targets: {len(problems)} problems")
    return 1 if problems else 0


def self_test():
    srcs = ["a/StealthA.cpp", "juggler/StealthB.cpp", "x/nr_stealth_c.cpp", "y/StealthD.cpp", "z/StealthE.cpp"]
    build = ('SOURCES += ["StealthA.cpp"]\n'
             "SOURCES += [\n    'StealthB.cpp',\n]\n"
             '    "x/nr_stealth_c.cpp",\n'
             "# StealthE.cpp, named in a comment only\n")
    got = orphans(srcs, build, '#include "y/StealthD.cpp"\n')
    assert got == ["z/StealthE.cpp"], got
    print("self-test ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        sys.exit(0)
    sys.exit(main(sys.argv[1:]))
