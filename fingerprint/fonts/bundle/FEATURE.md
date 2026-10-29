# fingerprint/fonts/bundle

Category: build, other, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `browser/fonts/moz.build` ((top level of browser/fonts/moz.build), already-hook): Replaces the single TwemojiMozilla.ttf install with a FINAL_TARGET_FILES list of 121 quoted entries (bundled Windows core fonts plus bundle-fonts.list and TwemojiMozilla.ttf, which stays in the list). Adds cocoa to the platform condition. The list is hand-maintained here and must stay in sync with the bundle manifest. The comment says macOS also needs --enable-bundled-fonts in its mozconfig. The f
- `browser/installer/package-manifest.in` ((packaging manifest, fonts entry), already-hook): Packages @RESPATH@/fonts/* on macOS as well (upstream: Windows and GTK only). Packaging condition only; supports the bundled font set on every host.
- `gfx/2d/2D.h` (class FTUserFontData (new borrowing constructor, GetData, Length, members mBorrowedData and mBorrowedLength), already-hook): Category is other rather than spoof: it lends the bundled font bytes (owned by StealthBundleFontList) to each face instead of copying the file per face, and no page-observable value changes. The lifetime rule is an invariant, not enforced here: the lent bytes must outlive every face, which holds only because the bundle cache is immortal (confirmed: gfx/thebes/StealthBundleFontList.cpp:532-534 leak
- `gfx/2d/Factory.cpp` (FTUserFontData::CloneFace, already-hook): The borrowed branch creates the FT_Face from the lent bytes, which FreeType keeps a pointer to, so the bytes must outlive the face (guaranteed only by the immortal bundle cache, see 2D.h). The branch (about 5 lines) can move to an own helper. Verified that Factory::NewFTFaceFromData exists at gfx/2d/Factory.cpp:575 in C:\ff\source. The function sits inside #ifdef MOZ_ENABLE_FREETYPE (Factory.cpp:1
- `gfx/2d/ScaledFontDWrite.cpp` (UnscaledFontDWrite::GetFontDescriptor, already-hook): Returns the declared file path (UTF-16 with NUL terminator, same convention as the Mozilla file-name path) when one was passed to the constructor. Otherwise it falls through to Mozilla's own `if (!mFont) return false;` guard, which is left unchanged. The comment reports 65.9 MB sent to WebRender as bytes instead of a path ([B154]). Category is other, not spoof: the change alters what gfx passes to
- `gfx/2d/UnscaledFontDWrite.h` (UnscaledFontDWrite constructor and member list (class UnscaledFontDWrite final : public UnscaledFont), already-hook): Adds an optional aFilePath argument with an empty default, so existing callers keep the upstream behaviour. Adds member mPercorsoDichiarato (std::wstring, ours) and the <string> include. Comment cross-references [B154].
- `gfx/thebes/gfxDWriteCommon.cpp` (gfxDWriteFontFileStream::GetFileSize; gfxDWriteFontFileStream::ReadFileFragment; gfxDWriteFontFileStream::Bytes and BytesLength (new); gfxDWriteFontFileLoader::CreateCustomFontFile (refactored into static CreateFontFileImpl); gfxDWriteFontFileLoader::CreateBorrowedFontFile (new), hard): Adds a non-owning DirectWrite stream that lends bytes to the immortal bundle cache instead of copying them into a FontData (comment: 106 MB where 35.4 MB is enough for one 35.4 MB .ttc). Hard to hook: the borrowed state lives in private members of Mozilla's gfxDWriteFontFileStream, and the two IDWriteFontFileStream methods that read them are Mozilla code. The CreateCustomFontFile body was also mov
- `gfx/thebes/gfxDWriteCommon.h` (gfxDWriteFontFileStream (class: new Borrowed constructor, Bytes/BytesLength declarations, mBorrowed/mBorrowedLength members, SizeOfExcludingThis); gfxDWriteFontFileLoader::CreateBorrowedFontFile declaration, already-hook): Declarations for the borrowed stream. SizeOfExcludingThis still reports 0 for a borrowed stream, because the bundle cache reports those bytes. The header comment states the immortal-cache lifetime rule that the code depends on.
- `gfx/thebes/gfxDWriteFontList.h` (gfxDWriteFontEntry (class members mBundleFaceIndex and mBundleFilePath), already-hook): Two members: mBundleFaceIndex (feature font-ttc-face-index, index inside a .ttc) and mBundleFilePath (feature font-bundle-file-path, the path given to WebRender). Only the path member is in this file's primary feature. Adds the <string> include.
- `gfx/thebes/gfxFT2FontList.cpp` (FT2FontEntry::ReadCMAP; FT2FontEntry::GetFontTableInternal, already-hook): ReadCMAP now uses gfxFontEntry::ShouldPublishCharacterMap (see gfxFontEntry.cpp). Second hunk, feature font-ttc-face-index: the face index (face_index & 0x7FFF) is passed to GetTableFromFontData so tables are read from the right face of a .ttc.
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


## gfx/2d/2D.h: FTUserFontData borrowing constructor (Prestito)

Hook: the constructor `FTUserFontData(const uint8_t* aData, uint32_t aLength, Prestito)`
with the `enum class Prestito { Si }` tag, the changed `GetData()` / `Length()`
and the members `mBorrowedData` / `mBorrowedLength` (all in class `FTUserFontData`).
The comment that stood on the constructor in 2D.h, moved here unchanged:

STEALTH: i byte appartengono a QUALCUN ALTRO e non vanno liberati qui.

Simmetrico al costruttore in prestito di `gfxDWriteFontFileStream` (§5.2l):
su ENTRAMBE le piattaforme una sola mappatura per FILE viene prestata a
tutte le facce di quel file, invece di una copia per faccia. Il prestito e'
sicuro per la stessa ragione di la': la cache di `StealthBundleFontList` e'
immortale per costruzione - l'oggetto e' volutamente leaked e non svuota mai
`mFileCache` - e il valore della mappa e' un `RefPtr`, quindi se la tabella
hash rialloca a spostarsi e' il puntatore e mai i byte.

Serve un costruttore NUOVO: `FontData` e' il proprietario che chiama
`free()` sui suoi byte, quindi un puntatore mappato non puo' stare li'
dentro. I byte prestati restano in `mBorrowedData` / `mBorrowedLength`,
`mFontData` resta nullo, e `GetData()` / `Length()` rispondono per
entrambi i casi, cosi' i chiamanti non sanno nulla del prestito.
`CloneFace` (gfx/2d/Factory.cpp) apre i byte prestati quando manca
`mFontData`.

## gfx/2d/Factory.cpp: FTUserFontData::CloneFace

Hook: the borrowed branch (`mBorrowedData`, `Factory::NewFTFaceFromData`). The
one-line comment that stood on it in Factory.cpp, moved here unchanged:

STEALTH: bundle bytes lent by StealthBundleFontList (see 2D.h).

(See the section on gfx/2d/2D.h above: the bytes are lent by the immortal bundle
cache.)

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

## gfx/thebes/gfxDWriteCommon.h: gfxDWriteFontFileStream::SizeOfExcludingThis

Hook: none in code (`SizeOfExcludingThis` still returns 0). Mozilla's comment in
the header there says "We don't report the size of mData, because the original
user font entry will account for that."; the sentence we appended to it, moved
here unchanged:

A borrowed stream owns no bytes either, and
counting the lender's would double-count them in about:memory: the
bundle cache is where those bytes belong and where they are reported.

(The Borrowed constructor, `Bytes()` / `BytesLength()` and
`CreateBorrowedFontFile` carry their comments in gfx/thebes/StealthFontFileStream.cpp;
the members `mBorrowed` / `mBorrowedLength` are described by the comment on
`Bytes()` / `BytesLength()` there.)

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
