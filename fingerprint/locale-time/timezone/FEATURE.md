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

## dom/base/Document.cpp: Document::InitCSP

Hook: after the CSP context is set up, `if (CSP_IsBypassedFor(*this)) return NS_OK;`
(CSP_IsBypassedFor is declared in dom/security/nsCSPUtils.h and defined in dom/security/StealthBypassCSP.cpp; feature automation/bypass-csp).

The CSP context set up earlier in InitCSP (before the CSP_IsBypassedFor early return) stays
even when the automation bypasses the page's policies: history entries and a later <meta>
expect one. What is skipped is every policy the page itself sends.

## dom/base/Document.cpp: GetFormattedTimeString

STEALTHFOX (firefox-50): the local time of the page, the declared zone's
(ExplodeLocalTime), not the C runtime's ([B339]).

The static function takes the Document instead of the `aUniversal` flag; its callers
Document::GetLastModified and Document::RetrieveRelevantHeaders pass `*this`.
ExplodeLocalTime is defined in dom/base/StealthSessionDeclaration.h/.cpp.

## dom/base/Document.cpp: Document::ComputeVisibilityState

In the branch that returns Hidden when the document is not visible, has no window, or its
outer window is in the background:

Stealth: GetForceActiveState removed in 150. The 146 hack relied on
it to report Visible regardless of OS focus, defeating a commercial
fingerprinting service's detection via document.hidden. TODO:
re-implement via a different mechanism (e.g. JS-side override or
pref-gated unconditional Visible).

## dom/ipc/ContentChild.cpp: ContentChild::RecvSetXPCOMProcessAttributes

At the end of the function, after the TRR mode block (no code of ours is left there):

STEALTHFOX (firefox-50): the block that applied `juggler.timezone.override`
to every realm of this process is gone. It never ran - the pref had no
default, so it arrived empty here - and from firefox-50 it would have:
the pref has a default (modules/libpref/init/StealthPrefList.yaml) so the session's zone
reaches content processes, and one zone stamped on every realm of a
process would have overridden a context's own. Every realm takes its zone
at birth from its context (dom/base/StealthSessionDeclaration.h), and
BrowsingContext::DidSet carries a change to the realms already alive.

## dom/ipc/ContentChild.cpp: ContentChild::RecvSystemTimezoneChanged

Stealth: SetTimeZoneOverride was removed in 150. Just reset like vanilla.
(The function only calls nsJSUtils::ResetTimeZone().)
