# automation/shadow-root

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/Element.cpp` (Element::GetShadowRootForBindings, movable): Closed shadow roots are returned only to the automation sandbox (ExpandedPrincipal subject), never UA widgets; the page keeps the spec answer (null). The decision is about 10 logic lines and can move to one of our files behind a one-line call. The principal test is deliberately before the pref read (the comment says content must not pay for the exception; timing was measured). mozilla::gfx::Stealt
- `dom/base/Element.h` (Element::GetShadowRootForBindings (declaration), already-hook): Declaration only. Caller is the generated binding: dom/webidl/Element.webidl:298 declares shadowRoot with [NeedsSubjectPrincipal, BinaryName="shadowRootForBindings"], so the generated code passes the subject principal (earlier doubt refuted, the build does not break).
- `dom/webidl/Element.webidl` (partial interface Element, readonly attribute shadowRoot, already-hook): Adds NeedsSubjectPrincipal so the native getter receives the caller's principal. [verified: Element.h:1800 and Element.cpp:1410-1480: content callers get null for closed roots (a non-ExpandedPrincipal returns nullptr), UA widgets are refused, and only the sandbox/automation principal gets the closed root, gated by mozilla::gfx::StealthEngineActive().] The page-visible answer is unchanged by design
