# automation/downloads

Category: juggler
Prefs: none read directly
Test: juggler/tests/download_complete_follows_the_idl.test.mjs; end to end in the wrapper (tests/test_download_is_intercepted_e2e.py)

## What it does

Our automation's download interceptor (Juggler, juggler/TargetRegistry.js) owns
the downloads its client asked for (Browser.setDownloadOptions). In
nsExternalAppHandler::OnStartRequest, before SetUpTempFile, StealthDownloadIntercept
asks it; when it takes the download, the saver writes straight to the file it
chose, with no nsITransfer and no downloads UI, and exactly one
onDownloadComplete follows (OnSaveComplete or Cancel; empty error name on
success). No interceptor, or one that declines or throws: stock Firefox.
Until 2026-10-30 only the setter existed and nothing read it, so no download
event ever reached the client. Hook ceilings went up for it then:
nsExternalHelperAppService.cpp 7 -> 13, .h 5 -> 9, moz.build 1 -> 2.

## Hooks in Mozilla files

- `uriloader/exthandler/nsExternalHelperAppService.cpp` (new method nsExternalHelperAppService::SetDownloadInterceptor, inserted after GetPreferredDownloadsDirectory (line 387), already-hook): Dead storage: mInterceptor is assigned here and never read in this file or the header; the comment says the intercept dispatch in CreateSaverForTempFile is not implemented. The setter does nothing else. The comment names a product (Playwright) in code: check against the no-vendor-names rule. Verified: git grep of the whole tree finds mInterceptor only at nsExternalHelperAppService.cpp:403 (write) 
- `uriloader/exthandler/nsExternalHelperAppService.h` (class nsExternalHelperAppService, new member after the last declared member (hunk header line 302), already-hook): Member is never read (see the .cpp entry). Remove together with the setter if the feature is not finished. Verified by whole-tree git grep (only uriloader/exthandler/nsExternalHelperAppService.cpp:403 writes it).
- `uriloader/exthandler/nsIExternalHelperAppService.idl` (file level: new interface nsIDownloadInterceptor (hunk 1, before the nsIExternalHelperAppService comment); interface nsIExternalHelperAppService, new method setDownloadInterceptor (hunk 2), movable): The interface body can move to our own .idl registered by our own moz.build, leaving one method line in Mozilla's file. The uuid is hard-coded and was copied from an earlier patch series; uniqueness was not checked. The comment says it was restored from an FF146 series. Nothing in this list calls onDownloadComplete or the interceptor. Verified: the IDL onDownloadComplete(aHandler, aErrorName) has 

## uriloader/exthandler/nsExternalHelperAppService.cpp: nsExternalHelperAppService::SetDownloadInterceptor

Stealthfox: install/replace the Juggler-side download interceptor.
Storage only; the actual intercept dispatch in CreateSaverForTempFile is
not implemented in this minimal port — Juggler's interceptor receives
constructor wiring but downloads still flow through the default save
path. Sufficient to satisfy juggler/TargetRegistry.js startup; download
intercept-and-redirect feature itself remains TODO.
