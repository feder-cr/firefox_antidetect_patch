# Firefox Stealth — branch `stealth/150`

Questo branch (`stealth/150`) sul fork `feder-cr/firefox` di `mozilla-firefox/firefox` è il **source of truth git-native** per gli stealth patches applicati a Firefox 150.0.1.

## Stato (2026-05-19)

- **Base**: `stealth-base/v150.0.1` → tag `FIREFOX_150_0_1_RELEASE` di `mozilla-firefox/firefox` (`ec9aaac47d4c...`)
- **HEAD**: branch tip (`6ca01bfe0ca9...`)
- **Commit ahead**: 25 commit sopra `stealth-base/v150.0.1`
- **Status build**: ✅ COMPILE pulito, ✅ RUNTIME funziona — `firefox.exe` + Playwright + Juggler pipe + new_page + page.mouse.* tutti passano sul clean build. Gap residuo: locale="en-US" via `Browser.setLocaleOverride` causa crash del content process dopo ~30s (workaround: `locale=""` su `InvisiblePlaywright`).

### Riassunto fix recenti (C1-C7)

| Gap | Commit | Cosa |
|---|---|---|
| C1+C2 | `8feb68c7dc22` | re-land setDownloadInterceptor (IDL + cpp + .h member) |
| C4 | `2495f9447cd9` | re-land 5 nsIDocShell stealth attributes (fileInputInterceptionEnabled, overrideHasFocus, bypassCSPEnabled, forceActiveState, disallowBFCache) |
| C5 | `951efca4f9f6` | port FF146 launcher+wmain juggler-pipe handle inheritance: senza questo il pipe Playwright→browser si disconnette immediatamente |
| C6 | `bec3da1e65e2` | port juggler-navigation-started observer notifications in nsDocShell.cpp + CanonicalBrowsingContext.cpp: senza questo Page.ready non viene mai emesso e ctx.new_page() hang |
| C7 (partial) | `6ca01bfe0ca9` | storage-only stub per nsIDocShell.languageOverride; il port completo richiede BrowsingContext FIELD + ICU/JS locale reset (multi-file, da fare poi) |

## Storia del branch

```
2f0dbec  fix(juggler): port 7 dist hand-patches into source (FF150 Fission, screencast stub, try/catch)
619db115 fix(juggler): disable screencast + try/catch on screencastService (FF150 libwebrtc gap)
eb135ac7 build: add missing libwebrtc/api/location.h + nICEr ice_component.c (FF150 patch series gap)
86f82412 fix(juggler): re-land jugglerSendMouseEvent C++ for FF150 (issue #9)
[+15 commit, applicazione di 0001-build-infra.patch ... 0015-storage-quota.patch dal repo feder-cr/firefox-stealth]
ec9aaac4 [stealth-base/v150.0.1] FF150_0_1_RELEASE vanilla mozilla-firefox
```

## Workflow per fare nuove modifiche

### 1. Clone questo branch
```bash
git clone -b stealth/150 https://github.com/feder-cr/firefox.git firefox-source
cd firefox-source
```

### 2. Sviluppo: edita, commit, push
```bash
# Modifica i file che servono
git add <files>
git commit -m "fix(area): descrizione"
git push origin stealth/150
git tag -f stealth-head/v150.0.1 HEAD
git push origin stealth-head/v150.0.1 --force
```

### 3. Build (~30-60 min cold, 5-30min incremental)
```bash
./mach.cmd bootstrap --no-system-changes --application-choice=browser  # solo prima volta
./mach.cmd build
```

### 4. Test
- E2E mouse suite: `release/stealthfox/tests/test_mouse.py` (12 cases)
- Stealth: CreepJS, FP Pro, sannysoft, reCAPTCHA

### 5. Quando branch produce build pulito + test verde, rigenera patch series

```bash
git format-patch stealth-base/v150.0.1..stealth/150 -o ../firefox-stealth/
# Manualmente: rinomina e bucket-grouping nei 15 .patch numerati per compat con README esistente
cd ../firefox-stealth
git add 000*.patch
git commit -m "regen from stealth/150 branch"
git push origin main
```

## Gap residui per build pulito che FUNZIONA a runtime

I gap C1-C7 sono stati portati. L'unico gap RIMASTO è il completamento di C7.

