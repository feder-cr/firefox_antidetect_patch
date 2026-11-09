/*
 * Port of SkScalerContext_win_dw.cpp (SkScalerContext_DW) over idwr. The constructor's mode
 * cascade, setAdvance, generateMetrics with the aliased retry and the PATH fallback,
 * generateFontMetrics and the mask conversions are the original's line for line. Each
 * DirectWrite call is the idwr call with the same arguments:
 *   CreateGlyphRunAnalysis (v1 or v2) / GetAlphaTextureBounds / CreateAlphaTexture
 *                                           -> idwr_glyph_run_texture_create / _bounds / _render
 *   GetGlyphRunOutline                      -> idwr_outline_scaler_run
 *   GetDesignGlyphMetrics, GetGdiCompatibleGlyphMetrics, GetMetrics, GetGdiCompatibleMetrics
 *                                           -> idwr_face_*
 *   TranslateColorGlyphRun (COLRv0)         -> idwr_translate_color_glyph_run
 * Left out, as in the Windows build: COLRv1 (Gecko paints it: SK_DISABLE_DIRECTWRITE_COLRv1),
 * SVG and PNG glyphs (never reachable). The DirectWrite mutex (null with FontFace4) is replaced
 * by the font's lock (IdwrFontHolder::fMutex): idwr is not thread safe per font, DirectWrite is.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "src/ports/SkScalerContext_idwr.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkDrawable.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/private/base/SkTo.h"
#include "src/base/SkEndian.h"
#include "src/core/SkColorData.h"
#include "src/core/SkGlyph.h"
#include "src/core/SkMaskGamma.h"
#include "src/ports/SkIdwrPathSink.h"
#include "src/sfnt/SkOTTable_EBLC.h"
#include "src/sfnt/SkOTTable_EBSC.h"
#include "src/sfnt/SkOTTable_gasp.h"
#include "src/sfnt/SkOTTable_maxp.h"
#include "src/utils/SkMatrix22.h"

#include <algorithm>
#include <vector>

namespace {
static inline const constexpr bool kSkShowTextBlitCoverage = false;

// The bytes of an sfnt table (AutoTDWriteTable).
template <typename T>
struct IdwrTable {
    IdwrTable(const idwr_font* font, uint32_t tag) {
        uint32_t size = 0;
        fData = idwr_font_table(font, tag, &size);
        fSize = size;
        fExists = fData != nullptr;
    }
    const T* operator->() const { return reinterpret_cast<const T*>(fData); }
    const T* get() const { return reinterpret_cast<const T*>(fData); }
    const uint8_t* fData;
    size_t fSize;
    bool fExists;
};

static bool isLCD(const SkScalerContextRec& rec) {
    return SkMask::kLCD16_Format == rec.fMaskFormat;
}

static bool is_hinted(IdwrFontTypeface* typeface) {
    IdwrTable<SkOTTableMaximumProfile> maxp(typeface->idwrFont(), IDWR_TAG('m', 'a', 'x', 'p'));
    if (!maxp.fExists) {
        return false;
    }
    if (maxp.fSize < sizeof(SkOTTableMaximumProfile::Version::TT)) {
        return false;
    }
    if (maxp->version.version != SkOTTableMaximumProfile::Version::TT::VERSION) {
        return false;
    }
    return (0 != maxp->version.tt.maxSizeOfInstructions);
}

/** A GaspRange is inclusive, [min, max]. */
struct GaspRange {
    using Behavior = SkOTTableGridAndScanProcedure::GaspRange::behavior;
    GaspRange(int min, int max, int version, Behavior flags)
            : fMin(min), fMax(max), fVersion(version), fFlags(flags) {}
    int fMin;
    int fMax;
    int fVersion;
    Behavior fFlags;
};

bool get_gasp_range(IdwrFontTypeface* typeface, int size, GaspRange* range) {
    IdwrTable<SkOTTableGridAndScanProcedure> gasp(typeface->idwrFont(), IDWR_TAG('g', 'a', 's', 'p'));
    if (!gasp.fExists) {
        return false;
    }
    if (gasp.fSize < sizeof(SkOTTableGridAndScanProcedure)) {
        return false;
    }
    if (gasp->version != SkOTTableGridAndScanProcedure::version0 &&
        gasp->version != SkOTTableGridAndScanProcedure::version1) {
        return false;
    }

    uint16_t numRanges = SkEndianSwap16(gasp->numRanges);
    if (numRanges > 1024 ||
        gasp.fSize < sizeof(SkOTTableGridAndScanProcedure) +
                             sizeof(SkOTTableGridAndScanProcedure::GaspRange) * numRanges) {
        return false;
    }

    const SkOTTableGridAndScanProcedure::GaspRange* rangeTable =
            SkTAfter<const SkOTTableGridAndScanProcedure::GaspRange>(gasp.get());
    int minPPEM = -1;
    for (uint16_t i = 0; i < numRanges; ++i, ++rangeTable) {
        int maxPPEM = SkEndianSwap16(rangeTable->maxPPEM);
        if (minPPEM < size && size <= maxPPEM) {
            range->fMin = minPPEM + 1;
            range->fMax = maxPPEM;
            range->fVersion = SkEndian_SwapBE16(gasp->version);
            range->fFlags = rangeTable->flags;
            return true;
        }
        minPPEM = maxPPEM;
    }
    return false;
}
/** If the rendering mode for the specified 'size' is gridfit, then place
 *  the gridfit range into 'range'. Otherwise, leave 'range' alone.
 */
static bool is_gridfit_only(GaspRange::Behavior flags) {
    return flags.raw.value == GaspRange::Behavior::Raw::GridfitMask;
}

