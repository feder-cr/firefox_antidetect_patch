# automation/downloads

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `uriloader/exthandler/nsExternalHelperAppService.cpp` (new method nsExternalHelperAppService::SetDownloadInterceptor, inserted after GetPreferredDownloadsDirectory (line 387), already-hook): Dead storage: mInterceptor is assigned here and never read in this file or the header; the comment says the intercept dispatch in CreateSaverForTempFile is not implemented. The setter does nothing else. The comment names a product (Playwright) in code: check against the no-vendor-names rule. Verified: git grep of the whole tree finds mInterceptor only at nsExternalHelperAppService.cpp:403 (write) 
- `uriloader/exthandler/nsExternalHelperAppService.h` (class nsExternalHelperAppService, new member after the last declared member (hunk header line 302), already-hook): Member is never read (see the .cpp entry). Remove together with the setter if the feature is not finished. Verified by whole-tree git grep (only uriloader/exthandler/nsExternalHelperAppService.cpp:403 writes it).
- `uriloader/exthandler/nsIExternalHelperAppService.idl` (file level: new interface nsIDownloadInterceptor (hunk 1, before the nsIExternalHelperAppService comment); interface nsIExternalHelperAppService, new method setDownloadInterceptor (hunk 2), movable): The interface body can move to our own .idl registered by our own moz.build, leaving one method line in Mozilla's file. The uuid is hard-coded and was copied from an earlier patch series; uniqueness was not checked. The comment says it was restored from an FF146 series. Nothing in this list calls onDownloadComplete or the interceptor. Verified: the IDL onDownloadComplete(aHandler, aErrorName) has 
