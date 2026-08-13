/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_DWRITECOMMON_H
#define GFX_DWRITECOMMON_H

// Mozilla includes
#include "mozilla/MemoryReporting.h"
#include "mozilla/FontPropertyTypes.h"
#include "nscore.h"
#include "nsCOMPtr.h"
#include "gfxFontConstants.h"
#include "nsTArray.h"
#include "gfxWindowsPlatform.h"

#include <windows.h>
#include <dwrite.h>

#define GFX_CLEARTYPE_PARAMS "gfx.font_rendering.cleartype_params."
#define GFX_CLEARTYPE_PARAMS_GAMMA "gfx.font_rendering.cleartype_params.gamma"
#define GFX_CLEARTYPE_PARAMS_CONTRAST \
  "gfx.font_rendering.cleartype_params.enhanced_contrast"
#define GFX_CLEARTYPE_PARAMS_LEVEL \
  "gfx.font_rendering.cleartype_params.cleartype_level"
#define GFX_CLEARTYPE_PARAMS_STRUCTURE \
  "gfx.font_rendering.cleartype_params.pixel_structure"
#define GFX_CLEARTYPE_PARAMS_MODE \
  "gfx.font_rendering.cleartype_params.rendering_mode"

#define DISPLAY1_REGISTRY_KEY \
  HKEY_CURRENT_USER, L"Software\\Microsoft\\Avalon.Graphics\\DISPLAY1"

#define ENHANCED_CONTRAST_VALUE_NAME L"EnhancedContrastLevel"

// FIXME: This shouldn't look at constants probably.
static inline DWRITE_FONT_STRETCH DWriteFontStretchFromStretch(
    mozilla::FontStretch aStretch) {
  if (aStretch == mozilla::FontStretch::ULTRA_CONDENSED) {
    return DWRITE_FONT_STRETCH_ULTRA_CONDENSED;
  }
  if (aStretch == mozilla::FontStretch::EXTRA_CONDENSED) {
    return DWRITE_FONT_STRETCH_EXTRA_CONDENSED;
  }
  if (aStretch == mozilla::FontStretch::CONDENSED) {
    return DWRITE_FONT_STRETCH_CONDENSED;
  }
  if (aStretch == mozilla::FontStretch::SEMI_CONDENSED) {
    return DWRITE_FONT_STRETCH_SEMI_CONDENSED;
  }
  if (aStretch == mozilla::FontStretch::NORMAL) {
    return DWRITE_FONT_STRETCH_NORMAL;
  }
  if (aStretch == mozilla::FontStretch::SEMI_EXPANDED) {
    return DWRITE_FONT_STRETCH_SEMI_EXPANDED;
  }
  if (aStretch == mozilla::FontStretch::EXPANDED) {
    return DWRITE_FONT_STRETCH_EXPANDED;
  }
  if (aStretch == mozilla::FontStretch::EXTRA_EXPANDED) {
    return DWRITE_FONT_STRETCH_EXTRA_EXPANDED;
  }
  if (aStretch == mozilla::FontStretch::ULTRA_EXPANDED) {
    return DWRITE_FONT_STRETCH_ULTRA_EXPANDED;
  }
  return DWRITE_FONT_STRETCH_UNDEFINED;
}

static inline mozilla::FontStretch FontStretchFromDWriteStretch(
    DWRITE_FONT_STRETCH aStretch) {
  switch (aStretch) {
    case DWRITE_FONT_STRETCH_ULTRA_CONDENSED:
      return mozilla::FontStretch::ULTRA_CONDENSED;
    case DWRITE_FONT_STRETCH_EXTRA_CONDENSED:
      return mozilla::FontStretch::EXTRA_CONDENSED;
    case DWRITE_FONT_STRETCH_CONDENSED:
      return mozilla::FontStretch::CONDENSED;
    case DWRITE_FONT_STRETCH_SEMI_CONDENSED:
      return mozilla::FontStretch::SEMI_CONDENSED;
    case DWRITE_FONT_STRETCH_NORMAL:
      return mozilla::FontStretch::NORMAL;
    case DWRITE_FONT_STRETCH_SEMI_EXPANDED:
      return mozilla::FontStretch::SEMI_EXPANDED;
    case DWRITE_FONT_STRETCH_EXPANDED:
      return mozilla::FontStretch::EXPANDED;
    case DWRITE_FONT_STRETCH_EXTRA_EXPANDED:
      return mozilla::FontStretch::EXTRA_EXPANDED;
    case DWRITE_FONT_STRETCH_ULTRA_EXPANDED:
      return mozilla::FontStretch::ULTRA_EXPANDED;
    default:
      return mozilla::FontStretch::NORMAL;
  }
}

class gfxDWriteFontFileStream final : public IDWriteFontFileStream {
 public:
  /**
   * Used by the FontFileLoader to create a new font stream,
   * this font stream is created from data in memory. The memory
   * passed may be released after object creation, it will be
   * copied internally.
   *
   * @param aData Font data
   */
  gfxDWriteFontFileStream(const uint8_t* aData, uint32_t aLength,
                          uint64_t aFontFileKey);

  /**
   * Stealth: the same thing WITHOUT the copy. The caller LENDS bytes it
   * guarantees outlive the stream; the only user is the bundled-font cache in
   * StealthBundleFontList, which is immortal by construction - that object is
   * deliberately leaked and never clears the cache, and it stores refcounted
   * blobs so rehashing cannot move the bytes.
   *
   * Why it exists: the bundle is 218.7 MB over 126 files, the largest a 35.4 MB
   * .ttc with two faces, and DirectWrite holds one stream per FACE. Copying
   * meant a permanent copy per face on top of the cache's own, so that single
   * file cost 106 MB where 35.4 is enough.
   */
  enum class Borrowed { Yes };
  gfxDWriteFontFileStream(const uint8_t* aData, uint32_t aLength,
                          uint64_t aFontFileKey, Borrowed);

