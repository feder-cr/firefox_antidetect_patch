/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Invisible attach of a Debugger (feature debugger-invisible-attach).
//
// Stealth (invisible-attach): read stealthMode BEFORE the no-GC region
// (JS_GetProperty can GC). When the Debugger object has the property
// stealthMode set to true (Juggler sets it right after `new Debugger()`,
// juggler/content/Runtime.js:131, before any addDebuggee),
// Debugger::addDebuggeeGlobal keeps tracking the global (debuggees,
// debuggeeZones: steps 2-3) but DOESN'T register in Realm::debuggers_ (step 1)
// and DOESN'T enter debug mode (step 5). Result: global->getDebuggers() stays
// empty => hasDebuggers()==false, Ion stays enabled, there is no
// slowPathOnNativeCall overhead, and the page realm is web-indistinguishable
// from a never-debugged realm. executeInGlobal/page.evaluate and GC teardown
// use debuggees/debuggeeZones (not debuggers_), so Juggler keeps working.
//
// Debugger::invisibleDebuggee_ (Debugger.h) is set when this Debugger attached
// its debuggees OUT of Realm::debuggers_ (stealthMode), so the page realm
// reports no debuggers (hasDebuggers()==false). It is cached at addDebuggee
// time because removeDebuggeeGlobal can run during GC sweep where JS (the
// stealthMode property read) cannot run, and there we must not try to erase
// from debuggers_ (findDebuggerInVector asserts the entry exists). It must stay
// a plain bool, placed between allocationsLog and trackingAllocationSites.

#ifndef debugger_StealthDebugger_h
#define debugger_StealthDebugger_h

#include "mozilla/Assertions.h"  // for MOZ_ASSERT

#include "js/PropertyAndElement.h"  // for JS_GetProperty
#include "js/RootingAPI.h"          // for JS::Rooted
#include "js/TypeDecls.h"           // for JSContext, JSObject
#include "js/Value.h"               // for JS::Value

namespace js {

// Reads the stealthMode property of the Debugger object. Call it BEFORE the
// no-GC region of addDebuggeeGlobal: JS_GetProperty can run JS and so can GC.
// The return value of JS_GetProperty is deliberately not propagated: on failure
// the result is false (not stealth) and the caller carries on.
inline bool StealthDebuggerReadMode(JSContext* cx, JSObject* debuggerObject) {
  MOZ_ASSERT(debuggerObject);
  JS::Rooted<JSObject*> dbgObj(cx, debuggerObject);
  JS::Rooted<JS::Value> stealthVal(cx);
  return JS_GetProperty(cx, dbgObj, "stealthMode", &stealthVal) &&
         stealthVal.isTrue();
}

// Releases the mozilla::ScopeExit rollback guards on the early successful
// return of an invisible attach. In that case step 4 (alloc-metadata builder,
// never attached for a stealth debuggee, so allocationMetadataBuilder_ stays
// null = vanilla even if tracking is ever enabled) and step 5 (setIsDebuggee,
// updateDebuggerObservesWasm/Coverage, ensureExecutionObservabilityOfRealm: the
// Realm does NOT enter debug mode) are skipped.
template <typename... Guards>
inline void StealthDebuggerReleaseGuards(Guards&... guards) {
  (guards.release(), ...);
}

}  // namespace js

#endif  // debugger_StealthDebugger_h
