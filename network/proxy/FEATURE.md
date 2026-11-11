# network/proxy

Category: other
Prefs: network.proxy.type (Mozilla's own pref; its default is 0 here)
Test: juggler/tests/system_proxy_is_off_by_default.test.mjs (node; it reads the default on Mozilla's line and the code that follows from it)

## What it does

The engine does not read the host's proxy settings. `network.proxy.type` defaulted to 5 (system): the parent created the system
settings object when the proxy service started (the WinINet Internet Settings key, its change watcher, the `*_proxy` variables) and
asked it again for every channel, even when Juggler gives the proxy. The default is now 0 (direct): `PrefsChanged` never creates the
object and `Resolve_Internal` returns before any host read. Juggler's channel filter is unchanged. A profile or `extra_prefs` can
still set another type.

## Hooks in Mozilla files

- `modules/libpref/init/StaticPrefList.yaml`: `value: 0  # STEALTH-HOOK(network/proxy)` on Mozilla's own line of `network.proxy.type`
  (1 line added, 1 rewritten). The file has one owner, so its hunk is carried by fingerprint/webgpu/adapter-declaration (which owns the
  file's other hunk); this folder owns only the test below.

## Our own files

- `juggler/tests/system_proxy_is_off_by_default.test.mjs`
