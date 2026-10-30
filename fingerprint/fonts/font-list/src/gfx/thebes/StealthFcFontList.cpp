/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthFcFontList.h"

#include <fontconfig/fontconfig.h>
#include <harfbuzz/hb.h>

#include <utility>

#include "FontData.h"
#include "SharedFontList-impl.h"
#include "StealthBundleFontList.h"
#include "gfxFcPlatformFontList.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/gfx/2D.h"
#include "nsString.h"
#include "nsTArray.h"

using mozilla::MakeAndAddRef;
using mozilla::MakeRefPtr;
using mozilla::gfx::Factory;
using mozilla::gfx::SharedFTFace;
using mozilla::gfx::UnscaledFontFontconfig;
namespace fontlist = mozilla::fontlist;

// Global namespace on purpose: gfxFontconfigFontEntry declares
// `friend struct StealthFcAccess;` (gfxFcPlatformFontList.h) so that this file
// can reach gfxFontEntry::InitializeFrom, which is protected.
struct StealthFcAccess {
  static void InitializeFrom(gfxFontconfigFontEntry* aEntry,
                             fontlist::Face* aFace,
                             const fontlist::Family* aFamily) {
    aEntry->InitializeFrom(aFace, aFamily);
  }
  static void AddUnscaledFont(gfxFontconfigFontEntry* aEntry,
                              const RefPtr<UnscaledFontFontconfig>& aFont) {
    aEntry->mUnscaledFontCache.Add(aFont);
  }
};

void StealthDeclareFontFile(FcPattern* aPattern, const char* aFile,
                            int aIndex) {
  // STEALTH: rimettere il file che `CreatePatternForFace` ha appena tolto.
  //
  // Quella funzione passa a fontconfig un nome di file FINTO e poi cancella
  // FC_FILE e FC_INDEX, perche' per un font scaricato da una pagina un file non
  // esiste. Per le facce del nostro bundle esiste, e tacerlo costa: senza file
  // nel pattern, `CreateScaledFont` costruisce un `UnscaledFont` senza percorso,
  // `GetFontDescriptor` fallisce e Gecko manda a WebRender i BYTE del font
  // invece del percorso. Misurati 65,9 MB nel processo padre contro 1,9 su una
  // pagina vuota - `70-known-bugs.md` [B154].
  //
  // Chi non passa un file non vede nessuna differenza: il ramo non scatta.
  if (aFile && *aFile) {
    FcPatternAddString(aPattern, FC_FILE,
                       reinterpret_cast<const FcChar8*>(aFile));
    FcPatternAddInteger(aPattern, FC_INDEX, aIndex);
  }
}

hb_face_t* StealthBaseHBFaceRef(gfxFontEntry* aEntry) {
  // Qualified call: the base implementation, not the Fontconfig override
  // (calling the override here would recurse).
  //
  // Oggetto nominato, non temporaneo: la conversione di AutoHBFace e'
  // ref-qualified `const&` proprio per rendere impossibile la forma che
  // penzola. Questa chiamata era gia' corretta - il temporaneo sarebbe
  // vissuto fino al punto e virgola - e cambia solo forma.
  const auto autoFace(aEntry->gfxFontEntry::GetHBFace());
  return hb_face_reference(autoFace);
}

uint32_t StealthTableFaceIndex(SharedFTFace* aFace) {
  return aFace ? uint32_t(aFace->GetFace()->face_index & 0x7FFF) : 0;
}

hb_blob_t* StealthHBFaceTable(gfxFontEntry* aEntry, uint32_t aTableTag) {
  // Come in StealthBaseHBFaceRef: oggetto nominato invece del temporaneo. Era
  // corretto anche prima (il temporaneo vive fino al punto e virgola), ma la
  // conversione di AutoHBFace ora e' `const&` per impedire la forma che
  // penzola.
  const auto face(aEntry->GetHBFace());
  return hb_face_reference_table(face, aTableTag);
}

