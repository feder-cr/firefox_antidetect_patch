/*
 * Port of SkTypeface_win_dw.cpp (DWriteFontTypeface) over idwr. The rendering side, onFilterRec
 * with the gamma and contrast the font was made with, is the Windows one verbatim. The PDF and
 * name-lookup side is rebuilt from the font bytes and has no pixel effect.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "src/ports/SkTypeface_idwr.h"

#include "include/core/SkData.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkStream.h"
#include "include/private/base/SkTo.h"
#include "src/base/SkEndian.h"
#include "src/core/SkFontDescriptor.h"
#include "src/core/SkFontStream.h"
#include "src/core/SkScalerContext.h"
#include "src/ports/SkScalerContext_idwr.h"
#include "src/sfnt/SkOTUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

IdwrFontHolder::IdwrFontHolder(sk_sp<SkData> data, uint32_t index)
        : fData(std::move(data)), fIndex(index), fFont(nullptr) {
    fFont = idwr_font_open(static_cast<const uint8_t*>(fData->data()), fData->size(), index);
}

IdwrFontHolder::~IdwrFontHolder() {
    if (fFont) {
        idwr_font_close(fFont);
    }
}

SkTypeface* SkCreateTypefaceFromIdwr(sk_sp<SkData> bytes,
                                     uint32_t faceIndex,
                                     uint32_t simulations,
                                     const idwr_axis_value* axes,
                                     uint32_t axisCount,
                                     SkFontStyle style,
                                     int renderingMode,
                                     float gamma,
                                     float contrast,
                                     float clearTypeLevel) {
    if (!bytes) {
        return nullptr;
    }
    sk_sp<IdwrFontHolder> holder(new IdwrFontHolder(std::move(bytes), faceIndex));
    if (!holder->fFont) {
        return nullptr;
    }
    std::vector<idwr_axis_value> values;
    if (axes) {
        values.assign(axes, axes + axisCount);
    }
    sk_sp<IdwrFontTypeface> typeface =
            IdwrFontTypeface::Make(std::move(holder), simulations, std::move(values), style,
                                   SkFontArguments::Palette{0, nullptr, 0}, renderingMode, gamma,
                                   contrast, clearTypeLevel);
    return typeface.release();
}

sk_sp<IdwrFontTypeface> IdwrFontTypeface::Make(sk_sp<IdwrFontHolder> font,
                                               uint32_t simulations,
                                               std::vector<idwr_axis_value> axes,
                                               SkFontStyle style,
                                               const SkFontArguments::Palette& palette,
                                               int renderingMode,
                                               float gamma,
                                               float contrast,
                                               float clearTypeLevel) {
    // CreateFontFace refuses a face whose COLR or CPAL fail DirectWrite's validation.
    {
        SkAutoMutexExclusive lock(font->fMutex);
        if (idwr_font_check_color_tables(font->fFont) != 0) {
            return nullptr;
        }
    }
    sk_sp<IdwrFontTypeface> typeface(new IdwrFontTypeface(
            std::move(font), simulations, std::move(axes), style, palette));
    // The defaults of DWriteFontTypeface, then what Gecko passed.
    typeface->fRenderingMode = renderingMode;
    typeface->fGamma = gamma;
    typeface->fContrast = contrast;
    typeface->fClearTypeLevel = clearTypeLevel;
    return typeface;
}

IdwrFontTypeface::IdwrFontTypeface(sk_sp<IdwrFontHolder> font,
                                   uint32_t simulations,
                                   std::vector<idwr_axis_value> axes,
                                   SkFontStyle style,
                                   const SkFontArguments::Palette& palette)
        : SkTypeface(style, false)
        , fHolder(std::move(font))
        , fSimulations(simulations)
        , fAxes(std::move(axes))
        , fRequestedPalette{palette.index, nullptr, 0} {
    if (palette.overrides && palette.overrideCount > 0) {
        fPaletteOverrides.assign(palette.overrides, palette.overrides + palette.overrideCount);
    }
    fRequestedPalette.overrides = fPaletteOverrides.data();
    fRequestedPalette.overrideCount = (int)fPaletteOverrides.size();
    this->initialize();
}

IdwrFontTypeface::~IdwrFontTypeface() = default;

void IdwrFontTypeface::initialize() {
    // The font is shared by every clone of this typeface, and other strikes may be using it.
    SkAutoMutexExclusive lock(fHolder->fMutex);
    const idwr_font* font = fHolder->fFont;
    fGlyphCount = idwr_font_glyph_count(font);
    idwr_font_metrics m;
    idwr_get_font_metrics(font, &m);
    fUpem = m.designUnitsPerEm;
    if (idwr_font_is_monospaced(font)) {
        this->setIsFixedPitch(true);
    }

    float coords[64];
    int axisCount = idwr_normalize_axes(font, fAxes.data(), (uint32_t)fAxes.size(), coords, 64);
    if (axisCount > 0) {
        fCoords.assign(coords, coords + axisCount);
    }

    idwr_face* face = idwr_face_create(font, fSimulations, fAxes.data(), (uint32_t)fAxes.size());
    if (!face) {
        return;
    }
    fIsColorFont = idwr_face_is_color_font(face) != 0;
    fPaletteCount = idwr_face_get_color_palette_count(face);
    if (fIsColorFont && fPaletteCount > 0) {
        // Out of range palette index values are palette 0. Overrides still apply.
        uint32_t base = 0;
        if (fRequestedPalette.index >= 0 && (uint32_t)fRequestedPalette.index < fPaletteCount) {
            base = (uint32_t)fRequestedPalette.index;
        }
        uint32_t n = idwr_face_get_palette_entry_count(face);
        std::vector<idwr_color_f> entries(n);
        if (idwr_face_get_palette_entries(face, base, 0, n, entries.data()) == 0) {
            fPalette.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                fPalette[i] = SkColorSetARGB(sk_float_round2int(entries[i].a * 255),
                                             sk_float_round2int(entries[i].r * 255),
                                             sk_float_round2int(entries[i].g * 255),
                                             sk_float_round2int(entries[i].b * 255));
            }
            for (const auto& o : fPaletteOverrides) {
                if (o.index < n) {
                    fPalette[o.index] = o.color;
                }
            }
        }
    }
    idwr_face_destroy(face);
}

std::unique_ptr<SkScalerContext> IdwrFontTypeface::onCreateScalerContext(
        const SkScalerContextEffects& effects, const SkDescriptor* desc) const {
    return std::make_unique<SkScalerContext_Idwr>(
            *const_cast<IdwrFontTypeface*>(this), effects, desc);
}

void IdwrFontTypeface::onFilterRec(SkScalerContextRec* rec) const {
    rec->useStrokeForFakeBold();

    if (rec->fFlags & SkScalerContext::kLCD_Vertical_Flag) {
        rec->fMaskFormat = SkMask::kA8_Format;
        rec->fFlags |= SkScalerContext::kGenA8FromLCD_Flag;
    }

    unsigned flagsWeDontSupport = SkScalerContext::kForceAutohinting_Flag |
                                  SkScalerContext::kEmbolden_Flag |
                                  SkScalerContext::kLCD_Vertical_Flag;
    rec->fFlags &= ~flagsWeDontSupport;

    SkFontHinting h = rec->getHinting();
    // DirectWrite2 allows for hinting to be turned off. Force everything else to normal.
    if (h != SkFontHinting::kNone) {
        h = SkFontHinting::kNormal;
    }
    rec->setHinting(h);

    // MOZ_SKIA: the values the font was made with, whatever SkSurfaceProps and the OS say.
    rec->setContrast(fContrast);
    rec->setDeviceGamma(fGamma);
}

sk_sp<SkTypeface> IdwrFontTypeface::onMakeClone(const SkFontArguments& args) const {
    // Skip if the current face index does not match the ttcIndex.
    if (fHolder->fIndex != SkTo<uint32_t>(args.getCollectionIndex())) {
        return sk_ref_sp(this);
    }

    uint32_t requested = fSimulations;
    if (args.getSyntheticBold().has_value()) {
        if (args.getSyntheticBold().value()) {
            requested |= IDWR_SIM_BOLD;
        } else {
            requested &= ~(uint32_t)IDWR_SIM_BOLD;
        }
    }
    if (args.getSyntheticOblique().has_value()) {
        if (args.getSyntheticOblique().value()) {
            requested |= IDWR_SIM_OBLIQUE;
        } else {
            requested &= ~(uint32_t)IDWR_SIM_OBLIQUE;
        }
    }

    float coords[64];
    int axisCount;
    {
        SkAutoMutexExclusive lock(fHolder->fMutex);
        axisCount = idwr_normalize_axes(fHolder->fFont, fAxes.data(), (uint32_t)fAxes.size(),
                                        coords, 64);
    }
    if (axisCount > 0) {
        // The current face's values, overridden by the arguments.
        std::vector<idwr_axis_value> values = fAxes;
        const auto& position = args.getVariationDesignPosition();
        for (int i = 0; i < position.coordinateCount; ++i) {
            bool found = false;
            for (auto& v : values) {
                if (v.tag == position.coordinates[i].axis) {
                    v.value = position.coordinates[i].value;
                    found = true;
                }
            }
            if (!found) {
                values.push_back({position.coordinates[i].axis, position.coordinates[i].value});
            }
        }
        return IdwrFontTypeface::Make(fHolder, requested, std::move(values), this->fontStyle(),
                                      args.getPalette(), fRenderingMode, fGamma, fContrast,
                                      fClearTypeLevel);
    }

    if (requested != fSimulations || args.getPalette().index != fRequestedPalette.index ||
        args.getPalette().overrideCount != fRequestedPalette.overrideCount ||
        (fRequestedPalette.overrideCount &&
         memcmp(args.getPalette().overrides, fRequestedPalette.overrides,
                fRequestedPalette.overrideCount * sizeof(fRequestedPalette.overrides[0])))) {
        return IdwrFontTypeface::Make(fHolder, requested, fAxes, this->fontStyle(),
                                      args.getPalette(), fRenderingMode, fGamma, fContrast,
                                      fClearTypeLevel);
    }
    return sk_ref_sp(this);
}

std::unique_ptr<SkStreamAsset> IdwrFontTypeface::onOpenStream(int* ttcIndex) const {
    *ttcIndex = (int)fHolder->fIndex;
    return SkMemoryStream::Make(fHolder->fData);
}

void IdwrFontTypeface::getGlyphToUnicodeMap(SkSpan<SkUnichar>) const {}

std::unique_ptr<SkAdvancedTypefaceMetrics> IdwrFontTypeface::onGetAdvancedMetrics() const {
    // PDF output only; Gecko prints through its own path.
    return nullptr;
}

void IdwrFontTypeface::onGetFontDescriptor(SkFontDescriptor* desc, bool* serialize) const {
    SkString family;
    this->getFamilyName(&family);
    desc->setFamilyName(family.c_str());
    desc->setStyle(this->fontStyle());
    desc->setPaletteIndex(fRequestedPalette.index);
    desc->setSyntheticBold(this->isSyntheticBold());
    desc->setSyntheticOblique(this->isSyntheticOblique());
    desc->setFactoryId(FactoryId);
    *serialize = false;
}

void IdwrFontTypeface::onCharsToGlyphs(SkSpan<const SkUnichar> uni,
                                       SkSpan<SkGlyphID> glyphs) const {
    SkASSERT(uni.size() == glyphs.size());
    SkAutoMutexExclusive lock(fHolder->fMutex);
    for (size_t i = 0; i < uni.size(); ++i) {
        glyphs[i] = idwr_font_glyph_index(fHolder->fFont, (uint32_t)uni[i]);
    }
}

int IdwrFontTypeface::onCountGlyphs() const { return fGlyphCount; }

void IdwrFontTypeface::getPostScriptGlyphNames(SkString*) const {}

int IdwrFontTypeface::onGetUPEM() const { return fUpem; }

void IdwrFontTypeface::onGetFamilyName(SkString* familyName) const {
    familyName->reset();
    sk_sp<SkTypeface::LocalizedStrings> names(this->createFamilyNameIterator());
    SkTypeface::LocalizedString first;
    if (names && names->next(&first)) {
        *familyName = first.fString;
    }
}

bool IdwrFontTypeface::onGetPostScriptName(SkString*) const { return false; }

int IdwrFontTypeface::onGetResourceName(SkString*) const { return 0; }

SkTypeface::LocalizedStrings* IdwrFontTypeface::onCreateFamilyNameIterator() const {
    sk_sp<SkTypeface::LocalizedStrings> nameIter =
            SkOTUtils::LocalizedStrings_NameTable::MakeForFamilyNames(*this);
    if (!nameIter) {
        nameIter = sk_make_sp<SkOTUtils::LocalizedStrings_SingleName>(SkString(), SkString("und"));
    }
    return nameIter.release();
}

bool IdwrFontTypeface::onGlyphMaskNeedsCurrentColor() const { return fPaletteCount > 0; }

namespace {
struct FvarAxis {
    uint32_t tag;
    float min, def, max;
    uint16_t flags;
};

float fixed16_16(const uint8_t* p) {
    int32_t v = (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
    return (float)v / 65536.0f;
}

std::vector<FvarAxis> read_fvar(const idwr_font* font) {
    std::vector<FvarAxis> axes;
    uint32_t size = 0;
    const uint8_t* t = idwr_font_table(font, IDWR_TAG('f', 'v', 'a', 'r'), &size);
    if (!t || size < 16) {
        return axes;
    }
    uint32_t offset = (uint32_t)t[4] << 8 | t[5];
    uint32_t count = (uint32_t)t[8] << 8 | t[9];
    uint32_t axisSize = (uint32_t)t[10] << 8 | t[11];
    if (axisSize < 20) {
        return axes;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t o = offset + i * axisSize;
        if (o + 20 > size) {
            break;
        }
        const uint8_t* a = t + o;
        FvarAxis axis;
        axis.tag = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
        axis.min = fixed16_16(a + 4);
        axis.def = fixed16_16(a + 8);
        axis.max = fixed16_16(a + 12);
        axis.flags = (uint16_t)(a[16] << 8 | a[17]);
        axes.push_back(axis);
    }
    return axes;
}

int count_variable(const std::vector<FvarAxis>& axes) {
    int n = 0;
    for (const auto& a : axes) {
        n += a.min < a.max;
    }
    return n;
}
}  // namespace

int IdwrFontTypeface::onGetVariationDesignPosition(
        SkSpan<SkFontArguments::VariationPosition::Coordinate> coordinates) const {
    if (fCoords.empty()) {
        return 0;
    }
    std::vector<FvarAxis> axes;
    {
        SkAutoMutexExclusive lock(fHolder->fMutex);
        axes = read_fvar(fHolder->fFont);
    }
    // The variable axes only, as DirectWrite counts them (minimum below maximum).
    int variable = count_variable(axes);
    if (coordinates.size() < (size_t)variable) {
        return variable;
    }
    size_t k = 0;
    for (const auto& a : axes) {
        if (!(a.min < a.max)) {
            continue;
        }
        float value = a.def;
        for (const auto& v : fAxes) {
            if (v.tag == a.tag) {
                value = v.value;
                break;
            }
        }
        coordinates[k].axis = a.tag;
        coordinates[k].value = std::min(std::max(value, a.min), a.max);
        ++k;
    }
    return variable;
}

int IdwrFontTypeface::onGetVariationDesignParameters(
        SkSpan<SkFontParameters::Variation::Axis> parameters) const {
    if (fCoords.empty()) {
        return 0;
    }
    std::vector<FvarAxis> axes;
    {
        SkAutoMutexExclusive lock(fHolder->fMutex);
        axes = read_fvar(fHolder->fFont);
    }
    int variable = count_variable(axes);
    if (parameters.size() < (size_t)variable) {
        return variable;
    }
    size_t k = 0;
    for (const auto& a : axes) {
        if (!(a.min < a.max)) {
            continue;
        }
        parameters[k].tag = a.tag;
        parameters[k].min = a.min;
        parameters[k].def = a.def;
        parameters[k].max = a.max;
        parameters[k].setHidden(a.flags & 1);  // the fvar HIDDEN_AXIS flag
        ++k;
    }
    return variable;
}

bool IdwrFontTypeface::onIsSyntheticBold() const { return fSimulations & IDWR_SIM_BOLD; }

bool IdwrFontTypeface::onIsSyntheticOblique() const { return fSimulations & IDWR_SIM_OBLIQUE; }

int IdwrFontTypeface::onGetTableTags(SkSpan<SkFontTableTag> tags) const {
    int ttcIndex;
    std::unique_ptr<SkStreamAsset> stream = this->openStream(&ttcIndex);
    return stream ? SkFontStream::GetTableTags(stream.get(), ttcIndex, tags) : 0;
}

size_t IdwrFontTypeface::onGetTableData(SkFontTableTag tag, size_t offset, size_t length,
                                        void* data) const {
    SkAutoMutexExclusive lock(fHolder->fMutex);
    uint32_t size = 0;
    const uint8_t* table = idwr_font_table(fHolder->fFont, tag, &size);
    if (!table || offset > size) {
        return 0;
    }
    size_t n = std::min<size_t>(length, size - offset);
    if (data) {
        memcpy(data, table + offset, n);
    }
    return n;
}

sk_sp<SkData> IdwrFontTypeface::onCopyTableData(SkFontTableTag tag) const {
    uint32_t size = 0;
    const uint8_t* table;
    {
        SkAutoMutexExclusive lock(fHolder->fMutex);
        table = idwr_font_table(fHolder->fFont, tag, &size);
    }
    if (!table) {
        return nullptr;
    }
    // The table lives in the font's bytes, which the holder keeps.
    sk_sp<IdwrFontHolder> holder = fHolder;
    return SkData::MakeWithProc(
            table, size,
            [](const void*, void* ctx) { static_cast<IdwrFontHolder*>(ctx)->unref(); },
            holder.release());
}
