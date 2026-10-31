# minimal/glean

Category: build
Prefs: none read directly
Test: on the built package, the Glean runtime markers reported by validate_release.py (assert_minimal_leaves_out); at run time, the parent's heap stays flat per request (scratchpad test_minapp_parent_memory_flat, to be moved next to the engine)

## What it does

Glean removed from the minimal engine at the source, under the Rust feature `stealth_minimal` that gkrust-features.mozbuild turns on with MOZ_STEALTH_MINIMAL. Glean is never initialized (no upload manager, no database, no glean.init thread), there is never a Glean object, every metric operation is dropped instead of queued, and child processes buffer nothing for the parent. Without this, an engine whose front-end never initializes Glean keeps every metric operation in Glean's pre-init queue, up to a million of them: about 2.4 KB of parent heap per request, measured on the official Firefox 159 too (2026-10-31). The direct readers left (about:glean and test getters) panic if called; the minimal engine never calls them. The normal engine compiles none of this.

## Hooks in Mozilla files

- `third_party/rust/glean-core/.cargo-checksum.json` (the files map, already-hook): New sha256 of the three glean-core files we edit.
- `third_party/rust/glean-core/Cargo.toml` (top of [features], already-hook): Declares stealth_minimal.
- `third_party/rust/glean-core/src/core/mod.rs` (top of global_glean(), already-hook): With stealth_minimal there is never a Glean object, so LTO drops the code that would use one.
- `third_party/rust/glean-core/src/dispatcher/global.rs` (launch(), already-hook): With stealth_minimal, launch() drops the task: nothing is queued (the pre-init queue held up to a million tasks).
- `third_party/rust/glean/.cargo-checksum.json` (the files map, already-hook): New sha256 of the two glean files we edit; Cargo verifies vendored crates file by file.
- `third_party/rust/glean/Cargo.toml` (top of [features], already-hook): Declares stealth_minimal, forwarded to glean-core.
- `third_party/rust/glean/src/lib.rs` (top of initialize_internal, already-hook): With stealth_minimal, Glean is never initialized: no upload manager, no database, no glean.init thread.
- `toolkit/components/glean/Cargo.toml` (top of [features], already-hook): Declares stealth_minimal, forwarded to firefox-on-glean.
- `toolkit/components/glean/api/Cargo.toml` (top of [features], already-hook): Declares stealth_minimal, forwarded to glean.
- `toolkit/components/glean/api/src/ipc.rs` (top of need_ipc(), already-hook): With stealth_minimal, child processes do not buffer metrics for the parent either.
- `toolkit/library/rust/gkrust-features.mozbuild` (after the glean_with_gecko feature, already-hook): Turns the Rust feature on when configure sets MOZ_STEALTH_MINIMAL.
- `toolkit/library/rust/shared/Cargo.toml` (top of [features], already-hook): Declares stealth_minimal, forwarded to fog_control.
