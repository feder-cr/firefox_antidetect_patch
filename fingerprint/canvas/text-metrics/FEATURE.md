# fingerprint/canvas/text-metrics

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/canvas/CanvasRenderingContext2D.cpp` (CanvasRenderingContext2D::DrawOrMeasureText (TextMetrics return); also CreatePattern (2 hunks) and a comment-only block near GetImageBuffer, movable): The DrawOrMeasureText hunk is about 45 logic lines (vertical metrics rounding, baseline switch, MakeUnique call). It can move into an own file taking a small input struct, leaving a one-line hook that returns a Maybe<TextMetrics>. The CreatePattern hunks (source snapshot read through the element) belong to canvas-noise-projection, not to text metrics. The 30-line comment block 'a 2D canvas reads b

## History: the removed per-run advance offset

The per-run advance jitter was REMOVED 2026-08-08. It is the one thing a public fingerprinting test page ever caught this build doing (the single entry in its list of detected lies: measureText reported as carrying metric noise), and it was both detectable and unrealistic.

DETECTABLE, by addition and nothing else. The offset was added to the LAST glyph of every shaped run, so measuring a string in pieces accumulated one offset per piece while measuring it whole accumulated one. Measured at 72px in Arial, 50 identical characters summed singly against the same 50 as one string:

    this build      +2.449982 px
    retail FF 152   +0.000069 px

A real Firefox is additive to within seven hundred-thousandths of a pixel; we were off by a factor of 35000. The test needs no reference values, no second machine and no knowledge of our fonts - just add the parts.

UNREALISTIC, which is the better reason. Two real Windows machines with the same fonts measure the same string identically; text width is not a per-machine quantity. Making every identity unique in it manufactured a variation the population being imitated does not have. Per-identity uniqueness still comes from the canvas PIXEL path, from the GPU persona and from the screen - none of which had to lie about a font metric to get it.

The v2 this replaces was itself a reduction of a v1 that perturbed every glyph and moved a long string by ~2.8px, which was scored as tampering by a commercial fingerprinting service's machine-learning check. Halving a tell twice is not the same as removing it.

Test: measureText additivity (the sum of the parts equals the whole).
