# platform/launch-counter

Category: other
Prefs: invisible_firefox.usage_ping.enabled, invisible_firefox.usage_ping.url
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `browser/components/BrowserGlue.sys.mjs` (BrowserGlue.prototype._beforeUIStartup (before lazy.SessionStartup.init()), movable): Adds a network request (GET, no payload) on every browser process start, gated by the pref, errors swallowed. It adds traffic of our own and removes nothing of Firefox's, hence category other rather than stillborn. Movable: move the block into an own module and leave a one-line call here. The compiled fallback URL (invisible_playwright, BrowserGlue.sys.mjs:426) is effectively dead because browser/