void StealthDeclareTextOptions(FcPattern* aPattern) {
  // STEALTH: la dichiarazione entra nel PATTERN, non nei lettori.
  //
  // I lettori di questi valori sono DUE, e correggerne uno lascia l'altro sulla
  // macchina: `PrepareFontOptions` (che il chiamante, CreateFontInstance,
  // chiama subito dopo questa funzione) costruisce i flag di caricamento
  // FreeType, e `ScaledFontFontconfig::InstanceData` (gfx/2d, righe 140-185)
  // rilegge lo STESSO pattern per costruire le opzioni che vanno a WebRender,
  // che e' il percorso che rasterizza davvero. Corretto solo il primo, la
  // misura non si muoveva di un bit: e' la regola 16 vista dal vivo, lo stesso
  // fatto calcolato in due punti.
  //
  // Scrivendolo nel pattern il posto che decide torna a essere uno, e i due
  // lettori restano il codice upstream che erano.
  //
  // Il pattern su cui lavorano ha gia' subito, lo dice il commento upstream in
  // `PrepareFontOptions`, "user and system fontconfig configurations": quindi
  // `/etc/fonts` decideva come rasterizziamo le facce che imbarchiamo NOI.
  // Misurato il 2026-08-16 attraverso il prodotto, stesso binario e stessa
  // pagina: hash dei pixel 3842037683 con il fontconfig dell'host (che su
  // questa macchina vale `hintslight`), 1512591551 con una configurazione
  // vuota, 2489286717 con `hintnone`. Una pagina legge quei byte con
  // `getImageData`.
  //
  // Sono questi DUE e non sei: `FC_RGBA`, `FC_LCD_FILTER` e
  // `FC_EMBEDDED_BITMAP` misurano identici in ogni configurazione provata, e
  // restano dove sono.
  //
  // Nessun ripiego se la dichiarazione manca: sarebbe tornare a chiedere alla
  // macchina, e `StealthDeclarationGate` rifiuta l'avvio prima di arrivare qui.
  const int32_t hintstyle =
      mozilla::StaticPrefs::zoom_stealth_text_freetype_hintstyle();
  if (hintstyle >= 0) {
    FcPatternDel(aPattern, FC_HINT_STYLE);
    FcPatternDel(aPattern, FC_HINTING);
    FcPatternAddInteger(aPattern, FC_HINT_STYLE, hintstyle);
    FcPatternAddBool(aPattern, FC_HINTING,
                     hintstyle == FC_HINT_NONE ? FcFalse : FcTrue);
  }
  const int32_t antialias =
      mozilla::StaticPrefs::zoom_stealth_text_freetype_antialias();
  if (antialias >= 0) {
    FcPatternDel(aPattern, FC_ANTIALIAS);
    FcPatternAddBool(aPattern, FC_ANTIALIAS, antialias ? FcTrue : FcFalse);
  }
}

void StealthDeclaredFontFile(FcPattern* aPattern, std::string& aFile,
                             int& aIndex) {
  // STEALTH: si chiede al PATTERN se nomina un file, invece di dedurlo dal fatto
  // che la faccia sia stata creata dalla memoria.
  //
  // La condizione di Mozilla, subito prima, e' `if (!face->GetFontData())`, cioe'
  // un PROXY: "creata dalla memoria" sta per "non ha un file". Per le facce del
  // nostro bundle quel proxy e' falso - i byte sono la mappatura DI un file, di
  // cui conosciamo percorso e indice - e la conseguenza era che
  // l'`UnscaledFont` nasceva senza percorso, `GetFontDescriptor` falliva, e
  // Gecko mandava a WebRender i BYTE del font: 65,9 MB nel processo padre
  // contro 1,9 su una pagina vuota (`70-known-bugs.md` [B154]).
  //
  // Il comportamento upstream non si muove, caso per caso: una faccia normale
  // esce dal blocco di Mozilla col file gia' scritto (o con l'errore che era);
  // una faccia da dati SENZA file nel pattern (un font della pagina:
  // `CreatePatternForFace` toglie il nome finto) lascia `file` vuoto come prima.
  // L'unico caso che cambia e' il nostro, che prima non era previsto.
  if (!aFile.empty()) {
    return;
  }
  const FcChar8* fcFile;
  int index;
  if (FcPatternGetString(aPattern, FC_FILE, 0,
                         const_cast<FcChar8**>(&fcFile)) == FcResultMatch &&
      FcPatternGetInteger(aPattern, FC_INDEX, 0, &index) == FcResultMatch) {
    aFile = reinterpret_cast<const char*>(fcFile);
    aIndex = index;
  }
}

