/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthHybridXray.h"

#include <type_traits>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/ProxyHandlerUtils.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "xpcprivate.h"
#include "XrayWrapper.h"

#include "js/CallAndConstruct.h"
#include "js/Class.h"
#include "js/GlobalObject.h"
#include "js/HeapAPI.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/ProtoKey.h"  // JSProto_*
#include "js/Proxy.h"
#include "js/Realm.h"
#include "js/RootingAPI.h"
#include "js/ValueArray.h"
#include "js/Wrapper.h"
#include "js/Zone.h"

using namespace mozilla::dom;
using namespace JS;
using namespace mozilla;

using js::BaseProxyHandler;
using js::Wrapper;

namespace xpc {

/*
 * STEALTHFOX [B322]: HYBRID XRAYS. The rule, and why it lives in the wrapper,
 * are written once, above the class in StealthHybridXray.h.
 */

bool IsHybridXrayWrapper(JSObject* obj) {
  if (!js::IsProxy(obj)) {
    return false;
  }
  const BaseProxyHandler* handler = js::GetProxyHandler(obj);
  return handler == &HybridXrayDOM::singleton ||
         handler == &HybridXrayJS::singleton;
}

// The page's object behind a hybrid Xray. For a window that is the
// WindowProxy, as for any wrapper: what the page itself reaches.
static JSObject* HybridTarget(JSObject* wrapper) {
  JSObject* target = Wrapper::wrappedObject(wrapper);
  JS::ExposeObjectToActiveJS(target);
  return target;
}

// XrayWrapper.cpp keeps its own IsTypedArrayKey (inline, not in a header). The
// same range test, asserted below so a reordering of js/ProtoKey.h breaks this
// file as it breaks that one.
static_assert(JSProto_Uint8ClampedArray - JSProto_Int8Array == 8,
              "New prototype added in typed array range");
static bool IsHybridTypedArrayKey(JSProtoKey key) {
  return key >= JSProto_Int8Array && key <= JSProto_Uint8ClampedArray;
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::nativeOwnPropertyDescriptor(
    JSContext* cx, HandleObject wrapper, HandleId id,
    MutableHandle<Maybe<PropertyDescriptor>> desc) const {
  desc.reset();
  if constexpr (std::is_same_v<Traits, JSXrayTraits>) {
    // The elements of a typed array are its data, and the Xray throws instead
    // of answering for them: they are the page's, so the standard view does
    // not know them.
    RootedObject holder(cx, JSXrayTraits::singleton.ensureHolder(cx, wrapper));
    if (!holder) {
      return false;
    }
    if (IsHybridTypedArrayKey(JSXrayTraits::getProtoKey(holder)) &&
        !JSXrayTraits::isPrototype(holder) &&
        IsArrayIndex(GetArrayIndexFromId(id))) {
      return true;
    }
  }
  return Base::getOwnPropertyDescriptor(cx, wrapper, id, desc);
}

// Is this level of the page's chain a STANDARD object, one the standards
// define: a WebIDL object (an instance, an interface or prototype object, the
// window) or a standard prototype or constructor of its realm. Every other
// level (an ordinary instance, a class's prototype) the page created.
static bool IsStandardLevel(JSObject* content) {
  JSObject* obj = js::UncheckedUnwrap(content, /* stopAtWindowProxy = */ false);
  return mozilla::dom::UseDOMXray(obj) ||
         JS::IdentifyStandardPrototype(obj) != JSProto_Null ||
         JS::IdentifyStandardConstructor(obj) != JSProto_Null;
}

// The native view of ONE standard level: the Xray's own resolution of it.
static bool NativeOwn(JSContext* cx, HandleObject level, HandleId id,
                      MutableHandle<Maybe<PropertyDescriptor>> desc) {
  const BaseProxyHandler* handler = js::GetProxyHandler(level);
  js::AutoEnterPolicy policy(cx, handler, level, id,
                             BaseProxyHandler::GET_PROPERTY_DESCRIPTOR, true);
  if (handler == &HybridXrayDOM::singleton) {
    return HybridXrayDOM::singleton.nativeOwnPropertyDescriptor(cx, level, id,
                                                                desc);
  }
  return HybridXrayJS::singleton.nativeOwnPropertyDescriptor(cx, level, id,
                                                             desc);
}

// THE RULE, LEVEL BY LEVEL OF THE PAGE'S OWN CHAIN (StealthHybridXray.h, above
// the class). A standard level answers with its native view, which no
// replacement the page made reaches; a level the page created answers with its
// own properties. A name the native view of some standard level knows is
// STANDARD for this object, and the answer is the first page-created level that
// defines it before that standard level (Point.prototype.toString, a custom
// element's click), or else the native definition: what the page added on the
// standard levels does not count for it (document.querySelector = trap). A name
// no standard level knows is not the standard's: `desc` stays empty and the
// caller asks the page's object as the page would, so an expando still shadows
// a class member of the same name.
//
// Walking the page's real chain has one price, and it is stated: a scripted
// Proxy the page inserts AS A PROTOTYPE inside a standard chain is asked for
// its own properties like any page-created level, and its traps run.
static bool ChainLookup(JSContext* cx, HandleObject wrapper, HandleId id,
                        MutableHandle<Maybe<PropertyDescriptor>> desc) {
  desc.reset();
  Rooted<Maybe<PropertyDescriptor>> pageFirst(cx);
  Rooted<Maybe<PropertyDescriptor>> levelDesc(cx);
  RootedObject level(cx, wrapper);
  RootedObject content(cx);
  RootedObject proto(cx);
  while (level) {
    content = js::UncheckedUnwrap(level, /* stopAtWindowProxy = */ true);
    if (IsHybridXrayWrapper(level) && IsStandardLevel(content)) {
      if (!NativeOwn(cx, level, id, &levelDesc)) {
        return false;
      }
      if (levelDesc.isSome()) {
        desc.set(pageFirst.isSome() ? pageFirst.get() : levelDesc.get());
        return true;
      }
    } else if (pageFirst.isNothing()) {
      {
        JSAutoRealm ar(cx, content);
        RootedId wrappedId(cx, id);
        if (!JS_WrapId(cx, &wrappedId)) {
          return false;
        }
        if (!JS_GetOwnPropertyDescriptorById(cx, content, wrappedId,
                                             &levelDesc)) {
          return false;
        }
      }
      if (levelDesc.isSome()) {
        if (!JS_WrapPropertyDescriptor(cx, &levelDesc)) {
          return false;
        }
        pageFirst.set(levelDesc.get());
      }
    }
    {
      JSAutoRealm ar(cx, content);
      if (!JS_GetPrototype(cx, content, &proto)) {
        return false;
      }
    }
    if (!JS_WrapObject(cx, &proto)) {
      return false;
    }
    level = proto;
  }
  return true;
}

// A name the standard does not define: the page's, own or inherited, as the
// page's object answers it.
static bool ContentLookup(JSContext* cx, HandleObject wrapper, HandleId id,
                          MutableHandle<Maybe<PropertyDescriptor>> desc) {
  RootedObject target(cx, HybridTarget(wrapper));
  {
    JSAutoRealm ar(cx, target);
    RootedId wrappedId(cx, id);
    if (!JS_WrapId(cx, &wrappedId)) {
      return false;
    }
    RootedObject contentHolder(cx);
    if (!JS_GetPropertyDescriptorById(cx, target, wrappedId, desc,
                                      &contentHolder)) {
      return false;
    }
  }
  return JS_WrapPropertyDescriptor(cx, desc);
}

bool HybridXrayGetBareName(JSContext* cx, HandleObject wrapper, HandleId id,
                           MutableHandle<Maybe<PropertyDescriptor>> desc) {
  MOZ_ASSERT(IsHybridXrayWrapper(wrapper));
  if (!ChainLookup(cx, wrapper, id, desc)) {
    return false;
  }
  if (desc.isSome()) {
    return true;
  }

  // The page's top-level let, const and class: bindings of the global lexical
  // environment, which the page's own bare names consult before the window's
  // properties. Measured on the first candidate: `class Point` at the top of a
  // page script, `pt instanceof Point` from an evaluate, "Point is not
  // defined", where the page's realm finds it.
  RootedObject global(cx, js::UncheckedUnwrap(wrapper,
                                              /* stopAtWindowProxy = */ false));
  if (JS_IsGlobalObject(global)) {
    {
      JSAutoRealm ar(cx, global);
      RootedId wrappedId(cx, id);
      if (!JS_WrapId(cx, &wrappedId)) {
        return false;
      }
      RootedObject lexical(cx, JS_GlobalLexicalEnvironment(global));
      if (!JS_GetOwnPropertyDescriptorById(cx, lexical, wrappedId, desc)) {
        return false;
      }
      // A binding still in its temporal dead zone has no value to hand out:
      // the page's own read would throw, and so does a missing name.
      if (desc.isSome() && (!desc->hasValue() || desc->value().isMagic())) {
        desc.reset();
      }
    }
    if (desc.isSome()) {
      return JS_WrapPropertyDescriptor(cx, desc);
    }
  }
  return ContentLookup(cx, wrapper, id, desc);
}

// [[Get]] of a name the standard does not define, on the page's object. The
// receiver crosses as any value does: the wrapper itself becomes the page's
// object again.
static bool ContentGet(JSContext* cx, HandleObject wrapper,
                       HandleValue receiver, HandleId id,
                       MutableHandleValue vp) {
  RootedObject target(cx, HybridTarget(wrapper));
  {
    JSAutoRealm ar(cx, target);
    RootedId wrappedId(cx, id);
    if (!JS_WrapId(cx, &wrappedId)) {
      return false;
    }
    RootedValue contentReceiver(cx, receiver);
    if (!JS_WrapValue(cx, &contentReceiver) ||
        !JS_ForwardGetPropertyTo(cx, target, wrappedId, contentReceiver, vp)) {
      return false;
    }
  }
  return JS_WrapValue(cx, vp);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::getOwnPropertyDescriptor(
    JSContext* cx, HandleObject wrapper, HandleId id,
    MutableHandle<Maybe<PropertyDescriptor>> desc) const {
  if (!nativeOwnPropertyDescriptor(cx, wrapper, id, desc)) {
    return false;
  }
  if (desc.isSome()) {
    return true;
  }
  RootedObject target(cx, HybridTarget(wrapper));
  {
    JSAutoRealm ar(cx, target);
    RootedId wrappedId(cx, id);
    if (!JS_WrapId(cx, &wrappedId)) {
      return false;
    }
    if (!JS_GetOwnPropertyDescriptorById(cx, target, wrappedId, desc)) {
      return false;
    }
  }
  return JS_WrapPropertyDescriptor(cx, desc);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::hasOwn(JSContext* cx, HandleObject wrapper,
                                       HandleId id, bool* bp) const {
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!nativeOwnPropertyDescriptor(cx, wrapper, id, &desc)) {
    return false;
  }
  if (desc.isSome()) {
    *bp = true;
    return true;
  }
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  RootedId wrappedId(cx, id);
  if (!JS_WrapId(cx, &wrappedId)) {
    return false;
  }
  return JS_HasOwnPropertyById(cx, target, wrappedId, bp);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::has(JSContext* cx, HandleObject wrapper,
                                    HandleId id, bool* bp) const {
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!ChainLookup(cx, wrapper, id, &desc)) {
    return false;
  }
  if (desc.isSome()) {
    *bp = true;
    return true;
  }
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  RootedId wrappedId(cx, id);
  if (!JS_WrapId(cx, &wrappedId)) {
    return false;
  }
  return JS_HasPropertyById(cx, target, wrappedId, bp);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::get(JSContext* cx, HandleObject wrapper,
                                    HandleValue receiver, HandleId id,
                                    MutableHandleValue vp) const {
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!ChainLookup(cx, wrapper, id, &desc)) {
    return false;
  }
  if (desc.isNothing()) {
    return ContentGet(cx, wrapper, receiver, id, vp);
  }

  // Everything after here follows [[Get]] for ordinary objects, on the
  // definition the rule picked.
  desc->assertComplete();
  if (desc->isDataDescriptor()) {
    vp.set(desc->value());
    return true;
  }
  RootedObject getter(cx, desc->getter());
  if (!getter) {
    vp.setUndefined();
    return true;
  }
  return Call(cx, receiver, getter, HandleValueArray::empty(), vp);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::set(JSContext* cx, HandleObject wrapper,
                                    HandleId id, HandleValue v,
                                    HandleValue receiver,
                                    ObjectOpResult& result) const {
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!ChainLookup(cx, wrapper, id, &desc)) {
    return false;
  }

  // The accessor the rule picked runs its setter: the standard one for
  // `el.textContent = x`, the page's own for an accessor its class defines.
  if (desc.isSome() && desc->isAccessorDescriptor()) {
    RootedObject setter(cx, desc->setter());
    if (!setter) {
      return result.failGetterOnly();
    }
    RootedValue ignored(cx);
    if (!Call(cx, receiver, setter, HandleValueArray(v), &ignored)) {
      return false;
    }
    return result.succeed();
  }

  // Everything else is a write the page must see: `window.x = 1`, an expando
  // on a node, an element of the page's array.
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  RootedId wrappedId(cx, id);
  if (!JS_WrapId(cx, &wrappedId)) {
    return false;
  }
  RootedValue value(cx, v);
  RootedValue contentReceiver(cx, receiver);
  if (!JS_WrapValue(cx, &value) || !JS_WrapValue(cx, &contentReceiver)) {
    return false;
  }
  return JS_ForwardSetPropertyTo(cx, target, wrappedId, value, contentReceiver,
                                 result);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::defineProperty(
    JSContext* cx, HandleObject wrapper, HandleId id,
    Handle<PropertyDescriptor> desc, ObjectOpResult& result) const {
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  RootedId wrappedId(cx, id);
  if (!JS_WrapId(cx, &wrappedId)) {
    return false;
  }
  Rooted<PropertyDescriptor> wrappedDesc(cx, desc);
  if (!JS_WrapPropertyDescriptor(cx, &wrappedDesc)) {
    return false;
  }
  return JS_DefinePropertyById(cx, target, wrappedId, wrappedDesc, result);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::delete_(JSContext* cx, HandleObject wrapper,
                                        HandleId id,
                                        ObjectOpResult& result) const {
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  RootedId wrappedId(cx, id);
  if (!JS_WrapId(cx, &wrappedId)) {
    return false;
  }
  return JS_DeletePropertyById(cx, target, wrappedId, result);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::ownPropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  // The page's own keys, in the page's order, then the standard own keys the
  // page's object does not list.
  RootedObject target(cx, HybridTarget(wrapper));
  {
    JSAutoRealm ar(cx, target);
    if (!js::GetPropertyKeys(cx, target,
                             JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS,
                             props)) {
      return false;
    }
  }
  for (size_t i = 0; i < props.length(); ++i) {
    if (!JS_WrapId(cx, props[i])) {
      return false;
    }
  }

  RootedIdVector native(cx);
  if (!Base::ownPropertyKeys(cx, wrapper, &native)) {
    return false;
  }
  size_t contentCount = props.length();
  for (size_t i = 0; i < native.length(); ++i) {
    bool listed = false;
    for (size_t j = 0; j < contentCount && !listed; ++j) {
      listed = props[j] == native[i];
    }
    if (!listed && !props.append(native[i])) {
      return false;
    }
  }
  return true;
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::getOwnEnumerablePropertyKeys(
    JSContext* cx, HandleObject wrapper, MutableHandleIdVector props) const {
  return BaseProxyHandler::getOwnEnumerablePropertyKeys(cx, wrapper, props);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::enumerate(JSContext* cx, HandleObject wrapper,
                                          MutableHandleIdVector props) const {
  return BaseProxyHandler::enumerate(cx, wrapper, props);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::getPrototype(
    JSContext* cx, HandleObject wrapper, MutableHandleObject protop) const {
  // The page's prototype, so `el instanceof MyElement` and
  // `obj instanceof PageClass` hold. It is the chain ChainLookup walks too.
  RootedObject target(cx, HybridTarget(wrapper));
  {
    JSAutoRealm ar(cx, target);
    if (!JS_GetPrototype(cx, target, protop)) {
      return false;
    }
  }
  return JS_WrapObject(cx, protop);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::setPrototype(JSContext* cx,
                                             HandleObject wrapper,
                                             HandleObject proto,
                                             ObjectOpResult& result) const {
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  RootedObject contentProto(cx, proto);
  if (!JS_WrapObject(cx, &contentProto) ||
      !JS_SetPrototype(cx, target, contentProto)) {
    return false;
  }
  return result.succeed();
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::setImmutablePrototype(JSContext* cx,
                                                      HandleObject wrapper,
                                                      bool* succeeded) const {
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  return JS_SetImmutablePrototype(cx, target, succeeded);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::preventExtensions(
    JSContext* cx, HandleObject wrapper, ObjectOpResult& result) const {
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  return JS_PreventExtensions(cx, target, result);
}

template <typename Traits>
bool HybridXrayWrapper<Traits>::isExtensible(JSContext* cx,
                                             HandleObject wrapper,
                                             bool* extensible) const {
  RootedObject target(cx, HybridTarget(wrapper));
  JSAutoRealm ar(cx, target);
  return JS_IsExtensible(cx, target, extensible);
}

template <typename Traits>
MOZ_GLOBINIT const xpc::HybridXrayWrapper<Traits>
    xpc::HybridXrayWrapper<Traits>::singleton(0);

template class HybridXrayDOM;
template class HybridXrayJS;

namespace {

// STEALTHFOX [B322]: what the PAGE reads the caller's objects through, the
// mirror of the hybrid Xray. An object the caller's code makes and hands to
// the page (an argument of a page function, `window.x = {...}`, a callback,
// an event's `detail`) keeps its identity and its own properties, shared both
// ways; but its STANDARD prototypes are shown as the page's own intrinsics of
// the same name, and everything inherited is looked up there, in the page's
// realm. Through a plain transparent wrapper the page saw an object of
// another realm: `arg instanceof Array` false, `arg.map` the caller's map, and
// `cb.constructor('return globalThis')()` compiling code in the caller's
// world. Through this one `instanceof` holds, `arr.map` is the page's own and
// `cb.constructor` is the page's Function.
//
// The function kinds that compile code are mapped too: AsyncFunction,
// GeneratorFunction and AsyncGeneratorFunction prototypes are plain objects
// to JS::IdentifyStandardPrototype, and each leads to a constructor that
// would compile in the caller's realm. What stays reachable is only what the
// caller's code hands over itself (a sloppy function returning `this`).
class CallerObjectWrapper : public js::CrossCompartmentWrapper {
 public:
  constexpr CallerObjectWrapper()
      : js::CrossCompartmentWrapper(0, /* aHasPrototype = */ true) {}

  virtual bool getPrototype(JSContext* cx, JS::HandleObject wrapper,
                            JS::MutableHandleObject protop) const override;
  virtual bool getPrototypeIfOrdinary(
      JSContext* cx, JS::HandleObject wrapper, bool* isOrdinary,
      JS::MutableHandleObject protop) const override;

  static const CallerObjectWrapper singleton;
};

const CallerObjectWrapper CallerObjectWrapper::singleton;

// The intrinsic of the caller's realm `proto` is, or JSProto_Null. Called in
// the caller's realm.
bool CallerIntrinsicKey(JSContext* cx, JS::HandleObject proto,
                        JSProtoKey* key) {
  *key = JSProto_Null;
  if (js::IsCrossCompartmentWrapper(proto)) {
    // the page's own object, set as a prototype: it crosses back as itself
    return true;
  }
  *key = JS::IdentifyStandardPrototype(proto);
  if (*key != JSProto_Null) {
    return true;
  }
  for (JSProtoKey k : {JSProto_AsyncFunction, JSProto_GeneratorFunction,
                       JSProto_AsyncGeneratorFunction}) {
    JS::RootedObject intrinsic(cx);
    if (!JS_GetClassPrototype(cx, k, &intrinsic)) {
      return false;
    }
    if (intrinsic == proto) {
      *key = k;
      return true;
    }
  }
  return true;
}

bool CallerObjectWrapper::getPrototype(JSContext* cx, JS::HandleObject wrapper,
                                       JS::MutableHandleObject protop) const {
  JS::RootedObject target(cx, wrappedObject(wrapper));
  JSProtoKey key = JSProto_Null;
  {
    JSAutoRealm ar(cx, target);
    if (!JS_GetPrototype(cx, target, protop)) {
      return false;
    }
    if (protop && !CallerIntrinsicKey(cx, protop, &key)) {
      return false;
    }
  }
  if (key != JSProto_Null) {
    // the intrinsic of the realm asking, which is one of the page's
    return JS_GetClassPrototype(cx, key, protop);
  }
  return JS_WrapObject(cx, protop);
}

bool CallerObjectWrapper::getPrototypeIfOrdinary(
    JSContext* cx, JS::HandleObject wrapper, bool* isOrdinary,
    JS::MutableHandleObject protop) const {
  // Computed for the realm asking, so never the target's own [[Prototype]].
  *isOrdinary = false;
  return true;
}

}  // namespace

const js::Wrapper* StealthCallerObjectWrapper() {
  return &CallerObjectWrapper::singleton;
}

const js::Wrapper* StealthHybridWrapper(XrayType xrayType) {
  if (xrayType == XrayForDOMObject) {
    return &HybridXrayDOM::singleton;
  }
  if (xrayType == XrayForJSObject) {
    return &HybridXrayJS::singleton;
  }
  return &js::CrossCompartmentWrapper::singleton;
}

StealthXrayFlags StealthComputeXrayFlags(JS::Realm* origin, JS::Realm* target) {
  CompartmentPrivate* originPrivate = CompartmentPrivate::Get(origin);
  CompartmentPrivate* targetPrivate = CompartmentPrivate::Get(target);
  bool targetHybrid = targetPrivate->hybridXrays;
  return {targetHybrid, originPrivate->wantXrays,
          targetPrivate->wantXrays || targetHybrid};
}

bool StealthCallerObjectApplies(JS::Realm* origin, JS::Realm* target,
                                bool targetIsChrome,
                                bool targetSubsumesOrigin) {
  CompartmentPrivate* originPrivate = CompartmentPrivate::Get(origin);
  CompartmentPrivate* targetPrivate = CompartmentPrivate::Get(target);
  // The same five terms as the branch had inline; "the target forces Xrays"
  // is wantXrays || hybridXrays.
  return originPrivate->hybridXrays && !targetPrivate->hybridXrays &&
         !targetIsChrome && targetSubsumesOrigin && !targetPrivate->wantXrays;
}

}  // namespace xpc