static bool has_bitmap_strike(IdwrFontTypeface* typeface, GaspRange range) {
    {
        IdwrTable<SkOTTableEmbeddedBitmapLocation> eblc(typeface->idwrFont(),
                                                        IDWR_TAG('E', 'B', 'L', 'C'));
        if (!eblc.fExists) {
            return false;
        }
        if (eblc.fSize < sizeof(SkOTTableEmbeddedBitmapLocation)) {
            return false;
        }
        if (eblc->version != SkOTTableEmbeddedBitmapLocation::version_initial) {
            return false;
        }

        uint32_t numSizes = SkEndianSwap32(eblc->numSizes);
        if (numSizes > 1024 ||
            eblc.fSize < sizeof(SkOTTableEmbeddedBitmapLocation) +
                                 sizeof(SkOTTableEmbeddedBitmapLocation::BitmapSizeTable) *
                                         numSizes) {
            return false;
        }

        const SkOTTableEmbeddedBitmapLocation::BitmapSizeTable* sizeTable =
                SkTAfter<const SkOTTableEmbeddedBitmapLocation::BitmapSizeTable>(eblc.get());
        for (uint32_t i = 0; i < numSizes; ++i, ++sizeTable) {
            if (sizeTable->ppemX == sizeTable->ppemY && range.fMin <= sizeTable->ppemX &&
                sizeTable->ppemX <= range.fMax) {
                if (sizeTable->endGlyphIndex >= sizeTable->startGlyphIndex + 3) {
                    return true;
                }
            }
        }
    }

    {
        IdwrTable<SkOTTableEmbeddedBitmapScaling> ebsc(typeface->idwrFont(),
                                                       IDWR_TAG('E', 'B', 'S', 'C'));
        if (!ebsc.fExists) {
            return false;
        }
        if (ebsc.fSize < sizeof(SkOTTableEmbeddedBitmapScaling)) {
            return false;
        }
        if (ebsc->version != SkOTTableEmbeddedBitmapScaling::version_initial) {
            return false;
        }

        uint32_t numSizes = SkEndianSwap32(ebsc->numSizes);
        if (numSizes > 1024 ||
            ebsc.fSize < sizeof(SkOTTableEmbeddedBitmapScaling) +
                                 sizeof(SkOTTableEmbeddedBitmapScaling::BitmapScaleTable) *
                                         numSizes) {
            return false;
        }

        const SkOTTableEmbeddedBitmapScaling::BitmapScaleTable* scaleTable =
                SkTAfter<const SkOTTableEmbeddedBitmapScaling::BitmapScaleTable>(ebsc.get());
        for (uint32_t i = 0; i < numSizes; ++i, ++scaleTable) {
            if (scaleTable->ppemX == scaleTable->ppemY && range.fMin <= scaleTable->ppemX &&
                scaleTable->ppemX <= range.fMax) {
                // EBSC tables are normally only found in bitmap only fonts.
                return true;
            }
        }
    }

    return false;
}

static bool both_zero(SkScalar a, SkScalar b) { return 0 == a && 0 == b; }

// returns false if there is any non-90-rotation or skew
static bool is_axis_aligned(const SkScalerContextRec& rec) {
    return 0 == rec.fPreSkewX && (both_zero(rec.fPost2x2[0][1], rec.fPost2x2[1][0]) ||
                                  both_zero(rec.fPost2x2[0][0], rec.fPost2x2[1][1]));
}

}  // namespace

