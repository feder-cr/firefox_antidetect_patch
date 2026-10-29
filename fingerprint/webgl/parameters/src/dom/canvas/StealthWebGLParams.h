/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_CANVAS_STEALTHWEBGLPARAMS_H_
#define DOM_CANVAS_STEALTHWEBGLPARAMS_H_

// What a WebGL context says about the machine it runs on, replaced by the
// values the session declares: getParameter, the renderer and vendor strings,
// getShaderPrecisionFormat and the extension list. Called from hooks in
// ClientWebGLContext.cpp; every function reports "not overridden" (false or
// Nothing()) when its pref is empty, and the caller then runs Mozilla's code.
// The prefs are the contract with the core: zoom.stealth.webgl.* and
// zoom.stealth.webgl2.*.

#include <string>

#include "WebGLTypes.h"
#include "js/TypeDecls.h"
#include "mozilla/Maybe.h"
#include "nsTArray.h"
#include "nsString.h"

namespace mozilla {

class ClientWebGLContext;
class ErrorResult;

// getParameter(pname) from one of three prefs, each a comma-separated list of
// "ENUM|VALUE" entries (ENUM in decimal):
//   zoom.stealth.webgl.int_params    VALUE an integer; the page reads a number;
//   zoom.stealth.webgl.float_params  VALUE "x" (a number) or "x:y" (a
//                                    Float32Array of two);
//   zoom.stealth.webgl.int2_params   VALUE "x:y" (an Int32Array of two).
// Checked in that order, before Mozilla's switch, so that the numerics of the
// host driver (Linux Mesa: MAX_TEXTURE_SIZE and the like) read as the Windows
// ANGLE values of the declared machine, and the hash of the whole parameter
// block is the one a Windows machine gives. True when it set aRetval; the
// caller returns. An entry that does not parse is skipped, as if absent.
bool StealthWebGLParameterOverride(ClientWebGLContext& aCtx, JSContext* aCx,
                                   GLenum aPname,
                                   JS::MutableHandle<JS::Value> aRetval,
                                   ErrorResult& aRv);

// gl.RENDERER when zoom.stealth.webgl.renderer is set: the pref, through
// SanitizeRenderer, as a retail browser does with the real renderer. True and
// aRet set when the pref is set.
//
// Measured on 2026-08-24 against a retail Firefox 151.0 signed by Mozilla
// Corporation, updater off, throwaway profile, on WebGL 1 and 2:
//
//     masked   ANGLE (Intel, ... ps_5_0), or similar
//     unmasked ANGLE (Intel, ... ps_5_0), or similar   <- identical
//
// The two are identical because webgl.sanitize-unmasked-renderer defaults to
// true, so both go through the same SanitizeRenderer. Making them differ is
// what would be the tell.
bool StealthWebGLRendererOverride(Maybe<std::string>& aRet);

// UNMASKED_RENDERER_WEBGL when zoom.stealth.webgl.renderer is set: sanitized
// like gl.RENDERER while webgl.sanitize-unmasked-renderer is true (the
// default), the pref as it is otherwise.
bool StealthWebGLUnmaskedRendererOverride(Maybe<std::string>& aRet);

// UNMASKED_VENDOR_WEBGL when zoom.stealth.webgl.vendor is set. The pref is
// read through StaticPrefs (a DataMutexString mirrored "always"): it reaches
// content processes and is thread-safe, so workers (OffscreenCanvas with
// WebGL) read the same declared value.
bool StealthWebGLUnmaskedVendorOverride(Maybe<std::string>& aRet);

// getShaderPrecisionFormat from zoom.stealth.webgl.shader_precisions, a
// comma-separated list of "SHADERTYPE*PRECISIONTYPE|rmin:rmax:p" entries in
// decimal; the three numbers are kept to 8 bits. A check can hash the twelve
// triples (two shader types times six precision types) into one signal; the
// host driver's triples are constant across sessions and give its platform
// away, so the declared machine's triples replace them. Nothing() when the
// pref has no usable entry for the pair.
Maybe<webgl::ShaderPrecisionFormat> StealthWebGLShaderPrecision(
    GLenum aShaderType, GLenum aPrecisionType);

// The extension whitelist: zoom.stealth.webgl.extensions (WebGL 1) or
// zoom.stealth.webgl2.extensions (WebGL 2), comma-separated names, trimmed. An
// empty pref means no override. The pref is re-read and re-parsed on every
// call.
//
// With a whitelist set: Some(true) for a name in the list, Some(false) for any
// other, whatever the driver provides. Nothing() when there is no whitelist.
Maybe<bool> StealthWebGLExtensionVerdict(bool aIsWebGL2, WebGLExtensionID aExt);

// With a whitelist set, appends the list itself, in order, as UTF-16 and
// returns true: getSupportedExtensions then agrees with IsSupported on the
// names it lists. It does not run the caller-type check that IsSupported
// applies first (IsExtensionForbiddenForCaller).
bool StealthWebGLExtensionList(bool aIsWebGL2, nsTArray<nsString>& aOut);

}  // namespace mozilla

#endif  // DOM_CANVAS_STEALTHWEBGLPARAMS_H_
