# platform/build-tools

Category: build, port-fix
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `python/mozbuild/mozbuild/backend/recursivemake.py` (class RecursiveMakeBackend, the objs_ref assignment near line 1607 (enclosing def name not extracted), already-hook): Applies to every platform and every build, not only Win32: relative object paths are replaced by absolute ones unconditionally. The comment blames mozmake 4.4.1 on Win32 for paths over about 150 chars; that claim is the author's and was not checked here. Could be a helper call in our own file, but a one-line change is already small.
- `python/mozbuild/mozbuild/frontend/data.py` (UnifiedSources.__init__ (class UnifiedSources, hunk header line 1181), already-hook): Magic number change (20 to 30). It changes unified source file names for all builds, so generated unified file names change even outside Windows. The comment cites collisions in libwebrtc (FF150) as the reason; the collision claim was not verified here.
- `python/mozbuild/mozbuild/jar.py` (JarMaker.getOutput (def getOutput(self, name, mode="wb"), line 557), already-hook): UNSURE between build and port-fix: the comment blames long CSS file names overflowing MAX_PATH, with no version mention. It is packaging code, so build is used. The long-path prefix is applied to every output of getOutput on Windows, not only to the webcompat files.
- `third_party/python/vsdownload/vsdownload.py` (_downloadPayload (def at line 634), plus import signal at the top of the file, movable): Per-file SIGALRM watchdog for slow downloads in the cross-compile build. The comment ties it to a flaky CDN, not to a Firefox version, so it is build and not port-fix. On native Windows, SIGALRM does not exist and the watchdog is skipped; the comment says main() sets a socket default timeout instead. Process-wide signal handler: not safe in threaded use. Move the watchdog into an own helper and ke