SkScalerContext_Idwr::SkScalerContext_Idwr(IdwrFontTypeface& typefaceRef,
                                           const SkScalerContextEffects& effects,
                                           const SkDescriptor* desc)
        : SkScalerContext(typefaceRef, effects, desc) {
    IdwrFontTypeface* typeface = this->getIdwrTypeface();
    // Its tables are read and its face made while other strikes use the same font.
    SkAutoMutexExclusive fontLock(typeface->fontMutex());
    fGlyphCount = typeface->glyphCount();

    fClearTypeLevel = int(typeface->GetClearTypeLevel() * 256);

    SkVector scale;
    fRec.computeMatrices(SkScalerContextRec::PreMatrixScale::kVertical, &scale, &fSkXform);

    fXform.m11 = SkScalarToFloat(fSkXform.getScaleX());
    fXform.m12 = SkScalarToFloat(fSkXform.getSkewY());
    fXform.m21 = SkScalarToFloat(fSkXform.getSkewX());
    fXform.m22 = SkScalarToFloat(fSkXform.getScaleY());
    fXform.dx = 0;
    fXform.dy = 0;

    // realTextSize is the actual device size we want (as opposed to the size the user requested).
    // gdiTextSize is the size we request when GDI compatible.
    // If the scale is negative, this means the matrix will do the flip anyway.
    const SkScalar realTextSize = scale.fY;
    // Due to floating point math, the lower bits are suspect. Round carefully.
    SkScalar gdiTextSize = SkScalarRoundToScalar(realTextSize * 64.0f) / 64.0f;
    if (gdiTextSize == 0) {
        gdiTextSize = SK_Scalar1;
    }

    bool bitmapRequested = SkToBool(fRec.fFlags & SkScalerContext::kEmbeddedBitmapText_Flag);
    bool treatLikeBitmap = false;
    bool axisAlignedBitmap = false;
    if (bitmapRequested) {
        // When embedded bitmaps are requested, treat the entire range like
        // a bitmap strike if the range is gridfit only and contains a bitmap.
        int bitmapPPEM = SkScalarTruncToInt(gdiTextSize);
        GaspRange range(bitmapPPEM, bitmapPPEM, 0, GaspRange::Behavior());
        if (get_gasp_range(typeface, bitmapPPEM, &range)) {
            if (!is_gridfit_only(range.fFlags)) {
                range = GaspRange(bitmapPPEM, bitmapPPEM, 0, GaspRange::Behavior());
            }
        }
        treatLikeBitmap = has_bitmap_strike(typeface, range);

        axisAlignedBitmap = is_axis_aligned(fRec);
    }

    GaspRange range(0, 0xFFFF, 0, GaspRange::Behavior());

    // If the user requested aliased, do so with aliased compatible metrics.
    if (SkMask::kBW_Format == fRec.fMaskFormat) {
        fTextSizeRender = gdiTextSize;
        fRenderingMode = IDWR_MODE_ALIASED;
        fTextureType = IDWR_TEXTURE_ALIASED_1x1;
        fTextSizeMeasure = gdiTextSize;
        fMeasuringMode = IDWR_MEASURING_GDI_CLASSIC;

        // If we can use a bitmap, use gdi classic rendering and measurement.
        // This will not always provide a bitmap, but matches expected behavior.
    } else if ((treatLikeBitmap && axisAlignedBitmap) || typeface->ForceGDI()) {
        fTextSizeRender = gdiTextSize;
        fRenderingMode = IDWR_MODE_GDI_CLASSIC;
        fTextureType = IDWR_TEXTURE_CLEARTYPE_3x1;
        fTextSizeMeasure = gdiTextSize;
        fMeasuringMode = IDWR_MEASURING_GDI_CLASSIC;

        // If rotated but the horizontal text could have used a bitmap,
        // render high quality rotated glyphs but measure using bitmap metrics.
    } else if (treatLikeBitmap) {
        fTextSizeRender = gdiTextSize;
        fRenderingMode = IDWR_MODE_NATURAL_SYMMETRIC;
        fTextureType = IDWR_TEXTURE_CLEARTYPE_3x1;
        fTextSizeMeasure = gdiTextSize;
        fMeasuringMode = IDWR_MEASURING_GDI_CLASSIC;

        // Force symmetric if the font is above the threshold or there is an explicit mode.
        // Here we check if the size exceeds 20 before checking the GASP table to match the
        // results of calling GetRecommendedRenderingMode/Direct2D, which skip looking at
        // the GASP table if the text is too large.
    } else if (realTextSize > SkIntToScalar(20) ||
               typeface->GetRenderingMode() == IDWR_MODE_NATURAL ||
               typeface->GetRenderingMode() == IDWR_MODE_NATURAL_SYMMETRIC) {
        fTextSizeRender = realTextSize;
        fRenderingMode = typeface->GetRenderingMode() == IDWR_MODE_NATURAL
                                 ? IDWR_MODE_NATURAL
                                 : IDWR_MODE_NATURAL_SYMMETRIC;
        fTextureType = IDWR_TEXTURE_CLEARTYPE_3x1;
        fTextSizeMeasure = realTextSize;
        fMeasuringMode = IDWR_MEASURING_NATURAL;
        // If the font has a gasp table version 1, use it to determine symmetric rendering.
    } else if (get_gasp_range(typeface, SkScalarRoundToInt(gdiTextSize), &range) &&
               range.fVersion >= 1) {
        fTextSizeRender = realTextSize;
        fRenderingMode = !range.fFlags.field.SymmetricSmoothing ? IDWR_MODE_NATURAL
                                                                : IDWR_MODE_NATURAL_SYMMETRIC;
        fTextureType = IDWR_TEXTURE_CLEARTYPE_3x1;
        fTextSizeMeasure = realTextSize;
        fMeasuringMode = IDWR_MEASURING_NATURAL;
        // Fonts with hints, no gasp or gasp version 0, and below 20px get non-symmetric rendering.
        // Often such fonts have hints which were only tested with GDI ClearType classic.
        // See https://crbug.com/385897
    } else {
        if (is_hinted(typeface)) {
            fTextSizeRender = gdiTextSize;
            fRenderingMode = IDWR_MODE_NATURAL;
        } else {
            // Unhinted but with no gasp and below 20px defaults to symmetric for
            // GetRecommendedRenderingMode.
            fTextSizeRender = realTextSize;
            fRenderingMode = IDWR_MODE_NATURAL_SYMMETRIC;
        }
        fTextureType = IDWR_TEXTURE_CLEARTYPE_3x1;
        fTextSizeMeasure = realTextSize;
        fMeasuringMode = IDWR_MEASURING_NATURAL;
    }

    // DirectWrite2 allows for grayscale hinting (Windows 10 and later: Factory2 and FontFace2
    // are always present).
    fAntiAliasMode = IDWR_AA_CLEARTYPE;
    if (SkMask::kA8_Format == fRec.fMaskFormat &&
        !(fRec.fFlags & SkScalerContext::kGenA8FromLCD_Flag)) {
        // IDWR_TEXTURE_ALIASED_1x1 is now misnamed, it must also be used with grayscale.
        fTextureType = IDWR_TEXTURE_ALIASED_1x1;
        fAntiAliasMode = IDWR_AA_GRAYSCALE;
    }

    // DirectWrite2 allows hinting to be disabled.
    fGridFitMode = IDWR_GRID_FIT_ENABLED;
    if (fRec.getHinting() == SkFontHinting::kNone) {
        fGridFitMode = IDWR_GRID_FIT_DISABLED;
        if (fRenderingMode != IDWR_MODE_ALIASED) {
            fRenderingMode = IDWR_MODE_NATURAL_SYMMETRIC;
        }
    }

    if (this->isLinearMetrics()) {
        fTextSizeMeasure = realTextSize;
        fMeasuringMode = IDWR_MEASURING_NATURAL;
    }

    // The GDI measuring modes don't seem to work well with CBDT fonts (DWrite.dll 10.0.18362.836).
    if (fMeasuringMode != IDWR_MEASURING_NATURAL) {
        uint32_t size = 0;
        if (idwr_font_table(typeface->idwrFont(), IDWR_TAG('C', 'B', 'D', 'T'), &size)) {
            fMeasuringMode = IDWR_MEASURING_NATURAL;
        }
    }

    // This context's own face: idwr_face is not thread safe.
    fFace = idwr_face_create(typeface->idwrFont(), typeface->simulations(),
                             typeface->axes().data(), (uint32_t)typeface->axes().size());
}

