# fingerprint/js-surface/branding

Category: build, other, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `chrome/nsChromeRegistry.cpp` (nsChromeRegistry::ConvertChromeURL, movable): The resolved chrome:// URI is passed through the declared-branding redirect. zoom.stealth.branding.archive is read indirectly: StealthBrandingRedirect (netwerk/protocol/res/StealthBranding.cpp:99) reads StaticPrefs::zoom_stealth_branding_archive at StealthBranding.cpp:42 (CONFIRMED); no pref appears in the added lines here. The ~13 logic lines (resolve, compare, redirect, rebuild) should move behi
- `chrome/nsChromeRegistryChrome.cpp` (nsChromeRegistryChrome::ManifestContent, already-hook): Records each package's base directory for the redirect in nsChromeRegistry.cpp. The callee is in StealthBranding.h (not in this list).
- `chrome/nsChromeRegistryContent.cpp` (nsChromeRegistryContent::RegisterPackage, already-hook): Content-process counterpart of the registration above; skipped when content is null.
- `netwerk/protocol/res/SubstitutingProtocolHandler.cpp` (SubstitutingProtocolHandler::ResolveURI, already-hook): Two hooks: the special-case branch and the normal resolution path, so resource://usercontext-content/branding/ reads the declared archive too (same redirect as chrome://branding/content/ in chrome/nsChromeRegistry.cpp:281). Category is spoof, not other: netwerk/protocol/res/StealthBranding.h states a page can read branding files through both URL schemes (contentaccessible) and the contents tell a 
- `netwerk/protocol/res/moz.build` (EXPORTS.mozilla.net and UNIFIED_SOURCES lists, already-hook): Registers our own files, both present in C:\ff\source\netwerk\protocol\res. No logic.
- `security/sandbox/linux/broker/SandboxBrokerPolicyFactory.cpp` (SandboxBrokerPolicyFactory::InitContentPolicy, already-hook): Gives content processes READ access to the declared branding archive (StealthBrandingDeclaredArchive). The grant is silently skipped when that call returns null or GetNativePath fails. The 5 logic lines are at the already-hook limit.
- `security/sandbox/win/src/sandboxbroker/sandboxBroker.cpp` (SandboxBroker::SetSecurityLevelForContentProcess, movable): Windows counterpart of the Linux broker change. Its 8-line block should move into one function in our own file (for example AddDeclaredBrandingReadRule(config)), leaving a one-line call. It reuses the existing 'result' variable of the function, which is already used throughout it.

## Our own files

- `netwerk/protocol/res/StealthBranding.cpp`
- `netwerk/protocol/res/StealthBranding.h`
