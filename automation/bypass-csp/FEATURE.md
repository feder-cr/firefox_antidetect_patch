# automation/bypass-csp

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/security/nsCSPUtils.cpp` (CSP_IsBypassedFor (new) and CSP_ApplyMetaCSPToDoc, movable): Reads the Juggler bypassCSP flag through nsIDocShell::GetBypassCSPEnabled. The header comment says InitCSP also uses it for header policies. [verified: CONFIRMED, the call site exists at dom/base/Document.cpp:4010 (not in this group).] The comment says the attribute was stored but never read before. The comment has an emoji marker that must be removed. The comment names Playwright and an internal 
- `dom/security/nsCSPUtils.h` (declaration of CSP_IsBypassedFor, before CSP_IsBrowserXHTML, already-hook): Declaration plus a four-line comment. The comment names Juggler and Playwright; Playwright is a third-party test tool, so this falls under the vendor-name rule.
