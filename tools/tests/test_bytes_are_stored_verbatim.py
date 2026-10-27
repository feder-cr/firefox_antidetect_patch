"""git stores this repo's files byte for byte, whatever core.autocrlf says.

The series rebuilds Firefox exactly only if every patch and every file of ours comes
back out of git with the bytes it went in with. On a machine with core.autocrlf=true
(this one, 2026-10-27) git normalised the line endings of the first commit, and a
check that read the working copy still said "identical". .gitattributes turns the
conversion off for every path; this test fails if a path is ever left to it.

Known-bad input: narrow .gitattributes (e.g. "*.md -text") and this test goes red. (Deleting
the file is not one: git then reads the committed copy.)

    python -m pytest tools/tests
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_no_tracked_file_is_left_to_line_ending_conversion():
    files = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z"], check=True,
                           capture_output=True).stdout.split(b"\0")
    files = [f for f in files if f]
    out = subprocess.run(["git", "-C", str(ROOT), "check-attr", "-z", "--stdin", "text"],
                         input=b"\0".join(files) + b"\0", check=True, capture_output=True).stdout
    fields = out.split(b"\0")
    converted = [fields[i].decode() for i in range(0, len(fields) - 2, 3) if fields[i + 2] != b"unset"]
    assert files, "no tracked files"
    assert not converted, "git may convert the line endings of: %s" % converted[:10]
