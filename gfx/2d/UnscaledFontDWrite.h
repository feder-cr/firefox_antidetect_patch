/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_UNSCALEDFONTDWRITE_H_
#define MOZILLA_GFX_UNSCALEDFONTDWRITE_H_

#include <dwrite.h>

#include <string>
#include <vector>

#include "2D.h"

namespace mozilla {
namespace gfx {

class ScaledFontDWrite;

class UnscaledFontDWrite final : public UnscaledFont {
 public:
  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(UnscaledFontDWrite, override)
  // STEALTH: `aFilePath` e' il file da cui questa faccia viene, quando esiste.
  //
  // Serve perche' `GetFontDescriptor` non riesce a ricavarlo da solo per le
  // nostre facce: quel percorso passa da `IDWriteLocalFontFileLoader`, e il
  // nostro caricatore presta byte in memoria, quindi non e' locale. Senza il
  // percorso Gecko ripiega sui BYTE del font verso WebRender - misurati 65,9 MB
  // nel processo padre su Linux, dove lo stesso difetto e' stato corretto per
  // primo. Vedi `70-known-bugs.md` [B154].
  //
  // Chi non passa un percorso non vede nessuna differenza: resta il
  // comportamento upstream, che per un font scaricato da una pagina e' quello
  // giusto perche' li' un file non esiste davvero.
  UnscaledFontDWrite(const RefPtr<IDWriteFontFace>& aFontFace,
                     const RefPtr<IDWriteFont>& aFont,
                     const std::wstring& aFilePath = std::wstring())
      : mFontFace(aFontFace), mFont(aFont), mPercorsoDichiarato(aFilePath) {}

  FontType GetType() const override { return FontType::DWRITE; }

  const RefPtr<IDWriteFontFace>& GetFontFace() const { return mFontFace; }
  const RefPtr<IDWriteFont>& GetFont() const { return mFont; }

  bool GetFontFileData(FontFileDataOutput aDataCallback, void* aBaton) override;

  already_AddRefed<ScaledFont> CreateScaledFont(
      Float aGlyphSize, const uint8_t* aInstanceData,
      uint32_t aInstanceDataLength, const FontVariation* aVariations,
      uint32_t aNumVariations) override;

  already_AddRefed<ScaledFont> CreateScaledFontFromWRFont(
      Float aGlyphSize, const wr::FontInstanceOptions* aOptions,
      const wr::FontInstancePlatformOptions* aPlatformOptions,
      const FontVariation* aVariations, uint32_t aNumVariations) override;

  bool GetFontDescriptor(FontDescriptorOutput aCb, void* aBaton) override;

  static already_AddRefed<UnscaledFont> CreateFromFontDescriptor(
      const uint8_t* aData, uint32_t aDataLength, uint32_t aIndex);

 private:
  bool InitBold();

  RefPtr<IDWriteFontFace> mFontFace;
  RefPtr<IDWriteFontFace> mFontFaceBold;
  RefPtr<IDWriteFont> mFont;
  std::vector<WCHAR> mFontFileName;
  // STEALTH: vuoto per ogni font che non sia dei nostri imbarcati.
  std::wstring mPercorsoDichiarato;
};

}  // namespace gfx
}  // namespace mozilla

#endif /* MOZILLA_GFX_UNSCALEDFONTDWRITE_H_ */
