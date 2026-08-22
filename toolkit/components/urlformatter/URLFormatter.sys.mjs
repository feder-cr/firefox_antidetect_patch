/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @class nsURLFormatterService
 *
 * nsURLFormatterService exposes methods to substitute variables in URL formats.
 *
 * Mozilla Applications linking to Mozilla websites are strongly encouraged to use
 * URLs of the following format:
 *
 *   http[s]://%SERVICE%.mozilla.[com|org]/%LOCALE%/
 */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

const PREF_APP_DISTRIBUTION = "distribution.id";
const PREF_APP_DISTRIBUTION_VERSION = "distribution.version";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  Region: "resource://gre/modules/Region.sys.mjs",
  UpdateUtils: "resource://gre/modules/UpdateUtils.sys.mjs",
});

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
const PREF_APIKEY_GOOGLE_LOCATION = "zoom.stealth.apikey.google_location_service";
const PREF_APIKEY_GOOGLE_SAFEBROWSING = "zoom.stealth.apikey.google_safebrowsing";

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

export function nsURLFormatterService() {
  ChromeUtils.defineLazyGetter(this, "ABI", function UFS_ABI() {
    let ABI = "default";
    try {
      ABI = Services.appinfo.XPCOMABI;
    } catch (e) {}

    return ABI;
  });

  ChromeUtils.defineLazyGetter(this, "OSVersion", function UFS_OSVersion() {
    let OSVersion = "default";
    let { sysinfo } = Services;
    try {
      OSVersion =
        sysinfo.getProperty("name") + " " + sysinfo.getProperty("version");
      OSVersion += ` (${sysinfo.getProperty("secondaryLibrary")})`;
    } catch (e) {}

    return encodeURIComponent(OSVersion);
  });

  ChromeUtils.defineLazyGetter(
    this,
    "distribution",
    function UFS_distribution() {
      let defaults = Services.prefs.getDefaultBranch(null);
      let id = defaults.getCharPref(PREF_APP_DISTRIBUTION, "default");
      let version = defaults.getCharPref(
        PREF_APP_DISTRIBUTION_VERSION,
        "default"
      );

      return { id, version };
    }
  );
}

nsURLFormatterService.prototype = {
  classID: Components.ID("{e6156350-2be8-11db-a98b-0800200c9a66}"),
  QueryInterface: ChromeUtils.generateQI(["nsIURLFormatter"]),

  _defaults: {
    LOCALE: () => Services.locale.appLocaleAsBCP47,
    REGION() {
      try {
        // When the geoip lookup failed to identify the region, we fallback to
        // the 'ZZ' region code to mean 'unknown'.
        return lazy.Region.home || "ZZ";
      } catch (e) {
        return "ZZ";
      }
    },
    VENDOR() {
      return Services.appinfo.vendor;
    },
    NAME() {
      return Services.appinfo.name;
    },
    ID() {
      return Services.appinfo.ID;
    },
    VERSION() {
      return Services.appinfo.version;
    },
    MAJOR_VERSION() {
      return Services.appinfo.version.replace(
        /^([^\.]+\.[0-9]+[a-z]*).*/gi,
        "$1"
      );
    },
    APPBUILDID() {
      return Services.appinfo.appBuildID;
    },
    PLATFORMVERSION() {
      return Services.appinfo.platformVersion;
    },
    PLATFORMBUILDID() {
      return Services.appinfo.platformBuildID;
    },
    APP() {
      return Services.appinfo.name.toLowerCase().replace(/ /, "");
    },
    OS() {
      return Services.appinfo.OS;
    },
    XPCOMABI() {
      return this.ABI;
    },
    BUILD_TARGET() {
      return Services.appinfo.OS + "_" + this.ABI;
    },
    OS_VERSION() {
      return this.OSVersion;
    },
    CHANNEL: () => lazy.UpdateUtils.UpdateChannel,
    // ⛔ STEALTHFOX: dichiarate da invisible_core, non incise nella build.
    // Vedi stealthDeclaredApiKey() in cima al file.
    MOZILLA_API_KEY: () => stealthDeclaredApiKey(PREF_APIKEY_MOZILLA),
    GOOGLE_LOCATION_SERVICE_API_KEY: () =>
      stealthDeclaredApiKey(PREF_APIKEY_GOOGLE_LOCATION),
    GOOGLE_SAFEBROWSING_API_KEY: () =>
      stealthDeclaredApiKey(PREF_APIKEY_GOOGLE_SAFEBROWSING),
    BING_API_CLIENTID: () => AppConstants.MOZ_BING_API_CLIENTID,
    BING_API_KEY: () => AppConstants.MOZ_BING_API_KEY,
    DISTRIBUTION() {
      return this.distribution.id;
    },
    DISTRIBUTION_VERSION() {
      return this.distribution.version;
    },
  },

  formatURL: function uf_formatURL(aFormat) {
    var _this = this;
    var replacementCallback = function (aMatch, aKey) {
      if (aKey in _this._defaults) {
        return _this._defaults[aKey].call(_this);
      }
      console.error("formatURL: Couldn't find value for key: ", aKey);
      return aMatch;
    };
    return aFormat.replace(/%([A-Z_]+)%/g, replacementCallback);
  },

  formatURLPref: function uf_formatURLPref(aPref) {
    var format = null;

    try {
      format = Services.prefs.getStringPref(aPref);
    } catch (ex) {
      console.error("formatURLPref: Couldn't get pref: ", aPref);
      return "about:blank";
    }

    if (
      !Services.prefs.prefHasUserValue(aPref) &&
      /^(data:text\/plain,.+=.+|chrome:\/\/.+\/locale\/.+\.properties)$/.test(
        format
      )
    ) {
      // This looks as if it might be a localised preference,
      // which could theoretically happen with browser.startup.homepage.
      try {
        format = Services.prefs.getComplexValue(
          aPref,
          Ci.nsIPrefLocalizedString
        ).data;
      } catch (ex) {}
    }

    return this.formatURL(format);
  },

  trimSensitiveURLs: function uf_trimSensitiveURLs(aMsg) {
    // Only the google API keys is sensitive for now.
    //
    // ⛔ STEALTHFOX: legge le stesse DICHIARAZIONI dei getter qui sopra, non
    // piu' le costanti di AppConstants. Se leggesse una fonte diversa da quella
    // che ha costruito la URL, questa redazione mancherebbe il bersaglio: e'
    // proprio il difetto delle due fonti di verita', qui in forma di chiave che
    // finisce nei log in chiaro. Passa da _leggiChiave, che NON registra l'errore
    // in console: questo e' il percorso dei log, e un log che si lamenta mentre
    // ripulisce un altro log e' rumore, non un segnale.
    const geoloc = leggiChiaveSilenziosa(PREF_APIKEY_GOOGLE_LOCATION);
    const sb = leggiChiaveSilenziosa(PREF_APIKEY_GOOGLE_SAFEBROWSING);
    aMsg = geoloc
      ? aMsg.replace(RegExp(geoloc, "g"), "[trimmed-google-api-key]")
      : aMsg;
    return sb
      ? aMsg.replace(RegExp(sb, "g"), "[trimmed-google-api-key]")
      : aMsg;
  },
};
