# automation/juggler

Category: build
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## The proxy of the session (juggler/SessionRoute.js)

`newProxyInfoFor` builds the nsIProxyInfo of a proxy record once, when the proxy is set (that call throws for a record that cannot be
expressed), and the channel filter gives the same object on every request: a filter that threw would end the request DIRECT, because
necko skips a filter that throws. The info does not tell the proxy to resolve the host: the browser resolves names itself (DoH,
network/doh) and connects to the IP.

The proxy filter starts pending: channels asked before the session's proxy is decided (`Browser.setBrowserProxy` or
`Browser.setNoBrowserProxy`) wait unanswered, `EarlyProxyHold.js` holds them from command-line-startup, and `TargetRegistry.roadOfSession`
tells the parent whether the road is decided (the pingsender at exit waits for it). An http/https proxy with credentials carries a
preemptive Basic `Proxy-Authorization` for the DoH CONNECT. Why and tests: network/doh.

## Hooks in Mozilla files

- `modules/libpref/moz.build` (pref_groups list, already-hook): Registers the 'juggler' pref group, so the juggler.* entries in StaticPrefList.yaml are built into the generated StaticPrefs headers. Pure registration.
- `toolkit/toolkit.mozbuild` (if CONFIG["ENABLE_WEBDRIVER"]: DIRS list (hunk header line 152), already-hook): Registers the Juggler directory when ENABLE_WEBDRIVER is set. The /juggler directory is our own code and is not in this list; Verified: C:/ff/source/juggler exists (moz.build, TargetRegistry.js, SessionRoute.js, components/).
- `dom/base/nsFrameLoader.cpp` (nsFrameLoader::MaybeNotifyCrashed, already-hook): the notification `oop-frameloader-crashed` carries the id of the BrowsingContext that crashed (`StealthCrashedContextData`, in our `dom/base/StealthFrameCrash.h`; null when the caller does not say). An out-of-process iframe's BrowserParent has the tab's owner element, so the notification's frameloader is the tab's for the crash of an iframe too, and TargetRegistry.js closed the tab for it; with the id it tells the two apart. 3 lines and the include.

## One process per site (Fission)

A page is held by one content process per fragment root: the document of the top, and every frame whose parent is in
another process. The parent reaches them through `ContentMux.js`: one channel per root, the commands routed by
`ROUTES` (by execution context, by frame, by the parent of a frame, to the main root, or to every root), what comes
back fanned in with the root it came from. `FrameLifecycle.js` is the parent's record of the frames of a page (one
`Page.frameAttached` / `frameDetached` per frame, the gate of the frame events). `FrameNavigations.js` keeps the navigations of the frames of a page across the roots that hold them. A crash of an iframe's process leaves
the tab (`TargetRegistry.js`, with the data of the notification above).

Tests (node, no browser): `juggler/tests/content_mux.test.mjs`, `content_mux_frame_routes.test.mjs`,
`fragment_root_agents.test.mjs`, `fragment_root_frames.test.mjs`, `frame_lifecycle.test.mjs`,
`frame_lifecycle_reannounce.test.mjs`, `page_handler_frame_gate.test.mjs`,
`page_handler_wires_the_frame_lifecycle.test.mjs`, `oop_frame_element.test.mjs`, `oop_frame_geometry.test.mjs`,
`crashed_iframe_keeps_the_tab.test.mjs`. Run: `node --test juggler/tests/*.test.mjs`.

## Our own files

