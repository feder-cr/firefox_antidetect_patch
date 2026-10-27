# fingerprint/locale-time/timezone

Category: other, spoof
Prefs: juggler.locale.override, juggler.timezone.override
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `docshell/base/BrowsingContext.cpp` (BrowsingContext::Attach; BrowsingContext::DidSet(FieldIndex<IDX_LanguageOverride>); BrowsingContext::DidSet(FieldIndex<IDX_TimezoneOverride>), movable): Attach seeds the timezone and language fields from the session (parent process, top-level only); about 14 logic lines in one hunk, so a one-call hook into our own file is the target. DidSet now reads the declared value (field, else session) instead of the field alone, so a cleared field falls back to the session value in every process. The pref names come from the comments; the reads happen in int
- `dom/base/Document.cpp` (Document::InitCSP; static GetFormattedTimeString (called from Document::GetLastModified and Document::RetrieveRelevantHeaders); Document::ComputeVisibilityState, already-hook): Four unrelated changes in one file. (1) spoof, feature timezone-declared: document.lastModified formatting now uses the declared zone via ExplodeLocalTime (defined in dom/base/StealthSessionDeclaration.h, ours); the UTC branch for RFP is gone. (2) juggler, feature csp-bypass: InitCSP returns early when CSP_IsBypassedFor(*this) (defined in dom/security/nsCSPUtils.cpp, another group). Give it its ow
- `dom/ipc/ContentChild.cpp` (ContentChild::RecvSetXPCOMProcessAttributes; ContentChild::RecvSystemTimezoneChanged, already-hook): Comment-only: no code added or removed. The first comment records removal of a juggler.timezone.override block (it never ran); the second is a note on RecvSystemTimezoneChanged. The patch should drop these comments; the zone now comes from StealthSessionDeclaration (dom/base, not in this group).
- `dom/xslt/xslt/txEXSLTFunctions.cpp` (txEXSLTFunctionCall::evaluate (date:date-time branch), already-hook): Replaces PR_ExplodeTime with ExplodeLocalTime (our dom/base/StealthSessionDeclaration.h). The removed code chose PR_GMTParameters when JSDateTimeUTC was resisted; the new call does not reference that flag. [verified: REFUTED, ExplodeLocalTime (dom/base/StealthSessionDeclaration.cpp:95-98) checks ShouldResistFingerprinting(JSDateTimeUTC) and uses PR_GMTParameters, then falls back to PR_LocalTimePar
- `js/xpconnect/src/nsXPConnect.cpp` (InitGlobalObjectOptions, already-hook): Page-observable: the realm's timezone and default locale. The Mozilla RFP calls (nsRFPService::GetSpoofedJSTimeZone and GetSpoofedJSLocale) are removed from this function and the nsRFPService.h include is gone; the zone/locale rule now lives in dom/base/StealthSessionDeclaration (ours, not in this group). aForceUTC and aLocaleEnUS still come from Mozilla callers and keep their meaning as flags. Th
