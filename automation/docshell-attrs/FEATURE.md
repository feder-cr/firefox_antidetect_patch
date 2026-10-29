# automation/docshell-attrs

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `docshell/base/nsDocShell.h` (class nsDocShell (member bit-fields, after mIsExecutingOnLoadHandler), already-hook): Storage for the two Juggler attributes declared in nsIDocShell.idl. These bit-fields must be initialised in the nsDocShell constructor (nsDocShell.cpp), which is what the fix there does; the two files must move together.
- `docshell/base/nsIDocShell.idl` (interface nsIDocShell (after synchronizeLayoutHistoryState), already-hook): Two attribute declarations, the rest is comment. The comment says overrideHasFocus, forceActiveState, disallowBFCache, languageOverride, overrideTimezone and overrideLocale are gone; grep of C:/ff/source finds no declaration of any of them in this file, so the comment matches the tree. The comment names the readers (HTMLInputElement for the file picker, CSP_IsBypassedFor for CSP); those readers ar

## docshell/base/nsDocShell.h: mFileInputInterceptionEnabled, mBypassCSPEnabled

Stealthfox: the flags behind the two nsIDocShell attributes Juggler
sets. Initialized in the constructor: they were not, and a bit-field the
constructor leaves alone holds whatever the allocation held, so a docshell
Juggler never wrote to could intercept file pickers, or bypass CSP.

(The two bit-fields sit after mIsExecutingOnLoadHandler, marked
`// STEALTH-HOOK(automation/docshell-attrs)` in the header; they are
initialised to false in nsDocShell.cpp and read and written by
StealthDocShellAttrs.cpp.)

## docshell/base/nsIDocShell.idl: fileInputInterceptionEnabled, bypassCSPEnabled

Stealthfox: the two attributes Juggler sets on a docshell, each READ
where it acts: fileInputInterceptionEnabled by HTMLInputElement (the
file picker), bypassCSPEnabled by CSP_IsBypassedFor (nsCSPUtils). The
port that added them also added overrideHasFocus, forceActiveState and
disallowBFCache as storage only; nothing read them, and they are gone.

(The two attribute declarations follow synchronizeLayoutHistoryState, marked
`// STEALTH-HOOK(automation/docshell-attrs): see FEATURE.md`; the previous
comment sat right above them in the IDL.)

## docshell/base/nsIDocShell.idl: languageOverride, overrideTimezone, overrideLocale (removed)

Stealthfox: languageOverride, overrideTimezone and overrideLocale, the
docshell's side channels for the language and the zone, are gone since
firefox-50: each acted on the realm current when Juggler called it, and
nothing else. Both now travel only in the BrowsingContext's
LanguageOverride and TimezoneOverride fields
(dom/base/StealthSessionDeclaration.h).

(This comment sat after the two attribute declarations of nsIDocShell.idl,
with no code of its own.)
