"""The engine is compiled the way the retail of the same version is, where it shows.

    python3 scripts/toolchain_parity.py <staged tree>

Every Firefox writes how it was built into about:buildconfig
(chrome/toolkit/content/global/buildconfig.html in omni.ja): the C and C++
compilers with their versions and flags, rustc with its version, and the
configure options. This gate reads that page from the staged tree and from the
retail of the same version and platform (Mozilla's complete MAR, the judge
stock_parity.py already uses) and compares what changes the machine code:

  - the C, C++ and Rust compiler versions;
  - the C and C++ optimisation flags (-O*);
  - the configure options that change code generation (CODEGEN_OPTIONS).

A page cannot read about:buildconfig (stock_parity.py measured it), so this is
not about the page surface: it is about the code a page RUNS. A different
compiler, a missing PGO or LTO, Rust without SIMD, all leave every value a page
reads as it was and change how fast each operation runs, and not uniformly.

WHY IT EXISTS. On 2026-10-27 the 157 candidates turned out to be compiled with
rustc 1.99.0 while the retail used 1.95.0: --enable-bootstrap fetched clang
from Mozilla but took Rust from whatever "stable" the CI runner had installed,
so the version would also have moved by itself from one month to the next.
Nothing checked it. A difference that stays is declared in
toolchain_parity_exceptions.json with why; a declaration that no longer
matches a difference fails the gate too.
"""
from __future__ import annotations

import html
import json
import re
import sys
from pathlib import Path

import stock_parity

BUILDCONFIG = "chrome/toolkit/content/global/buildconfig.html"
EXCEPTIONS = Path(__file__).resolve().parent / "toolchain_parity_exceptions.json"

#: Configure options that change the code the compiler emits. The value is
#: what a difference in it changes, for the message.
CODEGEN_OPTIONS = {
    "--enable-profile-use": "profile-guided optimisation (PGO)",
    "--with-pgo-jarlog": "PGO ordering of omni.ja",
    "--enable-rust-simd": "SIMD in Rust code",
    "--enable-frame-pointers": "frame pointers",
    "--disable-optimize": "optimisation off",
    "--enable-optimize": "optimisation level",
}


class ToolchainError(Exception):
    pass


def parse(page: str) -> dict:
    """{'tools': [(name, version, flags)], 'options': {name: value}} of a
    buildconfig.html."""
    rows = re.findall(r"<tr>\s*<td>(.*?)</td>\s*<td>(.*?)</td>\s*<td>(.*?)</td>\s*</tr>", page, re.S)
    tools = [tuple(html.unescape(c).strip() for c in row) for row in rows]
    m = re.search(r"<h2>Configure options</h2>\s*<p>(.*?)</p>", page, re.S)
    target = re.search(r"<th>target</th>\s*</tr>\s*<tr>\s*<td>(.*?)</td>", page, re.S)
    if not tools or not m:
        raise ToolchainError("buildconfig.html has no build tools table or no configure options")
    options = {}
    for token in re.findall(r"'[^']*'|\S+", html.unescape(m.group(1))):
        token = token.strip("'")
        name, _, value = token.partition("=")
        options[name.strip()] = value
    return {"tools": tools, "options": options,
            "target": html.unescape(target.group(1)).strip() if target else ""}


def lto_mode(config: dict) -> str | None:
    """The LTO the build got, as a sorted set of modes, or None.

    One option with two spellings: --enable-lto=X, or MOZ_LTO=X in the
    environment (lto-pgo.configure, option env="MOZ_LTO"); the retail uses the
    second, our CI the first. And one rule configure applies on top: with no
    "full" or "thin" given, it picks FULL when PGO and MOZ_AUTOMATION are on
    and the target is x86_64 Windows or Linux, THIN otherwise. The retail's
    page says MOZ_LTO=cross and the build was full on x86_64; ours asks for
    full,cross explicitly because we do not set MOZ_AUTOMATION."""
    opts = config["options"]
    value = opts.get("--enable-lto", opts.get("MOZ_LTO"))
    if value is None:
        return None
    modes = set(filter(None, value.split(","))) or {"cross"}
    if not modes & {"full", "thin"}:
        x86_64 = config.get("target", "").startswith("x86_64-")
        full = x86_64 and "MOZ_AUTOMATION" in opts and "--enable-profile-use" in opts
        modes.add("full" if full else "thin")
    return ",".join(sorted(modes))


def facts(config: dict) -> dict:
    """{key: value} of what this gate compares."""
    out = {}
    # The template (toolkit/content/buildconfig.html) lists them in this order:
    # @CC@, @CXX@, @RUSTC@. A compiler's name says nothing reliable (clang-cl
    # is the C++ compiler too), the order does.
    if len(config["tools"]) != 3 or not config["tools"][2][0].endswith("rustc"):
        raise ToolchainError("buildconfig.html lists %d compilers, not C, C++ and rustc"
                             % len(config["tools"]))
    for tool, (name, version, flags) in zip(("c", "c++", "rustc"), config["tools"]):
        out["%s version" % tool] = version
        if tool != "rustc":
            out["%s optimisation" % tool] = " ".join(f for f in flags.split() if f.startswith("-O"))
    lto = lto_mode(config)
    if lto is not None:
        out["lto"] = lto
    for option in CODEGEN_OPTIONS:
        if option in config["options"]:
            value = config["options"][option]
            # A path names the machine it was built on, not a choice.
            out["option %s" % option] = "set" if not value or "/" in value else value
    return out


def differences(ours: dict, retail: dict) -> dict:
    out = {}
    for key in sorted(set(ours) | set(retail)):
        if ours.get(key) != retail.get(key):
            out[key] = "retail [%s], this build [%s]" % (retail.get(key, "absent"), ours.get(key, "absent"))
    return out


def load_exceptions(platform: str, path: Path = EXCEPTIONS) -> dict:
    out = {}
    for entry in json.loads(path.read_text("utf-8"))["exceptions"]:
        missing = {"key", "platforms", "why"} - set(entry)
        if missing:
            raise ToolchainError("exception without %s: %s" % (sorted(missing), entry))
        if platform in entry["platforms"]:
            out[entry["key"]] = entry
    return out


def judge(tree: Path, retail=stock_parity.retail_files, exceptions: Path = EXCEPTIONS) -> tuple:
    """(platform, version, undeclared differences, stale declarations)."""
    platform, version = stock_parity.platform_of(tree), stock_parity.version_of(tree)
    ours = stock_parity.Files.from_directory(tree)
    stock = retail(version, platform)
    for files, who in ((ours, "this build"), (stock, "the retail")):
        if not files.has(BUILDCONFIG):
            raise ToolchainError("%s has no %s" % (who, BUILDCONFIG))
    diff = differences(facts(parse(ours.read(BUILDCONFIG).decode("utf-8"))),
                       facts(parse(stock.read(BUILDCONFIG).decode("utf-8"))))
    declared = load_exceptions(platform, exceptions)
    undeclared = {k: v for k, v in diff.items() if k not in declared}
    stale = sorted(k for k in declared if k not in diff)
    return platform, version, undeclared, stale


def main(argv) -> int:
    if len(argv) != 2:
        print("usage: toolchain_parity.py <staged tree>")
        return 2
    platform, version, undeclared, stale = judge(Path(argv[1]))
    for key, what in undeclared.items():
        print("  differs from retail %s %s: %s: %s" % (version, platform, key, what))
    for key in stale:
        print("  declared but no longer different: %s" % key)
    return 1 if undeclared or stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