void StealthPreferFileUnscaledFont(
    gfxFontconfigFontEntry* aEntry, RefPtr<UnscaledFontFontconfig>& aFont,
    std::string& aFile, int aIndex, SharedFTFace* aFace) {
  // STEALTH: si sceglie su `file`, non su `GetFontData()`. E' lo stesso proxy
  // corretto in StealthDeclaredFontFile: cio' che decide se un UnscaledFont
  // puo' descriversi con un percorso e' AVERE un percorso, non da dove sono
  // arrivati i byte.
  //
  // Mozilla ha gia' scelto e messo in cache; qui si corregge solo il caso del
  // bundle (UnscaledFont senza percorso ma file noto). La prima voce, senza piu'
  // riferimenti, muore e il suo puntatore debole in cache con lei; la ricerca
  // successiva per (file, indice) trova questa.
  // `aFont->GetFile()` non vuoto: Mozilla ha gia' usato il percorso (e `aFile`
  // e' stato spostato, non va letto).
  if (!aFont->GetFile().empty() || aFile.empty()) {
    return;
  }
  aFont = new UnscaledFontFontconfig(std::move(aFile), aIndex, aFace);
  StealthFcAccess::AddUnscaledFont(aEntry, aFont);
}

bool StealthPopulateSharedList(fontlist::FontList* aList) {
  // Uniform bundle: build the shared family list + faces from our manifest,
  // skipping the fontconfig enumeration (host fonts never enter). Linux builds
  // faces eagerly, so AddFaces each family here.
  auto* bundle = StealthBundleFontList::Get();  // never null; it crashes first
  if (!aList) {
    return false;
  }
  nsTArray<fontlist::Family::InitData> famData(bundle->Families().Clone());
  aList->SetFamilyNames(famData);
  for (uint32_t i = 0; i < aList->NumFamilies(); i++) {
    auto& fam = aList->Families()[i];
    nsTArray<fontlist::Face::InitData> faceData;
    bundle->GetFaces(fam.DisplayName().AsString(aList), faceData);
    fam.AddFaces(aList, faceData);
  }
  return true;
}

