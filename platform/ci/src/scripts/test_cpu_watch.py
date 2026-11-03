#!/usr/bin/env python3
"""cpu_watch.py on a fake /proc and on fixed samples.

  1. our use from the cgroup's cpu.stat, the machine's from /proc/stat;
  2. an unreadable cgroup falls back to the process tree, and says so;
  3. the top process comes with its threads, a linker with its output's name;
  4. the phases come from markers: phase.py's events, mach's and cargo's lines (as a real
     run printed them), the linker's process; nothing from timings;
  5. mean and peak of each phase; a phase with no sample has no row;
  6. warnings: a long phase under the share outside the link, the machine busy with
     what is not ours; none for a short phase, a link, or a healthy one;
  7. a sampler error costs a sample, never the watch: memory_watch.py keeps going;
  8. the report never fails, with no files at all.

Run it directly: `python3 scripts/test_cpu_watch.py`.
"""
from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cpu_watch as c  # noqa: E402
import memory_watch as w  # noqa: E402


def stat_line(user, idle, iowait=0, steal=0, cpus=4, running=3):
    return ("cpu  %d 0 0 %d %d 0 0 %d 0 0\n" % (user, idle, iowait, steal)
            + "".join("cpu%d 0 0 0 0 0 0 0 0 0 0\n" % n for n in range(cpus))
            + "intr 1\nprocs_running %d\nprocs_blocked 0\n" % running)


def proc_entry(root: Path, pid: int, comm: str, utime: int, stime: int, threads: int, cutime=0, cstime=0, cmd=""):
    d = root / str(pid)
    d.mkdir(exist_ok=True)
    (d / "stat").write_text("%d (%s) S 1 1 1 0 -1 0 0 0 0 0 %d %d %d %d 20 0 %d 0 1 1 1"
                            % (pid, comm, utime, stime, cutime, cstime, threads))
    (d / "cmdline").write_bytes(cmd.replace(" ", "\0").encode())


def sample(t, ours, busy=None, iowait=0.0, steal=0.0, total=48, avail=48, link=()):
    return {"t": t, "ours": ours, "src": "cgroup", "busy": ours if busy is None else busy, "iowait": iowait,
            "steal": steal, "total": total, "avail": avail, "run": 1, "link": list(link)}


def events(*rows):
    return [dict(zip(("event", "phase", "t"), r[:3]), **(r[3] if len(r) > 3 else {})) for r in rows]


T0 = c._epoch("2026-11-03T10:00:00Z")


def at(seconds):
    return __import__("time").strftime("%Y-%m-%dT%H:%M:%SZ", __import__("time").gmtime(T0 + seconds))


