"""Every json file of tools/ ends its lines one way.

hook-budget.json and removed-budget.json each held one CRLF line among LF lines (one
line written on Windows by a tool that kept the file's text mode), and the next edit by a
script that detected "the file's" line ending rewrote either all of the file as CRLF or
none of it, a whole-file diff in a commit of a few lines. This repository stores bytes
exactly (.gitattributes `* -text`), so the working copy is the committed copy.

Known-bad input: one CRLF line in a file of LF lines, as in the two files before the fix.

    python -m pytest tools/tests
"""
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]


def endings(data: bytes):
    crlf = data.count(b"\r\n")
    return crlf, data.count(b"\n") - crlf


def test_no_json_of_tools_mixes_line_endings():
    files = sorted(TOOLS.glob("*.json"))
    assert files, "no json under tools/"
    mixed = {}
    for f in files:
        crlf, lf = endings(f.read_bytes())
        if crlf and lf:
            mixed[f.name] = "%d CRLF and %d LF lines" % (crlf, lf)
    assert not mixed, "mixed line endings in %s" % mixed


def test_the_check_sees_a_mixed_file():
    """The check itself: a file with one CRLF line among LF lines is mixed."""
    crlf, lf = endings(b'{\n "a": 1,\r\n "b": 2\n}\n')
    assert crlf == 1 and lf == 3
