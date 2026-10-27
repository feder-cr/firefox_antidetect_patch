# automation/file-chooser

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/html/HTMLInputElement.cpp` (HTMLInputElement::InitFilePicker, already-hook): When Juggler asked to intercept the file chooser, InitFilePicker returns before the native dialog opens, without touching mPickerRunning. The JS side gets the element through the notification fired just above. Reads nsIDocShell::GetFileInputInterceptionEnabled, which is in the docshell group (not checked here). Adds #include nsIDocShell.h. The comment is in Italian and dates a fix to 2026-08-25. V
