# fingerprint/locale-time/accept-language

Category: other, spoof
Prefs: intl.accept_languages
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `netwerk/protocol/http/nsHttpHandler.cpp` (nsHttpHandler::AddStandardRequestHeaders, already-hook): Changes the Accept-Language value a page can observe on requests from a BrowsingContext with languageOverride set. A single tag prepares to itself; a list now gets q-values as intl.accept_languages does. If the prepared value is empty the header is not set (no fallback). Needs a forward declaration of the static PrepareAcceptLanguages (defined around line 2098 of this file, declared around line 74
- `netwerk/test/browser/browser_accept_language_override.js` (test_language_override_list_is_prepared (new add_task), movable): Test code (no test category in the enum). It is added inside an existing Mozilla test file; move it to our own test file and register that in the test moz.build. The expected header 'it-IT,it;q=0.9,en-US;q=0.8,en;q=0.7' is hard-coded and must be updated if Firefox changes the q-value preparation.

## netwerk/protocol/http/nsHttpHandler.cpp: nsHttpHandler::AddStandardRequestHeaders

STEALTHFOX: the override is a language LIST here, not one tag.
`juggler.locale.override` seeds the BrowsingContext LanguageOverride
field with the same list invisible_core writes to intl.accept_languages
("it-IT, it, en-US, en"), because navigator.languages is split from it.
Copied verbatim it went out as that raw list, spaces and no q-values, on
every request with a BrowsingContext, while requests without one (worker
fetches, service workers) took the else branch (the one after the
`if (!aLanguageOverride.IsEmpty())` branch) and sent the prepared
form: two different headers in one session, and only the second is what
Firefox sends. The list is prepared here by the same function that
prepares intl.accept_languages, so a list sends exactly what Firefox
sends for it. A single tag ("de-DE") prepares to itself.
