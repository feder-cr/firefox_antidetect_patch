/*
 * Port of SkTypeface_win_dw.h (DWriteFontTypeface) over idwr: no COM, no <dwrite*.h>.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef SkTypeface_idwr_priv_DEFINED
#define SkTypeface_idwr_priv_DEFINED

#define IDWR_STATIC 1
#include "idwr/idwr.h"

#include "include/core/SkColor.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkTypeface_idwr.h"
#include "include/private/base/SkMutex.h"
#include "src/core/SkAdvancedTypefaceMetrics.h"

#include <vector>

/** The opened font, shared by every clone of a typeface. idwr borrows the bytes.
 *
 *  An idwr_font, and every face, outline scaler and texture made from it, belongs to one thread at
 *  a time: the font carries lazy caches (the CFF metrics rasterizer) that calls taking it as
 *  const write. Skia locks per strike, and two strikes of one typeface (two sizes) run on two
 *  threads, so every idwr call that touches the font or anything derived from it holds fMutex.
 *  The Windows original has no such lock (DirectWrite is thread safe; maybe_dw_mutex is for
 *  Windows 8) and idwr is not DirectWrite, hence the lock here. One lock for the font, rather
 *  than one font per scaler context: the font is opened once from the shared bytes, the face
 *  tables and glyph lookups read it without a second copy, and a strike is not slowed by more
 *  than the other strikes of the same typeface. */
struct IdwrFontHolder : public SkNVRefCnt<IdwrFontHolder> {
    IdwrFontHolder(sk_sp<SkData> data, uint32_t index);
    ~IdwrFontHolder();
    sk_sp<SkData> fData;
    uint32_t fIndex;
    idwr_font* fFont;
    SkMutex fMutex;
};

class IdwrFontTypeface : public SkTypeface {
public:
    static constexpr SkTypeface::FactoryId FactoryId = SkSetFourByteTag('i', 'd', 'w', 'r');

    static sk_sp<IdwrFontTypeface> Make(sk_sp<IdwrFontHolder> font,
                                        uint32_t simulations,
                                        std::vector<idwr_axis_value> axes,
                                        SkFontStyle style,
                                        const SkFontArguments::Palette& palette,
                                        int renderingMode,
                                        float gamma,
                                        float contrast,
                                        float clearTypeLevel);

    ~IdwrFontTypeface() override;

    const idwr_font* idwrFont() const { return fHolder->fFont; }
    /** Held around every idwr call that uses idwrFont() or a face, scaler or texture of it. */
    SkMutex& fontMutex() const { return fHolder->fMutex; }
    uint32_t simulations() const { return fSimulations; }
    const std::vector<idwr_axis_value>& axes() const { return fAxes; }
    /** idwr_normalize_axes of axes(): what the outline scaler takes. */
    const std::vector<float>& coords() const { return fCoords; }
    int glyphCount() const { return fGlyphCount; }
    bool isColorFont() const { return fIsColorFont; }
    const std::vector<SkColor>& palette() const { return fPalette; }

    bool ForceGDI() const { return fRenderingMode == IDWR_MODE_GDI_CLASSIC; }
    int GetRenderingMode() const { return fRenderingMode; }
    float GetClearTypeLevel() const { return fClearTypeLevel; }

protected:
    sk_sp<SkTypeface> onMakeClone(const SkFontArguments&) const override;
    std::unique_ptr<SkStreamAsset> onOpenStream(int* ttcIndex) const override;
    std::unique_ptr<SkScalerContext> onCreateScalerContext(const SkScalerContextEffects&,
                                                           const SkDescriptor*) const override;
    void onFilterRec(SkScalerContextRec*) const override;
    void getGlyphToUnicodeMap(SkSpan<SkUnichar>) const override;
    std::unique_ptr<SkAdvancedTypefaceMetrics> onGetAdvancedMetrics() const override;
    void onGetFontDescriptor(SkFontDescriptor*, bool*) const override;
    void onCharsToGlyphs(SkSpan<const SkUnichar>, SkSpan<SkGlyphID>) const override;
    int onCountGlyphs() const override;
    void getPostScriptGlyphNames(SkString*) const override;
    int onGetUPEM() const override;
    void onGetFamilyName(SkString* familyName) const override;
    bool onGetPostScriptName(SkString*) const override;
    int onGetResourceName(SkString*) const override;
    SkTypeface::LocalizedStrings* onCreateFamilyNameIterator() const override;
    bool onGlyphMaskNeedsCurrentColor() const override;
    int onGetVariationDesignPosition(
            SkSpan<SkFontArguments::VariationPosition::Coordinate>) const override;
    int onGetVariationDesignParameters(SkSpan<SkFontParameters::Variation::Axis>) const override;
    bool onIsSyntheticBold() const override;
    bool onIsSyntheticOblique() const override;
    int onGetTableTags(SkSpan<SkFontTableTag>) const override;
    size_t onGetTableData(SkFontTableTag, size_t offset, size_t length, void* data) const override;
    sk_sp<SkData> onCopyTableData(SkFontTableTag) const override;

private:
    IdwrFontTypeface(sk_sp<IdwrFontHolder> font,
                     uint32_t simulations,
                     std::vector<idwr_axis_value> axes,
                     SkFontStyle style,
                     const SkFontArguments::Palette& palette);
    void initialize();

    sk_sp<IdwrFontHolder> fHolder;
    uint32_t fSimulations;
    std::vector<idwr_axis_value> fAxes;
    std::vector<float> fCoords;
    int fGlyphCount = 0;
    int fUpem = 0;
    bool fIsColorFont = false;
    uint32_t fPaletteCount = 0;
    std::vector<SkColor> fPalette;
    std::vector<SkFontArguments::Palette::Override> fPaletteOverrides;
    SkFontArguments::Palette fRequestedPalette;

    int fRenderingMode = IDWR_MODE_DEFAULT;
    float fGamma = 2.2f;
    float fContrast = 1.0f;
    float fClearTypeLevel = 1.0f;
    using INHERITED = SkTypeface;
};

#endif
