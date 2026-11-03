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
    5b. parallelism: 4 by default with GitHub's file unchanged, on the node
        min(CPU, RAM GB / 2);
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
import subprocess
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


def zram_case(run: str, find_ok: bool, hot_add_ok: bool) -> tuple:
    """The swap step's zram block (release.yml, "Free disk + 32G swap") run by
    bash -e, as the runner does, with sudo, modprobe and zramctl faked:
    `zramctl --find` succeeds or exits 1, /sys/class/zram-control/hot_add
    answers 3 or fails. Returns (the shell went on, the step summary)."""
    lines = run.splitlines()
    start = min(i for i, l in enumerate(lines) if "zram_swap() {" in l or "modprobe zram" in l)
    end = next(i for i in range(start, len(lines))
               if lines[i].strip() == "fi" and any("the disk file alone" in l for l in lines[start:i]))
    block = "\n".join(lines[start:end + 1])
    find = ("echo /dev/zram0; exit 0" if find_ok else
            "echo 'zramctl: /dev/zram0: failed to reset: Device or resource busy' >&2; exit 1")
    fake_sudo = "\n".join([
        "#!/bin/bash",
        'case "$1" in',
        '  zramctl) for a; do [ "$a" = --find ] && { %s; }; done; exit 0 ;;' % find,
        "  cat) %s ;;" % ("echo 3" if hot_add_ok else "exit 1"),
        "  *) exit 0 ;;",
        "esac", ""])
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        sudo = tmp / "sudo"
        sudo.write_text(fake_sudo, newline="\n")
        sudo.chmod(0o755)
        summary = tmp / "summary"
        summary.write_text("")
        env = dict(os.environ, PATH="%s%s%s" % (tmp, os.pathsep, os.environ.get("PATH", "")),
                   GITHUB_STEP_SUMMARY=str(summary))
        out = subprocess.run(["bash", "-e", "-c", block + "\necho WENT-ON"], env=env,
                             capture_output=True, text=True)
        return "WENT-ON" in out.stdout, summary.read_text()


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
    plain = mozconfig.compose(BASE, legs.by_name("linux-x86_64"), mozconfig.Plan("off", "", Path("pgo"), fake_pgo))
    wide = mozconfig.compose(BASE, legs.by_name("linux-x86_64"), mozconfig.Plan("off", "", Path("pgo"), fake_pgo, 30))
    cases.append(("parallelism: 4 unless the plan says otherwise, and only that line and its reason change",
                  plain == text and "mk_add_options MOZ_PARALLEL_BUILD=4\n" in plain
                  and mozconfig.active(wide) == [l.replace("BUILD=4", "BUILD=30") for l in mozconfig.active(plain)]
                  and "no swap" in wide and "no swap" not in plain))
    cases.append(("parallelism on the node: min(CPU, RAM GB / 2), measured values 48 CPU and 62.5 GB give 31",
                  mozconfig.node_parallel(48, 67177832448) == 31
                  and mozconfig.node_parallel(24, 67177832448) == 24
                  and mozconfig.node_parallel(48, 1) == 1))
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
        appeared = []
        real_replace = iw.os.replace

        def spy(src, dst):
            appeared.append(Path(dst).exists())
            real_replace(src, dst)
        iw.os.replace = spy
        try:
            iw.watch(prof, log, 0.01, stop=lambda: True)
        finally:
            iw.os.replace = real_replace
        record = log.with_suffix(".start.json")
        cases.append(("input watch: the start record appears whole, by one rename (run 41356589923 read it empty)",
                      appeared == [False] and json.loads(record.read_text(encoding="utf-8"))["sha256"]
                      and not list(tmp.glob("*.partial"))))
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
        again = steps.get("Compile cache - restore again if the first brought nothing", {})
        said = steps.get("Compile cache - what this build starts from", {})
        cases.append(("compile cache: a restore that brought nothing is tried again, and a cold build says so (run 40647970370)",
                      "steps.cache-restore.outputs.cache-matched-key == ''" in again.get("if", "")
                      and again.get("with", {}).get("restore-keys") == "sccache-${{ matrix.leg }}-"
                      and "builds cold" in said.get("run", "") and "GITHUB_STEP_SUMMARY" in said.get("run", "")))
        cases.append(("gtests: an input, off by default, on every leg, as a phase",
                      gt_input["type"] == "boolean" and gt_input["default"] is False and len(gt_step) == 1
                      and gt_step[0]["if"] == "github.event.inputs.gtests == 'true' && runner.environment == 'github-hosted'"
                      and "matrix" not in gt_step[0]["if"] and "phase gtest -- " in gt_step[0]["run"]))
        # A cross-compiled leg's runner cannot execute what it builds (run 41232307511:
        # "Exec format error" on firefox.exe): its gtests are built there and run by a
        # job on the target's OS, which downloads exactly that leg's dist/bin.
        cross = [leg for leg in legs.LEGS if leg.target]
        jobs = doc["jobs"]
        runners = {"x86_64-pc-windows-msvc": "windows-latest"}
        cross_ok = bool(cross)
        for leg in cross:
            ran = [j for j in jobs.values()
                   if j.get("runs-on") == runners.get(leg.target)
                   and any(st.get("with", {}).get("name") == "gtest-bin-%s" % leg.leg for st in j.get("steps", []))
                   and any("-unittest" in st.get("run", "") for st in j.get("steps", []))]
            cross_ok = cross_ok and len(ran) == 1 and ran[0].get("if") == "github.event.inputs.gtests == 'true'"
        kept = steps.get("Keep the cross-compiled gtests for the target's OS", {})
        cross_ok = (cross_ok and "matrix.target" in gt_step[0]["run"] and "recurse_gtest" in gt_step[0]["run"]
                    and kept.get("with", {}).get("name") == "gtest-bin-${{ matrix.leg }}")
        cases.append(("gtests: a cross-compiled leg's run on its target's OS (run 41232307511)", cross_ok))
        # apt has no network timeout: a stalled mirror held run 41248997155's
        # linux-x86_64 leg in `apt-get update` for 5h40, until the job's limit.
        build_steps = doc["jobs"]["build"]["steps"]
        net = [i for i, st in enumerate(build_steps) if "80-ci-network" in st.get("run", "")]
        apt_at = [i for i, st in enumerate(build_steps) if "apt-get" in st.get("run", "")]
        conf = build_steps[net[0]]["run"] if net else ""
        # zram is best-effort: run 41325666566's win-x86_64 leg died in the swap
        # step on "zramctl: /dev/zram0: failed to reset: Device or resource busy".
        # It runs the step's own shell with a fake sudo: Linux only, where the
        # CI runs it (a Windows host's "bash" may be WSL's, without our PATH).
        swap = steps.get("Free disk + 32G swap (Linux runners)", {}).get("run", "")
        if sys.platform.startswith("linux"):
            busy, busy_summary = zram_case(swap, find_ok=False, hot_add_ok=False)
            fresh, fresh_summary = zram_case(swap, find_ok=False, hot_add_ok=True)
            cases.append(("swap: a runner whose zram is taken gets a new device, or goes on with the disk file alone (run 41325666566)",
                          busy and "NO usable zram" in busy_summary
                          and fresh and "zram /dev/zram3" in fresh_summary))
        else:
            print("skip  swap: zram case runs on Linux only")
        # WebGPU on Linux: our lavapipe, built before the gtests and the package
        # and shipped with it (without one requestAdapter() answered null).
        names = [st.get("name", "") for st in build_steps]
        lvp = steps.get("WebGPU's Vulkan driver (linux-x86_64) - build", {})
        lvp_at = names.index(lvp["name"]) if lvp else -1
        key = steps.get("WebGPU's Vulkan driver (linux-x86_64) - its inputs", {})
        kept = [st for st in build_steps if "lavapipe" in st.get("name", "") + str(st.get("with", {}))
                and "cache" in st.get("uses", "")]
        pkg = (HERE / "package_linux.sh").read_text(encoding="utf-8")
        pins = json.loads((HERE / "lavapipe" / "VERSIONS.json").read_text(encoding="utf-8"))
        cases.append(("webgpu: linux-x86_64 builds its lavapipe from pinned sources and patches before the gtests and the package, and ships it",
                      lvp.get("if") == "matrix.leg == 'linux-x86_64'"
                      and "lavapipe_build.py --out" in lvp.get("run", "")
                      and "obj-rel/dist/bin/lavapipe" in lvp.get("run", "")
                      and 0 <= lvp_at < names.index("Package + validate")
                      and lvp_at < min(i for i, n in enumerate(names) if n.startswith("Engine gtests"))
                      and "lavapipe_build.py --key" in key.get("run", "")
                      and len(kept) == 2 and all("compile_cache == 'gha'" in st.get("if", "") for st in kept)
                      and '"$STAGING/lavapipe/"' in pkg and "linux-x86_64" in pkg
                      and len(pins["mesa"]["sha256"]) == 64 and len(pins["llvm"]["commit"]) == 40
                      and len(sorted((HERE / "lavapipe").glob("*.patch"))) >= 4))
        import lavapipe_build
        rtti_off = "-DLLVM_ENABLE_RTTI=OFF" in lavapipe_build.LLVM_CMAKE
        cases.append(("webgpu: LLVM and Mesa agree on RTTI (meson refuses a mismatch, run 41330839671)",
                      rtti_off and "-Dcpp_rtti=false" in lavapipe_build.MESON_OPTIONS
                      or not rtti_off and "-Dcpp_rtti=false" not in lavapipe_build.MESON_OPTIONS
                      and "-DLLVM_ENABLE_RTTI=ON" in lavapipe_build.LLVM_CMAKE))
        good_dyn = (" 0x1 (NEEDED)             Shared library: [libc.so.6]\n"
                    " 0xe (SONAME)             Library soname: [libvulkan.so.1]\n")
        good_sym = ("     1: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND memcpy@GLIBC_2.14 (2)\n"
                    "     2: 0000000000001000    20 FUNC    GLOBAL DEFAULT   12 vkGetInstanceProcAddr\n"
                    "     3: 0000000000001100    20 FUNC    GLOBAL DEFAULT   12 vk_icdGetInstanceProcAddr\n")
        cases.append(("webgpu: the lavapipe check refuses a host library, the wrong SONAME, a missing entry point and a symbol above Firefox's floor",
                      lavapipe_build.check_binary(good_dyn, good_sym) == []
                      and len(lavapipe_build.check_binary(
                          good_dyn.replace("libvulkan.so.1", "libvulkan_lvp.so")
                          + " 0x1 (NEEDED)             Shared library: [libdrm.so.2]\n",
                          good_sym.replace("GLIBC_2.14", "GLIBC_2.27").replace(" vkGetInstanceProcAddr", " other"))) == 4
                      and lavapipe_build.glslang_version("Glslang Version: 11:14.0.0") == (14, 0)))
        tls_sym = ("     5: 0000000000000000     0 FUNC    WEAK   DEFAULT  UND __cxa_thread_atexit_impl@GLIBC_2.18 (5)\n"
                   "     6: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND __libc_dlopen_mode@GLIBC_PRIVATE (6)\n")
        named = lavapipe_build.check_binary(good_dyn, good_sym + tls_sym)
        cases.append(("webgpu: the lavapipe check names the import above Firefox's glibc floor (run 41346336371: GLIBC_2.18)",
                      len(named) == 1 and "GLIBC_2.18 (__cxa_thread_atexit_impl)" in named[0]))
        floor_patch = sorted((HERE / "lavapipe").glob("*-lavapipe-glibc-floor-*.patch"))
        floor_text = floor_patch[0].read_text(encoding="utf-8") if floor_patch else ""
        cases.append(("webgpu: lavapipe meets glibc 2.17 as Firefox does, defining __cxa_thread_atexit_impl in the library (hidden, -z nodelete)",
                      "+  [ 'lavapipe_target.c', 'lvp_glibc_floor.c' ],\n" in floor_text
                      and "-Wl,-z,nodelete" in floor_text
                      and '+__attribute__((visibility("hidden"))) int\n+__cxa_thread_atexit_impl(' in floor_text
                      and "-static-libstdc++" in (HERE / "lavapipe_build.py").read_text(encoding="utf-8")))
        lvp = HERE / "lavapipe"

        def patch_text(glob):
            found = sorted(lvp.glob(glob))
            return found[0].read_text(encoding="utf-8") if found else ""
        layers = patch_text("*-lavapipe-layer-enumeration-without-a-loader.patch")
        cases.append(("webgpu: with no loader lavapipe answers no layers as 0 and VK_SUCCESS (run 41358084281: wgpu gave up the instance on VK_ERROR_LAYER_NOT_PRESENT)",
                      layers.count("+   *pPropertyCount = 0;\n+   return VK_SUCCESS;\n") == 2
                      and layers.count("-   return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);") == 2
                      and "+   return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);" not in layers))
        profile = patch_text("*-cpu-profile-with-f16c-without-fma.patch")
        options = patch_text("0001-*.patch")
        cases.append(("webgpu: lavapipe's CPU profile keeps F16C (shaderFloat16, which WebGPU's compliance needs on llvmpipe; run 41358084281 had no adapter) and drops FMA, AVX2, AVX-512",
                      '"GALLIUM_OVERRIDE_CPU_CAPS"' not in options
                      and all("+   util_cpu_caps.%s = 0;" % cap in profile for cap in ("has_fma", "has_avx2", "has_avx512f"))
                      and "has_f16c = 0" not in profile and "has_avx = 0" not in profile))
        honours = patch_text("*-lavapipe-limits-it-honours.patch")
        cases.append(("webgpu: lavapipe advertises the storage limits it honours, not gallium's legacy 1 << 27 and 32+64 slots (run 41372399866: 8 declared limits above the adapter's)",
                      "+      .maxStorageBufferRange                    = (1u << 31) - 4,\n" in honours
                      and "+      .maxFragmentCombinedOutputResources       = max_render_targets + 2 * MAX_DESCRIPTORS,\n" in honours
                      and "-      .maxStorageBufferRange                    = device->pscreen->caps.max_shader_buffer_size,\n" in honours))
        with tempfile.TemporaryDirectory() as t:
            fake = Path(t)
            (fake / "VERSIONS.json").write_text("{}", encoding="utf-8")
            (fake / "0001-x.patch").write_text("x", encoding="utf-8")
            (fake / "proof").mkdir()
            (fake / "proof" / "proof.c").write_text("a", encoding="utf-8")
            before = lavapipe_build.inputs_key("clang", fake / "VERSIONS.json", fake)
            (fake / "proof" / "proof.c").write_text("b", encoding="utf-8")
            after = lavapipe_build.inputs_key("clang", fake / "VERSIONS.json", fake)
        good = ("A storage buffer binding of 2147483644 bytes: first 0x12345678 last 0xdeadbeef -> OK\n"
                "B fragment shader with 64 storage buffers and 64 storage images: 0 wrong -> OK\n")
        check = getattr(lavapipe_build, "proof_problems", None)
        cases.append(("webgpu: every lavapipe build uses its raised limits for real before keeping the library, and the proof is in the cache key",
                      before != after and check is not None and check(good) == []
                      and len(check(good.replace("0 wrong -> OK", "3 wrong -> WRONG"))) == 1
                      and len(check(good.splitlines()[0])) == 1
                      and "prove(lib," in (HERE / "lavapipe_build.py").read_text(encoding="utf-8")
                      and all((lvp / "proof" / f).is_file() for f in ("proof.c", "big.comp", "many.vert", "many.frag"))))
        install = steps.get("Install Linux build tools", {})
        cases.append(("apt: timeouts and retries before every apt-get, and the install step has a limit (run 41248997155)",
                      len(net) == 1 and bool(apt_at) and net[0] < min(apt_at)
                      and all(k in conf for k in ('Acquire::http::Timeout', 'Acquire::https::Timeout', 'Acquire::Retries'))
                      and 0 < install.get("timeout-minutes", 0) <= 30))
        # The repository holds the patch series only: a job that needs the source makes
        # it with tools/engine.py materialize (the Mozilla tag + the series), and no
        # checkout names a branch of Firefox's tree any more.
        tree_jobs = set()
        for name in ("release.yml", "publish.yml", "republish.yml"):
            for job_name, job in yaml.safe_load((wf / name).read_text(encoding="utf-8"))["jobs"].items():
                if any("engine.py materialize" in s.get("run", "") for s in job.get("steps") or []):
                    tree_jobs.add("%s:%s" % (name, job_name))
        every = "\n".join((wf / n).read_text(encoding="utf-8") for n in ("release.yml", "publish.yml", "republish.yml"))
        cases.append(("source: every job that reads the tree materializes it from the series, none checks out a Firefox branch",
                      tree_jobs == {"release.yml:plan", "release.yml:build", "release.yml:report",
                                    "publish.yml:publish", "republish.yml:plan"}
                      and "SOURCE_REF" not in every and "SOURCE_REPO" not in every and "source_ref" not in every
                      and "series-commit.txt" in every and "--series-commit-file" in every))

    import test_cache_rules
    import test_release_builder
    import test_theta_node
    cases += test_theta_node.cases() + test_cache_rules.cases() + test_release_builder.cases()

    failures = 0
    for name, ok in cases:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
