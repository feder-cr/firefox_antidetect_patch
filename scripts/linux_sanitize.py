#!/usr/bin/env python3
"""Binary-level sanitization of the stripped Linux Firefox build.

Operates on files in-place under the staging dir.
Replaces `/home/feder/ff-build/firefox-146.0.1` (36 bytes) with a neutral
same-length path so that __FILE__ macros in .rodata (assertion messages,
panic strings, tracing events) no longer reveal the build machine's
layout.

Same-length replacement preserves all pointer offsets — binaries stay
functional, only the strings shown in panic/assert messages change.
"""
import os
import sys
from pathlib import Path

# (src_path, dst_path) pairs. Each pair must have matching byte lengths.
REPLACEMENTS = [
    (
        b"/home/feder/ff-build/firefox-150",
        b"/b/firefox-stealth/src/gecko-150",
    ),
    (
        b"/home/feder/ff-build/firefox-146.0.1",
        b"/b/firefox-stealth/src/gecko-146.0.1",
    ),
    (
        b"/home/feder/.mozbuild/sysroot-x86_64-linux-gnu",
        b"/b/firefox-stealth/sysroot/linux-x86_64-gnu-10",
    ),
]
for a, b in REPLACEMENTS:
    assert len(a) == len(b), f"length mismatch: {len(a)} ({a!r}) vs {len(b)} ({b!r})"

STAGING = Path(os.environ.get("STAGING", Path.home() / "ff-build" / "stealth-staging"))


def sanitize_file(path: Path) -> int:
    """Return number of replacements done. 0 if none."""
    try:
        data = path.read_bytes()
    except (OSError, IsADirectoryError):
        return 0
    total = 0
    new = data
    for src, dst in REPLACEMENTS:
        if src in new:
            total += new.count(src)
            new = new.replace(src, dst)
    if total == 0:
        return 0
    path.write_bytes(new)
    return total


def main() -> int:
    if not STAGING.is_dir():
        print(f"ERROR: staging dir not found: {STAGING}", file=sys.stderr)
        return 1

    total_files = 0
    total_replacements = 0
    touched_files = 0

    for root, _dirs, files in os.walk(STAGING):
        for name in files:
            p = Path(root) / name
            if p.is_symlink():
                continue
            total_files += 1
            n = sanitize_file(p)
            if n > 0:
                total_replacements += n
                touched_files += 1

    print(f"[sanitize] scanned: {total_files} files")
    print(f"[sanitize] touched: {touched_files} files")
    print(f"[sanitize] replaced: {total_replacements} occurrences across "
          f"{len(REPLACEMENTS)} patterns")
    for s, d in REPLACEMENTS:
        print(f"           {s.decode()!r} -> {d.decode()!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
