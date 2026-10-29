# fingerprint/canvas/as-drawn

Category: other, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `layout/base/nsLayoutUtils.cpp` (nsLayoutUtils::SurfaceFromElement(HTMLCanvasElement*, uint32_t, RefPtr<DrawTarget>&), already-hook): one call, StealthCanvasSnapshot (layout/base/StealthSurfaceFlags.h): callers with kStealthSfeAsDrawn get GetSurfaceSnapshotAsDrawn (no canvas noise); all others keep GetSurfaceSnapshot. The dependency CHECKED: HTMLCanvasElement::GetSurfaceSnapshotAsDrawn is declared at dom/html/HTMLCanvasElement.h:242 and defined at dom/html/HTMLCanvasElement.cpp:1566, both in the modified list.
- `layout/painting/nsImageRenderer.cpp` (nsImageRenderer::PrepareImage, already-hook): -moz-element painting takes the as-drawn path, so the element is shown without canvas noise, like on screen. Category spoof because what it renders is what a page sees.
- `widget/nsBaseDragService.cpp` (nsBaseDragSession::DrawDragForImage (hunk header line 1139), already-hook): Not spoof: GetSurfaceSnapshot applies CanvasUtils::StealthProjectSnapshot noise for WebGL/WebGPU contexts (dom/canvas/OffscreenCanvasDisplayHelper.cpp:459-478) and GetSurfaceSnapshotAsDrawn (line 481) skips it, so the drag image / taskbar preview show the real pixels; a page cannot read either. Companion of the canvas-noise feature, no page-visible effect.
- `widget/windows/TaskbarPreview.cpp` (TaskbarPreviewCallback::Done (hunk header line 332), already-hook): Not spoof: GetSurfaceSnapshot applies CanvasUtils::StealthProjectSnapshot noise for WebGL/WebGPU contexts (dom/canvas/OffscreenCanvasDisplayHelper.cpp:459-478) and GetSurfaceSnapshotAsDrawn (line 481) skips it, so the drag image / taskbar preview show the real pixels; a page cannot read either. Companion of the canvas-noise feature, no page-visible effect.

## The flag: kStealthSfeAsDrawn (layout/base/StealthSurfaceFlags.h)

A canvas as DRAWN, without the canvas noise: for the browser's own
readers that show it and never hand its bytes to a page (-moz-element,
nsDOMWindowUtils). Every other caller is a page reading the canvas or
drawing with it, and gets what HTMLCanvasElement::GetSurfaceSnapshot
and OffscreenCanvas::GetSurfaceSnapshot give a page.

It was SFE_AS_DRAWN = 1 << 9 at the end of nsLayoutUtils' SFE_* enum, the
place where Mozilla appends its next flag: a port would merge two lines taking
the same bit without a conflict. It is ours now, on bit 31 (Mozilla's end at
bit 8 in 157 and 159), marked STEALTH-FLAG-SPACE(layout/base/nsLayoutUtils.h,
SFE_), and build/stealth_check_sources.py fails if an SFE_ flag takes it.
