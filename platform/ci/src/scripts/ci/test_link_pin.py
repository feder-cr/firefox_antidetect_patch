"""link_pin.py against a fake sysfs and a fake /proc."""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import link_pin as lp  # noqa: E402


def sysfs(root: Path):
    """Two L3 groups as the 4484PX shows them: 96 MB on cpus 0-5,12-17, 32 MB on 6-11,18-23."""
    for cpu in range(24):
        big = cpu % 12 < 6
        d = root / ("cpu%d" % cpu) / "cache" / "index3"
        d.mkdir(parents=True)
        (d / "level").write_text("3\n")
        (d / "size").write_text("98304K\n" if big else "32768K\n")
        (d / "shared_cpu_list").write_text("0-5,12-17\n" if big else "6-11,18-23\n")
        f = root / ("cpu%d" % cpu) / "cpufreq"
        f.mkdir()
        (f / "scaling_governor").write_text("powersave\n")
        (f / "scaling_driver").write_text("amd-pstate-epp\n")


def proc(root: Path, pid: int, argv: list, uid_ok=True):
    d = root / str(pid)
    (d / "task" / str(pid)).mkdir(parents=True)
    (d / "cmdline").write_bytes(b"\0".join(a.encode() for a in argv) + b"\0")
    if not uid_ok:
        os.chmod(d, 0o755)


def cases() -> list:
    out = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "cpu"
        sysfs(root)
        g = lp.groups(root)
        out.append(("link pin: the largest L3 first, each group with its cpus",
                    [kb for kb, _ in g] == [98304, 32768] and g[0][1] == list(range(0, 6)) + list(range(12, 18))
                    and g[1][1] == list(range(6, 12)) + list(range(18, 24))))
        info = Path(tmp) / "cpuinfo"
        info.write_text("processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD EPYC 4484PX 12-Core Processor\n",
                        encoding="utf-8")
        lines = lp.machine(root, info)
        out.append(("link pin: it says the CPU, the groups, the driver and the governor",
                    lines[0] == "cpu: AMD EPYC 4484PX 12-Core Processor"
                    and "96 MB on cpus 0,1,2,3,4,5,12" in lines[1] and "scaling_governor: powersave" in lines
                    and "scaling_driver: amd-pstate-epp" in lines))
        out.append(("link pin: no cpuinfo is said, not guessed", lp.cpu_model(Path(tmp) / "none") == "not exposed"))
        p = Path(tmp) / "proc"
        proc(p, 101, ["/home/runner/.mozbuild/clang/bin/ld.lld", "-o", "libxul.so", "a.o"])
        proc(p, 102, ["/home/runner/.mozbuild/clang/bin/ld.lld", "-o", "libmozgtk.so", "a.o"])
        proc(p, 103, ["/home/runner/.mozbuild/clang/bin/lld-link", "-OUT:xul.dll", "a.obj"])
        proc(p, 104, ["/usr/bin/python3", "-o", "libxul.so"])
        proc(p, 105, ["/home/runner/.mozbuild/clang/bin/lld-link", "/OUT:../../dist/bin/xul.dll"])
        found = sorted(lp.links(os.getuid(), p))
        out.append(("link pin: the libxul and xul.dll links of this user, nothing else", found == [101, 103, 105]))
        out.append(("link pin: another user's link is the other leg's, never touched", lp.links(os.getuid() + 12345, p) == []))
        moved = []
        n = lp.pin(101, [0, 1], p, lambda tid, cpus: moved.append((tid, list(cpus))), lambda tid: set())
        out.append(("link pin: every thread of the link is moved", n == 1 and moved == [(101, [0, 1])]))
    return out


def main() -> int:
    got = cases()
    bad = 0
    for name, ok in got:
        print(("ok    " if ok else "FAIL  ") + name)
        bad += not ok
    print("%d/%d cases as expected" % (len(got) - bad, len(got)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
