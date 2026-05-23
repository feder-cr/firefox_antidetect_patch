# invisible_firefox

[![browser launches](https://img.shields.io/github/downloads/feder-cr/invisible_firefox/usage-counter/launch.txt?label=browser%20launches&color=blue)](https://github.com/feder-cr/invisible_firefox/releases/tag/usage-counter)

Patched build of Firefox 150 used as the binary backend of the [invisible_playwright](https://github.com/feder-cr/invisible_playwright) Python wrapper. Branch `stealth/150` is the source of truth; see [STEALTH_BRANCH_README.md](STEALTH_BRANCH_README.md) for the patch series.

## Anonymous launch counter

This build issues a single HTTPS GET to a public asset on this repo every time the browser process starts. The asset's `download_count` becomes the global launch counter (badge above).

What's sent:
- One `GET https://github.com/feder-cr/invisible_firefox/releases/download/usage-counter/launch.txt`
- Standard Firefox User-Agent. No identifiers, no payload, no cookies.

When it fires:
- At `final-ui-startup`, before any tab is created and before any web content can observe the request from its page context.
- Fire-and-forget: errors are swallowed, the browser never blocks on the network.

How to disable:
- **Per session** — set the pref `invisible_firefox.usage_ping.enabled` to `false` via `about:config`, or pass `extra_prefs={"invisible_firefox.usage_ping.enabled": false}` to `InvisiblePlaywright()`.
- **Permanently** — remove the `STEALTHFOX: anonymous launch counter` block in [`browser/components/BrowserGlue.sys.mjs`](browser/components/BrowserGlue.sys.mjs) plus the matching pref in [`browser/app/profile/firefox.js`](browser/app/profile/firefox.js), then rebuild from source.

Why: the build is distributed for free and the maintainer wants a rough sense of how many real installs there are. Download counts of the binary archives undercount (caches, mirrors) and overcount (CI re-pulls) — a per-launch ping is the closest signal.

---

# Firefox (upstream)

![Firefox Browser](./docs/readme/readme-banner.svg)

[Firefox](https://firefox.com/) is a fast, reliable and private web browser from the non-profit [Mozilla organization](https://mozilla.org/).

### Contributing

To learn how to contribute to Firefox read the [Firefox Contributors' Quick Reference document](https://firefox-source-docs.mozilla.org/contributing/contribution_quickref.html).

We use [bugzilla.mozilla.org](https://bugzilla.mozilla.org/) as our issue tracker, please file bugs there.

### Resources

* [Firefox Source Docs](https://firefox-source-docs.mozilla.org/) is our primary documentation repository
* Nightly development builds can be downloaded from [Firefox Nightly page](https://www.mozilla.org/firefox/channel/desktop/#nightly)

If you have a question about developing Firefox, and can't find the solution
on [Firefox Source Docs](https://firefox-source-docs.mozilla.org/), you can try asking your question on Matrix at
chat.mozilla.org in the [Introduction channel](https://chat.mozilla.org/#/room/#introduction:mozilla.org).
