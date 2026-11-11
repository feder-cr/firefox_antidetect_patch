"""The rules `engine.py check` enforces, each shown failing on the input it exists for.

The series is only useful while it stays a series: every folder listed and present,
each file of ours owned by one folder, and Mozilla's files carrying hooks rather than
our logic. These tests run the real check on the real repo (it must pass), then break
one rule at a time on a copy of the budget and prove the check notices.

    python -m pytest tools/tests
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import engine  # noqa: E402


def test_the_series_as_committed_breaks_no_rule():
    assert engine.problems() == []


def _with_budget(tmp_path, monkeypatch, change):
    budget = json.loads(engine.BUDGET.read_text(encoding="utf-8"))
    change(budget)
    path = tmp_path / "hook-budget.json"
    path.write_text(json.dumps(budget), encoding="utf-8")
    monkeypatch.setattr(engine, "BUDGET", path)
    return engine.problems()


def test_a_mozilla_file_that_grows_past_its_budget_fails(tmp_path, monkeypatch):
    def shrink(b):
        b["startupcache/StartupCache.h"] -= 1
    found = _with_budget(tmp_path, monkeypatch, shrink)
    assert any("startupcache/StartupCache.h" in p and "budget is" in p for p in found), found


def test_a_newly_patched_mozilla_file_needs_a_budget(tmp_path, monkeypatch):
    found = _with_budget(tmp_path, monkeypatch, lambda b: b.pop("startupcache/StartupCache.h"))
    assert any("no hook budget" in p for p in found), found


def test_a_budget_for_a_file_no_longer_patched_must_go(tmp_path, monkeypatch):
    found = _with_budget(tmp_path, monkeypatch, lambda b: b.__setitem__("dom/base/Gone.cpp", 3))
    assert any("dom/base/Gone.cpp" in p and "remove it" in p for p in found), found


def test_series_must_list_exactly_the_folders_on_disk(tmp_path, monkeypatch):
    listed = engine.series_folders()
    monkeypatch.setattr(engine, "series_folders", lambda: listed[1:])
    assert any("SERIES and the folders differ" in p for p in engine.problems())


def test_the_budget_only_goes_down(tmp_path, monkeypatch):
    budget = json.loads(engine.BUDGET.read_text(encoding="utf-8"))
    budget["startupcache/StartupCache.h"] += 40
    path = tmp_path / "hook-budget.json"
    path.write_text(json.dumps(budget), encoding="utf-8")
    monkeypatch.setattr(engine, "BUDGET", path)
    monkeypatch.setattr(engine, "REMOVED_BUDGET", _copy_removed_budget(tmp_path, lambda b: None))
    engine.shrink_budget()
    after = json.loads(path.read_text(encoding="utf-8"))
    assert after["startupcache/StartupCache.h"] == budget["startupcache/StartupCache.h"] - 40


# The second budget: Mozilla's lines we remove or rewrite. Such a line conflicts at a
# port like one we add, and a copy of it moved into a file of ours stops following
# Mozilla (InitializeFamily's cmap loader, 2026-10-30).
FILE = "gfx/thebes/gfxPlatformFontList.cpp"


def _copy_removed_budget(tmp_path, change):
    budget = json.loads(engine.REMOVED_BUDGET.read_text(encoding="utf-8"))
    change(budget)
    path = tmp_path / "removed-budget.json"
    path.write_text(json.dumps(budget), encoding="utf-8")
    return path


def test_a_mozilla_file_losing_more_lines_than_its_removed_budget_fails(tmp_path, monkeypatch):
    def shrink(b):
        b[FILE] -= 1
    monkeypatch.setattr(engine, "REMOVED_BUDGET", _copy_removed_budget(tmp_path, shrink))
    found = engine.problems()
    assert any(FILE in p and "removed budget is" in p for p in found), found


def test_a_file_without_a_removed_budget_may_remove_nothing(tmp_path, monkeypatch):
    monkeypatch.setattr(engine, "REMOVED_BUDGET", _copy_removed_budget(tmp_path, lambda b: b.pop(FILE)))
    found = engine.problems()
    assert any(FILE in p and "removed budget is 0" in p for p in found), found


def test_a_removed_budget_for_a_file_no_longer_patched_must_go(tmp_path, monkeypatch):
    path = _copy_removed_budget(tmp_path, lambda b: b.__setitem__("dom/base/Gone.cpp", 3))
    monkeypatch.setattr(engine, "REMOVED_BUDGET", path)
    found = engine.problems()
    assert any("removed-budget.json" in p and "dom/base/Gone.cpp" in p for p in found), found


def test_the_removed_budget_only_goes_down_and_drops_files_at_zero(tmp_path, monkeypatch):
    def loosen(b):
        b[FILE] += 40
        b["startupcache/StartupCache.h"] = 7  # patched, removes nothing
    path = _copy_removed_budget(tmp_path, loosen)
    monkeypatch.setattr(engine, "REMOVED_BUDGET", path)
    hook = tmp_path / "hook-budget.json"
    hook.write_bytes(engine.BUDGET.read_bytes())
    monkeypatch.setattr(engine, "BUDGET", hook)
    engine.shrink_budget()
    after = json.loads(path.read_text(encoding="utf-8"))
    real = json.loads(engine.REMOVED_BUDGET.read_text(encoding="utf-8"))
    assert after[FILE] == real[FILE]
    assert "startupcache/StartupCache.h" not in after


def test_lines_are_counted_inside_hunks_only(tmp_path):
    patch = tmp_path / "hooks.patch"
    patch.write_bytes(b"diff --git a/x.sql b/x.sql\n--- a/x.sql\n+++ b/x.sql\n"
                      b"@@ -1,2 +1,2 @@\n--- a comment\n+++ a line\n keep\n")
    assert engine.removed_lines(patch) == {"x.sql": 1}
    assert engine.added_lines(patch) == {"x.sql": 1}


def test_a_root_workflow_edited_by_hand_fails(tmp_path, monkeypatch):
    root = tmp_path / "wf"
    root.mkdir()
    for p in engine.WORKFLOWS_ROOT.glob("*.yml"):
        (root / p.name).write_bytes(p.read_bytes())
    (root / "release.yml").write_bytes((root / "release.yml").read_bytes() + b"# by hand\n")
    monkeypatch.setattr(engine, "WORKFLOWS_ROOT", root)
    assert any("release.yml differs" in p for p in engine.problems())


# Upstream code carried ahead of Mozilla (category backport): it is not ours, so it has
# no budget; its BACKPORT.json names the upstream, the revision Mozilla vendors and the
# commits (wgpu's Vulkan external textures on the vendored 4f4dc63, 2026-11-01).
BACKPORTED = "third_party/rust/naga/src/back/spv/image.rs"
RECORD = {"upstream": "https://github.com/gfx-rs/wgpu", "vendored": "4f4dc63",
          "commits": ["49325ca"], "drop_when": "Mozilla vendors a wgpu that has them"}


def _backport(tmp_path, monkeypatch, record):
    d = tmp_path / "f"
    d.mkdir()
    patch = d / "hooks.patch"
    patch.write_text('source = "git+https://github.com/gfx-rs/wgpu?rev=4f4dc63"\n', encoding="utf-8")
    if record is not None:
        (d / engine.BACKPORT).write_text(json.dumps(record), encoding="utf-8")
    monkeypatch.setattr(engine, "ROOT", tmp_path)
    return lambda files: engine.backport_problems("f", files, patch)


def test_a_well_formed_backport_breaks_no_rule(tmp_path, monkeypatch):
    assert _backport(tmp_path, monkeypatch, RECORD)({BACKPORTED}) == []


def test_upstream_code_needs_its_backport_record(tmp_path, monkeypatch):
    found = _backport(tmp_path, monkeypatch, None)({BACKPORTED})
    assert any("no BACKPORT.json" in p for p in found), found


def test_a_backport_record_names_all_it_must(tmp_path, monkeypatch):
    found = _backport(tmp_path, monkeypatch, dict(RECORD, drop_when=""))({BACKPORTED})
    assert any("lacks drop_when" in p for p in found), found


def test_a_backport_on_a_revision_mozilla_no_longer_vendors_fails(tmp_path, monkeypatch):
    found = _backport(tmp_path, monkeypatch, dict(RECORD, vendored="0123abc"))({BACKPORTED})
    assert any("Mozilla vendors another one now" in p for p in found), found


def test_only_vendored_code_can_be_a_backport(tmp_path, monkeypatch):
    found = _backport(tmp_path, monkeypatch, RECORD)({"gfx/wgpu_bindings/src/lib.rs"})
    assert any("outside vendored code" in p for p in found), found


def test_upstream_code_carries_no_budget(tmp_path, monkeypatch):
    found = _with_budget(tmp_path, monkeypatch, lambda b: b.__setitem__(BACKPORTED, 5000))
    assert any(BACKPORTED in p and "is upstream code" in p for p in found), found


def test_a_carriage_return_inside_a_string_literal_is_refused():
    """The build of a gtest broke on a carriage return and a C1 character written raw inside a string (the two bytes of a port)."""
    bad = 'EXPECT_EQ(0, memcmp(p, "\r\u0096", 2)) << "port";\n'
    problems = engine.cxx_byte_problems(bad)
    assert problems and "literal" in problems[0][1], problems


def test_a_character_outside_ascii_inside_a_literal_is_refused_but_not_in_a_comment():
    assert engine.cxx_byte_problems('const char* s = "caf\u00e9";\n')
    assert engine.cxx_byte_problems("// caf\u00e9 \u2014 a comment may say it\n/* \u00e0 */ int x;\n") == []


def test_escapes_raw_strings_and_digit_separators_are_clean():
    clean = (
        'const char a[] = "\\x0d\\x96\\"";\n'
        'const char b = \'\\n\';\n'
        'int big = 1\'000\'000;\n'
        'const char* r = R"x(a "quoted" \u00e9 text)x";\n'
    )
    assert engine.cxx_byte_problems(clean) == [], engine.cxx_byte_problems(clean)


def test_a_control_character_anywhere_is_refused_and_a_crlf_is_not():
    assert engine.cxx_byte_problems("int x;\x0b\n")
    assert engine.cxx_byte_problems("int x;\r\nint y;\r\n") == []
    assert engine.cxx_byte_problems("int x;\rint y;\n")


def test_the_files_of_ours_as_committed_have_no_such_character():
    assert engine.own_cxx_problems() == []


def test_a_prefixed_character_literal_is_a_literal_not_a_digit_separator():
    assert engine.cxx_byte_problems("int32_t bar = body.FindChar(u'|', start);\nconst auto s = aPath.RFindChar(u'\\\\');\n") == []
    assert engine.cxx_byte_problems("char c = L'\\u00e9';\n") == []
    assert engine.cxx_byte_problems("char16_t c = u'\u00e9';\n")


# A STEALTH-HOOK tag names a folder of SERIES. The hook of nsSOCKSIOLayer.cpp that warms the
# pool was tagged network/socks while network/proxy-udp owns it, and the evidence of the hook
# of netwerk/socket/moz.build quoted network/socks-udp-h3, a folder deleted since (review of
# the UDP line, 2026-11-11).
SOCKS_LAYER = "netwerk/socket/nsSOCKSIOLayer.cpp"


def _record(file):
    return next(r for r in json.loads(engine.INVENTORY.read_text(encoding="utf-8")) if r["file"] == file)


def test_the_tags_of_the_series_as_committed_are_all_folders():
    records = json.loads(engine.INVENTORY.read_text(encoding="utf-8"))
    patches = {f: (engine.ROOT / f / "hooks.patch").read_text(encoding="utf-8", errors="replace")
               for f in engine.existing_folders() if (engine.ROOT / f / "hooks.patch").exists()}
    assert engine.hook_tag_problems(set(engine.series_folders()), records, patches) == []


def test_an_evidence_tag_of_a_folder_that_does_not_exist_fails():
    record = dict(_record("netwerk/socket/moz.build"))
    record["evidence"] = [e.replace("network/proxy-udp", "network/socks-udp-h3") for e in record["evidence"]]
    assert any("network/socks-udp-h3" in e for e in record["evidence"])
    found = engine.hook_tag_problems(set(engine.series_folders()), [record], {})
    assert any("netwerk/socket/moz.build" in p and "network/socks-udp-h3" in p and "not a folder of SERIES" in p
               for p in found), found


def test_the_evidence_of_the_socks_layer_hook_names_the_folder_that_owns_it():
    record = _record(SOCKS_LAYER)
    assert engine.hook_tag_problems(set(engine.series_folders()), [record], {}) == []
    assert any("STEALTH-HOOK(network/proxy-udp)" in e for e in record["evidence"])


def test_a_hook_of_a_patch_tagged_with_a_folder_that_does_not_exist_fails():
    patch = "+  nsSOCKSUDPIOLayerWarm(proxy);  // STEALTH-HOOK(network/socks-udp-h3)\n"
    found = engine.hook_tag_problems(set(engine.series_folders()), [], {"network/proxy-udp": patch})
    assert any("network/proxy-udp/hooks.patch" in p and "network/socks-udp-h3" in p for p in found), found
    assert engine.hook_tag_problems(set(engine.series_folders()), [],
                                    {"network/proxy-udp": patch.replace("socks-udp-h3", "proxy-udp")}) == []


def test_the_check_reads_the_tags_of_the_inventory(tmp_path, monkeypatch):
    records = json.loads(engine.INVENTORY.read_text(encoding="utf-8"))
    for r in records:
        if r["file"] == SOCKS_LAYER:
            r["evidence"] = [e.replace("network/proxy-udp", "network/socks-udp-h3") for e in r["evidence"]]
    path = tmp_path / "inventory.json"
    path.write_text(json.dumps(records), encoding="utf-8")
    monkeypatch.setattr(engine, "INVENTORY", path)
    assert any(SOCKS_LAYER in p and "socks-udp-h3" in p for p in engine.problems())


# The counts of an inventory record are the ones of its file in the hooks.patch. The record of
# nsHttpHandler.cpp (accept-language) had the data of another feature (added 18, hunks 6, a
# parental-controls hook the file never had); the real diff is +11/-3 in 2 hunks, and 155
# records had counts of months ago.
HTTP_HANDLER = "netwerk/protocol/http/nsHttpHandler.cpp"


def _counts():
    out = {}
    for f in engine.existing_folders():
        patch = engine.ROOT / f / "hooks.patch"
        if patch.exists():
            a, r, h = engine.added_lines(patch), engine.removed_lines(patch), engine.hunk_counts(patch)
            out.update({p: (a[p], r.get(p, 0), h.get(p, 0)) for p in a})
    return out


def test_every_record_as_committed_has_the_counts_of_its_patch():
    records = json.loads(engine.INVENTORY.read_text(encoding="utf-8"))
    assert engine.record_count_problems(records, _counts()) == []


def test_the_record_of_the_accept_language_hook_is_its_own():
    record = _record(HTTP_HANDLER)
    assert (record["added"], record["removed"], record["hunks"]) == (11, 3, 2)
    assert any("PrepareAcceptLanguages" in e for e in record["evidence"])
    assert not any("ParentalControls" in e for e in record["evidence"])


def test_a_record_with_the_counts_of_another_feature_fails():
    record = dict(_record(HTTP_HANDLER), added=18, hunks=6)
    found = engine.record_count_problems([record], _counts())
    assert any(HTTP_HANDLER in p and "says added 18, removed 3, hunks 6" in p and "has 11, 3, 2" in p
               for p in found), found


def test_the_check_reads_the_counts_of_the_inventory(tmp_path, monkeypatch):
    records = json.loads(engine.INVENTORY.read_text(encoding="utf-8"))
    for r in records:
        if r["file"] == HTTP_HANDLER:
            r["removed"] = 0
    path = tmp_path / "inventory.json"
    path.write_text(json.dumps(records), encoding="utf-8")
    monkeypatch.setattr(engine, "INVENTORY", path)
    assert any(HTTP_HANDLER in p and "removed 0" in p for p in engine.problems())


def test_hunks_are_counted_per_file(tmp_path):
    patch = tmp_path / "hooks.patch"
    patch.write_bytes(b"diff --git a/a.cpp b/a.cpp\n--- a/a.cpp\n+++ b/a.cpp\n@@ -1,1 +1,2 @@\n x\n+y\n@@ -9,1 +10,2 @@\n z\n+w\n"
                      b"diff --git a/b.cpp b/b.cpp\n--- a/b.cpp\n+++ b/b.cpp\n@@ -1,1 +1,2 @@\n x\n+y\n")
    assert engine.hunk_counts(patch) == {"a.cpp": 2, "b.cpp": 1}
