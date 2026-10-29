/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// platform/script-cache: after the startup script cache is written, adopt the
// cache file we just wrote in place of the heap copies of the same bytes.
//
// Included ONLY by ScriptPreloader.cpp, as the very last thing in the file,
// after "}  // namespace mozilla". It is not a header and cannot be compiled on
// its own: it needs three things that exist only inside that translation unit,
// namely the file-static MAGIC, the anonymous-namespace gLog and the
// using-directives of the file (mozilla::loader, JS), which it inherits by
// reopening namespace mozilla. LOG is already #undef'd at that point, so
// MOZ_LOG is used directly. It must not be listed in moz.build.
//
// Both functions are members of ScriptPreloader (declared in
// ScriptPreloader.h), so the private state (mScripts, mMonitor,
// mWrittenCacheData and the nested type CachedStencil) is reached with no
// friend declaration and no accessor.
//
// Comment of the member `AutoMemMap mWrittenCacheData` (declared in
// ScriptPreloader.h between mCacheData and mMonitor, a position that fixes the
// destruction order and must not change):
//
//   La mappatura del file che abbiamo SCRITTO in questa sessione, dopo che
//   AdoptWrittenCache l'ha adottata. Vive quanto l'istanza, perche' i
//   mXDRRange delle voci puntano dentro di lei: distruggerla prima li
//   renderebbe pendenti.
//
//   E' un membro per valore e non un puntatore a una statica come mCacheData
//   perche' non deve sopravvivere all'istanza - mCacheData lo fa solo per come
//   e' costruito il singleton, non per un requisito di durata.

#ifndef StealthScriptCache_inl_h
#define StealthScriptCache_inl_h

#include <inttypes.h>
#include <string.h>

#include "mozilla/UniquePtr.h"