def cases() -> list:
    out = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        proc, cg = root / "proc", root / "cg"
        proc.mkdir()
        cg.mkdir()
        now = [T0]
        (proc / "stat").write_text(stat_line(0, 0))
        (cg / "cpu.stat").write_text("usage_usec 1000000\n")
        proc_entry(proc, 10, "cc1plus", 0, 0, 1)
        proc_entry(proc, 20, "ld.lld", 0, 0, 8, cmd="/x/ld.lld -o ../../dist/bin/libxul.so -shared")
        sampler = c.Sampler(root / "s.jsonl", available=4, proc=proc, cgroup=cg, clock=lambda: now[0])
        first = sampler.tick()
        # 60 s later on 4 CPUs: 240 s of ticks at 100 Hz, 180 of them busy, 30 s iowait, 30 s idle.
        now[0] += 60
        (proc / "stat").write_text(stat_line(15000, 3000, iowait=3000, steal=3000, running=7))
        (cg / "cpu.stat").write_text("usage_usec 121000000\n")
        proc_entry(proc, 10, "cc1plus", 3000, 0, 1)
        proc_entry(proc, 20, "ld.lld", 9000, 0, 8, cmd="/x/ld.lld -o ../../dist/bin/libxul.so -shared")
        line = sampler.tick()
        got = c.read_jsonl(root / "s.jsonl")
        out.append(("the first look only remembers", first is None and len(got) == 1))
        s = got[0]
        out.append(("our use from cpu.stat: 120 s in 60 s is 2 cores", s["ours"] == 2.0 and s["src"] == "cgroup"))
        out.append(("the machine: 4 CPUs, 3 busy with steal among them, 0.5 iowait, 0.5 steal",
                    s["total"] == 4 and s["busy"] == 3.0 and s["iowait"] == 0.5 and s["steal"] == 0.5 and s["run"] == 7))
        out.append(("the top process, its cores and its threads", s["top"] == ["ld.lld", 1.5, 8]))
        out.append(("a linker has the name of what it writes, by pid", s["link"] == [[20, "libxul.so"]]))
        out.append(("the log line says all of it", line is not None and all(
            part in line for part in ("[cpu] ours 2.0 of 4 cores (cgroup)", "machine busy 3.0", "iowait 0.5", "steal 0.5",
                                      "runnable 7", "top ld.lld 1.5 cores 8 threads"))))
        out.append(("not due yet: no second sample", sampler.tick() is None and len(c.read_jsonl(root / "s.jsonl")) == 1))

        # The same second interval with the cgroup unreadable: the process tree, which
        # counts a reaped child in its parent, and a machine's ticks as its clock.
        (cg / "cpu.stat").unlink()
        now[0] += 60
        (proc / "stat").write_text(stat_line(36000, 6000, iowait=3000, steal=3000))
        proc_entry(proc, 10, "cc1plus", 6000, 0, 1, cutime=6000)
        proc_entry(proc, 20, "ld.lld", 9000, 0, 8, cmd="/x/ld.lld -o ../../dist/bin/libxul.so -shared")
        sampler.tick()
        s = c.read_jsonl(root / "s.jsonl")[-1]
        out.append(("an unreadable cgroup: the process tree, 9000 ticks over 60 s are 1.5 cores, and it says so",
                    s["src"] == "tree" and s["ours"] == 1.5))
        (cg / "cpu.stat").mkdir()
        out.append(("a cpu.stat that cannot be read as a file is the same as none", c.cgroup_seconds(cg) is None))
        out.append(("-OUT: of the Windows linker, as a real run printed it",
                    c.OUTPUT.search("/x/lld-link -NOLOGO -DLL -OUT:../../../dist/bin/gtest/xul.dll -PDB:xul.pdb").group(1)
                    == "../../../dist/bin/gtest/xul.dll"))

    # Lines of run 41402916724, as mach printed them.
    out.append(("markers: mach's and cargo's own lines",
                c.build_mark(" 2:51.26 Finished reading 2878 moz.build files in 6.82s") == "configured"
                and c.build_mark(" 5:55.61    Compiling gkrust v0.1.0 (/home/runner/work/x)") == "rust-start"
                and c.build_mark("13:11.70     Finished `release` profile [optimized] target(s) in 14.46s") == "rust-end"
                and c.build_mark(" 0:37.01 Finished processing /home/runner/obj-rel/dist") is None
                and c.build_mark(" 3:35.12 mozglue/misc") is None))

    run = events(("start", "build", at(0)), ("mark", "build", at(300), {"mark": "configured"}),
                 ("mark", "build", at(600), {"mark": "rust-start"}), ("mark", "build", at(1500), {"mark": "rust-end"}),
                 ("end", "build", at(5400)), ("start", "package", at(5500)), ("end", "package", at(5800)))
    data = [sample(T0 + t, 30.0) for t in range(0, 3600, 60)]
    data += [sample(T0 + t, 1.0, link=[[77, "libxul.so"]]) for t in range(3600, 5400, 60)]
    data += [sample(T0 + t, 2.0) for t in range(5400, 5900, 60)]
    found = c.phases(data, run)
    names = {p[0]: p for p in found}
    out.append(("phases: configure and compile end on mach's mark and on the first long link, rust on cargo's, the link on its process",
                set(names) == {"build", "configure", "compile", "rust", "link libxul.so", "package"}
                and names["configure"][2:] == (T0, T0 + 300)
                and names["compile"][2:] == (T0 + 300, T0 + 3600)
                and names["rust"][2:] == (T0 + 600, T0 + 1500)
                and names["link libxul.so"][1:] == ("link", T0 + 3600, T0 + 5340)
                and names["package"][2:] == (T0 + 5500, T0 + 5800)))
    # The cold build of 03/11, as mach printed it: libgkcodecs at 35:14, the gtest stubs at
    # 37:15 and 38:36, libmozavcodec at 41:03, libxul at 41:46.
    real = events(("start", "build", at(0)), ("mark", "build", at(117), {"mark": "configured"}), ("end", "build", at(4869)))
    linked = [sample(T0 + t, 33.0) for t in range(0, 2100, 15)]
    linked += [sample(T0 + 2114 + i * 15, 33.0, link=[[501, "libgkcodecs.so"]]) for i in range(3)]
    linked += [sample(T0 + 2235 + i * 15, 33.0, link=[[502, "libxul_broken_buildid.so"]]) for i in range(2)]
    linked += [sample(T0 + 2316 + i * 15, 33.0, link=[[503, "libxul_correct_buildid.so"]]) for i in range(2)]
    linked += [sample(T0 + 2463 + i * 15, 33.0, link=[[504, "libmozavcodec.so"]]) for i in range(2)]
    linked += [sample(T0 + 2506 + i * 15, 1.0, link=[[505, "libxul.so"]]) for i in range(100)]
    by_name = {p[0]: p for p in c.phases(linked, real)}
    out.append(("compile ends at libxul's link, not at a stub or another library seen in two samples, on the real order",
                by_name["compile"][2:] == (T0 + 117, T0 + 2506)
                and {"link libgkcodecs.so", "link libxul_broken_buildid.so", "link libmozavcodec.so"} <= set(by_name)))
    windows = [sample(T0 + t, 33.0) for t in range(0, 300, 15)]
    windows += [sample(T0 + 300 + i * 15, 1.0, link=[[9, "xul.dll"]]) for i in range(3)]
    out.append(("compile ends at xul.dll on the Windows leg", {p[0]: p for p in c.phases(windows, real)}["compile"][3] == T0 + 300))
    only_stub = [sample(T0 + t, 33.0) for t in range(0, 300, 15)] + [sample(T0 + 300 + i * 15, 33.0, link=[[9, "libgkcodecs.so"]]) for i in range(3)]
    out.append(("a build stopped before libxul compiled until its end", {p[0]: p for p in c.phases(only_stub, real)}["compile"][3] == T0 + 4869))
    two = data + [sample(T0 + 6000, 1.0, link=[[88, "xul.dll"]]), sample(T0 + 6060, 1.0, link=[[88, "xul.dll"]]),
                  sample(T0 + 6120, 1.0, link=[[99, "small"]])]
    out.append(("each link has its row, one seen once does not", [p[0] for p in c.phases(two, run) if p[1] == "link"]
                == ["link libxul.so", "link xul.dll"]))
    cut = events(("start", "build", at(0)))
    out.append(("a build with no end goes as far as the samples", c.phases(data, cut)[0][2:] == (T0, data[-1]["t"])))
    out.append(("no samples, no phases", c.phases([], run) == []))

    rows = {r["name"]: r for r in (c.measure(p, data) for p in found) if r}
    compile_row = rows["compile"]
    out.append(("mean and peak of a phase", compile_row["mean"] == 30.0 and compile_row["peak"] == 30.0 and compile_row["seconds"] == 3300))
    mixed = c.measure(("x", "phase", T0, T0 + 180), [sample(T0, 10.0), sample(T0 + 60, 20.0, iowait=3.0), sample(T0 + 120, 30.0), sample(T0 + 180, 99.0)])
    out.append(("only the samples inside it", mixed["mean"] == 20.0 and mixed["peak"] == 30.0 and mixed["iowait"] == 1.0))
    out.append(("a phase with no sample has no row", c.measure(("x", "phase", T0 + 9000, T0 + 9100), data) is None))

    def row(name="compile", kind="phase", seconds=1800, mean=44.0, busy=None, total=48, avail=48):
        return {"name": name, "kind": kind, "start": T0, "end": T0 + seconds, "seconds": seconds, "mean": mean, "peak": mean, "busy": mean if busy is None else busy,
                "iowait": 0.0, "steal": 0.0, "total": total, "avail": avail, "src": "cgroup"}
    out.append(("no warning for a healthy phase", c.warnings([row()]) == []))
    under = c.warnings([row(name="package", mean=10.0)])
    out.append(("a long phase under half of the cores: a warning, naming the phase, the mean and the share",
                len(under) == 1 and "package" in under[0] and "10.0 cores" in under[0] and "50%" in under[0]))
    held = c.warnings([row(mean=40.0)])
    out.append(("the compile has a job for every core: under 85% it warns, at 40.8 of 48 it does not",
                len(held) == 1 and "85%" in held[0] and c.warnings([row(mean=40.8)]) == []))
    out.append(("the measured -j31 build's compile, 33 of 48, would have warned", len(c.warnings([row(mean=33.0)])) == 1))
    out.append(("the share is of the available cores, not the machine's", c.warnings([row(mean=14.0, avail=16)]) == []))
    out.append(("a short phase may idle", c.warnings([row(seconds=c.LONG_S, mean=1.0)]) == []))
    out.append(("a link may idle: a single thread for most of it", c.warnings([row(name="link libxul.so", kind="link", mean=1.0)]) == []))
    owner = c.warnings([row(mean=41.0, busy=47.0, total=48, avail=48), row(name="package", mean=30.0, busy=42.0)])[-1:]
    out.append(("the machine busy well above ours: a warning that names the owner's use",
                len(owner) == 1 and "owner" in owner[0] and "12.0" in owner[0]))
    out.append(("a little noise on the machine is not the owner", c.warnings([row(mean=44.0, busy=47.0)]) == []))
    out.append(("the same on a link: the machine is not ours even then", len(c.warnings([row(kind="link", mean=1.0, busy=20.0)])) == 1))
    still = row(name="package", mean=30.0, busy=42.0)
    out.append(("the owner's warning does not need steal, which reads 0 on the node's kernel", still["steal"] == 0.0 and len(c.warnings([still])) == 1))
    out.append(("the summary says steal does not show here", "Steal did not move" in c.render([row()])
                and "Steal did not move" not in c.render([dict(row(), steal=0.5)])))
    idle = row(mean=33.0, seconds=2400)
    out.append(("the idle core-minutes of a phase: 15 cores for 40 minutes", c.idle_core_minutes(idle) == 600.0))
    spaced = [sample(T0, 0.0), sample(T0 + 60, 24.0), sample(T0 + 120, 12.0)]
    out.append(("the whole job's use, weighted by time: 36 of 96 core-minutes", c.whole(spaced) == (36 / 96, 60.0, 96.0)))
    out.append(("no time, no figure", c.whole(spaced[:1]) is None and c.whole([]) is None))
    link = row(name="link libxul.so", kind="link", mean=3.5, seconds=2300)
    beside = c.render([link, dict(row(name="lavapipe", mean=30.0, seconds=900), start=T0 + 60, end=T0 + 960),
                       dict(row(name="package", seconds=60), start=T0 + 3000, end=T0 + 3060)], spaced)
    out.append(("the summary: a link is one thread by nature, with what ran beside it and the cores it took, not what ran after",
                "link libxul.so: one thread by nature" in beside and "Beside it: lavapipe and 2.5 cores" in beside and "package" not in beside.split("one thread by nature")[1]))
    out.append(("the summary: the whole job's percentage and idle core-minutes", "The whole job, from the build's start:** 38% of the cores, 60 core-minutes idle of 96" in beside))
    out.append(("the summary: each row its use and idle core-minutes", "| 7% | " in beside and "| %.0f |" % c.idle_core_minutes(link) in beside))
    text = c.render([row(mean=10.0), row(name="link libxul.so", kind="link", mean=1.0, seconds=3600)])
    out.append(("the table: duration, mean and peak against the total, iowait; the warnings under it",
                "48 cores on the machine" in text and "| compile | 30m00 | 10.0 | 10.0 |" in text
                and "| link libxul.so | 1h00 |" in text and "iowait" in text and "⚠️ CPU: compile" in text))
    with tempfile.TemporaryDirectory() as tmp:
        none = Path(tmp) / "none.jsonl"
        out.append(("no files at all: a line, no exception", "no samples" in c.report(none, none)))
        broken = Path(tmp) / "b.jsonl"
        broken.write_text('{"t": 1\n' + json.dumps(sample(T0, 1.0)) + "\n")
        out.append(("a cut line in the samples is skipped", len(c.read_jsonl(broken)) == 1))

        class Broken:
            calls = 0

            def tick(self):
                Broken.calls += 1
                raise OSError("no /proc/stat")
        out.append(("a sampler that fails is dropped, and says so once",
                    w.cpu_tick(Broken(), lambda _: None) is None and Broken.calls == 1))
        said = []
        healthy = type("H", (), {"tick": lambda self: "[cpu] x"})()
        out.append(("a healthy one stays, and its line reaches the log",
                    w.cpu_tick(healthy, said.append) is healthy and said == ["[cpu] x"]))
        out.append(("no sampler is not an error", w.cpu_tick(None, said.append) is None))
    return out


def main() -> int:
    got = cases()
    failures = 0
    for name, ok in got:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(got) - failures, len(got)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
