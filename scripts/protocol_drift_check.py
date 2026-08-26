#!/usr/bin/env python3
"""Fail when the installed Playwright sends juggler fields our Protocol.js does not declare.

Why this exists
---------------
Juggler's `checkScheme` validator is closed-world: a command payload carrying any
field the scheme does not declare is rejected outright, at runtime, with
"Found property <root>.x which is not described in this scheme". Nothing about
that is visible to the compiler or to a smoke test that only opens a page - the
browser builds, launches, and loads URLs perfectly. It surfaces only when a
client exercises the specific API, which is why the FF151 rebase shipped two
separate breakages of this kind:

  Browser.setDefaultViewport  viewport.isMobile     (1.61, caught by hand)
  Browser.setDefaultViewport  viewport.screenSize   (1.61, broke 97 of 133 e2e)
  Page.setViewportSize        isMobile, screenSize  (1.61, page.set_viewport_size)

A version bump of either side can add more. This check recovers the exact set of
payloads the installed Playwright emits and compares it against what we declare,
so the drift is caught before a build instead of after a release.

How it works
------------
Playwright ships its server bundled into one JS file, but the bundle keeps the
source-module markers, so the firefox client region (ffConnection..firefox.ts)
can be isolated exactly. Protocol payloads inside it are plain object literals -
wire field names cannot be minified - so every `.send("Domain.method", {...})`
in that region yields the literal field set for that command. Each field is then
resolved against our declaration, following pageTypes/browserTypes references.

Usage:  python scripts/protocol_drift_check.py [--proto juggler/protocol/Protocol.js]
Exit 0 = no drift, 1 = undeclared fields (or the bundle could not be located).
"""
from __future__ import annotations

import argparse
import os
import re
import sys

FF_MODULES = ("firefox/ffConnection.ts", "firefox/firefox.ts")


