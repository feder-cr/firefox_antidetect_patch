/*
 * A typeface drawn by idwr (gfx/idwr), the portable DirectWrite 10.0.26100.9711 text
 * rasterizer, on every OS. It stands where SkCreateTypefaceFromDWriteFont stands on Windows:
 * the same rendering mode, gamma, contrast and ClearType level reach the scaler context.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef SkTypeface_idwr_DEFINED
#define SkTypeface_idwr_DEFINED

#include "include/core/SkData.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypeface.h"
#include "include/private/base/SkAPI.h"

#include <stdint.h>

#define IDWR_STATIC 1
#include "idwr/idwr.h"

/** `bytes` is the whole sfnt file (the typeface keeps it alive), `faceIndex` the face of a
 *  collection. `simulations` is IDWR_SIM_BOLD | IDWR_SIM_OBLIQUE (DWRITE_FONT_SIMULATIONS),
 *  `axes` the instance's axis values as IDWriteFontResource::CreateFontFace takes them.
 *  `renderingMode` is a DWRITE_RENDERING_MODE number. Returns a new reference, or null when idwr
 *  refuses the font (as DirectWrite would not make the face). */
SK_API SkTypeface* SkCreateTypefaceFromIdwr(sk_sp<SkData> bytes,
                                            uint32_t faceIndex,
                                            uint32_t simulations,
                                            const idwr_axis_value* axes,
                                            uint32_t axisCount,
                                            SkFontStyle style,
                                            int renderingMode,
                                            float gamma,
                                            float contrast,
                                            float clearTypeLevel);

#endif
