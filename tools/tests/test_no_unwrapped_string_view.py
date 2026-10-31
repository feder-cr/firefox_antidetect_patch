"""Our C++ does not include <string_view>.

On Linux every file is compiled under `#pragma GCC visibility push(hidden)`
(config/gcc_hidden.h), and only the headers listed in config/system-headers.mozbuild
and config/stl-headers.mozbuild are wrapped back to default visibility. <string_view>
is in neither, and it is the first header to declare std::_Hash_bytes, which lives in
libstdc++.so. StealthWebGPUDeclaration.h included it first and its .cpp hashed a
string_view: the declaration was hidden, full LTO gave every caller in libxul (ANGLE's
translator among them) the hidden reference, and libxul.so did not link on
linux-arm64 (runs 41187230752 and 41194857738, cold and cached alike). Windows has no
such pragma, which is why it built.

Mozilla's own code reaches <string_view> after a wrapped header has declared the
hash; a file of ours that includes it can come first. So ours do not include it.

    python -m pytest tools/tests
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import engine  # noqa: E402

INCLUDE = re.compile(r"^\+?\s*#\s*include\s*<string_view>")
SOURCES = ("*.h", "*.cpp", "*.cc", "*.hpp")


def offenders(name, text, patch=False):
    return ["%s: %s" % (name, line.strip()) for line in text.splitlines()
            if INCLUDE.search(line) and (not patch or (line.startswith("+") and not line.startswith("+++")))]


def test_our_files_do_not_include_string_view():
    bad = []
    for pattern in SOURCES:
        for p in engine.ROOT.glob("**/src/**/" + pattern):
            bad += offenders(p.relative_to(engine.ROOT).as_posix(), p.read_text(encoding="utf-8", errors="replace"))
    for p in engine.ROOT.glob("**/hooks.patch"):
        bad += offenders(p.relative_to(engine.ROOT).as_posix(),
                         p.read_text(encoding="utf-8", errors="replace"), patch=True)
    assert bad == []


def test_the_header_that_broke_the_link_is_caught():
    old = "#include <string_view>\n\n#include \"mozilla/Maybe.h\"\n"
    assert offenders("StealthWebGPUDeclaration.h", old) == ["StealthWebGPUDeclaration.h: #include <string_view>"]
    assert offenders("hooks.patch", "+#include <string_view>\n", patch=True) != []
    assert offenders("hooks.patch", " #include <string_view>\n", patch=True) == []
