/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * ⛔ STEALTHFOX: le tre chiavi API le DICHIARA invisible_core, non la build.
 *
 * Upstream le incideva a tempo di compilazione: configure leggeva tre keyfile e
 * sostituiva @MOZ_..._API_KEY@ dentro AppConstants.sys.mjs, che a runtime e' una
 * costante congelata. Quel percorso e' stato RIMOSSO per intero - le tre voci di
 * AppConstants.sys.mjs, i tre DEFINES di toolkit/modules/moz.build e le tre
 * --with-*-api-keyfile del .mozconfig non esistono piu' - perche' una copia
 * compilata nel motore e' una seconda fonte di verita': due numeri per la stessa
 * cosa, che divergono in silenzio il giorno in cui uno dei due cambia.
 *
 * Adesso il valore vive in UN SOLO posto, invisible_core, e arriva come pref.
 * Il dominio e' finito e noto - tre chiavi - quindi la regola 2 e' soddisfatta.
 *
 * ⛔ E L'ASSENZA SI RIFIUTA, non si rimpiazza. Senza dichiarazione questa
 * funzione torna la stringa VUOTA, che e' esattamente cio' che il motore sa gia'
 * trattare: SafeBrowsing.checkGoogleSafeBrowsingKey la legge come falsy e spegne
 * il provider, quindi la richiesta non parte affatto. Tornare il SEGNAPOSTO
 * sarebbe stato peggio del difetto: "%GOOGLE_SAFEBROWSING_API_KEY%" e' una
 * stringa non vuota e diversa dalla sentinella, quindi quel controllo sarebbe
 * passato e Firefox avrebbe spedito una URL col segnaposto dentro.
 *
 * L'errore in console e' deliberato: una dichiarazione mancante deve essere
 * rumorosa, perche' il modo di sbagliare di questo meccanismo e' restare zitto.
 */
// ⛔ I NOMI SONO LETTERALI INTERI, E NON E' UNO STILE: E' UN GATE.
// La prima versione costruiva il nome (`"zoom.stealth.apikey." + nome`) e
// `test_every_stealth_pref_emitted_is_one_the_binary_reads` del core l'ha
// respinta con la ragione giusta: quel gate cerca ogni pref `zoom.stealth.*`
// che il core EMETTE dentro il sorgente del motore, perche' un nome che il
// binario non legge mai e' uno spoof che silenziosamente non fa niente, e da
// Python i due casi sono indistinguibili. Ne erano gia' stati spediti quattro
// cosi'. Un nome costruito e' invisibile a quel gate e a chiunque faccia una
// grep. Ogni letterale compare UNA volta, qui.
const PREF_APIKEY_MOZILLA = "zoom.stealth.apikey.mozilla";
const PREF_APIKEY_GOOGLE_LOCATION =
  "zoom.stealth.apikey.google_location_service";
const PREF_APIKEY_GOOGLE_SAFEBROWSING =
  "zoom.stealth.apikey.google_safebrowsing";

function leggiChiaveSilenziosa(pref) {
  // ⛔ L'UNICO punto del motore che LEGGE una di queste pref. I due chiamanti
  // (i getter di formatURL e la redazione dei log) passano di qui: se la
  // lettura fosse scritta due volte, il giorno in cui cambia la redazione
  // smetterebbe di ripulire senza che niente lo dica, e la chiave finirebbe
  // nei log in chiaro.
  return Services.prefs.getStringPref(pref, "");
}

function stealthDeclaredApiKey(pref) {
  const valore = leggiChiaveSilenziosa(pref);
  if (!valore) {
    console.error(
      "STEALTHFOX: nessuna chiave dichiarata in " +
        pref +
        " - la richiesta che la usa non partira'."
    );
  }
  return valore;
}

// ⛔ STEALTHFOX: dichiarate da invisible_core, non incise nella build.
// I getter restano pigri: la pref si legge a ogni formatURL, non al caricamento.
// stealthApplyApiKeyGetters, in fondo a URLFormatter.sys.mjs, li scrive SOPRA i
// tre getter di Mozilla, che restano come Mozilla li ha scritti e leggono voci
// di AppConstants che non esistono piu' (undefined): non vengono mai chiamati.
function stealthApiKeyGetters() {
  return {
    MOZILLA_API_KEY: () => stealthDeclaredApiKey(PREF_APIKEY_MOZILLA),
    GOOGLE_LOCATION_SERVICE_API_KEY: () =>
      stealthDeclaredApiKey(PREF_APIKEY_GOOGLE_LOCATION),
    GOOGLE_SAFEBROWSING_API_KEY: () =>
      stealthDeclaredApiKey(PREF_APIKEY_GOOGLE_SAFEBROWSING),
  };
}

export function stealthApplyApiKeyGetters(aFormatterPrototype) {
  Object.assign(aFormatterPrototype._defaults, stealthApiKeyGetters());
}

// ⛔ STEALTHFOX: legge le stesse DICHIARAZIONI dei getter qui sopra, non
// piu' le costanti di AppConstants. Chiamata in testa a trimSensitiveURLs: il
// codice di Mozilla che segue resta com'e' e, con quelle costanti undefined,
// lascia il messaggio intatto. Se leggesse una fonte diversa da quella
// che ha costruito la URL, questa redazione mancherebbe il bersaglio: e'
// proprio il difetto delle due fonti di verita', qui in forma di chiave che
// finisce nei log in chiaro. Passa da leggiChiaveSilenziosa, che NON registra
// l'errore in console: questo e' il percorso dei log, e un log che si lamenta
// mentre ripulisce un altro log e' rumore, non un segnale.
export function stealthTrimApiKeys(aMsg) {
  const geoloc = leggiChiaveSilenziosa(PREF_APIKEY_GOOGLE_LOCATION);
  const sb = leggiChiaveSilenziosa(PREF_APIKEY_GOOGLE_SAFEBROWSING);
  aMsg = geoloc
    ? aMsg.replace(RegExp(geoloc, "g"), "[trimmed-google-api-key]")
    : aMsg;
  return sb
    ? aMsg.replace(RegExp(sb, "g"), "[trimmed-google-api-key]")
    : aMsg;
}
