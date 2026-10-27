# automation/forced-offline

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `netwerk/protocol/http/nsHttpChannel.cpp` (nsHttpChannel::OnBeforeConnect; nsHttpChannel::MaybeUseHTTPSRRForUpgrade; nsHttpChannel::ContinueConnect; nsHttpChannel::SpeculativeConnect; nsHttpChannel::OpenCacheEntryInternal; nsHttpChannel::IsForcedOffline (new), already-hook): REGRESSION CONFIRMED: IsForcedOffline() (nsHttpChannel.cpp:8523) is a stub that returns false. The five call sites (OnBeforeConnect, MaybeUseHTTPSRRForUpgrade, ContinueConnect via a local, SpeculativeConnect, OpenCacheEntryInternal) no longer read bc->Top()->GetForceOffline(), so a forced-offline BrowsingContext no longer fails HTTP requests. The stub's own comment (reason: BrowsingContext ForceOf
- `netwerk/protocol/http/nsHttpChannel.h` (nsHttpChannel class, private helpers (after MaybeStartDNSPrefetch), already-hook): Declaration only. The definition in nsHttpChannel.cpp:8523 is a stub returning false and is a confirmed regression (BrowsingContext ForceOffline exists in 157, docshell/base/BrowsingContext.h:289); both should be reverted to Mozilla. The begin/end banner comments name a third-party project.