already_AddRefed<gfxFontEntry> StealthCreateBundleFontEntry(
    fontlist::Face* aFace, const fontlist::Family* aFamily,
    fontlist::FontList* aList) {
  // Instantiate the face straight from its bundle file via FreeType;
  // NewSharedFTFaceFromData takes the .ttc face index directly.
  const nsCString file(aFace->mDescriptor.AsString(aList));
  // Stealth: UNA mappatura per FILE, prestata a ogni faccia - la stessa cosa
  // che fa Windows, con lo stesso oggetto e la stessa vita.
  //
  // Come si e' arrivati qui, in due passi. Prima FreeType riceveva una copia
  // `malloc` dell'intero file PER FACCIA, posseduta e liberata dalla vecchia
  // FTUserFontData, e invisibile ad about:memory: misurato 2026-08-14 su una
  // pagina che tocca 1, 10, 34 e 68 famiglie, l'heap non classificato del
  // processo di contenuto faceva 0,6, 9,0, 98,7 e 319,9 MB, cioe' ~1,7 volte i
  // byte dei file toccati - che e' il rapporto facce/famiglia (129/68).
  //
  // Poi (§5.2m) si e' passati al costruttore per PERCORSO, che faceva chiamare
  // a CloneFace `Factory::NewFTFace(path, index)`: niente copia, ma una seconda
  // implementazione. Windows mappava il file e lo prestava allo stream DWrite,
  // Linux lasciava aprire il file a FreeType: due risposte diverse alla stessa
  // domanda, e due rami di CloneFace che divergono anche in ALTRO - il ramo per
  // percorso NON chiama `FT_Select_Charmap`, mentre quello in memoria seleziona
  // UNICODE o MS_SYMBOL. Cioe' su Linux le facce nascevano con la charmap che
  // FreeType sceglie da solo, divergendo non solo da Windows ma dall'upstream.
  //
  // Adesso le due piattaforme fanno la stessa cosa: `GetFaceBlob` mappa il file
  // una volta e lo tiene in una cache immortale, `FT_New_Memory_Face` prende in
  // prestito quei byte senza copiarli (e' il contratto della chiamata), e il
  // ramo in memoria (da Firefox 159 `Factory::NewSharedFTFaceFromData`, prima
  // `FTUserFontData::CloneFace`) rimette la selezione della charmap. Le pagine
  // diventano del FILE su entrambe: condivise dal sistema operativo fra i
  // processi, e sfrattabili invece che swappabili.
  auto* bundle = StealthBundleFontList::Get();
  RefPtr<StealthBundleFontList::FileBlob> blob = bundle->GetFaceBlob(file);
  if (!blob || !blob->Length()) {
    return nullptr;
  }
  RefPtr<FontData> lent =
      new FontData(blob->Data(), blob->Length(), FontData::Borrowed::Yes);
  RefPtr<SharedFTFace> face =
      Factory::NewSharedFTFaceFromData(lent, aFace->mIndex);
  if (!face) {
    return nullptr;
  }
  // Il PERCORSO va dichiarato insieme ai byte, o WebRender riceve i byte.
  // Non e' una seconda fonte: e' la stessa funzione che risolve il blob qui
  // sopra, `GetFacePath`, che risolve attraverso la stessa directory di
  // `GetFaceBlob`. Se manca il percorso si prosegue lo stesso - il font
  // disegna, si paga solo la copia - quindi qui NON si rifiuta.
  nsAutoCString percorso;
  if (!StealthBundleFontList::GetFacePath(file, percorso)) {
    percorso.Truncate();
  }
  nsAutoCString name(aFamily->DisplayName().AsString(aList));
  RefPtr fe = MakeRefPtr<gfxFontconfigFontEntry>(
      name, aFace->mWeight, aFace->mWidth, aFace->mStyle, std::move(face));
  // The data-font constructor builds its pattern with CreatePatternForFace,
  // which drops the file; it is declared again here, on the entry's own
  // pattern, before anything reads it.
  StealthDeclareFontFile(fe->GetPattern(),
                         percorso.IsEmpty() ? nullptr : percorso.get(),
                         aFace->mIndex);
  // Wire the entry back to its shared-list face, exactly as the non-stealth
  // path of CreateFontEntry does and as the CoreText bundle-only branch
  // (CoreTextFontList.cpp, CreateFontEntry) does. This branch omitted it -
  // found 2026-08-07 - which left mShmemFace null for EVERY bundled face on
  // Linux.
  //
  // The DWrite branch was named here too, citing gfxDWriteFontList.cpp:1211.
  // That was WRONG: 1211 is in the host-collection branch, and the bundle-only
  // branch (gfxDWriteFontList::CreateStealthBundleFontEntry, now in
  // StealthDWriteFontList.cpp) omitted the call just like this one did, until
  // 2026-08-08. So this comment asserted a symmetry that did not exist and
  // would have kept anyone from looking - which is the whole reason a
  // cross-reference in a comment has to be checked against the code it
  // names, not trusted.
  // Two consequences: gfxFont::SanitizeMetrics could not tell a bundled face
  // from any other font, so the manifest's vertical metrics were silently
  // never applied; and the entry's charmap could never be published back
  // into the shared list (the mShmemFace->SetCharacterMap path), leaving
  // families permanently not-fully-initialized, which is what lets the async
  // cmap path record a codepoint in mCodepointsWithNoFonts and keep that
  // verdict for the rest of the session.
  //
  // Setting mShmemFace here was necessary for that second consequence and NOT
  // sufficient: ReadCMAP still refused to publish, because it asked
  // `!IsUserFont()` and every bundled face is built from file data. That half
  // is fixed separately, in gfxFontEntry::ShouldPublishCharacterMap.
  StealthFcAccess::InitializeFrom(fe.get(), aFace, aFamily);
  return fe.forget();
}
