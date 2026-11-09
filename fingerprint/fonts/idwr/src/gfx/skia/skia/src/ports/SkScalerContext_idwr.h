/*
 * Port of SkScalerContext_win_dw.h (SkScalerContext_DW) over idwr: every DirectWrite call of the
 * original is the idwr call with the same arguments, the mode rules and the mask conversions are
 * the original's. No COM, no <dwrite*.h>, no COLRv1 (Gecko paints it), no SVG or PNG glyphs.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkScalerContext_idwr_DEFINED
#define SkScalerContext_idwr_DEFINED

#define IDWR_STATIC 1
#include "idwr/idwr.h"

#include "include/core/SkScalar.h"
#include "include/core/SkTypes.h"
#include "include/private/base/SkTDArray.h"
#include "src/core/SkScalerContext.h"
#include "src/ports/SkTypeface_idwr.h"

class SkGlyph;
class SkDescriptor;
class SkPathBuilder;

class SkScalerContext_Idwr : public SkScalerContext {
public:
    SkScalerContext_Idwr(IdwrFontTypeface&, const SkScalerContextEffects&, const SkDescriptor*);
    ~SkScalerContext_Idwr() override;

protected:
    GlyphMetrics generateMetrics(const SkGlyph&, SkArenaAlloc*) override;
    void generateImage(const SkGlyph&, void* imageBuffer) override;
    std::optional<GeneratedPath> generatePath(const SkGlyph&) override;
    sk_sp<SkDrawable> generateDrawable(const SkGlyph&) override;
    void generateFontMetrics(SkFontMetrics*) override;

private:
    bool setAdvance(const SkGlyph&, SkVector*);

    struct ScalerContextBits {
        using value_type = uint16_t;
        static const constexpr value_type NONE = 0;
        static const constexpr value_type DW = 1;
        static const constexpr value_type DW_1 = 2;
        static const constexpr value_type COLR = 5;
        static const constexpr value_type PATH = 7;
    };

    static void BilevelToBW(const uint8_t* SK_RESTRICT src, const SkGlyph& glyph, void* dst);

    template <bool APPLY_PREBLEND>
    static void GrayscaleToA8(const uint8_t* SK_RESTRICT src,
                              const SkGlyph& glyph,
                              void* dst,
                              const uint8_t* table8);

    template <bool APPLY_PREBLEND>
    static void RGBToA8(const uint8_t* SK_RESTRICT src,
                        const SkGlyph& glyph,
                        void* dst,
                        const uint8_t* table8);

    template <bool APPLY_PREBLEND, bool RGB>
    static void RGBToLcd16(const uint8_t* SK_RESTRICT src,
                           const SkGlyph& glyph,
                           void* dst,
                           const uint8_t* tableR,
                           const uint8_t* tableG,
                           const uint8_t* tableB,
                           int clearTypeLevel);

    IdwrFontTypeface* getIdwrTypeface() {
        return static_cast<IdwrFontTypeface*>(this->getTypeface());
    }

    // GetGlyphRunOutline of one run through the kept outline scaler, as an SkPath. The caller
    // holds the font lock (IdwrFontHolder::fMutex), as for getColorGlyphRun.
    bool runOutline(float emSize,
                    const uint16_t* glyphs,
                    const float* advances,
                    const idwr_glyph_offset* offsets,
                    uint32_t count,
                    int sideways,
                    int rtl,
                    SkPathBuilder* builder);

    // The font lock is held by the caller (the enumerator it returns belongs to the font too).
    bool getColorGlyphRun(const SkGlyph&, idwr_color_run_enum**);
    bool generateColorMetrics(const SkGlyph&, SkRect*);
    bool generateColorImage(const SkGlyph&, void* dst);
    bool drawColorImage(const SkGlyph&, SkCanvas&);

    // The idwr_glyph_run of one glyph at fTextSizeRender, as both CreateGlyphRunAnalysis call
    // sites of the original build it (the v1/v2 overload rule included).
    void makeRun(idwr_glyph_run*, const uint16_t* glyph, const float* advance,
                 const idwr_glyph_offset* offset, int renderingMode) const;

    bool generateDWMetrics(const SkGlyph&, int renderingMode, int textureType, SkRect*);
    const void* getDWMaskBits(const SkGlyph&, int renderingMode, int textureType);
    bool generateDWImage(const SkGlyph&, void* dst);

    SkTDArray<uint8_t> fBits;
    /** The total matrix without the text height scale. */
    SkMatrix fSkXform;
    /** The total matrix without the text height scale. */
    idwr_matrix fXform;
    /** The text size to render with. */
    SkScalar fTextSizeRender;
    /** The text size to measure with. */
    SkScalar fTextSizeMeasure;
    int fGlyphCount;
    int fRenderingMode;   // IDWR_MODE_*
    int fTextureType;     // IDWR_TEXTURE_*
    int fMeasuringMode;   // IDWR_MEASURING_*
    int fAntiAliasMode;   // IDWR_AA_*
    int fGridFitMode;     // IDWR_GRID_FIT_*
    int fClearTypeLevel;

    // Owned by this context: an idwr_face is not thread safe (its GDI calls cache a scaler),
    // and Skia locks per strike, not per typeface. The idwr_font is the only shared object.
    idwr_face* fFace = nullptr;
    idwr_outline_scaler* fOutline = nullptr;
};

#endif