SkScalerContext_Idwr::~SkScalerContext_Idwr() {
    SkAutoMutexExclusive fontLock(this->getIdwrTypeface()->fontMutex());
    if (fOutline) {
        idwr_outline_scaler_close(fOutline);
    }
    if (fFace) {
        idwr_face_destroy(fFace);
    }
}

bool SkScalerContext_Idwr::runOutline(float emSize,
                                      const uint16_t* glyphs,
                                      const float* advances,
                                      const idwr_glyph_offset* offsets,
                                      uint32_t count,
                                      int sideways,
                                      int rtl,
                                      SkPathBuilder* builder) {
    IdwrFontTypeface* typeface = this->getIdwrTypeface();
    if (!fOutline) {
        const auto& coords = typeface->coords();
        fOutline = idwr_outline_scaler_open(typeface->idwrFont(),
                                            coords.empty() ? nullptr : coords.data(),
                                            (uint16_t)coords.size(), typeface->simulations());
        if (!fOutline) {
            return false;
        }
    }
    std::vector<idwr_path_cmd> cmds(256);
    int n = idwr_outline_scaler_run(fOutline, emSize, glyphs, advances, offsets, count, sideways,
                                    rtl, cmds.data(), (int)cmds.size());
    if (n > (int)cmds.size()) {
        cmds.resize(n);
        n = idwr_outline_scaler_run(fOutline, emSize, glyphs, advances, offsets, count, sideways,
                                    rtl, cmds.data(), (int)cmds.size());
    }
    if (n < 0) {
        return false;
    }
    SkIdwrAppendPath(builder, cmds.data(), n);
    return true;
}

bool SkScalerContext_Idwr::setAdvance(const SkGlyph& glyph, SkVector* advance) {
    SkAutoMutexExclusive fontLock(this->getIdwrTypeface()->fontMutex());
    *advance = {0, 0};
    uint16_t glyphId = glyph.getGlyphID();

    // DirectWrite treats all out of bounds glyph ids as having the same data as glyph 0.
    // For consistency with all other backends, treat out of range glyph ids as an error.
    if (fGlyphCount <= glyphId || !fFace) {
        return false;
    }

    idwr_glyph_metrics gm;

    if (IDWR_MEASURING_GDI_CLASSIC == fMeasuringMode ||
        IDWR_MEASURING_GDI_NATURAL == fMeasuringMode) {
        if (idwr_face_get_gdi_compatible_glyph_metrics(
                    fFace,
                    fTextSizeMeasure,
                    1.0f,  // pixelsPerDip
                    // This parameter does not act like the lpmat2 parameter to GetGlyphOutlineW.
                    nullptr,
                    IDWR_MEASURING_GDI_NATURAL == fMeasuringMode,
                    &glyphId,
                    1,
                    &gm,
                    0) != 0) {
            return false;
        }
    } else {
        if (idwr_face_get_design_glyph_metrics(fFace, &glyphId, 1, &gm, 0) != 0) {
            return false;
        }
    }

    idwr_font_metrics dwfm;
    idwr_face_get_metrics(fFace, &dwfm);
    SkScalar advanceX = fTextSizeMeasure * gm.advanceWidth / dwfm.designUnitsPerEm;

    *advance = {advanceX, 0};
    if (IDWR_MEASURING_GDI_CLASSIC == fMeasuringMode ||
        IDWR_MEASURING_GDI_NATURAL == fMeasuringMode) {
        // DirectWrite produced 'compatible' metrics, but while close,
        // the end result is not always an integer as it would be with GDI.
        advance->fX = SkScalarRoundToScalar(advance->fX);
    }
    *advance = fSkXform.mapVector(*advance);
    return true;
}

void SkScalerContext_Idwr::makeRun(idwr_glyph_run* run,
                                   const uint16_t* glyph,
                                   const float* advance,
                                   const idwr_glyph_offset* offset,
                                   int renderingMode) const {
    const IdwrFontTypeface* typeface = const_cast<SkScalerContext_Idwr*>(this)->getIdwrTypeface();
    *run = {};
    run->em_size = SkScalarToFloat(fTextSizeRender);
    run->count = 1;
    run->glyphs = glyph;
    run->advances = advance;
    run->offsets = offset;
    run->sideways = 0;
    run->bidi_level = 0;
    run->transform = &fXform;
    run->pixels_per_dip = 1.0f;
    run->origin_x = 0.0f;
    run->origin_y = 0.0f;
    run->rendering_mode = renderingMode;
    run->keep_mode = 0;
    run->measuring_mode = fMeasuringMode;
    // IDWriteFactory2::CreateGlyphRunAnalysis is very bad at aliased glyphs, so it is used only
    // with grid fit disabled or grayscale; otherwise IDWriteFactory::CreateGlyphRunAnalysis
    // (grid fit DEFAULT, ClearType).
    if (fGridFitMode == IDWR_GRID_FIT_DISABLED || fAntiAliasMode == IDWR_AA_GRAYSCALE) {
        run->grid_fit = fGridFitMode;
        run->antialias = fAntiAliasMode;
    } else {
        run->grid_fit = IDWR_GRID_FIT_DEFAULT;
        run->antialias = IDWR_AA_CLEARTYPE;
    }
    run->simulations = typeface->simulations();
    run->glyph_metrics = nullptr;
}

