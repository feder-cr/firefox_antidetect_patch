# platform/webrender-memory

Category: other
Prefs: none read directly
Test: wr_glyph_rasterizer unit tests `a_namespace_takes_its_fonts_out_of_the_shared_resources`, `a_font_two_namespaces_have_stays_until_the_last_one_is_cleared`, `another_namespace_is_not_touched` (cargo test -p wr_glyph_rasterizer)

## What it does

WebRender clears what a window left in the shared pools when the window's namespace is cleared. A window that left first (UnregisterWindow reaches the render backend on the direct channel, ClearNamespace on the low-priority queue) used to skip the cleanup of the font resources the windows of the pool share, so the fonts, instances and glyph data of every closed window stayed. The render backend now holds the shared font resources and clears them for the namespace itself, through one call that says what it cleared; a font that two namespaces have stays until the last one is cleared.

## Hooks in Mozilla files

- `gfx/wr/webrender/src/render_backend.rs` (RenderBackend (the shared font resources are kept in the backend; clear_namespace clears them whether or not the window is still registered), movable): A namespace whose window left first used to skip the cleanup of the font resources the windows share (UnregisterWindow reaches the backend on the direct channel, ClearNamespace on the low-priority queue): the fonts, instances and glyph data of a closed window stayed in the shared pool. The backend now clears the shared font resources itself. Fixes Mozilla's own code; follows Mozilla when they chan
- `gfx/wr/webrender/src/render_backend_pool.rs` (RenderBackendPool::new (the font resources cloned for the scene builder are cloned once more for the render backend), already-hook): Hands the shared font resources to the render backend, which clears them for a namespace whose window left.
- `gfx/wr/webrender/src/resource_cache.rs` (ResourceCache::clear_namespace (the font resources are cleared through SharedFontResources::clear_namespace), movable): The font part of clear_namespace is one call that returns what was cleared, so the backend and the cache clear the same way. Replaces 14 lines of Mozilla's that did the same steps one by one.
- `gfx/wr/wr_glyph_rasterizer/src/rasterizer.rs` (SharedFontResources::clear_namespace and the type it returns (new), with a unit test, movable): SharedFontResources::clear_namespace returns the local font keys, the shared instance keys and the shared font keys it removed; a unit test fails when a closed window's shared fonts stay in the pool.
