"""The CI's .mozconfig, composed in layers.

The repository's .mozconfig is ONE file for the two development machines and
the CI. A leg extends it with the layers below, in this order, and each layer
writes the reason it exists into the file as comments, so the final
.mozconfig a build log prints explains itself.

  cache      the compile cache's sccache, by absolute path, or none
  symbols    line tables for C, C++ and Rust (crash dumps -> file:line)
  toolchain  the retail's PGO profile and LTO mode for this leg
  platform   target, host, and the Windows C++ runtime
  machine    parallelism and objdir of the runner

A layer is a function (leg, plan) -> Layer. The order and the active lines
are fixed by scripts/ci/test_ci.py: a flag that changes is a decision, and
it changes the test with it.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List

from legs import Leg


@dataclass
class Plan:
    compile_cache: str            # gha | off (scripts/release_plan.py)
    sccache_path: str = ""
    pgo_dir: Path = Path("pgo")
    # upstream_toolchain.pgo(leg, out) -> lines; injected so the tests run offline
    pgo: Callable = None


@dataclass
class Layer:
    name: str
    why: str
    lines: List[str] = field(default_factory=list)


def cache(leg: Leg, plan: Plan) -> Layer:
    lines = []
    if plan.compile_cache == "gha":
        if not plan.sccache_path:
            raise ValueError("compile_cache=gha needs the pinned sccache's path")
        lines = ["ac_add_options --with-ccache=%s" % plan.sccache_path,
                 "mk_add_options 'export RUSTC_WRAPPER=%s'" % plan.sccache_path]
    return Layer("cache", (
        "The repository's sccache lines name it without a path (the path is the "
        "machine's) and are removed here. With compile_cache=gha the pinned sccache "
        "comes back by ABSOLUTE path: by name, configure takes the one mach "
        "downloads into ~/.mozbuild, another version on two of the three legs. "
        "compile_cache=off (every release) compiles cold for real."), lines)


def symbols(leg: Leg, plan: Plan) -> Layer:
    return Layer("symbols", (
        "Line tables and function names, the least that turns a crash dump into "
        "file:line; the shipped files carry none (Linux strips, Windows keeps the "
        ".pdb in the symbols artifact). The value reaches C and C++ only: with any "
        "debug symbols rust.configure asks rustc for FULL debuginfo, and the first "
        "run with symbols lost all three legs out of memory (run 40437686466). "
        "rust.mk puts the environment's RUSTFLAGS after its defaults, so line "
        "tables win for Rust too."), [
        "ac_add_options --enable-debug-symbols=-gline-tables-only",
        "mk_add_options 'export RUSTFLAGS=-Cdebuginfo=line-tables-only'"])


def toolchain(leg: Leg, plan: Plan) -> Layer:
    lines = list(plan.pgo(leg.leg, plan.pgo_dir))
    lines += ["export CFLAGS=-Wno-backend-plugin", "export CXXFLAGS=-Wno-backend-plugin"]
    return Layer("toolchain", (
        "PGO and LTO as the retail build of this tree: Mozilla's own profile, "
        "verified against its chain of trust, and the LTO mode its CI uses on this "
        "platform (scripts/upstream_toolchain.py). The profile was recorded on "
        "Mozilla's source, so every function we changed reports 'function control "
        "flow change detected (hash mismatch)': harmless, and tens of thousands "
        "of lines on Windows, whose log stream stopped under them (run "
        "40446702889). -Wno-backend-plugin silences them; the rest stays."), lines)


def platform(leg: Leg, plan: Plan) -> Layer:
    lines = []
    if leg.target:
        lines.append("ac_add_options --target=%s" % leg.target)
    if leg.host:
        lines.append("ac_add_options --host=%s" % leg.host)
    if leg.family == "win":
        lines.append("ac_add_options --with-redist")
    return Layer("platform", (
        "The target, and the host the runner's compiler reports under a name rustc "
        "cannot translate (scripts/ci/legs.py). Windows ships the C++ runtime "
        "beside the binaries as the retail does (B306): without --with-redist a "
        "clean Windows could not start the browser. No "
        "--disable-default-browser-agent: the retail builds the agent, and adding "
        "it back here removed its page-readable image (B157)."), lines)


def machine(leg: Leg, plan: Plan) -> Layer:
    return Layer("machine", (
        "The runner's 4 cores, and the objdir every later step reads."), [
        "mk_add_options MOZ_PARALLEL_BUILD=4",
        "mk_add_options MOZ_OBJDIR=@TOPSRCDIR@/obj-rel"])


LAYERS = (cache, symbols, toolchain, platform, machine)

_SCCACHE_ACTIVE = re.compile(r"^[^#]*sccache", re.I)


def compose(base: str, leg: Leg, plan: Plan) -> str:
    """The repository's .mozconfig with its active sccache lines removed, then
    every layer with its reason."""
    kept = [l for l in base.splitlines() if not _SCCACHE_ACTIVE.match(l)]
    out = kept + ["", "# --- release CI layers for %s (mold intentionally OFF - it segfaults libxul) ---" % leg.leg]
    for make in LAYERS:
        layer = make(leg, plan)
        out.append("# [%s] %s" % (layer.name, layer.why))
        out.extend(layer.lines)
    return "\n".join(out) + "\n"


def active(text: str) -> List[str]:
    return [l for l in text.splitlines() if l.strip() and not l.lstrip().startswith("#")]
