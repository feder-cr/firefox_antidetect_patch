# automation/network-interception

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `netwerk/base/nsINetworkInterceptController.idl` (interface nsIInterceptedChannel (after resetInterception), already-hook): Two Playwright methods added between the Playwright begin/end markers. Declarations only. The earlier claim that nothing implements them is REFUTED: netwerk/protocol/http/InterceptedHttpChannel.cpp:733 defines InterceptedHttpChannel::InterceptAfterServiceWorkerResets and :739 defines ResetInterceptionWithURI (member mInterceptAfterServiceWorkerResets at InterceptedHttpChannel.h:94; declarations co
- `netwerk/protocol/http/InterceptedHttpChannel.cpp` (InterceptedHttpChannel::ResetInterception (new InterceptAfterServiceWorkerResets and ResetInterceptionWithURI); InterceptedHttpChannel::OnStartRequest, movable): Hunk 1 adds an 18-line state machine (two new NS_IMETHODIMPs plus a branch in ResetInterception). Move the bodies to our own file and keep a 2-line hook in ResetInterception. Hunk 2 wraps the active MOZ_ASSERT_IF and MOZ_DIAGNOSTIC_ASSERT (loading principal / service-worker tainting; two assert macros) in a block comment (0 added logic lines): keep as a documented Juggler-only deviation or move be
- `netwerk/protocol/http/InterceptedHttpChannel.h` (InterceptedHttpChannel class members (after mInterceptionReset), already-hook): Member declarations only (0 logic lines). A Mozilla class must hold this state, but one pointer to our own state struct would replace both lines with one. The comment block names a third-party project; rewrite it when moving the code. The members back the methods declared in netwerk/base/nsINetworkInterceptController.idl:65,68 (not a missing declaration).
