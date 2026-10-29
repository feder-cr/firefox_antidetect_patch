# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Text checks on our C/C++ sources for mistakes a compiler finds only on a
CI runner, an hour into the build. Each rule comes from a candidate build that
failed on it.

1. Every `mozilla::Name` names something declared in namespace mozilla.
   Code moved out of a Mozilla file loses that file's `using namespace
   mozilla`, and qualifying its names by hand put a global class in the wrong
   namespace: `mozilla::ExpandedPrincipal` (caps/ExpandedPrincipal.h declares
   it at global scope) failed on every platform. The headers that mention a
   name are read with their namespace blocks followed brace by brace; a name
   is reported when headers declare it (class, struct, union, enum,
   enumerator, namespace, using, typedef, function or variable) and none of
   those declarations sits directly in namespace mozilla. A name no header
   declares in a form this reads (an enumerator a macro generates, say) is
   not reported.

2. No identifier that a Windows header defines as a macro: windef.h makes
   `near` and `far` empty, rpcndr.h makes `small` a char, and so on. A local
   `bool near;` compiled on Linux and became `bool ;` on Windows.

3. In our headers, nrappkit's log levels (LOG_INFO, LOG_WARNING, ...) appear
   only inside namespace mozilla. nr_socket_prsock.cpp undefines them around
   Mozilla's includes and redefines them to names that resolve only there
   (mozilla::LOG_INFO on Windows), as its own r_log calls do; a header of
   ours used LOG_INFO at global scope and did not compile on Windows.

These are text checks, not a compiler: they can miss, but what they report
is wrong.

Run: python build/stealth_check_sources.py [file...]   (exit 1 on any problem)
     python build/stealth_check_sources.py --self-test
"""

import os
import re
import subprocess
import sys

TREE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OURS = re.compile(r"(^|/)((Test)?Stealth[A-Za-z0-9]*|nr_stealth\w*)\.(h|c|cpp|mm)$")
# Lower-case or short names that Windows headers #define (windef.h, rpcndr.h,
# objbase.h, wingdi.h, winnt.h, minwindef.h).
WINDOWS_MACROS = {"near", "far", "NEAR", "FAR", "small", "hyper", "interface", "pascal", "cdecl",
                  "IN", "OUT", "OPTIONAL", "ERROR", "DELETE", "TRANSPARENT", "OPAQUE"}
NR_LOG_LEVELS = {"LOG_EMERG", "LOG_ALERT", "LOG_CRIT", "LOG_ERR", "LOG_WARNING", "LOG_NOTICE",
                 "LOG_INFO", "LOG_DEBUG"}
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


def scoped_tokens(text):
    """(identifier, enclosing namespaces) for every identifier of stripped text.
    Class and function bodies do not change the namespaces."""
    stack = []  # namespace names per open brace (None for other braces)
    stmt = []
    for tok in TOKEN.findall(text):
        if tok == "{":
            m = re.search(r"\bnamespace\s*(\w*(?:\s*::\s*\w+)*)\s*$", " ".join(stmt))
            stack.append(tuple(m.group(1).replace(" ", "").split("::")) if m and m.group(1) else (() if m else None))
            stmt = []
        elif tok == "}":
            if stack:
                stack.pop()
            stmt = []
        elif tok == ";":
            stmt = []
        else:
            stmt.append(tok)
            if re.match(r"[A-Za-z_]\w*$", tok):
                yield tok, tuple(part for e in stack if e for part in e)


def text_problems(rel, text):
    """Rules 2 and 3 on one file of ours (raw text)."""
    bad = []
    for tok, ns in scoped_tokens(strip(text)):
        if tok in WINDOWS_MACROS:
            bad.append(f"{rel}: `{tok}` is a macro in Windows headers")
        if tok in NR_LOG_LEVELS and rel.endswith(".h") and ns[:1] != ("mozilla",):
            bad.append(f"{rel}: {tok} outside namespace mozilla")
    return sorted(set(bad))


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
    files = argv or [p for p in git("ls-files", "-z", "--", "*.h", "*.c", "*.cpp", "*.mm").split("\0") if OURS.search(p)]
    if not argv and len(files) < 50:
        print(f"only {len(files)} Stealth* sources found: the scan is not reading the tree")
        return 1
    names = qualified_names(files)
    pattern = r"\b(" + "|".join(sorted(names)) + r")\b"
    hits = [p for p in git("grep", "-lzE", pattern, "--", "*.h").split("\0") if p]
    headers = {h: open(os.path.join(TREE, h), encoding="utf-8", errors="replace").read() for h in hits}
    bad = problems(names, headers)
    for rel in files:
        bad += text_problems(rel, open(os.path.join(TREE, rel), encoding="utf-8", errors="replace").read())
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
    h["x/G.h"] = "namespace mozilla::dom { class D2; }\nclass G;\n"
    assert problems({"G": "a"}, h), "`namespace a::b {` closes both levels at its brace"
    assert text_problems("a.cpp", "bool f() { bool near; near = 1; return near; }")
    assert text_problems("a.cpp", "// near\nbool f() { const char* s = \"near\"; bool nearSegment; }") == []
    hdr = 'inline void F() { r_log(LOG_GENERIC, LOG_INFO, "x"); }\n'
    assert text_problems("a.h", hdr) == ["a.h: LOG_INFO outside namespace mozilla"]
    assert text_problems("a.h", "namespace mozilla {\n" + hdr + "}\n") == []
    assert text_problems("a.cpp", hdr) == [], "a .cpp of ours sets its own context"
    print("self-test ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test(); sys.exit(0)
    sys.exit(main(sys.argv[1:]))