namespace mozilla {

void ScriptPreloader::ReportWrittenCache(
    nsIHandleReportCallback* aHandleReport, nsISupports* aData) {
  // La mappatura del file scritto in QUESTA sessione, adottata al posto delle
  // copie sull'heap (AdoptWrittenCache). Va riportata, altrimenti i megabyte
  // spariscono dall'heap e non ricompaiono da nessuna parte, e un totale che non
  // torna e' indistinguibile da un risparmio vero.
  MOZ_COLLECT_REPORT(
      "explicit/script-preloader/non-heap/memmapped-written-cache",
      KIND_NONHEAP, UNITS_BYTES,
      mWrittenCacheData.nonHeapSizeOfExcludingThis(),
      "The cache file this session wrote, mapped read-only and adopted in "
      "place of the heap copies of the same bytes.");
}

// Dopo aver scritto il file di cache, adotta i byte del FILE al posto delle
// copie sull'heap: mappa il file appena scritto e ripunta ogni voce dentro la
// mappatura, liberando buffer e stencil che diventano ridondanti.
//
// Perche' esiste. Con un profilo CALDO questa cache e' una mappatura di file e
// non costa heap: mXDRRange punta dentro mCacheData, IsMemMapped() e' vero e
// FreeData() non ha niente da liberare. Con un profilo FRESCO gli stessi byte
// sono heap anonimo, perche' sono stati costruiti in memoria per la scrittura,
// e li' restano per tutta la sessione: misurato 16,52 MB nel processo padre,
// invariati fra 40 s e 180 s, con il file da 13.343.940 byte gia' su disco.
// Questo porta la prima sessione nello stesso stato della seconda.
//
// I byte sono su disco: da qui in avanti tenerne una copia sull'heap vuol dire
// avere due volte la stessa cosa. MoveTo aggiorna `cacheFile` sulla nuova
// posizione, quindi e' lui il file da mappare (lo passa il chiamante, WriteCache,
// subito dopo MoveTo).
//
// Non restituisce nulla: l'unico chiamante (WriteCache) registrava l'errore e
// andava avanti, quindi la registrazione sta qui e il gancio e' una riga sola.
// Il testo dei due messaggi di log e' quello di prima.
void ScriptPreloader::AdoptWrittenCache(nsIFile* aCacheFile) {
  auto adopt = [&]() -> Result<Ok, nsresult> {
    // ⛔ Si adotta UNA volta sola, e non e' una precauzione teorica.
    // AutoMemMap::init apre con MOZ_ASSERT(!fd): in debug scatta, in RELEASE
    // l'assert non c'e' e la seconda init sovrascriverebbe fd e addr, lasciando
    // PENDENTI i mXDRRange delle voci gia' adottate - cioe' puntatori dentro una
    // regione smappata, che e' un crash e non un degrado.
    //
    // Una seconda scrittura puo' succedere: il commento in Run() dice che
    // l'invalidazione della cache "can trigger a new write during shutdown". In
    // quel caso le voci di allora tengono le loro copie sull'heap, che e' il
    // comportamento di prima, e la mappatura vecchia resta viva finche' vive
    // l'istanza.
    if (mWrittenCacheData.initialized()) {
      return Ok();
    }

    MOZ_TRY(mWrittenCacheData.init(aCacheFile));

    mMonitor.AssertNotCurrentThreadOwns();
    MonitorAutoLock mal(mMonitor);

    size_t adottate = 0;
    size_t inPrestito = 0;

    // Lettura dell'intestazione del file mappato: la stessa aritmetica e gli
    // stessi controlli di InitCacheInternal, lato LETTORE. Chi SCRIVE calcola
    // l'inizio della sezione dati come MAGIC + headerSize + header, senza i 4
    // byte del crc che pure scrive, mentre chi LEGGE li conta. Le due formule
    // danno lo stesso risultato solo perche' l'allineamento e' un divisore di 4.
    // Questa e' una copia di InitCacheInternal (che resta com'e' in Mozilla): se
    // quella cambia, va cambiata anche questa.
    auto size = mWrittenCacheData.size();

    uint32_t headerSize;
    uint32_t crc;
    if (size < sizeof(MAGIC) + sizeof(headerSize) + sizeof(crc)) {
      return Err(NS_ERROR_UNEXPECTED);
    }

    auto data = mWrittenCacheData.get<uint8_t>();
    MOZ_RELEASE_ASSERT(JS::IsTranscodingBytecodeAligned(data.get()));

    auto end = data + size;

    if (memcmp(MAGIC, data.get(), sizeof(MAGIC))) {
      return Err(NS_ERROR_UNEXPECTED);
    }
    data += sizeof(MAGIC);

    headerSize = LittleEndian::readUint32(data.get());
    data += sizeof(headerSize);

    crc = LittleEndian::readUint32(data.get());
    data += sizeof(crc);

    if (data + headerSize > end) {
      return Err(NS_ERROR_UNEXPECTED);
    }

    if (crc != ComputeCrc32c(~0, data.get(), headerSize)) {
      return Err(NS_ERROR_UNEXPECTED);
    }

    Range<const uint8_t> header(data, data + headerSize);
    data += headerSize;

    // Reconstruct alignment padding if required.
    size_t currentOffset = data - mWrittenCacheData.get<uint8_t>();
    data += JS::AlignTranscodingBytecodeOffset(currentOffset) - currentOffset;

    InputBuffer buf(header);

    size_t offset = 0;
    while (!buf.finished()) {
      auto script = MakeUnique<CachedStencil>(*this, buf);
      MOZ_RELEASE_ASSERT(script);

      auto scriptData = data + script->mOffset;
      if (!JS::IsTranscodingBytecodeAligned(scriptData.get())) {
        return Err(NS_ERROR_UNEXPECTED);
      }

      if (scriptData + script->mSize > end) {
        return Err(NS_ERROR_UNEXPECTED);
      }

      // Make sure offsets match what we'd expect based on script ordering and
      // size, as a basic sanity check.
      if (script->mOffset != offset) {
        return Err(NS_ERROR_UNEXPECTED);
      }
      offset += script->mSize;

      script->mXDRRange.emplace(scriptData, scriptData + script->mSize);

      auto* vivo = mScripts.Get(script->mCachePath);
      if (!vivo) {
        continue;
      }

      // Uno stencil che PRENDE IN PRESTITO il buffer non si puo' lasciare
      // indietro: distruggere il buffer sotto di lui darebbe puntatori
      // pendenti, e rilasciare la nostra referenza non basta, perche' qualcun
      // altro puo' tenerne una. Questa voce resta com'e'.
      if (vivo->mStencil && JS::StencilIsBorrowed(vivo->mStencil)) {
        inPrestito++;
        continue;
      }

      // I byte devono essere gli stessi, non solo la chiave.
      if (vivo->mSize != script->mSize) {
        continue;
      }

      // Sostituisce la copia sull'heap con gli stessi byte presi dal file di
      // cache mappato. Dopo questo passo IsMemMapped() e' vero, quindi
      // FreeData() torna a essere il no-op corretto e non c'e' piu' niente da
      // liberare: la sola copia dei byte e' il file.
      //
      // Qui si e' gia' verificato (sopra) che nessuno stencil stia PRENDENDO
      // IN PRESTITO il buffer (JS::StencilIsBorrowed), altrimenti distruggerlo
      // qui lascerebbe lo stencil con puntatori pendenti.
      vivo->mXDRRange.reset();
      if (!vivo->mXDRData.empty()) {
        vivo->mXDRData.destroy();
      }
      vivo->mXDRRange.emplace(script->Range());

      // E lo stencil va via con loro. Non e' una perdita di cache: e'
      // esattamente lo stato in cui questa voce si trova all'avvio SUCCESSIVO,
      // dove mStencil e' nullo e GetStencil decodifica dal range mappato. La
      // prima sessione smette di essere il caso speciale che paga due volte.
      vivo->mStencil = nullptr;
      adottate++;
    }

    if (buf.error()) {
      return Err(NS_ERROR_UNEXPECTED);
    }

    MOZ_LOG(gLog, LogLevel::Info,
            ("Adopted the written cache: %zu scripts, %zu left borrowed\n",
             adottate, inPrestito));
    return Ok();
  };

  // Se l'adozione non riesce non e' un errore della SCRITTURA: il file e' valido
  // e la sessione continua col comportamento di prima, cioe' le copie
  // sull'heap. Si registra e si va avanti, perche' far fallire la scrittura per
  // un'ottimizzazione mancata sarebbe peggio del problema che risolve.
  if (auto risultato = adopt(); risultato.isErr()) {
    MOZ_LOG(gLog, LogLevel::Info,
            ("Could not adopt the written cache (0x%" PRIx32 "), keeping the "
             "heap copies\n",
             static_cast<uint32_t>(risultato.unwrapErr())));
  }
}

}  // namespace mozilla

#endif  // StealthScriptCache_inl_h
