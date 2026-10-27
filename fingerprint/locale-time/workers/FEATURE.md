# fingerprint/locale-time/workers

Category: other, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/serviceworkers/ServiceWorkerPrivate.cpp` (ServiceWorkerPrivate::Initialize (remote worker data), new static ContextDeclaration, ServiceWorkerPrivate::RefreshRemoteWorkerData, movable): ContextDeclaration (about 35 lines) walks every BrowsingContextGroup on each spawn and compares language and zone across the top-level pages of the user context. Move it to our file. Cost: it runs on every RefreshRemoteWorkerData, so each service-worker spawn scans all groups. [verified: CONFIRMED, callers are ServiceWorkerPrivate.cpp:820 (Initialize) and :1388 (spawn), definition :1983; a main-th
- `dom/workers/WorkerLoadInfo.h` (struct WorkerLoadInfoData (comment after mTimezoneOverride), already-hook): Comment only, no code. It belongs in our docs rather than the Mozilla file. It refers to intl/locale/StealthSessionPrefs.h, which is our file.
- `dom/workers/WorkerPrivate.cpp` (WorkerPrivate::WorkerPrivate (main-thread branch, hunk @@ -2972), WorkerPrivate::UpdateLanguageOverrideInternal (@@ -6350), WorkerPrivate::UpdateLanguagesInternal (@@ -6396), movable): Each hunk is at most 5 logic lines, so each is already a hook. The constructor still calls Navigator::GetAcceptLanguages inline. Behaviour change: UpdateLanguagesInternal ignores intl.accept_languages for workers that follow an owner's LanguageOverride. The session prefs are read by intl/locale/StealthSessionPrefs.h (not in this group), so prefs is empty here. A comment block of about 30 lines car
- `dom/worklet/WorkletGlobalScope.cpp` (WorkletGlobalScope::CreateRealmOptions, already-hook): One call to SetRealmTimeAndLanguage (dom/base/StealthSessionDeclaration.h, our file) replaces the two resistFingerprinting branches. The RFP flags are passed in as arguments. The 4 added lines are one multi-line call, counted as lines. The worklet thread reads LoadInfo().Language() and Timezone(), which are set on the main thread (WorkletImpl). [verified: SetRealmTimeAndLanguage (dom/base/StealthS
- `dom/worklet/WorkletImpl.cpp` (WorkletLoadInfo::WorkletLoadInfo(nsPIDOMWindowInner*), already-hook): The nsPIDOMWindowInlines.h include is a port-fix, needed only on 157 (the comment says 151 linked by accident). The rest of the file is spoof. Split the include into a port-fix patch if the series is cut by category.
- `dom/worklet/WorkletImpl.h` (class WorkletLoadInfo (accessors and members), already-hook): Two members and their accessors, no logic. Also adds an nsString.h include. The comment says the values are set at construction and never modified, and may be read from any thread.
