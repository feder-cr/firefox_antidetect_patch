/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// DirectWrite font file streams that borrow their bytes (fingerprint/fonts/
// bundle). Windows only: listed in the Windows SOURCES of
// StealthBundleFontList.mozbuild; gfxDWriteCommon.cpp's hooks call into this
// file. Non-unified.

#include "StealthFontFileStream.h"

#include "FontData.h"
#include "mozilla/Assertions.h"
#include "mozilla/RefPtr.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Logging.h"
#include "nsDebug.h"

using namespace mozilla;

StaticMutex sFontFileStreamsMutex MOZ_UNANNOTATED;
uint64_t sNextFontFileKey = 0;
MOZ_RUNINIT std::unordered_map<uint64_t, gfxDWriteFontFileStream*>
    sFontFileStreams;

// Stealth: a stream that does NOT hold a FontData. The caller LENDS bytes it
// guarantees outlive the stream; the only user is the bundled-font cache in
// StealthBundleFontList, which is immortal by construction - that object is
// deliberately leaked and never clears the cache, and it stores refcounted
// blobs so rehashing cannot move the bytes.
//
// Why it exists: the bundle is 218.7 MB over 126 files, the largest a 35.4 MB
// .ttc with two faces, and DirectWrite holds one stream per FACE. A FontData
// owns a malloc'd buffer and frees it (gfx/thebes/FontData.h), so wrapping
// the cache's bytes in one means a permanent copy per face on top of the
// cache's own: that single file cost 106 MB where 35.4 is enough.
//
// Nothing to copy. The caller guarantees the bytes outlive this object; the
// paragraphs above say why that guarantee holds for the only caller.
gfxDWriteFontFileStream::gfxDWriteFontFileStream(const uint8_t* aData,
                                                 uint32_t aLength,
                                                 uint64_t aFontFileKey,
                                                 Borrowed)
    : mBorrowed(aData), mBorrowedLength(aLength), mFontFileKey(aFontFileKey) {}

// Bytes() and BytesLength(), declared in gfxDWriteCommon.h. Exactly one of
// these two is in use: mData when the stream holds a FontData, mBorrowed when it
// was lent bytes that outlive it (then mData is null). Defined in the .cpp (this
// file) because FontData is only forward-declared in the header.
const uint8_t* gfxDWriteFontFileStream::Bytes() const {
  return mBorrowed ? mBorrowed : mData->Data();
}

uint32_t gfxDWriteFontFileStream::BytesLength() const {
  return mBorrowed ? mBorrowedLength : mData->Length();
}

// The body of the two entry points below, which differ ONLY in whether the
// stream holds a FontData or borrows the bytes. Written as one function instead
// of two so the registration, the key allocation and the locking cannot drift
// apart. aBorrowedData non-null selects the borrowed stream (aFontData is then
// ignored and may be null).
static HRESULT CreateFontFileImpl(FontData* aFontData,
                                  const uint8_t* aBorrowedData,
                                  uint32_t aBorrowedLength,
                                  IDWriteFontFile** aFontFile,
                                  gfxDWriteFontFileStream** aFontFileStream) {
  MOZ_ASSERT(aFontFile);
  MOZ_ASSERT(aFontFileStream);

  RefPtr<IDWriteFactory> factory = gfx::Factory::GetDWriteFactory();
  if (!factory) {
    gfxCriticalError()
        << "Failed to get DWrite Factory in CreateCustomFontFile.";
    return E_FAIL;
  }

  sFontFileStreamsMutex.Lock();
  uint64_t fontFileKey = sNextFontFileKey++;
  RefPtr<gfxDWriteFontFileStream> ffsRef =
      aBorrowedData ? new gfxDWriteFontFileStream(
                          aBorrowedData, aBorrowedLength, fontFileKey,
                          gfxDWriteFontFileStream::Borrowed::Yes)
                    : new gfxDWriteFontFileStream(aFontData, fontFileKey);
  sFontFileStreams[fontFileKey] = ffsRef;
  sFontFileStreamsMutex.Unlock();

  RefPtr<IDWriteFontFile> fontFile;
  HRESULT hr = factory->CreateCustomFontFileReference(
      &fontFileKey, sizeof(fontFileKey),
      gfxDWriteFontFileLoader::Instance(), getter_AddRefs(fontFile));
  if (FAILED(hr)) {
    NS_WARNING("Failed to load font file from data!");
    return hr;
  }

  fontFile.forget(aFontFile);
  ffsRef.forget(aFontFileStream);

  return S_OK;
}

HRESULT StealthCreateOwningFontFile(FontData* aFontData,
                                    IDWriteFontFile** aFontFile,
                                    gfxDWriteFontFileStream** aFontFileStream) {
  return CreateFontFileImpl(aFontData, /* aBorrowedData */ nullptr,
                            /* aBorrowedLength */ 0, aFontFile,
                            aFontFileStream);
}

// Lends the bytes to the stream instead of copying them; see the comment on
// gfxDWriteFontFileStream's Borrowed constructor above for why the bytes outlive
// it.
//
// Stealth: as gfxDWriteFontFileLoader::CreateCustomFontFile, but the stream
// BORROWS aFontData instead of copying it. Only for bytes that outlive the
// stream; today that is exactly the bundled font cache. See
// gfxDWriteFontFileStream's Borrowed constructor.
HRESULT gfxDWriteFontFileLoader::CreateBorrowedFontFile(
    const uint8_t* aFontData, uint32_t aLength, IDWriteFontFile** aFontFile,
    gfxDWriteFontFileStream** aFontFileStream) {
  return CreateFontFileImpl(/* aFontData */ nullptr, aFontData, aLength,
                            aFontFile, aFontFileStream);
}