### C7. (Partial → completare) `languageOverride` BrowsingContext FIELD
La stub committata in `6ca01bfe0ca9` mantiene `docShell.languageOverride = locale` no-op (storage only). Il fix completo richiede:
- BrowsingContext FIELD `LanguageOverride` (con `DidSet`/`CanSet` callbacks) per propagazione cross-process
- `SetIcuLocale(aLanguageOverride)` callback con `icu::Locale::setDefault`, `JS_ResetDefaultLocale`, `ResetDefaultLocaleInAllWorkers`
- Multi-file: `BrowsingContext.{h,cpp}`, `CanonicalBrowsingContext.cpp` (txn.SetLanguageOverride), `nsDocShell.cpp` (call SetIcuLocale)

Sintomo del gap C7: `Browser.setLocaleOverride` con `locale="en-US"` (default di `InvisiblePlaywright`) causa crash del content process dopo ~30 secondi (GPU init timeout). Workaround: passare `locale=""` esplicitamente.

### C3. (Opzionale) Screencast: `HeadlessWindowCapturer` + `VideoCaptureModuleEx`
Richiede patch a `third_party/libwebrtc` per i tipi `VideoCaptureModuleEx` + `RawFrameCallback`. Complicato perché mozilla potrebbe averli rimossi da upstream libwebrtc. Per ora **DISABLED** (stub no-op in `juggler/TargetRegistry.js`).

## Cosa fa stealth/150 attualmente

- ✅ Compile pulito (no errors, 1 warning preesistente upstream)
- ✅ `jugglerSendMouseEvent` C++ proper landato (issue #9 fix)
- ✅ `juggler/` correttamente hooked nel build (toolkit.mozbuild DIRS)
- ✅ FF150 Fission cross-process navigation handlers (FrameTree.js, JugglerFrameParent.sys.mjs)
- ✅ `setDownloadInterceptor` IDL + cpp + .h member (C1+C2)
- ✅ `nsIDocShell` 5 stealth attributes (C4)
- ✅ Launcher → child juggler-pipe handle inheritance (C5)
- ✅ Juggler navigation observer notifications (C6)
- ✅ Stealth funziona: webdriver=False, UA override, languages, page.mouse.* OK
- ⚠️ `Browser.setLocaleOverride` con locale non vuoto crasha content process dopo 30s (C7 partial, workaround `locale=""`)

## firefox-2 SHIPPED resta canonical

Per gli utenti, `firefox-2` (https://github.com/feder-cr/invisible_playwright/releases/tag/firefox-2) è il binario stable:
- Built May 3 2026 sul working tree locale che aveva tutte le modifiche
- Suo `xul.dll` (sha256 `97b38e1bc3a6cdfd...`) contiene 3 simboli C++ extra rispetto a una build da clean fork
- Test mouse suite: 8/12 PASS (4 fail sono flakiness Playwright/timing, non stealth/build)

## Riferimento: i 7 file Juggler hand-patched (catturati nel commit `2f0dbec`)

| File | Cosa | Linee |
|---|---|---|
| `juggler/components/Juggler.js` | debug dump on pipe disconnect | +1 |
| `juggler/TargetRegistry.js` | screencast stub + setDownloadInterceptor handling | +73/-10 |
| `juggler/SimpleChannel.js` | class → var for hot-reload compat | +3/-3 |
| `juggler/JugglerFrameParent.sys.mjs` | FF150 Fission 2-actor routing + pending actor registry | +26/-1 |
| `juggler/content/JugglerFrameChild.jsm` | try/catch removeListeners | +4 |
| `juggler/content/JugglerFrameChild.sys.mjs` | try/catch removeListeners | +4 |
| `juggler/content/FrameTree.js` | FF150 Fission cross-process abort + try/catch | +17 |

## Riferimento dei 4 file source modificati (oltre alle 15 patch ufficiali)

| File | Cosa | Linee |
|---|---|---|
| `toolkit/toolkit.mozbuild` | aggiunge `"/juggler",` a DIRS | +1 |
| `xpcom/reflect/xptinfo/xptinfo.h` | PARAM_BUFFER_COUNT 14 → 15 | +3 |
| `third_party/libwebrtc/api/location.h` | NUOVO file 352 byte | NUOVO |
| `dom/media/webrtc/transport/third_party/nICEr/src/ice/ice_component.c` | NUOVO file 74k byte | NUOVO |

## Vedi anche

- Schema completo input/output: `c:/tmp/firefox_stealth_schema.md` (workspace privato)
- Memory analytics: `project_patch_series_gaps.md` (workspace privato)
- Public deliverable: https://github.com/feder-cr/firefox-stealth (15 .patch files — currently INCOMPLETE, vedi gap residui sopra)
- Python wrapper + binary releases: https://github.com/feder-cr/invisible_playwright
