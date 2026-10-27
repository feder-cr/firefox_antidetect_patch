# automation/hybrid-xrays

Category: juggler, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `js/xpconnect/src/Sandbox.cpp` (SandboxProxyHandler::getPropertyDescriptorImpl; xpc::CreateSandboxObject; SandboxOptions::Parse, movable): Support for Juggler's caller world only (the hybridXrays sandbox option, which only Juggler passes); the page-visible effect lives in WrapperFactory.cpp (spoof). Movable parts: the hybridXrays validation (about 7 lines) and the locale block (about 8 lines). The locale block reimplements the primary-tag step (FindChar, Truncate, Trim) instead of calling intl::PrimaryLanguageTag, which BrowsingConte
- `js/xpconnect/src/XPCJSRuntime.cpp` (CompartmentPrivate::CompartmentPrivate, already-hook): Initialiser for the new CompartmentPrivate member. It must stay in declaration order (right after wantXrays in xpcprivate.h), or the compiler warns about reordered initialisers.
- `js/xpconnect/src/xpcprivate.h` (SandboxOptions::SandboxOptions (constructor); class SandboxOptions (member); CompartmentPrivate::CanShareCompartmentWith; class CompartmentPrivate (member), already-hook): Declarations for the hybridXrays flag on SandboxOptions and CompartmentPrivate. The one real Mozilla logic change is in CanShareCompartmentWith: a hybrid compartment is never shared. Comments refer to rule text in XrayWrapper.h.
- `js/xpconnect/wrappers/WrapperFactory.cpp` (DEBUG_CheckUnwrapSafety; SelectHybridWrapper (new, before WrapperFactory::Rewrap); CallerObjectWrapper::getPrototype / getPrototypeIfOrdinary (new); WrapperFactory::Rewrap, hard): Page-observable: an object the caller's world hands to the page keeps its identity, but its standard prototypes are the page's own (instanceof Array holds, constructor is the page's Function). The new CallerObjectWrapper class (about 60 lines) and CallerIntrinsicKey could move to our own file behind a declaration. Hard because Rewrap's own predicates are edited in place: wantXrays is replaced by o
- `js/xpconnect/wrappers/XrayWrapper.cpp` (new code after XrayWrapper<Base,Traits>::getPropertyKeys: IsHybridXrayWrapper, HybridXrayWrapper<Traits> methods, IsStandardLevel, NativeOwn, ChainLookup, ContentLookup, HybridXrayGetBareName, ContentGet, movable): Single pure addition of about 460 lines in one place, with no edits to existing Mozilla code, so it moves to our own file. Access needed: IsTypedArrayKey is file-local (inline, XrayWrapper.cpp:77), so it must be exposed first. ensureHolder (XrayWrapper.h:102), getProtoKey (:266) and isPrototype (:271) are already public members, so they need no change (the first pass said they must be made reachab
- `js/xpconnect/wrappers/XrayWrapper.h` (class XrayWrapper (new protected constructor); new class HybridXrayWrapper; free functions IsHybridXrayWrapper and HybridXrayGetBareName (declarations), movable): Only the protected constructor added to XrayWrapper must stay in this Mozilla header. The new class declarations and the design comment (about 70 lines of comment, which states the name rule) could move to our own header included from here. The hunk adds a bare 'public:' followed by a blank line (cosmetic).
