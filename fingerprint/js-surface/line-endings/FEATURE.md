# fingerprint/js-surface/line-endings

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/file/BlobSet.cpp` (BlobSet::AppendUTF8String, already-hook): Removes the #ifdef XP_WIN around the CRLF conversion, so endings:'native' gives CRLF on every platform (Linux included). Not behind a pref: every build now behaves like Windows. The added lines are comments only; no logic was added. Decide whether the persona should set a pref instead of changing the upstream #ifdef.
- `dom/serializers/nsPlainTextSerializer.cpp` (DetermineLineBreak(const int32_t aFlags, nsAString& aLineBreak), already-hook): The default plain-text line break changes from NS_ULINEBREAK (LF on Linux and macOS) to CRLF on every build. The serializer appears to back other plain-text output such as copy, so the effect may be wider than getSelection().toString(); callers are not verified in this diff. Comment cites internal bug B294. [verified: UNVERIFIED, callers of DetermineLineBreak were not traced.]