bool SkScalerContext_Idwr::generateDWMetrics(const SkGlyph& glyph,
                                             int renderingMode,
                                             int textureType,
                                             SkRect* bounds) {
    IdwrFontTypeface* typeface = this->getIdwrTypeface();
    SkAutoMutexExclusive fontLock(typeface->fontMutex());

    // Measure raster size.
    fXform.dx = SkFixedToFloat(glyph.getSubXFixed());
    fXform.dy = SkFixedToFloat(glyph.getSubYFixed());

    float advance = 0;
    uint16_t glyphId = glyph.getGlyphID();
    idwr_glyph_offset offset = {0.0f, 0.0f};

    idwr_glyph_run run;
    this->makeRun(&run, &glyphId, &advance, &offset, renderingMode);

    idwr_glyph_run_texture* texture = nullptr;
    if (idwr_glyph_run_texture_create(typeface->idwrFont(), typeface->axes().data(),
                                      (uint32_t)typeface->axes().size(), &run, &texture) != 0) {
        return false;
    }
    int32_t bbox[4];
    int32_t hr = idwr_glyph_run_texture_bounds(texture, textureType, bbox);
    idwr_glyph_run_texture_destroy(texture);
    if (hr != 0) {
        return false;
    }

    // GetAlphaTextureBounds succeeds but sometimes returns empty bounds like
    // { 0x80000000, 0x80000000, 0x80000000, 0x80000000 }
    // for small but not quite zero and large (but not really large) glyphs,
    // Only set as non-empty if the returned bounds are non-empty.
    if (bbox[0] >= bbox[2] || bbox[1] >= bbox[3]) {
        return false;
    }

    *bounds = SkRect::MakeLTRB(bbox[0], bbox[1], bbox[2], bbox[3]);
    return true;
}

bool SkScalerContext_Idwr::getColorGlyphRun(const SkGlyph& glyph, idwr_color_run_enum** colorGlyph) {
    float advance = 0;
    uint16_t glyphId = glyph.getGlyphID();
    idwr_glyph_offset offset = {0.0f, 0.0f};

    idwr_glyph_run run;
    this->makeRun(&run, &glyphId, &advance, &offset, fRenderingMode);

    if (!fFace) {
        return false;
    }
    int32_t hr = idwr_translate_color_glyph_run(fFace, 0, 0, &run, IDWR_IMAGE_FACTORY2,
                                                fMeasuringMode, &fXform, 0, colorGlyph);
    if (hr == IDWR_HR_E_NOCOLOR) {
        return false;
    }
    return hr == 0;
}

bool SkScalerContext_Idwr::generateColorMetrics(const SkGlyph& glyph, SkRect* bounds) {
    SkAutoMutexExclusive fontLock(this->getIdwrTypeface()->fontMutex());
    idwr_color_run_enum* colorLayers = nullptr;
    if (!getColorGlyphRun(glyph, &colorLayers)) {
        return false;
    }
    SkASSERT(colorLayers);

    *bounds = SkRect::MakeEmpty();
    int hasNextRun = 0;
    bool ok = true;
    while (idwr_color_run_enum_move_next(colorLayers, &hasNextRun) == 0 && hasNextRun) {
        const idwr_color_glyph_run* colorGlyph;
        if (idwr_color_run_enum_current(colorLayers, &colorGlyph) != 0) {
            ok = false;
            break;
        }
        SkPathBuilder builder;
        if (!this->runOutline(colorGlyph->em_size, colorGlyph->glyphs, colorGlyph->advances,
                              colorGlyph->offsets, colorGlyph->count, colorGlyph->sideways,
                              colorGlyph->bidi_level % 2, &builder)) {
            ok = false;
            break;
        }
        bounds->join(builder.detach().getBounds());
    }
    idwr_color_run_enum_destroy(colorLayers);
    if (!ok) {
        return false;
    }
    SkMatrix matrix = fSkXform;
    if (this->isSubpixel()) {
        matrix.postTranslate(SkFixedToScalar(glyph.getSubXFixed()),
                             SkFixedToScalar(glyph.getSubYFixed()));
    }
    matrix.mapRect(bounds);
    return true;
}

SkScalerContext::GlyphMetrics SkScalerContext_Idwr::generateMetrics(const SkGlyph& glyph,
                                                                    SkArenaAlloc* alloc) {
    GlyphMetrics mx(glyph.maskFormat());

    mx.extraBits = ScalerContextBits::NONE;

    if (!this->setAdvance(glyph, &mx.advance)) {
        return mx;
    }

    IdwrFontTypeface* typeface = this->getIdwrTypeface();
    if (typeface->isColorFont()) {
        // COLRv1 is painted by Gecko, so only the COLRv0 layers DirectWrite translates.
        if (generateColorMetrics(glyph, &mx.bounds)) {
            mx.maskFormat = SkMask::kARGB32_Format;
            mx.extraBits |= ScalerContextBits::COLR;
            mx.neverRequestPath = true;
            return mx;
        }
    }

    if (this->generateDWMetrics(glyph, fRenderingMode, fTextureType, &mx.bounds)) {
        mx.extraBits = ScalerContextBits::DW;
        return mx;
    }

    // GetAlphaTextureBounds succeeds but returns an empty RECT if there are no
    // glyphs of the specified texture type or it is too big for smoothing.
    // When this happens, try with the alternate texture type.
    if (IDWR_TEXTURE_ALIASED_1x1 != fTextureType || IDWR_AA_GRAYSCALE == fAntiAliasMode) {
        if (this->generateDWMetrics(glyph, IDWR_MODE_ALIASED, IDWR_TEXTURE_ALIASED_1x1,
                                    &mx.bounds)) {
            mx.maskFormat = SkMask::kBW_Format;
            mx.extraBits = ScalerContextBits::DW_1;
            return mx;
        }
    }

    // GetAlphaTextureBounds can fail for various reasons.
    // As a fallback, attempt to generate the metrics and image from the path.
    mx.computeFromPath = true;
    mx.extraBits = ScalerContextBits::PATH;
    return mx;
}

