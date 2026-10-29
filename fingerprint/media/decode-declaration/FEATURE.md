# fingerprint/media/decode-declaration

Category: build, other, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/DecoderTraits.cpp` (CanHandleMediaType (comment above it), already-hook): Comment-only (13 lines) recording that a codec-string table was removed on 2026-08-08 and that the declaration now lives in PDMFactory::Supports (dom/media/StealthMediaDeclaration.h, not in this group). No code in the diff. Drop the comment from the patch.
- `dom/media/mediacapabilities/MediaCapabilities.cpp` (new file-scope StealthDeclaredDecodingInfoFor (hunk @@ -233) and the promise lambda in MediaCapabilities::CreateNonWebRTCDecodingInfo (hunk @@ -1159), movable): The helper loops over codecs and calls StealthDeclaredDecodingInfo (StealthMediaDeclaration.*, not in this group). Move the helper to StealthMediaDeclaration.cpp; the Mozilla file keeps the include and one call in the lambda that returns early. The declared answer overrides on both resolve branches, not only on reject. Comments are long measurement narratives (2026-08-08) and belong in docs. Audio
- `dom/media/moz.build` (EXPORTS += [...] and UNIFIED_SOURCES += [...] in dom/media/moz.build, already-hook): 
- `dom/media/platforms/PDMFactory.cpp` (PDMFactory::Supports(const SupportDecoderParams&, DecoderDoctorDiagnostics*) const, movable): The block maps two bools to a DecodeSupportSet inside Mozilla code. Movable: one hook such as an if-let on a StealthDecodeSupport(aParams) result, with the bit mapping in our file. The hook returns before the mEMEPDM->Supports branch, so declared answers also override EME answers when EME is present. Verify that is intended. [verified: CONFIRMED that the hook precedes the EME branch, PDMFactory.cp

## Our own files

- `dom/media/StealthMediaDeclaration.cpp`
- `dom/media/StealthMediaDeclaration.h`

## dom/media/DecoderTraits.cpp: CanHandleMediaType

Stealth: there is deliberately no declared-answer hook in DecoderTraits.cpp any
more, in front of CanHandleMediaType.

One lived at the top of CanHandleMediaType until 2026-08-08, matching whole type
STRINGS against a table from invisible_core. It could only ever hold the
strings somebody had seen: measured that day against retail Windows, our
Linux build differed on 12 rows and the table covered 3 of them, because the
space it was trying to enumerate - profile x constraint x level x container x
the audio codec travelling along - has no end.

The declaration moved one layer down, to PDMFactory::Supports, where the key
is a track mime type and the set is a dozen names Gecko defines itself. See
dom/media/StealthMediaDeclaration.h. Everything in DecoderTraits.cpp is then
Gecko's own host-independent logic and needs no help.
