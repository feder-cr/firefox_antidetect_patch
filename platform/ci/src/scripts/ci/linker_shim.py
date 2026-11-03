"""The Linux node's linker, with huge pages for its heap.

A full-LTO libxul link is one thread for 29 minutes on 24 GB. The node's transparent
huge pages are on `madvise`, and glibc.malloc.hugetlb=1 makes glibc's malloc ask for
them; Mozilla's lld is dynamically linked to the system's glibc (2.35 on the node, the
first that reads this tunable) and has no allocator of its own.

For the link and nothing else: every other process of the build keeps its pages and the
memory the job count was measured with. So a small script stands in for the linker, and
COMPILER_PATH, where clang looks before its own directory, says where it is. That leaves
the command line, configure's options, about:buildconfig and sccache's keys as they were.

Not on Windows. lld-link is found through LINKER, and that is a configure option:
about:buildconfig, in omni.ja, would print the script's path (scripts/toolchain_parity.py
refuses that). The Windows leg's build has the tunable in its environment instead.

The linker's output does not depend on this: malloc returns other addresses, as ASLR
makes it on every run anyway.

And ld.lld caps its own threads at 16 when nothing asks (lld/ELF/Driver.cpp), which is the
last 170 s of the link peaking at 19.7 of 48 cores: `--threads`, before Mozilla's flags,
which name none. With full LTO there is one partition whatever
the threads (--lto-partitions), and lld writes the same bytes with any number of them.
"""
from __future__ import annotations

import sys
from pathlib import Path

TUNABLE = "glibc.malloc.hugetlb=1"
THP_ENABLED = Path("/sys/kernel/mm/transparent_hugepage/enabled")
THP_DEFRAG = Path("/sys/kernel/mm/transparent_hugepage/defrag")


def script(real: Path, first_args: str = "") -> str:
    return '#!/bin/sh\nGLIBC_TUNABLES=%s\nexport GLIBC_TUNABLES\nexec "%s" %s"$@"\n' % (TUNABLE, real, first_args)


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return "unknown"


def write(out: Path, clang_bin: Path = Path.home() / ".mozbuild" / "clang" / "bin", threads: int = 0) -> list:
    """The stand-in for ld.lld; the lines for $GITHUB_ENV."""
    out.mkdir(parents=True, exist_ok=True)
    path = out / "ld.lld"
    path.write_text(script(clang_bin / "ld.lld", "--threads=%d " % threads if threads > 0 else ""), encoding="utf-8", newline="\n")
    path.chmod(0o755)
    print("huge pages for the link: %s, enabled: %s, defrag: %s" % (TUNABLE, _read(THP_ENABLED), _read(THP_DEFRAG)), file=sys.stderr)
    return ["COMPILER_PATH=%s" % out]
