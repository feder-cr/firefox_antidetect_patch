# fingerprint/canvas/text-metrics

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/canvas/CanvasRenderingContext2D.cpp` (CanvasRenderingContext2D::DrawOrMeasureText (TextMetrics return); also CreatePattern (2 hunks) and a comment-only block near GetImageBuffer, movable): The DrawOrMeasureText hunk is about 45 logic lines (vertical metrics rounding, baseline switch, MakeUnique call). It can move into an own file taking a small input struct, leaving a one-line hook that returns a Maybe<TextMetrics>. The CreatePattern hunks (source snapshot read through the element) belong to canvas-noise-projection, not to text metrics. The 30-line comment block 'a 2D canvas reads b
