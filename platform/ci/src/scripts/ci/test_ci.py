#!/usr/bin/env python3
"""scripts/ci against the cases it exists for.

  legs
    1. the asset names are the ones the release has always published
       (firefox-157.0.1-stealth-<os>-<arch>.<ext>), derived, not written;
    2. the matrix carries every field release.yml reads;
  mozconfig
    3. the flags decided on 2026-10-27, leg by leg, in order: a flag that
       changes is a decision and changes this test;
    4. the repository's sccache lines go, case-insensitive (SCCACHE_CACHE_SIZE
       too), its comments stay;
    5. compile_cache=gha without the pinned sccache's path is refused;
    6. every layer writes its reason into the file;
  phase
    7. a failing command fails the phase with its exit code, and the summary
       carries its first error lines and the memory watch's kill;
    8. a passing one records its time;
  report
    9. one row per phase; a leg that never started and a phase that never
       ended (cancelled, runner lost) are rows, not gaps;
  symbols_match
   10. a xul whose symbols describe another binary is refused (run
       40593659072), and a package with no xul to check never passes empty;
  input_watch
   11. a build input that moves is logged with who was running, and one whose
       content changed is refused.

    python3 scripts/ci/test_ci.py
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import legs  # noqa: E402
import mozconfig  # noqa: E402
import phase  # noqa: E402
import report  # noqa: E402

BASE = """# the development machines' file
ac_add_options --enable-release
ac_add_options --with-ccache=sccache
mk_add_options 'export RUSTC_WRAPPER=sccache'
mk_add_options 'export SCCACHE_CACHE_SIZE=50G'
# sccache is in PATH on both machines
ac_add_options --disable-debug-symbols
"""


def fake_pgo(leg, out):
    lto = {"win-x86_64": "full,cross", "linux-x86_64": "full,cross", "linux-arm64": "cross"}[leg]
    return ["ac_add_options --enable-profile-use=cross",
            "ac_add_options --with-pgo-profile-path=%s/merged.profdata" % out,
            "ac_add_options --with-pgo-jarlog=%s/en-US.log" % out,
            "ac_add_options --enable-lto=%s" % lto]


def expected(leg, cache):
    lines = ["ac_add_options --enable-release", "ac_add_options --disable-debug-symbols"]
    if cache == "gha":
        lines += ["ac_add_options --with-ccache=/s/sccache", "mk_add_options 'export RUSTC_WRAPPER=/s/sccache'"]
    lines += ["ac_add_options --enable-debug-symbols=-gline-tables-only",
              "mk_add_options 'export RUSTFLAGS=-Cdebuginfo=line-tables-only'"]
    lines += fake_pgo(leg, "pgo")
    lines += ["export CFLAGS=-Wno-backend-plugin", "export CXXFLAGS=-Wno-backend-plugin"]
    lines += {"linux-x86_64": ["ac_add_options --host=x86_64-unknown-linux-gnu"],
              "linux-arm64": [],
              "win-x86_64": ["ac_add_options --target=x86_64-pc-windows-msvc",
                             "ac_add_options --host=x86_64-unknown-linux-gnu",
                             "ac_add_options --with-redist"]}[leg]
    lines += ["mk_add_options MOZ_PARALLEL_BUILD=4", "mk_add_options MOZ_OBJDIR=@TOPSRCDIR@/obj-rel"]
    return lines


def compose(leg, cache, sccache="/s/sccache"):
    plan = mozconfig.Plan(cache, sccache, Path("pgo"), fake_pgo)
    return mozconfig.compose(BASE, legs.by_name(leg), plan)


def with_env(tmp, **env):
    old = {k: os.environ.get(k) for k in env}
    os.environ.update(env)
    return old


def restore(old):
    for k, v in old.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v


def phase_case(tmp: Path, argv, memwatch=""):
    (tmp / "memwatch.log").write_text(memwatch, encoding="utf-8")
    phase.EVENTS = tmp / "events.jsonl"
    phase.LOGS = tmp / "logs"
    old = with_env(tmp, GITHUB_STEP_SUMMARY=str(tmp / "summary.md"), CI_LEG="win-x86_64",
                   CI_MEMWATCH=str(tmp / "memwatch.log"))
    try:
        code = phase.run("build", argv)
    finally:
        restore(old)
    events = [json.loads(l) for l in (tmp / "events.jsonl").read_text(encoding="utf-8").splitlines()]
    return code, events, (tmp / "summary.md").read_text(encoding="utf-8")


def main() -> int:
    cases = []
    names = {l.leg: l.asset("157.0.1") for l in legs.LEGS}
    cases.append(("asset names as the release has always published them", names == {
        "linux-x86_64": "firefox-157.0.1-stealth-linux-x86_64.tar.gz",
        "linux-arm64": "firefox-157.0.1-stealth-linux-arm64.tar.gz",
        "win-x86_64": "firefox-157.0.1-stealth-win-x86_64.zip"}))
    inc = json.loads(legs.matrix("157.0.1"))["include"]
    need = {"leg", "runner", "family", "host", "target", "rust_target", "extra_pkgs", "asset"}
    cases.append(("the matrix carries every field release.yml reads",
                  len(inc) == 3 and all(need <= set(e) for e in inc)))

    for leg in ("linux-x86_64", "linux-arm64", "win-x86_64"):
        for cache in ("gha", "off"):
            got = mozconfig.active(compose(leg, cache))
            cases.append(("the decided flags, %s, compile_cache=%s" % (leg, cache), got == expected(leg, cache)))
    text = compose("linux-x86_64", "off")
    cases.append(("sccache lines go case-insensitive, comments stay",
                  "SCCACHE_CACHE_SIZE" not in text and "# sccache is in PATH on both machines" in text))

    def gha_without_path():
        try:
            compose("linux-x86_64", "gha", sccache="")
        except ValueError:
            return True
        return False
    cases.append(("compile_cache=gha without the sccache path is refused", gha_without_path()))
    cases.append(("every layer writes its reason",
                  all(("# [%s] " % f.__name__) in text for f in mozconfig.LAYERS)))

    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        code, events, summ = phase_case(tmp, [sys.executable, "-c",
            "print('compiling'); print('warning: unused'); print('121:36.08 E clang++: error: linker command failed'); raise SystemExit(3)"],
            memwatch="14:10:15 [mem] KILLED pid 1 (14086 MB) to keep the runner alive: ld.lld\n")
        cases.append(("a failing command fails the phase with its exit code", code == 3 and events[-1]["exit"] == 3))
        cases.append(("the summary names the first error and the kill, not the warning",
                      "E clang++: error: linker command failed" in summ and "KILLED" in summ and "unused" not in summ))
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        code, events, summ = phase_case(tmp, [sys.executable, "-c", "print('ok')"])
        cases.append(("a passing command records its time",
                      code == 0 and events[0]["event"] == "start" and events[-1]["exit"] == 0 and "✅" in summ))

    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        ok = root / "events-linux-x86_64"
        ok.mkdir()
        (ok / "events.jsonl").write_text("\n".join(json.dumps(e) for e in [
            {"phase": "build", "event": "start"}, {"phase": "build", "event": "end", "exit": 0, "seconds": 5400},
            {"phase": "package", "event": "start"}]) + "\n", encoding="utf-8")
        (ok / "memwatch.log").write_text("[mem] available 1000/16000 MB, swap used 4000/24000 MB, disk free 40 GB\n", encoding="utf-8")
        (ok / "sccache-stats.txt").write_text("Cache hits rate                 87.50 %\n", encoding="utf-8")
        bad = root / "events-win-x86_64"
        bad.mkdir()
        (bad / "events.jsonl").write_text(json.dumps({"phase": "build", "event": "start"}) + "\n" + json.dumps(
            {"phase": "build", "event": "end", "exit": 2, "seconds": 60, "killed": ["[mem] KILLED pid 9 ld.lld"]}) + "\n", encoding="utf-8")
        got = report.rows(root)
        table = {(r[0], r[1]): r for r in got}
        cases.append(("a passing phase: time, memory peak, cache hits",
                      table[("linux-x86_64", "build")][2:] == ("✅", "1h30m", "18.6 GB", "87.50 %")))
        cases.append(("a phase that never ended is a row",
                      table[("linux-x86_64", "package")][2].startswith("⏹️")))
        cases.append(("a failed phase names why", "KILLED" in table[("win-x86_64", "build")][2]))
        cases.append(("a leg that never started is a row", table[("linux-arm64", "-")][2] == "⚪ never started"))

    import re
    literal = re.compile(r"firefox-\d+(\.\d+)*-stealth-")
    wf = HERE.parent.parent / ".github" / "workflows"
    written = sorted("%s:%d" % (p.name, i + 1) for p in wf.glob("*.yml")
                     for i, l in enumerate(p.read_text(encoding="utf-8").splitlines()) if literal.search(l))
    cases.append(("no workflow writes a versioned asset name (%s)" % (", ".join(written) or "none"), not written))

    # verify-hidden-desktop.yml was a copy of the gate's guards with another
    # setup (the published core, no seal) and said it was the same command.
    copies = sorted(p.name for p in wf.glob("*.yml")
                    if "tests/test_hidden_desktop.py" in p.read_text(encoding="utf-8") and p.name != "gate.yml")
    cases.append(("the gate's guards run in gate.yml only (copies: %s)" % (", ".join(copies) or "none"), not copies))

    import payload_guard as pg

    def guard(view, download=(0, "", ""), published=b"SEAL", ours=b"SEAL"):
        with tempfile.TemporaryDirectory() as t:
            seal = Path(t) / "seal.json"
            seal.write_bytes(ours)

            def gh(args):
                if args[:2] == ["release", "view"]:
                    return view
                if download[0] == 0:
                    out = Path(args[args.index("--dir") + 1])
                    (out / "seal.json").write_bytes(published)
                return download
            try:
                return pg.check("firefox-60", "o/r", seal, gh)
            except pg.GuardError as exc:
                return "REFUSED " + str(exc)
    with_seal = (0, json.dumps({"assets": [{"name": "seal.json"}]}), "")
    cases.append(("payload guard: no release yet proceeds",
                  "first publish" in guard((1, "", "release not found"))))
    cases.append(("payload guard: a network error is refused, not read as 'no seal yet'",
                  guard((1, "", "HTTP 502: Bad Gateway")).startswith("REFUSED")))
    cases.append(("payload guard: a release without a seal proceeds",
                  "first seal" in guard((0, json.dumps({"assets": [{"name": "x.zip"}]}), ""))))
    cases.append(("payload guard: an identical seal proceeds", "identical" in guard(with_seal)))
    cases.append(("payload guard: a different seal is refused",
                  "DIFFERENT" in guard(with_seal, published=b"OTHER")))
    cases.append(("payload guard: a seal that cannot be downloaded is refused",
                  guard(with_seal, download=(1, "", "HTTP 500")).startswith("REFUSED")))

    import struct
    import symbols_match as sm

    def pe(guid_hex, age):
        # MZ -> PE32+ with one section holding the debug directory and its RSDS.
        g = bytes.fromhex(guid_hex)
        raw16 = struct.pack("<IHH", *struct.unpack(">IHH", g[:8])) + g[8:]
        data = bytearray(0x600)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3C, 0x40)
        data[0x40:0x44] = b"PE\0\0"
        struct.pack_into("<HH", data, 0x44, 0x8664, 1)
        struct.pack_into("<H", data, 0x54, 240)
        struct.pack_into("<H", data, 0x58, 0x20B)
        struct.pack_into("<II", data, 0x58 + 112 + 6 * 8, 0x1000, 28)
        struct.pack_into("<IIII", data, 0x58 + 240 + 8, 0x200, 0x1000, 0x200, 0x400)
        struct.pack_into("<IIII", data, 0x400 + 12, 2, 24, 0, 0x500)
        data[0x500:0x504] = b"RSDS"
        data[0x504:0x514] = raw16
        struct.pack_into("<I", data, 0x514, age)
        return bytes(data)

    def elf(build_id):
        data = bytearray(0x200)
        data[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<Q", data, 0x28, 0x100)
        struct.pack_into("<HH", data, 0x3A, 64, 2)
        note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
        struct.pack_into("<IIQQQQ", data, 0x100 + 64, 0, 7, 0, 0, 0x180, len(note))
        data[0x180:0x180 + len(note)] = note
        return bytes(data)

    shipped, described = "030D79D80656024F4C4C44205044422E", "3E2BB50004249B654C4C44205044422E"
    cases.append(("symbols: a PE's id is its CodeView GUID and age",
                  sm.pe_debug_id(pe(shipped, 1)) == shipped + "1"))
    cases.append(("symbols: an ELF's id is its build id in breakpad's byte order",
                  sm.elf_debug_id(elf(bytes(range(1, 21)))) == "040302010605080709" + "0A0B0C0D0E0F10" + "0"))

    def symbols(binaries, ids):
        try:
            return sm.check(binaries, ids)
        except sm.MismatchError as exc:
            return "REFUSED " + str(exc)
    # Run 40593659072: the package's xul.dll and the archive's xul.pdb.
    got = symbols({"xul.dll": pe(shipped, 1)}, {"xul.pdb": {described + "1"}})
    cases.append(("symbols: run 40593659072's xul (shipped 030D79D8, described 3E2BB500) is refused",
                  got.startswith("REFUSED") and shipped in got and described in got))
    cases.append(("symbols: a xul the archive describes passes",
                  symbols({"xul.dll": pe(shipped, 1), "firefox.exe": pe(described, 2)},
                          {"xul.pdb": {shipped + "1"}}) == ["xul.dll %s1" % shipped]))
    cases.append(("symbols: a package with no xul to check is refused, never passed empty",
                  symbols({"firefox.exe": pe(shipped, 1)}, {"firefox.pdb": {shipped + "1"}}).startswith("REFUSED")))
    cases.append(("symbols: an ELF xul is checked by its own name",
                  symbols({"libxul.so": elf(bytes(range(1, 21)))},
                          {"libxul.so": {"0403020106050807090A0B0C0D0E0F100"}})[0].startswith("libxul.so")))

    import threading
    import input_watch as iw
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        prof, log = tmp / "merged.profdata", tmp / "inputwatch.log"
        prof.write_bytes(b"PROFILE")
        th = threading.Thread(target=iw.watch, args=(prof, log, 0.05))
        th.start()
        while not log.with_suffix(".start.json").exists():
            time.sleep(0.01)
        cases.append(("input watch: an untouched profile verifies", "unchanged" in iw.verify(prof, log)
                      and "mtime moved" not in iw.verify(prof, log)))
        st = prof.stat()
        os.utime(prof, ns=(st.st_atime_ns, st.st_mtime_ns + 10 ** 9))
        th.join(60)
        cases.append(("input watch: the first touch is logged with the processes then running",
                      not th.is_alive() and "changed: mtime" in log.read_text(encoding="utf-8")))
        cases.append(("input watch: a touched but identical profile verifies, saying so",
                      "mtime moved" in iw.verify(prof, log)))
        try:
            iw.verify(prof, log, strict=True)
            refused = False
        except ValueError as exc:
            refused = "new mtime" in str(exc)
        cases.append(("input watch: strict, a rewritten profile is refused (what sccache 0.18.0 did)", refused))
        prof.write_bytes(b"ANOTHER")
        try:
            iw.verify(prof, log)
            refused = False
        except ValueError as exc:
            refused = "CONTENT" in str(exc)
        cases.append(("input watch: a profile whose content changed is refused", refused))
        try:
            iw.verify(prof, tmp / "never.log")
            refused = False
        except ValueError:
            refused = True
        cases.append(("input watch: a watch that never started is refused, not passed", refused))

    import subprocess
    import sccache_build as sb

    def fake_build(tmp, check_fails=False, cached=False, patches=("0002-b.patch", "0001-a.patch")):
        pdir, out, calls = tmp / "patches", tmp / "bin", []
        pdir.mkdir()
        for name in patches:
            (pdir / name).write_text("diff\n", encoding="utf-8")
        if cached:
            out.mkdir()
            (out / sb.binary_name()).write_bytes(b"BIN")

        def run(cmd, check=True, env=None):
            calls.append(cmd)
            if check_fails and "--check" in cmd:
                raise subprocess.CalledProcessError(1, cmd)
            if cmd[0] == "cargo":
                release = Path(cmd[cmd.index("--manifest-path") + 1]).parent / "target" / "release"
                release.mkdir(parents=True)
                (release / sb.binary_name()).write_bytes(b"BIN")
            return subprocess.CompletedProcess(cmd, 0)
        try:
            exe = sb.build("v0.18.0", pdir, out, run)
        except (subprocess.CalledProcessError, ValueError) as exc:
            exe = exc
        return exe, calls
    with tempfile.TemporaryDirectory() as t:
        exe, calls = fake_build(Path(t), cached=True)
        cases.append(("sccache: a binary already built for these patches is used as it is",
                      isinstance(exe, Path) and calls == []))
    with tempfile.TemporaryDirectory() as t:
        exe, calls = fake_build(Path(t))
        applied = [Path(c[-1]).name for c in calls if c[:1] == ["git"] and "apply" in c and "--check" not in c]
        cases.append(("sccache: cloned at the pinned tag with LF kept, every patch checked then applied in order, built --locked",
                      isinstance(exe, Path) and exe.is_file()
                      and calls[0][:3] == ["git", "-c", "core.autocrlf=false"] and "v0.18.0" in calls[0]
                      and applied == ["0001-a.patch", "0002-b.patch"]
                      and calls[-1][:2] == ["cargo", "build"] and "--locked" in calls[-1]))
    with tempfile.TemporaryDirectory() as t:
        exe, calls = fake_build(Path(t), check_fails=True)
        cases.append(("sccache: a patch that no longer applies is a refusal, never a binary without it",
                      isinstance(exe, subprocess.CalledProcessError) and not any(c[0] == "cargo" for c in calls)
                      and not (Path(t) / "bin" / sb.binary_name()).exists()))
    with tempfile.TemporaryDirectory() as t:
        exe, calls = fake_build(Path(t), patches=())
        cases.append(("sccache: no patches is refused (that is the upstream release)", isinstance(exe, ValueError)))
    patch = (HERE / "sccache" / "0001-rust-profile-use-is-an-input.patch").read_text(encoding="utf-8")
    cases.append(("sccache: the patch takes -C profile-use off a Rust compile's outputs",
                  "-        if let Some(profile) = &self.parsed_args.profile {" in patch
                  and '-                    ("profile-use", Some(v)) => profile = Some(v.clone()),' in patch))
    rel = (wf / "release.yml").read_text(encoding="utf-8")
    pinned = re.search(r'SCCACHE_PINNED_VERSION=(\S+)"', rel).group(1)
    cases.append(("sccache: built, never downloaded, at the pinned version, keyed by its patches",
                  "sccache-action" not in rel and "sccache-build --tag v%s " % pinned in rel
                  and "hashFiles('scripts/ci/sccache/*.patch')" in rel and "-v%s-" % pinned in rel))
    cases.append(("input watch: the build verifies the profile strictly", "--verify --strict" in rel))
    try:
        import yaml
    except ImportError:
        yaml = None
    if yaml is None:
        cases.append(("gtests: the workflow can be read (PyYAML is missing)", False))
    else:
        doc = yaml.safe_load(rel)
        gt_input = doc[True]["workflow_dispatch"]["inputs"]["gtests"]
        gt_step = [s for s in doc["jobs"]["build"]["steps"] if s.get("name", "").startswith("Engine gtests")]
        steps = {s.get("name", ""): s for s in doc["jobs"]["build"]["steps"]}
        again = steps.get("Compile cache (candidates only) - restore again if the first brought nothing", {})
        said = steps.get("Compile cache (candidates only) - what this build starts from", {})
        cases.append(("compile cache: a restore that brought nothing is tried again, and a cold build says so (run 40647970370)",
                      "steps.cache-restore.outputs.cache-matched-key == ''" in again.get("if", "")
                      and again.get("with", {}).get("restore-keys") == "sccache-${{ matrix.leg }}-"
                      and "builds cold" in said.get("run", "") and "GITHUB_STEP_SUMMARY" in said.get("run", "")))
        cases.append(("gtests: an input, off by default, on every leg, as a phase",
                      gt_input["type"] == "boolean" and gt_input["default"] is False and len(gt_step) == 1
                      and gt_step[0]["if"] == "github.event.inputs.gtests == 'true'"
                      and "matrix" not in gt_step[0]["if"] and "phase gtest -- " in gt_step[0]["run"]))

    failures = 0
    for name, ok in cases:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
