# automation/juggler

Category: build
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `modules/libpref/moz.build` (pref_groups list, already-hook): Registers the 'juggler' pref group, so the juggler.* entries in StaticPrefList.yaml are built into the generated StaticPrefs headers. Pure registration.
- `toolkit/toolkit.mozbuild` (if CONFIG["ENABLE_WEBDRIVER"]: DIRS list (hunk header line 152), already-hook): Registers the Juggler directory when ENABLE_WEBDRIVER is set. The /juggler directory is our own code and is not in this list; Verified: C:/ff/source/juggler exists (moz.build, TargetRegistry.js, SessionRoute.js, components/).

## Our own files

- `juggler/ChannelEventSink.sys.mjs`
- `juggler/Helper.js`
- `juggler/JugglerFrameParent.jsm`
- `juggler/JugglerFrameParent.sys.mjs`
- `juggler/NetworkObserver.js`
- `juggler/SessionRoute.js`
- `juggler/SimpleChannel.js`
- `juggler/StealthCursor.js`
- `juggler/TargetRegistry.js`
- `juggler/components/Juggler.js`
- `juggler/components/components.conf`
- `juggler/components/moz.build`
- `juggler/content/FrameTree.js`
- `juggler/content/JugglerFrameChild.jsm`
- `juggler/content/JugglerFrameChild.sys.mjs`
- `juggler/content/PageAgent.js`
- `juggler/content/Runtime.js`
- `juggler/content/WorkerMain.js`
- `juggler/content/hidden-scrollbars.css`
- `juggler/content/main.js`
- `juggler/moz.build`
- `juggler/pipe/components.conf`
- `juggler/pipe/moz.build`
- `juggler/pipe/nsIRemoteDebuggingPipe.idl`
- `juggler/pipe/nsRemoteDebuggingPipe.cpp`
- `juggler/pipe/nsRemoteDebuggingPipe.h`
- `juggler/protocol/BrowserHandler.js`
- `juggler/protocol/Dispatcher.js`
- `juggler/protocol/PageHandler.js`
- `juggler/protocol/PrimitiveTypes.js`
- `juggler/protocol/Protocol.js`
- `juggler/screencast/components.conf`
- `juggler/screencast/moz.build`
- `juggler/screencast/nsIScreencastService.idl`
- `juggler/screencast/nsScreencastService.cpp`
- `juggler/screencast/nsScreencastService.h`
- `juggler/tests/bfcache_reattach.test.mjs`
- `juggler/tests/caller_world.test.mjs`
- `juggler/tests/crashed_tab_is_closed.test.mjs`
- `juggler/tests/frame_tree.test.mjs`
- `juggler/tests/input_waits_are_bounded.test.mjs`
- `juggler/tests/navigation_abort.test.mjs`
- `juggler/tests/not_a_chrome_package.test.mjs`
- `juggler/tests/request_body_is_always_read.test.mjs`
- `juggler/tests/response_tells_its_security_and_service_worker.test.mjs`
- `juggler/tests/runtime_ids_are_unique_per_session.test.mjs`
- `juggler/tests/shared_cookies_leave_with_their_owner.test.mjs`
- `juggler/tests/simple_channel.test.mjs`
- `juggler/tests/worker_debugger_manager_is_a_service.test.mjs`
