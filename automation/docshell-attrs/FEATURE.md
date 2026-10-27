# automation/docshell-attrs

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `docshell/base/nsDocShell.h` (class nsDocShell (member bit-fields, after mIsExecutingOnLoadHandler), already-hook): Storage for the two Juggler attributes declared in nsIDocShell.idl. These bit-fields must be initialised in the nsDocShell constructor (nsDocShell.cpp), which is what the fix there does; the two files must move together.
- `docshell/base/nsIDocShell.idl` (interface nsIDocShell (after synchronizeLayoutHistoryState), already-hook): Two attribute declarations, the rest is comment. The comment says overrideHasFocus, forceActiveState, disallowBFCache, languageOverride, overrideTimezone and overrideLocale are gone; grep of C:/ff/source finds no declaration of any of them in this file, so the comment matches the tree. The comment names the readers (HTMLInputElement for the file picker, CSP_IsBypassedFor for CSP); those readers ar
