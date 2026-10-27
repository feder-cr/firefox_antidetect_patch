# fingerprint/canvas/as-drawn

Category: other, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `layout/base/nsLayoutUtils.cpp` (nsLayoutUtils::SurfaceFromElement(HTMLCanvasElement*, uint32_t, RefPtr<DrawTarget>&), already-hook): Callers with SFE_AS_DRAWN get GetSurfaceSnapshotAsDrawn (no canvas noise); all others keep GetSurfaceSnapshot. The dependency CHECKED: HTMLCanvasElement::GetSurfaceSnapshotAsDrawn is declared at dom/html/HTMLCanvasElement.h:242 and defined at dom/html/HTMLCanvasElement.cpp:1566, both in the modified list.
- `layout/base/nsLayoutUtils.h` (nsLayoutUtils::SurfaceFromElement (SurfaceFromElementFlags enum), already-hook): New flag bit 9, which follows SFE_ALLOW_UNCROPPED_UNSCALED = 1 << 8 in the same enum. The comment says the only callers are the browser's own readers (-moz-element, nsDOMWindowUtils): CONFIRMED, callers are layout/painting/nsImageRenderer.cpp (this group) and dom/base/nsDOMWindowUtils.cpp:1539.
- `layout/painting/nsImageRenderer.cpp` (nsImageRenderer::PrepareImage, already-hook): -moz-element painting takes the as-drawn path, so the element is shown without canvas noise, like on screen. Category spoof because what it renders is what a page sees.
- `widget/nsBaseDragService.cpp` (nsBaseDragSession::DrawDragForImage (hunk header line 1139), already-hook): Not spoof: GetSurfaceSnapshot applies CanvasUtils::StealthProjectSnapshot noise for WebGL/WebGPU contexts (dom/canvas/OffscreenCanvasDisplayHelper.cpp:459-478) and GetSurfaceSnapshotAsDrawn (line 481) skips it, so the drag image / taskbar preview show the real pixels; a page cannot read either. Companion of the canvas-noise feature, no page-visible effect.
- `widget/windows/TaskbarPreview.cpp` (TaskbarPreviewCallback::Done (hunk header line 332), already-hook): Not spoof: GetSurfaceSnapshot applies CanvasUtils::StealthProjectSnapshot noise for WebGL/WebGPU contexts (dom/canvas/OffscreenCanvasDisplayHelper.cpp:459-478) and GetSurfaceSnapshotAsDrawn (line 481) skips it, so the drag image / taskbar preview show the real pixels; a page cannot read either. Companion of the canvas-noise feature, no page-visible effect.
