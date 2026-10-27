# stillborn/api-keys

Category: build, other
Prefs: zoom.stealth.apikey, zoom.stealth.apikey.google_location_service, zoom.stealth.apikey.google_safebrowsing, zoom.stealth.apikey.mozilla
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/components/url-classifier/SafeBrowsing.sys.mjs` (SafeBrowsing.init() (line 354), already-hook): The observed name is a branch, not one key. observe() calls readPrefs() (checked in the Mozilla base at line 495), which re-reads the key, so a late declaration enables the provider. The added comment block is about 12 lines in Italian and should be trimmed or moved out of the Mozilla file.
- `toolkit/components/urlformatter/URLFormatter.sys.mjs` (module scope helpers (hunk 1); nsURLFormatterService.prototype getter table (hunk 2, line 121); nsURLFormatterService.prototype.trimSensitiveURLs (hunk 3, line 180), movable): Keys move from build-time AppConstants to prefs declared by invisible_core, read through one reader. An undeclared key gives an empty string plus a console error, so the request is not sent. The three pref names are literal constants on purpose (a grep gate). trimSensitiveURLs now reads the same prefs for log redaction. The helper functions (hunk 1) belong in our own file. Pref names contain a ven
- `toolkit/modules/AppConstants.sys.mjs` (AppConstants object literal (module scope), keys near line 179, already-hook): Removes three build-time constants and the #if around the location-service key. About 20 comment lines are added to a Mozilla file: move the explanation out and keep only the removals. A grep of toolkit/modules, url-classifier, urlformatter and telemetry found no remaining reader (comments only); git grep of the whole tracked tree (excluding third_party) finds no reader or definer of the three key
- `toolkit/modules/moz.build` (module level, the OS_TARGET block (hunk 1, line 277) and the for var tuple of DEFINES (hunk 2, line 296), already-hook): Removes the DEFINES that fed the three keys. Must move together with the AppConstants.sys.mjs removal, otherwise the preprocessor fails. Only comments were added in the middle of the tuple.
