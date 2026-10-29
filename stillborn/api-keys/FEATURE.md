# stillborn/api-keys

Category: build, other
Prefs: zoom.stealth.apikey, zoom.stealth.apikey.google_location_service, zoom.stealth.apikey.google_safebrowsing, zoom.stealth.apikey.mozilla
Test: toolkit/components/urlformatter/tests/unit/test_stealth_apikeys.js

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/components/url-classifier/SafeBrowsing.sys.mjs` (SafeBrowsing.init() (line 354), already-hook): The observed name is a branch, not one key. observe() calls readPrefs() (checked in the Mozilla base at line 495), which re-reads the key, so a late declaration enables the provider. The added comment block is about 12 lines in Italian and should be trimmed or moved out of the Mozilla file.
- `toolkit/components/urlformatter/URLFormatter.sys.mjs` (module imports; end of the module; nsURLFormatterService.prototype.trimSensitiveURLs, already-hook): Keys move from build-time AppConstants to prefs declared by invisible_core. Mozilla's three getters and its redaction stay as Mozilla wrote them (they read AppConstants entries this folder removes, so they see undefined); an Object.assign at the end of the module writes our getters over theirs, and trimSensitiveURLs calls stealthTrimApiKeys first.
- `toolkit/modules/AppConstants.sys.mjs` (AppConstants object literal (module scope), keys near line 179, already-hook): Removes three build-time constants and the #if around the location-service key. About 20 comment lines are added to a Mozilla file: move the explanation out and keep only the removals. A grep of toolkit/modules, url-classifier, urlformatter and telemetry found no remaining reader (comments only); git grep of the whole tracked tree (excluding third_party) finds no reader or definer of the three key
- `toolkit/modules/moz.build` (module level, the OS_TARGET block (hunk 1, line 277) and the for var tuple of DEFINES (hunk 2, line 296), already-hook): Removes the DEFINES that fed the three keys. Must move together with the AppConstants.sys.mjs removal, otherwise the preprocessor fails. Only comments were added in the middle of the tuple.

## toolkit/components/url-classifier/SafeBrowsing.sys.mjs: SafeBrowsing.init() (observer zoom.stealth.apikey)

Gancio: la riga `Services.prefs.addObserver("zoom.stealth.apikey", this);` in SafeBrowsing.init(); "questa riga" nel testo che segue e' quella.

⛔ STEALTHFOX: la chiave di Safe Browsing non e' piu' incisa nel binario,
la DICHIARA invisible_core come pref (StealthApiKeys.sys.mjs,
stealthDeclaredApiKey). Senza questa riga la dichiarazione che arriva DOPO
l'avvio non verrebbe mai riletta, e questo e' il caso normale invece che
l'eccezione: sul percorso Playwright le prefs del core viaggiano sul
protocollo e `Browser.enable` le applica dopo il lancio, mentre `init()`
di SafeBrowsing.sys.mjs gira all'avvio. Il provider resterebbe spento per sempre con la chiave
presente, che e' il modo peggiore di sbagliare - silenzioso e permanente.

`observe` cade su `readPrefs()`, che in coda rifa' `controlUpdateChecking()`
e quindi `updateProviderURLs()` -> `checkGoogleSafeBrowsingKey()`: la
chiave viene riletta e il provider si accende quando la dichiarazione
arriva, da qualunque canale arrivi.

## toolkit/modules/AppConstants.sys.mjs: le tre chiavi API tolte dall'oggetto AppConstants

⛔ STEALTHFOX: QUI STAVANO LE TRE CHIAVI API, E SONO STATE TOLTE.

```
  MOZ_GOOGLE_LOCATION_SERVICE_API_KEY   (dentro un `#if !defined(MOZ_WIDGET_ANDROID) && !defined(XP_WIN)`)
  MOZ_GOOGLE_SAFEBROWSING_API_KEY
  MOZ_MOZILLA_API_KEY
```

I nomi sono scritti NUDI apposta: nella forma con le chiocciole sarebbero
segnaposto veri per il preprocessore, che non trovando piu' i DEFINES
corrispondenti li lascerebbe li' o si fermerebbe. Un commento non deve
poter rompere la build che descrive.

Erano incise da configure a tempo di compilazione leggendo tre keyfile.
Adesso le dichiara invisible_core e arrivano come pref: il solo lettore e'
stealthDeclaredApiKey() in StealthApiKeys.sys.mjs (URLFormatter.sys.mjs lo importa), che e' anche l'unico punto
che sapeva leggerle da qui. Lasciarle come costanti avrebbe significato
tenere DUE fonti per lo stesso valore, che e' cio' che la regola 7 vieta:
non una fuga, ma due numeri per la stessa cosa che possono divergere.

I DEFINES corrispondenti sono stati tolti da toolkit/modules/moz.build nello
stesso cambiamento: se restassero li' senza queste voci di AppConstants.sys.mjs non farebbero
niente, e se restassero queste voci senza quei DEFINES la preelaborazione
fallirebbe. Si muovono insieme.

MOZ_BING_API_* restano perche' hanno un consumatore diverso e non sono state
spostate: il dominio dichiarato e' quello delle tre chiavi, non di tutte.