  ~gfxDWriteFontFileStream();

  // IUnknown interface
  IFACEMETHOD(QueryInterface)(IID const& iid, OUT void** ppObject) {
    if (iid == __uuidof(IDWriteFontFileStream)) {
      *ppObject = static_cast<IDWriteFontFileStream*>(this);
      return S_OK;
    } else if (iid == __uuidof(IUnknown)) {
      *ppObject = static_cast<IUnknown*>(this);
      return S_OK;
    } else {
      return E_NOINTERFACE;
    }
  }

  IFACEMETHOD_(ULONG, AddRef)() {
    MOZ_ASSERT(int32_t(mRefCnt) >= 0, "illegal refcnt");
    return ++mRefCnt;
  }

  IFACEMETHOD_(ULONG, Release)();

  // IDWriteFontFileStream methods
  virtual HRESULT STDMETHODCALLTYPE
  ReadFileFragment(void const** fragmentStart, UINT64 fileOffset,
                   UINT64 fragmentSize, OUT void** fragmentContext);

  virtual void STDMETHODCALLTYPE ReleaseFileFragment(void* fragmentContext);

  virtual HRESULT STDMETHODCALLTYPE GetFileSize(OUT UINT64* fileSize);

  virtual HRESULT STDMETHODCALLTYPE GetLastWriteTime(OUT UINT64* lastWriteTime);

  // Deliberately reports mData only: a borrowed stream owns no bytes, and
  // counting the lender's would double-count them in about:memory. The bundle
  // cache is where those bytes belong and where they should be reported.
  size_t SizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mData.ShallowSizeOfExcludingThis(mallocSizeOf);
  }

  size_t SizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + SizeOfExcludingThis(mallocSizeOf);
  }

 private:
  // Exactly one of these two is in use: mData when the stream OWNS its copy,
  // mBorrowed when it was lent bytes that outlive it. Length() is not a valid
  // stand-in for "which one", because an owned stream whose allocation failed is
  // legitimately empty - hence the explicit pointer.
  const uint8_t* Bytes() const { return mBorrowed ? mBorrowed : mData.Elements(); }
  uint32_t BytesLength() const { return mBorrowed ? mBorrowedLength : mData.Length(); }

  FallibleTArray<uint8_t> mData;
  const uint8_t* mBorrowed = nullptr;
  uint32_t mBorrowedLength = 0;
  mozilla::Atomic<uint32_t> mRefCnt;
  uint64_t mFontFileKey;
};

class gfxDWriteFontFileLoader : public IDWriteFontFileLoader {
 public:
  gfxDWriteFontFileLoader() {}

  // IUnknown interface
  IFACEMETHOD(QueryInterface)(IID const& iid, OUT void** ppObject) {
    if (iid == __uuidof(IDWriteFontFileLoader)) {
      *ppObject = static_cast<IDWriteFontFileLoader*>(this);
      return S_OK;
    } else if (iid == __uuidof(IUnknown)) {
      *ppObject = static_cast<IUnknown*>(this);
      return S_OK;
    } else {
      return E_NOINTERFACE;
    }
  }

  IFACEMETHOD_(ULONG, AddRef)() { return 1; }

  IFACEMETHOD_(ULONG, Release)() { return 1; }

  // IDWriteFontFileLoader methods
  /**
   * Important! Note the key here -has- to be a pointer to a uint64_t.
   */
  virtual HRESULT STDMETHODCALLTYPE CreateStreamFromKey(
      void const* fontFileReferenceKey, UINT32 fontFileReferenceKeySize,
      OUT IDWriteFontFileStream** fontFileStream);

  /**
   * Gets the singleton loader instance. Note that when using this font
   * loader, the key must be a pointer to a unint64_t.
   */
  static IDWriteFontFileLoader* Instance() {
    if (!mInstance) {
      mInstance = new gfxDWriteFontFileLoader();
      mozilla::gfx::Factory::GetDWriteFactory()->RegisterFontFileLoader(
          mInstance);
    }
    return mInstance;
  }

  /**
   * Creates a IDWriteFontFile and IDWriteFontFileStream from aFontData.
   * The data from aFontData will be copied internally, so the caller
   * is free to dispose of it once this method returns.
   *
   * @param aFontData the font data for the custom font file
   * @param aLength length of the font data
   * @param aFontFile out param for the created font file
   * @param aFontFileStream out param for the corresponding stream
   * @return HRESULT of internal calls
   */
  static HRESULT CreateCustomFontFile(
      const uint8_t* aFontData, uint32_t aLength, IDWriteFontFile** aFontFile,
      gfxDWriteFontFileStream** aFontFileStream);

  /**
   * Stealth: as above, but the stream BORROWS aFontData instead of copying it.
   * Only for bytes that outlive the stream; today that is exactly the bundled
   * font cache. See gfxDWriteFontFileStream's Borrowed constructor.
   */
  static HRESULT CreateBorrowedFontFile(
      const uint8_t* aFontData, uint32_t aLength, IDWriteFontFile** aFontFile,
      gfxDWriteFontFileStream** aFontFileStream);

  size_t SizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

 private:
  static IDWriteFontFileLoader* mInstance;
};

#endif /* GFX_DWRITECOMMON_H */
