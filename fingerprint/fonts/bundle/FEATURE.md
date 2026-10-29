# fingerprint/fonts/bundle

Category: build, other, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `browser/fonts/moz.build` ((top level of browser/fonts/moz.build), already-hook): Replaces the single TwemojiMozilla.ttf install with a FINAL_TARGET_FILES list of 121 quoted entries (bundled Windows core fonts plus bundle-fonts.list and TwemojiMozilla.ttf, which stays in the list). Adds cocoa to the platform condition. The list is hand-maintained here and must stay in sync with the bundle manifest. The comment says macOS also needs --enable-bundled-fonts in its mozconfig. The f
- `browser/installer/package-manifest.in` ((packaging manifest, fonts entry), already-hook): Packages @RESPATH@/fonts/* on macOS as well (upstream: Windows and GTK only). Packaging condition only; supports the bundled font set on every host.
- `gfx/2d/ScaledFontDWrite.cpp` (UnscaledFontDWrite::GetFontDescriptor, already-hook): Returns the declared file path (UTF-16 with NUL terminator, same convention as the Mozilla file-name path) when one was passed to the constructor. Otherwise it falls through to Mozilla's own `if (!mFont) return false;` guard, which is left unchanged. The comment reports 65.9 MB sent to WebRender as bytes instead of a path ([B154]). Category is other, not spoof: the change alters what gfx passes to
- `gfx/2d/UnscaledFontDWrite.h` (UnscaledFontDWrite constructor and member list (class UnscaledFontDWrite final : public UnscaledFont), already-hook): Adds an optional aFilePath argument with an empty default, so existing callers keep the upstream behaviour. Adds member mPercorsoDichiarato (std::wstring, ours) and the <string> include. Comment cross-references [B154].
- `gfx/thebes/FontData.h` (class FontData (Borrowed constructor, destructor, member mBorrowed), already-hook): A borrowed FontData does not free its bytes. The bundle path wraps the bytes lent by StealthBundleFontList in one: on FreeType builds it goes to Mozilla's FTUserFontData(FontData*), whose CloneFace opens it as it opens any FontData; on Windows to Mozilla's gfxDWriteFontFileLoader::CreateCustomFontFile, whose stream reads it as it reads any FontData. The bytes must outlive every face, which holds only because the bundle cache is immortal.
- `gfx/thebes/gfxDWriteFontList.h` (gfxDWriteFontEntry (class members mBundleFaceIndex and mBundleFilePath), already-hook): Two members: mBundleFaceIndex (feature font-ttc-face-index, index inside a .ttc) and mBundleFilePath (feature font-bundle-file-path, the path given to WebRender). Only the path member is in this file's primary feature. Adds the <string> include.
- `gfx/thebes/gfxFcPlatformFontList.h` (gfxFontconfigFontEntry::gfxFontconfigFontEntry (explicit constructor for data fonts), already-hook): Adds defaulted aFile and aIndex arguments, so upstream data-font callers compile unchanged. The bundle path passes both. Comment cross-references [B154].
- `gfx/thebes/gfxFontEntry.cpp` (gfxFontEntry::ShouldPublishCharacterMap (new, placed before gfxFontEntry::TrySetShmemCharacterMap), movable): One predicate replaces the four copies of '!IsUserFont() && mShmemFace' in the ReadCMAP paths (CoreText, DirectWrite, FreeType, fontconfig). It returns false for a non-shared-list face and for local() borrows, true for installed system faces, and for data faces only under bundle-only. Movable: the 8 logic lines can move into a free function in our own file.
- `gfx/thebes/gfxFontEntry.h` (gfxFontEntry::AutoHBFace (conversion operator); gfxFontEntry (class, new ShouldPublishCharacterMap declaration), already-hook): Primary feature hb-face-lifetime: the conversion operator is ref-qualified (const&, and const&& deleted), so binding the result of GetHBFace() to a raw pointer becomes a compile error. This fixes a dangling pointer in the stealth GetDesignFont (gfxHarfBuzzShaper.cpp), not a Firefox upstream problem; the comment reports crashes before the fix. Also adds the ShouldPublishCharacterMap declaration (fe
- `gfx/thebes/gfxFontUtils.cpp` (gfxFontUtils::FindSfntOffset (new static); gfxFontUtils::FindTableDirEntry; gfxFontUtils::GetTableFromFontData, movable): Generic .ttc support: table lookup and table blob now resolve through the requested face's offset inside the collection. Before this change the lookup read the file header only, so only the first face could be read. The default aFaceIndex = 0 keeps old callers unchanged. Not stealth-specific in principle; it is needed for the bundle's .ttc files. Movable: FindSfntOffset can move to our own file.
- `gfx/thebes/gfxFontUtils.h` (gfxFontUtils (static declarations FindSfntOffset, FindTableDirEntry, GetTableFromFontData), already-hook): Declarations match gfxFontUtils.cpp. aFaceIndex defaults to 0 on the two existing functions.
- `gfx/thebes/gfxPlatformGtk.cpp` (gfxPlatformGtk::GetCommonFallbackFonts, movable): Puts the Windows persona fallback names first (Segoe UI Emoji when colour is preferred, then Arial, Segoe UI, Segoe UI Symbol, Cambria Math), before the upstream Linux names, which are not in the bundle. The order is stated in the comment as a measured requirement. Since CommonFontFallback now takes names from the manifest first, this list is only reached when the manifest returns nothing. Movable
- `gfx/thebes/gfxTextRun.cpp` (gfxFontGroup::FindFallbackFaceForChar, already-hook): The async-fallback condition is replaced by pfl->MayDeferCmapLoading(), the same predicate GlobalFontFallback uses. Comment: this site was missing the bundle-only condition before. Feature gfx.font_rendering.fallback.async is read through that predicate.
- `gfx/thebes/moz.build` ((top level of gfx/thebes/moz.build: EXPORTS += [ ... ] and a new SOURCES += [ ... ] block after UNIFIED_SOURCES), already-hook): Exports StealthBundleFontList.h and StealthDeclarationGate.h, and compiles StealthBundleFontList.cpp and StealthDeclarationGate.cpp as non-unified SOURCES (the comment says short file-static helpers could collide in a unified build). The comment says dom/canvas reads the coverage ladder through the exported header. Those two .cpp/.h files are our own and are not in this diff.

## Our own files

- `browser/fonts/LEELAWUI.TTF`
- `browser/fonts/LUCON.TTF`
- `browser/fonts/L_10646.TTF`
- `browser/fonts/Nirmala.ttf`
- `browser/fonts/SitkaSmall.ttf`
- `browser/fonts/arial7_04.ttf`
- `browser/fonts/arialbd7_04.ttf`
- `browser/fonts/arialbi7_04.ttf`
- `browser/fonts/ariali7_04.ttf`
- `browser/fonts/ariblk.ttf`
- `browser/fonts/bahnschrift2_07.ttf`
- `browser/fonts/bundle-fonts.list`
- `browser/fonts/calibri6_26.ttf`
- `browser/fonts/calibrib6_26.ttf`
- `browser/fonts/calibrii6_26.ttf`
- `browser/fonts/calibriz6_26.ttf`
- `browser/fonts/cambria6_99.ttc`
- `browser/fonts/cambriab6_98.ttf`
- `browser/fonts/cambriai6_98.ttf`
- `browser/fonts/cambriaz6_98.ttf`
- `browser/fonts/candara5_64.ttf`
- `browser/fonts/candarab5_64.ttf`
- `browser/fonts/candarai5_64.ttf`
- `browser/fonts/candaraz5_64.ttf`
- `browser/fonts/comic5_15.ttf`
- `browser/fonts/comicbd5_15.ttf`
- `browser/fonts/comici5_15.ttf`
- `browser/fonts/comicz5_15.ttf`
- `browser/fonts/consola7_00.ttf`
- `browser/fonts/consolab7_00.ttf`
- `browser/fonts/consolai7_00.ttf`
- `browser/fonts/consolaz7_00.ttf`
- `browser/fonts/constan5_93.ttf`
- `browser/fonts/constanb5_93.ttf`
- `browser/fonts/constani5_93.ttf`
- `browser/fonts/constanz5_93.ttf`
- `browser/fonts/corbel6_01.ttf`
- `browser/fonts/corbelb6_01.ttf`
- `browser/fonts/corbeli6_01.ttf`
- `browser/fonts/corbelz6_01.ttf`
- `browser/fonts/cour6_94.ttf`
- `browser/fonts/courbd6_94.ttf`
- `browser/fonts/courbi6_94.ttf`
- `browser/fonts/couri6_94.ttf`
- `browser/fonts/ebrima5_19.ttf`
- `browser/fonts/ebrimabd5_19.ttf`
- `browser/fonts/framd5_02.ttf`
- `browser/fonts/framdit5_01.ttf`
- `browser/fonts/gabriola5_93.ttf`
- `browser/fonts/gadugi1_13.ttf`
- `browser/fonts/gadugib1_13.ttf`
- `browser/fonts/georgia5_59.ttf`
- `browser/fonts/georgiab5_59.ttf`
- `browser/fonts/georgiai5_59.ttf`
- `browser/fonts/georgiaz5_59.ttf`
- `browser/fonts/himalaya5_23.ttf`
- `browser/fonts/impact5_11.ttf`
- `browser/fonts/inkfree2_00.ttf`
- `browser/fonts/javatext1_10.ttf`
- `browser/fonts/leelauib5_06.ttf`
- `browser/fonts/leelawui5_06.ttf`
- `browser/fonts/malgun.ttf`
- `browser/fonts/marlett5_01.ttf`
- `browser/fonts/micross7_03.ttf`
- `browser/fonts/mingliub.ttc`
- `browser/fonts/mmrtext1_21.ttf`
- `browser/fonts/mmrtextb1_21.ttf`
- `browser/fonts/monbaiti5_54.ttf`
- `browser/fonts/msgothic5_32.ttc`
- `browser/fonts/msjh6_15.ttc`
- `browser/fonts/msyh6_31.ttc`
- `browser/fonts/msyi6_00.ttf`
- `browser/fonts/mvboli6_85.ttf`
- `browser/fonts/ntailu5_99.ttf`
- `browser/fonts/ntailub5_99.ttf`
- `browser/fonts/pala5_03.ttf`
- `browser/fonts/palab5_03.ttf`
- `browser/fonts/palabi5_03.ttf`
- `browser/fonts/palai5_03.ttf`
- `browser/fonts/phagspa6_00.ttf`
- `browser/fonts/phagspab6_00.ttf`
- `browser/fonts/segmdl21_86.ttf`
- `browser/fonts/segoeicons1_44.ttf`
- `browser/fonts/segoepr5_04.ttf`
- `browser/fonts/segoeprb5_04.ttf`
- `browser/fonts/segoesc5_03.ttf`
- `browser/fonts/segoescb5_03.ttf`
- `browser/fonts/segoeui5_67.ttf`
- `browser/fonts/segoeuib5_67.ttf`
- `browser/fonts/segoeuii5_66.ttf`
- `browser/fonts/segoeuil.ttf`
- `browser/fonts/segoeuisl.ttf`
- `browser/fonts/segoeuiz5_66.ttf`
- `browser/fonts/seguiemj1_60.ttf`
- `browser/fonts/seguihis1_09.ttf`
- `browser/fonts/seguisym6_24.ttf`
- `browser/fonts/seguivar2_03.ttf`
- `browser/fonts/simsun.ttc`
- `browser/fonts/simsunb5_08.ttf`
- `browser/fonts/sylfaen5_08.ttf`
- `browser/fonts/symbol5_01.ttf`
- `browser/fonts/tahoma7_04.ttf`
- `browser/fonts/tahomabd7_04.ttf`
- `browser/fonts/taile6_00.ttf`
- `browser/fonts/taileb6_00.ttf`
- `browser/fonts/times7_11.ttf`
- `browser/fonts/timesbd7_11.ttf`
- `browser/fonts/timesbi7_11.ttf`
- `browser/fonts/timesi7_11.ttf`
- `browser/fonts/trebuc5_15.ttf`
- `browser/fonts/trebucbd5_15.ttf`
- `browser/fonts/trebucbi5_15.ttf`
- `browser/fonts/trebucit5_15.ttf`
- `browser/fonts/verdana5_33.ttf`
- `browser/fonts/verdanab5_33.ttf`
- `browser/fonts/verdanai5_33.ttf`
- `browser/fonts/verdanaz5_33.ttf`
- `browser/fonts/webdings5_01.ttf`
- `browser/fonts/wingding5_01.ttf`
- `browser/fonts/yugothm1_95.ttc`
- `gfx/thebes/StealthBundleFontList.cpp`
- `gfx/thebes/StealthBundleFontList.h`

## Why StealthBundleFontList.h is exported

StealthBundleFontList.h is exported because dom/canvas reads the coverage ladder from it (the alpha-quantisation hook in CanvasRenderingContext2D).


## gfx/thebes/FontData.h: the Borrowed constructor

Hook: `FontData(const uint8_t*, uint32_t, Borrowed)`, the `mBorrowed` member and
the destructor that frees only what it owns. Until 2026-10-29 the borrow lived
in `FTUserFontData` (gfx/2d/2D.h, a constructor tagged `Prestito`, members
`mBorrowedData` / `mBorrowedLength`) with a second branch in its `CloneFace`
(gfx/2d/Factory.cpp). Firefox 159 removes `FTUserFontData`, and those two
hooks were the largest conflict of the 157 -> 159 merge (94 of our lines);
`FontData.h` is unchanged across those releases. Mozilla's `CloneFace` opens
a FontData with the same `FT_New_Memory_Face` on the same bytes and selects
the charmap, so the faces are the same.

The comment that stood on the constructor in 2D.h, kept unchanged:

STEALTH: i byte appartengono a QUALCUN ALTRO e non vanno liberati qui.

Simmetrico al costruttore in prestito di `gfxDWriteFontFileStream` (§5.2l):
su ENTRAMBE le piattaforme una sola mappatura per FILE viene prestata a
tutte le facce di quel file, invece di una copia per faccia. Il prestito e'
sicuro per la stessa ragione di la': la cache di `StealthBundleFontList` e'
immortale per costruzione - l'oggetto e' volutamente leaked e non svuota mai
`mFileCache` - e il valore della mappa e' un `RefPtr`, quindi se la tabella
hash rialloca a spostarsi e' il puntatore e mai i byte.

`FontData` was the owner that calls `free()` on its bytes, which is why the
first version needed a second pair of members in `FTUserFontData`; the
Borrowed constructor marks the bytes as not owned instead, so every caller
downstream sees an ordinary FontData.

## gfx/2d/ScaledFontDWrite.cpp: UnscaledFontDWrite::GetFontDescriptor

Hook: the early `if (!mPercorsoDichiarato.empty())` block that hands the declared
file path to the callback. The comment that stood on it in ScaledFontDWrite.cpp,
moved here unchanged except that "qui sotto" is replaced by the spot it named:

STEALTH: se il percorso e' DICHIARATO, si usa quello e si salta il resto.

La guardia `!mFont` che segue il blocco, in `GetFontDescriptor`, e' un PROXY:
"non e' un font di sistema" sta
per "non ha un file", e il commento al punto di costruzione lo dice
esplicitamente ("this signals whether or not we can safely query a
descriptor"). Per un font scaricato da una pagina e' corretto, perche' li'
un file non esiste. Per le nostre facce imbarcate e' falso: il file esiste
e lo conosciamo, e tacerlo fa mandare a WebRender i BYTE del font invece
del percorso. Misurati 65,9 MB nel processo padre su Linux, dove la stessa
correzione e' stata fatta per prima - `70-known-bugs.md` [B154].

Non si passa da `GetFontFileName` perche' quella pretende un
`IDWriteLocalFontFileLoader`, e il nostro caricatore presta byte in
memoria: non e' locale e non lo sara'. La convenzione dei byte e' la stessa
che usa lei, terminatore nullo compreso.

## gfx/2d/UnscaledFontDWrite.h: UnscaledFontDWrite constructor and mPercorsoDichiarato

Hook: the optional constructor argument `aFilePath` (default empty), the member
`std::wstring mPercorsoDichiarato` and the `<string>` include. The comment on the
constructor, moved here unchanged:

STEALTH: `aFilePath` e' il file da cui questa faccia viene, quando esiste.

Serve perche' `GetFontDescriptor` non riesce a ricavarlo da solo per le
nostre facce: quel percorso passa da `IDWriteLocalFontFileLoader`, e il
nostro caricatore presta byte in memoria, quindi non e' locale. Senza il
percorso Gecko ripiega sui BYTE del font verso WebRender - misurati 65,9 MB
nel processo padre su Linux, dove lo stesso difetto e' stato corretto per
primo. Vedi `70-known-bugs.md` [B154].

Chi non passa un percorso non vede nessuna differenza: resta il
comportamento upstream, che per un font scaricato da una pagina e' quello
giusto perche' li' un file non esiste davvero.

The comment on the member `mPercorsoDichiarato`:

STEALTH: vuoto per ogni font che non sia dei nostri imbarcati.

## Windows: the DirectWrite stream lends through FontData

Until 2026-10-30 gfxDWriteCommon.cpp and .h carried a second kind of
`gfxDWriteFontFileStream` that held lent bytes instead of a FontData (a
`Borrowed` constructor, `Bytes()` / `BytesLength()` in `GetFileSize` and
`ReadFileFragment`, and `CreateBorrowedFontFile`), and the body of
`CreateCustomFontFile` with its static stream registry had moved into a file of
ours: 36 of Mozilla's lines removed or rewritten in one file. It predates the
Borrowed constructor of FontData. Now `CreateStealthBundleFontEntry`
(StealthDWriteFontList.cpp) wraps the cache's bytes in a borrowed FontData and
calls Mozilla's `CreateCustomFontFile`, so both files are Mozilla's again; one
copy of each file still serves every face.

`gfxDWriteFontFileStream::SizeOfExcludingThis` reports 0 for every stream
(Mozilla's comment: "We don't report the size of mData, because the original
user font entry will account for that"). For a lent FontData that is also
right: the bundle cache is where those bytes belong and where they are
reported, and counting them again would double-count them in about:memory.

## gfx/thebes/gfxDWriteFontList.h: gfxDWriteFontEntry::mBundleFaceIndex and mBundleFilePath

Hook: the two members (and the `<string>` include). The comments that stood on them
in gfxDWriteFontList.h, moved here unchanged:

mBundleFaceIndex: Face index within a custom (bundle) font file; 0 for single-face files and
for system fonts. Lets a .ttc face be addressed by index in CreateFontFace.

mBundleFilePath: Bundle file path; see StealthDWriteFontList.cpp (CreateFontInstance).

## gfx/thebes/gfxFontEntry.h: gfxFontEntry::AutoHBFace conversion operator

Hook: `operator hb_face_t*() const&` and the deleted `operator hb_face_t*() const&&`
(feature hb-face-lifetime). The comment that stood on it in gfxFontEntry.h, moved
here unchanged:

La conversione vale solo su un LVALUE. Legarsi a un temporaneo e'
vietato dal TIPO, non da una convenzione:

        hb_face_t* f = entry->GetHBFace();   // non compila
        const auto f(entry->GetHBFace());    // corretto

GetHBFace() torna per valore, quindi scrivendo la prima forma il
temporaneo si converte e viene distrutto alla fine di quella stessa
istruzione: il puntatore e' penzolante dalla riga dopo, e chi lo usa
scrive dentro memoria liberata. Misurato il 2026-08-10 su
gfxHarfBuzzShaper::GetDesignFont(), che era l'unico dei quattordici
chiamanti a sbagliarlo: 6 sessioni di navigazione morte su 10 contro 0
dopo la correzione. Il difetto era silenzioso perche' la memoria appena
liberata resta quasi sempre leggibile, e il segmentation fault arrivava
altrove e molto dopo.

Sta nel TIPO e non solo in un gate perche' il progetto sapeva gia' la
regola in tredici punti su quattordici e il quattordicesimo e' stato
scritto sbagliato lo stesso: qui il compilatore lo rifiuta.

## gfx/thebes/gfxFontEntry.h: gfxFontEntry::ShouldPublishCharacterMap declaration

Hook: the declaration `bool ShouldPublishCharacterMap() const;` (defined in
gfxFontEntry.cpp, forwarding to `StealthBundleFontList::ShouldPublishCharacterMap`
in gfx/thebes/StealthBundleFontList.cpp, which explains each branch of the predicate
in its own comments). The comment
that stood on the declaration, moved here unchanged:

Stealth: does this entry's character map belong in the shared font list?
Every backend's ReadCMAP asked `!IsUserFont() && mShmemFace`, four copies of
one rule, and that rule is WRONG for a bundled face: it is constructed from
file data (so IsUserFont() is true) while being one of the list's own faces.
The consequence was that no bundled face's cmap ever reached shared memory.
One predicate, four readers.

## gfx/thebes/gfxFontUtils.h: FindTableDirEntry and GetTableFromFontData (aFaceIndex)

Hook: the new parameter `uint32_t aFaceIndex = 0` on `FindTableDirEntry` and
`GetTableFromFontData`, and the declaration of `FindSfntOffset` (defined in
gfx/thebes/StealthBundleFontList.cpp). In gfxFontUtils.h we had edited Mozilla's
doc comments of the two functions; Mozilla's wording is restored there and our
version is kept here, unchanged.

FindTableDirEntry:

Find the table directory entry for a given table tag, in a (validated)
buffer of font data (single-face sfnt or .ttc collection). aFaceIndex
selects the face within a .ttc (ignored for single-face fonts).
Returns null if the tag is not present.

GetTableFromFontData:

Return a blob that wraps a table found within a buffer of font data
(single-face sfnt or .ttc collection). aFaceIndex selects the face
within a .ttc (ignored for single-face fonts). Table offsets are
relative to the beginning of the file, so the blob points into aFontData.
The blob does NOT own its data; caller guarantees that the buffer
will remain valid at least as long as the blob.
Returns null if the specified table is not found.
This method assumes aFontData is valid font data; before using this,
caller is responsible to do any sanitization/validation necessary.

## gfx/thebes/gfxTextRun.cpp: gfxFontGroup::FindFallbackFaceForChar

Hook: the condition `pfl->MayDeferCmapLoading()` in place of the three-part
async-fallback condition. The comment that stood on it (the same text now also sits
at `gfxPlatformFontList::MayDeferCmapLoading` in gfx/thebes/StealthBundleFontList.cpp,
the function the hook calls):

Stealth: this is the site the bundle-only condition was missing from. The
twin site in GlobalFontFallback spelled it out by hand, this one (FindFallbackFaceForChar) did not, and
under bundle-only NO family is ever fully initialized - so every text run
that needed a fallback face within a family took this early return in the
content process. MayDeferCmapLoading is now the single answer for both.
