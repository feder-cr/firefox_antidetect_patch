/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthWebGLParams.h"

#include <array>
#include <cstdio>
#include <vector>

#include "ClientWebGLContext.h"
#include "js/Value.h"
#include "mozilla/StaticPrefs_webgl.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/dom/TypedArray.h"
#include "nsDependentSubstring.h"

namespace mozilla {

namespace webgl {
std::string SanitizeRenderer(const std::string&);  // SanitizeRenderer.cpp
}  // namespace webgl

namespace {

// Parse a comma-separated "ENUM|VALUE" pref entry for the getParameter
// override. ENUM in decimal, VALUE may itself contain ':' for vec2. Returns
// true if `aPname` was found and fills out the split body into `aValueOut`.
bool LookupStealthParamEntry(const nsACString& aPref, GLenum aPname,
                             nsACString& aValueOut) {
  if (aPref.IsEmpty()) return false;
  const char* data = aPref.BeginReading();
  int32_t len = int32_t(aPref.Length());
  int32_t start = 0;
  for (int32_t i = 0; i <= len; i++) {
    if (i == len || data[i] == ',') {
      if (i > start) {
        nsDependentCSubstring entry(data + start, i - start);
        int32_t bar = entry.FindChar('|');
        if (bar > 0) {
          nsDependentCSubstring namePart(entry, 0, bar);
          nsAutoCString nameStr(namePart);
          int32_t enumVal = 0;
          if (sscanf(nameStr.get(), "%d", &enumVal) == 1 &&
              GLenum(enumVal) == aPname) {
            aValueOut = Substring(entry, bar + 1, entry.Length() - bar - 1);
            return true;
          }
        }
      }
      start = i + 1;
    }
  }
  return false;
}

// The whitelist for WebGL 1 or WebGL 2: the trimmed names of the
// corresponding pref. Empty = no override (use the native list).
std::vector<nsCString> GetStealthExtensionWhitelist(bool aIsWebGL2) {
  std::vector<nsCString> out;
  nsAutoCString raw;
  {
    auto lock = aIsWebGL2
                    ? mozilla::StaticPrefs::zoom_stealth_webgl2_extensions()
                    : mozilla::StaticPrefs::zoom_stealth_webgl_extensions();
    raw = *lock;
  }
  if (raw.IsEmpty()) return out;
  int32_t start = 0;
  while (start < static_cast<int32_t>(raw.Length())) {
    int32_t comma = raw.FindChar(',', start);
    if (comma < 0) comma = raw.Length();
    nsAutoCString item(Substring(raw, start, comma - start));
    item.Trim(" \t\r\n");
    if (!item.IsEmpty()) out.push_back(item);
    start = comma + 1;
  }
  return out;
}

}  // namespace

StealthWebGLParameterAnswer::~StealthWebGLParameterAnswer() {
  // Null is Firefox's "no such pname here" (with its error already queued):
  // that answer is the retail one and stays.
  if (mRetval.isNull() || mRv.Failed()) {
    return;
  }
  StealthWebGLParameterOverride(mCtx, mCx, mPname, mRetval, mRv);
}

bool StealthWebGLParameterOverride(ClientWebGLContext& aCtx, JSContext* aCx,
                                   GLenum aPname,
                                   JS::MutableHandle<JS::Value> aRetval,
                                   ErrorResult& aRv) {
  nsWrapperCache* const creator = &aCtx;
  nsAutoCString intPref, floatPref, int2Pref;
  {
    auto lk = mozilla::StaticPrefs::zoom_stealth_webgl_int_params();
    intPref = *lk;
  }
  {
    auto lk = mozilla::StaticPrefs::zoom_stealth_webgl_float_params();
    floatPref = *lk;
  }
  {
    auto lk = mozilla::StaticPrefs::zoom_stealth_webgl_int2_params();
    int2Pref = *lk;
  }
  nsAutoCString body;
  if (LookupStealthParamEntry(intPref, aPname, body)) {
    long long v = 0;
    nsAutoCString bodyCopy(body);
    if (sscanf(bodyCopy.get(), "%lld", &v) == 1) {
      aRetval.set(JS::NumberValue(double(v)));
      return true;
    }
  }
  if (LookupStealthParamEntry(floatPref, aPname, body)) {
    nsAutoCString bodyCopy(body);
    int32_t colon = bodyCopy.FindChar(':');
    if (colon > 0) {
      nsAutoCString a(Substring(bodyCopy, 0, colon));
      nsAutoCString b(
          Substring(bodyCopy, colon + 1, bodyCopy.Length() - colon - 1));
      double fa = 0, fb = 0;
      if (sscanf(a.get(), "%lf", &fa) == 1 &&
          sscanf(b.get(), "%lf", &fb) == 1) {
        const auto arr = std::array<float, 2>{float(fa), float(fb)};
        aRetval.set(JS::ObjectOrNullValue(
            dom::Float32Array::Create(aCx, creator, arr, aRv)));
        return true;
      }
    } else {
      double fv = 0;
      if (sscanf(bodyCopy.get(), "%lf", &fv) == 1) {
        aRetval.set(JS::NumberValue(fv));
        return true;
      }
    }
  }
  if (LookupStealthParamEntry(int2Pref, aPname, body)) {
    nsAutoCString bodyCopy(body);
    int32_t colon = bodyCopy.FindChar(':');
    if (colon > 0) {
      nsAutoCString a(Substring(bodyCopy, 0, colon));
      nsAutoCString b(
          Substring(bodyCopy, colon + 1, bodyCopy.Length() - colon - 1));
      int32_t ia = 0, ib = 0;
      if (sscanf(a.get(), "%d", &ia) == 1 && sscanf(b.get(), "%d", &ib) == 1) {
        const auto arr = std::array<int32_t, 2>{ia, ib};
        aRetval.set(JS::ObjectOrNullValue(
            dom::Int32Array::Create(aCx, creator, arr, aRv)));
        return true;
      }
    }
  }
  return false;
}

bool StealthWebGLRendererOverride(Maybe<std::string>& aRet) {
  nsAutoCString stealthRenderer;
  {
    auto lock = mozilla::StaticPrefs::zoom_stealth_webgl_renderer();
    stealthRenderer = *lock;
  }
  if (stealthRenderer.IsEmpty()) {
    return false;
  }
  aRet = Some(std::string{webgl::SanitizeRenderer(stealthRenderer.get())});
  return true;
}

bool StealthWebGLUnmaskedRendererOverride(Maybe<std::string>& aRet) {
  nsAutoCString stealthRenderer;
  {
    auto lock = mozilla::StaticPrefs::zoom_stealth_webgl_renderer();
    stealthRenderer = *lock;
  }
  if (stealthRenderer.IsEmpty()) {
    return false;
  }
  if (StaticPrefs::webgl_sanitize_unmasked_renderer()) {
    aRet = Some(webgl::SanitizeRenderer(stealthRenderer.get()));
  } else {
    aRet = Some(nsCString{stealthRenderer});
  }
  return true;
}

bool StealthWebGLUnmaskedVendorOverride(Maybe<std::string>& aRet) {
  nsAutoCString stealthVendor;
  {
    auto lock = mozilla::StaticPrefs::zoom_stealth_webgl_vendor();
    stealthVendor = *lock;
  }
  if (stealthVendor.IsEmpty()) {
    return false;
  }
  aRet = Some(nsCString{stealthVendor});
  return true;
}

Maybe<webgl::ShaderPrecisionFormat> StealthWebGLShaderPrecision(
    const GLenum aShaderType, const GLenum aPrecisionType) {
  nsAutoCString pref;
  {
    auto lk = mozilla::StaticPrefs::zoom_stealth_webgl_shader_precisions();
    pref = *lk;
  }
  if (pref.IsEmpty()) {
    return Nothing();
  }
  // Find entry "SHADERTYPE*PRECISIONTYPE|rmin:rmax:p"
  const char* data = pref.BeginReading();
  int32_t plen = int32_t(pref.Length());
  int32_t start = 0;
  for (int32_t i = 0; i <= plen; i++) {
    if (i == plen || data[i] == ',') {
      if (i > start) {
        nsDependentCSubstring entry(data + start, i - start);
        int32_t bar = entry.FindChar('|');
        if (bar > 0) {
          nsAutoCString key(Substring(entry, 0, bar));
          nsAutoCString val(
              Substring(entry, bar + 1, entry.Length() - bar - 1));
          int32_t star = key.FindChar('*');
          if (star > 0) {
            unsigned int st = 0, pt = 0;
            if (sscanf(key.get(), "%u*%u", &st, &pt) == 2 &&
                st == aShaderType && pt == aPrecisionType) {
              int rmin = 0, rmax = 0, prec = 0;
              if (sscanf(val.get(), "%d:%d:%d", &rmin, &rmax, &prec) == 3) {
                webgl::ShaderPrecisionFormat fake;
                fake.rangeMin = static_cast<uint8_t>(rmin & 0xFF);
                fake.rangeMax = static_cast<uint8_t>(rmax & 0xFF);
                fake.precision = static_cast<uint8_t>(prec & 0xFF);
                return Some(fake);
              }
            }
          }
        }
      }
      start = i + 1;
    }
  }
  return Nothing();
}

Maybe<bool> StealthWebGLExtensionVerdict(const bool aIsWebGL2,
                                         const WebGLExtensionID aExt) {
  auto whitelist = GetStealthExtensionWhitelist(aIsWebGL2);
  if (whitelist.empty()) {
    return Nothing();
  }
  const auto& extName = GetExtensionName(aExt);
  for (const auto& allowed : whitelist) {
    if (allowed.Equals(extName)) return Some(true);
  }
  return Some(false);
}

bool StealthWebGLExtensionList(const bool aIsWebGL2,
                               nsTArray<nsString>& aOut) {
  auto whitelist = GetStealthExtensionWhitelist(aIsWebGL2);
  if (whitelist.empty()) {
    return false;
  }
  for (const auto& name : whitelist) {
    aOut.AppendElement(NS_ConvertUTF8toUTF16(name));
  }
  return true;
}

}  // namespace mozilla
