# firefox_antidetect_patch

[![browser launches](https://raw.githubusercontent.com/feder-cr/invisible_playwright/badges/docs/badges/launches.svg)](https://github.com/feder-cr/invisible_firefox/releases/tag/usage-counter)

Firefox, patched at the C++ source level so the browser fingerprint is produced by the
engine instead of being injected into the page. This repository holds the patched
source and publishes the built binaries as releases.

It is the engine behind two projects, and most people want one of those instead of
this repository:

- **[invisible_playwright](https://github.com/feder-cr/invisible_playwright)**, a
  Python wrapper that drives it through the standard Playwright API.
- **[invisible_firefox](https://github.com/feder-cr/invisible_firefox)**, a profile
  manager for running many identities from one desktop app.

Current release: **firefox-18**, built from Firefox 151.0. Branch **`stealth/151`** is
the source of truth; see [STEALTH_BRANCH_README.md](STEALTH_BRANCH_README.md) for the
patch series.

## What is patched, and why it is done here

An injected script has to run inside the page, which means the page can find it. The
override is a function whose source can be printed, its result can be compared against
a value nobody thought to change with it, and a worker or an iframe can ask the same
question in a context the patch never reached. Doing it in the engine removes the seam
instead of hiding it.

The surfaces set from source: navigator and the user agent,
[screen geometry](https://github.com/feder-cr/invisible_playwright/blob/main/docs/screen-size-headless-tells.md),
[WebGL vendor and renderer strings](https://github.com/feder-cr/invisible_playwright/blob/main/docs/webgl-renderer-strings.md)
with [their numeric limits](https://github.com/feder-cr/invisible_playwright/blob/main/docs/webgl-parameters-are-identical.md),
canvas pixel output, [the font set](https://github.com/feder-cr/invisible_playwright/blob/main/docs/bundled-fonts-cross-platform.md),
[the audio pipeline](https://github.com/feder-cr/invisible_playwright/blob/main/docs/audiocontext-fingerprinting.md),
[WebRTC ICE candidates](https://github.com/feder-cr/invisible_playwright/blob/main/docs/webrtc-ice-candidate-spoofing.md),
timezone, locale, and the proxy path.

Every value arrives through a preference, so nothing is hardcoded in C++ and one seed
reproduces one machine. What each of those surfaces reveals, and what it costs to get
right, is written up one at a time at
**[feder-cr.github.io/invisible_playwright](https://feder-cr.github.io/invisible_playwright/)**.

## Anonymous launch counter

This build issues a single HTTPS GET to a public asset on
[feder-cr/invisible_firefox](https://github.com/feder-cr/invisible_firefox/releases/tag/usage-counter)
every time the browser process starts. That asset's `download_count` is the global
launch counter in the badge above, which also carries the 1,162,836 launches counted
before 2026-07-22 (the asset was hosted elsewhere until then, and its count cannot be
carried over, so the badge adds the two).

What is sent:

- One `GET https://github.com/feder-cr/invisible_firefox/releases/download/usage-counter/launch.txt`
- A standard Firefox User-Agent. No identifiers, no payload, no cookies.

When it fires:

- At `final-ui-startup`, before any tab exists and before web content can observe the
  request from a page context.
- Fire and forget. Errors are swallowed and the browser never blocks on the network.

How to turn it off:

- **Per session**, set `invisible_firefox.usage_ping.enabled` to `false` in
  `about:config`, or pass
  `extra_prefs={"invisible_firefox.usage_ping.enabled": False}` to
  `InvisiblePlaywright()`.
- **Permanently**, remove the `STEALTHFOX: anonymous launch counter` block in
  [`browser/components/BrowserGlue.sys.mjs`](browser/components/BrowserGlue.sys.mjs)
  and the matching preference in
  [`browser/app/profile/firefox.js`](browser/app/profile/firefox.js), then rebuild.

Why it exists: the build is given away, and archive download counts both undercount
and overcount, because caches and mirrors hide real installs while CI re-pulls inflate
them. A per-launch ping is the closest honest signal of how many installs are real.

## Bugs and questions

File them in this repository's issue tracker. Please do not file them at Mozilla: this
is an unaffiliated fork, and the patches here are not theirs.

---

# Firefox (upstream)

This project is a fork of Firefox. It is not affiliated with or endorsed by Mozilla.
The upstream project's own README follows, because everything in it is still true of
the code this fork is built on.

![Firefox Browser](./docs/readme/readme-banner.svg)

[Firefox](https://firefox.com/) is a fast, reliable and private web browser from the non-profit [Mozilla organization](https://mozilla.org/).

### Contributing

To learn how to contribute to Firefox read the [Firefox Contributors' Quick Reference document](https://firefox-source-docs.mozilla.org/contributing/contribution_quickref.html).

Bugs in Firefox itself belong at [bugzilla.mozilla.org](https://bugzilla.mozilla.org/). Bugs in this fork do not; see above.

### Resources

* [Firefox Source Docs](https://firefox-source-docs.mozilla.org/) is the primary documentation repository
* Nightly development builds can be downloaded from the [Firefox Nightly page](https://www.mozilla.org/firefox/channel/desktop/#nightly)

If you have a question about developing Firefox, and can't find the solution
on [Firefox Source Docs](https://firefox-source-docs.mozilla.org/), you can try asking your question on Matrix at
chat.mozilla.org in the [Introduction channel](https://chat.mozilla.org/#/room/#introduction:mozilla.org).