void SkScalerContext_Idwr::generateFontMetrics(SkFontMetrics* metrics) {
    if (nullptr == metrics) {
        return;
    }

    sk_bzero(metrics, sizeof(*metrics));

    IdwrFontTypeface* typeface = this->getIdwrTypeface();
    SkAutoMutexExclusive fontLock(typeface->fontMutex());
    idwr_font_metrics dwfm;
    sk_bzero(&dwfm, sizeof(dwfm));
    if (!fFace) {
        return;
    }
    if (IDWR_MEASURING_GDI_CLASSIC == fMeasuringMode ||
        IDWR_MEASURING_GDI_NATURAL == fMeasuringMode) {
        idwr_face_get_gdi_compatible_metrics(fFace,
                                             fTextSizeRender,
                                             1.0f,  // pixelsPerDip
                                             &fXform,
                                             &dwfm);
    } else {
        idwr_face_get_metrics(fFace, &dwfm);
    }

    SkScalar upem = SkIntToScalar(dwfm.designUnitsPerEm);

    metrics->fAscent = -fTextSizeRender * SkIntToScalar(dwfm.ascent) / upem;
    metrics->fDescent = fTextSizeRender * SkIntToScalar(dwfm.descent) / upem;
    metrics->fLeading = fTextSizeRender * SkIntToScalar(dwfm.lineGap) / upem;
    metrics->fXHeight = fTextSizeRender * SkIntToScalar(dwfm.xHeight) / upem;
    metrics->fCapHeight = fTextSizeRender * SkIntToScalar(dwfm.capHeight) / upem;
    metrics->fUnderlineThickness = fTextSizeRender * SkIntToScalar(dwfm.underlineThickness) / upem;
    metrics->fUnderlinePosition = -(fTextSizeRender * SkIntToScalar(dwfm.underlinePosition) / upem);
    metrics->fStrikeoutThickness =
            fTextSizeRender * SkIntToScalar(dwfm.strikethroughThickness) / upem;
    metrics->fStrikeoutPosition =
            -(fTextSizeRender * SkIntToScalar(dwfm.strikethroughPosition) / upem);

    metrics->fFlags |= SkFontMetrics::kUnderlineThicknessIsValid_Flag;
    metrics->fFlags |= SkFontMetrics::kUnderlinePositionIsValid_Flag;
    metrics->fFlags |= SkFontMetrics::kStrikeoutThicknessIsValid_Flag;
    metrics->fFlags |= SkFontMetrics::kStrikeoutPositionIsValid_Flag;

    // IDWriteFontFace5::HasVariations: the font has variation axes, whether or not values are
    // applied. idwr_normalize_axes gives the font's axis count with the defaults filled in, so
    // coords() is non-empty exactly for a font with an fvar of axes (none without fvar).
    if (!typeface->coords().empty()) {
        // The bounds are only valid for the default variation.
        metrics->fFlags |= SkFontMetrics::kBoundsInvalid_Flag;
    }

    // IDWriteFontFace1::GetMetrics, the glyph box.
    idwr_font_metrics dwfm1;
    idwr_face_get_metrics(fFace, &dwfm1);
    metrics->fTop = -fTextSizeRender * SkIntToScalar(dwfm1.glyphBoxTop) / upem;
    metrics->fBottom = -fTextSizeRender * SkIntToScalar(dwfm1.glyphBoxBottom) / upem;
    metrics->fXMin = fTextSizeRender * SkIntToScalar(dwfm1.glyphBoxLeft) / upem;
    metrics->fXMax = fTextSizeRender * SkIntToScalar(dwfm1.glyphBoxRight) / upem;

    metrics->fMaxCharWidth = metrics->fXMax - metrics->fXMin;
}

///////////////////////////////////////////////////////////////////////////////

void SkScalerContext_Idwr::BilevelToBW(const uint8_t* SK_RESTRICT src,
                                       const SkGlyph& glyph,
                                       void* imageBuffer) {
    const int width = glyph.width();
    const size_t dstRB = (width + 7) >> 3;
    uint8_t* SK_RESTRICT dst = static_cast<uint8_t*>(imageBuffer);

    int byteCount = width >> 3;
    int bitCount = width & 7;

    for (int y = 0; y < glyph.height(); ++y) {
        if (byteCount > 0) {
            for (int i = 0; i < byteCount; ++i) {
                unsigned byte = 0;
                byte |= src[0] & (1 << 7);
                byte |= src[1] & (1 << 6);
                byte |= src[2] & (1 << 5);
                byte |= src[3] & (1 << 4);
                byte |= src[4] & (1 << 3);
                byte |= src[5] & (1 << 2);
                byte |= src[6] & (1 << 1);
                byte |= src[7] & (1 << 0);
                dst[i] = byte;
                src += 8;
            }
        }
        if (bitCount > 0) {
            unsigned byte = 0;
            unsigned mask = 0x80;
            for (int i = 0; i < bitCount; i++) {
                byte |= (src[i]) & mask;
                mask >>= 1;
            }
            dst[byteCount] = byte;
        }
        src += bitCount;
        dst += dstRB;
    }
}

template <bool APPLY_PREBLEND>
void SkScalerContext_Idwr::GrayscaleToA8(const uint8_t* SK_RESTRICT src,
                                         const SkGlyph& glyph,
                                         void* imageBuffer,
                                         const uint8_t* table8) {
    const size_t dstRB = glyph.rowBytes();
    const int width = glyph.width();
    uint8_t* SK_RESTRICT dst = static_cast<uint8_t*>(imageBuffer);

    for (int y = 0; y < glyph.height(); y++) {
        for (int i = 0; i < width; i++) {
            U8CPU a = *(src++);
            dst[i] = sk_apply_lut_if<APPLY_PREBLEND>(a, table8);
        }
        dst = SkTAddOffset<uint8_t>(dst, dstRB);
    }
}

template <bool APPLY_PREBLEND>
void SkScalerContext_Idwr::RGBToA8(const uint8_t* SK_RESTRICT src,
                                   const SkGlyph& glyph,
                                   void* imageBuffer,
                                   const uint8_t* table8) {
    const size_t dstRB = glyph.rowBytes();
    const int width = glyph.width();
    uint8_t* SK_RESTRICT dst = static_cast<uint8_t*>(imageBuffer);

    for (int y = 0; y < glyph.height(); y++) {
        for (int i = 0; i < width; i++) {
            // Ignore the R, B channels. It looks the closest to what
            // D2D with grayscale AA has. But there's no way
            // to just get a grayscale AA alpha texture from a glyph run.
            U8CPU g = src[1];
            src += 3;

            dst[i] = sk_apply_lut_if<APPLY_PREBLEND>(g, table8);
        }
        dst = SkTAddOffset<uint8_t>(dst, dstRB);
    }
}