- `dom/base/StealthFrameCrash.h`
- `juggler/ChannelEventSink.sys.mjs`
- `juggler/ContentMux.js`
- `juggler/FrameLifecycle.js`
- `juggler/FrameNavigations.js`
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
- `juggler/pipe/StealthJugglerGate.cpp`
- `juggler/pipe/StealthJugglerGate.h`
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
- `juggler/tests/_content_main.mjs`
- `juggler/tests/bfcache_for_each_root.test.mjs`
- `juggler/tests/bfcache_reattach.test.mjs`
- `juggler/tests/caller_world.test.mjs`
- `juggler/tests/content_mux.test.mjs`
- `juggler/tests/content_mux_frame_routes.test.mjs`
- `juggler/tests/content_mux_retry.test.mjs`
- `juggler/tests/content_mux_roots_end.test.mjs`
- `juggler/tests/content_root_skips_settings_it_does_not_apply.test.mjs`
- `juggler/tests/context_ends_leaves_no_process.test.mjs`
- `juggler/tests/cookies_are_set_as_asked_or_refused.test.mjs`
- `juggler/tests/crashed_iframe_keeps_the_tab.test.mjs`
- `juggler/tests/crashed_tab_is_closed.test.mjs`
- `juggler/tests/describe_node_too_deep.test.mjs`
- `juggler/tests/dispose_leaves_nothing_of_the_documents.test.mjs`
- `juggler/tests/docshell_settings_follow_every_local_docshell.test.mjs`
- `juggler/tests/download_complete_follows_the_idl.test.mjs`
- `juggler/tests/explicit_size_flag_is_free.test.mjs`
- `juggler/tests/fission_3_review.test.mjs`
- `juggler/tests/fragment_root_agents.test.mjs`
- `juggler/tests/fragment_root_frames.test.mjs`
- `juggler/tests/frame_lifecycle.test.mjs`
- `juggler/tests/frame_lifecycle_reannounce.test.mjs`
- `juggler/tests/frame_navigations.test.mjs`
- `juggler/tests/frame_tree.test.mjs`
- `juggler/tests/frame_tree_fanout_needs_a_window.test.mjs`
- `juggler/tests/fullscreen_releases_the_viewport.test.mjs`
- `juggler/tests/geolocation_reaches_every_root.test.mjs`
- `juggler/tests/held_requests_end_with_their_request.test.mjs`
- `juggler/tests/input_review_findings.test.mjs`
- `juggler/tests/input_waits_are_bounded.test.mjs`
- `juggler/tests/javascript_disabled_is_the_parents.test.mjs`
- `juggler/tests/keys_go_to_the_focused_root.test.mjs`
- `juggler/tests/media_feature_refusals_name_the_knob.test.mjs`
- `juggler/tests/mouse_ack_comes_from_any_root.test.mjs`
- `juggler/tests/navigation_abort.test.mjs`
- `juggler/tests/navigation_refused_before_a_channel.test.mjs`
- `juggler/tests/not_a_chrome_package.test.mjs`
- `juggler/tests/oop_frame_element.test.mjs`
- `juggler/tests/oop_frame_geometry.test.mjs`
- `juggler/tests/page_handler_frame_gate.test.mjs`
- `juggler/tests/page_handler_wires_the_frame_lifecycle.test.mjs`
- `juggler/tests/page_target_location_filter.test.mjs`
- `juggler/tests/request_body_is_always_read.test.mjs`
- `juggler/tests/response_bodies_follow_the_navigation.test.mjs`
- `juggler/tests/response_tells_its_security_and_service_worker.test.mjs`
- `juggler/tests/review_findings_workers.test.mjs`
- `juggler/tests/runtime_debugger_module_url.test.mjs`
- `juggler/tests/runtime_ids_are_unique_per_session.test.mjs`
- `juggler/tests/screenshot_releases_its_canvas.test.mjs`
- `juggler/tests/screenshot_retry_is_bounded.test.mjs`
- `juggler/tests/scroll_into_view_if_needed_works.test.mjs`
- `juggler/tests/shared_cookies_leave_with_their_owner.test.mjs`
- `juggler/tests/simple_channel.test.mjs`
- `juggler/tests/websocket_listener_follows_the_idl.test.mjs`
- `juggler/tests/wheel_waits_for_its_delivery.test.mjs`
- `juggler/tests/worker_debugger_manager_is_a_service.test.mjs`
- `juggler/tests/worker_ends_with_its_frame.test.mjs`
- `juggler/tests/worker_follows_the_document_in_the_cache.test.mjs`
