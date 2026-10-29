# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Every `mozilla::Name` in our Stealth* sources names something declared in
namespace mozilla.

Code moved out of a Mozilla file into ours loses that file's `using namespace
mozilla`, and qualifying its names by hand can put a global class in the
wrong namespace: the second candidate of the hook layout failed on
`mozilla::ExpandedPrincipal` (caps/ExpandedPrincipal.h declares it at global
scope), half an hour into the CI build.

The headers that mention a name are read with their namespace blocks
followed brace by brace. A name is reported when headers declare it (class,
struct, union, enum, enumerator, namespace, using, typedef, function or
variable) and none of those declarations sits directly in namespace mozilla.
A name no header declares in a form this reads (an enumerator a macro
generates, say) is not reported: this is a text check, not a compiler, so it
can miss, but what it reports is wrong.

Run: python build/stealth_check_namespaces.py [file...]   (exit 1 on any problem)
"""

import os
import re
import subprocess
import sys

TREE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OURS = re.compile(r"(^|/)Stealth[A-Za-z0-9]*\.(h|cpp|mm)$")
QUALIFIED = re.compile(r"(?<![\w:])mozilla::([A-Za-z_]\w*)\b(?!::)")
COMMENTS = re.compile(r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.S)
TOKEN = re.compile(r"::|[A-Za-z_]\w*|\S")
KEYWORDS = {"return", "case", "else", "new", "delete", "throw", "sizeof", "decltype", "operator"}


def git(*args):
    return subprocess.run(["git", "-C", TREE, *args], capture_output=True, check=True).stdout.decode()


def strip(text):
    text = COMMENTS.sub(lambda m: " " if m.group(0)[0] == "/" else '""', text)
    return re.sub(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", "", text, flags=re.M)  # preprocessor lines


def declarations(text, wanted):
    """{name: set of namespace paths} for the declarations of `wanted` names.
    A path is a tuple of namespace names; None means inside a class or a
    function body, which no `mozilla::Name` can reach."""
    found = {}
    stack = []  # one entry per open brace: ("ns", names) | ("enum", scoped) | ("other",)
    stmt = []

    def path():
        if any(e[0] == "other" for e in stack):
            return None
        return tuple(part for e in stack if e[0] == "ns" for part in e[1])

    def note(name, where):
        if name in wanted:
            found.setdefault(name, set()).add(where)

    def statement(toks, at):
        if not toks or at is None:
            return
        for i, t in enumerate(toks):
            if t in ("class", "struct", "union", "enum", "namespace"):
                j = i + 1
                while j < len(toks) and (toks[j] in ("class", "struct") or re.match(r"(MOZ|NS|JS|alignas)_?\w*$", toks[j]) or toks[j] in "[]()"):
                    j += 1
                if j < len(toks) and re.match(r"\w+$", toks[j]):
                    note(toks[j], at)
            if t == "using" and i + 2 < len(toks) and toks[i + 2] == "=":
                note(toks[i + 1], at)
            if t == "using" and toks[-1:] != ["="]:
                note(toks[-1], at)
        if toks[0] == "typedef":
            note(toks[-1], at)
        for i, t in enumerate(toks[1:], 1):
            nxt = toks[i + 1] if i + 1 < len(toks) else ";"
            prev = toks[i - 1]
            if t in wanted and nxt in ("(", "=", "[", "{", ";", ",") and prev not in KEYWORDS and prev != "::" \
                    and (re.match(r"\w+$", prev) or prev in ("*", "&", ">")):
                note(t, at)

    for tok in TOKEN.findall(text):
        if tok in ("{", "}", ";") or (tok == "," and stack and stack[-1][0] == "enum"):
            top = stack[-1] if stack else None
            if top and top[0] == "enum":
                if stmt and re.match(r"\w+$", stmt[0]):
                    p = path()
                    note(stmt[0], None if p is None or top[1] else p)
            else:
                statement(stmt, path())
            if tok == "{":
                s = " ".join(stmt)
                m = re.search(r"\bnamespace\s*(\w*(?:\s*::\s*\w+)*)\s*$", s)
                if m:  # `namespace a::b {` is one brace; an anonymous namespace adds nothing
                    stack.append(("ns", tuple(m.group(1).replace(" ", "").split("::")) if m.group(1) else ()))
                elif re.search(r'\bextern\s*""\s*$', s):
                    stack.append(("ns", ()))
                elif re.search(r"\benum\b", s) and not re.search(r"[()=]", s.split("enum", 1)[1]):
                    stack.append(("enum", bool(re.search(r"\benum\s+(class|struct)\b", s))))
                else:
                    stack.append(("other",))
            elif tok == "}":
                if stack:
                    stack.pop()
            stmt = []
        else:
            stmt.append(tok)
    return found


def problems(names, headers):
    """names: {name: file of ours}; headers: {path: text}."""
    decl = {}
    for text in headers.values():
        for n, paths in declarations(strip(text), set(names)).items():
            # a member or a scoped enumerator says nothing about namespace scope
            if paths - {None}:
                decl.setdefault(n, set()).update(paths - {None})
    return [f"{rel}: mozilla::{n} is declared only outside namespace mozilla"
            for n, rel in sorted(names.items())
            if n in decl and ("mozilla",) not in decl[n]]


def qualified_names(files):
    names = {}
    for rel in files:
        text = strip(open(os.path.join(TREE, rel), encoding="utf-8", errors="replace").read())
        for n in QUALIFIED.findall(text):
            names.setdefault(n, rel)
    return names


def main(argv):
    files = argv or [p for p in git("ls-files", "-z", "--", "*.h", "*.cpp", "*.mm").split("\0") if OURS.search(p)]
    if not argv and len(files) < 50:
        print(f"only {len(files)} Stealth* sources found: the scan is not reading the tree")
        return 1
    names = qualified_names(files)
    pattern = r"\b(" + "|".join(sorted(names)) + r")\b"
    hits = [p for p in git("grep", "-lzE", pattern, "--", "*.h").split("\0") if p]
    headers = {h: open(os.path.join(TREE, h), encoding="utf-8", errors="replace").read() for h in hits}
    bad = problems(names, headers)
    for line in bad:
        print(line)
    print(f"{len(files)} Stealth* sources, {len(names)} mozilla:: names, {len(headers)} headers read: {len(bad)} problems")
    return 1 if bad else 0


def self_test():
    h = {"caps/E.h": "class E : public I {};\n",
         "caps/B.h": "class E;\nnamespace mozilla {\nclass B { template <class T> bool Is(); };\n"
                     "namespace CanvasUtils { void F(); }\nenum Msg { eA, eB = 2 };\nenum class S { eC };\n}\n",
         "x/N.h": "namespace mozilla::dom { class D; }\nnamespace mozilla { namespace detail { class Q; } }\n"}
    assert problems({"E": "a"}, h) == ["a: mozilla::E is declared only outside namespace mozilla"], problems({"E": "a"}, h)
    assert problems({"B": "a", "CanvasUtils": "a", "eA": "a", "eB": "a", "Gen": "a"}, h) == []
    assert problems({"eC": "a"}, h) == [], "only a scoped enumerator: undecided, not reported"
    assert problems({"D": "a"}, h) and problems({"Q": "a"}, h), "nested namespaces are not mozilla"
    print("self-test ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test(); sys.exit(0)
    sys.exit(main(sys.argv[1:]))