template <bool APPLY_PREBLEND, bool RGB>
void SkScalerContext_Idwr::RGBToLcd16(const uint8_t* SK_RESTRICT src,
                                      const SkGlyph& glyph,
                                      void* imageBuffer,
                                      const uint8_t* tableR,
                                      const uint8_t* tableG,
                                      const uint8_t* tableB,
                                      int clearTypeLevel) {
    const size_t dstRB = glyph.rowBytes();
    const int width = glyph.width();
    uint16_t* SK_RESTRICT dst = static_cast<uint16_t*>(imageBuffer);

    for (int y = 0; y < glyph.height(); y++) {
        for (int i = 0; i < width; i++) {
            int r, g, b;
            if (RGB) {
                r = sk_apply_lut_if<APPLY_PREBLEND>(*(src++), tableR);
                g = sk_apply_lut_if<APPLY_PREBLEND>(*(src++), tableG);
                b = sk_apply_lut_if<APPLY_PREBLEND>(*(src++), tableB);
            } else {
                b = sk_apply_lut_if<APPLY_PREBLEND>(*(src++), tableB);
                g = sk_apply_lut_if<APPLY_PREBLEND>(*(src++), tableG);
                r = sk_apply_lut_if<APPLY_PREBLEND>(*(src++), tableR);
            }
            r = g + (((r - g) * clearTypeLevel) >> 8);
            b = g + (((b - g) * clearTypeLevel) >> 8);
            dst[i] = SkPack888ToRGB16(r, g, b);
        }
        dst = SkTAddOffset<uint16_t>(dst, dstRB);
    }
}

const void* SkScalerContext_Idwr::getDWMaskBits(const SkGlyph& glyph,
                                                int renderingMode,
                                                int textureType) {
    IdwrFontTypeface* typeface = this->getIdwrTypeface();
    SkAutoMutexExclusive fontLock(typeface->fontMutex());

    int sizeNeeded = glyph.width() * glyph.height();
    if (IDWR_TEXTURE_CLEARTYPE_3x1 == textureType) {
        sizeNeeded *= 3;
    }
    if (sizeNeeded > fBits.size()) {
        fBits.resize(sizeNeeded);
    }

    // erase
    memset(fBits.begin(), 0, sizeNeeded);

    fXform.dx = SkFixedToFloat(glyph.getSubXFixed());
    fXform.dy = SkFixedToFloat(glyph.getSubYFixed());

    float advance = 0.0f;
    uint16_t index = glyph.getGlyphID();
    idwr_glyph_offset offset = {0.0f, 0.0f};

    idwr_glyph_run run;
    this->makeRun(&run, &index, &advance, &offset, renderingMode);

    idwr_glyph_run_texture* texture = nullptr;
    if (idwr_glyph_run_texture_create(typeface->idwrFont(), typeface->axes().data(),
                                      (uint32_t)typeface->axes().size(), &run, &texture) != 0) {
        return nullptr;
    }
    // NOTE: this assumes that the glyph has already been measured
    // with an exact same glyph run analysis.
    int32_t bbox[4] = {glyph.left(), glyph.top(), glyph.left() + glyph.width(),
                       glyph.top() + glyph.height()};
    int32_t hr = idwr_glyph_run_texture_render(texture, textureType, bbox, fBits.begin(),
                                               (uint32_t)sizeNeeded);
    idwr_glyph_run_texture_destroy(texture);
    if (hr != 0) {
        return nullptr;
    }
    return fBits.begin();
}

bool SkScalerContext_Idwr::generateDWImage(const SkGlyph& glyph, void* imageBuffer) {
    // Create the mask.
    ScalerContextBits::value_type format = glyph.extraBits();
    int renderingMode = fRenderingMode;
    int textureType = fTextureType;
    if (format == ScalerContextBits::DW_1) {
        renderingMode = IDWR_MODE_ALIASED;
        textureType = IDWR_TEXTURE_ALIASED_1x1;
    }
    const void* bits = this->getDWMaskBits(glyph, renderingMode, textureType);
    if (!bits) {
        sk_bzero(imageBuffer, glyph.imageSize());
        return false;
    }

    // Copy the mask into the glyph.
    const uint8_t* src = (const uint8_t*)bits;
    if (IDWR_MODE_ALIASED == renderingMode) {
        SkASSERT(SkMask::kBW_Format == glyph.maskFormat());
        SkASSERT(IDWR_TEXTURE_ALIASED_1x1 == textureType);
        BilevelToBW(src, glyph, imageBuffer);
    } else if (!isLCD(fRec)) {
        if (textureType == IDWR_TEXTURE_ALIASED_1x1) {
            if (fPreBlend.isApplicable()) {
                GrayscaleToA8<true>(src, glyph, imageBuffer, fPreBlend.fG);
            } else {
                GrayscaleToA8<false>(src, glyph, imageBuffer, fPreBlend.fG);
            }
        } else {
            if (fPreBlend.isApplicable()) {
                RGBToA8<true>(src, glyph, imageBuffer, fPreBlend.fG);
            } else {
                RGBToA8<false>(src, glyph, imageBuffer, fPreBlend.fG);
            }
        }
    } else {
        SkASSERT(SkMask::kLCD16_Format == glyph.maskFormat());
        if (fPreBlend.isApplicable()) {
            if (fRec.fFlags & SkScalerContext::kLCD_BGROrder_Flag) {
                RGBToLcd16<true, false>(src, glyph, imageBuffer, fPreBlend.fR, fPreBlend.fG,
                                        fPreBlend.fB, fClearTypeLevel);
            } else {
                RGBToLcd16<true, true>(src, glyph, imageBuffer, fPreBlend.fR, fPreBlend.fG,
                                       fPreBlend.fB, fClearTypeLevel);
            }
        } else {
            if (fRec.fFlags & SkScalerContext::kLCD_BGROrder_Flag) {
                RGBToLcd16<false, false>(src, glyph, imageBuffer, fPreBlend.fR, fPreBlend.fG,
                                         fPreBlend.fB, fClearTypeLevel);
            } else {
                RGBToLcd16<false, true>(src, glyph, imageBuffer, fPreBlend.fR, fPreBlend.fG,
                                        fPreBlend.fB, fClearTypeLevel);
            }
        }
    }
    return true;
}

