# fingerprint/js-surface/eval-name

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `js/src/debugger/Frame.cpp` (EvaluateInEnv, already-hook): Changes the default filename and introduction type of debugger-evaluated code, which a page can read through Error.stack and script introduction data. A filename given by the caller still wins. A grep of js/src/debugger finds no other 'debugger eval' string literal; the other hits are comments.
