# automation/navigation

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `docshell/base/CanonicalBrowsingContext.cpp` (CanonicalBrowsingContext::LoadURI; CanonicalBrowsingContext::FixupAndLoadURIString, movable): The same 11-line observer block is pasted into both functions. Each block is about 10 logic lines, so it moves into one helper in our own file called from both places. The notification fires before LoadURI(loadState, true) and carries the load identifier as its string payload. Nothing in Mozilla listens for it; only Juggler does.
- `docshell/base/nsDocShell.cpp` (nsDocShell::nsDocShell (constructor); nsDocShell::Get/SetFileInputInterceptionEnabled; nsDocShell::Get/SetBypassCSPEnabled; nsDocShell::InternalLoad; NotifyJugglerLinkClick (static); OnLinkClickEvent::Run / Cancel / NotifyJugglerDone; nsDocShell::OnLinkClickWithLoadState, movable): Three features share this file (the notes said four, but only three are listed); feature field is the largest. (a) juggler-docshell-attrs, hunks 1-2: constructor init of the two bit-fields (must stay, fixes uninitialised bits) and the four accessors (about 16 logic lines, movable to an own docshell source file). (b) juggler-navigation-events, hunk 3: InternalLoad notifies juggler-navigation-starte
- `netwerk/base/LoadInfo.cpp` (LoadInfo::LoadInfo(const LoadInfo& rhs) (copy-constructor init list); LoadInfo::GetJugglerLoadIdentifier / SetJugglerLoadIdentifier appended after LoadInfo::UpdateParentAddressSpaceInfo, already-hook): Implements the nsILoadInfo.jugglerLoadIdentifier attribute from nsILoadInfo.idl. The copy constructor carries the value. In this diff only DocumentLoadListener.cpp sets it. The accessors must stay in LoadInfo because it implements the interface.
- `netwerk/base/LoadInfo.h` (class LoadInfo final (member block after mSkipHTTPSUpgrade), already-hook): Member declaration only. Zero is the default, so loads that never receive an id report 0.
- `netwerk/base/TRRLoadInfo.cpp` (TRRLoadInfo::GetJugglerLoadIdentifier / TRRLoadInfo::SetJugglerLoadIdentifier (end of file), already-hook): Stubs required because TRRLoadInfo implements nsILoadInfo. Risk 'infallible attribute returns NS_ERROR_NOT_IMPLEMENTED': REFUTED as a new problem. Upstream TRRLoadInfo.cpp:132-155 already returns NS_ERROR_NOT_IMPLEMENTED for the [infallible] attributes securityMode, isInThirdPartyContext and isThirdPartyContextToTopWindow (nsILoadInfo.idl:490, 637, 649); the new stubs follow the same pattern.
- `netwerk/base/nsILoadInfo.idl` (interface nsILoadInfo (end of interface, after the %{ C++ block), already-hook): IDL attribute declaration only. Declared [infallible]; TRRLoadInfo returns NS_ERROR_NOT_IMPLEMENTED for it, which is the existing upstream pattern for other [infallible] attributes (TRRLoadInfo.cpp:132-155), so not a new risk.
- `netwerk/ipc/DocumentLoadListener.cpp` (CreateDocumentLoadInfo (static function), already-hook): Copies the load identifier from the load state to the LoadInfo of each document load. GetLoadIdentifier confirmed at C:/ff/source/docshell/base/nsDocShellLoadState.h:365 (returns mLoadIdentifier). The only place in this diff that sets the identifier.