bool SkScalerContext_Idwr::drawColorImage(const SkGlyph& glyph, SkCanvas& canvas) {
    // Also from the drawable, long after generateDrawable and outside the strike's lock.
    SkAutoMutexExclusive fontLock(this->getIdwrTypeface()->fontMutex());
    idwr_color_run_enum* colorLayers = nullptr;
    if (!getColorGlyphRun(glyph, &colorLayers)) {
        return false;
    }

    SkPaint paint;
    paint.setAntiAlias(fRenderingMode != IDWR_MODE_ALIASED);

    if (this->isSubpixel()) {
        canvas.translate(SkFixedToScalar(glyph.getSubXFixed()),
                         SkFixedToScalar(glyph.getSubYFixed()));
    }
    canvas.concat(fSkXform);

    IdwrFontTypeface* typeface = this->getIdwrTypeface();
    size_t paletteEntryCount = typeface->palette().size();
    const SkColor* palette = typeface->palette().data();
    int hasNextRun = 0;
    bool ok = true;
    while (idwr_color_run_enum_move_next(colorLayers, &hasNextRun) == 0 && hasNextRun) {
        const idwr_color_glyph_run* colorGlyph;
        if (idwr_color_run_enum_current(colorLayers, &colorGlyph) != 0) {
            ok = false;
            break;
        }

        SkColor color;
        if (colorGlyph->palette_index == 0xffff) {
            color = fRec.fForegroundColor;
        } else if (colorGlyph->palette_index < paletteEntryCount) {
            color = palette[colorGlyph->palette_index];
        } else {
            color = SK_ColorBLACK;
        }
        paint.setColor(color);

        SkPathBuilder builder;
        if (!this->runOutline(colorGlyph->em_size, colorGlyph->glyphs, colorGlyph->advances,
                              colorGlyph->offsets, colorGlyph->count, colorGlyph->sideways,
                              colorGlyph->bidi_level % 2, &builder)) {
            ok = false;
            break;
        }
        canvas.drawPath(builder.detach(), paint);
    }
    idwr_color_run_enum_destroy(colorLayers);
    return ok;
}

bool SkScalerContext_Idwr::generateColorImage(const SkGlyph& glyph, void* imageBuffer) {
    SkASSERT(glyph.maskFormat() == SkMask::Format::kARGB32_Format);

    SkBitmap dstBitmap;
    dstBitmap.installPixels(
            SkImageInfo::Make(glyph.width(), glyph.height(), kN32_SkColorType, kPremul_SkAlphaType),
            imageBuffer,
            glyph.rowBytes());

    SkCanvas canvas(dstBitmap);
    canvas.clear(SK_ColorTRANSPARENT);
    canvas.translate(-SkIntToScalar(glyph.left()), -SkIntToScalar(glyph.top()));

    return this->drawColorImage(glyph, canvas);
}

void SkScalerContext_Idwr::generateImage(const SkGlyph& glyph, void* imageBuffer) {
    ScalerContextBits::value_type format = glyph.extraBits();
    if (format == ScalerContextBits::DW || format == ScalerContextBits::DW_1) {
        this->generateDWImage(glyph, imageBuffer);
    } else if (format == ScalerContextBits::COLR) {
        this->generateColorImage(glyph, imageBuffer);
    } else if (format == ScalerContextBits::PATH) {
        this->generateImageFromPath(glyph, imageBuffer);
    } else {
        SK_ABORT("Bad format");
    }
}

std::optional<SkScalerContext::GeneratedPath> SkScalerContext_Idwr::generatePath(
        const SkGlyph& glyph) {
    SkAutoMutexExclusive fontLock(this->getIdwrTypeface()->fontMutex());
    SkGlyphID glyphID = glyph.getGlyphID();

    // DirectWrite treats all out of bounds glyph ids as having the same data as glyph 0.
    // For consistency with all other backends, treat out of range glyph ids as an error.
    if (fGlyphCount <= glyphID) {
        return {};
    }

    SkPathBuilder builder;
    uint16_t glyphId = SkTo<uint16_t>(glyphID);
    // TODO (as in the original): convert to<->from DIUs? It appears DirectWrite only hints at em.
    if (!this->runOutline(SkScalarToFloat(fTextSizeRender), &glyphId, nullptr, nullptr, 1, 0, 0,
                          &builder)) {
        return {};
    }

    builder.transform(fSkXform);
    return {{builder.detach(), false}};
}

sk_sp<SkDrawable> SkScalerContext_Idwr::generateDrawable(const SkGlyph& glyph) {
    struct COLRGlyphDrawable : public SkDrawable {
        SkScalerContext_Idwr* fSelf;
        SkGlyph fGlyph;
        COLRGlyphDrawable(SkScalerContext_Idwr* self, const SkGlyph& glyph)
                : fSelf(self), fGlyph(glyph) {}
        SkRect onGetBounds() override { return fGlyph.rect(); }
        size_t onApproximateBytesUsed() override { return sizeof(COLRGlyphDrawable); }
        void onDraw(SkCanvas* canvas) override { fSelf->drawColorImage(fGlyph, *canvas); }
    };
    ScalerContextBits::value_type format = glyph.extraBits();
    if (format == ScalerContextBits::COLR) {
        return sk_sp<SkDrawable>(new COLRGlyphDrawable(this, glyph));
    }
    return nullptr;
}