# --------------------------------------------------------------------------
#  tiny JS literal reader (enough for protocol payloads: objects, no regexes)
# --------------------------------------------------------------------------
def strip_comments(s: str) -> str:
    """Remove // and /* */ comments, preserving string contents and length-independence."""
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c in "\"'`":
            q = c
            out.append(c)
            i += 1
            while i < n:
                if s[i] == "\\":
                    out.append(s[i:i + 2])
                    i += 2
                    continue
                out.append(s[i])
                if s[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and s[i + 1] == "/":
            while i < n and s[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and s[i + 1] == "*":
            i = s.find("*/", i + 2)
            i = n if i < 0 else i + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def balanced(s: str, i: int) -> str | None:
    """Return the {...} literal starting at s[i]."""
    depth, j, instr = 0, i, None
    while j < len(s):
        c = s[j]
        if instr:
            if c == "\\":
                j += 2
                continue
            if c == instr:
                instr = None
        elif c in "\"'`":
            instr = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return s[i:j + 1]
        j += 1
    return None


def split_top(body: str) -> list[str]:
    """Split an object-literal body on top-level commas."""
    depth, instr, start, parts = 0, None, 0, []
    for j, c in enumerate(body):
        if instr:
            if c == "\\":
                continue
            if c == instr:
                instr = None
            continue
        if c in "\"'`":
            instr = c
        elif c in "{[(":
            depth += 1
        elif c in "}])":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(body[start:j])
            start = j + 1
    parts.append(body[start:])
    return parts


def keypaths(lit: str, region: str = "", prefix: str = "") -> list[str]:
    """Dotted field paths of an object literal.

    `region` lets a shorthand or identifier value ({ browserContextId, viewport })
    be expanded by finding the `const viewport = { ... }` that built it - which is
    exactly how Playwright assembles the setDefaultViewport payload, and where the
    screenSize regression hid.
    """
    out: list[str] = []
    for part in split_top(lit[1:-1]):
        part = part.strip()
        if not part or part.startswith("..."):
            continue
        m = re.match(r'^["\']?([A-Za-z_$][\w$]*)["\']?\s*:\s*(.*)$', part, re.S)
        if m:
            key, val = m.group(1), m.group(2).strip()
        elif re.match(r"^[A-Za-z_$][\w$]*$", part):
            key, val = part, part          # shorthand { viewport }
        else:
            continue
        out.append(prefix + key)
        sub = None
        if val.startswith("{"):
            sub = balanced(val, 0)
        elif region and re.match(r"^[A-Za-z_$][\w$]*$", val):
            vm = re.search(r"\b(?:const|let|var)\s+%s\s*=\s*\{" % re.escape(val), region)
            if vm:
                sub = balanced(region, vm.end() - 1)
        if sub:
            out += keypaths(sub, region, prefix + key + ".")
    return out


# --------------------------------------------------------------------------
#  inputs
# --------------------------------------------------------------------------
def find_client_source() -> tuple[str, str] | None:
    """Locate the installed client's firefox code: (label, source text).

    Two layouts exist in the versions we support. Recent Playwright bundles the
    whole server into lib/coreBundle.js (module markers kept, so the firefox
    region is recoverable); older releases ship lib/server/firefox/*.js as
    separate files, which are simply concatenated here. Supporting both matters
    because the pin spans several minors, and a checker that silently skips on
    the older layout would leave exactly those clients unverified.
    """
    try:
        import playwright  # noqa: F401  (import only to locate the package)
    except ImportError:
        return None
    root = os.path.dirname(playwright.__file__)

    bundle = os.path.join(root, "driver", "package", "lib", "coreBundle.js")
    if not os.path.exists(bundle):
        for dirpath, _dirs, files in os.walk(os.path.join(root, "driver")):
            if "coreBundle.js" in files:
                bundle = os.path.join(dirpath, "coreBundle.js")
                break
    if os.path.exists(bundle):
        src = open(bundle, encoding="utf-8", errors="replace").read()
        return ("coreBundle.js", firefox_region(src))

    ffdir = os.path.join(root, "driver", "package", "lib", "server", "firefox")
    if os.path.isdir(ffdir):
        parts = []
        for name in sorted(os.listdir(ffdir)):
            if name.endswith(".js"):
                parts.append(open(os.path.join(ffdir, name), encoding="utf-8",
                                  errors="replace").read())
        if parts:
            return ("lib/server/firefox/*.js", "\n".join(parts))
    return None


def firefox_region(src: str) -> str:
    mods = [(m.start(), m.group(1)) for m in
            re.finditer(r"//\s*(packages/playwright-core/src/server/[\w/.-]+)", src)]
    if not mods:
        raise SystemExit("bundle has no module markers - Playwright build layout changed")
    start = next((i for i, n in mods if n.endswith(FF_MODULES[0])), None)
    last = next((i for i, n in mods if n.endswith(FF_MODULES[1])), None)
    if start is None or last is None:
        raise SystemExit("firefox module markers not found in bundle")
    end = next((i for i, _n in mods if i > last), len(src))
    return src[start:end]


def sent_payloads(region: str) -> dict[str, set[str]]:
    out: dict[str, set[str]] = {}
    for m in re.finditer(r'\.send\(\s*"([A-Z]\w*\.\w+)"\s*(?:,\s*(\{))?', region):
        method = m.group(1)
        out.setdefault(method, set())
        if m.group(2):
            lit = balanced(region, m.end() - 1)
            if lit:
                out[method].update(keypaths(lit, region))
    return out


def scheme_fields(lit: str, proto: str, depth: int = 0) -> set[str]:
    """Dotted field paths a scheme literal declares, following *Types.X references.

    Resolution has to recurse: Viewport references pageTypes.Size, so a payload
    field like viewport.viewportSize.width is only reachable two hops in. Depth is
    capped because a scheme may reference itself (nested frames, remote objects).
    """
    keys: set[str] = set()
    if depth > 4:
        return keys
    for part in split_top(lit[1:-1]):
        part = part.strip()
        m = re.match(r'^["\']?([A-Za-z_$][\w$]*)["\']?\s*:\s*(.+)$', part, re.S)
        if not m:
            continue
        key, val = m.group(1), m.group(2)
        keys.add(key)
        sub = None
        ref = re.search(r"\b(\w+Types\.\w+)", val)
        if ref:
            tm = re.search(r"^%s\s*=\s*\{" % re.escape(ref.group(1)), proto, re.M)
            if tm:
                sub = balanced(proto, tm.end() - 1)
        elif val.lstrip().startswith("{"):
            sub = balanced(val, val.index("{"))
        if sub:
            keys.update(key + "." + k for k in scheme_fields(sub, proto, depth + 1))
    return keys


def listened_events(region: str) -> set[str]:
    """Juggler events the client subscribes to.

    The other half of the contract: a command we fail to accept breaks loudly,
    but an event the client waits for and we never declare breaks quietly - the
    client just hangs until its timeout.
    """
    events: set[str] = set()
    for pat in (r'\.on\(\s*"([A-Z]\w*\.\w+)"',
                r'\.once\(\s*"([A-Z]\w*\.\w+)"',
                r'addEventListener\([^,()]{0,60},\s*"([A-Z]\w*\.\w+)"'):
        events |= {m.group(1) for m in re.finditer(pat, region)}
    return events


def declares_event(proto: str, event: str) -> bool:
    domain, name = event.split(".")
    dm = re.search(r"^const %s = \{" % re.escape(domain), proto, re.M)
    if not dm:
        return False
    block = balanced(proto, dm.end() - 1)
    em = re.search(r"events\s*:\s*\{", block or "")
    if not em:
        return False
    elit = balanced(block, em.end() - 1)
    return bool(elit and re.search(r"^\s*'?%s'?\s*:" % re.escape(name), elit, re.M))


def declared(proto: str, method: str) -> set[str] | None:
    """Field paths our Protocol.js declares for a command, or None if unknown."""
    domain, name = method.split(".")
    dm = re.search(r"^const %s = \{" % re.escape(domain), proto, re.M)
    if not dm:
        return None
    block = balanced(proto, dm.end() - 1)
    if not block:
        return None
    mm = re.search(r"^\s*'?%s'?\s*:\s*\{" % re.escape(name), block, re.M)
    if not mm:
        return None
    mblock = balanced(block, mm.end() - 1)
    pm = re.search(r"params\s*:\s*\{", mblock or "")
    if not pm:
        return set()
    plit = balanced(mblock, pm.end() - 1)
    return scheme_fields(plit, proto)


#: ⛔ RIMOZIONI DELIBERATE, ognuna con la voce che la documenta.
#:
#: Questo gate esiste per la divergenza ACCIDENTALE - un campo che il client
#: manda e noi non dichiariamo piu' dopo un rebase, che `checkScheme` rifiuta
#: solo a RUNTIME e con la build verde. Ma dal 2026-08-24 il progetto ha tolto
#: da Juggler dei comandi APPOSTA, con la decisione scritta, e da allora il gate
#: era rosso su quelle scelte: cioe' rosso per sempre, che e' la condizione in
#: cui un gate smette di essere letto e comincia a essere aggirato.
#:
#: Una voce qui dice "so che il client lo manda e so che noi non lo abbiamo
#: piu', ed e' voluto". Tutto cio' che NON e' in questa mappa resta rosso.
#: Aggiungere una riga qui e' una decisione: va fatta solo quando la rimozione
#: e' gia' documentata, e il rimando serve a ritrovarne la ragione.
RIMOSSI_APPOSTA = {
    # NetworkObserver ridotto "all'osso, non a zero" su richiesta del
    # proprietario: via tutta la copia dei corpi delle risposte. `response.body()`
    # RIFIUTA chiaramente, ed e' il comportamento voluto.
    # -> 71-bug-archive.md [B170]; il degrado silenzioso di tracing/HAR che ne
    #    consegue e' aperto in 70-known-bugs.md [B176].
    "Network.getResponseBody":
        "NetworkObserver all'osso [B170]; conseguenze in [B176]",
    # La potatura di TargetRegistry ha tolto l'apparato degli override delle
    # media feature. L'handler ora RIFIUTA i tre parametri invece di ignorarli,
    # perche' ignorarli renderebbe l'API una bugia.
    # -> 20-our-patches.md 28.4, piu' il gate tests/gates/juggler_wiring.py (AU).
    "Browser.setContrast":
        "override delle media feature tolti, l'handler rifiuta (20-our-patches 28.4)",
    "Browser.setForcedColors":
        "override delle media feature tolti, l'handler rifiuta (20-our-patches 28.4)",
    "Browser.setReducedMotion":
        "override delle media feature tolti, l'handler rifiuta (20-our-patches 28.4)",
    # Il sottosistema screencast non era compilato da nessuno: verificato
    # nell'objdir, `Cc['@mozilla.org/juggler/screencast;1']` lanciava sempre.
    # -> il commit "Il sottosistema screencast non era compilato da nessuno".
    "Page.screencastFrame":
        "screencast mai compilato, rimosso e verificato nell'objdir",
}


def main() -> int:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--proto", default=os.path.join(here, "juggler", "protocol", "Protocol.js"))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    found = find_client_source()
    if not found:
        print("FAIL: cannot locate the installed Playwright's firefox client code")
        return 1
    label, region = found
    if not os.path.exists(args.proto):
        print("FAIL: Protocol.js not found at %s" % args.proto)
        return 1

    proto = strip_comments(open(args.proto, encoding="utf-8", errors="replace").read())

    sent = sent_payloads(region)
    events = listened_events(region)
    unheard = sorted(e for e in events if not declares_event(proto, e)
                     and e not in RIMOSSI_APPOSTA)
    drift, unknown = [], []
    for method in sorted(sent):
        dec = declared(proto, method)
        if dec is None:
            if method not in RIMOSSI_APPOSTA:
                unknown.append(method)
            continue
        missing = sorted(f for f in sent[method] if f not in dec)
        if missing:
            drift.append((method, missing))
        if args.verbose:
            print("  %-38s %s" % (method, ", ".join(sorted(sent[method])) or "(no params)"))

    print("checked %d juggler commands and %d events from %s"
          % (len(sent), len(events), label))
    # Un'esclusione silenziosa e' peggio di un rosso: si dice sempre cosa NON si
    # sta guardando, e perche', cosi' il verdetto verde e' leggibile per intero.
    visti = sorted(n for n in RIMOSSI_APPOSTA
                   if n in sent or n in events)
    if visti:
        print("\nNON contati, perche' rimossi APPOSTA (%d):" % len(visti))
        for n in visti:
            print("  %-28s %s" % (n, RIMOSSI_APPOSTA[n]))
    if unheard:
        print("\nEvents the client waits for that Protocol.js does not declare:")
        for e in unheard:
            print("  %s" % e)
    if unknown:
        print("\nCommands the client sends that Protocol.js does not declare at all:")
        for m in unknown:
            print("  %s" % m)
    if drift:
        print("\nPROTOCOL DRIFT - fields sent but not declared (checkScheme rejects these):")
        for method, fields in drift:
            print("  %-38s %s" % (method, ", ".join(fields)))
        print("\nDeclare them in juggler/protocol/Protocol.js (Optional, and ignored unless")
        print("the handler genuinely needs them) and rebuild, or the client breaks at runtime.")
        return 1
    if unknown or unheard:
        return 1
    print("PROTOCOL DRIFT CHECK PASS - every field the client sends and every event"
          " it waits for is declared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
